// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// White-box obligations for the internal support layer the public answers are
// built on: the identity index, the resolved adjacency index, iterative cycle
// detection, the bounded traversal, the structural source sets, canonical table
// ordering and the generation-file frame codec.
//
// Every rejection below is asserted through its ErrorCode and never through
// message text. Nothing here asks whether a component is running, whether medium
// is flowing, whether a path has capacity or whether a source is eligible: the
// internals answer structural questions only.

#include "graph_internal.hpp"
#include "validate_internal.hpp"

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/topology.hpp"

#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;
namespace internal = dccp::cooling_topology::internal;

using ct_test::aid;
using ct_test::base_draft;
using ct_test::branch_node;
using ct_test::crah_node;
using ct_test::cid;
using ct_test::crac_node;
using ct_test::edge;
using ct_test::gid;
using ct_test::loop_node;
using ct_test::manifold_node;
using ct_test::nid;
using ct_test::plant_node;
using ct_test::pump_node;
using ct_test::reference_facility;
using ct_test::source_node;

/// Asserts a documented rejection without reading the failure text.
void expect_code(const ct::Error& error, ct::ErrorCode expected, std::string_view what) {
  CT_CHECK_MSG(error.code() == expected,
               std::string(what) + ": expected " + std::string(ct::error_code_name(expected)) +
                   ", observed " + std::string(ct::error_code_name(error.code())));
}

/// The edge kinds whose cycles the circuit stage of validation forbids.
constexpr std::array<ct::EdgeKind, 1> kSupplies{ct::EdgeKind::Supplies};
constexpr std::array<ct::EdgeKind, 1> kReturns{ct::EdgeKind::Returns};
constexpr std::array<ct::EdgeKind, 1> kContains{ct::EdgeKind::Contains};
constexpr std::array<ct::EdgeKind, 1> kDepends{ct::EdgeKind::DependsOn};

/// A named node/edge/alias table. An index borrows the table it was built from,
/// so the table must outlive the index; every use below keeps both in one scope.
struct Tables {
  std::vector<ct::Node> nodes;
  std::vector<ct::Edge> edges;
  std::vector<ct::Alias> aliases;
};

internal::GraphIndex index_of(const Tables& tables) {
  internal::GraphIndex graph;
  graph.build(tables.nodes, tables.edges, tables.aliases);
  return graph;
}

std::uint32_t node_at(const internal::GraphIndex& graph, std::string_view identity) {
  const auto found = graph.identities().find(identity);
  CT_REQUIRE(found.has_value());
  return *found;
}

std::size_t edge_at(const internal::GraphIndex& graph, std::string_view identity) {
  const std::vector<ct::Edge>& edges = graph.edges();
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (edges[index].id.str() == identity) {
      return index;
    }
  }
  CT_REQUIRE(false);
  return edges.size();
}

std::vector<std::string> identity_texts(const std::vector<ct::Node>& nodes) {
  std::vector<std::string> out;
  out.reserve(nodes.size());
  for (const ct::Node& node : nodes) {
    out.push_back(node.id.str());
  }
  return out;
}

std::vector<std::string> edge_texts(const std::vector<ct::Edge>& edges) {
  std::vector<std::string> out;
  out.reserve(edges.size());
  for (const ct::Edge& record : edges) {
    out.push_back(record.id.str());
  }
  return out;
}

std::vector<std::string> alias_texts(const std::vector<ct::Alias>& aliases) {
  std::vector<std::string> out;
  out.reserve(aliases.size());
  for (const ct::Alias& alias : aliases) {
    out.push_back(alias.id.str());
  }
  return out;
}

std::vector<std::string> group_texts(const std::vector<ct::RedundancyGroup>& groups) {
  std::vector<std::string> out;
  out.reserve(groups.size());
  for (const ct::RedundancyGroup& group : groups) {
    out.push_back(group.id.str());
  }
  return out;
}

bool is_sorted_unique(const std::vector<std::string>& values) {
  return std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end();
}

// ---------------------------------------------------------------------------
// Identity index
// ---------------------------------------------------------------------------

std::vector<ct::Node> identity_nodes() {
  return {source_node("source:fw"), plant_node("plant:p1"), plant_node("plant:p2"), loop_node("loop:l1"),
          pump_node("pump:p1")};
}

/// One alias that resolves, one whose target does not exist and one that points
/// at another alias: only the first is a resolvable secondary identity.
std::vector<ct::Alias> identity_aliases() {
  return {ct::Alias{aid("alias.legacy-plant"), nid("plant:p2")},
          ct::Alias{aid("alias.ghost"), nid("plant:missing")},
          ct::Alias{aid("alias.chain"), nid("alias.legacy-plant")}};
}

std::string resolved_text(const internal::NodeIndex& index, const std::vector<ct::Node>& nodes,
                          std::string_view identity) {
  const auto found = index.find(identity);
  return found.has_value() ? nodes[*found].id.str() : std::string("(absent)");
}

CT_TEST(internal_node_index_separates_node_identities_from_aliases) {
  const std::vector<ct::Node> nodes = identity_nodes();
  const std::vector<ct::Alias> aliases = identity_aliases();
  internal::NodeIndex index;
  index.build(nodes, aliases);

  CT_CHECK_EQ(index.node_count(), nodes.size());
  CT_CHECK_EQ(index.aliases().size(), aliases.size());

  const auto plant = index.find_node("plant:p2");
  CT_REQUIRE(plant.has_value());
  CT_CHECK_EQ(nodes[*plant].id.str(), std::string("plant:p2"));

  // The combined lookup resolves the canonical identity and the alias to the
  // same element; the alias-only lookup resolves it too.
  const auto through_alias = index.find("alias.legacy-plant");
  CT_REQUIRE(through_alias.has_value());
  CT_CHECK_EQ(*through_alias, *plant);
  const auto alias_only = index.find_alias("alias.legacy-plant");
  CT_REQUIRE(alias_only.has_value());
  CT_CHECK_EQ(*alias_only, *plant);

  // A node identity is not an alias, and an alias is not a node identity.
  CT_CHECK(!index.find_node("alias.legacy-plant").has_value());
  CT_CHECK(!index.find_alias("plant:p2").has_value());

  // An alias whose target is not a node, and an alias that targets another
  // alias, are not resolvable: an alias is never a chain.
  CT_CHECK(!index.find("alias.ghost").has_value());
  CT_CHECK(!index.find_alias("alias.ghost").has_value());
  CT_CHECK(!index.find("alias.chain").has_value());
  CT_CHECK(!index.find_alias("alias.chain").has_value());
  CT_CHECK(!index.find("(absent)").has_value());
  CT_CHECK(!index.find(std::string_view()).has_value());
  CT_CHECK(!index.find_node(std::string_view()).has_value());
  CT_CHECK(!index.find_alias(std::string_view()).has_value());
}

CT_TEST(internal_node_index_lookup_is_repeatable_and_order_independent) {
  const std::vector<ct::Node> nodes = identity_nodes();
  const std::vector<ct::Alias> aliases = identity_aliases();

  internal::NodeIndex first;
  first.build(nodes, aliases);
  internal::NodeIndex again;
  again.build(nodes, aliases);

  // The same table in the opposite order: the slot arrays are sorted, so which
  // identity an index denotes may not depend on the order the table was built
  // from. The raw slot number is only ever an index into the supplied table.
  const std::vector<ct::Node> reversed_nodes(nodes.rbegin(), nodes.rend());
  const std::vector<ct::Alias> reversed_aliases(aliases.rbegin(), aliases.rend());
  internal::NodeIndex reversed;
  reversed.build(reversed_nodes, reversed_aliases);

  const std::array<std::string_view, 6> spellings{"plant:p1", "plant:p2",  "loop:l1",
                                                  "pump:p1",  "source:fw", "alias.legacy-plant"};
  for (const std::string_view spelling : spellings) {
    CT_CHECK_EQ(resolved_text(again, nodes, spelling), resolved_text(first, nodes, spelling));
    CT_CHECK_EQ(resolved_text(reversed, reversed_nodes, spelling), resolved_text(first, nodes, spelling));
    CT_CHECK_EQ(again.find_node(spelling).has_value(), first.find_node(spelling).has_value());
    CT_CHECK_EQ(reversed.find_node(spelling).has_value(), first.find_node(spelling).has_value());
    CT_CHECK_EQ(reversed.find_alias(spelling).has_value(), first.find_alias(spelling).has_value());
  }

  // Every node is found, and exactly the aliases whose target is a node are
  // resolvable, in both orders.
  std::size_t resolvable = 0;
  for (const ct::Node& node : nodes) {
    CT_CHECK(first.find_node(node.id.value()).has_value());
    CT_CHECK(reversed.find_node(node.id.value()).has_value());
  }
  for (const ct::Alias& alias : aliases) {
    const auto combined = first.find(alias.id.value());
    const auto target = first.find_node(alias.target.value());
    if (target.has_value()) {
      ++resolvable;
      CT_CHECK(first.find_alias(alias.id.value()).has_value());
      CT_CHECK(reversed.find_alias(alias.id.value()).has_value());
      CT_REQUIRE(combined.has_value());
      CT_CHECK_EQ(*combined, *target);
    } else {
      CT_CHECK(!first.find_alias(alias.id.value()).has_value());
      CT_CHECK(!combined.has_value());
    }
  }
  CT_CHECK_EQ(resolvable, std::size_t(1));
}

// ---------------------------------------------------------------------------
// Cycle detection
// ---------------------------------------------------------------------------

CT_TEST(internal_find_cycle_accepts_the_supply_dag_of_the_reference_facility) {
  const ct::TopologyDraft draft = reference_facility();
  const Tables tables{draft.nodes, draft.edges, draft.aliases};
  const internal::GraphIndex graph = index_of(tables);
  CT_CHECK_EQ(graph.node_count(), tables.nodes.size());
  CT_CHECK_EQ(graph.edge_count(), tables.edges.size());

  std::vector<std::string> cycle;
  CT_CHECK(!internal::find_cycle(graph, kSupplies, cycle));
  CT_CHECK(cycle.empty());
  CT_CHECK(!internal::find_cycle(graph, kReturns, cycle));
  CT_CHECK(!internal::find_cycle(graph, kContains, cycle));
  CT_CHECK(!internal::find_cycle(graph, kDepends, cycle));
  CT_CHECK(!internal::find_cycle_union(graph, kDepends, kContains, cycle));
  CT_CHECK(cycle.empty());
}

CT_TEST(internal_find_cycle_reports_a_sorted_unique_cycle_set) {
  Tables tables;
  tables.nodes = {loop_node("loop:a"), loop_node("loop:b"), loop_node("loop:c"), loop_node("loop:tail")};
  tables.edges = {
      edge("e:a-b", ct::EdgeKind::Supplies, "loop:a", ct::PortRole::SupplyOut, "loop:b", ct::PortRole::SourceIn),
      edge("e:b-c", ct::EdgeKind::Supplies, "loop:b", ct::PortRole::SupplyOut, "loop:c", ct::PortRole::SourceIn),
      edge("e:tail-a", ct::EdgeKind::Supplies, "loop:tail", ct::PortRole::SupplyOut, "loop:a",
           ct::PortRole::SourceIn),
      edge("e:c-a", ct::EdgeKind::Supplies, "loop:c", ct::PortRole::SupplyOut, "loop:a", ct::PortRole::SourceIn),
  };
  const internal::GraphIndex graph = index_of(tables);

  std::vector<std::string> cycle;
  CT_REQUIRE(internal::find_cycle(graph, kSupplies, cycle));
  CT_CHECK(is_sorted_unique(cycle));
  CT_CHECK_EQ(cycle, (std::vector<std::string>{"e:a-b", "e:b-c", "e:c-a"}));

  // The same edge set supplied in the opposite order reports the same cycle:
  // the reported identities are the cycle's edges, not a traversal artifact.
  std::vector<ct::Edge> permuted = tables.edges;
  std::reverse(permuted.begin(), permuted.end());
  const Tables permuted_tables{tables.nodes, permuted, tables.aliases};
  const internal::GraphIndex permuted_graph = index_of(permuted_tables);
  std::vector<std::string> permuted_cycle;
  CT_REQUIRE(internal::find_cycle(permuted_graph, kSupplies, permuted_cycle));
  CT_CHECK(is_sorted_unique(permuted_cycle));
  CT_CHECK_EQ(permuted_cycle, cycle);

  // None of those edges is a return edge, so the return graph stays acyclic.
  std::vector<std::string> returns_cycle;
  CT_CHECK(!internal::find_cycle(graph, kReturns, returns_cycle));
  CT_CHECK(returns_cycle.empty());
}

CT_TEST(internal_find_cycle_detects_a_self_cycle_and_reports_none_without_edges) {
  Tables tables;
  tables.nodes = {loop_node("loop:self")};
  tables.edges = {edge("e:self", ct::EdgeKind::Supplies, "loop:self", ct::PortRole::SupplyOut, "loop:self",
                       ct::PortRole::SourceIn)};
  const internal::GraphIndex graph = index_of(tables);

  std::vector<std::string> cycle;
  CT_REQUIRE(internal::find_cycle(graph, kSupplies, cycle));
  CT_CHECK(is_sorted_unique(cycle));
  CT_CHECK_EQ(cycle, (std::vector<std::string>{"e:self"}));

  // A table with nodes but no edges has no cycle, and neither has an empty
  // table.
  const Tables no_edges{{loop_node("loop:a"), loop_node("loop:b")}, {}, {}};
  const internal::GraphIndex no_edges_graph = index_of(no_edges);
  cycle.clear();
  CT_CHECK(!internal::find_cycle(no_edges_graph, kSupplies, cycle));
  CT_CHECK(cycle.empty());

  const Tables empty{{}, {}, {}};
  const internal::GraphIndex empty_graph = index_of(empty);
  CT_CHECK(!internal::find_cycle(empty_graph, kSupplies, cycle));
  CT_CHECK(cycle.empty());

  // An edge that names an unknown node is a dead end, never a cycle.
  const Tables dangling{{loop_node("loop:a")},
                        {edge("e:dangling", ct::EdgeKind::Supplies, "loop:a", ct::PortRole::SupplyOut,
                              "loop:missing", ct::PortRole::SourceIn)},
                        {}};
  const internal::GraphIndex dangling_graph = index_of(dangling);
  CT_CHECK(!internal::find_cycle(dangling_graph, kSupplies, cycle));
}

/// Node identity of a chain position: "loop:n00000", "loop:n00001", ...
std::string chain_name(std::size_t index) {
  std::string name = "loop:n";
  char digits[6] = {'0', '0', '0', '0', '0', '\0'};
  for (int position = 4; position >= 0 && index != 0; --position) {
    digits[position] = static_cast<char>('0' + (index % 10));
    index /= 10;
  }
  name += digits;
  return name;
}

std::vector<ct::Node> chain_nodes(std::size_t count) {
  std::vector<ct::Node> nodes;
  nodes.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    nodes.push_back(loop_node(chain_name(index)));
  }
  return nodes;
}

std::vector<ct::Edge> chain_edges(std::size_t count, bool close_the_ring) {
  std::vector<ct::Edge> edges;
  edges.reserve(close_the_ring ? count : count - 1);
  for (std::size_t index = 0; index + 1 < count; ++index) {
    edges.push_back(edge("e:n" + std::to_string(index), ct::EdgeKind::Supplies, chain_name(index),
                         ct::PortRole::SupplyOut, chain_name(index + 1), ct::PortRole::SourceIn));
  }
  if (close_the_ring) {
    edges.push_back(edge("e:nclose", ct::EdgeKind::Supplies, chain_name(count - 1), ct::PortRole::SupplyOut,
                         chain_name(0), ct::PortRole::SourceIn));
  }
  return edges;
}

CT_TEST(internal_find_cycle_is_iterative_over_a_long_chain) {
  // 20000 nodes is deeper than any recursive search could descend on a default
  // thread stack. The search carries its own explicit stack, so it terminates by
  // construction; a hang, a stack overflow or a false positive here is a defect.
  constexpr std::size_t kChainLength = 20000;
  const Tables tables{chain_nodes(kChainLength), chain_edges(kChainLength, false), {}};
  const internal::GraphIndex graph = index_of(tables);
  CT_CHECK_EQ(graph.node_count(), kChainLength);
  CT_CHECK_EQ(graph.edge_count(), kChainLength - 1);

  std::vector<std::string> cycle;
  CT_CHECK(!internal::find_cycle(graph, kSupplies, cycle));
  CT_CHECK(cycle.empty());

  // Closing the chain into one ring must be found, and the reported set is every
  // edge of the ring.
  const Tables ring{chain_nodes(kChainLength), chain_edges(kChainLength, true), {}};
  const internal::GraphIndex ring_graph = index_of(ring);
  CT_CHECK_EQ(ring_graph.edge_count(), kChainLength);
  CT_REQUIRE(internal::find_cycle(ring_graph, kSupplies, cycle));
  CT_CHECK_EQ(cycle.size(), kChainLength);
  CT_CHECK(is_sorted_unique(cycle));
  CT_CHECK(std::binary_search(cycle.begin(), cycle.end(), std::string("e:n0")));
  CT_CHECK(std::binary_search(cycle.begin(), cycle.end(), std::string("e:nclose")));
}

// ---------------------------------------------------------------------------
// Bounded traversal
// ---------------------------------------------------------------------------

/// A supply graph whose levels are known by inspection:
///
///   a -> b -> x          x is also fed directly by a, so its shortest distance
///   a -> c -> z          is one and not two;
///   a -> x               y is fed by both b and c, so which edge reached it
///   b -> y               shows that a level is expanded in ascending node order
///   c -> y               even though the edge table lists c's edge first.
Tables traversal_tables() {
  Tables tables;
  tables.nodes = {loop_node("loop:a"), loop_node("loop:b"), loop_node("loop:c"), loop_node("loop:x"),
                  loop_node("loop:y"), loop_node("loop:z")};
  tables.edges = {
      edge("e:a-b", ct::EdgeKind::Supplies, "loop:a", ct::PortRole::SupplyOut, "loop:b", ct::PortRole::SourceIn),
      edge("e:a-c", ct::EdgeKind::Supplies, "loop:a", ct::PortRole::SupplyOut, "loop:c", ct::PortRole::SourceIn),
      edge("e:a-x", ct::EdgeKind::Supplies, "loop:a", ct::PortRole::SupplyOut, "loop:x", ct::PortRole::SourceIn),
      edge("e:c-z", ct::EdgeKind::Supplies, "loop:c", ct::PortRole::SupplyOut, "loop:z", ct::PortRole::SourceIn),
      edge("e:b-x", ct::EdgeKind::Supplies, "loop:b", ct::PortRole::SupplyOut, "loop:x", ct::PortRole::SourceIn),
      edge("e:b-y", ct::EdgeKind::Supplies, "loop:b", ct::PortRole::SupplyOut, "loop:y", ct::PortRole::SourceIn),
      edge("e:c-y", ct::EdgeKind::Supplies, "loop:c", ct::PortRole::SupplyOut, "loop:y", ct::PortRole::SourceIn),
  };
  return tables;
}

std::uint32_t depth_of(const internal::TraversalResult& result, std::uint32_t node) {
  const auto found = std::find(result.visited.begin(), result.visited.end(), node);
  CT_REQUIRE(found != result.visited.end());
  return result.depth[static_cast<std::size_t>(found - result.visited.begin())];
}

CT_TEST(internal_traverse_reports_shortest_distance_levels_and_bounds) {
  const Tables tables = traversal_tables();
  const internal::GraphIndex graph = index_of(tables);
  const std::uint32_t a = node_at(graph, "loop:a");
  const std::uint32_t b = node_at(graph, "loop:b");
  const std::uint32_t c = node_at(graph, "loop:c");
  const std::uint32_t x = node_at(graph, "loop:x");
  const std::uint32_t y = node_at(graph, "loop:y");
  const std::uint32_t z = node_at(graph, "loop:z");

  const internal::TraversalResult full = internal::traverse(graph, a, kSupplies, false, 10, 100);
  CT_CHECK(!full.truncated);
  CT_CHECK_EQ(full.visited, (std::vector<std::uint32_t>{b, c, x, y, z}));
  CT_CHECK_EQ(full.depth.size(), full.visited.size());
  CT_CHECK_EQ(full.via_edge.size(), full.visited.size());
  // The origin is never a member of its own reachable set.
  CT_CHECK(std::find(full.visited.begin(), full.visited.end(), a) == full.visited.end());

  // Depth is the shortest distance: x is one edge from a even though b also
  // reaches it one level deeper.
  CT_CHECK_EQ(depth_of(full, b), std::uint32_t(1));
  CT_CHECK_EQ(depth_of(full, c), std::uint32_t(1));
  CT_CHECK_EQ(depth_of(full, x), std::uint32_t(1));
  CT_CHECK_EQ(depth_of(full, y), std::uint32_t(2));
  CT_CHECK_EQ(depth_of(full, z), std::uint32_t(2));

  // A level is a contiguous run of ascending node indices, and a node one level
  // deeper hangs off a node one level above it.
  for (std::size_t index = 1; index < full.visited.size(); ++index) {
    CT_CHECK(full.depth[index - 1] <= full.depth[index]);
    if (full.depth[index - 1] == full.depth[index]) {
      CT_CHECK(full.visited[index - 1] < full.visited[index]);
    }
  }
  for (std::size_t index = 0; index < full.visited.size(); ++index) {
    const std::uint32_t edge_index = full.via_edge[index];
    CT_REQUIRE(edge_index < graph.edge_count());
    CT_CHECK_EQ(graph.to_of(edge_index), full.visited[index]);
    if (full.depth[index] > 1) {
      CT_CHECK_EQ(depth_of(full, graph.from_of(edge_index)), full.depth[index] - 1);
    } else {
      CT_CHECK_EQ(graph.from_of(edge_index), a);
    }
  }
  // The level is expanded in ascending node order: y is reached from b, whose
  // edge the table lists after c's edge to the same node.
  CT_CHECK_EQ(full.via_edge[3], static_cast<std::uint32_t>(edge_at(graph, "e:b-y")));
  CT_CHECK_EQ(full.depth[3], std::uint32_t(2));

  // max_depth bounds the search and reports that it did so.
  const internal::TraversalResult shallow = internal::traverse(graph, a, kSupplies, false, 1, 100);
  CT_CHECK(shallow.truncated);
  CT_CHECK_EQ(shallow.visited, (std::vector<std::uint32_t>{b, c, x}));

  // Two levels are reached, but the frontier still holds the second level, so
  // the search cannot claim it is complete and says so.
  const internal::TraversalResult two_levels = internal::traverse(graph, a, kSupplies, false, 2, 100);
  CT_CHECK(two_levels.truncated);
  CT_CHECK_EQ(two_levels.visited, full.visited);

  const internal::TraversalResult three_levels = internal::traverse(graph, a, kSupplies, false, 3, 100);
  CT_CHECK(!three_levels.truncated);
  CT_CHECK_EQ(three_levels.visited, full.visited);

  const internal::TraversalResult none = internal::traverse(graph, a, kSupplies, false, 0, 100);
  CT_CHECK(none.truncated);
  CT_CHECK(none.visited.empty());

  // max_nodes bounds the number of results, not the depth.
  const internal::TraversalResult limited = internal::traverse(graph, a, kSupplies, false, 10, 2);
  CT_CHECK(limited.truncated);
  CT_CHECK_EQ(limited.visited.size(), std::size_t(2));

  // Following incoming edges answers the mirror question with the same rules.
  const internal::TraversalResult upstream = internal::traverse(graph, z, kSupplies, true, 10, 100);
  CT_CHECK(!upstream.truncated);
  CT_CHECK_EQ(upstream.visited, (std::vector<std::uint32_t>{c, a}));
  CT_CHECK_EQ(depth_of(upstream, c), std::uint32_t(1));
  CT_CHECK_EQ(depth_of(upstream, a), std::uint32_t(2));

  // An origin outside the table is refused, not dereferenced.
  const internal::TraversalResult absent = internal::traverse(graph, internal::kNoNode, kSupplies, false, 10, 100);
  CT_CHECK(absent.truncated);
  CT_CHECK(absent.visited.empty());
}

// ---------------------------------------------------------------------------
// Structural sources
// ---------------------------------------------------------------------------

/// A fed path, an isolated loop, a pump on each, a pump with no installation
/// site, and two elements attached to a fed manifold only by containment.
Tables source_tables() {
  Tables tables;
  tables.nodes = {source_node("source:fw"), plant_node("plant:p1"), loop_node("loop:l1"), loop_node("loop:l2"),
                  manifold_node("manifold:m1"), branch_node("branch:b1"), pump_node("pump:p1"),
                  pump_node("pump:p2"), pump_node("pump:p3"), crac_node("crac:c1"), crah_node("crah:void")};
  tables.edges = {
      edge("e:src-plant", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut, "plant:p1",
           ct::PortRole::SourceIn),
      edge("e:plant-loop", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut, "loop:l1",
           ct::PortRole::SourceIn),
      edge("e:loop-manifold", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "manifold:m1",
           ct::PortRole::SourceIn),
      edge("e:manifold-branch", ct::EdgeKind::Contains, "manifold:m1", ct::PortRole::Container, "branch:b1",
           ct::PortRole::Contained),
      edge("e:manifold-crac", ct::EdgeKind::Contains, "manifold:m1", ct::PortRole::Container, "crac:c1",
           ct::PortRole::Contained),
      edge("e:pump-1", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut, "pump:p1",
           ct::PortRole::Terminal),
      edge("e:pump-3", ct::EdgeKind::Pumps, "loop:l2", ct::PortRole::SupplyOut, "pump:p3",
           ct::PortRole::Terminal),
  };
  return tables;
}

CT_TEST(internal_structural_sources_and_member_source_sets) {
  const Tables tables = source_tables();
  const internal::GraphIndex graph = index_of(tables);
  const std::uint32_t source = node_at(graph, "source:fw");
  const std::uint32_t plant = node_at(graph, "plant:p1");
  const std::uint32_t loop = node_at(graph, "loop:l1");
  const std::uint32_t isolated = node_at(graph, "loop:l2");
  const std::uint32_t manifold = node_at(graph, "manifold:m1");
  const std::uint32_t branch = node_at(graph, "branch:b1");
  const std::uint32_t pump = node_at(graph, "pump:p1");
  const std::uint32_t pump_without_host = node_at(graph, "pump:p2");
  const std::uint32_t pump_on_isolated_loop = node_at(graph, "pump:p3");
  const std::uint32_t contained = node_at(graph, "crac:c1");
  const std::uint32_t disconnected = node_at(graph, "crah:void");

  CT_CHECK(internal::is_origin_kind(ct::NodeKind::CoolingSource));
  CT_CHECK(internal::is_origin_kind(ct::NodeKind::CoolingPlant));
  CT_CHECK(!internal::is_origin_kind(ct::NodeKind::CoolingLoop));
  CT_CHECK(!internal::is_origin_kind(ct::NodeKind::Pump));

  const internal::SourceSet loop_sources = internal::structural_sources_reaching(graph, loop, 1024);
  CT_CHECK(!loop_sources.truncated);
  CT_CHECK_EQ(loop_sources.sources, (std::vector<std::uint32_t>{source}));
  CT_CHECK_EQ(internal::structural_sources_reaching(graph, manifold, 1024).sources,
              (std::vector<std::uint32_t>{source}));
  // The intermediate plant is fed, so it is a dependency and not an origin.
  CT_CHECK_EQ(internal::structural_sources_reaching(graph, plant, 1024).sources,
              (std::vector<std::uint32_t>{source}));

  // A pump's sources are the sources of the element it is installed on.
  const internal::SourceSet pump_sources = internal::member_source_set(graph, pump, 1024);
  CT_CHECK(!pump_sources.truncated);
  CT_CHECK_EQ(pump_sources.sources, loop_sources.sources);
  CT_CHECK_EQ(internal::member_source_set(graph, loop, 1024).sources, loop_sources.sources);

  // A pump with no installation site has no sources at all.
  CT_CHECK(internal::member_source_set(graph, pump_without_host, 1024).sources.empty());

  // A distribution element that declares no feed is its own structural origin,
  // because a loop kind may deliver medium; a pump installed on it therefore
  // inherits exactly that set and not an empty one.
  CT_CHECK_EQ(internal::structural_sources_reaching(graph, isolated, 1024).sources,
              (std::vector<std::uint32_t>{isolated}));
  CT_CHECK_EQ(internal::member_source_set(graph, isolated, 1024).sources,
              (std::vector<std::uint32_t>{isolated}));
  CT_CHECK_EQ(internal::member_source_set(graph, pump_on_isolated_loop, 1024).sources,
              (std::vector<std::uint32_t>{isolated}));

  // Containment is not a cooling relation: an element enclosed by a fed
  // manifold is not thereby fed. A branch that declares no feed is its own
  // structural origin only because a branch kind may deliver medium.
  CT_CHECK(internal::structural_sources_reaching(graph, contained, 1024).sources.empty());
  CT_CHECK(internal::member_source_set(graph, contained, 1024).sources.empty());

  // An element that declares no connection of any kind and cannot deliver medium
  // has no structural source.
  CT_CHECK(internal::structural_sources_reaching(graph, disconnected, 1024).sources.empty());
  CT_CHECK(internal::member_source_set(graph, disconnected, 1024).sources.empty());
  CT_CHECK(graph.in_edges(disconnected).empty());
  CT_CHECK(graph.out_edges(disconnected).empty());
  CT_CHECK_EQ(internal::structural_sources_reaching(graph, branch, 1024).sources,
              (std::vector<std::uint32_t>{branch}));

  // Every supplier kind with no incoming supplies edge is an origin: the fed
  // plant, loop and manifold are not, but the unfed loop and branch are.
  CT_CHECK_EQ(internal::origin_nodes(graph), (std::vector<std::uint32_t>{source, isolated, branch}));

  // An identity outside the table is answered, not dereferenced.
  CT_CHECK(internal::structural_sources_reaching(graph, internal::kNoNode, 1024).sources.empty());
  CT_CHECK(internal::member_source_set(graph, internal::kNoNode, 1024).sources.empty());
}

// ---------------------------------------------------------------------------
// Canonical ordering
// ---------------------------------------------------------------------------

CT_TEST(internal_canonical_order_sorts_every_table_and_matches_validation) {
  const ct::TopologyDraft draft = reference_facility();
  const auto ordered = ct::canonical_order(draft);
  CT_REQUIRE(ordered.has_value());
  CT_CHECK(is_sorted_unique(identity_texts(ordered->nodes)));
  CT_CHECK(is_sorted_unique(edge_texts(ordered->edges)));
  CT_CHECK(is_sorted_unique(alias_texts(ordered->aliases)));
  CT_CHECK(is_sorted_unique(group_texts(ordered->groups)));

  // The internal validation entry point returns exactly the tables the
  // canonical order returns.
  const auto validated = internal::validate_and_order(draft);
  CT_REQUIRE(validated.has_value());
  CT_CHECK_EQ(identity_texts(validated->nodes), identity_texts(ordered->nodes));
  CT_CHECK_EQ(edge_texts(validated->edges), edge_texts(ordered->edges));
  CT_CHECK_EQ(alias_texts(validated->aliases), alias_texts(ordered->aliases));
  CT_CHECK_EQ(group_texts(validated->groups), group_texts(ordered->groups));
  CT_CHECK_EQ(validated->changeovers.size(), ordered->changeovers.size());
}

CT_TEST(internal_canonical_order_rejects_duplicate_and_empty_identities) {
  // A draft small enough to state every rejection against one base: order is
  // never the document's, and a repeated identity in any table is refused
  // instead of being silently merged.
  ct::TopologyDraft draft = base_draft();
  draft.nodes = {loop_node("loop:l1"), source_node("source:fw"), pump_node("pump:p1")};
  draft.edges = {
      edge("e:b", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut, "loop:l1",
           ct::PortRole::SourceIn),
      edge("e:a", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut, "pump:p1", ct::PortRole::Terminal),
  };
  draft.aliases = {ct::Alias{aid("alias.z"), nid("loop:l1")}, ct::Alias{aid("alias.a"), nid("pump:p1")}};
  auto group_z = ct::RedundancyGroup::create(
      gid("group:z"), ct::RedundancyScheme::NPlusOne, ct::RedundancyScope::Pump, "pumps", "declared",
      {ct::RedundancyMember{nid("pump:p1"), "pump:p1", std::nullopt}}, false, false);
  CT_REQUIRE(group_z.has_value());
  auto group_a = ct::RedundancyGroup::create(
      gid("group:a"), ct::RedundancyScheme::NPlusOne, ct::RedundancyScope::Pump, "pumps", "declared",
      {ct::RedundancyMember{nid("pump:p1"), "pump:p1", std::nullopt}}, false, false);
  CT_REQUIRE(group_a.has_value());
  draft.groups = {*group_z, *group_a};
  auto changeover = ct::ChangeoverGroup::create(
      cid("changeover:x"), "declared",
      {ct::Endpoint{nid("loop:l1"), ct::PortRole::SourceIn}, ct::Endpoint{nid("pump:p1"), ct::PortRole::Terminal}},
      1);
  CT_REQUIRE(changeover.has_value());
  draft.changeovers = {*changeover};

  const auto ordered = ct::canonical_order(draft);
  CT_REQUIRE(ordered.has_value());
  CT_CHECK_EQ(identity_texts(ordered->nodes), (std::vector<std::string>{"loop:l1", "pump:p1", "source:fw"}));
  CT_CHECK_EQ(edge_texts(ordered->edges), (std::vector<std::string>{"e:a", "e:b"}));
  CT_CHECK_EQ(alias_texts(ordered->aliases), (std::vector<std::string>{"alias.a", "alias.z"}));
  CT_CHECK_EQ(group_texts(ordered->groups), (std::vector<std::string>{"group:a", "group:z"}));

  ct::TopologyDraft duplicate_nodes = draft;
  duplicate_nodes.nodes.push_back(loop_node("loop:l1"));
  auto rejected_nodes = ct::canonical_order(duplicate_nodes);
  CT_REQUIRE(!rejected_nodes.has_value());
  expect_code(rejected_nodes.error(), ct::ErrorCode::DuplicateIdentifier, "duplicate node identity");

  ct::TopologyDraft duplicate_edges = draft;
  duplicate_edges.edges.push_back(edge("e:a", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut,
                                       "loop:l1", ct::PortRole::SourceIn));
  auto rejected_edges = ct::canonical_order(duplicate_edges);
  CT_REQUIRE(!rejected_edges.has_value());
  expect_code(rejected_edges.error(), ct::ErrorCode::DuplicateIdentifier, "duplicate edge identity");

  ct::TopologyDraft duplicate_aliases = draft;
  duplicate_aliases.aliases.push_back(ct::Alias{aid("alias.a"), nid("loop:l1")});
  auto rejected_aliases = ct::canonical_order(duplicate_aliases);
  CT_REQUIRE(!rejected_aliases.has_value());
  expect_code(rejected_aliases.error(), ct::ErrorCode::DuplicateIdentifier, "duplicate alias identity");

  ct::TopologyDraft duplicate_groups = draft;
  duplicate_groups.groups.push_back(*group_a);
  auto rejected_groups = ct::canonical_order(duplicate_groups);
  CT_REQUIRE(!rejected_groups.has_value());
  expect_code(rejected_groups.error(), ct::ErrorCode::DuplicateIdentifier, "duplicate group identity");

  ct::TopologyDraft duplicate_changeovers = draft;
  duplicate_changeovers.changeovers = {*changeover, *changeover};
  auto rejected_changeovers = ct::canonical_order(duplicate_changeovers);
  CT_REQUIRE(!rejected_changeovers.has_value());
  expect_code(rejected_changeovers.error(), ct::ErrorCode::DuplicateIdentifier, "duplicate changeover identity");

  // An empty identity can only be constructed directly, and it is refused
  // before any table is sorted.
  ct::TopologyDraft empty_node_draft;
  ct::Node blank;
  blank.attributes = ct::PumpAttributes{};
  empty_node_draft.nodes = {blank};
  auto empty_node = ct::canonical_order(empty_node_draft);
  CT_REQUIRE(!empty_node.has_value());
  expect_code(empty_node.error(), ct::ErrorCode::MalformedIdentifier, "empty node identity");

  ct::TopologyDraft empty_edge_draft = draft;
  ct::Edge no_identity;
  empty_edge_draft.edges = {no_identity};
  auto empty_edge = ct::canonical_order(empty_edge_draft);
  CT_REQUIRE(!empty_edge.has_value());
  expect_code(empty_edge.error(), ct::ErrorCode::MalformedIdentifier, "empty edge identity");

  ct::TopologyDraft empty_group_draft = draft;
  ct::RedundancyGroup no_group_identity;
  empty_group_draft.groups = {no_group_identity};
  auto empty_group = ct::canonical_order(empty_group_draft);
  CT_REQUIRE(!empty_group.has_value());
  expect_code(empty_group.error(), ct::ErrorCode::MalformedIdentifier, "empty group identity");
}

// ---------------------------------------------------------------------------
// Generation file frame
// ---------------------------------------------------------------------------

void put_u16(std::string& bytes, std::size_t offset, std::uint16_t value) {
  for (int index = 0; index < 2; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] = static_cast<char>((value >> (8 * index)) & 0xFFu);
  }
}

void put_u64(std::string& bytes, std::size_t offset, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] = static_cast<char>((value >> (8 * index)) & 0xFFu);
  }
}

CT_TEST(internal_generation_frame_round_trips_and_rejects_each_defect) {
  const ct::Topology topology = ct_test::build(reference_facility());
  auto payload_result = topology.canonical_bytes();
  CT_REQUIRE(payload_result.has_value());
  const std::string payload = *payload_result;

  auto framed_result = ct::encode_generation_file(payload);
  CT_REQUIRE(framed_result.has_value());
  const std::string framed = *framed_result;
  constexpr std::size_t kFrameHeaderBytes = 8 + 2 + 2 + 8;
  CT_CHECK_EQ(framed.size(), payload.size() + kFrameHeaderBytes + ct::Digest::kBytes);
  CT_CHECK_EQ(std::string(framed.substr(0, ct::kGenerationFileMagic.size())), std::string(ct::kGenerationFileMagic));

  const auto decoded = ct::decode_generation_file(framed);
  CT_REQUIRE(decoded.has_value());
  CT_CHECK_EQ(decoded->payload, payload);
  CT_CHECK_EQ(decoded->schema_version, ct::kCanonicalSchemaVersion);
  CT_CHECK(decoded->payload_digest == ct::digest_bytes(payload));
  CT_CHECK(!decoded->payload_digest.is_zero());

  // The encoder refuses the one payload it cannot frame.
  auto empty_payload = ct::encode_generation_file(std::string_view());
  CT_REQUIRE(!empty_payload.has_value());
  expect_code(empty_payload.error(), ct::ErrorCode::EmptyInput, "empty payload");

  std::string wrong_magic = framed;
  wrong_magic[0] = 'X';
  auto magic = ct::decode_generation_file(wrong_magic);
  CT_REQUIRE(!magic.has_value());
  expect_code(magic.error(), ct::ErrorCode::MalformedRecord, "wrong magic");

  std::string wrong_schema = framed;
  put_u16(wrong_schema, 8, static_cast<std::uint16_t>(ct::kCanonicalSchemaVersion + 1));
  auto schema = ct::decode_generation_file(wrong_schema);
  CT_REQUIRE(!schema.has_value());
  expect_code(schema.error(), ct::ErrorCode::UnsupportedSchemaVersion, "unsupported schema version");

  std::string reserved_set = framed;
  put_u16(reserved_set, 10, 1);
  auto reserved = ct::decode_generation_file(reserved_set);
  CT_REQUIRE(!reserved.has_value());
  expect_code(reserved.error(), ct::ErrorCode::MalformedRecord, "non-zero reserved field");

  std::string longer_payload = framed;
  put_u64(longer_payload, 12, static_cast<std::uint64_t>(payload.size()) + 1);
  auto longer = ct::decode_generation_file(longer_payload);
  CT_REQUIRE(!longer.has_value());
  expect_code(longer.error(), ct::ErrorCode::CountMismatch, "declared payload longer than the frame");

  std::string shorter_payload = framed;
  put_u64(shorter_payload, 12, static_cast<std::uint64_t>(payload.size()) - 1);
  auto shorter = ct::decode_generation_file(shorter_payload);
  CT_REQUIRE(!shorter.has_value());
  expect_code(shorter.error(), ct::ErrorCode::CountMismatch, "declared payload shorter than the frame");

  auto truncated_header = ct::decode_generation_file(framed.substr(0, kFrameHeaderBytes + ct::Digest::kBytes - 1));
  CT_REQUIRE(!truncated_header.has_value());
  expect_code(truncated_header.error(), ct::ErrorCode::TruncatedInput, "frame shorter than its fixed header");

  auto truncated_payload = ct::decode_generation_file(framed.substr(0, framed.size() - 4));
  CT_REQUIRE(!truncated_payload.has_value());
  expect_code(truncated_payload.error(), ct::ErrorCode::CountMismatch, "frame cut inside the payload");

  auto empty_input = ct::decode_generation_file(std::string_view());
  CT_REQUIRE(!empty_input.has_value());
  expect_code(empty_input.error(), ct::ErrorCode::TruncatedInput, "empty input");

  auto trailing = ct::decode_generation_file(framed + "trailing bytes");
  CT_REQUIRE(!trailing.has_value());
  expect_code(trailing.error(), ct::ErrorCode::CountMismatch, "trailing bytes");

  std::string edited_payload = framed;
  const std::size_t last_payload_byte = edited_payload.size() - ct::Digest::kBytes - 1;
  edited_payload[last_payload_byte] = static_cast<char>(edited_payload[last_payload_byte] ^ 0x01);
  auto digest = ct::decode_generation_file(edited_payload);
  CT_REQUIRE(!digest.has_value());
  expect_code(digest.error(), ct::ErrorCode::DigestMismatch, "payload edited under its digest");
}

}  // namespace
