// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_MODEL_HPP
#define DCCP_COOLING_TOPOLOGY_MODEL_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dccp/cooling_topology/evidence.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/strong_id.hpp"
#include "dccp/cooling_topology/units.hpp"

namespace dccp::cooling_topology {

// ===========================================================================
// Vocabulary
// ===========================================================================
//
// Every enum below classifies *structure*. None of them describes an operating
// state. In particular there is no field anywhere in this model for "running",
// "flowing", "available", "eligible", "safe" or "energized": those are owned by
// adjacent components and arrive only as EvidenceBinding records.

/// The medium an element carries or conditions. Medium continuity is a
/// structural compatibility rule: a Supplies or Returns edge whose two
/// endpoints both declare a medium requires the two declarations to agree.
enum class CoolingMedium : std::uint8_t {
  ChilledWater = 0,
  CondenserWater = 1,
  Glycol = 2,
  ProcessWater = 3,
  Refrigerant = 4,
  FacilityWater = 5,
  Air = 6,
};

/// Plant classification (what kind of cooling plant an element is).
enum class PlantKind : std::uint8_t {
  ChillerPlant = 0,
  HeatRejectionPlant = 1,
  HybridPlant = 2,
  DistrictSupply = 3,
};

enum class ChillerKind : std::uint8_t {
  Centrifugal = 0,
  Screw = 1,
  Scroll = 2,
  Absorption = 3,
  MagneticBearing = 4,
};

enum class PumpKind : std::uint8_t {
  Centrifugal = 0,
  VerticalTurbine = 1,
  PositiveDisplacement = 2,
};

/// Declared structural role of a pump in its installed arrangement. Duty and
/// Standby are declarations about how the plant is *built*, not about which
/// pump is running and not about whether the standby pump is eligible.
enum class PumpRole : std::uint8_t {
  Duty = 0,
  Standby = 1,
  Jockey = 2,
};

enum class LoopKind : std::uint8_t {
  Primary = 0,
  Secondary = 1,
  CondenserWater = 2,
  ChilledWater = 3,
  Glycol = 4,
  ProcessWater = 5,
  HeatRejection = 6,
};

enum class CduKind : std::uint8_t {
  RackCdu = 0,
  RowCdu = 1,
  InRowCdu = 2,
  InRackCdu = 3,
};

/// Physical placement class of an air-handling unit. Placement is structural:
/// it does not describe airflow direction, set points or containment policy.
enum class AirHandlerPlacement : std::uint8_t {
  Perimeter = 0,
  InRow = 1,
  Overhead = 2,
  RearDoor = 3,
};

enum class ManifoldKind : std::uint8_t {
  Supply = 0,
  Return = 1,
  Combined = 2,
};

enum class BranchKind : std::uint8_t {
  RackBranch = 0,
  ZoneBranch = 1,
  EquipmentBranch = 2,
};

enum class ZoneClass : std::uint8_t {
  ColdAisle = 0,
  HotAisle = 1,
  Room = 2,
  Rack = 3,
  Pod = 4,
  Plenum = 5,
};

enum class SinkKind : std::uint8_t {
  RackLoad = 0,
  DeviceLoad = 1,
  FacilityLoad = 2,
};

enum class SourceKind : std::uint8_t {
  FacilityWater = 0,
  AmbientAir = 1,
  Well = 2,
  DistrictCooling = 3,
  ThermalStore = 4,
};

/// Declared scheme of a redundancy group. The scheme is a declaration of the
/// intended structural arrangement; it is never a statement that the arrangement
/// is currently available or that a standby member is eligible to run.
enum class RedundancyScheme : std::uint8_t {
  N = 0,
  NPlusOne = 1,
  TwoN = 2,
  TwoNPlusOne = 3,
  DistributedRedundant = 4,
  ConcurrentlyMaintainable = 5,
};

/// What a redundancy group is a redundancy *of*. The scope constrains the node
/// kinds its members may have, so a group that mixes unrelated element kinds is
/// rejected instead of being silently accepted.
enum class RedundancyScope : std::uint8_t {
  Plant = 0,
  Chiller = 1,
  Pump = 2,
  CoolingLoop = 3,
  Distribution = 4,
  SourcePath = 5,
  AirHandling = 6,
};

std::string_view to_token(CoolingMedium value) noexcept;
std::string_view to_token(PlantKind value) noexcept;
std::string_view to_token(ChillerKind value) noexcept;
std::string_view to_token(PumpKind value) noexcept;
std::string_view to_token(PumpRole value) noexcept;
std::string_view to_token(LoopKind value) noexcept;
std::string_view to_token(CduKind value) noexcept;
std::string_view to_token(AirHandlerPlacement value) noexcept;
std::string_view to_token(ManifoldKind value) noexcept;
std::string_view to_token(BranchKind value) noexcept;
std::string_view to_token(ZoneClass value) noexcept;
std::string_view to_token(SinkKind value) noexcept;
std::string_view to_token(SourceKind value) noexcept;
std::string_view to_token(RedundancyScheme value) noexcept;
std::string_view to_token(RedundancyScope value) noexcept;

Result<CoolingMedium> parse_cooling_medium(std::string_view token);
Result<PlantKind> parse_plant_kind(std::string_view token);
Result<ChillerKind> parse_chiller_kind(std::string_view token);
Result<PumpKind> parse_pump_kind(std::string_view token);
Result<PumpRole> parse_pump_role(std::string_view token);
Result<LoopKind> parse_loop_kind(std::string_view token);
Result<CduKind> parse_cdu_kind(std::string_view token);
Result<AirHandlerPlacement> parse_air_handler_placement(std::string_view token);
Result<ManifoldKind> parse_manifold_kind(std::string_view token);
Result<BranchKind> parse_branch_kind(std::string_view token);
Result<ZoneClass> parse_zone_class(std::string_view token);
Result<SinkKind> parse_sink_kind(std::string_view token);
Result<SourceKind> parse_source_kind(std::string_view token);
Result<RedundancyScheme> parse_redundancy_scheme(std::string_view token);
Result<RedundancyScope> parse_redundancy_scope(std::string_view token);

// ===========================================================================
// Node kinds and ports
// ===========================================================================

/// Structural element kinds. Every element of the cooling graph is one of these;
/// there is no generic "device" node.
enum class NodeKind : std::uint8_t {
  CoolingPlant = 0,
  Chiller = 1,
  Pump = 2,
  CoolingLoop = 3,
  Cdu = 4,
  Crah = 5,
  Crac = 6,
  Manifold = 7,
  Branch = 8,
  ThermalZone = 9,
  CoolingSink = 10,
  CoolingSource = 11,
};

std::string_view to_token(NodeKind value) noexcept;
Result<NodeKind> parse_node_kind(std::string_view token);

/// Typed connection points.
///
/// The port model is deliberately small and uniform. Every distribution or
/// conversion element (source, plant, chiller, loop, manifold, branch, CDU)
/// carries the same four hydronic ports, so an edge's direction is checked
/// against the role a port plays rather than against the node kind alone:
///
///   SupplyOut  - outbound: cooled medium leaves this element downstream
///   SourceIn   - inbound:  cooled medium enters this element from upstream
///   HeatOut    - outbound: warmed medium leaves this element toward rejection
///   ReturnIn   - inbound:  warmed medium enters this element from downstream
///
/// A heat-exchange element therefore has two hydronic sides and uses the same
/// four ports on both: its cooling side is fed by SourceIn and delivers through
/// SupplyOut, and its heat-rejection side is fed by ReturnIn and rejects through
/// HeatOut. Which loop is on which side is determined by the loop's declared
/// kind and medium, and the medium-continuity rule rejects a mismatched pairing.
enum class PortRole : std::uint8_t {
  SupplyOut = 0,
  SourceIn = 1,
  HeatOut = 2,
  ReturnIn = 3,
  Terminal = 4,   ///< installation point of a pump; endpoint of a dependency edge
  Container = 5,  ///< containment: the containing side
  Contained = 6,  ///< containment: the contained side
  Server = 7,     ///< Serves: the element that provides the service
  Served = 8,     ///< Serves: the element that receives the service
};

std::string_view to_token(PortRole value) noexcept;
Result<PortRole> parse_port_role(std::string_view token);

/// Edge kinds. Each kind has an explicit endpoint-compatibility rule; a
/// combination that is not listed is rejected, never coerced.
enum class EdgeKind : std::uint8_t {
  /// Directed hydronic supply: cooled medium may structurally flow from the
  /// SupplyOut port of the first endpoint to the SourceIn port of the second.
  Supplies = 0,
  /// Directed hydronic return: warmed medium may structurally flow from the
  /// HeatOut port of the first endpoint to the ReturnIn port of the second.
  Returns = 1,
  /// Directed thermal service: the first endpoint conditions or delivers
  /// cooling service to the zone or sink at the second endpoint.
  Serves = 2,
  /// Installation: the distribution element at the first endpoint has the pump
  /// at the second endpoint installed on the named side (SupplyOut or ReturnIn).
  Pumps = 3,
  /// Structural containment. Not a cooling path: an element inside a plant does
  /// not thereby receive medium from it.
  Contains = 4,
  /// Declared structural dependency. Must form a DAG, both alone and together
  /// with containment.
  DependsOn = 5,
};

std::string_view to_token(EdgeKind value) noexcept;
Result<EdgeKind> parse_edge_kind(std::string_view token);

/// True when the port is legal for the given node kind at all.
bool port_allowed_for_kind(NodeKind kind, PortRole role) noexcept;

/// True when the node kind can deliver cooled medium (may appear as the first
/// endpoint of a Supplies edge).
bool is_supplier_kind(NodeKind kind) noexcept;
/// True when the node kind can receive cooled medium (may appear as the second
/// endpoint of a Supplies edge).
bool is_consumer_kind(NodeKind kind) noexcept;
/// True when the node kind can reject heat (may appear as the first endpoint of
/// a Returns edge).
bool is_heat_rejector_kind(NodeKind kind) noexcept;
/// True when the node kind can receive warmed medium (may appear as the second
/// endpoint of a Returns edge).
bool is_return_target_kind(NodeKind kind) noexcept;
/// True when the node kind can provide thermal service (Serves first endpoint).
bool is_server_kind(NodeKind kind) noexcept;
/// True when the node kind can receive thermal service (Serves second endpoint).
bool is_served_kind(NodeKind kind) noexcept;
/// True when the node kind can have a pump installed on it (Pumps first endpoint).
bool is_pump_host_kind(NodeKind kind) noexcept;

/// Documented containment pair: is the contained kind legal inside the container
/// kind? Containment is a physical enclosure relation, not a cooling path.
bool containment_pair_allowed(NodeKind container, NodeKind contained) noexcept;
/// True when the node kind may be a container at all.
bool is_container_kind(NodeKind kind) noexcept;
/// True when the node kind must have exactly one container (structural
/// completeness rule: a branch belongs to a distribution element and a sink is
/// located in a zone).
bool requires_container(NodeKind kind) noexcept;
/// True when the node kind must have exactly one pump installation site.
bool requires_pump_host(NodeKind kind) noexcept;
/// True when the node kind can be a member of a redundancy group.
bool is_redundancy_member_kind(NodeKind kind) noexcept;
/// True when the member kind is allowed by the redundancy scope.
bool redundancy_scope_allows(RedundancyScope scope, NodeKind member) noexcept;

/// The declared medium of a node, when it declares one. Absence means "not
/// declared" - never a default, never a violation by itself.
std::optional<CoolingMedium> declared_medium(const struct Node& node) noexcept;

// ===========================================================================
// Node attributes
// ===========================================================================

struct CoolingPlantAttributes {
  PlantKind kind = PlantKind::ChillerPlant;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct ChillerAttributes {
  ChillerKind kind = ChillerKind::Centrifugal;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct PumpAttributes {
  PumpKind kind = PumpKind::Centrifugal;
  PumpRole role = PumpRole::Duty;
};

struct CoolingLoopAttributes {
  LoopKind kind = LoopKind::Secondary;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct CduAttributes {
  CduKind kind = CduKind::RackCdu;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct CrahAttributes {
  AirHandlerPlacement placement = AirHandlerPlacement::Perimeter;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct CracAttributes {
  AirHandlerPlacement placement = AirHandlerPlacement::Perimeter;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct ManifoldAttributes {
  ManifoldKind kind = ManifoldKind::Supply;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct BranchAttributes {
  BranchKind kind = BranchKind::RackBranch;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct ThermalZoneAttributes {
  ZoneClass zone_class = ZoneClass::Room;
};

struct CoolingSinkAttributes {
  SinkKind kind = SinkKind::RackLoad;
  /// Opaque identity of the consumer owned by another registry.
  ExternalRef consumer;
  std::optional<MilliCelsius> max_acceptable_supply_temperature;
};

struct CoolingSourceAttributes {
  SourceKind kind = SourceKind::FacilityWater;
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply_temperature;
};

using NodeAttributes = std::variant<CoolingPlantAttributes, ChillerAttributes, PumpAttributes, CoolingLoopAttributes,
                                    CduAttributes, CrahAttributes, CracAttributes, ManifoldAttributes,
                                    BranchAttributes, ThermalZoneAttributes, CoolingSinkAttributes,
                                    CoolingSourceAttributes>;

/// Node kind implied by an attribute payload.
NodeKind kind_of(const NodeAttributes& attributes) noexcept;

struct Node {
  NodeId id;
  NodeAttributes attributes;
  /// Optional human-readable name; valid UTF-8, no control characters.
  std::string display_name;
  /// Additional opaque external references (location, rack, asset, ...).
  std::vector<ExternalRef> references;

  static Result<Node> create(NodeId id, NodeAttributes attributes, std::string display_name,
                             std::vector<ExternalRef> references);

  NodeKind kind() const noexcept { return kind_of(attributes); }
  const CoolingPlantAttributes* as_cooling_plant() const noexcept;
  const ChillerAttributes* as_chiller() const noexcept;
  const PumpAttributes* as_pump() const noexcept;
  const CoolingLoopAttributes* as_cooling_loop() const noexcept;
  const CduAttributes* as_cdu() const noexcept;
  const CrahAttributes* as_crah() const noexcept;
  const CracAttributes* as_crac() const noexcept;
  const ManifoldAttributes* as_manifold() const noexcept;
  const BranchAttributes* as_branch() const noexcept;
  const ThermalZoneAttributes* as_thermal_zone() const noexcept;
  const CoolingSinkAttributes* as_cooling_sink() const noexcept;
  const CoolingSourceAttributes* as_cooling_source() const noexcept;
};

/// Declared design supply temperature of a node, when it declares one.
std::optional<MilliCelsius> declared_supply_temperature(const Node& node) noexcept;
/// Highest design supply temperature a node declares it accepts, when declared.
std::optional<MilliCelsius> declared_max_supply_temperature(const Node& node) noexcept;

/// True when both sides of a declared design-temperature relation are present,
/// which is the only case in which the relation can be evaluated. A relation with
/// one side missing is not a violation and is never treated as satisfied: an
/// absent declaration is "not declared", which is distinct from "compatible".
bool temperature_relation_declared(const std::optional<MilliCelsius>& supply,
                                   const std::optional<MilliCelsius>& limit) noexcept;

// ===========================================================================
// Edges
// ===========================================================================

struct Endpoint {
  NodeId node;
  PortRole port = PortRole::SourceIn;

  friend bool operator==(const Endpoint& lhs, const Endpoint& rhs) noexcept = default;
  friend std::strong_ordering operator<=>(const Endpoint& lhs, const Endpoint& rhs) noexcept {
    if (auto cmp = lhs.node <=> rhs.node; cmp != 0) {
      return cmp;
    }
    return static_cast<std::uint8_t>(lhs.port) <=> static_cast<std::uint8_t>(rhs.port);
  }
};

struct Edge {
  EdgeId id;
  EdgeKind kind = EdgeKind::Supplies;
  Endpoint from;
  Endpoint to;

  static Result<Edge> create(EdgeId id, EdgeKind kind, Endpoint from, Endpoint to);
};

/// Canonical duplicate-detection key of an edge.
///
/// Every edge kind is directed, so the key is the ordered endpoint pair and an
/// exactly reversed pair is a distinct key. That matters: a reversed containment
/// must be reported as an invalid containment and a reversed dependency as an
/// impossible dependency cycle, not silently collapsed into a duplicate edge.
struct EdgeKey {
  EdgeKind kind = EdgeKind::Supplies;
  Endpoint first;
  Endpoint second;

  static EdgeKey of(const Edge& edge) noexcept;
  friend bool operator==(const EdgeKey&, const EdgeKey&) noexcept = default;
  friend std::strong_ordering operator<=>(const EdgeKey&, const EdgeKey&) noexcept = default;
};

// ===========================================================================
// Redundancy
// ===========================================================================

/// One member of a redundancy group.
///
/// The declared spelling preserves the producer's spelling (canonical node id or
/// alias); the node field is the resolved canonical identity. Two members that
/// resolve to the same node are a double count and are rejected, whether the
/// producer spelled them the same way or through two aliases.
struct RedundancyMember {
  NodeId node;
  std::string declared;
  std::optional<ExternalRef> failure_domain;

  friend bool operator==(const RedundancyMember&, const RedundancyMember&) noexcept = default;
};

/// A declared redundancy arrangement.
///
/// A group states what is *built*: which elements are members, under which
/// scheme and scope, and which independence properties the arrangement is
/// declared to have. It is never a statement about operational readiness,
/// eligibility, capacity or failover authority. Membership in a group proves
/// only that the producer declared the relationship.
struct RedundancyGroup {
  RedundancyGroupId id;
  RedundancyScheme scheme = RedundancyScheme::N;
  RedundancyScope scope = RedundancyScope::Plant;
  std::string display_name;
  /// Declared structural basis of the arrangement (free text, descriptive only).
  std::string basis;
  std::vector<RedundancyMember> members;
  /// When set, every member must declare a failure-domain reference and no two
  /// members may declare the same one.
  bool require_distinct_failure_domains = false;
  /// When set, no two members may share a structural source and neither may be
  /// an ancestor of the other in the supply graph. This is a claim about
  /// *structure*; availability and capacity are not evaluated.
  bool require_independent_sources = false;

  static Result<RedundancyGroup> create(RedundancyGroupId id, RedundancyScheme scheme, RedundancyScope scope,
                                        std::string display_name, std::string basis,
                                        std::vector<RedundancyMember> members,
                                        bool require_distinct_failure_domains, bool require_independent_sources);
};

/// A secondary identity for a node (legacy name, vendor tag, former id).
///
/// Aliases exist so that producers can use the name they know while the
/// canonical topology keeps one identity per physical element. An alias id must
/// not collide with a node id or with another alias id, and an alias must not
/// target another alias (no chains, no cycles).
struct Alias {
  AliasId id;
  NodeId target;
};

/// A declared structural changeover arrangement.
///
/// A changeover group states that the physical arrangement admits at most
/// max_concurrent of the listed endpoints as the source-side connection at one
/// time (for example a changeover valve between a fluid cooler and a cooling
/// tower). It is a structural declaration only: this library does not know or
/// claim which member is selected, whether any member is reachable, whether a
/// changeover is permitted, or that the arrangement is currently honoured by the
/// installed controls.
struct ChangeoverGroup {
  ChangeoverGroupId id;
  std::string display_name;
  std::vector<Endpoint> members;
  std::uint32_t max_concurrent = 1;

  static Result<ChangeoverGroup> create(ChangeoverGroupId id, std::string display_name,
                                        std::vector<Endpoint> members, std::uint32_t max_concurrent);
};

/// True when the declared arrangement forbids two endpoints from being the
/// selected source-side connection at the same time: they are members of one
/// changeover group whose max_concurrent is 1.
bool structurally_mutually_exclusive(const ChangeoverGroup& group, const Endpoint& lhs, const Endpoint& rhs) noexcept;

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_MODEL_HPP
