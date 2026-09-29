// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Proof obligations for the structural vocabulary: every token table
// round-trips, every documented kind/port/scope/containment matrix holds in
// BOTH directions (with the complement asserted false), construction rejects
// exactly the documented shapes, and the claim boundary is total - every
// excluded claim is NotOwned.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;

void expect_code(const ct::Error& error, ct::ErrorCode expected, const std::string& what) {
  CT_CHECK_MSG(error.code() == expected,
               what + ": expected " + std::string(ct::error_code_name(expected)) + ", observed " +
                   std::string(ct::error_code_name(error.code())) + " [" + error.to_string() + "]");
}

constexpr ct::NodeKind kAllNodeKinds[] = {
    ct::NodeKind::CoolingPlant, ct::NodeKind::Chiller,     ct::NodeKind::Pump,        ct::NodeKind::CoolingLoop,
    ct::NodeKind::Cdu,          ct::NodeKind::Crah,        ct::NodeKind::Crac,        ct::NodeKind::Manifold,
    ct::NodeKind::Branch,       ct::NodeKind::ThermalZone, ct::NodeKind::CoolingSink, ct::NodeKind::CoolingSource,
};

constexpr ct::PortRole kAllPortRoles[] = {
    ct::PortRole::SupplyOut, ct::PortRole::SourceIn,  ct::PortRole::HeatOut, ct::PortRole::ReturnIn,
    ct::PortRole::Terminal,  ct::PortRole::Container, ct::PortRole::Contained, ct::PortRole::Server,
    ct::PortRole::Served,
};

constexpr ct::RedundancyScope kAllScopes[] = {
    ct::RedundancyScope::Plant,        ct::RedundancyScope::Chiller,       ct::RedundancyScope::Pump,
    ct::RedundancyScope::CoolingLoop,  ct::RedundancyScope::Distribution,  ct::RedundancyScope::SourcePath,
    ct::RedundancyScope::AirHandling,
};

/// Round-trips every enumerator of one token table and requires the unknown and
/// empty tokens to be rejected with UnknownEnumToken.
template <class Enum, class Parse>
void check_enum_table(const std::vector<Enum>& values, Parse parse, const char* what) {
  std::vector<std::string> tokens;
  for (const Enum value : values) {
    const std::string token(ct::to_token(value));
    CT_CHECK_MSG(!token.empty() && token != "unknown", std::string(what) + ": value renders no token");
    for (const std::string& seen : tokens) {
      CT_CHECK_MSG(seen != token, std::string(what) + ": two enumerators share the token " + token);
    }
    tokens.push_back(token);
    auto parsed = parse(std::string_view(token));
    CT_CHECK_MSG(parsed.has_value(), std::string(what) + ": token rejected: " + token);
    if (parsed.has_value()) {
      CT_CHECK_MSG(*parsed == value, std::string(what) + ": round trip mismatch for " + token);
    }
  }
  const std::string unknown = "no-such-token-42";
  auto parsed_unknown = parse(std::string_view(unknown));
  CT_CHECK_MSG(!parsed_unknown.has_value(), std::string(what) + ": unknown token accepted");
  if (!parsed_unknown.has_value()) {
    expect_code(parsed_unknown.error(), ct::ErrorCode::UnknownEnumToken, std::string(what) + " unknown token");
  }
  auto parsed_empty = parse(std::string_view());
  CT_CHECK_MSG(!parsed_empty.has_value(), std::string(what) + ": empty token accepted");
  if (!parsed_empty.has_value()) {
    expect_code(parsed_empty.error(), ct::ErrorCode::UnknownEnumToken, std::string(what) + " empty token");
  }
  // A token is case-sensitive: an upper-cased token must not be accepted.
  if (!tokens.empty()) {
    std::string shouted = tokens.front();
    for (char& character : shouted) {
      if (character >= 'a' && character <= 'z') {
        character = static_cast<char>(character - 'a' + 'A');
      }
    }
    if (shouted != tokens.front()) {
      auto parsed_shouted = parse(std::string_view(shouted));
      CT_CHECK_MSG(!parsed_shouted.has_value(), std::string(what) + ": upper-cased token accepted");
    }
  }
}

// ---------------------------------------------------------------------------
// Documented matrices, written out independently of the implementation.
// ---------------------------------------------------------------------------

bool is_hydronic(ct::PortRole role) {
  return role == ct::PortRole::SupplyOut || role == ct::PortRole::SourceIn || role == ct::PortRole::HeatOut ||
         role == ct::PortRole::ReturnIn;
}

bool is_containment_port(ct::PortRole role) {
  return role == ct::PortRole::Container || role == ct::PortRole::Contained;
}

bool expected_port_allowed(ct::NodeKind kind, ct::PortRole role) {
  // Every element carries the generic terminal connection point: it is the
  // endpoint of a dependency edge and, for a pump host, the installation point.
  const bool terminal = role == ct::PortRole::Terminal;
  // A serving element carries a server port: the reference here must agree with
  // is_server_kind(), which classifies the loop, manifold, branch and CDU as
  // serving kinds alongside the CRAH and CRAC.
  const bool server = role == ct::PortRole::Server;
  switch (kind) {
    case ct::NodeKind::CoolingPlant:
    case ct::NodeKind::Chiller:
      return is_hydronic(role) || is_containment_port(role) || terminal;
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Cdu:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
      return is_hydronic(role) || is_containment_port(role) || terminal || server;
    case ct::NodeKind::Pump:
      return terminal || role == ct::PortRole::Contained;
    case ct::NodeKind::Crah:
    case ct::NodeKind::Crac:
      return role == ct::PortRole::SourceIn || role == ct::PortRole::HeatOut || role == ct::PortRole::Server ||
             role == ct::PortRole::Contained || terminal;
    case ct::NodeKind::ThermalZone:
      return role == ct::PortRole::Served || is_containment_port(role) || terminal;
    case ct::NodeKind::CoolingSink:
      return role == ct::PortRole::SourceIn || role == ct::PortRole::HeatOut || role == ct::PortRole::Served ||
             role == ct::PortRole::Contained || terminal;
    case ct::NodeKind::CoolingSource:
      return role == ct::PortRole::SupplyOut || role == ct::PortRole::ReturnIn || terminal;
  }
  return false;
}

bool expected_supplier(ct::NodeKind kind) {
  switch (kind) {
    case ct::NodeKind::CoolingSource:
    case ct::NodeKind::CoolingPlant:
    case ct::NodeKind::Chiller:
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
    case ct::NodeKind::Cdu:
      return true;
    default:
      return false;
  }
}

bool expected_consumer(ct::NodeKind kind) {
  switch (kind) {
    case ct::NodeKind::CoolingPlant:
    case ct::NodeKind::Chiller:
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
    case ct::NodeKind::Cdu:
    case ct::NodeKind::Crah:
    case ct::NodeKind::Crac:
    case ct::NodeKind::CoolingSink:
      return true;
    default:
      return false;
  }
}

bool expected_heat_rejector(ct::NodeKind kind) { return expected_consumer(kind); }

bool expected_return_target(ct::NodeKind kind) {
  switch (kind) {
    case ct::NodeKind::CoolingSource:
    case ct::NodeKind::CoolingPlant:
    case ct::NodeKind::Chiller:
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
    case ct::NodeKind::Cdu:
      return true;
    default:
      return false;
  }
}

bool expected_server(ct::NodeKind kind) {
  switch (kind) {
    case ct::NodeKind::Crah:
    case ct::NodeKind::Crac:
    case ct::NodeKind::Cdu:
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
      return true;
    default:
      return false;
  }
}

bool expected_served(ct::NodeKind kind) {
  return kind == ct::NodeKind::ThermalZone || kind == ct::NodeKind::CoolingSink;
}

bool expected_pump_host(ct::NodeKind kind) {
  return kind == ct::NodeKind::CoolingLoop || kind == ct::NodeKind::Manifold || kind == ct::NodeKind::Branch;
}

bool expected_containment_pair(ct::NodeKind container, ct::NodeKind contained) {
  switch (container) {
    case ct::NodeKind::CoolingPlant:
      return contained == ct::NodeKind::Chiller || contained == ct::NodeKind::Pump ||
             contained == ct::NodeKind::CoolingLoop || contained == ct::NodeKind::Manifold ||
             contained == ct::NodeKind::Cdu;
    case ct::NodeKind::CoolingLoop:
      return contained == ct::NodeKind::Pump || contained == ct::NodeKind::Manifold ||
             contained == ct::NodeKind::Branch || contained == ct::NodeKind::Chiller ||
             contained == ct::NodeKind::Cdu;
    case ct::NodeKind::Chiller:
      return contained == ct::NodeKind::Pump;
    case ct::NodeKind::Cdu:
      return contained == ct::NodeKind::Pump;
    case ct::NodeKind::Manifold:
      return contained == ct::NodeKind::Branch || contained == ct::NodeKind::Pump ||
             contained == ct::NodeKind::Cdu;
    case ct::NodeKind::Branch:
      return contained == ct::NodeKind::Pump || contained == ct::NodeKind::Cdu;
    case ct::NodeKind::ThermalZone:
      return contained == ct::NodeKind::CoolingSink || contained == ct::NodeKind::Crah ||
             contained == ct::NodeKind::Crac || contained == ct::NodeKind::Cdu;
    default:
      return false;
  }
}

bool expected_container_kind(ct::NodeKind kind) {
  switch (kind) {
    case ct::NodeKind::CoolingPlant:
    case ct::NodeKind::Chiller:
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Cdu:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
    case ct::NodeKind::ThermalZone:
      return true;
    default:
      return false;
  }
}

bool expected_scope_allows(ct::RedundancyScope scope, ct::NodeKind member) {
  switch (scope) {
    case ct::RedundancyScope::Plant:
      return member == ct::NodeKind::CoolingPlant;
    case ct::RedundancyScope::Chiller:
      return member == ct::NodeKind::Chiller;
    case ct::RedundancyScope::Pump:
      return member == ct::NodeKind::Pump;
    case ct::RedundancyScope::CoolingLoop:
      return member == ct::NodeKind::CoolingLoop;
    case ct::RedundancyScope::Distribution:
      return member == ct::NodeKind::Manifold || member == ct::NodeKind::Branch || member == ct::NodeKind::Cdu;
    case ct::RedundancyScope::SourcePath:
      return member == ct::NodeKind::CoolingSource;
    case ct::RedundancyScope::AirHandling:
      return member == ct::NodeKind::Crah || member == ct::NodeKind::Crac;
  }
  return false;
}

}  // namespace

CT_TEST(model_medium_and_kind_tokens_round_trip) {
  check_enum_table<ct::CoolingMedium>(
      {ct::CoolingMedium::ChilledWater, ct::CoolingMedium::CondenserWater, ct::CoolingMedium::Glycol,
       ct::CoolingMedium::ProcessWater, ct::CoolingMedium::Refrigerant, ct::CoolingMedium::FacilityWater,
       ct::CoolingMedium::Air},
      &ct::parse_cooling_medium, "cooling-medium");
  check_enum_table<ct::PlantKind>(
      {ct::PlantKind::ChillerPlant, ct::PlantKind::HeatRejectionPlant, ct::PlantKind::HybridPlant,
       ct::PlantKind::DistrictSupply},
      &ct::parse_plant_kind, "plant-kind");
  check_enum_table<ct::ChillerKind>(
      {ct::ChillerKind::Centrifugal, ct::ChillerKind::Screw, ct::ChillerKind::Scroll, ct::ChillerKind::Absorption,
       ct::ChillerKind::MagneticBearing},
      &ct::parse_chiller_kind, "chiller-kind");
  check_enum_table<ct::PumpKind>(
      {ct::PumpKind::Centrifugal, ct::PumpKind::VerticalTurbine, ct::PumpKind::PositiveDisplacement},
      &ct::parse_pump_kind, "pump-kind");
  check_enum_table<ct::PumpRole>({ct::PumpRole::Duty, ct::PumpRole::Standby, ct::PumpRole::Jockey},
                                 &ct::parse_pump_role, "pump-role");
  check_enum_table<ct::LoopKind>(
      {ct::LoopKind::Primary, ct::LoopKind::Secondary, ct::LoopKind::CondenserWater, ct::LoopKind::ChilledWater,
       ct::LoopKind::Glycol, ct::LoopKind::ProcessWater, ct::LoopKind::HeatRejection},
      &ct::parse_loop_kind, "loop-kind");
  check_enum_table<ct::CduKind>(
      {ct::CduKind::RackCdu, ct::CduKind::RowCdu, ct::CduKind::InRowCdu, ct::CduKind::InRackCdu},
      &ct::parse_cdu_kind, "cdu-kind");
}

CT_TEST(model_placement_and_class_tokens_round_trip) {
  check_enum_table<ct::AirHandlerPlacement>(
      {ct::AirHandlerPlacement::Perimeter, ct::AirHandlerPlacement::InRow, ct::AirHandlerPlacement::Overhead,
       ct::AirHandlerPlacement::RearDoor},
      &ct::parse_air_handler_placement, "air-handler-placement");
  check_enum_table<ct::ManifoldKind>({ct::ManifoldKind::Supply, ct::ManifoldKind::Return, ct::ManifoldKind::Combined},
                                     &ct::parse_manifold_kind, "manifold-kind");
  check_enum_table<ct::BranchKind>(
      {ct::BranchKind::RackBranch, ct::BranchKind::ZoneBranch, ct::BranchKind::EquipmentBranch},
      &ct::parse_branch_kind, "branch-kind");
  check_enum_table<ct::ZoneClass>(
      {ct::ZoneClass::ColdAisle, ct::ZoneClass::HotAisle, ct::ZoneClass::Room, ct::ZoneClass::Rack,
       ct::ZoneClass::Pod, ct::ZoneClass::Plenum},
      &ct::parse_zone_class, "zone-class");
  check_enum_table<ct::SinkKind>(
      {ct::SinkKind::RackLoad, ct::SinkKind::DeviceLoad, ct::SinkKind::FacilityLoad}, &ct::parse_sink_kind,
      "sink-kind");
  check_enum_table<ct::SourceKind>(
      {ct::SourceKind::FacilityWater, ct::SourceKind::AmbientAir, ct::SourceKind::Well,
       ct::SourceKind::DistrictCooling, ct::SourceKind::ThermalStore},
      &ct::parse_source_kind, "source-kind");
}

CT_TEST(model_redundancy_tokens_round_trip) {
  check_enum_table<ct::RedundancyScheme>(
      {ct::RedundancyScheme::N, ct::RedundancyScheme::NPlusOne, ct::RedundancyScheme::TwoN,
       ct::RedundancyScheme::TwoNPlusOne, ct::RedundancyScheme::DistributedRedundant,
       ct::RedundancyScheme::ConcurrentlyMaintainable},
      &ct::parse_redundancy_scheme, "redundancy-scheme");
  check_enum_table<ct::RedundancyScope>(
      {ct::RedundancyScope::Plant, ct::RedundancyScope::Chiller, ct::RedundancyScope::Pump,
       ct::RedundancyScope::CoolingLoop, ct::RedundancyScope::Distribution, ct::RedundancyScope::SourcePath,
       ct::RedundancyScope::AirHandling},
      &ct::parse_redundancy_scope, "redundancy-scope");
}

CT_TEST(model_evidence_tokens_round_trip) {
  check_enum_table<ct::ExternalRefKind>(
      {ct::ExternalRefKind::Facility, ct::ExternalRefKind::Rack, ct::ExternalRefKind::Asset,
       ct::ExternalRefKind::Location, ct::ExternalRefKind::FailureDomain, ct::ExternalRefKind::Consumer,
       ct::ExternalRefKind::Registry, ct::ExternalRefKind::Evidence},
      &ct::parse_external_ref_kind, "external-ref-kind");
  check_enum_table<ct::EvidenceKind>(
      {ct::EvidenceKind::OperatingState, ct::EvidenceKind::FlowObservation, ct::EvidenceKind::CapacityStatement,
       ct::EvidenceKind::SourceEligibility, ct::EvidenceKind::ThermalSafetyStatement,
       ct::EvidenceKind::ActuationCommand, ct::EvidenceKind::FailoverDecision,
       ct::EvidenceKind::AirflowPolicyStatement, ct::EvidenceKind::LiquidCoolingPolicy,
       ct::EvidenceKind::PlacementDecision, ct::EvidenceKind::PowerStateObservation},
      &ct::parse_evidence_kind, "evidence-kind");
  check_enum_table<ct::ExcludedClaim>(
      {ct::ExcludedClaim::ComponentRunning, ct::ExcludedClaim::MediumFlowing,
       ct::ExcludedClaim::PathUsableCapacity, ct::ExcludedClaim::RedundantSourceEligible,
       ct::ExcludedClaim::ZoneThermallySafe, ct::ExcludedClaim::ActuationAuthority,
       ct::ExcludedClaim::FailoverAuthority, ct::ExcludedClaim::AirflowPolicy,
       ct::ExcludedClaim::LiquidCoolingControl, ct::ExcludedClaim::FacilityPlacement,
       ct::ExcludedClaim::PowerState},
      &ct::parse_excluded_claim, "excluded-claim");
}

CT_TEST(model_graph_tokens_round_trip) {
  check_enum_table<ct::NodeKind>(
      {ct::NodeKind::CoolingPlant, ct::NodeKind::Chiller, ct::NodeKind::Pump, ct::NodeKind::CoolingLoop,
       ct::NodeKind::Cdu, ct::NodeKind::Crah, ct::NodeKind::Crac, ct::NodeKind::Manifold, ct::NodeKind::Branch,
       ct::NodeKind::ThermalZone, ct::NodeKind::CoolingSink, ct::NodeKind::CoolingSource},
      &ct::parse_node_kind, "node-kind");
  check_enum_table<ct::PortRole>(
      {ct::PortRole::SupplyOut, ct::PortRole::SourceIn, ct::PortRole::HeatOut, ct::PortRole::ReturnIn,
       ct::PortRole::Terminal, ct::PortRole::Container, ct::PortRole::Contained, ct::PortRole::Server,
       ct::PortRole::Served},
      &ct::parse_port_role, "port-role");
  check_enum_table<ct::EdgeKind>(
      {ct::EdgeKind::Supplies, ct::EdgeKind::Returns, ct::EdgeKind::Serves, ct::EdgeKind::Pumps,
       ct::EdgeKind::Contains, ct::EdgeKind::DependsOn},
      &ct::parse_edge_kind, "edge-kind");
  check_enum_table<ct::ProvenanceOrigin>(
      {ct::ProvenanceOrigin::Authored, ct::ProvenanceOrigin::Imported, ct::ProvenanceOrigin::Reconciled,
       ct::ProvenanceOrigin::Recovered},
      &ct::parse_provenance_origin, "provenance-origin");
}

CT_TEST(model_port_allowed_for_kind_matrix_holds_in_both_directions) {
  std::size_t allowed = 0;
  std::size_t forbidden = 0;
  for (const ct::NodeKind kind : kAllNodeKinds) {
    for (const ct::PortRole role : kAllPortRoles) {
      const bool expected = expected_port_allowed(kind, role);
      const bool observed = ct::port_allowed_for_kind(kind, role);
      if (expected) {
        ++allowed;
      } else {
        ++forbidden;
      }
      CT_CHECK_MSG(observed == expected, std::string("port legality for kind ") +
                                             std::string(ct::to_token(kind)) + " role " +
                                             std::string(ct::to_token(role)) +
                                             " expected " + (expected ? "allowed" : "forbidden"));
    }
  }
  // Both directions are populated: the table is neither all-true nor all-false.
  // 12 node kinds x 9 port roles = 108 pairs; the four serving distribution
  // kinds carry a server port, which is what makes 70 allowed rather than 66.
  CT_CHECK_EQ(allowed + forbidden, static_cast<std::size_t>(108));
  CT_CHECK_EQ(allowed, static_cast<std::size_t>(70));
  CT_CHECK_EQ(forbidden, static_cast<std::size_t>(38));
}

CT_TEST(model_kind_classification_predicates_hold_in_both_directions) {
  std::size_t supplier_true = 0;
  std::size_t consumer_true = 0;
  std::size_t rejector_true = 0;
  std::size_t return_target_true = 0;
  std::size_t server_true = 0;
  std::size_t served_true = 0;
  std::size_t pump_host_true = 0;
  for (const ct::NodeKind kind : kAllNodeKinds) {
    const std::string name(ct::to_token(kind));
    CT_CHECK_MSG(ct::is_supplier_kind(kind) == expected_supplier(kind), name + " supplier classification");
    CT_CHECK_MSG(ct::is_consumer_kind(kind) == expected_consumer(kind), name + " consumer classification");
    CT_CHECK_MSG(ct::is_heat_rejector_kind(kind) == expected_heat_rejector(kind),
                 name + " heat-rejector classification");
    CT_CHECK_MSG(ct::is_return_target_kind(kind) == expected_return_target(kind),
                 name + " return-target classification");
    CT_CHECK_MSG(ct::is_server_kind(kind) == expected_server(kind), name + " server classification");
    CT_CHECK_MSG(ct::is_served_kind(kind) == expected_served(kind), name + " served classification");
    CT_CHECK_MSG(ct::is_pump_host_kind(kind) == expected_pump_host(kind), name + " pump-host classification");
    supplier_true += expected_supplier(kind) ? 1u : 0u;
    consumer_true += expected_consumer(kind) ? 1u : 0u;
    rejector_true += expected_heat_rejector(kind) ? 1u : 0u;
    return_target_true += expected_return_target(kind) ? 1u : 0u;
    server_true += expected_server(kind) ? 1u : 0u;
    served_true += expected_served(kind) ? 1u : 0u;
    pump_host_true += expected_pump_host(kind) ? 1u : 0u;
  }
  CT_CHECK_EQ(supplier_true, static_cast<std::size_t>(7));
  CT_CHECK_EQ(consumer_true, static_cast<std::size_t>(9));
  CT_CHECK_EQ(rejector_true, static_cast<std::size_t>(9));
  CT_CHECK_EQ(return_target_true, static_cast<std::size_t>(7));
  CT_CHECK_EQ(server_true, static_cast<std::size_t>(6));
  CT_CHECK_EQ(served_true, static_cast<std::size_t>(2));
  CT_CHECK_EQ(pump_host_true, static_cast<std::size_t>(3));
  // A supplier that cannot receive and a receiver that cannot supply.
  CT_CHECK(ct::is_supplier_kind(ct::NodeKind::CoolingSource));
  CT_CHECK(!ct::is_consumer_kind(ct::NodeKind::CoolingSource));
  CT_CHECK(!ct::is_supplier_kind(ct::NodeKind::CoolingSink));
  CT_CHECK(ct::is_consumer_kind(ct::NodeKind::CoolingSink));
  CT_CHECK(!ct::is_heat_rejector_kind(ct::NodeKind::CoolingSource));
  CT_CHECK(!ct::is_return_target_kind(ct::NodeKind::CoolingSink));
}

CT_TEST(model_containment_matrices_hold_in_both_directions) {
  std::size_t pairs = 0;
  for (const ct::NodeKind container : kAllNodeKinds) {
    for (const ct::NodeKind contained : kAllNodeKinds) {
      const bool expected = expected_containment_pair(container, contained);
      if (expected) {
        ++pairs;
      }
      CT_CHECK_MSG(ct::containment_pair_allowed(container, contained) == expected,
                   std::string("containment of ") + std::string(ct::to_token(contained)) + " in " +
                       std::string(ct::to_token(container)));
    }
    CT_CHECK_MSG(ct::is_container_kind(container) == expected_container_kind(container),
                 std::string(ct::to_token(container)) + " container classification");
  }
  CT_CHECK_EQ(pairs, static_cast<std::size_t>(21));

  // Mandatory container and mandatory pump host are exact predicates.
  for (const ct::NodeKind kind : kAllNodeKinds) {
    const bool needs_container = kind == ct::NodeKind::Branch || kind == ct::NodeKind::CoolingSink;
    CT_CHECK_MSG(ct::requires_container(kind) == needs_container,
                 std::string(ct::to_token(kind)) + " requires-container");
    CT_CHECK_MSG(ct::requires_pump_host(kind) == (kind == ct::NodeKind::Pump),
                 std::string(ct::to_token(kind)) + " requires-pump-host");
    CT_CHECK_MSG(ct::is_redundancy_member_kind(kind) == (kind != ct::NodeKind::ThermalZone),
                 std::string(ct::to_token(kind)) + " redundancy-member");
  }
  CT_CHECK(ct::requires_container(ct::NodeKind::Branch));
  CT_CHECK(ct::requires_container(ct::NodeKind::CoolingSink));
  CT_CHECK(!ct::requires_container(ct::NodeKind::Manifold));
  CT_CHECK(ct::requires_pump_host(ct::NodeKind::Pump));
  CT_CHECK(!ct::requires_pump_host(ct::NodeKind::CoolingLoop));
}

CT_TEST(model_redundancy_scope_matrix_holds_in_both_directions) {
  std::size_t allowed = 0;
  for (const ct::RedundancyScope scope : kAllScopes) {
    std::size_t allowed_for_scope = 0;
    for (const ct::NodeKind kind : kAllNodeKinds) {
      const bool expected = expected_scope_allows(scope, kind);
      allowed_for_scope += expected ? 1u : 0u;
      CT_CHECK_MSG(ct::redundancy_scope_allows(scope, kind) == expected,
                   std::string("scope ") + std::string(ct::to_token(scope)) + " with member " +
                       std::string(ct::to_token(kind)));
    }
    allowed += allowed_for_scope;
    // Every scope admits at least one member kind and excludes at least one.
    CT_CHECK_MSG(allowed_for_scope > 0 && allowed_for_scope < 12, "scope cardinality");
  }
  CT_CHECK_EQ(allowed, static_cast<std::size_t>(10));
  CT_CHECK(ct::redundancy_scope_allows(ct::RedundancyScope::Distribution, ct::NodeKind::Cdu));
  CT_CHECK(!ct::redundancy_scope_allows(ct::RedundancyScope::Pump, ct::NodeKind::CoolingPlant));
  CT_CHECK(!ct::redundancy_scope_allows(ct::RedundancyScope::Plant, ct::NodeKind::CoolingSource));
}

CT_TEST(model_external_ref_create_and_ordering) {
  auto empty = ct::ExternalRef::create(ct::ExternalRefKind::Facility, "", ct::ExternalGeneration(0));
  CT_REQUIRE(!empty.has_value());
  expect_code(empty.error(), ct::ErrorCode::MissingField, "empty external identity");

  auto over_long = ct::ExternalRef::create(ct::ExternalRefKind::Facility,
                                           std::string(ct::limits::kMaxExternalIdentityBytes + 1u, 'x'),
                                           ct::ExternalGeneration(0));
  CT_REQUIRE(!over_long.has_value());
  expect_code(over_long.error(), ct::ErrorCode::TextTooLong, "over-long external identity");

  auto nul = ct::ExternalRef::create(ct::ExternalRefKind::Facility, std::string("a\0b", 3),
                                     ct::ExternalGeneration(0));
  CT_REQUIRE(!nul.has_value());
  expect_code(nul.error(), ct::ErrorCode::InvalidUtf8, "NUL-bearing external identity");

  auto exact = ct::ExternalRef::create(ct::ExternalRefKind::Facility,
                                       std::string(ct::limits::kMaxExternalIdentityBytes, 'x'),
                                       ct::ExternalGeneration(0));
  CT_REQUIRE(exact.has_value());
  CT_CHECK_EQ(exact->identity.size(), ct::limits::kMaxExternalIdentityBytes);

  auto preserved = ct::ExternalRef::create(ct::ExternalRefKind::Consumer, "Rack A/1 (row 2)",
                                           ct::ExternalGeneration(9));
  CT_REQUIRE(preserved.has_value());
  CT_CHECK_EQ(preserved->identity, std::string("Rack A/1 (row 2)"));
  CT_CHECK_EQ(preserved->generation.value(), static_cast<std::uint64_t>(9));

  auto same = ct::ExternalRef::create(ct::ExternalRefKind::Consumer, "Rack A/1 (row 2)",
                                      ct::ExternalGeneration(9));
  CT_REQUIRE(same.has_value());
  CT_CHECK(preserved->same_binding_as(*same));
  CT_CHECK(*preserved == *same);
  auto other_kind = ct::ExternalRef::create(ct::ExternalRefKind::Asset, "Rack A/1 (row 2)",
                                            ct::ExternalGeneration(9));
  CT_REQUIRE(other_kind.has_value());
  CT_CHECK(!preserved->same_binding_as(*other_kind));
  auto other_generation = ct::ExternalRef::create(ct::ExternalRefKind::Consumer, "Rack A/1 (row 2)",
                                                  ct::ExternalGeneration(10));
  CT_REQUIRE(other_generation.has_value());
  CT_CHECK(!preserved->same_binding_as(*other_generation));

  // Ordering key is exactly (kind, identity bytes, generation).
  const ct::ExternalRef first = ct::ExternalRef::create(ct::ExternalRefKind::Facility, "b",
                                                        ct::ExternalGeneration(3)).value();
  const ct::ExternalRef second = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "a",
                                                         ct::ExternalGeneration(1)).value();
  const ct::ExternalRef third = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "a",
                                                        ct::ExternalGeneration(2)).value();
  const ct::ExternalRef fourth = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "b",
                                                         ct::ExternalGeneration(0)).value();
  CT_CHECK(first < second);
  CT_CHECK(second < third);
  CT_CHECK(third < fourth);
  CT_CHECK(!(second < second));
  CT_CHECK(second <= second);
  CT_CHECK(fourth > first);
}

CT_TEST(model_node_create_rejections) {
  ct::ChillerAttributes attributes;
  attributes.kind = ct::ChillerKind::Centrifugal;

  auto empty_id = ct::Node::create(ct::NodeId(), attributes, {}, {});
  CT_REQUIRE(!empty_id.has_value());
  expect_code(empty_id.error(), ct::ErrorCode::MalformedIdentifier, "empty node identity");

  const std::string control_name = std::string("Rack") + std::string(1, static_cast<char>(0x01));
  auto bad_text = ct::Node::create(ct_test::nid("chiller:ch-1"), attributes, control_name, {});
  CT_REQUIRE(!bad_text.has_value());
  expect_code(bad_text.error(), ct::ErrorCode::InvalidUtf8, "control character in display name");

  auto over_long_text = ct::Node::create(ct_test::nid("chiller:ch-1"), attributes,
                                         std::string(ct::limits::kMaxDisplayNameBytes + 1u, 'x'), {});
  CT_REQUIRE(!over_long_text.has_value());
  expect_code(over_long_text.error(), ct::ErrorCode::InvalidUtf8, "over-long display name");

  std::vector<ct::ExternalRef> too_many;
  for (std::size_t index = 0; index <= ct::limits::kMaxNodeReferences; ++index) {
    too_many.push_back(ct_test::extref("location:row-" + std::to_string(index), ct::ExternalRefKind::Location, 0));
  }
  auto over_refs = ct::Node::create(ct_test::nid("chiller:ch-1"), attributes, {}, too_many);
  CT_REQUIRE(!over_refs.has_value());
  expect_code(over_refs.error(), ct::ErrorCode::LimitExceeded, "too many node references");

  std::vector<ct::ExternalRef> exact_refs(too_many.begin(), too_many.end() - 1);
  auto at_bound = ct::Node::create(ct_test::nid("chiller:ch-1"), attributes, {}, exact_refs);
  CT_REQUIRE(at_bound.has_value());
  CT_CHECK_EQ(at_bound->references.size(), ct::limits::kMaxNodeReferences);
  CT_CHECK_EQ(at_bound->kind(), ct::NodeKind::Chiller);
  CT_CHECK(at_bound->as_chiller() != nullptr);
  CT_CHECK(at_bound->as_pump() == nullptr);
  CT_CHECK(at_bound->as_cooling_plant() == nullptr);
  CT_CHECK(at_bound->as_cooling_source() == nullptr);

  std::vector<ct::ExternalRef> duplicates = {ct_test::extref("asset:a", ct::ExternalRefKind::Asset, 1),
                                             ct_test::extref("asset:b", ct::ExternalRefKind::Asset, 1),
                                             ct_test::extref("asset:a", ct::ExternalRefKind::Asset, 1)};
  auto duplicate = ct::Node::create(ct_test::nid("chiller:ch-1"), attributes, {}, duplicates);
  CT_REQUIRE(!duplicate.has_value());
  expect_code(duplicate.error(), ct::ErrorCode::DuplicateField, "duplicate node reference");

  std::vector<ct::ExternalRef> distinct = {ct_test::extref("asset:a", ct::ExternalRefKind::Asset, 1),
                                           ct_test::extref("asset:a", ct::ExternalRefKind::Asset, 2),
                                           ct_test::extref("asset:a", ct::ExternalRefKind::Rack, 1)};
  auto accepted = ct::Node::create(ct_test::nid("chiller:ch-1"), attributes, "Chiller 1", distinct);
  CT_REQUIRE(accepted.has_value());
  CT_CHECK_EQ(accepted->display_name, std::string("Chiller 1"));
  CT_CHECK_EQ(accepted->references.size(), static_cast<std::size_t>(3));
  CT_CHECK_EQ(accepted->id.str(), std::string("chiller:ch-1"));
}

CT_TEST(model_edge_create_and_canonical_key) {
  auto empty_id = ct::Edge::create(ct::EdgeId(), ct::EdgeKind::Supplies,
                                   ct_test::endpoint("a", ct::PortRole::SupplyOut),
                                   ct_test::endpoint("b", ct::PortRole::SourceIn));
  CT_REQUIRE(!empty_id.has_value());
  expect_code(empty_id.error(), ct::ErrorCode::MalformedIdentifier, "empty edge identity");

  auto empty_from = ct::Edge::create(ct_test::eid("e1"), ct::EdgeKind::Supplies, ct::Endpoint(),
                                     ct_test::endpoint("b", ct::PortRole::SourceIn));
  CT_REQUIRE(!empty_from.has_value());
  expect_code(empty_from.error(), ct::ErrorCode::EndpointMissing, "empty from endpoint");

  auto empty_to = ct::Edge::create(ct_test::eid("e1"), ct::EdgeKind::Supplies,
                                   ct_test::endpoint("a", ct::PortRole::SupplyOut), ct::Endpoint());
  CT_REQUIRE(!empty_to.has_value());
  expect_code(empty_to.error(), ct::ErrorCode::EndpointMissing, "empty to endpoint");

  const ct::Edge supplies = ct_test::edge("e1", ct::EdgeKind::Supplies, "a", ct::PortRole::SupplyOut, "b",
                                          ct::PortRole::SourceIn);
  const ct::Edge reversed_supplies = ct_test::edge("e2", ct::EdgeKind::Supplies, "b", ct::PortRole::SourceIn, "a",
                                                   ct::PortRole::SupplyOut);
  CT_CHECK(!(ct::EdgeKey::of(supplies) == ct::EdgeKey::of(reversed_supplies)));

  // Every edge kind is directed, so an exactly reversed pair is a distinct key.
  // That is what lets a reversed containment be reported as an invalid
  // containment and a reversed dependency as an impossible dependency cycle,
  // instead of both being collapsed into a duplicate edge.
  const ct::Edge contains = ct_test::edge("e3", ct::EdgeKind::Contains, "a", ct::PortRole::Container, "b",
                                          ct::PortRole::Contained);
  const ct::Edge contains_reversed = ct_test::edge("e4", ct::EdgeKind::Contains, "b", ct::PortRole::Contained, "a",
                                                   ct::PortRole::Container);
  CT_CHECK(!(ct::EdgeKey::of(contains) == ct::EdgeKey::of(contains_reversed)));
  CT_CHECK(ct::EdgeKey::of(contains) == ct::EdgeKey::of(contains));

  const ct::Edge depends = ct_test::edge("e5", ct::EdgeKind::DependsOn, "a", ct::PortRole::Terminal, "b",
                                         ct::PortRole::Terminal);
  const ct::Edge depends_reversed = ct_test::edge("e6", ct::EdgeKind::DependsOn, "b", ct::PortRole::Terminal, "a",
                                                  ct::PortRole::Terminal);
  CT_CHECK(!(ct::EdgeKey::of(depends) == ct::EdgeKey::of(depends_reversed)));
  CT_CHECK(ct::EdgeKey::of(depends) == ct::EdgeKey::of(depends));

  // The same reversed pair under a different kind is a different key.
  const ct::Edge returns_reversed = ct_test::edge("e7", ct::EdgeKind::Returns, "b", ct::PortRole::ReturnIn, "a",
                                                  ct::PortRole::HeatOut);
  CT_CHECK(!(ct::EdgeKey::of(contains) == ct::EdgeKey::of(returns_reversed)));

  // Different ports on the same nodes are a different key.
  const ct::Edge serves_one = ct_test::edge("e8", ct::EdgeKind::Serves, "a", ct::PortRole::Server, "b",
                                            ct::PortRole::Served);
  const ct::Edge serves_two = ct_test::edge("e9", ct::EdgeKind::Serves, "a", ct::PortRole::Served, "b",
                                            ct::PortRole::Server);
  CT_CHECK(!(ct::EdgeKey::of(serves_one) == ct::EdgeKey::of(serves_two)));

  // Edge keys order by (kind, first endpoint, second endpoint).
  const ct::EdgeKey key_returns = ct::EdgeKey::of(returns_reversed);
  const ct::EdgeKey key_contains = ct::EdgeKey::of(contains);
  CT_CHECK(key_returns < key_contains);
  CT_CHECK(!(key_contains < key_returns));
  CT_CHECK(key_contains < ct::EdgeKey::of(depends));
}

CT_TEST(model_redundancy_group_create_rejections) {
  const std::vector<ct::RedundancyMember> two_members = {
      ct::RedundancyMember{ct_test::nid("pump:p-1"), "pump:p-1", std::nullopt},
      ct::RedundancyMember{ct_test::nid("pump:p-2"), "pump:p-2", std::nullopt},
  };

  auto empty_id = ct::RedundancyGroup::create(ct::RedundancyGroupId(), ct::RedundancyScheme::NPlusOne,
                                              ct::RedundancyScope::Pump, {}, {}, two_members, false, false);
  CT_REQUIRE(!empty_id.has_value());
  expect_code(empty_id.error(), ct::ErrorCode::MalformedIdentifier, "empty redundancy group identity");

  auto empty_members = ct::RedundancyGroup::create(ct_test::gid("group:empty"), ct::RedundancyScheme::NPlusOne,
                                                   ct::RedundancyScope::Pump, {}, {}, {}, false, false);
  CT_REQUIRE(!empty_members.has_value());
  expect_code(empty_members.error(), ct::ErrorCode::GroupEmpty, "empty member list");

  std::vector<ct::RedundancyMember> over_long;
  over_long.reserve(ct::limits::kMaxGroupMemberCount + 1u);
  for (std::size_t index = 0; index <= ct::limits::kMaxGroupMemberCount; ++index) {
    over_long.push_back(ct::RedundancyMember{ct_test::nid("pump:p-" + std::to_string(index)),
                                             "pump:p-" + std::to_string(index), std::nullopt});
  }
  auto too_many = ct::RedundancyGroup::create(ct_test::gid("group:long"), ct::RedundancyScheme::NPlusOne,
                                              ct::RedundancyScope::Pump, {}, {}, over_long, false, false);
  CT_REQUIRE(!too_many.has_value());
  expect_code(too_many.error(), ct::ErrorCode::LimitExceeded, "over-long member list");

  std::vector<ct::RedundancyMember> exact = over_long;
  exact.pop_back();
  auto at_bound = ct::RedundancyGroup::create(ct_test::gid("group:long"), ct::RedundancyScheme::NPlusOne,
                                              ct::RedundancyScope::Pump, {}, {}, exact, false, false);
  CT_REQUIRE(at_bound.has_value());
  CT_CHECK_EQ(at_bound->members.size(), ct::limits::kMaxGroupMemberCount);

  const std::vector<ct::RedundancyMember> no_spelling = {
      ct::RedundancyMember{ct_test::nid("pump:p-1"), "", std::nullopt},
  };
  auto missing_spelling = ct::RedundancyGroup::create(ct_test::gid("group:spelling"), ct::RedundancyScheme::N,
                                                      ct::RedundancyScope::Pump, {}, {}, no_spelling, false, false);
  CT_REQUIRE(!missing_spelling.has_value());
  expect_code(missing_spelling.error(), ct::ErrorCode::GroupMemberMissing, "member without declared spelling");

  const std::vector<ct::RedundancyMember> no_node = {
      ct::RedundancyMember{ct::NodeId(), "pump:p-1", std::nullopt},
  };
  auto missing_node = ct::RedundancyGroup::create(ct_test::gid("group:node"), ct::RedundancyScheme::N,
                                                  ct::RedundancyScope::Pump, {}, {}, no_node, false, false);
  CT_REQUIRE(!missing_node.has_value());
  expect_code(missing_node.error(), ct::ErrorCode::GroupMemberMissing, "member without node identity");

  auto bad_text = ct::RedundancyGroup::create(
      ct_test::gid("group:text"), ct::RedundancyScheme::N, ct::RedundancyScope::Pump,
      std::string(1, static_cast<char>(0x02)), {}, two_members, false, false);
  CT_REQUIRE(!bad_text.has_value());
  expect_code(bad_text.error(), ct::ErrorCode::InvalidUtf8, "control character in group display name");

  auto accepted = ct::RedundancyGroup::create(ct_test::gid("group:pumps"), ct::RedundancyScheme::NPlusOne,
                                              ct::RedundancyScope::Pump, "Pumps", "duty plus standby", two_members,
                                              true, true);
  CT_REQUIRE(accepted.has_value());
  CT_CHECK(accepted->require_distinct_failure_domains);
  CT_CHECK(accepted->require_independent_sources);
  CT_CHECK_EQ(accepted->members.size(), static_cast<std::size_t>(2));
  CT_CHECK_EQ(accepted->scope, ct::RedundancyScope::Pump);
  CT_CHECK_EQ(accepted->scheme, ct::RedundancyScheme::NPlusOne);
}

CT_TEST(model_changeover_group_create_and_mutual_exclusion) {
  const std::vector<ct::Endpoint> two = {
      ct_test::endpoint("tower:ct-1", ct::PortRole::SupplyOut),
      ct_test::endpoint("cooler:fc-1", ct::PortRole::SupplyOut),
  };
  const std::vector<ct::Endpoint> three = {
      ct_test::endpoint("tower:ct-1", ct::PortRole::SupplyOut),
      ct_test::endpoint("cooler:fc-1", ct::PortRole::SupplyOut),
      ct_test::endpoint("hx:hx-1", ct::PortRole::SupplyOut),
  };

  auto empty_id = ct::ChangeoverGroup::create(ct::ChangeoverGroupId(), {}, three, 1);
  CT_REQUIRE(!empty_id.has_value());
  expect_code(empty_id.error(), ct::ErrorCode::MalformedIdentifier, "empty changeover identity");

  auto single = ct::ChangeoverGroup::create(ct_test::cid("co:one"), {}, {two.front()}, 1);
  CT_REQUIRE(!single.has_value());
  expect_code(single.error(), ct::ErrorCode::ChangeoverCardinality, "fewer than two members");

  auto zero = ct::ChangeoverGroup::create(ct_test::cid("co:zero"), {}, two, 0);
  CT_REQUIRE(!zero.has_value());
  expect_code(zero.error(), ct::ErrorCode::ChangeoverCardinality, "max_concurrent zero");

  auto all = ct::ChangeoverGroup::create(ct_test::cid("co:all"), {}, two, 2);
  CT_REQUIRE(!all.has_value());
  expect_code(all.error(), ct::ErrorCode::ChangeoverCardinality, "max_concurrent equals member count");

  auto boundary = ct::ChangeoverGroup::create(ct_test::cid("co:boundary"), {}, three, 2);
  CT_REQUIRE(boundary.has_value());
  CT_CHECK_EQ(boundary->members.size(), static_cast<std::size_t>(3));
  CT_CHECK_EQ(boundary->max_concurrent, static_cast<std::uint32_t>(2));

  auto too_many = ct::ChangeoverGroup::create(
      ct_test::cid("co:long"), {},
      std::vector<ct::Endpoint>(ct::limits::kMaxChangeoverMemberCount + 1u, two.front()), 1);
  CT_REQUIRE(!too_many.has_value());
  expect_code(too_many.error(), ct::ErrorCode::LimitExceeded, "over-long changeover member list");

  const std::vector<ct::Endpoint> bad_member = {two.front(), ct::Endpoint()};
  auto missing = ct::ChangeoverGroup::create(ct_test::cid("co:missing"), {}, bad_member, 1);
  CT_REQUIRE(!missing.has_value());
  expect_code(missing.error(), ct::ErrorCode::ChangeoverMemberInvalid, "changeover member without a node");

  auto exclusive = ct::ChangeoverGroup::create(ct_test::cid("co:exclusive"), "Valve", three, 1);
  CT_REQUIRE(exclusive.has_value());
  for (const ct::Endpoint& lhs : three) {
    for (const ct::Endpoint& rhs : three) {
      const bool expected = !(lhs == rhs);
      CT_CHECK_MSG(ct::structurally_mutually_exclusive(*exclusive, lhs, rhs) == expected,
                   "mutual exclusion for " + lhs.node.str() + " / " + rhs.node.str());
    }
  }
  // Distinct ports on one node are distinct members, so they are exclusive.
  const ct::Endpoint other_port = ct_test::endpoint("tower:ct-1", ct::PortRole::ReturnIn);
  CT_CHECK(!ct::structurally_mutually_exclusive(*exclusive, three.front(), other_port));
  CT_CHECK(!ct::structurally_mutually_exclusive(*exclusive, other_port, three.front()));

  // A group that tolerates two concurrent members excludes nothing.
  for (const ct::Endpoint& lhs : three) {
    for (const ct::Endpoint& rhs : three) {
      CT_CHECK(!ct::structurally_mutually_exclusive(*boundary, lhs, rhs));
    }
  }
}

CT_TEST(model_claim_boundary_is_total) {
  const ct::EvidencePosture posture;
  std::vector<std::string> tokens;
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(ct::ExcludedClaim::PowerState); ++raw) {
    const auto claim = static_cast<ct::ExcludedClaim>(raw);
    // Every excluded claim, without exception, is NotOwned by this component.
    CT_CHECK_MSG(ct::EvidencePosture::claim_disposition(claim) == ct::ClaimDisposition::NotOwned,
                 "claim disposition for " + std::string(ct::to_token(claim)));
    CT_CHECK_MSG(posture.claim_disposition(claim) == ct::ClaimDisposition::NotOwned,
                 "instance claim disposition for " + std::string(ct::to_token(claim)));
    const std::string token(ct::to_token(claim));
    CT_CHECK(!token.empty());
    for (const std::string& seen : tokens) {
      CT_CHECK_MSG(seen != token, "two excluded claims share the token " + token);
    }
    tokens.push_back(token);
    CT_CHECK_MSG(!std::string(ct::excluded_claim_owner(claim)).empty(),
                 "claim owner for " + token);
    CT_CHECK_MSG(std::string(ct::excluded_claim_owner(claim)) != "unknown owner",
                 "claim owner missing for " + token);
  }
  CT_CHECK_EQ(tokens.size(), static_cast<std::size_t>(11));

  CT_CHECK_EQ(ct::to_token(ct::ClaimDisposition::NotOwned), std::string_view("not_owned"));
  CT_CHECK(!std::string(ct::posture_statement()).empty());
  const std::string report = ct::posture_report();
  std::size_t lines = 0;
  for (const char character : report) {
    if (character == '\n') {
      ++lines;
    }
  }
  CT_CHECK_EQ(lines, static_cast<std::size_t>(12));
  for (const std::string& token : tokens) {
    CT_CHECK_MSG(report.find(token) != std::string::npos, "posture report omits " + token);
  }

  // Every evidence kind maps onto exactly one excluded claim, and the mapping is
  // a bijection onto the eleven claims.
  std::vector<std::uint8_t> mapped;
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(ct::EvidenceKind::PowerStateObservation); ++raw) {
    const auto kind = static_cast<ct::EvidenceKind>(raw);
    const ct::ExcludedClaim claim = ct::claim_of(kind);
    CT_CHECK_MSG(static_cast<std::uint8_t>(claim) < 11,
                 "claim mapping for " + std::string(ct::to_token(kind)));
    for (const std::uint8_t seen : mapped) {
      CT_CHECK_MSG(seen != static_cast<std::uint8_t>(claim),
                   "two evidence kinds map to " + std::string(ct::to_token(claim)));
    }
    mapped.push_back(static_cast<std::uint8_t>(claim));
  }
  CT_CHECK_EQ(mapped.size(), static_cast<std::size_t>(11));
  for (std::uint8_t raw = 0; raw < 11; ++raw) {
    bool found = false;
    for (const std::uint8_t seen : mapped) {
      found = found || seen == raw;
    }
    CT_CHECK_MSG(found, "no evidence kind maps to claim index " + std::to_string(raw));
  }
}
