// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// durable_store - the durable lifecycle of a cooling topology store.
//
// The program opens a store in its own temporary directory, publishes two
// generations of the same facility, reads the committed history back, walks the
// parent chain from the digests the store returns, verifies the store deeply,
// and then asks for recovery while the store is healthy.
//
// The last step is the interesting one: recovery on a healthy store must do
// nothing. The store reports the no-action outcome and changes no head, no
// digest and no residue: recovery was not needed, so no retained publication is
// adopted and no error is raised.
//
// The temporary directory the program created is removed before it exits, and
// the removal is reported.
//
// Nothing here is a capability claim: a retained generation is evidence about
// what was published, never about what is running, flowing or authorized.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/mutation.hpp"
#include "dccp/cooling_topology/store.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "dccp/cooling_topology/version.hpp"

namespace {

using dccp::cooling_topology::AttemptOrdinal;
using dccp::cooling_topology::Digest;
using dccp::cooling_topology::Error;
using dccp::cooling_topology::ErrorCode;
using dccp::cooling_topology::ExternalGeneration;
using dccp::cooling_topology::ExternalRef;
using dccp::cooling_topology::ExternalRefKind;
using dccp::cooling_topology::HistoryEntry;
using dccp::cooling_topology::MutationAuthority;
using dccp::cooling_topology::MutationId;
using dccp::cooling_topology::PublicationReceipt;
using dccp::cooling_topology::RecoveryOutcome;
using dccp::cooling_topology::RecoveryOptions;
using dccp::cooling_topology::RecoveryReport;
using dccp::cooling_topology::Store;
using dccp::cooling_topology::StoreId;
using dccp::cooling_topology::StoreInfo;
using dccp::cooling_topology::StoreMode;
using dccp::cooling_topology::StoreOpenState;
using dccp::cooling_topology::StoreOptions;
using dccp::cooling_topology::Topology;
using dccp::cooling_topology::TopologyDraft;
using dccp::cooling_topology::TopologyGeneration;
using dccp::cooling_topology::VerifyFinding;
using dccp::cooling_topology::VerifyOptions;
using dccp::cooling_topology::VerifyReport;

int g_failures = 0;

void section(std::string_view title) { std::cout << "\n== " << title << " ==\n"; }

bool check(bool condition, std::string_view what) {
  std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
  if (!condition) {
    ++g_failures;
  }
  return condition;
}

void report_error(std::string_view what, const Error& error) {
  std::cout << "  [FAIL] " << what << ": " << dccp::cooling_topology::error_code_name(error.code()) << ": "
            << error.message();
  if (!error.subject().empty()) {
    std::cout << " [subject=" << error.subject() << "]";
  }
  std::cout << '\n';
  ++g_failures;
}

std::string digest_text(const Digest& digest) {
  return digest.is_zero() ? std::string("none") : digest.to_hex().substr(0, 16);
}

/// Revision 1: one plant, one primary loop, one duty pump, one branch and one
/// rack load.
constexpr std::string_view kRevisionOne = R"CTG(
node src.water cooling_source kind=facility_water medium=chilled_water design-supply=6.000C name="Facility water intake"
node plant.1 cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000C max-supply=12.000C name="Chiller plant 1"
node loop.1 cooling_loop kind=primary medium=chilled_water design-supply=7.000C max-supply=12.000C name="Primary loop 1"
node pump.1 pump kind=centrifugal role=duty name="Primary pump 1"
node branch.1 branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Rack branch 1"
node zone.1 thermal_zone class=room name="Hall 1"
node sink.1 cooling_sink kind=rack_load consumer=consumer:rack-1 max-supply=27.000C name="Rack 1 load"

edge e.src.plant   supplies src.water.supply_out -> plant.1.source_in
edge e.plant.loop  supplies plant.1.supply_out   -> loop.1.source_in
edge e.loop.branch supplies loop.1.supply_out    -> branch.1.source_in
edge e.branch.sink supplies branch.1.supply_out  -> sink.1.source_in

edge r.sink.branch returns sink.1.heat_out   -> branch.1.return_in
edge r.branch.loop returns branch.1.heat_out -> loop.1.return_in
edge r.loop.plant  returns loop.1.heat_out   -> plant.1.return_in
edge r.plant.src   returns plant.1.heat_out  -> src.water.return_in

edge p.loop.pump pumps loop.1.supply_out -> pump.1.terminal

edge c.plant.loop  contains plant.1.container -> loop.1.contained
edge c.loop.branch contains loop.1.container  -> branch.1.contained
edge c.loop.pump   contains loop.1.container  -> pump.1.contained
edge c.zone.sink   contains zone.1.container  -> sink.1.contained
)CTG";

/// Revision 2 adds a second branch, a standby pump and a second rack load.
constexpr std::string_view kRevisionTwo = R"CTG(
node pump.2 pump kind=centrifugal role=standby name="Primary pump 2"
node branch.2 branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Rack branch 2"
node sink.2 cooling_sink kind=rack_load consumer=consumer:rack-2 max-supply=27.000C name="Rack 2 load"

edge e.loop.branch.2 supplies loop.1.supply_out   -> branch.2.source_in
edge e.branch.2.sink supplies branch.2.supply_out -> sink.2.source_in

edge r.sink.2.branch returns sink.2.heat_out   -> branch.2.return_in
edge r.branch.2.loop returns branch.2.heat_out -> loop.1.return_in

edge p.loop.pump.2 pumps loop.1.return_in -> pump.2.terminal

edge c.loop.branch.2 contains loop.1.container -> branch.2.contained
edge c.loop.pump.2   contains loop.1.container -> pump.2.contained
edge c.zone.sink.2   contains zone.1.container -> sink.2.contained

alias loop.primary.1 loop.1

group rg.pumps scope=pump scheme=n_plus_one name="Primary pumps" pump.1 pump.2
)CTG";

std::string document(std::uint32_t revision) {
  std::string out = "facility facility:durable-demo@2\n\n";
  out += "provenance producer=cooling-topology-examples/1.0.0 origin=authored witness=\"durable-store example revision ";
  out += std::to_string(revision);
  out += "\" source=facility:durable-demo@2 authority-epoch=31\n";
  out += kRevisionOne;
  if (revision >= 2) {
    out += kRevisionTwo;
  }
  return out;
}

MutationId mutation_id(std::string_view spelling) {
  const auto parsed = MutationId::parse(spelling);
  return parsed.has_value() ? parsed.value() : MutationId{};
}

int run(const std::string& root) {
  using namespace dccp::cooling_topology;

  std::cout << "cooling_topology " << version_string() << " - durable_store example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';
  std::cout << "temporary store root: " << root << '\n';

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  StoreOptions options;
  options.root = root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 3;
  options.idempotency_retention = 8;
  options.durable_flush = true;

  const auto store_id = StoreId::parse("store.durable-demo");
  const auto facility = ExternalRef::create(ExternalRefKind::Facility, "durable-demo", ExternalGeneration(2));
  if (!store_id.has_value() || !facility.has_value()) {
    report_error("static inputs", !store_id.has_value() ? store_id.error() : facility.error());
    return 1;
  }

  section("1. create a store with a three-generation retention window");
  auto created = Store::create(options, store_id.value(), facility.value());
  if (!created.has_value()) {
    report_error("Store::create", created.error());
    return 1;
  }
  Store store = std::move(created.value());
  auto fresh = store.info();
  if (!fresh.has_value()) {
    report_error("Store::info", fresh.error());
    return 1;
  }
  const StoreInfo& initial = fresh.value();
  std::cout << "  store_id=" << initial.store_id.str() << " root=" << initial.root
            << " facility=" << to_token(initial.facility.kind) << ":" << initial.facility.identity
            << " open_state=" << to_token(initial.open_state)
            << " mode=" << to_token(initial.mode) << '\n';
  std::cout << "  epoch=" << initial.epoch.value() << " incarnation=" << initial.incarnation.value()
            << " head=" << initial.head.value() << " floor=" << initial.floor.value()
            << " retention=" << options.retained_generations << '\n';
  check(initial.open_state == StoreOpenState::Fresh, "a new store starts at open_state=fresh");
  check(!initial.head.published(), "a new store has no published head");
  check(initial.retained_generations == 0, "a new store retains no generation");

  section("2. publish two generations");
  std::vector<TopologyGeneration> published;
  for (std::uint32_t revision = 1; revision <= 2; ++revision) {
    auto current = store.info();
    if (!current.has_value()) {
      report_error("Store::info", current.error());
      return 1;
    }
    const auto parsed = parse_import(document(revision), nullptr);
    if (!parsed.has_value()) {
      report_error("parse_import", parsed.error());
      return 1;
    }
    MutationAuthority authority;
    authority.epoch = current.value().epoch;
    authority.incarnation = current.value().incarnation;
    authority.expected_base = current.value().head;
    const auto ordinal = AttemptOrdinal::parse(1);
    if (!ordinal.has_value()) {
      report_error("AttemptOrdinal::parse", ordinal.error());
      return 1;
    }
    PublicationRequest request;
    request.authority = authority;
    request.mutation = mutation_id("m.gen." + std::to_string(revision));
    request.attempt = ordinal.value();
    request.draft = parsed.value();
    const auto receipt = store.publish(request);
    if (!receipt.has_value()) {
      report_error("publish revision " + std::to_string(revision), receipt.error());
      return 1;
    }
    const PublicationReceipt& outcome = receipt.value();
    published.push_back(outcome.generation);
    std::cout << "  published generation=" << outcome.generation.value()
              << " parent=" << outcome.parent_generation.value()
              << " digest=" << digest_text(outcome.digest)
              << " durability=" << to_token(outcome.durability)
              << " replayed=" << (outcome.replayed ? "yes" : "no") << '\n';
  }
  check(published.size() == 2, "two generations were published");
  check(published.size() == 2 && published.back().value() == 2,
        "the newest published generation is 2");
  auto after_publish = store.info();
  if (!after_publish.has_value()) {
    report_error("Store::info", after_publish.error());
    return 1;
  }
  std::cout << "  head=" << after_publish.value().head.value()
            << " floor=" << after_publish.value().floor.value()
            << " retained_generations=" << after_publish.value().retained_generations
            << " idempotency_records=" << after_publish.value().idempotency_records << '\n';
  check(after_publish.value().retained_generations == 2, "both generations are retained");
  check(after_publish.value().writable && after_publish.value().publication_allowed,
        "the handle may still publish, which is what a healthy head means here");

  section("3. the committed history and the parent chain");
  const auto history = store.history();
  if (!history.has_value()) {
    report_error("Store::history", history.error());
    return 1;
  }
  const std::vector<HistoryEntry>& entries = history.value();
  std::cout << "  entries=" << entries.size() << " (newest first)\n";
  bool chain_intact = true;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const HistoryEntry& entry = entries[index];
    // The newer neighbour (index - 1) must name this generation as its parent.
    const bool link_ok = index == 0
                             ? entry.is_head
                             : (entries[index - 1].parent_generation == entry.generation &&
                                entries[index - 1].parent_digest == entry.digest);
    chain_intact = chain_intact && link_ok;
    std::cout << "    generation=" << entry.generation.value()
              << " parent=" << entry.parent_generation.value()
              << " digest=" << digest_text(entry.digest)
              << " parent_digest=" << digest_text(entry.parent_digest)
              << " commit_sequence=" << entry.commit_sequence.value()
              << " file_bytes=" << entry.file_bytes
              << " head=" << (entry.is_head ? "yes" : "no")
              << " library_chain_verified=" << (entry.chain_verified ? "yes" : "no")
              << " parent_digest_link=" << (link_ok ? "yes" : "no") << '\n';
  }
  check(entries.size() == 2, "history lists the two retained generations");
  check(chain_intact, "every retained generation links to its newer neighbour through parent_digest");
  const auto first_generation = store.load(TopologyGeneration(1));
  const auto second_generation = store.load(TopologyGeneration(2));
  if (!first_generation.has_value() || !second_generation.has_value()) {
    report_error("Store::load", !first_generation.has_value() ? first_generation.error()
                                                             : second_generation.error());
    return 1;
  }
  const Topology& one = first_generation.value();
  const Topology& two = second_generation.value();
  std::cout << "  generation 1: digest=" << digest_text(one.digest())
            << " nodes=" << one.node_count() << " edges=" << one.edge_count() << '\n';
  std::cout << "  generation 2: digest=" << digest_text(two.digest())
            << " parent=" << two.parent_generation().value()
            << " parent_digest=" << digest_text(two.parent_digest())
            << " nodes=" << two.node_count() << " edges=" << two.edge_count() << '\n';
  check(two.parent_generation().value() == 1, "generation 2 names generation 1 as its parent");
  check(two.parent_digest() == one.digest(),
        "generation 2 binds the digest of the generation it succeeds");
  check(two.node_count() > one.node_count(), "generation 2 adds elements over generation 1");

  section("4. deep verification of the store");
  VerifyOptions verify_options;
  verify_options.deep = true;
  verify_options.verify_idempotency = true;
  verify_options.verify_canonical_fixed_point = true;
  const auto verified = store.verify(verify_options);
  if (!verified.has_value()) {
    report_error("Store::verify", verified.error());
    return 1;
  }
  const VerifyReport& verify = verified.value();
  std::cout << "  ok=" << (verify.ok() ? "yes" : "no") << " head=" << verify.head.value()
            << " head_digest=" << digest_text(verify.head_digest) << '\n';
  std::cout << "  verified head=" << (verify.head_verified ? "yes" : "no")
            << " manifest=" << (verify.manifest_verified ? "yes" : "no")
            << " floor=" << (verify.floor_verified ? "yes" : "no")
            << " chain=" << (verify.chain_verified ? "yes" : "no")
            << " canonical_fixed_point=" << (verify.canonical_fixed_point_verified ? "yes" : "no")
            << " recovered_state=" << (verify.recovered_state ? "yes" : "no") << '\n';
  std::cout << "  generations_present=" << verify.generations_present
            << " generations_verified=" << verify.generations_verified
            << " staged_residue_found=" << verify.staged_residue_found
            << " orphan_generations_found=" << verify.orphan_generations_found
            << " unreferenced_generations_found=" << verify.unreferenced_generations_found
            << " findings=" << verify.findings.size() << '\n';
  for (const VerifyFinding& finding : verify.findings) {
    std::cout << "    finding " << to_token(finding.severity) << " " << finding.code << " ["
              << finding.subject << "] " << finding.detail << '\n';
  }
  check(verify.head_verified, "the head payload verifies against the committed manifest");
  check(verify.manifest_verified, "the committed manifest matches the open handle");
  check(verify.floor_verified, "the durable generation floor is consistent with the head");
  check(verify.chain_verified, "the committed parent chain verifies from the head downwards");
  check(verify.canonical_fixed_point_verified, "decode then re-encode reproduces every retained digest");
  check(verify.staged_residue_found == 0 && verify.orphan_generations_found == 0,
        "no staging residue and no uncommitted generation was left behind");

  section("5. recovery on a healthy store: nothing to do");
  auto before_recovery = store.info();
  if (!before_recovery.has_value()) {
    report_error("Store::info", before_recovery.error());
    return 1;
  }
  const RecoveryOptions recovery_options;
  const auto recovered = store.recover(recovery_options);
  if (!recovered.has_value()) {
    report_error("Store::recover", recovered.error());
  } else {
    const RecoveryReport& recovery = recovered.value();
    std::cout << "  recover outcome=" << to_token(recovery.outcome)
              << " head_before=" << recovery.head_before.value()
              << " head_after=" << recovery.head_after.value()
              << " head_digest_after=" << digest_text(recovery.head_digest_after)
              << " residue_removed=" << recovery.residue_removed
              << " floor_respected=" << (recovery.floor_respected ? "yes" : "no") << '\n';
    std::cout << "  explanation " << recovery.explanation << '\n';
    for (std::size_t index = 0; index < recovery.steps.size(); ++index) {
      std::cout << "  step " << index << ": " << recovery.steps[index] << '\n';
    }
    std::cout << "  recovery was not needed: the healthy-store verdict is reported as outcome="
              << to_token(RecoveryOutcome::NoAction)
              << " rather than as an error, because nothing was refused and nothing was adopted\n";
    check(recovery.outcome == RecoveryOutcome::NoAction,
          "recovery on a healthy store reports the no-action outcome");
    check(recovery.head_before == recovery.head_after &&
              recovery.head_before == before_recovery.value().head,
          "recovery changed no committed head");
    check(recovery.head_digest_after == before_recovery.value().head_digest,
          "recovery changed no head digest");
    check(recovery.residue_removed == 0, "recovery found no residue to retire");
    check(recovery.steps.empty(), "recovery took no steps, because no state was adopted");
  }
  auto after_recovery = store.info();
  if (!after_recovery.has_value()) {
    report_error("Store::info", after_recovery.error());
    return 1;
  }
  check(after_recovery.value().head == before_recovery.value().head &&
            after_recovery.value().commit_sequence == before_recovery.value().commit_sequence,
        "no commit happened, so no counter moved");
  check(after_recovery.value().open_state == before_recovery.value().open_state,
        "recovery did not change the state the handle was opened in");
  check(after_recovery.value().open_state != StoreOpenState::Recovered,
        "the handle does not report recovered state, because nothing was adopted");

  section("6. what this run established, and what it did not");
  std::cout << "  established: the store committed two generations with durable flush, retained both, and\n"
            << "    the second names the first as its parent through the parent digest.\n";
  std::cout << "  established: deep verification confirms the head, the manifest, the durable floor, the\n"
            << "    committed chain and the canonical fixed point of every retained generation.\n";
  std::cout << "  established: recovery on the healthy store is a no-op: recovery was not needed, the\n"
            << "    outcome is " << to_token(RecoveryOutcome::NoAction)
            << ", and no head, digest, residue or counter changed.\n";
  std::cout << "  NOT established by any answer above:\n";
  std::cout << "    - that the stored structure is operating, or that any element is energized;\n";
  std::cout << "    - that a verified store is a safe or available plant: durability and integrity are not\n"
            << "      availability, capacity or thermal safety;\n";
  std::cout << "    - that recovered state would be fresh state: a recovery adopts a retained publication and\n"
            << "      its result must be revalidated by the components that own the operational claims.\n";
  std::cout << "  " << posture_statement() << '\n';

  section("7. clean up the temporary directory this program created");
  const auto closed = store.close();
  if (!closed.has_value()) {
    report_error("Store::close", closed.error());
  }
  const std::uintmax_t removed = std::filesystem::remove_all(root, ignored);
  const bool present = std::filesystem::exists(root, ignored);
  std::cout << "  temporary root " << root << " entries_removed=" << removed
            << " present_after_cleanup=" << (present ? "yes" : "no") << '\n';
  check(!present, "the temporary directory is gone");

  section("result");
  if (g_failures != 0) {
    std::cout << "  FAILED: " << g_failures << " expectation(s) not met\n";
    return 1;
  }
  std::cout << "  OK: the durable lifecycle behaved as documented\n";
  return 0;
}

}  // namespace

int main() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    std::cout << "  [FAIL] the system temporary directory is not available: " << error.message() << '\n';
    return 1;
  }
  return run((base / "cooling_topology_durable_store_example").string());
}
