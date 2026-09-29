// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared fixtures for the suite. Everything here is synthetic: the topology it
// builds describes a small but realistic cooling plant so that structural rules
// are exercised on a graph a cooling engineer would recognise. It is not a model
// of any real facility and proves nothing about real hardware.

#ifndef COOLING_TOPOLOGY_TEST_SUPPORT_HPP
#define COOLING_TOPOLOGY_TEST_SUPPORT_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/diff.hpp"
#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/store.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "test_framework.hpp"

namespace ct_test {

namespace ct = dccp::cooling_topology;

inline ct::NodeId nid(std::string_view text) {
  auto parsed = ct::NodeId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::EdgeId eid(std::string_view text) {
  auto parsed = ct::EdgeId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::RedundancyGroupId gid(std::string_view text) {
  auto parsed = ct::RedundancyGroupId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::AliasId aid(std::string_view text) {
  auto parsed = ct::AliasId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::ChangeoverGroupId cid(std::string_view text) {
  auto parsed = ct::ChangeoverGroupId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::StoreId sid(std::string_view text) {
  auto parsed = ct::StoreId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::MutationId mid(std::string_view text) {
  auto parsed = ct::MutationId::parse(text);
  CT_REQUIRE(parsed.has_value());
  return *parsed;
}

inline ct::ExternalRef extref(std::string_view identity,
                              ct::ExternalRefKind kind = ct::ExternalRefKind::Facility,
                              std::uint64_t generation = 0) {
  auto created = ct::ExternalRef::create(kind, std::string(identity), ct::ExternalGeneration(generation));
  CT_REQUIRE(created.has_value());
  return *created;
}

inline ct::Endpoint endpoint(std::string_view node, ct::PortRole port) {
  ct::Endpoint value;
  value.node = nid(node);
  value.port = port;
  return value;
}

inline ct::Edge edge(std::string_view id, ct::EdgeKind kind, std::string_view from, ct::PortRole from_port,
                     std::string_view to, ct::PortRole to_port) {
  auto created = ct::Edge::create(eid(id), kind, endpoint(from, from_port), endpoint(to, to_port));
  CT_REQUIRE(created.has_value());
  return *created;
}

inline ct::Node node(std::string_view id, ct::NodeAttributes attributes, std::string name = {}) {
  auto created = ct::Node::create(nid(id), std::move(attributes), std::move(name), {});
  CT_REQUIRE(created.has_value());
  return *created;
}

inline ct::ExternalRef consumer(std::string_view identity) {
  return extref(identity, ct::ExternalRefKind::Consumer);
}

inline ct::Node source_node(std::string_view id, ct::SourceKind kind = ct::SourceKind::FacilityWater,
                            std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater,
                            std::optional<ct::MilliCelsius> design = std::nullopt) {
  ct::CoolingSourceAttributes attributes;
  attributes.kind = kind;
  attributes.medium = medium;
  attributes.design_supply_temperature = design;
  return node(id, attributes);
}

inline ct::Node plant_node(std::string_view id, std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater,
                           std::optional<ct::MilliCelsius> design = std::nullopt,
                           std::optional<ct::MilliCelsius> maximum = std::nullopt) {
  ct::CoolingPlantAttributes attributes;
  attributes.kind = ct::PlantKind::ChillerPlant;
  attributes.medium = medium;
  attributes.design_supply_temperature = design;
  attributes.max_acceptable_supply_temperature = maximum;
  return node(id, attributes);
}

inline ct::Node chiller_node(std::string_view id, std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater) {
  ct::ChillerAttributes attributes;
  attributes.kind = ct::ChillerKind::Centrifugal;
  attributes.medium = medium;
  return node(id, attributes);
}

inline ct::Node pump_node(std::string_view id, ct::PumpRole role = ct::PumpRole::Duty) {
  ct::PumpAttributes attributes;
  attributes.kind = ct::PumpKind::Centrifugal;
  attributes.role = role;
  return node(id, attributes);
}

inline ct::Node loop_node(std::string_view id, ct::LoopKind kind = ct::LoopKind::Secondary,
                          std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater,
                          std::optional<ct::MilliCelsius> design = std::nullopt,
                          std::optional<ct::MilliCelsius> maximum = std::nullopt) {
  ct::CoolingLoopAttributes attributes;
  attributes.kind = kind;
  attributes.medium = medium;
  attributes.design_supply_temperature = design;
  attributes.max_acceptable_supply_temperature = maximum;
  return node(id, attributes);
}

inline ct::Node manifold_node(std::string_view id,
                              std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater) {
  ct::ManifoldAttributes attributes;
  attributes.kind = ct::ManifoldKind::Combined;
  attributes.medium = medium;
  return node(id, attributes);
}

inline ct::Node branch_node(std::string_view id,
                            std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater) {
  ct::BranchAttributes attributes;
  attributes.kind = ct::BranchKind::RackBranch;
  attributes.medium = medium;
  return node(id, attributes);
}

inline ct::Node cdu_node(std::string_view id, std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater,
                         std::optional<ct::MilliCelsius> maximum = std::nullopt) {
  ct::CduAttributes attributes;
  attributes.kind = ct::CduKind::RackCdu;
  attributes.medium = medium;
  attributes.max_acceptable_supply_temperature = maximum;
  return node(id, attributes);
}

inline ct::Node crah_node(std::string_view id,
                          std::optional<ct::CoolingMedium> medium = ct::CoolingMedium::ChilledWater) {
  ct::CrahAttributes attributes;
  attributes.placement = ct::AirHandlerPlacement::InRow;
  attributes.medium = medium;
  return node(id, attributes);
}

inline ct::Node crac_node(std::string_view id, std::optional<ct::CoolingMedium> medium = std::nullopt) {
  ct::CracAttributes attributes;
  attributes.placement = ct::AirHandlerPlacement::Perimeter;
  attributes.medium = medium;
  return node(id, attributes);
}

inline ct::Node zone_node(std::string_view id, ct::ZoneClass zone_class = ct::ZoneClass::Room) {
  ct::ThermalZoneAttributes attributes;
  attributes.zone_class = zone_class;
  return node(id, attributes);
}

inline ct::Node sink_node(std::string_view id, std::string_view consumer_identity,
                          std::optional<ct::MilliCelsius> maximum = std::nullopt) {
  ct::CoolingSinkAttributes attributes;
  attributes.kind = ct::SinkKind::RackLoad;
  attributes.consumer = consumer(consumer_identity);
  attributes.max_acceptable_supply_temperature = maximum;
  return node(id, attributes);
}

inline ct::TopologyDraft base_draft(std::string_view facility = "dc-1") {
  ct::TopologyDraft draft;
  draft.facility = extref(facility, ct::ExternalRefKind::Facility, 7);
  auto provenance = ct::Provenance::create("dccp-cooling-topology/1.0.0", ct::ProvenanceOrigin::Authored,
                                           "synthetic fixture", std::nullopt, ct::AuthorityEpoch(), {});
  CT_REQUIRE(provenance.has_value());
  draft.provenance = *provenance;
  return draft;
}

inline ct::Topology build(const ct::TopologyDraft& draft) {
  auto topology = ct::Topology::create_first(draft);
  CT_REQUIRE(topology.has_value());
  return *topology;
}

inline ct::Result<ct::Topology> try_build(const ct::TopologyDraft& draft) {
  return ct::Topology::create_first(draft);
}

/// A small but realistic synthetic facility:
///
///   facility-water source -> chiller plant
///   plant -> primary loop -> secondary loop -> two manifolds
///   each manifold -> branch -> CDU -> rack sink
///   a cold-aisle zone contains the sinks; an in-row CRAH serves the zone from
///   the secondary loop; duty and standby pumps are installed on the secondary
///   loop on the supply side.
///
/// Redundancy: a Plant group over two chiller plants, a Pump group over the duty
/// and standby pumps, and a Distribution group over the two manifolds.
inline ct::TopologyDraft reference_facility() {
  ct::TopologyDraft draft = base_draft();
  draft.nodes = {
      source_node("source:facility-water"),
      plant_node("plant:chp-a", ct::CoolingMedium::ChilledWater),
      plant_node("plant:chp-b", ct::CoolingMedium::ChilledWater),
      chiller_node("chiller:ch-1"),
      loop_node("loop:primary", ct::LoopKind::Primary),
      loop_node("loop:secondary", ct::LoopKind::Secondary),
      pump_node("pump:p-1", ct::PumpRole::Duty),
      pump_node("pump:p-2", ct::PumpRole::Standby),
      manifold_node("manifold:m-a"),
      manifold_node("manifold:m-b"),
      branch_node("branch:b-a1"),
      branch_node("branch:b-b1"),
      cdu_node("cdu:cdu-a1"),
      cdu_node("cdu:cdu-b1"),
      sink_node("sink:rack-a1", "rack-a1"),
      sink_node("sink:rack-b1", "rack-b1"),
      crah_node("crah:ir-1"),
      zone_node("zone:cold-aisle-1", ct::ZoneClass::ColdAisle),
  };
  draft.edges = {
      edge("e:src-plant-a", ct::EdgeKind::Supplies, "source:facility-water", ct::PortRole::SupplyOut,
           "plant:chp-a", ct::PortRole::SourceIn),
      edge("e:src-plant-b", ct::EdgeKind::Supplies, "source:facility-water", ct::PortRole::SupplyOut,
           "plant:chp-b", ct::PortRole::SourceIn),
      edge("e:plant-primary", ct::EdgeKind::Supplies, "plant:chp-a", ct::PortRole::SupplyOut,
           "loop:primary", ct::PortRole::SourceIn),
      edge("e:primary-secondary", ct::EdgeKind::Supplies, "loop:primary", ct::PortRole::SupplyOut,
           "loop:secondary", ct::PortRole::SourceIn),
      edge("e:secondary-manifold-a", ct::EdgeKind::Supplies, "loop:secondary", ct::PortRole::SupplyOut,
           "manifold:m-a", ct::PortRole::SourceIn),
      edge("e:secondary-manifold-b", ct::EdgeKind::Supplies, "loop:secondary", ct::PortRole::SupplyOut,
           "manifold:m-b", ct::PortRole::SourceIn),
      edge("e:manifold-a-branch", ct::EdgeKind::Supplies, "manifold:m-a", ct::PortRole::SupplyOut,
           "branch:b-a1", ct::PortRole::SourceIn),
      edge("e:manifold-b-branch", ct::EdgeKind::Supplies, "manifold:m-b", ct::PortRole::SupplyOut,
           "branch:b-b1", ct::PortRole::SourceIn),
      edge("e:branch-a-cdu", ct::EdgeKind::Supplies, "branch:b-a1", ct::PortRole::SupplyOut, "cdu:cdu-a1",
           ct::PortRole::SourceIn),
      edge("e:branch-b-cdu", ct::EdgeKind::Supplies, "branch:b-b1", ct::PortRole::SupplyOut, "cdu:cdu-b1",
           ct::PortRole::SourceIn),
      edge("e:cdu-a-sink", ct::EdgeKind::Supplies, "cdu:cdu-a1", ct::PortRole::SupplyOut, "sink:rack-a1",
           ct::PortRole::SourceIn),
      edge("e:cdu-b-sink", ct::EdgeKind::Supplies, "cdu:cdu-b1", ct::PortRole::SupplyOut, "sink:rack-b1",
           ct::PortRole::SourceIn),
      edge("e:sink-a-return", ct::EdgeKind::Returns, "sink:rack-a1", ct::PortRole::HeatOut, "cdu:cdu-a1",
           ct::PortRole::ReturnIn),
      edge("e:sink-b-return", ct::EdgeKind::Returns, "sink:rack-b1", ct::PortRole::HeatOut, "cdu:cdu-b1",
           ct::PortRole::ReturnIn),
      edge("e:cdu-a-return", ct::EdgeKind::Returns, "cdu:cdu-a1", ct::PortRole::HeatOut, "branch:b-a1",
           ct::PortRole::ReturnIn),
      edge("e:cdu-b-return", ct::EdgeKind::Returns, "cdu:cdu-b1", ct::PortRole::HeatOut, "branch:b-b1",
           ct::PortRole::ReturnIn),
      edge("e:branch-a-return", ct::EdgeKind::Returns, "branch:b-a1", ct::PortRole::HeatOut, "manifold:m-a",
           ct::PortRole::ReturnIn),
      edge("e:branch-b-return", ct::EdgeKind::Returns, "branch:b-b1", ct::PortRole::HeatOut, "manifold:m-b",
           ct::PortRole::ReturnIn),
      edge("e:manifold-a-return", ct::EdgeKind::Returns, "manifold:m-a", ct::PortRole::HeatOut,
           "loop:secondary", ct::PortRole::ReturnIn),
      edge("e:manifold-b-return", ct::EdgeKind::Returns, "manifold:m-b", ct::PortRole::HeatOut,
           "loop:secondary", ct::PortRole::ReturnIn),
      edge("e:secondary-primary-return", ct::EdgeKind::Returns, "loop:secondary", ct::PortRole::HeatOut,
           "loop:primary", ct::PortRole::ReturnIn),
      edge("e:primary-plant-return", ct::EdgeKind::Returns, "loop:primary", ct::PortRole::HeatOut,
           "plant:chp-a", ct::PortRole::ReturnIn),
      edge("e:plant-source-return", ct::EdgeKind::Returns, "plant:chp-a", ct::PortRole::HeatOut,
           "source:facility-water", ct::PortRole::ReturnIn),
      edge("e:crah-secondary", ct::EdgeKind::Supplies, "loop:secondary", ct::PortRole::SupplyOut, "crah:ir-1",
           ct::PortRole::SourceIn),
      edge("e:crah-zone", ct::EdgeKind::Serves, "crah:ir-1", ct::PortRole::Server, "zone:cold-aisle-1",
           ct::PortRole::Served),
      edge("e:pump-1-install", ct::EdgeKind::Pumps, "loop:secondary", ct::PortRole::SupplyOut, "pump:p-1",
           ct::PortRole::Terminal),
      edge("e:pump-2-install", ct::EdgeKind::Pumps, "loop:secondary", ct::PortRole::SupplyOut, "pump:p-2",
           ct::PortRole::Terminal),
      edge("e:contains-plant-chiller", ct::EdgeKind::Contains, "plant:chp-a", ct::PortRole::Container,
           "chiller:ch-1", ct::PortRole::Contained),
      edge("e:contains-manifold-a", ct::EdgeKind::Contains, "loop:secondary", ct::PortRole::Container,
           "manifold:m-a", ct::PortRole::Contained),
      edge("e:contains-manifold-b", ct::EdgeKind::Contains, "loop:secondary", ct::PortRole::Container,
           "manifold:m-b", ct::PortRole::Contained),
      edge("e:contains-branch-a", ct::EdgeKind::Contains, "manifold:m-a", ct::PortRole::Container,
           "branch:b-a1", ct::PortRole::Contained),
      edge("e:contains-branch-b", ct::EdgeKind::Contains, "manifold:m-b", ct::PortRole::Container,
           "branch:b-b1", ct::PortRole::Contained),
      edge("e:contains-sink-a", ct::EdgeKind::Contains, "zone:cold-aisle-1", ct::PortRole::Container,
           "sink:rack-a1", ct::PortRole::Contained),
      edge("e:contains-sink-b", ct::EdgeKind::Contains, "zone:cold-aisle-1", ct::PortRole::Container,
           "sink:rack-b1", ct::PortRole::Contained),
      edge("e:contains-crah", ct::EdgeKind::Contains, "zone:cold-aisle-1", ct::PortRole::Container,
           "crah:ir-1", ct::PortRole::Contained),
  };
  auto plant_group = ct::RedundancyGroup::create(
      gid("group:plants"), ct::RedundancyScheme::NPlusOne, ct::RedundancyScope::Plant, "plants", "two plants",
      {ct::RedundancyMember{nid("plant:chp-a"), "plant:chp-a", std::nullopt},
       ct::RedundancyMember{nid("plant:chp-b"), "plant:chp-b", std::nullopt}},
      false, false);
  CT_REQUIRE(plant_group.has_value());
  auto pump_group = ct::RedundancyGroup::create(
      gid("group:pumps"), ct::RedundancyScheme::NPlusOne, ct::RedundancyScope::Pump, "pumps", "duty plus standby",
      {ct::RedundancyMember{nid("pump:p-1"), "pump:p-1", std::nullopt},
       ct::RedundancyMember{nid("pump:p-2"), "pump:p-2", std::nullopt}},
      false, false);
  CT_REQUIRE(pump_group.has_value());
  auto manifold_group = ct::RedundancyGroup::create(
      gid("group:manifolds"), ct::RedundancyScheme::TwoN, ct::RedundancyScope::Distribution, "manifolds",
      "independent manifolds",
      {ct::RedundancyMember{nid("manifold:m-a"), "manifold:m-a", std::nullopt},
       ct::RedundancyMember{nid("manifold:m-b"), "manifold:m-b", std::nullopt}},
      false, false);
  CT_REQUIRE(manifold_group.has_value());
  draft.groups = {*plant_group, *pump_group, *manifold_group};
  return draft;
}

/// A single-path facility: one source, one plant, one loop, one branch, one CDU
/// and one sink. Used for single-point-of-failure and impact proofs.
inline ct::TopologyDraft single_path_facility() {
  ct::TopologyDraft draft = base_draft("dc-single");
  draft.nodes = {
      source_node("source:fw"),
      plant_node("plant:p1"),
      loop_node("loop:l1"),
      manifold_node("manifold:mf1"),
      branch_node("branch:br1"),
      cdu_node("cdu:c1"),
      sink_node("sink:r1", "rack-r1"),
      zone_node("zone:z1"),
  };
  draft.edges = {
      edge("e1", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut, "plant:p1",
           ct::PortRole::SourceIn),
      edge("e2", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut, "loop:l1",
           ct::PortRole::SourceIn),
      edge("e3", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "manifold:mf1",
           ct::PortRole::SourceIn),
      edge("e4", ct::EdgeKind::Supplies, "manifold:mf1", ct::PortRole::SupplyOut, "branch:br1",
           ct::PortRole::SourceIn),
      edge("e5", ct::EdgeKind::Supplies, "branch:br1", ct::PortRole::SupplyOut, "cdu:c1",
           ct::PortRole::SourceIn),
      edge("e6", ct::EdgeKind::Supplies, "cdu:c1", ct::PortRole::SupplyOut, "sink:r1", ct::PortRole::SourceIn),
      edge("e7", ct::EdgeKind::Returns, "sink:r1", ct::PortRole::HeatOut, "cdu:c1", ct::PortRole::ReturnIn),
      edge("e8", ct::EdgeKind::Returns, "cdu:c1", ct::PortRole::HeatOut, "loop:l1", ct::PortRole::ReturnIn),
      edge("e9", ct::EdgeKind::Returns, "loop:l1", ct::PortRole::HeatOut, "plant:p1", ct::PortRole::ReturnIn),
      edge("e10", ct::EdgeKind::Contains, "manifold:mf1", ct::PortRole::Container, "branch:br1",
           ct::PortRole::Contained),
      edge("e11", ct::EdgeKind::Contains, "zone:z1", ct::PortRole::Container, "sink:r1",
           ct::PortRole::Contained),
  };
  return draft;
}

}  // namespace ct_test

#endif  // COOLING_TOPOLOGY_TEST_SUPPORT_HPP
