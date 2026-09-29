// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// stale_update - the failure path of a state-dependent publication.
//
// The program opens a durable store in its own temporary directory, publishes
// two generations, and then shows three things:
//
//   (a) a mutation planned against a base generation that the store has moved
//       past is fenced with STALE_BASE_GENERATION and changes nothing;
//   (b) an exact retry of an already accepted attempt is served from the
//       recorded receipt (replayed=true) even though the base it was planned
//       against is long gone, so a lost response is not mistaken for a second
//       mutation and nothing is re-actuated;
//   (c) reusing one mutation identity for different content is an
//       IDEMPOTENCY_CONFLICT rather than a second publication.
//
// Every refusal leaves the store verifiable and still at its committed head.
// The temporary directory the program created is removed before it exits, and
// the removal is reported.
//
// Nothing here is a capability claim: a published generation is a record of
// declared structure, never a statement that anything is running or authorized.

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
using dccp::cooling_topology::Error;
using dccp::cooling_topology::ErrorCode;
using dccp::cooling_topology::ExternalGeneration;
using dccp::cooling_topology::ExternalRef;
using dccp::cooling_topology::ExternalRefKind;
using dccp::cooling_topology::MutationAuthority;
using dccp::cooling_topology::MutationId;
using dccp::cooling_topology::PublicationReceipt;
using dccp::cooling_topology::PublicationRequest;
using dccp::cooling_topology::Store;
using dccp::cooling_topology::StoreId;
using dccp::cooling_topology::StoreInfo;
using dccp::cooling_topology::StoreMode;
using dccp::cooling_topology::StoreOpenState;
using dccp::cooling_topology::StoreOptions;
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

void report_rejection(std::string_view label, const Error& error, ErrorCode expected) {
  std::cout << "  " << label << " -> " << dccp::cooling_topology::error_code_name(error.code()) << ": "
            << error.message() << '\n';
  check(error.code() == expected,
        std::string(label) + " is refused with " +
            std::string(dccp::cooling_topology::error_code_name(expected)));
}

/// Generation 1: one plant, one primary loop, one duty pump, one branch and one
/// rack load.
constexpr std::string_view kGenerationOne = R"CTG(
node src.plant.water cooling_source kind=facility_water medium=chilled_water design-supply=6.000C name="Facility water intake"
node plant.1 cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000C max-supply=12.000C name="Chiller plant 1"
node loop.1 cooling_loop kind=primary medium=chilled_water design-supply=7.000C max-supply=12.000C name="Primary loop 1"
node pump.1 pump kind=centrifugal role=duty name="Primary pump 1"
node branch.1 branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Rack branch 1"
node zone.1 thermal_zone class=room name="Hall 1"
node sink.1 cooling_sink kind=rack_load consumer=consumer:rack-1 max-supply=27.000C name="Rack 1 load"

edge e.src.plant   supplies src.plant.water.supply_out -> plant.1.source_in
edge e.plant.loop  supplies plant.1.supply_out         -> loop.1.source_in
edge e.loop.branch supplies loop.1.supply_out          -> branch.1.source_in
edge e.branch.sink supplies branch.1.supply_out        -> sink.1.source_in

edge r.sink.branch returns sink.1.heat_out   -> branch.1.return_in
edge r.branch.loop returns branch.1.heat_out -> loop.1.return_in
edge r.loop.plant  returns loop.1.heat_out   -> plant.1.return_in
edge r.plant.src   returns plant.1.heat_out  -> src.plant.water.return_in

edge p.loop.pump pumps loop.1.supply_out -> pump.1.terminal

edge c.plant.loop  contains plant.1.container -> loop.1.contained
edge c.loop.branch contains loop.1.container  -> branch.1.contained
edge c.loop.pump   contains loop.1.container  -> pump.1.contained
edge c.zone.sink   contains zone.1.container  -> sink.1.contained
)CTG";

/// Generation 2 adds a second branch, a second pump and a second sink.
constexpr std::string_view kGenerationTwo = R"CTG(
node pump.2 pump kind=centrifugal role=standby name="Primary pump 2"
node branch.2 branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Rack branch 2"
node sink.2 cooling_sink kind=rack_load consumer=consumer:rack-2 max-supply=27.000C name="Rack 2 load"

edge e.loop.branch.2 supplies loop.1.supply_out  -> branch.2.source_in
edge e.branch.2.sink supplies branch.2.supply_out -> sink.2.source_in

edge r.sink.2.branch returns sink.2.heat_out   -> branch.2.return_in
edge r.branch.2.loop returns branch.2.heat_out -> loop.1.return_in

edge p.loop.pump.2 pumps loop.1.return_in -> pump.2.terminal

edge c.loop.branch.2 contains loop.1.container -> branch.2.contained
edge c.loop.pump.2   contains loop.1.container -> pump.2.contained
edge c.zone.sink.2   contains zone.1.container -> sink.2.contained
)CTG";

std::string document(std::uint32_t revision) {
  std::string out = "facility facility:stale-demo@7\n\n";
  out += "provenance producer=cooling-topology-examples/1.0.0 origin=authored witness=\"stale-update example revision ";
  out += std::to_string(revision);
  out += "\" source=facility:stale-demo@7 authority-epoch=29\n";
  out += kGenerationOne;
  if (revision >= 2) {
    out += kGenerationTwo;
  }
  return out;
}

MutationId mutation_id(std::string_view spelling) {
  const auto parsed = MutationId::parse(spelling);
  return parsed.has_value() ? parsed.value() : MutationId{};
}

PublicationRequest request_of(const TopologyDraft& draft, const MutationAuthority& authority,
                              std::string_view mutation, std::uint32_t attempt) {
  PublicationRequest request;
  request.authority = authority;
  request.mutation = mutation_id(mutation);
  const auto ordinal = AttemptOrdinal::parse(attempt);
  if (ordinal.has_value()) {
    request.attempt = ordinal.value();
  }
  request.draft = draft;
  return request;
}

int run(const std::string& root) {
  using namespace dccp::cooling_topology;

  std::cout << "cooling_topology " << version_string() << " - stale_update example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';
  std::cout << "temporary store root: " << root << '\n';

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  StoreOptions options;
  options.root = root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 4;
  options.idempotency_retention = 8;
  options.durable_flush = true;

  const auto store_id = StoreId::parse("store.stale-demo");
  const auto facility = ExternalRef::create(ExternalRefKind::Facility, "stale-demo", ExternalGeneration(7));
  if (!store_id.has_value() || !facility.has_value()) {
    report_error("static inputs", !store_id.has_value() ? store_id.error() : facility.error());
    return 1;
  }

  const auto parsed_one = parse_import(document(1), nullptr);
  const auto parsed_two = parse_import(document(2), nullptr);
  if (!parsed_one.has_value() || !parsed_two.has_value()) {
    report_error("parse_import", !parsed_one.has_value() ? parsed_one.error() : parsed_two.error());
    return 1;
  }
  const TopologyDraft& draft_one = parsed_one.value();
  const TopologyDraft& draft_two = parsed_two.value();

  section("1. create the store and publish two generations");
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
  std::cout << "  fresh: open_state=" << to_token(fresh.value().open_state)
            << " epoch=" << fresh.value().epoch.value()
            << " incarnation=" << fresh.value().incarnation.value()
            << " head=" << fresh.value().head.value() << '\n';
  check(fresh.value().open_state == StoreOpenState::Fresh, "a new store reports open_state=fresh");
  check(!fresh.value().head.published(), "a new store has published no generation");

  MutationAuthority first_authority;
  first_authority.epoch = fresh.value().epoch;
  first_authority.incarnation = fresh.value().incarnation;
  first_authority.expected_base = TopologyGeneration{};

  const PublicationRequest first_request = request_of(draft_one, first_authority, "m.gen1", 1);
  const auto first = store.publish(first_request);
  if (!first.has_value()) {
    report_error("publish generation 1", first.error());
    return 1;
  }
  std::cout << "  published generation=" << first.value().generation.value()
            << " digest=" << first.value().digest.to_hex().substr(0, 16)
            << " durability=" << to_token(first.value().durability)
            << " replayed=" << (first.value().replayed ? "yes" : "no") << '\n';
  check(first.value().generation.value() == 1, "generation 1 is published");
  check(!first.value().replayed, "a first publication is not a replay");

  MutationAuthority second_authority;
  second_authority.epoch = fresh.value().epoch;
  second_authority.incarnation = fresh.value().incarnation;
  second_authority.expected_base = TopologyGeneration(1);
  const auto second = store.publish(request_of(draft_two, second_authority, "m.gen2", 1));
  if (!second.has_value()) {
    report_error("publish generation 2", second.error());
    return 1;
  }
  std::cout << "  published generation=" << second.value().generation.value()
            << " digest=" << second.value().digest.to_hex().substr(0, 16)
            << " replayed=" << (second.value().replayed ? "yes" : "no") << '\n';
  check(second.value().generation.value() == 2, "generation 2 is published");

  auto before_retry = store.info();
  if (!before_retry.has_value()) {
    report_error("Store::info", before_retry.error());
    return 1;
  }
  const StoreInfo before = before_retry.value();
  std::cout << "  head=" << before.head.value() << " floor=" << before.floor.value()
            << " retained=" << before.retained_generations
            << " idempotency_records=" << before.idempotency_records << '\n';

  section("2. (a) a mutation planned against a stale base generation is fenced");
  MutationAuthority stale_base;
  stale_base.epoch = before.epoch;
  stale_base.incarnation = before.incarnation;
  stale_base.expected_base = TopologyGeneration(1);
  std::cout << "  the request states epoch=" << stale_base.epoch.value()
            << " incarnation=" << stale_base.incarnation.value()
            << " expected_base=" << stale_base.expected_base.value()
            << ", and the committed head is " << before.head.value() << '\n';
  std::cout << "  the epoch and the incarnation are still current, so only the base generation is stale\n";
  const auto fenced = store.publish(request_of(draft_two, stale_base, "m.stale.base", 1));
  if (fenced.has_value()) {
    std::cout << "  [FAIL] the store accepted a request planned against a stale base generation\n";
    ++g_failures;
  } else {
    report_rejection("stale base", fenced.error(), ErrorCode::StaleBaseGeneration);
  }
  auto after_fence = store.info();
  if (!after_fence.has_value()) {
    report_error("Store::info", after_fence.error());
    return 1;
  }
  check(after_fence.value().head == before.head,
        "the refused request left the committed head where it was");

  section("3. (b) an exact retry of the accepted attempt is a replay, not a re-actuation");
  std::cout << "  the retry is the ORIGINAL generation-1 request: mutation="
            << first_request.mutation.str() << " attempt=" << first_request.attempt.value()
            << " expected_base=" << first_request.authority.expected_base.value()
            << "; the head has since moved to " << before.head.value() << '\n';
  const auto replay = store.publish(first_request);
  if (!replay.has_value()) {
    report_error("replay", replay.error());
  } else {
    const PublicationReceipt& receipt = replay.value();
    std::cout << "  replay: generation=" << receipt.generation.value()
              << " digest=" << receipt.digest.to_hex().substr(0, 16)
              << " replayed=" << (receipt.replayed ? "yes" : "no")
              << " head_after=" << receipt.head_after.value()
              << " durability=" << to_token(receipt.durability) << '\n';
    check(receipt.replayed, "the retry is served from the recorded accepted attempt (replayed=true)");
    check(receipt.generation.value() == 1, "the replay returns the generation the attempt published");
    check(receipt.head_after == before.head, "the replay reports the current head without publishing");
  }
  auto after_replay = store.info();
  if (!after_replay.has_value()) {
    report_error("Store::info", after_replay.error());
    return 1;
  }
  check(after_replay.value().head == before.head, "the replay did not advance the head");
  check(after_replay.value().retained_generations == before.retained_generations,
        "the replay did not write a generation file");
  check(after_replay.value().idempotency_records == before.idempotency_records,
        "the replay did not add an accepted-attempt record");

  section("4. (c) the same mutation identity with different content is a conflict");
  const auto conflict = store.publish(request_of(draft_two, stale_base, "m.gen1", 1));
  if (conflict.has_value()) {
    std::cout << "  [FAIL] the store accepted different content under an already used mutation identity\n";
    ++g_failures;
  } else {
    report_rejection("same mutation, different content", conflict.error(), ErrorCode::IdempotencyConflict);
  }

  section("5. the store is still verifiable and still at generation 2");
  const auto head = store.head();
  if (!head.has_value()) {
    report_error("Store::head", head.error());
    return 1;
  }
  std::cout << "  head generation=" << head.value().generation().value()
            << " digest=" << head.value().digest().to_hex().substr(0, 16)
            << " nodes=" << head.value().node_count() << " edges=" << head.value().edge_count() << '\n';
  check(head.value().generation().value() == 2, "every refused request left the head at generation 2");
  const auto verified = store.verify(VerifyOptions{});
  if (!verified.has_value()) {
    report_error("Store::verify", verified.error());
    return 1;
  }
  const VerifyReport& verify = verified.value();
  std::cout << "  verify: ok=" << (verify.ok() ? "yes" : "no")
            << " head_verified=" << (verify.head_verified ? "yes" : "no")
            << " manifest_verified=" << (verify.manifest_verified ? "yes" : "no")
            << " chain_verified=" << (verify.chain_verified ? "yes" : "no")
            << " generations=" << verify.generations_verified << "/" << verify.generations_present
            << " staged_residue=" << verify.staged_residue_found
            << " orphan_generations=" << verify.orphan_generations_found << '\n';
  for (const VerifyFinding& finding : verify.findings) {
    std::cout << "    finding " << to_token(finding.severity) << " " << finding.code << " ["
              << finding.subject << "] " << finding.detail << '\n';
  }
  check(verify.ok(), "verify reports the store healthy after the refused requests");

  section("6. what this run established, and what it did not");
  std::cout << "  established: a request whose stated base generation no longer matches the committed head\n"
            << "    is refused with stale_base_generation and leaves the store unchanged.\n";
  std::cout << "  established: a retry that carries the same mutation identity, attempt ordinal and content\n"
            << "    as an accepted attempt returns the recorded receipt with replayed=true, writes no new\n"
            << "    generation, and therefore re-actuates nothing.\n";
  std::cout << "  established: reusing one mutation identity for different content is refused with\n"
            << "    idempotency_conflict instead of overwriting the recorded outcome.\n";
  std::cout << "  NOT established by any answer above:\n";
  std::cout << "    - that the published structure is operating, that coolant is flowing, or that any plant\n"
            << "      is available;\n";
  std::cout << "    - that the fencing decisions are a safety property: they protect the durability protocol\n"
            << "      against lost responses and superseded writers, nothing else;\n";
  std::cout << "    - that a replayed receipt is evidence of anything beyond the fact that the attempt was\n"
            << "      already accepted by this store;\n";
  std::cout << "    - that recovery, failover or any control action was taken: none was requested.\n";
  std::cout << "  " << posture_statement() << '\n';

  section("7. clean up the temporary directory this program created");
  const auto closed = store.close();
  if (!closed.has_value()) {
    report_error("Store::close", closed.error());
  }
  check(!store.is_open(), "the store handle reports is_open=false after close");
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
  std::cout << "  OK: every refusal carried its documented code and the store stayed verifiable\n";
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
  return run((base / "cooling_topology_stale_update_example").string());
}
