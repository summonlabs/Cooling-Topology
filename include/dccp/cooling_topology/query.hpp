// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_QUERY_HPP
#define DCCP_COOLING_TOPOLOGY_QUERY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/evidence.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {

// ===========================================================================
// Epistemic posture of every answer
// ===========================================================================
//
// Every result type in this header carries a ClaimClass and an EvidencePosture.
// The vocabulary is deliberately restricted: an answer is either
// "StructurallyPossible" (a path exists in this generation) or
// "StructurallyImpossible" (no path exists in this generation, for any
// operating state). There is no third value that could carry an operational
// meaning, and every excluded claim is reported NotOwned.

/// Classification of a structural finding.
enum class ClaimClass : std::uint8_t {
  /// The structural relationship exists in this generation. It is *possible*;
  /// it is not asserted to be operating, flowing, capable, eligible, safe or
  /// authorized.
  StructurallyPossible = 0,
  /// No structural relationship exists in this generation, for any operating
  /// state.
  StructurallyImpossible = 1,
};

std::string_view to_token(ClaimClass value) noexcept;
/// One-line reminder of what a ClaimClass does and does not assert.
std::string_view claim_class_statement(ClaimClass value) noexcept;

// ===========================================================================
// Query options
// ===========================================================================

struct QueryOptions {
  /// Maximum traversal depth. Defaults to the documented bound.
  std::size_t max_depth = limits::kMaxQueryDepth;
  /// Maximum number of result elements before the query reports truncation.
  std::size_t max_results = limits::kMaxQueryResultCount;
  /// Maximum enumerated paths.
  std::size_t max_paths = limits::kMaxPathCount;
  /// Maximum candidate elements considered by dependency-removal analyses.
  std::size_t max_candidates = 4096;
  /// Maximum nodes expanded by one bounded search: the independent-path search
  /// and each dependency-removal probe. A search that hits the bound reports
  /// truncation instead of returning a partial answer as if it were complete.
  std::size_t max_search_nodes = limits::kMaxIndependentPathSearch;
  /// Follow Contains edges when reporting containment peers. Containment is
  /// never a cooling path; it is reported separately and never mixed into
  /// reachability.
  bool include_containment_peers = true;
};

/// A reached element in a structural traversal.
struct ReachedElement {
  NodeId node;
  std::uint32_t depth = 0;
  /// Edge through which the element was reached; empty for the origin.
  EdgeId via_edge;
  /// Endpoint of the traversed edge that lies on the reached element.
  PortRole entered_port = PortRole::SourceIn;
};

/// Structural supply upstream closure: elements that can structurally deliver
/// cooled medium to the origin. Traverses Supplies edges backwards.
struct UpstreamResult {
  NodeId origin;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<ReachedElement> elements;
  /// True when the traversal hit a configured bound; the result is then a
  /// prefix, never a complete claim.
  bool truncated = false;
  EvidencePosture posture;
};

/// Structural supply downstream closure: elements the origin can structurally
/// deliver cooled medium to. Traverses Supplies edges forwards.
struct DownstreamResult {
  NodeId origin;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<ReachedElement> elements;
  bool truncated = false;
  EvidencePosture posture;
};

/// One structural supply path between two elements.
struct CoolingPath {
  /// Node sequence, origin first. Adjacent entries are connected by the edge at
  /// the same index in edges.
  std::vector<NodeId> nodes;
  /// Edge sequence; size() == nodes.size() - 1.
  std::vector<EdgeId> edges;
  /// True when the path passes through a node that hosts a pump.
  bool passes_pump_host = false;
  /// True when the path traverses an endpoint that belongs to a changeover
  /// group, so a declared structural changeover constrains the arrangement.
  bool crosses_changeover = false;
  /// Number of distinct structural sources that can reach the origin of the
  /// path.
  std::size_t origin_source_count = 0;
};

/// Why two structurally possible paths cannot both be the selected arrangement.
enum class ExclusivityReason : std::uint8_t {
  /// Both paths traverse different members of one declared changeover group
  /// whose max_concurrent is 1.
  SharedChangeoverGroup = 0,
};

std::string_view to_token(ExclusivityReason reason) noexcept;

struct PathPairExclusivity {
  std::size_t first = 0;
  std::size_t second = 0;
  ExclusivityReason reason = ExclusivityReason::SharedChangeoverGroup;
  /// Identity of the declared changeover group that makes the pair exclusive.
  std::string witness;
};

/// Result of a structural supply-path query.
struct PathQueryResult {
  NodeId from;
  NodeId to;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<CoolingPath> paths;
  std::vector<PathPairExclusivity> exclusive_pairs;
  /// True when path enumeration stopped at a configured bound.
  bool truncated = false;
  /// Sources that can structurally reach the destination at all (canonical
  /// order).
  std::vector<NodeId> reachable_sources;
  EvidencePosture posture;
};

/// One structurally complete circuit: a supply path out and a return path back.
///
/// A hydronic circuit is the one place where a cycle in the directed graph is
/// physically meaningful: supply edges form a DAG, return edges form a DAG, and
/// their union contains the circuit. The validator enforces exactly that.
struct CoolingCircuit {
  std::vector<NodeId> supply_nodes;
  std::vector<EdgeId> supply_edges;
  std::vector<NodeId> return_nodes;
  std::vector<EdgeId> return_edges;
  bool crosses_changeover = false;
};

struct CircuitQueryResult {
  NodeId from;
  NodeId to;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<CoolingCircuit> circuits;
  bool truncated = false;
  EvidencePosture posture;
};

/// Result of a common-dependency query.
struct CommonDependencyResult {
  std::vector<NodeId> subjects;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  /// Elements that are structural supply dependencies of every subject,
  /// excluding the subjects themselves. Canonical order.
  std::vector<NodeId> dependencies;
  bool truncated = false;
  EvidencePosture posture;
};

/// One element whose removal leaves subjects with no structural source.
struct DependencyPoint {
  NodeId node;
  /// Subject elements that lose every structural source path when this element
  /// is removed (canonical order).
  std::vector<NodeId> disconnected_subjects;
  /// True when every subject loses all structural source paths.
  bool disconnects_all_subjects = false;
  /// True when this element is itself a structural source, in which case
  /// removing it removes the source and not a shared dependency.
  bool is_structural_source = false;
};

struct SinglePointResult {
  std::vector<NodeId> subjects;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<DependencyPoint> points;
  /// True when the candidate set was truncated by a configured bound.
  bool truncated = false;
  EvidencePosture posture;
};

/// Edge-disjoint structural supply paths between two elements.
///
/// The count is a structural property of the graph: how many supply routes exist
/// that share no edge. It is not a capacity, not a flow and not a statement that
/// any route is in service.
struct IndependentPathResult {
  NodeId from;
  NodeId to;
  ClaimClass claim = ClaimClass::StructurallyImpossible;
  /// Number of edge-disjoint supply paths found, bounded by options.max_paths.
  std::size_t path_count = 0;
  /// The discovered paths, in deterministic discovery order.
  std::vector<CoolingPath> paths;
  bool truncated = false;
  EvidencePosture posture;
};

/// Which redundancy groups a node participates in.
struct GroupMembership {
  RedundancyGroupId group;
  std::size_t member_index = 0;
  std::string declared;
};

struct RedundancyMembershipResult {
  NodeId node;
  std::vector<GroupMembership> memberships;
  EvidencePosture posture;
};

/// Structural verdict on a declared redundancy group. The verdict evaluates the
/// declaration against the graph; it never evaluates readiness or eligibility.
enum class RedundancyVerdict : std::uint8_t {
  /// The declaration is consistent with the graph.
  DeclarationConsistent = 0,
  /// The declaration is contradicted by the graph.
  DeclarationViolated = 1,
  /// The graph does not contain enough declared structure to decide.
  IndependenceUnproven = 2,
};

std::string_view to_token(RedundancyVerdict verdict) noexcept;

struct RedundancyMemberReport {
  NodeId node;
  std::string declared;
  /// Structural sources that can reach the member (canonical order).
  std::vector<NodeId> sources;
  /// Failure-domain identity bytes declared for the member, when declared.
  std::string failure_domain;
  bool failure_domain_declared = false;
};

struct RedundancyGroupReport {
  RedundancyGroupId group;
  RedundancyScheme scheme = RedundancyScheme::N;
  RedundancyScope scope = RedundancyScope::Plant;
  std::vector<RedundancyMemberReport> members;
  /// Sources shared by two or more members (canonical order). Empty when every
  /// member has a disjoint source set.
  std::vector<NodeId> shared_sources;
  /// Members that are structural ancestors of another member (canonical order).
  std::vector<NodeId> ancestor_members;
  /// Failure-domain identities declared by two or more members (canonical
  /// order).
  std::vector<std::string> duplicate_failure_domains;
  /// True when every declared independence property holds in the graph.
  bool independence_holds = false;
  RedundancyVerdict verdict = RedundancyVerdict::IndependenceUnproven;
  bool truncated = false;
  EvidencePosture posture;
};

/// Structural verdict on how a sink is fed.
enum class SinkFeedVerdict : std::uint8_t {
  /// Exactly one structural supply route reaches the sink.
  SingleFeed = 0,
  /// More than one supply route reaches the sink and no two of them share a
  /// structural source.
  MultipleIndependentFeeds = 1,
  /// More than one supply route reaches the sink but they share a structural
  /// source.
  MultipleFeedsSharingDependency = 2,
  /// No supply edge reaches the sink in this generation.
  NoStructuralFeed = 3,
};

std::string_view to_token(SinkFeedVerdict verdict) noexcept;

struct SinkFeed {
  /// Element that directly feeds the sink (the Supplies edge source).
  NodeId feeder;
  EdgeId edge;
  /// Structural sources that can reach the feeder (canonical order).
  std::vector<NodeId> sources;
};

struct SinkServiceReport {
  NodeId sink;
  SinkKind kind = SinkKind::RackLoad;
  /// Direct feeders in canonical order.
  std::vector<SinkFeed> feeds;
  /// Sources shared by two or more feeders (canonical order).
  std::vector<NodeId> shared_sources;
  SinkFeedVerdict verdict = SinkFeedVerdict::NoStructuralFeed;
  EvidencePosture posture;
};

/// Structural verdict on a thermal zone.
enum class ZoneVerdict : std::uint8_t {
  /// At least one serving element and at least one contained sink exist.
  Served = 0,
  /// No serving element reaches the zone in this generation.
  NoServingElement = 1,
  /// The zone serves nothing: it contains no sink.
  NoContainedSink = 2,
  /// Neither a serving element nor a contained sink exists.
  Unpopulated = 3,
};

std::string_view to_token(ZoneVerdict verdict) noexcept;

struct ZoneReport {
  NodeId zone;
  ZoneClass zone_class = ZoneClass::Room;
  /// Elements that declare they serve the zone, in canonical order.
  std::vector<NodeId> serving_elements;
  /// Sinks contained in the zone, in canonical order.
  std::vector<NodeId> contained_sinks;
  /// Air-handling units contained in the zone, in canonical order.
  std::vector<NodeId> contained_air_handlers;
  /// Redundancy groups whose members serve or are contained in the zone.
  std::vector<RedundancyGroupId> groups;
  ZoneVerdict verdict = ZoneVerdict::Unpopulated;
  EvidencePosture posture;
};

/// Containment neighbours: elements sharing an enclosure. Reported separately
/// from cooling impact because containment is not a cooling relation.
struct ContainmentPeer {
  NodeId node;
  /// True when the peer contains the origin.
  bool contains_origin = false;
  /// True when the origin contains the peer.
  bool contained_by_origin = false;
  /// True when both share one container.
  bool sibling = false;
};

struct BlastRadiusResult {
  NodeId origin;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  /// Elements the origin can structurally supply (downstream closure).
  std::vector<ReachedElement> supplied_downstream;
  /// Elements sharing an enclosure with the origin. Not a cooling claim.
  std::vector<ContainmentPeer> containment_peers;
  /// Sinks that are downstream of the origin (canonical order).
  std::vector<NodeId> affected_sinks;
  /// Zones that are downstream of the origin or served by a downstream element
  /// (canonical order).
  std::vector<NodeId> affected_zones;
  bool truncated = false;
  EvidencePosture posture;
};

/// One connected component of the structural graph.
///
/// Connectivity here is undirected over every edge kind including containment
/// and dependency, so it answers "is this element structurally attached to
/// anything at all", not "can it be cooled". An element with no edge of any kind
/// is reported separately as isolated.
struct ComponentReport {
  std::vector<std::vector<NodeId>> components;
  std::vector<NodeId> isolated;
  bool truncated = false;
  EvidencePosture posture;
};

// ===========================================================================
// Queries
// ===========================================================================

Result<UpstreamResult> upstream_of(const Topology& topology, const NodeId& identity,
                                   const QueryOptions& options = {});
Result<DownstreamResult> downstream_of(const Topology& topology, const NodeId& identity,
                                       const QueryOptions& options = {});
Result<PathQueryResult> possible_supply_paths(const Topology& topology, const NodeId& from, const NodeId& to,
                                              const QueryOptions& options = {});
Result<CircuitQueryResult> possible_circuits(const Topology& topology, const NodeId& from, const NodeId& to,
                                             const QueryOptions& options = {});
Result<IndependentPathResult> independent_supply_paths(const Topology& topology, const NodeId& from, const NodeId& to,
                                                       const QueryOptions& options = {});
Result<CommonDependencyResult> common_dependencies(const Topology& topology, const std::vector<NodeId>& subjects,
                                                   const QueryOptions& options = {});
Result<SinglePointResult> single_points_of_structural_dependency(const Topology& topology,
                                                                 const std::vector<NodeId>& subjects,
                                                                 const QueryOptions& options = {});
Result<RedundancyMembershipResult> redundancy_membership(const Topology& topology, const NodeId& identity);
Result<RedundancyGroupReport> redundancy_group_report(const Topology& topology, const RedundancyGroupId& group,
                                                      const QueryOptions& options = {});
Result<SinkServiceReport> sink_service_report(const Topology& topology, const NodeId& identity,
                                             const QueryOptions& options = {});
Result<ZoneReport> zone_report(const Topology& topology, const NodeId& identity, const QueryOptions& options = {});
Result<BlastRadiusResult> blast_radius(const Topology& topology, const NodeId& identity,
                                       const QueryOptions& options = {});
Result<ComponentReport> connected_components(const Topology& topology, const QueryOptions& options = {});

/// Structural sources that can reach an element; canonical order.
Result<std::vector<NodeId>> sources_serving(const Topology& topology, const NodeId& identity,
                                            const QueryOptions& options = {});

/// Deterministic description of one node for inspection output.
std::string describe_node(const Topology& topology, const Node& node);
/// Deterministic description of one edge for inspection output.
std::string describe_edge(const Topology& topology, const Edge& edge);

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_QUERY_HPP
