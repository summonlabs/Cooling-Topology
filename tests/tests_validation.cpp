// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace ct_test;
namespace ct = dccp::cooling_topology;

/// Primary error code of a rejected draft, or Ok when the draft is accepted.
ct::ErrorCode primary_code(const ct::TopologyDraft& draft) {
  const ct::ValidationReport report = ct::Topology::validate_draft(draft);
  const ct::ValidationIssue* issue = report.primary();
  return issue == nullptr ? ct::ErrorCode::Ok : issue->code;
}

std::string primary_key(const ct::TopologyDraft& draft) {
  const ct::ValidationReport report = ct::Topology::validate_draft(draft);
  const ct::ValidationIssue* issue = report.primary();
  if (issue == nullptr) {
    return "ok";
  }
  return std::string(ct::to_token(issue->severity)) + "|" + std::string(ct::error_code_name(issue->code)) + "|" +
         issue->subject + "|" + issue->message;
}

bool has_warning(const ct::ValidationReport& report, ct::ErrorCode code) {
  for (const ct::ValidationIssue& issue : report.issues) {
    if (issue.severity == ct::ValidationSeverity::Warning && issue.code == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

CT_TEST(validation_reference_facility_is_valid) {
  ct::TopologyDraft draft = reference_facility();
  const ct::ValidationReport report = ct::Topology::validate_draft(draft);
  for (const ct::ValidationIssue& issue : report.issues) {
    report_note(std::string(ct::error_code_name(issue.code)) + " " + issue.subject + ": " + issue.message);
  }
  CT_CHECK(report.valid());
  CT_CHECK_EQ(report.error_count(), std::size_t{0});

  ct::Topology topology = build(draft);
  CT_CHECK_EQ(topology.node_count(), draft.nodes.size());
  CT_CHECK_EQ(topology.edge_count(), draft.edges.size());
  CT_CHECK_EQ(topology.group_count(), std::size_t{3});
  CT_CHECK(topology.digest().is_zero() == false);
}

CT_TEST(validation_reference_facility_has_no_warnings) {
  // The fixture is deliberately complete: every sink has a feed, the zone has a
  // serving element and no element is isolated.
  const ct::ValidationReport report = ct::Topology::validate_draft(reference_facility());
  for (const ct::ValidationIssue& issue : report.issues) {
    report_note(std::string(ct::to_token(issue.severity)) + " " + std::string(ct::error_code_name(issue.code)) +
                " " + issue.subject);
  }
  CT_CHECK_EQ(report.warning_count(), std::size_t{0});
}

CT_TEST(validation_single_path_fixture_reports_zone_service_absent_as_warning) {
  ct::TopologyDraft draft = single_path_facility();
  const ct::ValidationReport report = ct::Topology::validate_draft(draft);
  CT_CHECK(report.valid());
  CT_CHECK(has_warning(report, ct::ErrorCode::ZoneServiceAbsent));
  // A warning never gates publication.
  CT_CHECK(try_build(draft).has_value());
}

CT_TEST(validation_identity_stage_rejections) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(draft.nodes.front());
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::DuplicateIdentifier);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(draft.edges.front());
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::DuplicateIdentifier);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Alias alias;
    alias.id = ct::AliasId::parse("loop:l1").value();
    alias.target = nid("plant:p1");
    draft.aliases.push_back(alias);
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::IdentityConflict);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Alias first;
    first.id = ct::AliasId::parse("alias:one").value();
    first.target = nid("plant:p1");
    // An alias target is a NodeId by type, so an alias can only name another
    // alias by *spelling* an alias identity; that is exactly the chain the
    // validator must refuse.
    ct::Alias second;
    second.id = ct::AliasId::parse("alias:two").value();
    second.target = nid("alias:one");
    draft.aliases = {first, second};
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::AliasCycle);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Alias alias;
    alias.id = ct::AliasId::parse("alias:missing").value();
    alias.target = nid("plant:absent");
    draft.aliases = {alias};
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::AliasTargetMissing);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Alias duplicate_a;
    duplicate_a.id = ct::AliasId::parse("alias:dup").value();
    duplicate_a.target = nid("plant:p1");
    ct::Alias duplicate_b;
    duplicate_b.id = ct::AliasId::parse("alias:dup").value();
    duplicate_b.target = nid("loop:l1");
    draft.aliases = {duplicate_a, duplicate_b};
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::DuplicateIdentifier);
  }
}

CT_TEST(validation_endpoint_stage_rejections) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:missing", ct::EdgeKind::Supplies, "plant:absent", ct::PortRole::SupplyOut,
                               "loop:l1", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::EndpointMissing);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:self", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "loop:l1",
                               ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::SelfEdge);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:dup", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut, "loop:l1",
                               ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::DuplicateEdge);
  }
  {
    // An exactly reversed contains pair is a distinct edge, so it is not a
    // duplicate: the role stage reports that the pair is not a legal enclosure.
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:rev", ct::EdgeKind::Contains, "branch:br1", ct::PortRole::Contained,
                               "manifold:mf1", ct::PortRole::Container));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ContainmentKindInvalid);
  }
}

CT_TEST(validation_role_stage_rejections) {
  const auto rejected = [](ct::TopologyDraft draft, std::string_view id, ct::EdgeKind kind, std::string_view from,
                           ct::PortRole from_port, std::string_view to, ct::PortRole to_port) {
    draft.edges.push_back(edge(id, kind, from, from_port, to, to_port));
    return primary_code(draft);
  };
  // A cooling sink can never deliver cooled medium, so the kind relation is
  // refused before port legality is even considered.
  CT_CHECK_EQ(rejected(single_path_facility(), "e:bad-port", ct::EdgeKind::Supplies, "sink:r1",
                       ct::PortRole::SupplyOut, "cdu:c1", ct::PortRole::ReturnIn),
              ct::ErrorCode::InvalidEdgeKindPair);
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(pump_node("pump:px"));
    draft.edges.push_back(edge("e:pump-host", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut,
                               "pump:px", ct::PortRole::Terminal));
    // A pump can never deliver cooled medium.
    CT_CHECK_EQ(rejected(draft, "e:pump-port", ct::EdgeKind::Supplies, "pump:px", ct::PortRole::SupplyOut,
                         "loop:l1", ct::PortRole::SourceIn),
                ct::ErrorCode::InvalidEdgeKindPair);
  }
}

CT_TEST(validation_role_stage_requires_the_documented_endpoint_roles) {
  auto base = [] {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(pump_node("pump:px"));
    draft.edges.push_back(edge("e:pump-host", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut, "pump:px",
                               ct::PortRole::Terminal));
    draft.nodes.push_back(crac_node("crac:dx1"));
    draft.edges.push_back(edge("e:crac-dep", ct::EdgeKind::DependsOn, "crac:dx1", ct::PortRole::Terminal,
                               "zone:z1", ct::PortRole::Terminal));
    return draft;
  };
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:wrong-side", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SourceIn,
                               "loop:l1", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::InvalidEdgeEndpointPair);
  }
  {
    // Every kind that can receive a supplies edge does so through source_in; a
    // cooling rack air conditioner has no return_in port, so naming one is a
    // port error rather than a pairing error.
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:bad-target", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut,
                               "crac:dx1", ct::PortRole::ReturnIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::InvalidPortForKind);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:zone-delivers", ct::EdgeKind::Supplies, "zone:z1", ct::PortRole::SupplyOut,
                               "crac:dx1", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::InvalidEdgeKindPair);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:source-consumes", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut,
                               "source:fw", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::SourceTerminalInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:plant-serves", ct::EdgeKind::Serves, "plant:p1", ct::PortRole::Server,
                               "zone:z1", ct::PortRole::Served));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::PlantBoundaryInvalid);
  }
  {
    // A cooling distribution unit is not a pump host.
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:pump-misuse", ct::EdgeKind::Pumps, "cdu:c1", ct::PortRole::SupplyOut,
                               "pump:px", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::PumpAttachmentInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:pump-to-nonpump", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut,
                               "manifold:mf1", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::PumpAttachmentInvalid);
  }
  {
    // A pump installation must name a header side, not the generic terminal.
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:pump-bad-side", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::Terminal,
                               "pump:px", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::PumpAttachmentInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:returns-bad-source", ct::EdgeKind::Returns, "loop:l1", ct::PortRole::HeatOut,
                               "zone:z1", ct::PortRole::ReturnIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ReturnTargetInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:returns-bad-target", ct::EdgeKind::Returns, "loop:l1", ct::PortRole::HeatOut,
                               "sink:r1", ct::PortRole::ReturnIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ReturnTargetInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:contains-bad", ct::EdgeKind::Contains, "loop:l1", ct::PortRole::Container,
                               "zone:z1", ct::PortRole::Contained));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ContainmentKindInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:dep-bad-port", ct::EdgeKind::DependsOn, "crac:dx1", ct::PortRole::SourceIn,
                               "zone:z1", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::InvalidEdgeEndpointPair);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:serves-from-zone", ct::EdgeKind::Serves, "zone:z1", ct::PortRole::Server,
                               "sink:r1", ct::PortRole::Served));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ZoneServiceInvalid);
  }
  {
    ct::TopologyDraft draft = base();
    draft.edges.push_back(edge("e:serves-from-source", ct::EdgeKind::Serves, "source:fw", ct::PortRole::Server,
                               "zone:z1", ct::PortRole::Served));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ZoneServiceInvalid);
  }
}

CT_TEST(validation_circuit_stage_rejects_directed_supply_and_return_cycles) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:cycle", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut,
                               "plant:p1", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::SupplyCycle);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:rcycle", ct::EdgeKind::Returns, "plant:p1", ct::PortRole::HeatOut, "loop:l1",
                               ct::PortRole::ReturnIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ReturnCycle);
  }
  {
    // Supply edges and return edges form the physical circuit together: their
    // union is a cycle and that is exactly what a working loop looks like. The
    // closed fixture has a return path all the way back to the source.
    const ct::Topology topology = build(reference_facility());
    const std::vector<ct::EdgeId> supply_edges = topology.edges_of_kind(ct::EdgeKind::Supplies);
    const std::vector<ct::EdgeId> return_edges = topology.edges_of_kind(ct::EdgeKind::Returns);
    CT_CHECK(supply_edges.empty() == false);
    CT_CHECK(return_edges.empty() == false);
    auto circuit = ct::possible_circuits(topology, nid("source:facility-water"), nid("sink:rack-a1"));
    CT_REQUIRE(circuit.has_value());
    CT_CHECK_EQ(circuit->claim, ct::ClaimClass::StructurallyPossible);
    CT_REQUIRE(circuit->circuits.empty() == false);
    const ct::CoolingCircuit& found = circuit->circuits.front();
    CT_CHECK_EQ(found.supply_nodes.front().str(), std::string("source:facility-water"));
    CT_CHECK_EQ(found.supply_nodes.back().str(), std::string("sink:rack-a1"));
    CT_CHECK_EQ(found.return_nodes.front().str(), std::string("sink:rack-a1"));
    CT_CHECK_EQ(found.return_nodes.back().str(), std::string("source:facility-water"));
    CT_CHECK_EQ(found.supply_edges.size() + 1, found.supply_nodes.size());
    CT_CHECK_EQ(found.return_edges.size() + 1, found.return_nodes.size());
  }
  {
    // A supply path with no matching return path is not a circuit: the one-way
    // fixture has no return edge into the source, so the claim is impossible
    // rather than merely unreported.
    const ct::Topology topology = build(single_path_facility());
    auto circuit = ct::possible_circuits(topology, nid("source:fw"), nid("sink:r1"));
    CT_REQUIRE(circuit.has_value());
    CT_CHECK_EQ(circuit->claim, ct::ClaimClass::StructurallyImpossible);
    CT_CHECK(circuit->circuits.empty());
  }
}

CT_TEST(validation_medium_and_temperature_rules) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(loop_node("loop:cond", ct::LoopKind::CondenserWater, ct::CoolingMedium::CondenserWater));
    draft.edges.push_back(edge("e:medium", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut,
                               "loop:cond", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::MediumMismatch);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    const auto supply = ct::MilliCelsius::create(9000).value();
    const auto limit = ct::MilliCelsius::create(7000).value();
    for (ct::Node& node : draft.nodes) {
      if (node.id == nid("plant:p1")) {
        std::get<ct::CoolingPlantAttributes>(node.attributes).design_supply_temperature = supply;
      }
      if (node.id == nid("loop:l1")) {
        std::get<ct::CoolingLoopAttributes>(node.attributes).max_acceptable_supply_temperature = limit;
      }
    }
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::TemperatureIncompatible);
  }
  {
    // Equal declared temperatures are compatible: the rule is "at or below".
    ct::TopologyDraft draft = single_path_facility();
    const auto value = ct::MilliCelsius::create(7000).value();
    for (ct::Node& node : draft.nodes) {
      if (node.id == nid("plant:p1")) {
        std::get<ct::CoolingPlantAttributes>(node.attributes).design_supply_temperature = value;
      }
      if (node.id == nid("loop:l1")) {
        std::get<ct::CoolingLoopAttributes>(node.attributes).max_acceptable_supply_temperature = value;
      }
    }
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::Ok);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Node primary = loop_node("loop:pri", ct::LoopKind::Primary);
    ct::Node secondary = loop_node("loop:sec", ct::LoopKind::Secondary);
    draft.nodes.push_back(primary);
    draft.nodes.push_back(secondary);
    draft.edges.push_back(edge("e:rev-loop", ct::EdgeKind::Supplies, "loop:sec", ct::PortRole::SupplyOut,
                               "loop:pri", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::LoopKindMismatch);
  }
  {
    // The forward direction is the documented arrangement and is accepted.
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(loop_node("loop:pri", ct::LoopKind::Primary));
    draft.nodes.push_back(loop_node("loop:sec", ct::LoopKind::Secondary));
    draft.edges.push_back(edge("e:fwd-loop", ct::EdgeKind::Supplies, "loop:pri", ct::PortRole::SupplyOut,
                               "loop:sec", ct::PortRole::SourceIn));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::Ok);
  }
}

CT_TEST(validation_containment_rules) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:second-parent", ct::EdgeKind::Contains, "loop:l1", ct::PortRole::Container,
                               "branch:br1", ct::PortRole::Contained));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::AmbiguousParentage);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:ccycle", ct::EdgeKind::Contains, "branch:br1", ct::PortRole::Container,
                               "manifold:mf1", ct::PortRole::Contained));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ContainmentKindInvalid);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(branch_node("branch:orphan"));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::MissingContainer);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(sink_node("sink:orphan", "rack-orphan"));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::MissingContainer);
  }
}

CT_TEST(validation_attachment_rules) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(pump_node("pump:lonely"));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::PumpAttachmentInvalid);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(pump_node("pump:double"));
    draft.edges.push_back(edge("e:double-a", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut,
                               "pump:double", ct::PortRole::Terminal));
    draft.edges.push_back(edge("e:double-b", ct::EdgeKind::Pumps, "manifold:mf1", ct::PortRole::SupplyOut,
                               "pump:double", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::PumpAttachmentInvalid);
  }
}

CT_TEST(validation_service_edges_cannot_form_a_cycle) {
  // The port matrix partitions the node kinds: a serving element has a server
  // port and no served port, and a served element has a served port and no
  // server port. A Serves cycle is therefore impossible by construction, which
  // is a stronger statement than rejecting one. The validator keeps a defensive
  // ServiceCycle check; this obligation proves the matrix that makes it
  // unreachable, and proves a service cycle attempt is still refused.
  for (std::size_t index = 0; index < 12; ++index) {
    const auto kind = static_cast<ct::NodeKind>(index);
    if (ct::is_server_kind(kind)) {
      CT_CHECK(ct::port_allowed_for_kind(kind, ct::PortRole::Server));
      CT_CHECK(ct::port_allowed_for_kind(kind, ct::PortRole::Served) == false);
    }
    if (ct::is_served_kind(kind)) {
      CT_CHECK(ct::port_allowed_for_kind(kind, ct::PortRole::Served));
      CT_CHECK(ct::port_allowed_for_kind(kind, ct::PortRole::Server) == false);
    }
  }
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(crah_node("crah:a"));
  draft.nodes.push_back(zone_node("zone:z2"));
  draft.edges.push_back(edge("e:sz1", ct::EdgeKind::Serves, "crah:a", ct::PortRole::Server, "zone:z1",
                             ct::PortRole::Served));
  draft.edges.push_back(edge("e:sz2", ct::EdgeKind::Serves, "crah:a", ct::PortRole::Server, "zone:z2",
                             ct::PortRole::Served));
  CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::Ok);
  draft.edges.push_back(edge("e:zcycle", ct::EdgeKind::Serves, "zone:z2", ct::PortRole::Server, "zone:z1",
                             ct::PortRole::Served));
  CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::ZoneServiceInvalid);
}

CT_TEST(validation_dependency_cycles_are_rejected) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:d1", ct::EdgeKind::DependsOn, "cdu:c1", ct::PortRole::Terminal, "branch:br1",
                               ct::PortRole::Terminal));
    draft.edges.push_back(edge("e:d2", ct::EdgeKind::DependsOn, "branch:br1", ct::PortRole::Terminal, "cdu:c1",
                               ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::DependencyCycle);
  }
  {
    // A dependency that points back into the containment chain is impossible: a
    // contained element cannot be a structural dependency of its container.
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:dep-up", ct::EdgeKind::DependsOn, "branch:br1", ct::PortRole::Terminal,
                               "manifold:mf1", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::DependencyCycle);
  }
  {
    // The forward direction respects the nesting order and is accepted.
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:dep-down", ct::EdgeKind::DependsOn, "manifold:mf1", ct::PortRole::Terminal,
                               "branch:br1", ct::PortRole::Terminal));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::Ok);
  }
}

CT_TEST(validation_completeness_warnings) {
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(crac_node("crac:lonely"));
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_CHECK(report.valid());
    CT_CHECK(has_warning(report, ct::ErrorCode::IsolatedElement));
    CT_CHECK_EQ(report.error_count(), std::size_t{0});
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(sink_node("sink:unfed", "rack-unfed"));
    draft.edges.push_back(edge("e:unfed-in-zone", ct::EdgeKind::Contains, "zone:z1", ct::PortRole::Container,
                               "sink:unfed", ct::PortRole::Contained));
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_CHECK(report.valid());
    CT_CHECK(has_warning(report, ct::ErrorCode::SinkFeedAbsent));
  }
}

CT_TEST(validation_is_deterministic_under_input_permutation) {
  ct::TopologyDraft broken = reference_facility();
  // Two independent defects in different stages: the earlier stage must win.
  broken.edges.push_back(edge("e:acycle", ct::EdgeKind::Supplies, "loop:secondary", ct::PortRole::SupplyOut,
                              "loop:primary", ct::PortRole::SourceIn));
  broken.edges.push_back(edge("e:bself", ct::EdgeKind::Returns, "cdu:cdu-a1", ct::PortRole::HeatOut,
                              "cdu:cdu-a1", ct::PortRole::ReturnIn));
  const std::string expected = primary_key(broken);
  CT_CHECK(expected.rfind("error|", 0) == 0);

  Rng rng(case_seed("validation_is_deterministic_under_input_permutation"));
  for (int round = 0; round < 24; ++round) {
    ct::TopologyDraft shuffled = broken;
    for (std::size_t index = shuffled.nodes.size(); index > 1; --index) {
      const std::size_t other = rng.below(static_cast<std::uint32_t>(index));
      std::swap(shuffled.nodes[index - 1], shuffled.nodes[other]);
    }
    for (std::size_t index = shuffled.edges.size(); index > 1; --index) {
      const std::size_t other = rng.below(static_cast<std::uint32_t>(index));
      std::swap(shuffled.edges[index - 1], shuffled.edges[other]);
    }
    CT_CHECK_EQ(primary_key(shuffled), expected);
  }
}

CT_TEST(validation_issue_list_is_ordered_and_stable) {
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(branch_node("branch:orphan-a"));
  draft.nodes.push_back(sink_node("sink:orphan-b", "rack-orphan-b"));
  const ct::ValidationReport first = ct::Topology::validate_draft(draft);
  const ct::ValidationReport second = ct::Topology::validate_draft(draft);
  CT_REQUIRE(first.issues.size() == second.issues.size());
  for (std::size_t index = 0; index < first.issues.size(); ++index) {
    CT_CHECK_EQ(std::string(ct::error_code_name(first.issues[index].code)),
                std::string(ct::error_code_name(second.issues[index].code)));
    CT_CHECK_EQ(first.issues[index].subject, second.issues[index].subject);
  }
  // Errors always precede warnings, so the primary issue is an error.
  bool seen_warning = false;
  for (const ct::ValidationIssue& issue : first.issues) {
    if (issue.severity == ct::ValidationSeverity::Warning) {
      seen_warning = true;
    } else {
      CT_CHECK(seen_warning == false);
    }
  }
}

CT_TEST(validation_create_reports_the_same_primary_error_as_validate_draft) {
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(pump_node("pump:lonely"));
  const ct::ValidationReport report = ct::Topology::validate_draft(draft);
  CT_REQUIRE(report.primary() != nullptr);
  const auto created = try_build(draft);
  CT_REQUIRE(created.has_value() == false);
  CT_CHECK_EQ(created.error().code(), report.primary()->code);
  CT_CHECK_EQ(created.error().subject(), report.primary()->subject);
  CT_CHECK_EQ(created.error().category(), ct::ErrorCategory::Structure);
}

CT_TEST(validation_bounds_are_enforced_before_allocation) {
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Node wide = plant_node("plant:wide");
    for (std::size_t index = 0; index <= ct::limits::kMaxNodeReferences; ++index) {
      wide.references.push_back(extref("loc-" + std::to_string(index), ct::ExternalRefKind::Location));
    }
    draft.nodes.push_back(wide);
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::LimitExceeded);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Node hot = plant_node("plant:hot");
    std::get<ct::CoolingPlantAttributes>(hot.attributes).design_supply_temperature =
        ct::MilliCelsius(ct::limits::kMaxTemperatureMilliCelsius + 1);
    draft.nodes.push_back(hot);
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::QuantityOutOfRange);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::CoolingSinkAttributes sink;
    sink.kind = ct::SinkKind::RackLoad;
    sink.consumer = extref("rack-x", ct::ExternalRefKind::Rack);
    auto node_result = ct::Node::create(nid("sink:wrong-kind"), sink, "", {});
    CT_REQUIRE(node_result.has_value());
    draft.nodes.push_back(*node_result);
    draft.edges.push_back(edge("e:sink-wrong", ct::EdgeKind::Contains, "zone:z1", ct::PortRole::Container,
                               "sink:wrong-kind", ct::PortRole::Contained));
    CT_CHECK_EQ(primary_code(draft), ct::ErrorCode::InvalidArgument);
  }
}
