// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// benchmarks - completed-operation benchmarks for the cooling_topology library.
//
// Every timed region contains the *whole* mandatory work of the operation it
// names, never a submission, enqueue or teardown step:
//
//   build_generation    Topology::create over a synthetic facility: full
//                       validation, canonical ordering, encoding and digest
//   canonical_encode    canonical_bytes() of that validated generation
//   canonical_decode    Topology::decode of the encoded image: framing, digest
//                       check, decode, re-validation and fixed-point check
//   validate_draft      Topology::validate_draft over the same draft
//   supply_paths        possible_supply_paths between a structural source and a
//                       rack sink
//   independent_paths   independent_supply_paths between two elements
//   components          connected_components() of the whole structural graph
//   publish_durable     Store::publish of a new generation with durable_flush,
//                       i.e. INCLUDING validation, encoding, staging write,
//                       flush, read-back verification, atomic rename, manifest
//                       commit, floor advance and residue retirement
//
// The workload is a documented, deterministic function of the leg count: no
// randomness, no wall-clock seeding, no environment-dependent content, so two
// runs on the same host build the same facility and produce the same digest.
//
// Workload classification: SYNTHETIC means the timed work runs on a simulated
// facility topology held in memory; REAL means the timed work performs actual
// process and filesystem work on this host (the durable publication, including
// its durability cost). The label describes the workload, never a comparison.
//
// The optional argument is an iteration scale in percent: "benchmarks 25" runs a
// quarter of the default iterations, "benchmarks 200" runs twice as many. The
// default completes in a few seconds on this host.
//
// Timing uses std::chrono::steady_clock for measurement only. No timing value is
// ever used as a decision input: every decision in this file is exact integer
// arithmetic.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/mutation.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/store.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "dccp/cooling_topology/version.hpp"

namespace {

using dccp::cooling_topology::AttemptOrdinal;
using dccp::cooling_topology::ExternalGeneration;
using dccp::cooling_topology::ExternalRef;
using dccp::cooling_topology::ExternalRefKind;
using dccp::cooling_topology::MutationId;
using dccp::cooling_topology::NodeId;
using dccp::cooling_topology::PublicationRequest;
using dccp::cooling_topology::QueryOptions;
using dccp::cooling_topology::Store;
using dccp::cooling_topology::StoreId;
using dccp::cooling_topology::StoreMode;
using dccp::cooling_topology::StoreOptions;
using dccp::cooling_topology::Topology;
using dccp::cooling_topology::TopologyDraft;
using dccp::cooling_topology::TopologyGeneration;

/// Observations sink. Every measured operation deposits something here, so the
/// work cannot be deleted as dead code; every write is a volatile access and is
/// therefore an observable side effect, and no measured value is ever used as a
/// decision input.
volatile std::uint64_t g_sink = 0;

constexpr std::size_t kDefaultLegs = 40;

std::string number_text(std::uint64_t value) { return std::to_string(value); }
std::string bool_text(bool value) { return value ? std::string("true") : std::string("false"); }

std::string microseconds_text(long long nanoseconds) {
  const long long whole = nanoseconds / 1000;
  const long long thousandths = nanoseconds % 1000;
  std::string out = std::to_string(whole);
  out.push_back('.');
  if (thousandths < 100) {
    out.push_back('0');
  }
  if (thousandths < 10) {
    out.push_back('0');
  }
  out += std::to_string(thousandths);
  return out;
}

std::string join_path(const std::string& root, const std::string& name) {
  return (std::filesystem::path(root) / name).string();
}

std::size_t remove_tree(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::remove_all(path, ignored);
}

// ---------------------------------------------------------------------------
// Deterministic synthetic workload
// ---------------------------------------------------------------------------

struct Workload {
  std::size_t legs = 0;
  std::size_t nodes = 0;
  std::size_t edges = 0;
  std::size_t document_bytes = 0;
  std::string scale;
  std::string document;
  TopologyDraft draft;
  Topology generation;
  std::string canonical;
  /// The canonical image framed for durable storage, which is what
  /// Topology::decode() accepts; the store writes exactly this frame.
  std::string generation_file;
  NodeId source;
  NodeId target;
};

/// Builds a "ctg" document with exactly 4 + 3 * legs nodes.
///
/// The generator is a documented function of the leg index k:
///
///   src.0                cooling_source, the only structural source
///   loop.a, loop.b       cooling_loop, both fed from src.0
///   zone.0               thermal_zone containing every sink and air handler
///   per leg k            branch.k  fed by both loops, contained in loop.a
///                        sink.k    fed by branch.k, contained in zone.0
///                        crah.k    fed by branch.k, serves zone.0
///
/// nodes = 4 + 3 * legs and edges = 4 + 12 * legs, so the caller chooses the
/// exact size. Every identity and every connection is a pure function of k; the
/// document contains no clock, no random value and no environment-dependent
/// text.
std::string synthetic_document(std::size_t legs) {
  std::string out;
  out.reserve(4096 + legs * 512);
  out += "facility facility:benchmark@1\n\n";
  out += "provenance producer=cooling-topology-benchmarks/1.0.0 origin=authored witness=\"deterministic ";
  out += std::to_string(legs);
  out += "-leg workload\" authority-epoch=1\n\n";
  out += "node src.0 cooling_source kind=facility_water medium=chilled_water design-supply=6.000C\n";
  out += "node loop.a cooling_loop kind=primary medium=chilled_water design-supply=7.000C max-supply=12.000C\n";
  out += "node loop.b cooling_loop kind=primary medium=chilled_water design-supply=7.000C max-supply=12.000C\n";
  out += "node zone.0 thermal_zone class=room\n";
  out += "edge e.src.loop.a supplies src.0.supply_out -> loop.a.source_in\n";
  out += "edge e.src.loop.b supplies src.0.supply_out -> loop.b.source_in\n";
  out += "edge r.loop.a.src returns loop.a.heat_out -> src.0.return_in\n";
  out += "edge r.loop.b.src returns loop.b.heat_out -> src.0.return_in\n";
  for (std::size_t index = 0; index < legs; ++index) {
    const std::string k = std::to_string(index);
    out += "node branch.";
    out += k;
    out += " branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C\n";
    out += "node sink.";
    out += k;
    out += " cooling_sink kind=rack_load consumer=consumer:rack-";
    out += k;
    out += " max-supply=27.000C\n";
    out += "node crah.";
    out += k;
    out += " crah placement=in_row medium=chilled_water max-supply=18.000C\n";
    out += "edge e.loop.a.branch.";
    out += k;
    out += " supplies loop.a.supply_out -> branch.";
    out += k;
    out += ".source_in\n";
    out += "edge e.loop.b.branch.";
    out += k;
    out += " supplies loop.b.supply_out -> branch.";
    out += k;
    out += ".source_in\n";
    out += "edge e.branch.sink.";
    out += k;
    out += " supplies branch.";
    out += k;
    out += ".supply_out -> sink.";
    out += k;
    out += ".source_in\n";
    out += "edge e.branch.crah.";
    out += k;
    out += " supplies branch.";
    out += k;
    out += ".supply_out -> crah.";
    out += k;
    out += ".source_in\n";
    out += "edge r.sink.branch.";
    out += k;
    out += " returns sink.";
    out += k;
    out += ".heat_out -> branch.";
    out += k;
    out += ".return_in\n";
    out += "edge r.crah.branch.";
    out += k;
    out += " returns crah.";
    out += k;
    out += ".heat_out -> branch.";
    out += k;
    out += ".return_in\n";
    out += "edge r.branch.loop.a.";
    out += k;
    out += " returns branch.";
    out += k;
    out += ".heat_out -> loop.a.return_in\n";
    out += "edge r.branch.loop.b.";
    out += k;
    out += " returns branch.";
    out += k;
    out += ".heat_out -> loop.b.return_in\n";
    out += "edge c.loop.a.branch.";
    out += k;
    out += " contains loop.a.container -> branch.";
    out += k;
    out += ".contained\n";
    out += "edge c.zone.sink.";
    out += k;
    out += " contains zone.0.container -> sink.";
    out += k;
    out += ".contained\n";
    out += "edge c.zone.crah.";
    out += k;
    out += " contains zone.0.container -> crah.";
    out += k;
    out += ".contained\n";
    out += "edge s.crah.zone.";
    out += k;
    out += " serves crah.";
    out += k;
    out += ".server -> zone.0.served\n";
  }
  return out;
}

NodeId node_id(std::string_view spelling) {
  const auto parsed = NodeId::parse(spelling);
  return parsed.has_value() ? parsed.value() : NodeId{};
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct CaseResult {
  std::string name;
  std::string workload;
  const char* evidence = "SYNTHETIC";
  std::size_t iterations = 0;
  long long total_ns = 0;
  bool completed = true;
  std::string reason;
};

void print_case(const CaseResult& result) {
  std::string line = "CASE name=";
  line += result.name;
  line += " workload=\"";
  line += result.workload;
  line += "\" evidence=";
  line += result.evidence;
  line += " iterations=";
  line += number_text(result.iterations);
  line += " total_us=";
  line += microseconds_text(result.total_ns);
  line += " us_per_op=";
  line += result.iterations == 0 ? std::string("0.000")
                                 : microseconds_text(result.total_ns / static_cast<long long>(result.iterations));
  line += " ops_per_sec=";
  line += result.total_ns <= 0
              ? std::string("0")
              : number_text(static_cast<std::uint64_t>(result.iterations) * 1000000000ull /
                            static_cast<std::uint64_t>(result.total_ns));
  if (!result.completed) {
    line += " completed=false reason=";
    line += result.reason;
  }
  std::cout << line << '\n';
}

/// Runs one case: one untimed warm-up call, then the timed repetitions. The
/// timed region of one iteration is exactly the operation the case names, so the
/// reported microseconds per operation are microseconds of completed work.
template <class Step>
void run_case(std::vector<CaseResult>& results, std::string name, std::string workload, const char* evidence,
              std::size_t iterations, Step step) {
  CaseResult result;
  result.name = std::move(name);
  result.workload = std::move(workload);
  result.evidence = evidence;
  result.iterations = iterations;
  long long elapsed = 0;
  if (!step(elapsed)) {
    result.completed = false;
    result.iterations = 0;
    result.reason = "warm-up did not complete";
    results.push_back(std::move(result));
    return;
  }
  long long total = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    if (!step(elapsed)) {
      result.completed = false;
      result.iterations = index;
      result.total_ns = total;
      result.reason = "a timed repetition did not complete";
      results.push_back(std::move(result));
      return;
    }
    total += elapsed;
  }
  result.total_ns = total;
  results.push_back(std::move(result));
}

std::size_t scaled_iterations(std::size_t base, std::size_t scale_percent) {
  const std::size_t scaled = base * scale_percent / 100;
  return scaled == 0 ? 1 : scaled;
}

StoreOptions store_options_for(const std::string& path) {
  StoreOptions options;
  options.root = path;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 4;
  options.idempotency_retention = 4;
  options.durable_flush = true;
  options.enable_fault_injection = false;
  return options;
}

void print_context(const std::string& store_root, std::size_t scale_percent) {
  std::cout << "cooling_topology " << dccp::cooling_topology::version_string()
            << " - completed-operation benchmarks\n";
  std::cout << "boundary: " << dccp::cooling_topology::systems_boundary() << '\n';
  std::cout << "host:\n";
#if defined(_WIN32)
  std::cout << "  os=windows\n";
#elif defined(__linux__)
  std::cout << "  os=linux\n";
#elif defined(__APPLE__)
  std::cout << "  os=macos\n";
#else
  std::cout << "  os=unknown\n";
#endif
#if defined(_M_X64)
  std::cout << "  arch=x86_64\n";
#elif defined(_M_ARM64)
  std::cout << "  arch=arm64\n";
#elif defined(_M_IX86)
  std::cout << "  arch=x86\n";
#else
  std::cout << "  arch=unknown\n";
#endif
#if defined(_MSC_VER)
  std::cout << "  compiler=msvc " << _MSC_VER << " (" << _MSC_FULL_VER << ")\n";
#elif defined(__clang__)
  std::cout << "  compiler=clang " << __clang_major__ << "." << __clang_minor__ << "\n";
#elif defined(__GNUC__)
  std::cout << "  compiler=gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
#else
  std::cout << "  compiler=unknown\n";
#endif
#if defined(NDEBUG)
  std::cout << "  build_type=release (NDEBUG defined)\n";
#else
  std::cout << "  build_type=debug (NDEBUG not defined)\n";
#endif
  std::cout << "  pointer_bytes=" << sizeof(void*) << "\n";
  std::cout << "  iteration_scale_percent=" << scale_percent << "\n";
  std::cout << "  store_root=" << store_root << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dccp::cooling_topology;

  std::size_t scale_percent = 100;
  if (argc > 1) {
    const std::string argument = argv[1] == nullptr ? std::string() : std::string(argv[1]);
    bool digits = !argument.empty() && argument.size() <= 6;
    std::uint64_t value = 0;
    for (const char byte : argument) {
      if (byte < '0' || byte > '9') {
        digits = false;
        break;
      }
      value = value * 10 + static_cast<std::uint64_t>(byte - '0');
    }
    if (!digits || value == 0) {
      std::cout << "error: usage: benchmarks [iteration-scale-percent]\n";
      std::cout << "  iteration-scale-percent is a positive integer, default 100\n";
      return 1;
    }
    scale_percent = static_cast<std::size_t>(value);
  }
  if (scale_percent > 100000) {
    std::cout << "error: the iteration scale must not exceed 100000 percent\n";
    return 1;
  }

  std::error_code error;
  const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (error) {
    std::cout << "error: the system temporary directory is not available: " << error.message() << '\n';
    return 1;
  }
  const std::string store_root = (temp / "cooling_topology_benchmarks").string();
  remove_tree(store_root);
  std::filesystem::create_directories(store_root, error);
  if (error) {
    std::cout << "error: cannot create the benchmark store root: " << store_root << '\n';
    return 1;
  }
  if (!std::filesystem::is_directory(store_root, error)) {
    std::cout << "error: the benchmark store root is not a directory: " << store_root << '\n';
    return 1;
  }

  print_context(store_root, scale_percent);

  // ---- the workload ------------------------------------------------------
  Workload workload;
  workload.legs = kDefaultLegs;
  workload.nodes = 4 + 3 * workload.legs;
  workload.edges = 4 + 12 * workload.legs;
  workload.scale = number_text(workload.nodes) + "n/" + number_text(workload.edges) + "e";
  workload.document = synthetic_document(workload.legs);
  workload.document_bytes = workload.document.size();
  workload.source = node_id("src.0");
  workload.target = node_id("sink." + std::to_string(workload.legs - 1));

  const std::string facility_text = "facility:benchmark@1";

  {
    const auto parsed = parse_import(workload.document, nullptr);
    if (!parsed.has_value()) {
      std::cout << "error: the generated document failed to parse: " << parsed.error().to_string() << '\n';
      return 1;
    }
    workload.draft = parsed.value();
    const auto created = Topology::create_first(workload.draft);
    if (!created.has_value()) {
      std::cout << "error: the generated facility failed to validate: " << created.error().to_string() << '\n';
      return 1;
    }
    workload.generation = created.value();
    const auto encoded = workload.generation.canonical_bytes();
    if (!encoded.has_value()) {
      std::cout << "error: canonical encoding failed: " << encoded.error().to_string() << '\n';
      return 1;
    }
    workload.canonical = encoded.value();
    const auto framed = encode_generation_file(workload.canonical);
    if (!framed.has_value()) {
      std::cout << "error: framing the canonical image failed: " << framed.error().to_string() << '\n';
      return 1;
    }
    workload.generation_file = framed.value();
  }

  std::cout << "\nworkload legs=" << workload.legs << " nodes=" << workload.nodes
            << " edges=" << workload.edges
            << " document_bytes=" << workload.document_bytes
            << " canonical_bytes=" << workload.canonical.size()
            << " generation_file_bytes=" << workload.generation_file.size()
            << " digest=" << workload.generation.digest().to_hex().substr(0, 16) << '\n';
  std::cout << "workload shape: 1 cooling source, 2 primary loops, " << workload.legs
            << " rack branches, " << workload.legs << " air handlers, " << workload.legs
            << " rack sinks, 1 thermal zone\n";

  std::vector<CaseResult> results;

  // (1) build the generation: full validation, ordering, encoding and digest.
  {
    const Workload* handle = &workload;
    run_case(results, "build_generation",
             "synthetic facility " + workload.scale + ", " + number_text(workload.document_bytes) +
                 " document bytes",
             "SYNTHETIC", scaled_iterations(16, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const auto built = Topology::create_first(handle->draft);
               const auto stop = std::chrono::steady_clock::now();
               if (!built.has_value()) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += built.value().node_count();
               return true;
             });
  }

  // (2) canonical encoding of the validated generation.
  {
    const Workload* handle = &workload;
    run_case(results, "canonical_encode",
             "generation " + workload.scale + ", " + number_text(workload.canonical.size()) +
                 " canonical bytes",
             "SYNTHETIC", scaled_iterations(100, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const auto bytes = handle->generation.canonical_bytes();
               const auto stop = std::chrono::steady_clock::now();
               if (!bytes.has_value()) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += bytes.value().size();
               return true;
             });
  }

  // (3) decode of the durable generation file: framing, payload digest check,
  // canonical decode, re-validation and the encode/decode fixed-point check.
  {
    const Workload* handle = &workload;
    run_case(results, "canonical_decode",
             "generation " + workload.scale + ", " + number_text(workload.generation_file.size()) +
                 " generation-file bytes",
             "SYNTHETIC", scaled_iterations(16, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const auto decoded = Topology::decode(handle->generation_file);
               const auto stop = std::chrono::steady_clock::now();
               if (!decoded.has_value() || decoded.value().digest() != handle->generation.digest()) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += decoded.value().edge_count();
               return true;
             });
  }

  // (4) validation of the same draft, collecting every issue.
  {
    const Workload* handle = &workload;
    run_case(results, "validate_draft", "synthetic facility " + workload.scale + ", same draft",
             "SYNTHETIC", scaled_iterations(32, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const ValidationReport report = Topology::validate_draft(handle->draft);
               const auto stop = std::chrono::steady_clock::now();
               if (!report.valid()) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += report.issues.size();
               return true;
             });
  }

  // (5) supply paths between the structural source and the last rack sink.
  {
    const Workload* handle = &workload;
    run_case(results, "supply_paths",
             "source " + handle->source.str() + " -> sink " + handle->target.str() + ", " +
                 number_text(handle->nodes) + " nodes",
             "SYNTHETIC", scaled_iterations(200, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const auto paths = possible_supply_paths(handle->generation, handle->source, handle->target,
                                                        QueryOptions{});
               const auto stop = std::chrono::steady_clock::now();
               if (!paths.has_value() || paths.value().paths.empty() || paths.value().truncated) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += paths.value().paths.size();
               return true;
             });
  }

  // (6) edge-disjoint supply paths between the same two elements.
  {
    const Workload* handle = &workload;
    run_case(results, "independent_paths",
             "source " + handle->source.str() + " -> sink " + handle->target.str() + ", " +
                 number_text(handle->nodes) + " nodes",
             "SYNTHETIC", scaled_iterations(100, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const auto independent = independent_supply_paths(handle->generation, handle->source,
                                                                 handle->target, QueryOptions{});
               const auto stop = std::chrono::steady_clock::now();
               if (!independent.has_value() || independent.value().path_count == 0 ||
                   independent.value().truncated) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += independent.value().path_count;
               return true;
             });
  }

  // (7) connected components over every edge kind, including containment.
  {
    const Workload* handle = &workload;
    run_case(results, "components", "whole structural graph " + workload.scale, "SYNTHETIC",
             scaled_iterations(200, scale_percent),
             [handle](long long& elapsed) {
               const auto start = std::chrono::steady_clock::now();
               const auto components = connected_components(handle->generation, QueryOptions{});
               const auto stop = std::chrono::steady_clock::now();
               if (!components.has_value() || components.value().components.size() != 1 ||
                   !components.value().isolated.empty()) {
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += components.value().components.front().size();
               return true;
             });
  }

  // (8) a durable publication, including the durability cost.
  {
    const auto store_id = StoreId::parse("store.benchmark");
    const auto facility = ExternalRef::create(ExternalRefKind::Facility, "benchmark", ExternalGeneration(1));
    const auto mutation = MutationId::parse("m.benchmark.publish");
    const auto attempt = AttemptOrdinal::parse(1);
    if (!store_id.has_value() || !facility.has_value() || !mutation.has_value() || !attempt.has_value()) {
      std::cout << "error: the embedded store identity, facility, mutation or attempt is not valid\n";
      return 1;
    }
    const StoreId store_id_value = store_id.value();
    const ExternalRef facility_value = facility.value();
    const MutationId mutation_value = mutation.value();
    const AttemptOrdinal attempt_value = attempt.value();
    const TopologyDraft* draft_handle = &workload.draft;
    const std::string root_prefix = join_path(store_root, "publish");
    std::size_t removed_count = 0;
    std::size_t failed_count = 0;
    std::size_t counter = 0;
    run_case(results, "publish_durable",
             "1 generation into a fresh durable store, " + workload.scale + ", durable_flush=true",
             "REAL", scaled_iterations(4, scale_percent),
             [&, root_prefix, draft_handle, store_id_value, facility_value, mutation_value,
              attempt_value](long long& elapsed) {
               const std::string store_path = root_prefix + "-" + std::to_string(counter++);
               remove_tree(store_path);
               auto created = Store::create(store_options_for(store_path), store_id_value, facility_value);
               if (!created.has_value()) {
                 ++failed_count;
                 remove_tree(store_path);
                 return false;
               }
               Store store = std::move(created.value());
               auto info = store.info();
               if (!info.has_value()) {
                 ++failed_count;
                 store.close();
                 remove_tree(store_path);
                 return false;
               }
               PublicationRequest request;
               request.authority.epoch = info.value().epoch;
               request.authority.incarnation = info.value().incarnation;
               request.authority.expected_base = TopologyGeneration{};
               request.mutation = mutation_value;
               request.attempt = attempt_value;
               request.draft = *draft_handle;
               const auto start = std::chrono::steady_clock::now();
               const auto receipt = store.publish(request);
               const auto stop = std::chrono::steady_clock::now();
               if (!receipt.has_value()) {
                 ++failed_count;
                 store.close();
                 remove_tree(store_path);
                 return false;
               }
               elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
               g_sink += receipt.value().digest.bytes()[0];
               const bool durable = receipt.value().durability == PublicationDurability::Durable;
               store.close();
               if (remove_tree(store_path) > 0) {
                 ++removed_count;
               }
               if (std::filesystem::exists(store_path)) {
                 ++failed_count;
                 return false;
               }
               return durable;
             });
    std::cout << "publish_durable cleanup: stores_removed=" << removed_count
              << " removal_failures=" << failed_count
              << " facility=" << facility_text << " store_id=" << store_id_value.str() << '\n';
    if (failed_count != 0) {
      results.back().completed = false;
      results.back().reason = "a store could not be published or removed";
    }
  }

  // ---- results -----------------------------------------------------------
  std::cout << "\nresults (completed operations; each timed region includes every mandatory step)\n";
  bool all_completed = true;
  for (const CaseResult& result : results) {
    print_case(result);
    all_completed = all_completed && result.completed && result.iterations != 0;
  }

  const std::size_t leftover = remove_tree(store_root);
  const bool root_gone = !std::filesystem::exists(store_root, error);
  std::cout << "\ncleanup: store_root=" << store_root << " entries_removed=" << leftover
            << " root_present_after_cleanup=" << bool_text(!root_gone) << '\n';
  std::cout << "posture: " << dccp::cooling_topology::posture_statement() << '\n';

  if (!all_completed) {
    std::cout << "FAILED: at least one operation could not be completed on this host\n";
    return 1;
  }
  if (!root_gone) {
    std::cout << "FAILED: benchmark residue was left behind\n";
    return 1;
  }
  std::cout << "OK: every measured operation completed on this host\n";
  return 0;
}
