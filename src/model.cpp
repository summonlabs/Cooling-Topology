// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "dccp/cooling_topology/text.hpp"

namespace dccp::cooling_topology {
namespace {

// ---------------------------------------------------------------------------
// Token tables
// ---------------------------------------------------------------------------

constexpr std::string_view kCoolingMediumTokens[] = {"chilled_water", "condenser_water", "glycol",
                                                     "process_water", "refrigerant",     "facility_water",
                                                     "air"};
constexpr std::string_view kPlantKindTokens[] = {"chiller_plant", "heat_rejection_plant", "hybrid_plant",
                                                 "district_supply"};
constexpr std::string_view kChillerKindTokens[] = {"centrifugal", "screw", "scroll", "absorption",
                                                   "magnetic_bearing"};
constexpr std::string_view kPumpKindTokens[] = {"centrifugal", "vertical_turbine", "positive_displacement"};
constexpr std::string_view kPumpRoleTokens[] = {"duty", "standby", "jockey"};
constexpr std::string_view kLoopKindTokens[] = {"primary",       "secondary",    "condenser_water", "chilled_water",
                                                "glycol",        "process_water", "heat_rejection"};
constexpr std::string_view kCduKindTokens[] = {"rack_cdu", "row_cdu", "in_row_cdu", "in_rack_cdu"};
constexpr std::string_view kAirHandlerPlacementTokens[] = {"perimeter", "in_row", "overhead", "rear_door"};
constexpr std::string_view kManifoldKindTokens[] = {"supply", "return", "combined"};
constexpr std::string_view kBranchKindTokens[] = {"rack_branch", "zone_branch", "equipment_branch"};
constexpr std::string_view kZoneClassTokens[] = {"cold_aisle", "hot_aisle", "room", "rack", "pod", "plenum"};
constexpr std::string_view kSinkKindTokens[] = {"rack_load", "device_load", "facility_load"};
constexpr std::string_view kSourceKindTokens[] = {"facility_water", "ambient_air", "well", "district_cooling",
                                                  "thermal_store"};
constexpr std::string_view kRedundancySchemeTokens[] = {"n",          "n_plus_one",   "two_n",
                                                        "two_n_plus_one", "distributed_redundant",
                                                        "concurrently_maintainable"};
constexpr std::string_view kRedundancyScopeTokens[] = {"plant",      "chiller",     "pump",        "cooling_loop",
                                                       "distribution", "source_path", "air_handling"};
constexpr std::string_view kExternalRefKindTokens[] = {"facility", "rack",       "asset",   "location",
                                                       "failure_domain", "consumer", "registry", "evidence"};
constexpr std::string_view kNodeKindTokens[] = {"cooling_plant", "chiller",  "pump",     "cooling_loop",
                                                "cdu",           "crah",     "crac",     "manifold",
                                                "branch",        "thermal_zone", "cooling_sink", "cooling_source"};
constexpr std::string_view kPortRoleTokens[] = {"supply_out", "source_in", "heat_out", "return_in", "terminal",
                                                "container",  "contained", "server",   "served"};
constexpr std::string_view kEdgeKindTokens[] = {"supplies", "returns", "serves", "pumps", "contains", "depends_on"};

constexpr std::size_t kNodeKindCount = 12;

/// Generic token lookup. A value outside the declared range never indexes the
/// table.
template <class Enum, std::size_t N>
std::string_view token_of(Enum value, const std::string_view (&tokens)[N]) noexcept {
  const auto raw = static_cast<std::size_t>(value);
  return raw < N ? tokens[raw] : std::string_view("unknown");
}

template <class Enum, std::size_t N>
Result<Enum> parse_token(std::string_view token, const std::string_view (&tokens)[N], std::string_view what) {
  for (std::size_t index = 0; index < N; ++index) {
    if (token == tokens[index]) {
      return static_cast<Enum>(index);
    }
  }
  std::string message = "unknown ";
  message += what;
  message += " token";
  return Error(ErrorCode::UnknownEnumToken, std::move(message)).with_subject(std::string(token.substr(0, 64)));
}

// ---------------------------------------------------------------------------
// Port legality
// ---------------------------------------------------------------------------

constexpr std::uint32_t port_bit(PortRole role) noexcept {
  return 1u << static_cast<unsigned>(role);
}

/// Every element carries the generic terminal connection point: it is the
/// endpoint of a dependency edge and the installation point of a pump.
constexpr std::uint32_t kDistributionPorts =
    port_bit(PortRole::SupplyOut) | port_bit(PortRole::SourceIn) | port_bit(PortRole::HeatOut) |
    port_bit(PortRole::ReturnIn) | port_bit(PortRole::Terminal);

/// Ports legal on each node kind, indexed by NodeKind. The table is the
/// normative statement of which connection points a kind carries; a role that is
/// absent here can never appear on an endpoint of that kind.
constexpr std::uint32_t kPortMask[] = {
    /* CoolingPlant  */ kDistributionPorts | port_bit(PortRole::Container) | port_bit(PortRole::Contained),
    /* Chiller       */ kDistributionPorts | port_bit(PortRole::Container) | port_bit(PortRole::Contained),
    /* Pump          */ port_bit(PortRole::Terminal) | port_bit(PortRole::Contained),
    /* CoolingLoop   */ kDistributionPorts | port_bit(PortRole::Container) | port_bit(PortRole::Contained) |
        port_bit(PortRole::Server),
    /* Cdu           */ kDistributionPorts | port_bit(PortRole::Container) | port_bit(PortRole::Contained) |
        port_bit(PortRole::Server),
    /* Crah          */ port_bit(PortRole::SourceIn) | port_bit(PortRole::HeatOut) | port_bit(PortRole::Server) |
        port_bit(PortRole::Contained) | port_bit(PortRole::Terminal),
    /* Crac          */ port_bit(PortRole::SourceIn) | port_bit(PortRole::HeatOut) | port_bit(PortRole::Server) |
        port_bit(PortRole::Contained) | port_bit(PortRole::Terminal),
    /* Manifold      */ kDistributionPorts | port_bit(PortRole::Container) | port_bit(PortRole::Contained) |
        port_bit(PortRole::Server),
    /* Branch        */ kDistributionPorts | port_bit(PortRole::Container) | port_bit(PortRole::Contained) |
        port_bit(PortRole::Server),
    /* ThermalZone   */ port_bit(PortRole::Served) | port_bit(PortRole::Container) | port_bit(PortRole::Contained) |
        port_bit(PortRole::Terminal),
    /* CoolingSink   */ port_bit(PortRole::SourceIn) | port_bit(PortRole::HeatOut) | port_bit(PortRole::Served) |
        port_bit(PortRole::Contained) | port_bit(PortRole::Terminal),
    /* CoolingSource */ port_bit(PortRole::SupplyOut) | port_bit(PortRole::ReturnIn) | port_bit(PortRole::Terminal),
};

static_assert(sizeof(kPortMask) / sizeof(kPortMask[0]) == kNodeKindCount, "one port mask per node kind");

constexpr NodeKind kSupplierKinds[] = {NodeKind::CoolingSource, NodeKind::CoolingPlant, NodeKind::Chiller,
                                       NodeKind::CoolingLoop,   NodeKind::Manifold,     NodeKind::Branch,
                                       NodeKind::Cdu};
constexpr NodeKind kConsumerKinds[] = {NodeKind::CoolingPlant, NodeKind::Chiller,  NodeKind::CoolingLoop,
                                       NodeKind::Manifold,     NodeKind::Branch,   NodeKind::Cdu,
                                       NodeKind::Crah,         NodeKind::Crac,     NodeKind::CoolingSink};
constexpr NodeKind kHeatRejectorKinds[] = {NodeKind::CoolingPlant, NodeKind::Chiller, NodeKind::CoolingLoop,
                                           NodeKind::Manifold,     NodeKind::Branch,  NodeKind::Cdu,
                                           NodeKind::Crah,         NodeKind::Crac,    NodeKind::CoolingSink};
constexpr NodeKind kReturnTargetKinds[] = {NodeKind::CoolingSource, NodeKind::CoolingPlant, NodeKind::Chiller,
                                           NodeKind::CoolingLoop,   NodeKind::Manifold,     NodeKind::Branch,
                                           NodeKind::Cdu};
constexpr NodeKind kServerKinds[] = {NodeKind::Crah,         NodeKind::Crac,   NodeKind::Cdu,
                                     NodeKind::CoolingLoop,  NodeKind::Manifold, NodeKind::Branch};
constexpr NodeKind kServedKinds[] = {NodeKind::ThermalZone, NodeKind::CoolingSink};
constexpr NodeKind kPumpHostKinds[] = {NodeKind::CoolingLoop, NodeKind::Manifold, NodeKind::Branch};

template <std::size_t N>
constexpr bool contains_kind(const NodeKind (&kinds)[N], NodeKind kind) noexcept {
  for (std::size_t index = 0; index < N; ++index) {
    if (kinds[index] == kind) {
      return true;
    }
  }
  return false;
}

/// Containment pairs, documented in the header. A pair that is not listed is
/// rejected; containment is an enclosure relation between physical elements and
/// is never inferred from a cooling connection.
constexpr bool containment_pair(NodeKind container, NodeKind contained) noexcept {
  switch (container) {
    case NodeKind::CoolingPlant:
      return contained == NodeKind::Chiller || contained == NodeKind::Pump || contained == NodeKind::CoolingLoop ||
             contained == NodeKind::Manifold || contained == NodeKind::Cdu;
    case NodeKind::CoolingLoop:
      return contained == NodeKind::Pump || contained == NodeKind::Manifold || contained == NodeKind::Branch ||
             contained == NodeKind::Chiller || contained == NodeKind::Cdu;
    case NodeKind::Chiller:
      return contained == NodeKind::Pump;
    case NodeKind::Cdu:
      return contained == NodeKind::Pump;
    case NodeKind::Manifold:
      return contained == NodeKind::Branch || contained == NodeKind::Pump || contained == NodeKind::Cdu;
    case NodeKind::Branch:
      return contained == NodeKind::Pump || contained == NodeKind::Cdu;
    case NodeKind::ThermalZone:
      return contained == NodeKind::CoolingSink || contained == NodeKind::Crah || contained == NodeKind::Crac ||
             contained == NodeKind::Cdu;
    default:
      return false;
  }
}

constexpr bool scope_allows(RedundancyScope scope, NodeKind member) noexcept {
  switch (scope) {
    case RedundancyScope::Plant:
      return member == NodeKind::CoolingPlant;
    case RedundancyScope::Chiller:
      return member == NodeKind::Chiller;
    case RedundancyScope::Pump:
      return member == NodeKind::Pump;
    case RedundancyScope::CoolingLoop:
      return member == NodeKind::CoolingLoop;
    case RedundancyScope::Distribution:
      return member == NodeKind::Manifold || member == NodeKind::Branch || member == NodeKind::Cdu;
    case RedundancyScope::SourcePath:
      return member == NodeKind::CoolingSource;
    case RedundancyScope::AirHandling:
      return member == NodeKind::Crah || member == NodeKind::Crac;
    default:
      return false;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

std::string_view to_token(CoolingMedium value) noexcept { return token_of(value, kCoolingMediumTokens); }
std::string_view to_token(PlantKind value) noexcept { return token_of(value, kPlantKindTokens); }
std::string_view to_token(ChillerKind value) noexcept { return token_of(value, kChillerKindTokens); }
std::string_view to_token(PumpKind value) noexcept { return token_of(value, kPumpKindTokens); }
std::string_view to_token(PumpRole value) noexcept { return token_of(value, kPumpRoleTokens); }
std::string_view to_token(LoopKind value) noexcept { return token_of(value, kLoopKindTokens); }
std::string_view to_token(CduKind value) noexcept { return token_of(value, kCduKindTokens); }
std::string_view to_token(AirHandlerPlacement value) noexcept {
  return token_of(value, kAirHandlerPlacementTokens);
}
std::string_view to_token(ManifoldKind value) noexcept { return token_of(value, kManifoldKindTokens); }
std::string_view to_token(BranchKind value) noexcept { return token_of(value, kBranchKindTokens); }
std::string_view to_token(ZoneClass value) noexcept { return token_of(value, kZoneClassTokens); }
std::string_view to_token(SinkKind value) noexcept { return token_of(value, kSinkKindTokens); }
std::string_view to_token(SourceKind value) noexcept { return token_of(value, kSourceKindTokens); }
std::string_view to_token(RedundancyScheme value) noexcept { return token_of(value, kRedundancySchemeTokens); }
std::string_view to_token(RedundancyScope value) noexcept { return token_of(value, kRedundancyScopeTokens); }

Result<CoolingMedium> parse_cooling_medium(std::string_view token) {
  return parse_token<CoolingMedium>(token, kCoolingMediumTokens, "cooling-medium");
}
Result<PlantKind> parse_plant_kind(std::string_view token) {
  return parse_token<PlantKind>(token, kPlantKindTokens, "plant-kind");
}
Result<ChillerKind> parse_chiller_kind(std::string_view token) {
  return parse_token<ChillerKind>(token, kChillerKindTokens, "chiller-kind");
}
Result<PumpKind> parse_pump_kind(std::string_view token) {
  return parse_token<PumpKind>(token, kPumpKindTokens, "pump-kind");
}
Result<PumpRole> parse_pump_role(std::string_view token) {
  return parse_token<PumpRole>(token, kPumpRoleTokens, "pump-role");
}
Result<LoopKind> parse_loop_kind(std::string_view token) {
  return parse_token<LoopKind>(token, kLoopKindTokens, "loop-kind");
}
Result<CduKind> parse_cdu_kind(std::string_view token) {
  return parse_token<CduKind>(token, kCduKindTokens, "cdu-kind");
}
Result<AirHandlerPlacement> parse_air_handler_placement(std::string_view token) {
  return parse_token<AirHandlerPlacement>(token, kAirHandlerPlacementTokens, "air-handler-placement");
}
Result<ManifoldKind> parse_manifold_kind(std::string_view token) {
  return parse_token<ManifoldKind>(token, kManifoldKindTokens, "manifold-kind");
}
Result<BranchKind> parse_branch_kind(std::string_view token) {
  return parse_token<BranchKind>(token, kBranchKindTokens, "branch-kind");
}
Result<ZoneClass> parse_zone_class(std::string_view token) {
  return parse_token<ZoneClass>(token, kZoneClassTokens, "zone-class");
}
Result<SinkKind> parse_sink_kind(std::string_view token) {
  return parse_token<SinkKind>(token, kSinkKindTokens, "sink-kind");
}
Result<SourceKind> parse_source_kind(std::string_view token) {
  return parse_token<SourceKind>(token, kSourceKindTokens, "source-kind");
}
Result<RedundancyScheme> parse_redundancy_scheme(std::string_view token) {
  return parse_token<RedundancyScheme>(token, kRedundancySchemeTokens, "redundancy-scheme");
}
Result<RedundancyScope> parse_redundancy_scope(std::string_view token) {
  return parse_token<RedundancyScope>(token, kRedundancyScopeTokens, "redundancy-scope");
}

// ---------------------------------------------------------------------------
// External references
// ---------------------------------------------------------------------------

std::string_view to_token(ExternalRefKind value) noexcept { return token_of(value, kExternalRefKindTokens); }

Result<ExternalRefKind> parse_external_ref_kind(std::string_view token) {
  return parse_token<ExternalRefKind>(token, kExternalRefKindTokens, "external-ref-kind");
}

Result<ExternalRef> ExternalRef::create(ExternalRefKind kind, std::string identity,
                                        ExternalGeneration generation) {
  if (identity.empty()) {
    return Error(ErrorCode::MissingField, "external identity must not be empty");
  }
  if (identity.size() > limits::kMaxExternalIdentityBytes) {
    return Error(ErrorCode::TextTooLong, "external identity exceeds the external-identity byte bound");
  }
  if (!is_valid_external_identity(identity, limits::kMaxExternalIdentityBytes)) {
    return Error(ErrorCode::InvalidUtf8,
                 "external identity must be valid UTF-8 without embedded NUL; it is otherwise "
                 "preserved byte for byte")
        .with_subject(identity.substr(0, 64));
  }
  ExternalRef reference;
  reference.kind = kind;
  reference.identity = std::move(identity);
  reference.generation = generation;
  return reference;
}

bool ExternalRef::same_binding_as(const ExternalRef& other) const noexcept {
  return kind == other.kind && generation == other.generation && external_identity_equal(identity, other.identity);
}

bool operator==(const ExternalRef& lhs, const ExternalRef& rhs) noexcept { return lhs.same_binding_as(rhs); }

std::strong_ordering operator<=>(const ExternalRef& lhs, const ExternalRef& rhs) noexcept {
  if (const auto cmp = static_cast<std::uint8_t>(lhs.kind) <=> static_cast<std::uint8_t>(rhs.kind); cmp != 0) {
    return cmp;
  }
  if (const int cmp = lhs.identity.compare(rhs.identity); cmp != 0) {
    return cmp < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
  }
  return lhs.generation <=> rhs.generation;
}

// ---------------------------------------------------------------------------
// Node kinds and ports
// ---------------------------------------------------------------------------

std::string_view to_token(NodeKind value) noexcept { return token_of(value, kNodeKindTokens); }

Result<NodeKind> parse_node_kind(std::string_view token) {
  return parse_token<NodeKind>(token, kNodeKindTokens, "node-kind");
}

std::string_view to_token(PortRole value) noexcept { return token_of(value, kPortRoleTokens); }

Result<PortRole> parse_port_role(std::string_view token) {
  return parse_token<PortRole>(token, kPortRoleTokens, "port-role");
}

std::string_view to_token(EdgeKind value) noexcept { return token_of(value, kEdgeKindTokens); }

Result<EdgeKind> parse_edge_kind(std::string_view token) {
  return parse_token<EdgeKind>(token, kEdgeKindTokens, "edge-kind");
}

bool port_allowed_for_kind(NodeKind kind, PortRole role) noexcept {
  const auto kind_index = static_cast<std::size_t>(kind);
  const auto role_index = static_cast<std::size_t>(role);
  if (kind_index >= kNodeKindCount || role_index > static_cast<std::size_t>(PortRole::Served)) {
    return false;
  }
  return (kPortMask[kind_index] & port_bit(role)) != 0;
}

bool is_supplier_kind(NodeKind kind) noexcept { return contains_kind(kSupplierKinds, kind); }
bool is_consumer_kind(NodeKind kind) noexcept { return contains_kind(kConsumerKinds, kind); }
bool is_heat_rejector_kind(NodeKind kind) noexcept { return contains_kind(kHeatRejectorKinds, kind); }
bool is_return_target_kind(NodeKind kind) noexcept { return contains_kind(kReturnTargetKinds, kind); }
bool is_server_kind(NodeKind kind) noexcept { return contains_kind(kServerKinds, kind); }
bool is_served_kind(NodeKind kind) noexcept { return contains_kind(kServedKinds, kind); }
bool is_pump_host_kind(NodeKind kind) noexcept { return contains_kind(kPumpHostKinds, kind); }

bool containment_pair_allowed(NodeKind container, NodeKind contained) noexcept {
  return containment_pair(container, contained);
}

bool is_container_kind(NodeKind kind) noexcept {
  switch (kind) {
    case NodeKind::CoolingPlant:
    case NodeKind::Chiller:
    case NodeKind::CoolingLoop:
    case NodeKind::Cdu:
    case NodeKind::Manifold:
    case NodeKind::Branch:
    case NodeKind::ThermalZone:
      return true;
    default:
      return false;
  }
}

bool requires_container(NodeKind kind) noexcept {
  return kind == NodeKind::Branch || kind == NodeKind::CoolingSink;
}

bool requires_pump_host(NodeKind kind) noexcept { return kind == NodeKind::Pump; }

bool is_redundancy_member_kind(NodeKind kind) noexcept { return kind != NodeKind::ThermalZone; }

bool redundancy_scope_allows(RedundancyScope scope, NodeKind member) noexcept { return scope_allows(scope, member); }

// ---------------------------------------------------------------------------
// Node attributes
// ---------------------------------------------------------------------------

NodeKind kind_of(const NodeAttributes& attributes) noexcept {
  static_assert(std::variant_size_v<NodeAttributes> == kNodeKindCount,
                "the attribute variant must have exactly one alternative per node kind");
  return static_cast<NodeKind>(attributes.index());
}

const CoolingPlantAttributes* Node::as_cooling_plant() const noexcept {
  return std::get_if<CoolingPlantAttributes>(&attributes);
}
const ChillerAttributes* Node::as_chiller() const noexcept { return std::get_if<ChillerAttributes>(&attributes); }
const PumpAttributes* Node::as_pump() const noexcept { return std::get_if<PumpAttributes>(&attributes); }
const CoolingLoopAttributes* Node::as_cooling_loop() const noexcept {
  return std::get_if<CoolingLoopAttributes>(&attributes);
}
const CduAttributes* Node::as_cdu() const noexcept { return std::get_if<CduAttributes>(&attributes); }
const CrahAttributes* Node::as_crah() const noexcept { return std::get_if<CrahAttributes>(&attributes); }
const CracAttributes* Node::as_crac() const noexcept { return std::get_if<CracAttributes>(&attributes); }
const ManifoldAttributes* Node::as_manifold() const noexcept { return std::get_if<ManifoldAttributes>(&attributes); }
const BranchAttributes* Node::as_branch() const noexcept { return std::get_if<BranchAttributes>(&attributes); }
const ThermalZoneAttributes* Node::as_thermal_zone() const noexcept {
  return std::get_if<ThermalZoneAttributes>(&attributes);
}
const CoolingSinkAttributes* Node::as_cooling_sink() const noexcept {
  return std::get_if<CoolingSinkAttributes>(&attributes);
}
const CoolingSourceAttributes* Node::as_cooling_source() const noexcept {
  return std::get_if<CoolingSourceAttributes>(&attributes);
}

Result<Node> Node::create(NodeId id, NodeAttributes attributes, std::string display_name,
                          std::vector<ExternalRef> references) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "node identity must not be empty");
  }
  if (!display_name.empty() && !is_valid_display_text(display_name, limits::kMaxDisplayNameBytes)) {
    return Error(ErrorCode::InvalidUtf8,
                 "node display name must be valid UTF-8 display text without control characters")
        .with_subject(id.str());
  }
  if (references.size() > limits::kMaxNodeReferences) {
    return Error(ErrorCode::LimitExceeded, "node carries more external references than the documented bound")
        .with_subject(id.str());
  }
  for (std::size_t index = 0; index < references.size(); ++index) {
    if (references[index].identity.empty()) {
      return Error(ErrorCode::MissingField, "node external reference must not be empty").with_subject(id.str());
    }
    for (std::size_t other = index + 1; other < references.size(); ++other) {
      if (references[index].same_binding_as(references[other])) {
        return Error(ErrorCode::DuplicateField, "node declares the same external reference twice")
            .with_subject(id.str())
            .with_detail("reference index " + std::to_string(index) + " duplicates index " + std::to_string(other));
      }
    }
  }
  Node node;
  node.id = std::move(id);
  node.attributes = std::move(attributes);
  node.display_name = std::move(display_name);
  node.references = std::move(references);
  return node;
}

std::optional<CoolingMedium> declared_medium(const Node& node) noexcept {
  if (const auto* value = node.as_cooling_plant()) {
    return value->medium;
  }
  if (const auto* value = node.as_chiller()) {
    return value->medium;
  }
  if (const auto* value = node.as_cooling_loop()) {
    return value->medium;
  }
  if (const auto* value = node.as_cdu()) {
    return value->medium;
  }
  if (const auto* value = node.as_crah()) {
    return value->medium;
  }
  if (const auto* value = node.as_crac()) {
    return value->medium;
  }
  if (const auto* value = node.as_manifold()) {
    return value->medium;
  }
  if (const auto* value = node.as_branch()) {
    return value->medium;
  }
  if (const auto* value = node.as_cooling_source()) {
    return value->medium;
  }
  return std::nullopt;
}

std::optional<MilliCelsius> declared_supply_temperature(const Node& node) noexcept {
  if (const auto* value = node.as_cooling_plant()) {
    return value->design_supply_temperature;
  }
  if (const auto* value = node.as_chiller()) {
    return value->design_supply_temperature;
  }
  if (const auto* value = node.as_cooling_loop()) {
    return value->design_supply_temperature;
  }
  if (const auto* value = node.as_cdu()) {
    return value->design_supply_temperature;
  }
  if (const auto* value = node.as_manifold()) {
    return value->design_supply_temperature;
  }
  if (const auto* value = node.as_branch()) {
    return value->design_supply_temperature;
  }
  if (const auto* value = node.as_cooling_source()) {
    return value->design_supply_temperature;
  }
  return std::nullopt;
}

std::optional<MilliCelsius> declared_max_supply_temperature(const Node& node) noexcept {
  if (const auto* value = node.as_cooling_plant()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_chiller()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_cooling_loop()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_cdu()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_crah()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_crac()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_manifold()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_branch()) {
    return value->max_acceptable_supply_temperature;
  }
  if (const auto* value = node.as_cooling_sink()) {
    return value->max_acceptable_supply_temperature;
  }
  return std::nullopt;
}

bool temperature_relation_declared(const std::optional<MilliCelsius>& supply,
                                   const std::optional<MilliCelsius>& limit) noexcept {
  return supply.has_value() && limit.has_value();
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

Result<Edge> Edge::create(EdgeId id, EdgeKind kind, Endpoint from, Endpoint to) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "edge identity must not be empty");
  }
  if (from.node.empty() || to.node.empty()) {
    return Error(ErrorCode::EndpointMissing, "edge endpoint must name a node").with_subject(id.str());
  }
  Edge edge;
  edge.id = std::move(id);
  edge.kind = kind;
  edge.from = std::move(from);
  edge.to = std::move(to);
  return edge;
}

EdgeKey EdgeKey::of(const Edge& edge) noexcept {
  EdgeKey key;
  key.kind = edge.kind;
  // Every edge kind in this model is directed, so the key is the ordered
  // endpoint pair. An exactly reversed pair is therefore NOT a duplicate: a
  // reversed containment is an invalid containment, and a reversed dependency is
  // an impossible dependency cycle, and both must reach the stage that names
  // them instead of being collapsed into a duplicate here.
  key.first = edge.from;
  key.second = edge.to;
  return key;
}

// ---------------------------------------------------------------------------
// Redundancy
// ---------------------------------------------------------------------------

Result<RedundancyGroup> RedundancyGroup::create(RedundancyGroupId id, RedundancyScheme scheme,
                                                RedundancyScope scope, std::string display_name, std::string basis,
                                                std::vector<RedundancyMember> members,
                                                bool require_distinct_failure_domains,
                                                bool require_independent_sources) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "redundancy group identity must not be empty");
  }
  if (!display_name.empty() && !is_valid_display_text(display_name, limits::kMaxDisplayNameBytes)) {
    return Error(ErrorCode::InvalidUtf8, "redundancy group display name must be valid UTF-8 display text")
        .with_subject(id.str());
  }
  if (!basis.empty() && !is_valid_display_text(basis, limits::kMaxBasisBytes)) {
    return Error(ErrorCode::InvalidUtf8, "redundancy group basis must be valid UTF-8 display text")
        .with_subject(id.str());
  }
  if (members.empty()) {
    return Error(ErrorCode::GroupEmpty, "redundancy group must declare at least one member").with_subject(id.str());
  }
  if (members.size() > limits::kMaxGroupMemberCount) {
    return Error(ErrorCode::LimitExceeded, "redundancy group declares more members than the documented bound")
        .with_subject(id.str());
  }
  for (const RedundancyMember& member : members) {
    if (member.node.empty()) {
      return Error(ErrorCode::GroupMemberMissing, "redundancy member must name a node").with_subject(id.str());
    }
    if (member.declared.empty()) {
      return Error(ErrorCode::GroupMemberMissing, "redundancy member must carry its declared spelling")
          .with_subject(id.str());
    }
    if (!is_valid_display_text(member.declared, limits::kMaxIdentifierBytes)) {
      return Error(ErrorCode::MalformedIdentifier,
                   "redundancy member declared spelling must be valid identifier text")
          .with_subject(id.str());
    }
    if (member.failure_domain.has_value() && member.failure_domain->identity.empty()) {
      return Error(ErrorCode::GroupMemberMissing, "redundancy member failure domain must not be empty")
          .with_subject(id.str());
    }
  }
  RedundancyGroup group;
  group.id = std::move(id);
  group.scheme = scheme;
  group.scope = scope;
  group.display_name = std::move(display_name);
  group.basis = std::move(basis);
  group.members = std::move(members);
  group.require_distinct_failure_domains = require_distinct_failure_domains;
  group.require_independent_sources = require_independent_sources;
  return group;
}

// ---------------------------------------------------------------------------
// Changeover groups
// ---------------------------------------------------------------------------

Result<ChangeoverGroup> ChangeoverGroup::create(ChangeoverGroupId id, std::string display_name,
                                                std::vector<Endpoint> members, std::uint32_t max_concurrent) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "changeover group identity must not be empty");
  }
  if (!display_name.empty() && !is_valid_display_text(display_name, limits::kMaxDisplayNameBytes)) {
    return Error(ErrorCode::InvalidUtf8, "changeover group display name must be valid UTF-8 display text")
        .with_subject(id.str());
  }
  if (members.size() < 2) {
    return Error(ErrorCode::ChangeoverCardinality,
                 "a changeover arrangement needs at least two declared members")
        .with_subject(id.str());
  }
  if (members.size() > limits::kMaxChangeoverMemberCount) {
    return Error(ErrorCode::LimitExceeded, "changeover group declares more members than the documented bound")
        .with_subject(id.str());
  }
  if (max_concurrent == 0 || static_cast<std::size_t>(max_concurrent) >= members.size()) {
    return Error(ErrorCode::ChangeoverCardinality,
                 "max_concurrent must be at least 1 and strictly below the member count, otherwise the "
                 "declaration is not a changeover")
        .with_subject(id.str());
  }
  for (const Endpoint& member : members) {
    if (member.node.empty()) {
      return Error(ErrorCode::ChangeoverMemberInvalid, "changeover member must name a node").with_subject(id.str());
    }
  }
  ChangeoverGroup group;
  group.id = std::move(id);
  group.display_name = std::move(display_name);
  group.members = std::move(members);
  group.max_concurrent = max_concurrent;
  return group;
}

bool structurally_mutually_exclusive(const ChangeoverGroup& group, const Endpoint& lhs,
                                     const Endpoint& rhs) noexcept {
  if (group.max_concurrent != 1 || lhs == rhs) {
    return false;
  }
  bool found_lhs = false;
  bool found_rhs = false;
  for (const Endpoint& member : group.members) {
    found_lhs = found_lhs || member == lhs;
    found_rhs = found_rhs || member == rhs;
  }
  return found_lhs && found_rhs;
}

}  // namespace dccp::cooling_topology
