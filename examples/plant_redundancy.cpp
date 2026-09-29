// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// plant_redundancy - a two-plant, two-pump facility with two sinks.
//
// The program imports an embedded "ctg" document that describes:
//
//                            +----------------> rack 2 load
//                            |
//   facility water -> chiller plant A -> primary loop A -> rack branch A -+-> rack 1 load
//   wells field    -> chiller plant B -> primary loop B -> rack branch B -+-> rack 1 load
//                            |
//                            +----------------> in-row air handler (serves the zone)
//
// and then asks the library every structural question that document supports:
// the redundancy group verdicts, the independence of the declared members, the
// structural sources of each sink, how each sink is fed, and which single
// element removals would leave a sink without any structural supply route.
//
// Everything printed here is a statement about STRUCTURE. "structurally
// possible" is not "running", not "flowing", not "capable", not "eligible" and
// not "authorized"; this component models declared connectivity only and says so
// on every answer.

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "dccp/cooling_topology/version.hpp"

namespace {

using dccp::cooling_topology::ClaimClass;
using dccp::cooling_topology::CoolingPath;
using dccp::cooling_topology::DependencyPoint;
using dccp::cooling_topology::Error;
using dccp::cooling_topology::GroupMembership;
using dccp::cooling_topology::ImportStats;
using dccp::cooling_topology::IndependentPathResult;
using dccp::cooling_topology::NodeId;
using dccp::cooling_topology::QueryOptions;
using dccp::cooling_topology::RedundancyVerdict;
using dccp::cooling_topology::RedundancyGroupReport;
using dccp::cooling_topology::RedundancyMemberReport;
using dccp::cooling_topology::SinglePointResult;
using dccp::cooling_topology::SinkFeed;
using dccp::cooling_topology::SinkServiceReport;
using dccp::cooling_topology::Topology;
using dccp::cooling_topology::TopologyDraft;

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

std::string joined_path(const CoolingPath& path) {
  std::string out;
  for (const NodeId& node : path.nodes) {
    if (!out.empty()) {
      out += " -> ";
    }
    out += node.str();
  }
  return out;
}

NodeId node_id(std::string_view spelling) {
  const auto parsed = NodeId::parse(spelling);
  return parsed.has_value() ? parsed.value() : NodeId{};
}

/// The facility document. Deterministic, embedded, and the only input.
constexpr std::string_view kFacilityDocument = R"CTG(
# Two chiller plants on two independent sources, one primary loop each, one
# pumped branch per loop, two rack loads and one air handler per branch.
facility facility:plant-redundancy-demo@3

provenance producer=cooling-topology-examples/1.0.0 origin=authored witness="plant redundancy example, revision A" source=facility:plant-redundancy-demo@3 authority-epoch=17

node src.water cooling_source kind=facility_water medium=chilled_water design-supply=6.000C name="Facility water intake"
node src.well  cooling_source kind=well          medium=chilled_water design-supply=8.000C name="Wells field"

node plant.a cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000C max-supply=12.000C name="Chiller plant A"
node plant.b cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000C max-supply=12.000C name="Chiller plant B"

node loop.a cooling_loop kind=primary medium=chilled_water design-supply=7.000C max-supply=12.000C name="Primary loop A"
node loop.b cooling_loop kind=primary medium=chilled_water design-supply=7.000C max-supply=12.000C name="Primary loop B"

node pump.a pump kind=centrifugal role=duty    name="Loop A duty pump"
node pump.b pump kind=centrifugal role=standby name="Loop B standby pump"

node branch.a branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Rack branch A"
node branch.b branch kind=rack_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Rack branch B"

node crah.a crah placement=in_row medium=chilled_water max-supply=18.000C name="In-row air handler A"
node crah.b crah placement=in_row medium=chilled_water max-supply=18.000C name="In-row air handler B"

node zone.hall1 thermal_zone class=room name="Hall 1"

node sink.rack1 cooling_sink kind=rack_load consumer=consumer:rack-1 max-supply=27.000C name="Rack 1 load"
node sink.rack2 cooling_sink kind=rack_load consumer=consumer:rack-2 max-supply=27.000C name="Rack 2 load"

edge e.src.plant.a  supplies src.water.supply_out -> plant.a.source_in
edge e.src.plant.b  supplies src.well.supply_out  -> plant.b.source_in

edge e.plant.loop.a supplies plant.a.supply_out -> loop.a.source_in
edge e.plant.loop.b supplies plant.b.supply_out -> loop.b.source_in

edge e.loop.branch.a supplies loop.a.supply_out -> branch.a.source_in
edge e.loop.branch.b supplies loop.b.supply_out -> branch.b.source_in

edge e.branch.a.sink1 supplies branch.a.supply_out -> sink.rack1.source_in
edge e.branch.b.sink1 supplies branch.b.supply_out -> sink.rack1.source_in
edge e.branch.a.sink2 supplies branch.a.supply_out -> sink.rack2.source_in

edge e.branch.a.crah  supplies branch.a.supply_out -> crah.a.source_in
edge e.branch.b.crah  supplies branch.b.supply_out -> crah.b.source_in

edge r.sink1.branch.a returns sink.rack1.heat_out -> branch.a.return_in
edge r.sink1.branch.b returns sink.rack1.heat_out -> branch.b.return_in
edge r.sink2.branch.a returns sink.rack2.heat_out -> branch.a.return_in

edge r.crah.a.branch returns crah.a.heat_out -> branch.a.return_in
edge r.crah.b.branch returns crah.b.heat_out -> branch.b.return_in

edge r.branch.a.loop returns branch.a.heat_out -> loop.a.return_in
edge r.branch.b.loop returns branch.b.heat_out -> loop.b.return_in

edge r.loop.a.plant returns loop.a.heat_out -> plant.a.return_in
edge r.loop.b.plant returns loop.b.heat_out -> plant.b.return_in

edge r.plant.a.src returns plant.a.heat_out -> src.water.return_in
edge r.plant.b.src returns plant.b.heat_out -> src.well.return_in

edge p.loop.a.pump pumps loop.a.supply_out -> pump.a.terminal
edge p.loop.b.pump pumps loop.b.supply_out -> pump.b.terminal

edge s.crah.a.zone serves crah.a.server -> zone.hall1.served
edge s.crah.b.zone serves crah.b.server -> zone.hall1.served

edge c.plant.a.loop   contains plant.a.container    -> loop.a.contained
edge c.plant.b.loop   contains plant.b.container    -> loop.b.contained
edge c.loop.a.branch  contains loop.a.container     -> branch.a.contained
edge c.loop.b.branch  contains loop.b.container     -> branch.b.contained
edge c.loop.a.pump    contains loop.a.container     -> pump.a.contained
edge c.loop.b.pump    contains loop.b.container     -> pump.b.contained
edge c.zone.sink1     contains zone.hall1.container -> sink.rack1.contained
edge c.zone.sink2     contains zone.hall1.container -> sink.rack2.contained
edge c.zone.crah.a    contains zone.hall1.container -> crah.a.contained
edge c.zone.crah.b    contains zone.hall1.container -> crah.b.contained

alias loop.primary.a loop.a

group rg.plants scope=plant scheme=two_n name="Chiller plants" require-distinct-failure-domains require-independent-sources plant.a@failure_domain:fd.hall-a plant.b@failure_domain:fd.hall-b
group rg.pumps scope=pump scheme=n_plus_one name="Loop pumps" pump.a pump.b
)CTG";

void report_group(const Topology& topology, std::string_view spelling) {
  const auto parsed = dccp::cooling_topology::RedundancyGroupId::parse(spelling);
  if (!parsed.has_value()) {
    report_error("redundancy group identity", parsed.error());
    return;
  }
  const auto result = dccp::cooling_topology::redundancy_group_report(topology, parsed.value(), QueryOptions{});
  if (!result.has_value()) {
    report_error("redundancy_group_report", result.error());
    return;
  }
  const RedundancyGroupReport& report = result.value();
  std::cout << "  group " << report.group.str() << " scope=" << dccp::cooling_topology::to_token(report.scope)
            << " scheme=" << dccp::cooling_topology::to_token(report.scheme)
            << " verdict=" << dccp::cooling_topology::to_token(report.verdict)
            << " independence_holds=" << (report.independence_holds ? "yes" : "no")
            << " truncated=" << (report.truncated ? "yes" : "no") << '\n';
  for (const RedundancyMemberReport& member : report.members) {
    std::cout << "    member " << member.node.str() << " declared=" << member.declared
              << " sources=" << names_of(member.sources)
              << " failure_domain="
              << (member.failure_domain_declared ? member.failure_domain : std::string("(none)")) << '\n';
  }
  std::cout << "    shared_sources=" << names_of(report.shared_sources)
            << " ancestor_members=" << names_of(report.ancestor_members)
            << " duplicate_failure_domains=" << report.duplicate_failure_domains.size() << '\n';
}

bool group_verdict(const Topology& topology, std::string_view spelling, RedundancyVerdict& verdict,
                   std::size_t& shared_sources) {
  const auto parsed = dccp::cooling_topology::RedundancyGroupId::parse(spelling);
  if (!parsed.has_value()) {
    report_error("redundancy group identity", parsed.error());
    return false;
  }
  const auto result = dccp::cooling_topology::redundancy_group_report(topology, parsed.value(), QueryOptions{});
  if (!result.has_value()) {
    report_error("redundancy_group_report", result.error());
    return false;
  }
  verdict = result.value().verdict;
  shared_sources = result.value().shared_sources.size();
  return true;
}

void report_sink(const Topology& topology, const NodeId& sink) {
  const auto sources = dccp::cooling_topology::sources_serving(topology, sink, QueryOptions{});
  if (!sources.has_value()) {
    report_error("sources_serving", sources.error());
    return;
  }
  std::cout << "  sink " << sink.str() << " structural_sources=" << sources.value().size() << " "
            << names_of(sources.value()) << '\n';
  const auto service = dccp::cooling_topology::sink_service_report(topology, sink, QueryOptions{});
  if (!service.has_value()) {
    report_error("sink_service_report", service.error());
    return;
  }
  const SinkServiceReport& report = service.value();
  std::cout << "    verdict=" << dccp::cooling_topology::to_token(report.verdict)
            << " feeds=" << report.feeds.size()
            << " shared_sources=" << names_of(report.shared_sources) << '\n';
  for (const SinkFeed& feed : report.feeds) {
    std::cout << "    feed " << feed.feeder.str() << " via " << feed.edge.str()
              << " sources=" << names_of(feed.sources) << '\n';
  }
}

void report_independent(const Topology& topology, std::string_view from, std::string_view to) {
  const auto result = dccp::cooling_topology::independent_supply_paths(topology, node_id(from), node_id(to),
                                                                      QueryOptions{});
  if (!result.has_value()) {
    report_error("independent_supply_paths", result.error());
    return;
  }
  const IndependentPathResult& independent = result.value();
  std::cout << "  independent " << independent.from.str() << " -> " << independent.to.str()
            << " claim=" << dccp::cooling_topology::to_token(independent.claim)
            << " edge_disjoint_paths=" << independent.path_count
            << " truncated=" << (independent.truncated ? "yes" : "no") << '\n';
  for (const CoolingPath& path : independent.paths) {
    std::cout << "    path " << joined_path(path) << '\n';
  }
}

int run() {
  using namespace dccp::cooling_topology;

  std::cout << "cooling_topology " << version_string() << " - plant_redundancy example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';

  section("1. import the embedded facility document");
  ImportStats stats;
  const auto parsed = parse_import(kFacilityDocument, &stats);
  if (!parsed.has_value()) {
    report_error("parse_import", parsed.error());
    return 1;
  }
  const TopologyDraft& draft = parsed.value();
  std::cout << "  lines=" << stats.lines << " nodes=" << stats.nodes << " edges=" << stats.edges
            << " groups=" << stats.groups << " aliases=" << stats.aliases
            << " changeovers=" << stats.changeovers << " evidence=" << stats.evidence << '\n';
  check(stats.nodes == 15, "the document declares 15 elements");
  check(stats.edges == 36, "the document declares 36 edges");
  check(stats.groups == 2, "the document declares two redundancy groups");

  section("2. build the generation");
  const auto created = Topology::create_first(draft);
  if (!created.has_value()) {
    report_error("Topology::create_first", created.error());
    return 1;
  }
  const Topology& topology = created.value();
  std::cout << "  generation=" << topology.generation().value()
            << " digest=" << topology.digest().to_hex() << '\n';
  std::cout << "  nodes=" << topology.node_count() << " edges=" << topology.edge_count()
            << " groups=" << topology.group_count() << " aliases=" << topology.aliases().size() << '\n';
  for (const Node& node : topology.nodes()) {
    std::cout << "    " << describe_node(topology, node) << '\n';
  }
  check(topology.node_count() == 15 && topology.edge_count() == 36,
        "the generation holds the 15 elements and 36 edges the document declared");
  check(!topology.digest().is_zero(), "the generation carries a non-zero canonical digest");

  section("3. structural sources of the generation");
  const std::vector<NodeId> sources = topology.structural_sources();
  std::cout << "  structural_sources=" << names_of(sources) << '\n';
  check(sources.size() == 2, "the generation has exactly two structural sources");
  check(sources.size() == 2 && sources[0].str() == "src.water" && sources[1].str() == "src.well",
        "the two structural sources are src.water and src.well, in canonical order");

  const NodeId plant_a = node_id("plant.a");
  const NodeId plant_b = node_id("plant.b");
  const NodeId pump_a = node_id("pump.a");
  const NodeId sink_rack1 = node_id("sink.rack1");
  const NodeId sink_rack2 = node_id("sink.rack2");

  section("4. redundancy group verdicts");
  report_group(topology, "rg.plants");
  report_group(topology, "rg.pumps");
  RedundancyVerdict plants_verdict = RedundancyVerdict::IndependenceUnproven;
  RedundancyVerdict pumps_verdict = RedundancyVerdict::IndependenceUnproven;
  std::size_t plants_shared = 1;
  std::size_t pumps_shared = 1;
  const bool plants_read = group_verdict(topology, "rg.plants", plants_verdict, plants_shared);
  const bool pumps_read = group_verdict(topology, "rg.pumps", pumps_verdict, pumps_shared);
  check(plants_read && plants_verdict == RedundancyVerdict::DeclarationConsistent,
        "the plant group's declared independence is consistent with the graph");
  check(plants_read && plants_shared == 0, "the two plants share no structural source in this generation");
  check(pumps_read && pumps_verdict == RedundancyVerdict::IndependenceUnproven,
        "the pump group declares no independence property, so its verdict is independence_unproven");
  check(pumps_read && pumps_shared == 0,
        "the pump group happens to share no source, which is reported but is not a declaration");

  section("5. independence of the declared members and their sources");
  report_independent(topology, "src.water", "sink.rack1");
  report_independent(topology, "src.well", "sink.rack1");
  report_independent(topology, "plant.a", "sink.rack1");
  const auto water_paths = independent_supply_paths(topology, node_id("src.water"), sink_rack1, QueryOptions{});
  const auto well_paths = independent_supply_paths(topology, node_id("src.well"), sink_rack1, QueryOptions{});
  check(water_paths.has_value() && water_paths.value().path_count == 1,
        "src.water has exactly one edge-disjoint route to rack 1");
  check(well_paths.has_value() && well_paths.value().path_count == 1,
        "src.well has exactly one edge-disjoint route to rack 1");
  for (const NodeId& member : {plant_a, plant_b, pump_a}) {
    const auto membership = redundancy_membership(topology, member);
    if (!membership.has_value()) {
      report_error("redundancy_membership", membership.error());
      continue;
    }
    std::cout << "  membership " << membership.value().node.str()
              << " groups=" << membership.value().memberships.size();
    for (const GroupMembership& entry : membership.value().memberships) {
      std::cout << " [" << entry.group.str() << " index=" << entry.member_index
                << " declared=" << entry.declared << "]";
    }
    std::cout << '\n';
  }

  section("6. how each sink is structurally fed");
  report_sink(topology, sink_rack1);
  report_sink(topology, sink_rack2);
  const auto rack1_service = sink_service_report(topology, sink_rack1, QueryOptions{});
  const auto rack2_service = sink_service_report(topology, sink_rack2, QueryOptions{});
  check(rack1_service.has_value() && rack1_service.value().feeds.size() == 2 &&
            rack1_service.value().verdict == SinkFeedVerdict::MultipleIndependentFeeds,
        "rack 1 has two structural feeds from disjoint sources");
  check(rack2_service.has_value() && rack2_service.value().feeds.size() == 1 &&
            rack2_service.value().verdict == SinkFeedVerdict::SingleFeed,
        "rack 2 has exactly one structural feed route");
  const auto rack1_sources = sources_serving(topology, sink_rack1, QueryOptions{});
  const auto rack2_sources = sources_serving(topology, sink_rack2, QueryOptions{});
  check(rack1_sources.has_value() && rack1_sources.value().size() == 2,
        "both structural sources can reach rack 1");
  check(rack2_sources.has_value() && rack2_sources.value().size() == 1 &&
            rack2_sources.value()[0].str() == "src.water",
        "only src.water can structurally reach rack 2");
  const auto zone = zone_report(topology, node_id("zone.hall1"), QueryOptions{});
  if (!zone.has_value()) {
    report_error("zone_report", zone.error());
    return 1;
  }
  std::cout << "  zone " << zone.value().zone.str() << " class=" << to_token(zone.value().zone_class)
            << " verdict=" << to_token(zone.value().verdict)
            << " serving=" << names_of(zone.value().serving_elements)
            << " contained_sinks=" << names_of(zone.value().contained_sinks)
            << " contained_air_handlers=" << names_of(zone.value().contained_air_handlers) << "\n";
  check(zone.value().verdict == ZoneVerdict::Served && zone.value().contained_air_handlers.size() == 2,
        "the zone is served by two declared air handlers and contains both rack loads");

  section("7. single points of structural dependency");
  const auto rack1_points = single_points_of_structural_dependency(topology, {sink_rack1}, QueryOptions{});
  if (!rack1_points.has_value()) {
    report_error("single_points_of_structural_dependency", rack1_points.error());
    return 1;
  }
  const SinglePointResult& single = rack1_points.value();
  std::cout << "  subjects=" << names_of(single.subjects)
            << " claim=" << to_token(single.claim) << " points=" << single.points.size()
            << " truncated=" << (single.truncated ? "yes" : "no") << '\n';
  check(single.points.empty(),
        "no single declared element removal leaves rack 1 without any structural supply route");
  const auto both_points =
      single_points_of_structural_dependency(topology, {sink_rack1, sink_rack2}, QueryOptions{});
  if (!both_points.has_value()) {
    report_error("single_points_of_structural_dependency", both_points.error());
    return 1;
  }
  const SinglePointResult& shared = both_points.value();
  std::cout << "  subjects=" << names_of(shared.subjects) << " claim=" << to_token(shared.claim)
            << " points=" << shared.points.size() << '\n';
  bool any_disconnects_all = false;
  for (const DependencyPoint& point : shared.points) {
    any_disconnects_all = any_disconnects_all || point.disconnects_all_subjects;
    std::cout << "    point " << point.node.str() << " disconnects=" << point.disconnected_subjects.size()
              << " all=" << (point.disconnects_all_subjects ? "yes" : "no")
              << " structural_source=" << (point.is_structural_source ? "yes" : "no")
              << " subjects=" << names_of(point.disconnected_subjects) << '\n';
  }
  check(!shared.points.empty(), "several elements are single points for the sink pair");
  check(!any_disconnects_all, "no listed element disconnects both sinks at once");

  section("8. what this run established, and what it did not");
  std::cout << "  established: the declared structure of generation " << topology.generation().value()
            << " contains two sources, two plants, two loops, two pumps,\n"
            << "    two rack branches, two air handlers and two sinks, and the declared plant independence\n"
            << "    is consistent with that structure (no shared structural source, distinct failure\n"
            << "    domains).\n";
  std::cout << "  established: rack 1 is fed by two routes from disjoint sources; rack 2 is fed by one\n"
            << "    route, so every element on that route is a single structural dependency of rack 2.\n";
  std::cout << "  NOT established by any answer above:\n";
  std::cout << "    - that either plant, loop or pump is running, or that either pump is duty or standby in\n"
            << "      service: pump role is a declaration about how the plant is built;\n";
  std::cout << "    - that coolant is flowing anywhere, or that any route has usable cooling capacity;\n";  std::cout << "    - that the standby pump is eligible, or that a failover is permitted or possible;\n";
  std::cout << "    - that any sink is thermally safe: no temperature, load or flow is measured here;\n";
  std::cout << "    - that the second plant is available, or that the two branches are energized.\n";
  std::cout << "  " << posture_statement() << '\n';

  section("result");
  if (g_failures != 0) {
    std::cout << "  FAILED: " << g_failures << " expectation(s) not met\n";
    return 1;
  }
  std::cout << "  OK: every structural expectation held\n";
  return 0;
}

}  // namespace

int main() { return run(); }
