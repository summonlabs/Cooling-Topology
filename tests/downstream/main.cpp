// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// downstream_consumer - an out-of-tree consumer of the installed package.
//
// This program only ever sees the installed headers and the imported target
// dccp::cooling_topology. It imports a document, builds a generation, frames and
// decodes its canonical image, asks a supply-path question and a redundancy
// question of it, and then runs the whole durable lifecycle in the one directory
// it was given as argv[1]: create a store, publish one generation, read the head
// back, verify the store, close it. It writes nothing outside that directory.
//
// It exits 0 only when every check holds, 2 on a usage error and 1 otherwise.

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/evidence.hpp"
#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/store.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "dccp/cooling_topology/version.hpp"

#ifndef COOLING_TOPOLOGY_PACKAGE_VERSION
#error "COOLING_TOPOLOGY_PACKAGE_VERSION must be defined by the build system"
#endif

namespace {

using namespace dccp::cooling_topology;

int g_failures = 0;

void check(bool condition, std::string_view what) {
  std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
  if (!condition) {
    ++g_failures;
  }
}

void section(std::string_view title) { std::cout << "\n" << title << '\n'; }

std::string names_of(const std::vector<NodeId>& ids) {
  std::string out;
  for (const NodeId& id : ids) {
    if (!out.empty()) {
      out += ", ";
    }
    out += id.str();
  }
  return out.empty() ? std::string("(none)") : out;
}

bool contains(const std::vector<NodeId>& ids, std::string_view identity) {
  for (const NodeId& id : ids) {
    if (id.value() == identity) {
      return true;
    }
  }
  return false;
}

/// A small consumer document: one facility-water source feeding two chiller
/// plants, a secondary loop with a duty and a standby pump, one manifold, one
/// rack branch, one CDU, one rack sink and the in-row CRAH that serves its
/// cold-aisle zone. The source declares no medium, so no medium-continuity
/// relation is claimed across the facility boundary; every declaration below is
/// structural only and says nothing about operation.
constexpr std::string_view kDocument = R"CTG(facility facility:consumer-site@3

provenance producer="cooling-topology-downstream/1.0.0" origin=imported witness="downstream consumer document"

node source-fw cooling_source kind=facility_water
node plant-a cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000 max-supply=12.500
node plant-b cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000 max-supply=12.500
node loop-sec cooling_loop kind=secondary medium=chilled_water design-supply=7.000 max-supply=12.500
node pump-a pump kind=centrifugal role=duty
node pump-b pump kind=centrifugal role=standby
node manifold-m1 manifold kind=combined medium=chilled_water design-supply=7.000 max-supply=12.500
node branch-b1 branch kind=rack_branch medium=chilled_water design-supply=7.000 max-supply=12.500
node cdu-c1 cdu kind=rack_cdu medium=chilled_water design-supply=7.000 max-supply=12.500
node sink-r7 cooling_sink kind=rack_load consumer=rack-7 consumer-kind=consumer max-supply=25.000
node crah-1 crah placement=in_row medium=chilled_water max-supply=18.000
node zone-z1 thermal_zone class=cold_aisle

edge e-source-plant-a supplies source-fw.supply_out -> plant-a.source_in
edge e-source-plant-b supplies source-fw.supply_out -> plant-b.source_in
edge e-plant-loop supplies plant-a.supply_out -> loop-sec.source_in
edge e-loop-manifold supplies loop-sec.supply_out -> manifold-m1.source_in
edge e-manifold-branch supplies manifold-m1.supply_out -> branch-b1.source_in
edge e-branch-cdu supplies branch-b1.supply_out -> cdu-c1.source_in
edge e-cdu-sink supplies cdu-c1.supply_out -> sink-r7.source_in
edge e-sink-cdu returns sink-r7.heat_out -> cdu-c1.return_in
edge e-cdu-branch returns cdu-c1.heat_out -> branch-b1.return_in
edge e-branch-manifold returns branch-b1.heat_out -> manifold-m1.return_in
edge e-manifold-loop returns manifold-m1.heat_out -> loop-sec.return_in
edge e-loop-plant returns loop-sec.heat_out -> plant-a.return_in
edge e-plant-source returns plant-a.heat_out -> source-fw.return_in
edge e-loop-crah supplies loop-sec.supply_out -> crah-1.source_in
edge e-crah-zone serves crah-1.server -> zone-z1.served
edge e-loop-pump-a pumps loop-sec.supply_out -> pump-a.terminal
edge e-loop-pump-b pumps loop-sec.supply_out -> pump-b.terminal
edge e-contains-manifold contains loop-sec.container -> manifold-m1.contained
edge e-contains-branch contains manifold-m1.container -> branch-b1.contained
edge e-contains-sink contains zone-z1.container -> sink-r7.contained
edge e-contains-crah contains zone-z1.container -> crah-1.contained

group group:plants scope=plant scheme=n_plus_one name="Chiller plants" plant-a plant-b
group group:pumps scope=pump scheme=n_plus_one name="Secondary loop pumps" pump-a pump-b
)CTG";

/// One framed canonical image of a generation, or an empty string when the
/// generation cannot be encoded. Never writes anywhere.
std::string framed_image(const Topology& topology) {
  const auto payload = topology.canonical_bytes();
  if (!payload.has_value()) {
    std::cout << "  [FAIL] canonical_bytes: " << payload.error().to_string() << '\n';
    return std::string();
  }
  const auto framed = encode_generation_file(payload.value());
  if (!framed.has_value()) {
    std::cout << "  [FAIL] encode_generation_file: " << framed.error().to_string() << '\n';
    return std::string();
  }
  return framed.value();
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "downstream consumer of the installed cooling_topology package\n";
  std::cout << "  package_version_macro=" << COOLING_TOPOLOGY_PACKAGE_VERSION << '\n';
  std::cout << "  library_version_string=" << version_string() << '\n';
  std::cout << "  component_id=" << component_id() << '\n';
  std::cout << "  boundary=" << systems_boundary() << '\n';

  if (argc != 2) {
    std::cout << "usage: downstream_consumer <absolute-store-directory>\n";
    return 2;
  }
  const std::string store_root = argv[1];
  if (store_root.empty() || !std::filesystem::path(store_root).is_absolute()) {
    std::cout << "  [FAIL] the store directory argument must be an absolute path\n";
    return 2;
  }
  std::cout << "  store_root=" << store_root << '\n';

  check(!std::string_view(COOLING_TOPOLOGY_PACKAGE_VERSION).empty(), "the found package reported a version");
  check(version_string() == std::string_view(COOLING_TOPOLOGY_PACKAGE_VERSION),
        "the linked library version equals the version the package was found at");

  section("1. import a document and build a generation");
  const auto parsed = parse_import(kDocument);
  if (!parsed.has_value()) {
    std::cout << "  [FAIL] parse_import: " << parsed.error().to_string() << '\n';
    return 1;
  }
  const auto created = Topology::create_first(parsed.value());
  if (!created.has_value()) {
    std::cout << "  [FAIL] Topology::create_first: " << created.error().to_string() << '\n';
    return 1;
  }
  const Topology& topology = created.value();
  std::cout << "  generation=" << topology.generation().value() << '\n';
  std::cout << "  digest=" << topology.digest().to_hex() << '\n';
  std::cout << "  nodes=" << topology.node_count() << " edges=" << topology.edge_count()
            << " groups=" << topology.group_count() << '\n';
  std::cout << "  facility=" << to_token(topology.facility().kind) << ":" << topology.facility().identity << "@"
            << topology.facility().generation.value() << '\n';
  check(topology.generation().value() == TopologyGeneration::kFirstPublished, "the consumer built generation 1");
  check(!topology.digest().is_zero(), "the built generation carries a digest");

  section("2. frame the canonical image and decode it again");
  const std::string framed = framed_image(topology);
  if (framed.empty()) {
    return 1;
  }
  std::cout << "  framed_bytes=" << framed.size() << " magic=" << framed.substr(0, 8) << '\n';
  const auto decoded = Topology::decode(framed);
  if (!decoded.has_value()) {
    std::cout << "  [FAIL] Topology::decode: " << decoded.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  decoded_digest=" << decoded.value().digest().to_hex() << '\n';
  check(decoded.value().digest() == topology.digest(), "the decoded generation carries the same digest");
  check(decoded.value().node_count() == topology.node_count(), "the decoded generation has the same node count");
  check(decoded.value().edge_count() == topology.edge_count(), "the decoded generation has the same edge count");

  const auto source = NodeId::parse("source-fw");
  const auto sink = NodeId::parse("sink-r7");
  if (!source.has_value() || !sink.has_value()) {
    std::cout << "  [FAIL] embedded identities are not valid\n";
    return 1;
  }

  section("3. a structural supply-path question");
  const auto paths = possible_supply_paths(topology, source.value(), sink.value());
  if (!paths.has_value()) {
    std::cout << "  [FAIL] possible_supply_paths: " << paths.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  query: possible_supply_paths(source-fw -> sink-r7) claim=" << to_token(paths.value().claim)
            << " paths=" << paths.value().paths.size()
            << " reachable_sources=" << names_of(paths.value().reachable_sources) << '\n';
  check(paths.value().claim == ClaimClass::StructurallyPossible, "the supply relation is structurally possible");
  check(!paths.value().paths.empty(), "at least one structural supply path reaches the sink");
  check(contains(paths.value().reachable_sources, "source-fw"), "the source structurally reaches the sink");

  section("4. a redundancy question");
  const auto group_id = RedundancyGroupId::parse("group:plants");
  if (!group_id.has_value()) {
    std::cout << "  [FAIL] RedundancyGroupId::parse: " << group_id.error().to_string() << '\n';
    return 1;
  }
  const auto group = redundancy_group_report(topology, group_id.value(), QueryOptions{});
  if (!group.has_value()) {
    std::cout << "  [FAIL] redundancy_group_report: " << group.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  query: redundancy_group_report(group:plants) scope=" << to_token(group.value().scope)
            << " scheme=" << to_token(group.value().scheme) << " verdict=" << to_token(group.value().verdict)
            << " independence_holds=" << (group.value().independence_holds ? "true" : "false")
            << " shared_sources=" << names_of(group.value().shared_sources) << '\n';
  check(group.value().members.size() == 2, "the declared plant group has two members");
  check(contains(group.value().shared_sources, "source-fw"),
        "both declared plants draw on the same structural source, so no independence is proven");

  section("5. publish one generation into a durable store");
  StoreOptions options;
  options.root = store_root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 2;
  options.idempotency_retention = 4;
  options.durable_flush = true;

  const auto store_id = StoreId::parse("store.downstream-consumer");
  if (!store_id.has_value()) {
    std::cout << "  [FAIL] the store identity is not valid\n";
    return 1;
  }
  // A store is bound to the facility it holds and refuses a generation that
  // describes another one, so the binding is taken from the document.
  auto opened = Store::create(options, store_id.value(), parsed.value().facility);
  if (!opened.has_value()) {
    std::cout << "  [FAIL] Store::create: " << opened.error().to_string() << '\n';
    return 1;
  }
  Store store = std::move(opened.value());
  const auto info = store.info();
  if (!info.has_value()) {
    std::cout << "  [FAIL] Store::info: " << info.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  store=" << info.value().store_id.str() << " open_state=" << to_token(info.value().open_state)
            << " epoch=" << info.value().epoch.value() << " incarnation=" << info.value().incarnation.value()
            << " head=" << info.value().head.value() << '\n';
  check(info.value().open_state == StoreOpenState::Fresh, "a newly created store reports open_state=fresh");
  check(!info.value().head.published(), "a newly created store has no published head");

  const auto ordinal = AttemptOrdinal::parse(1);
  const auto mutation = MutationId::parse("m.downstream.1");
  if (!ordinal.has_value() || !mutation.has_value()) {
    std::cout << "  [FAIL] the mutation identity or attempt ordinal is not valid\n";
    return 1;
  }
  MutationAuthority authority;
  authority.epoch = info.value().epoch;
  authority.incarnation = info.value().incarnation;
  authority.expected_base = info.value().head;
  PublicationRequest request;
  request.authority = authority;
  request.mutation = mutation.value();
  request.attempt = ordinal.value();
  request.draft = parsed.value();
  const auto receipt = store.publish(request);
  if (!receipt.has_value()) {
    std::cout << "  [FAIL] Store::publish: " << receipt.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  published generation=" << receipt.value().generation.value()
            << " parent=" << receipt.value().parent_generation.value()
            << " durability=" << to_token(receipt.value().durability)
            << " replayed=" << (receipt.value().replayed ? "true" : "false") << '\n';
  check(receipt.value().generation.value() == TopologyGeneration::kFirstPublished,
        "the first publication is generation 1");
  check(receipt.value().digest == topology.digest(), "the published digest is the digest of the built generation");

  section("6. read the head back and verify the store");
  const auto head = store.head();
  if (!head.has_value()) {
    std::cout << "  [FAIL] Store::head: " << head.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  head generation=" << head.value().generation().value()
            << " digest=" << head.value().digest().to_hex() << " nodes=" << head.value().node_count()
            << " edges=" << head.value().edge_count() << '\n';
  check(head.value().generation().value() == receipt.value().generation.value(),
        "the head read back is the generation that was published");
  check(head.value().digest() == topology.digest(), "the head read back carries the published digest");

  const auto verified = store.verify(VerifyOptions{});
  if (!verified.has_value()) {
    std::cout << "  [FAIL] Store::verify: " << verified.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  verify head_verified=" << (verified.value().head_verified ? "true" : "false")
            << " chain_verified=" << (verified.value().chain_verified ? "true" : "false")
            << " canonical_fixed_point=" << (verified.value().canonical_fixed_point_verified ? "true" : "false")
            << " generations_verified=" << verified.value().generations_verified
            << " findings=" << verified.value().findings.size() << '\n';
  check(verified.value().ok(), "the store verifies without a defect finding");
  check(verified.value().head_verified, "the retained head verifies");
  check(verified.value().generations_verified == 1, "the one published generation was verified");

  const auto closed = store.close();
  if (!closed.has_value()) {
    std::cout << "  [FAIL] Store::close: " << closed.error().to_string() << '\n';
    return 1;
  }
  check(!store.is_open(), "the store handle is closed");

  std::cout << "\n  posture: " << posture_statement() << '\n';
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s) not met\n";
    return 1;
  }
  std::cout << "OK: the installed package is usable out of tree\n";
  return 0;
}
