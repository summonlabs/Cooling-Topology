// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// primary_secondary_circuit - one hydronic circuit, from the plant to the load
// and back again.
//
// The program imports an embedded "ctg" document that describes a primary /
// secondary pumped arrangement:
//
//   facility water -> chiller plant -> primary loop -> secondary loop -> branch -+-> load
//          ^                                                                     |
//          +----------------------------- return path ---------------------------+
//                                        branch also feeds one room air handler
//
// It then shows four structural facts and the boundary between them:
//
//   (a) the complete circuit: the supply path out of the plant and the return
//       path back into it, as the library reports the pair;
//   (b) supply edges alone form a DAG: no supply route leads from the load back
//       to the plant, and the upstream closure of the load never re-reaches it;
//   (c) the validator refuses a document whose supply edges close a cycle, with
//       SUPPLY_CYCLE, because a hydronic circuit is only a cycle when supply and
//       return are read together;
//   (d) supply plus return do form the circuit cycle that (b) and (c) forbid for
//       supply alone.
//
// Nothing here is a claim that any pump is running, that coolant is flowing,
// that the circuit is balanced, that the coils are clean, or that the load is
// thermally safe.

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

using dccp::cooling_topology::CoolingCircuit;
using dccp::cooling_topology::CoolingPath;
using dccp::cooling_topology::Edge;
using dccp::cooling_topology::EdgeId;
using dccp::cooling_topology::EdgeKind;
using dccp::cooling_topology::Endpoint;
using dccp::cooling_topology::Error;
using dccp::cooling_topology::ImportStats;
using dccp::cooling_topology::NodeId;
using dccp::cooling_topology::PortRole;
using dccp::cooling_topology::QueryOptions;
using dccp::cooling_topology::ReachedElement;
using dccp::cooling_topology::Topology;
using dccp::cooling_topology::TopologyDraft;
using dccp::cooling_topology::UpstreamResult;
using dccp::cooling_topology::ValidationIssue;
using dccp::cooling_topology::ValidationReport;

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

NodeId node_id(std::string_view spelling) {
  const auto parsed = NodeId::parse(spelling);
  return parsed.has_value() ? parsed.value() : NodeId{};
}

std::string chain(const std::vector<NodeId>& nodes) {
  std::string out;
  for (const NodeId& node : nodes) {
    if (!out.empty()) {
      out += " -> ";
    }
    out += node.str();
  }
  return out.empty() ? std::string("(empty)") : out;
}

std::string edges_of(const std::vector<EdgeId>& edges) {
  std::string out;
  for (const EdgeId& edge : edges) {
    if (!out.empty()) {
      out += ", ";
    }
    out += edge.str();
  }
  return out.empty() ? std::string("(empty)") : out;
}

/// The primary / secondary document. Deterministic, embedded, and the only
/// input.
constexpr std::string_view kCircuitDocument = R"CTG(
# Primary / secondary arrangement: the plant feeds the primary loop, the primary
# loop feeds the secondary loop, and the secondary loop feeds one rack branch
# and one room air handler.
facility facility:primary-secondary-demo@5

provenance producer=cooling-topology-examples/1.0.0 origin=authored witness="primary/secondary circuit example" source=facility:primary-secondary-demo@5 authority-epoch=23

node src.facility cooling_source kind=facility_water medium=chilled_water design-supply=6.000C name="Facility water intake"

node plant.pri cooling_plant kind=chiller_plant medium=chilled_water design-supply=6.500C max-supply=12.000C name="Chiller plant"

node loop.primary   cooling_loop kind=primary   medium=chilled_water design-supply=6.500C max-supply=12.000C name="Primary loop"
node loop.secondary cooling_loop kind=secondary medium=chilled_water design-supply=7.000C max-supply=18.000C name="Secondary loop"

node pump.pri pump kind=centrifugal role=duty name="Primary pump"
node pump.sec pump kind=centrifugal role=duty name="Secondary pump"

node branch.sec branch kind=zone_branch medium=chilled_water design-supply=7.000C max-supply=20.000C name="Zone branch"

node crah.room crah placement=in_row medium=chilled_water max-supply=18.000C name="Room air handler"

node zone.room thermal_zone class=room name="White space"

node sink.load cooling_sink kind=device_load consumer=consumer:cdu-1 max-supply=25.000C name="Row CDU load"

edge e.src.plant      supplies src.facility.supply_out   -> plant.pri.source_in
edge e.plant.primary  supplies plant.pri.supply_out      -> loop.primary.source_in
edge e.primary.second supplies loop.primary.supply_out   -> loop.secondary.source_in
edge e.second.branch  supplies loop.secondary.supply_out -> branch.sec.source_in
edge e.branch.load    supplies branch.sec.supply_out     -> sink.load.source_in
edge e.branch.crah    supplies branch.sec.supply_out     -> crah.room.source_in

edge r.load.branch    returns sink.load.heat_out         -> branch.sec.return_in
edge r.crah.branch    returns crah.room.heat_out         -> branch.sec.return_in
edge r.branch.second  returns branch.sec.heat_out        -> loop.secondary.return_in
edge r.second.primary returns loop.secondary.heat_out    -> loop.primary.return_in
edge r.primary.plant  returns loop.primary.heat_out      -> plant.pri.return_in
edge r.plant.src      returns plant.pri.heat_out         -> src.facility.return_in

edge p.primary.pump   pumps loop.primary.supply_out   -> pump.pri.terminal
edge p.secondary.pump pumps loop.secondary.supply_out -> pump.sec.terminal

edge s.crah.zone      serves crah.room.server -> zone.room.served

edge c.plant.primary    contains plant.pri.container       -> loop.primary.contained
edge c.primary.pump     contains loop.primary.container    -> pump.pri.contained
edge c.secondary.branch contains loop.secondary.container  -> branch.sec.contained
edge c.secondary.pump   contains loop.secondary.container  -> pump.sec.contained
edge c.zone.load        contains zone.room.container       -> sink.load.contained
edge c.zone.crah        contains zone.room.container       -> crah.room.contained
)CTG";

/// The same facility with one supply connection written the other way round:
/// the secondary loop also supplies the primary loop. Structure alone cannot
/// tell this apart from a legitimate connection, so the validator refuses it
/// because supply edges must stay acyclic.
bool with_reversed_supply(const TopologyDraft& draft, TopologyDraft& mutated) {
  mutated = draft;
  const auto id = EdgeId::parse("e.secondary.primary.reversed");
  if (!id.has_value()) {
    report_error("edge identity", id.error());
    return false;
  }
  const auto edge = Edge::create(id.value(), EdgeKind::Supplies,
                                 Endpoint{node_id("loop.secondary"), PortRole::SupplyOut},
                                 Endpoint{node_id("loop.primary"), PortRole::SourceIn});
  if (!edge.has_value()) {
    report_error("Edge::create", edge.error());
    return false;
  }
  mutated.edges.push_back(edge.value());
  return true;
}

void print_report(const ValidationReport& report) {
  for (const ValidationIssue& issue : report.issues) {
    std::cout << "    " << dccp::cooling_topology::to_token(issue.severity) << " "
              << dccp::cooling_topology::error_code_name(issue.code) << " " << issue.subject << ": "
              << issue.message << '\n';
  }
  const ValidationIssue* primary = report.primary();
  if (primary != nullptr) {
    std::cout << "    primary " << dccp::cooling_topology::error_code_name(primary->code) << " "
              << primary->subject << ": " << primary->message << '\n';
  }
}

int run() {
  using namespace dccp::cooling_topology;

  std::cout << "cooling_topology " << version_string() << " - primary_secondary_circuit example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';

  section("1. import and build the generation");
  ImportStats stats;
  const auto parsed = parse_import(kCircuitDocument, &stats);
  if (!parsed.has_value()) {
    report_error("parse_import", parsed.error());
    return 1;
  }
  const TopologyDraft& draft = parsed.value();
  std::cout << "  lines=" << stats.lines << " nodes=" << stats.nodes << " edges=" << stats.edges
            << " groups=" << stats.groups << " aliases=" << stats.aliases << '\n';
  const auto created = Topology::create_first(draft);
  if (!created.has_value()) {
    report_error("Topology::create_first", created.error());
    return 1;
  }
  const Topology& topology = created.value();
  std::cout << "  generation=" << topology.generation().value()
            << " digest=" << topology.digest().to_hex() << '\n';
  std::cout << "  nodes=" << topology.node_count() << " edges=" << topology.edge_count() << '\n';
  for (const Node& node : topology.nodes()) {
    std::cout << "    " << describe_node(topology, node) << '\n';
  }
  for (const Edge& edge : topology.edges()) {
    std::cout << "    " << describe_edge(topology, edge) << '\n';
  }
  check(topology.node_count() == 10, "the generation holds the ten declared elements");
  check(topology.edge_count() == 21, "the generation holds the twenty-one declared edges");
  check(!topology.digest().is_zero(), "the generation carries a non-zero canonical digest");

  const NodeId source = node_id("src.facility");
  const NodeId load = node_id("sink.load");

  section("2. the complete circuit: supply path out, return path back");
  const auto circuits = possible_circuits(topology, source, load, QueryOptions{});
  if (!circuits.has_value()) {
    report_error("possible_circuits", circuits.error());
    return 1;
  }
  std::cout << "  from=" << circuits.value().from.str() << " to=" << circuits.value().to.str()
            << " claim=" << to_token(circuits.value().claim)
            << " circuits=" << circuits.value().circuits.size()
            << " truncated=" << (circuits.value().truncated ? "yes" : "no") << '\n';
  for (const CoolingCircuit& circuit : circuits.value().circuits) {
    std::cout << "    supply " << chain(circuit.supply_nodes) << '\n';
    std::cout << "      via " << edges_of(circuit.supply_edges) << '\n';
    std::cout << "    return " << chain(circuit.return_nodes) << '\n';
    std::cout << "      via " << edges_of(circuit.return_edges) << '\n';
  }
  check(circuits.value().circuits.size() == 1, "exactly one structural circuit joins the plant and the load");
  if (circuits.value().circuits.size() == 1) {
    const CoolingCircuit& circuit = circuits.value().circuits.front();
    check(circuit.supply_nodes.size() == 6 && circuit.supply_edges.size() == 5,
          "the supply path visits six elements over five supply edges");
    check(circuit.return_nodes.size() == 6 && circuit.return_edges.size() == 5,
          "the return path visits six elements over five return edges");
    check(circuit.supply_nodes.front() == source && circuit.supply_nodes.back() == load,
          "the supply path leaves the plant and arrives at the load");
    check(circuit.return_nodes.front() == load && circuit.return_nodes.back() == source,
          "the return path leaves the load and arrives back at the plant");
    check(!circuit.crosses_changeover,
          "no declared changeover group constrains this circuit in this generation");
  }

  section("3. supply edges alone are a DAG");
  const auto reverse = possible_supply_paths(topology, load, source, QueryOptions{});
  if (!reverse.has_value()) {
    report_error("possible_supply_paths", reverse.error());
    return 1;
  }
  std::cout << "  possible_supply_paths(" << load.str() << " -> " << source.str()
            << ") claim=" << to_token(reverse.value().claim)
            << " paths=" << reverse.value().paths.size() << '\n';
  check(reverse.value().paths.empty(),
        "no supply route leads from the load back to the plant: supply alone is acyclic here");
  check(reverse.value().claim == ClaimClass::StructurallyImpossible,
        "the library answers structurally_impossible for the reversed supply relation");

  const auto upstream = upstream_of(topology, load, QueryOptions{});
  if (!upstream.has_value()) {
    report_error("upstream_of", upstream.error());
    return 1;
  }
  const UpstreamResult& closure = upstream.value();
  std::cout << "  upstream_of(" << load.str() << ") claim=" << to_token(closure.claim)
            << " reached=" << closure.elements.size()
            << " truncated=" << (closure.truncated ? "yes" : "no") << '\n';
  bool origin_reached = false;
  for (const ReachedElement& element : closure.elements) {
    origin_reached = origin_reached || element.node == load;
    std::cout << "    depth=" << element.depth << " " << element.node.str()
              << " via=" << element.via_edge.str() << " port=" << to_token(element.entered_port) << '\n';
  }
  check(!origin_reached, "the upstream closure of the load never re-reaches the load itself");
  check(!closure.truncated, "the supply closure is complete within the configured bounds");

  section("4. the validator refuses supply edges that close a cycle");
  TopologyDraft mutated;
  if (!with_reversed_supply(draft, mutated)) {
    return 1;
  }
  const ValidationReport report = Topology::validate_draft(mutated);
  std::cout << "  issues errors=" << report.error_count() << " warnings=" << report.warning_count()
            << " valid=" << (report.valid() ? "yes" : "no") << '\n';
  print_report(report);
  check(!report.valid(), "the mutated document is structurally invalid");
  check(report.primary() != nullptr && report.primary()->code == ErrorCode::SupplyCycle,
        "the primary rejection is SUPPLY_CYCLE: supply edges must remain acyclic");
  const auto refused = Topology::create_first(mutated);
  check(!refused.has_value() && refused.error().code() == ErrorCode::SupplyCycle,
        "Topology::create_first refuses the mutated document with supply_cycle");
  if (!refused.has_value()) {
    std::cout << "  refusal: " << error_code_name(refused.error().code()) << ": "
              << refused.error().message() << '\n';
  }

  section("5. supply plus return do form the circuit cycle");
  std::size_t overlap = 0;
  if (circuits.value().circuits.size() == 1) {
    const CoolingCircuit& circuit = circuits.value().circuits.front();
    for (const EdgeId& supply_edge : circuit.supply_edges) {
      for (const EdgeId& return_edge : circuit.return_edges) {
        if (supply_edge == return_edge) {
          ++overlap;
        }
      }
    }
    std::cout << "  the same five elements appear as supply nodes and as return nodes, in opposite order\n";
    std::cout << "  supply edges and return edges are disjoint kinds and share no edge: overlap=" << overlap
              << '\n';
  }
  check(overlap == 0, "the supply edge set and the return edge set are disjoint");
  check(circuits.value().claim == ClaimClass::StructurallyPossible,
        "the closed circuit is structurally possible in this generation");

  section("6. what this run established, and what it did not");
  std::cout << "  established: one supply chain out of src.facility reaches sink.load, one return chain\n"
            << "    leads back, and the pair is reported as a single closed circuit in generation "
            << topology.generation().value() << ".\n";
  std::cout << "  established: supply edges taken alone are acyclic here (no reverse supply route, no\n"
            << "    re-entry in the upstream closure), and the validator rejects a document that closes a\n"
            << "    supply cycle with supply_cycle.\n";
  std::cout << "  NOT established by any answer above:\n";
  std::cout << "    - that either pump is running, or that the declared duty pumps are the selected ones;\n";
  std::cout << "    - that coolant is flowing around the circuit, or in which direction any flow is set;\n";
  std::cout << "    - that the circuit is balanced, purged, filled, vented or commissioned;\n";
  std::cout << "    - that the loop temperatures are met, or that the coils and strainers are clean;\n";
  std::cout << "    - that the primary and secondary loops may be operated together: no control,\n"
            << "      set point or changeover authority is modelled here.\n";
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