// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal graph index shared by validation, query and diff. Not installed and
// not part of the public contract.

#ifndef DCCP_COOLING_TOPOLOGY_GRAPH_INTERNAL_HPP
#define DCCP_COOLING_TOPOLOGY_GRAPH_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology::internal {

/// Sentinel returned when an endpoint does not resolve to a node.
inline constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;

/// Identity lookup over a node table: canonical node ids and aliases, each in a
/// sorted slot array so a lookup never depends on hash order.
class NodeIndex {
 public:
  NodeIndex() = default;

  void build(const std::vector<Node>& nodes, const std::vector<Alias>& aliases);

  /// Resolves a canonical node identity or an alias identity.
  std::optional<std::uint32_t> find(std::string_view identity) const noexcept;
  /// Resolves a canonical node identity only.
  std::optional<std::uint32_t> find_node(std::string_view identity) const noexcept;
  /// Resolves an alias identity only.
  std::optional<std::uint32_t> find_alias(std::string_view identity) const noexcept;

  std::size_t node_count() const noexcept { return node_count_; }

  /// The alias table the index was built from, for reporting.
  std::span<const Alias> aliases() const noexcept { return aliases_; }

 private:
  std::size_t node_count_ = 0;
  std::vector<std::pair<std::string_view, std::uint32_t>> node_slots_;
  std::vector<std::pair<std::string_view, std::uint32_t>> alias_slots_;
  std::span<const Alias> aliases_;
};

/// Directed adjacency over a resolved edge list.
///
/// Edge endpoints are resolved once. An endpoint that names an unknown node or
/// an alias with no target resolves to kNoNode; callers that require a valid
/// graph must have validated it first, and every traversal treats kNoNode as a
/// dead end rather than dereferencing it.
class GraphIndex {
 public:
  GraphIndex() = default;

  void build(const std::vector<Node>& nodes, const std::vector<Edge>& edges, const std::vector<Alias>& aliases);

  std::size_t node_count() const noexcept { return node_count_; }
  std::size_t edge_count() const noexcept { return edges_ == nullptr ? 0 : edges_->size(); }
  const NodeIndex& identities() const noexcept { return identities_; }
  const std::vector<Node>& nodes() const noexcept { return *nodes_; }
  const std::vector<Edge>& edges() const noexcept { return *edges_; }

  std::uint32_t from_of(std::uint32_t edge) const noexcept { return from_[edge]; }
  std::uint32_t to_of(std::uint32_t edge) const noexcept { return to_[edge]; }

  std::span<const std::uint32_t> out_edges(std::uint32_t node) const noexcept;
  std::span<const std::uint32_t> in_edges(std::uint32_t node) const noexcept;

 private:
  std::size_t node_count_ = 0;
  const std::vector<Node>* nodes_ = nullptr;
  const std::vector<Edge>* edges_ = nullptr;
  NodeIndex identities_;
  std::vector<std::uint32_t> from_;
  std::vector<std::uint32_t> to_;
  std::vector<std::uint32_t> out_flat_;
  std::vector<std::uint32_t> in_flat_;
  std::vector<std::size_t> out_offset_;
  std::vector<std::size_t> in_offset_;
};

/// True when the node kind can be reached as a structural cooling source: a
/// source or plant that delivers medium and has no incoming Supplies edge.
bool is_origin_kind(NodeKind kind) noexcept;

/// Directed cycle detection restricted to a set of edge kinds.
///
/// Iterative (no recursion), bounded by the graph size, and deterministic: the
/// search visits nodes in ascending index order, so the reported cycle is always
/// the same for the same graph. When a cycle is found, cycle_edges receives the
/// identities of the edges that form it, in traversal order.
bool find_cycle(const GraphIndex& graph, std::span<const EdgeKind> kinds, std::vector<std::string>& cycle_edges);

/// Directed cycle detection over the union of two edge-kind sets.
bool find_cycle_union(const GraphIndex& graph, std::span<const EdgeKind> first, std::span<const EdgeKind> second,
                      std::vector<std::string>& cycle_edges);

/// Bounded breadth-first traversal.
///
/// follow_in selects incoming edges, otherwise outgoing. kinds selects the edge
/// kinds that are traversed. Nodes are visited in ascending index order within a
/// depth level, so the visit order is a pure function of the graph.
struct TraversalResult {
  std::vector<std::uint32_t> visited;         ///< discovery order, excluding the origin
  std::vector<std::uint32_t> depth;           ///< depth per visited entry
  std::vector<std::uint32_t> via_edge;        ///< edge index per visited entry
  bool truncated = false;
};

TraversalResult traverse(const GraphIndex& graph, std::uint32_t origin, std::span<const EdgeKind> kinds, bool follow_in,
                         std::size_t max_depth, std::size_t max_nodes);

/// Structural sources that can reach a node by following Supplies edges
/// backwards, plus the Pumps edges of the hosts on the way (a pump is installed
/// on a loop, so a loop that reaches a sink reaches it through its pumps). The
/// result is sorted ascending by node index. Bounded by max_nodes.
struct SourceSet {
  std::vector<std::uint32_t> sources;
  bool truncated = false;
};

SourceSet structural_sources_reaching(const GraphIndex& graph, std::uint32_t node, std::size_t max_nodes);

/// All nodes that can deliver cooled medium (supplier kinds) and have no
/// incoming Supplies edge, in ascending node order.
std::vector<std::uint32_t> origin_nodes(const GraphIndex& graph);

/// Structural sources of a redundancy member.
///
/// A member that can deliver medium is traced through its own incoming Supplies
/// edges. A pump is attached through a Pumps edge rather than a Supplies edge,
/// so its sources are the sources of the distribution element it is installed
/// on; a pump with no installation site has no sources.
SourceSet member_source_set(const GraphIndex& graph, std::uint32_t node, std::size_t max_nodes);

}  // namespace dccp::cooling_topology::internal

#endif  // DCCP_COOLING_TOPOLOGY_GRAPH_INTERNAL_HPP
