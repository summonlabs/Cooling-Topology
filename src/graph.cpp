// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "graph_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dccp::cooling_topology::internal {
namespace {

std::uint32_t kind_bit(EdgeKind kind) noexcept { return 1u << static_cast<unsigned>(kind); }

/// Iterative depth-first cycle search over the edges whose kind bit is set.
/// Deterministic: start nodes are visited in ascending index order and each
/// node's outgoing edges are visited in the order the adjacency lists hold (the
/// order of the edge table, which the caller has already made canonical).
bool find_cycle_mask(const GraphIndex& graph, std::uint32_t kind_mask,
                     std::vector<std::string>& cycle_edges) {
  const std::size_t count = graph.node_count();
  if (count == 0) {
    return false;
  }
  const std::vector<Edge>& edges = graph.edges();
  std::vector<std::uint8_t> color(count, 0);
  std::vector<std::uint32_t> parent_edge(count, kNoNode);
  std::vector<std::pair<std::uint32_t, std::size_t>> stack;
  stack.reserve(64);

  for (std::uint32_t start = 0; start < count; ++start) {
    if (color[start] != 0) {
      continue;
    }
    color[start] = 1;
    stack.clear();
    stack.emplace_back(start, 0);
    while (!stack.empty()) {
      const std::uint32_t node = stack.back().first;
      std::size_t cursor = stack.back().second;
      const std::span<const std::uint32_t> outgoing = graph.out_edges(node);
      bool descended = false;
      while (cursor < outgoing.size()) {
        const std::uint32_t edge_index = outgoing[cursor];
        ++cursor;
        if ((kind_mask & kind_bit(edges[edge_index].kind)) == 0) {
          continue;
        }
        const std::uint32_t target = graph.to_of(edge_index);
        if (target == kNoNode) {
          continue;
        }
        if (color[target] == 1) {
          // The target is on the current stack: the parent chain from the
          // target back to the current node closes a cycle.
          stack.back().second = cursor;
          std::vector<std::string> collected;
          collected.push_back(edges[edge_index].id.str());
          for (std::size_t index = stack.size(); index-- > 0;) {
            const std::uint32_t on_stack = stack[index].first;
            if (on_stack == target) {
              break;
            }
            const std::uint32_t via = parent_edge[on_stack];
            if (via != kNoNode) {
              collected.push_back(edges[via].id.str());
            }
          }
          std::sort(collected.begin(), collected.end());
          collected.erase(std::unique(collected.begin(), collected.end()), collected.end());
          cycle_edges = std::move(collected);
          return true;
        }
        if (color[target] == 0) {
          parent_edge[target] = edge_index;
          color[target] = 1;
          stack.back().second = cursor;
          stack.emplace_back(target, 0);
          descended = true;
          break;
        }
      }
      if (descended) {
        continue;
      }
      stack.back().second = cursor;
      color[node] = 2;
      stack.pop_back();
    }
  }
  return false;
}

std::uint32_t mask_of(std::span<const EdgeKind> kinds) noexcept {
  std::uint32_t mask = 0;
  for (const EdgeKind kind : kinds) {
    mask |= kind_bit(kind);
  }
  return mask;
}

std::span<const std::uint32_t> slice(const std::vector<std::uint32_t>& flat,
                                     const std::vector<std::size_t>& offset, std::uint32_t node) noexcept {
  if (static_cast<std::size_t>(node) + 1 >= offset.size()) {
    return {};
  }
  const std::size_t begin = offset[node];
  const std::size_t end = offset[static_cast<std::size_t>(node) + 1];
  return std::span<const std::uint32_t>(flat.data() + begin, end - begin);
}

}  // namespace

void NodeIndex::build(const std::vector<Node>& nodes, const std::vector<Alias>& aliases) {
  node_count_ = nodes.size();
  aliases_ = std::span<const Alias>(aliases.data(), aliases.size());

  node_slots_.clear();
  node_slots_.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    node_slots_.emplace_back(nodes[index].id.value(), static_cast<std::uint32_t>(index));
  }
  std::sort(node_slots_.begin(), node_slots_.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first < rhs.first;
    }
    return lhs.second < rhs.second;
  });

  alias_slots_.clear();
  alias_slots_.reserve(aliases.size());
  for (std::size_t index = 0; index < aliases.size(); ++index) {
    Alias alias = aliases[index];
    const auto target = find_node(alias.target.value());
    if (target.has_value()) {
      alias_slots_.emplace_back(aliases[index].id.value(), *target);
    }
  }
  std::sort(alias_slots_.begin(), alias_slots_.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first < rhs.first;
    }
    return lhs.second < rhs.second;
  });
}

std::optional<std::uint32_t> NodeIndex::find_node(std::string_view identity) const noexcept {
  const auto it = std::lower_bound(node_slots_.begin(), node_slots_.end(), identity,
                                   [](const auto& slot, std::string_view value) { return slot.first < value; });
  if (it == node_slots_.end() || it->first != identity) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::uint32_t> NodeIndex::find_alias(std::string_view identity) const noexcept {
  const auto it = std::lower_bound(alias_slots_.begin(), alias_slots_.end(), identity,
                                   [](const auto& slot, std::string_view value) { return slot.first < value; });
  if (it == alias_slots_.end() || it->first != identity) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::uint32_t> NodeIndex::find(std::string_view identity) const noexcept {
  if (const auto node = find_node(identity)) {
    return node;
  }
  return find_alias(identity);
}

void GraphIndex::build(const std::vector<Node>& nodes, const std::vector<Edge>& edges,
                       const std::vector<Alias>& aliases) {
  node_count_ = nodes.size();
  nodes_ = &nodes;
  edges_ = &edges;
  identities_.build(nodes, aliases);

  from_.assign(edges.size(), kNoNode);
  to_.assign(edges.size(), kNoNode);
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (const auto resolved = identities_.find(edges[index].from.node.value())) {
      from_[index] = *resolved;
    }
    if (const auto resolved = identities_.find(edges[index].to.node.value())) {
      to_[index] = *resolved;
    }
  }

  std::vector<std::uint32_t> out_counts(node_count_ + 1, 0);
  std::vector<std::uint32_t> in_counts(node_count_ + 1, 0);
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (from_[index] != kNoNode) {
      ++out_counts[from_[index] + 1];
    }
    if (to_[index] != kNoNode) {
      ++in_counts[to_[index] + 1];
    }
  }
  for (std::size_t index = 1; index < out_counts.size(); ++index) {
    out_counts[index] += out_counts[index - 1];
    in_counts[index] += in_counts[index - 1];
  }
  out_offset_.assign(out_counts.begin(), out_counts.end());
  in_offset_.assign(in_counts.begin(), in_counts.end());
  out_flat_.assign(edges.size(), 0);
  in_flat_.assign(edges.size(), 0);
  std::vector<std::size_t> out_cursor(out_offset_.begin(), out_offset_.end() - 1);
  std::vector<std::size_t> in_cursor(in_offset_.begin(), in_offset_.end() - 1);
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (from_[index] != kNoNode) {
      out_flat_[out_cursor[from_[index]]++] = static_cast<std::uint32_t>(index);
    }
    if (to_[index] != kNoNode) {
      in_flat_[in_cursor[to_[index]]++] = static_cast<std::uint32_t>(index);
    }
  }
}

std::span<const std::uint32_t> GraphIndex::out_edges(std::uint32_t node) const noexcept {
  return slice(out_flat_, out_offset_, node);
}

std::span<const std::uint32_t> GraphIndex::in_edges(std::uint32_t node) const noexcept {
  return slice(in_flat_, in_offset_, node);
}

bool is_origin_kind(NodeKind kind) noexcept {
  return kind == NodeKind::CoolingSource || kind == NodeKind::CoolingPlant;
}

bool find_cycle(const GraphIndex& graph, std::span<const EdgeKind> kinds,
                std::vector<std::string>& cycle_edges) {
  return find_cycle_mask(graph, mask_of(kinds), cycle_edges);
}

bool find_cycle_union(const GraphIndex& graph, std::span<const EdgeKind> first, std::span<const EdgeKind> second,
                      std::vector<std::string>& cycle_edges) {
  return find_cycle_mask(graph, mask_of(first) | mask_of(second), cycle_edges);
}

TraversalResult traverse(const GraphIndex& graph, std::uint32_t origin, std::span<const EdgeKind> kinds,
                         bool follow_in, std::size_t max_depth, std::size_t max_nodes) {
  TraversalResult result;
  if (origin == kNoNode || origin >= graph.node_count()) {
    result.truncated = true;
    return result;
  }
  const std::uint32_t mask = mask_of(kinds);
  const std::vector<Edge>& edges = graph.edges();
  std::vector<std::uint8_t> seen(graph.node_count(), 0);
  seen[origin] = 1;
  std::vector<std::uint32_t> frontier;
  frontier.push_back(origin);
  std::vector<std::uint32_t> next;
  for (std::size_t depth = 0; depth < max_depth && !frontier.empty(); ++depth) {
    next.clear();
    for (const std::uint32_t node : frontier) {
      const std::span<const std::uint32_t> adjacent =
          follow_in ? graph.in_edges(node) : graph.out_edges(node);
      for (const std::uint32_t edge_index : adjacent) {
        if ((mask & kind_bit(edges[edge_index].kind)) == 0) {
          continue;
        }
        const std::uint32_t other =
            follow_in ? graph.from_of(edge_index) : graph.to_of(edge_index);
        if (other == kNoNode || seen[other] != 0) {
          continue;
        }
        if (result.visited.size() >= max_nodes) {
          result.truncated = true;
          return result;
        }
        seen[other] = 1;
        result.visited.push_back(other);
        result.depth.push_back(static_cast<std::uint32_t>(depth + 1));
        result.via_edge.push_back(edge_index);
        next.push_back(other);
      }
    }
    std::sort(next.begin(), next.end());
    frontier.swap(next);
  }
  if (!frontier.empty()) {
    result.truncated = true;
  }
  return result;
}

SourceSet structural_sources_reaching(const GraphIndex& graph, std::uint32_t node, std::size_t max_nodes) {
  SourceSet result;
  if (node == kNoNode || node >= graph.node_count()) {
    return result;
  }
  const std::vector<Edge>& edges = graph.edges();
  const std::vector<Node>& nodes = graph.nodes();
  std::vector<std::uint8_t> seen(graph.node_count(), 0);
  std::vector<std::uint32_t> stack;
  seen[node] = 1;
  stack.push_back(node);
  std::size_t expanded = 0;
  while (!stack.empty()) {
    const std::uint32_t current = stack.back();
    stack.pop_back();
    if (++expanded > max_nodes) {
      result.truncated = true;
      break;
    }
    const NodeKind kind = nodes[current].kind();
    if (is_supplier_kind(kind)) {
      bool fed = false;
      for (const std::uint32_t edge_index : graph.in_edges(current)) {
        if (edges[edge_index].kind == EdgeKind::Supplies) {
          fed = true;
          break;
        }
      }
      if (!fed) {
        result.sources.push_back(current);
      }
    }
    for (const std::uint32_t edge_index : graph.in_edges(current)) {
      if (edges[edge_index].kind != EdgeKind::Supplies) {
        continue;
      }
      const std::uint32_t upstream = graph.from_of(edge_index);
      if (upstream == kNoNode || seen[upstream] != 0) {
        continue;
      }
      seen[upstream] = 1;
      stack.push_back(upstream);
    }
  }
  std::sort(result.sources.begin(), result.sources.end());
  result.sources.erase(std::unique(result.sources.begin(), result.sources.end()), result.sources.end());
  return result;
}

SourceSet member_source_set(const GraphIndex& graph, std::uint32_t node, std::size_t max_nodes) {
  if (node == kNoNode || node >= graph.node_count()) {
    return SourceSet{};
  }
  if (graph.nodes()[node].kind() != NodeKind::Pump) {
    return structural_sources_reaching(graph, node, max_nodes);
  }
  const std::vector<Edge>& edges = graph.edges();
  for (const std::uint32_t edge_index : graph.in_edges(node)) {
    if (edges[edge_index].kind != EdgeKind::Pumps) {
      continue;
    }
    const std::uint32_t host = graph.from_of(edge_index);
    if (host == kNoNode) {
      return SourceSet{};
    }
    return structural_sources_reaching(graph, host, max_nodes);
  }
  return SourceSet{};
}

std::vector<std::uint32_t> origin_nodes(const GraphIndex& graph) {
  std::vector<std::uint32_t> result;
  const std::vector<Edge>& edges = graph.edges();
  const std::vector<Node>& nodes = graph.nodes();
  for (std::uint32_t index = 0; index < graph.node_count(); ++index) {
    if (!is_supplier_kind(nodes[index].kind())) {
      continue;
    }
    bool fed = false;
    for (const std::uint32_t edge_index : graph.in_edges(index)) {
      if (edges[edge_index].kind == EdgeKind::Supplies) {
        fed = true;
        break;
      }
    }
    if (!fed) {
      result.push_back(index);
    }
  }
  return result;
}

}  // namespace dccp::cooling_topology::internal
