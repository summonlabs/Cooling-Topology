// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/query.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/text.hpp"
#include "graph_internal.hpp"

namespace dccp::cooling_topology {
namespace {

using internal::GraphIndex;
using internal::kNoNode;
using internal::NodeIndex;
using internal::SourceSet;

constexpr EdgeKind kSuppliesKinds[] = {EdgeKind::Supplies};
constexpr EdgeKind kReturnsKinds[] = {EdgeKind::Returns};
constexpr EdgeKind kContainsKinds[] = {EdgeKind::Contains};

/// Everything a query needs, derived once from an immutable generation.
struct QueryContext {
  const Topology* topology = nullptr;
  GraphIndex graph;

  static QueryContext build(const Topology& topology) {
    QueryContext context;
    context.topology = &topology;
    context.graph.build(topology.nodes(), topology.edges(), topology.aliases());
    return context;
  }

  Result<std::uint32_t> lookup(const NodeId& identity) const {
    const auto found = graph.identities().find(identity.value());
    if (!found.has_value()) {
      return Error(ErrorCode::NotFound, "identity names neither a node nor an alias in this generation")
          .with_subject(identity.str());
    }
    return *found;
  }

  const Node& node(std::uint32_t index) const { return graph.nodes()[index]; }
  std::size_t node_count() const { return graph.node_count(); }
};

std::vector<std::uint32_t> sorted_unique(std::vector<std::uint32_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::vector<NodeId> to_ids(const QueryContext& context, const std::vector<std::uint32_t>& indices) {
  std::vector<NodeId> ids;
  ids.reserve(indices.size());
  for (const std::uint32_t index : indices) {
    ids.push_back(context.node(index).id);
  }
  return ids;
}

bool hosts_pump(const QueryContext& context, std::uint32_t index) {
  const std::vector<Edge>& edges = context.graph.edges();
  for (const std::uint32_t edge : context.graph.out_edges(index)) {
    if (edges[edge].kind == EdgeKind::Pumps) {
      return true;
    }
  }
  return false;
}

/// Changeover membership of one endpoint, as (group index, member index).
std::vector<std::pair<std::uint32_t, std::uint32_t>> changeover_membership(const QueryContext& context,
                                                                          const Endpoint& endpoint) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
  const std::vector<ChangeoverGroup>& groups = context.topology->changeovers();
  for (std::uint32_t group = 0; group < groups.size(); ++group) {
    if (groups[group].max_concurrent != 1) {
      continue;
    }
    for (std::uint32_t member = 0; member < groups[group].members.size(); ++member) {
      if (groups[group].members[member] == endpoint) {
        result.emplace_back(group, member);
      }
    }
  }
  return result;
}

/// Bounded depth-first enumeration of simple supply paths from origin to target.
///
/// Supplies edges form a DAG (enforced by validation), so a simple-path
/// enumeration terminates; the traversal still tracks the current stack and is
/// bounded by max_paths, max_depth and a total step budget so that a huge or
/// adversarial graph cannot exhaust time or memory.
struct PathSearch {
  const QueryContext* context = nullptr;
  const QueryOptions* options = nullptr;
  std::uint32_t origin = kNoNode;
  std::uint32_t target = kNoNode;
  std::vector<CoolingPath> paths;
  bool truncated = false;
  std::size_t budget = 0;

  std::vector<std::uint32_t> stack_nodes;
  std::vector<std::uint32_t> stack_edges;
  std::vector<std::uint8_t> on_stack;

  void run() {
    stack_nodes.push_back(origin);
    on_stack.assign(context->node_count(), 0);
    on_stack[origin] = 1;
    expand();
    on_stack[origin] = 0;
    stack_nodes.pop_back();
  }

  void expand() {
    if (paths.size() >= options->max_paths || truncated) {
      truncated = true;
      return;
    }
    const std::vector<Edge>& edges = context->graph.edges();
    const std::uint32_t current = stack_nodes.back();
    if (current == target) {
      CoolingPath path;
      path.nodes.reserve(stack_nodes.size());
      for (const std::uint32_t node : stack_nodes) {
        path.nodes.push_back(context->node(node).id);
      }
      path.edges.reserve(stack_edges.size());
      for (const std::uint32_t edge : stack_edges) {
        path.edges.push_back(context->graph.edges()[edge].id);
      }
      path.passes_pump_host = false;
      path.crosses_changeover = false;
      for (const std::uint32_t node : stack_nodes) {
        path.passes_pump_host = path.passes_pump_host || hosts_pump(*context, node);
      }
      for (const std::uint32_t edge : stack_edges) {
        const Edge& record = context->graph.edges()[edge];
        path.crosses_changeover = path.crosses_changeover ||
                                  !changeover_membership(*context, record.from).empty() ||
                                  !changeover_membership(*context, record.to).empty();
      }
      path.origin_source_count = internal::structural_sources_reaching(context->graph, origin,
                                                                      options->max_search_nodes)
                                     .sources.size();
      paths.push_back(std::move(path));
      return;
    }
    if (stack_edges.size() >= options->max_depth || stack_edges.size() >= limits::kMaxPathLength) {
      truncated = true;
      return;
    }
    for (const std::uint32_t edge : context->graph.out_edges(current)) {
      if (budget == 0) {
        truncated = true;
        return;
      }
      --budget;
      if (edges[edge].kind != EdgeKind::Supplies) {
        continue;
      }
      const std::uint32_t next = context->graph.to_of(edge);
      if (next == kNoNode || on_stack[next] != 0) {
        continue;
      }
      on_stack[next] = 1;
      stack_nodes.push_back(next);
      stack_edges.push_back(edge);
      expand();
      stack_edges.pop_back();
      stack_nodes.pop_back();
      on_stack[next] = 0;
      if (truncated) {
        return;
      }
    }
  }
};

/// Finds one return path between two elements, bounded and terminating.
///
/// The search is an iterative depth-first walk over Returns edges. A node is
/// marked visited once and is never unmarked, so a dead-end subtree is expanded
/// at most once and the walk is O(V + E): re-opening a node on the way back up
/// would let its parent re-descend into the same dead end forever, which is a
/// non-terminating search rather than a slow one. The walk stops when it finds
/// the target, when the frontier is exhausted, or when a documented bound is
/// reached, and it reports the last case through the truncated flag. The step
/// budget is tested before every decrement, so it can never wrap.
bool find_return_path(const QueryContext& context, std::uint32_t from, std::uint32_t to,
                      const QueryOptions& options, std::vector<std::uint32_t>& nodes,
                      std::vector<std::uint32_t>& edges, bool* truncated) {
  if (from == to) {
    nodes.assign(1, from);
    edges.clear();
    return true;
  }
  const std::vector<Edge>& table = context.graph.edges();
  const std::size_t depth_bound =
      options.max_depth < limits::kMaxPathLength ? options.max_depth : limits::kMaxPathLength;
  std::size_t budget = options.max_search_nodes == 0 ? 1 : options.max_search_nodes;

  std::vector<std::uint8_t> visited(context.node_count(), 0);
  std::vector<std::uint32_t> node_stack;
  std::vector<std::uint32_t> edge_stack;
  std::vector<std::size_t> frame_cursor;
  visited[from] = 1;
  node_stack.push_back(from);
  frame_cursor.push_back(0);

  while (!node_stack.empty()) {
    const std::uint32_t current = node_stack.back();
    if (current == to) {
      nodes = node_stack;
      edges = edge_stack;
      return true;
    }
    const std::span<const std::uint32_t> outgoing = context.graph.out_edges(current);
    bool descended = false;
    while (frame_cursor.back() < outgoing.size()) {
      if (budget == 0) {
        if (truncated != nullptr) {
          *truncated = true;
        }
        return false;
      }
      --budget;
      const std::uint32_t edge = outgoing[frame_cursor.back()];
      ++frame_cursor.back();
      if (table[edge].kind != EdgeKind::Returns) {
        continue;
      }
      const std::uint32_t next = context.graph.to_of(edge);
      if (next == kNoNode || visited[next] != 0) {
        continue;
      }
      if (edge_stack.size() >= depth_bound) {
        if (truncated != nullptr) {
          *truncated = true;
        }
        continue;
      }
      visited[next] = 1;
      node_stack.push_back(next);
      edge_stack.push_back(edge);
      frame_cursor.push_back(0);
      descended = true;
      break;
    }
    if (descended) {
      continue;
    }
    node_stack.pop_back();
    frame_cursor.pop_back();
    if (!edge_stack.empty()) {
      edge_stack.pop_back();
    }
  }
  return false;
}

const char* claim_statement(ClaimClass claim) noexcept {
  return claim == ClaimClass::StructurallyPossible
             ? "structural possibility only: the relationship exists in this generation and is not "
               "asserted to be operating, flowing, capable, eligible, safe or authorized"
             : "structural impossibility: no such relationship exists in this generation for any "
               "operating state";
}

}  // namespace

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

std::string_view to_token(ClaimClass value) noexcept {
  return value == ClaimClass::StructurallyPossible ? std::string_view("structurally_possible")
                                                   : std::string_view("structurally_impossible");
}

std::string_view claim_class_statement(ClaimClass value) noexcept { return claim_statement(value); }

std::string_view to_token(ExclusivityReason reason) noexcept {
  return reason == ExclusivityReason::SharedChangeoverGroup ? std::string_view("shared_changeover_group")
                                                            : std::string_view("unknown");
}

std::string_view to_token(RedundancyVerdict verdict) noexcept {
  switch (verdict) {
    case RedundancyVerdict::DeclarationConsistent:
      return "declaration_consistent";
    case RedundancyVerdict::DeclarationViolated:
      return "declaration_violated";
    case RedundancyVerdict::IndependenceUnproven:
      return "independence_unproven";
    default:
      return "unknown";
  }
}

std::string_view to_token(SinkFeedVerdict verdict) noexcept {
  switch (verdict) {
    case SinkFeedVerdict::SingleFeed:
      return "single_feed";
    case SinkFeedVerdict::MultipleIndependentFeeds:
      return "multiple_independent_feeds";
    case SinkFeedVerdict::MultipleFeedsSharingDependency:
      return "multiple_feeds_sharing_dependency";
    case SinkFeedVerdict::NoStructuralFeed:
      return "no_structural_feed";
    default:
      return "unknown";
  }
}

std::string_view to_token(ZoneVerdict verdict) noexcept {
  switch (verdict) {
    case ZoneVerdict::Served:
      return "served";
    case ZoneVerdict::NoServingElement:
      return "no_serving_element";
    case ZoneVerdict::NoContainedSink:
      return "no_contained_sink";
    case ZoneVerdict::Unpopulated:
      return "unpopulated";
    default:
      return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Reachability
// ---------------------------------------------------------------------------

Result<UpstreamResult> upstream_of(const Topology& topology, const NodeId& identity,
                                   const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(origin, context.lookup(identity));
  UpstreamResult result;
  result.origin = context.node(origin).id;
  const internal::TraversalResult traversal = internal::traverse(context.graph, origin, kSuppliesKinds, true,
                                                                 options.max_depth, options.max_results);
  result.truncated = traversal.truncated;
  for (std::size_t index = 0; index < traversal.visited.size(); ++index) {
    ReachedElement element;
    element.node = context.node(traversal.visited[index]).id;
    element.depth = traversal.depth[index];
    element.via_edge = context.graph.edges()[traversal.via_edge[index]].id;
    element.entered_port = context.graph.edges()[traversal.via_edge[index]].from.port;
    result.elements.push_back(std::move(element));
  }
  result.claim = result.elements.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<DownstreamResult> downstream_of(const Topology& topology, const NodeId& identity,
                                       const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(origin, context.lookup(identity));
  DownstreamResult result;
  result.origin = context.node(origin).id;
  const internal::TraversalResult traversal = internal::traverse(context.graph, origin, kSuppliesKinds, false,
                                                                 options.max_depth, options.max_results);
  result.truncated = traversal.truncated;
  for (std::size_t index = 0; index < traversal.visited.size(); ++index) {
    ReachedElement element;
    element.node = context.node(traversal.visited[index]).id;
    element.depth = traversal.depth[index];
    element.via_edge = context.graph.edges()[traversal.via_edge[index]].id;
    element.entered_port = context.graph.edges()[traversal.via_edge[index]].to.port;
    result.elements.push_back(std::move(element));
  }
  result.claim = result.elements.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<std::vector<NodeId>> sources_serving(const Topology& topology, const NodeId& identity,
                                            const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(node, context.lookup(identity));
  const SourceSet sources = internal::member_source_set(context.graph, node, options.max_search_nodes);
  return to_ids(context, sources.sources);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

Result<PathQueryResult> possible_supply_paths(const Topology& topology, const NodeId& from, const NodeId& to,
                                              const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(origin, context.lookup(from));
  CT_TRY(target, context.lookup(to));
  PathQueryResult result;
  result.from = context.node(origin).id;
  result.to = context.node(target).id;

  const SourceSet reachable = internal::member_source_set(context.graph, target, options.max_search_nodes);
  result.reachable_sources = to_ids(context, reachable.sources);

  if (origin != target) {
    PathSearch search;
    search.context = &context;
    search.options = &options;
    search.origin = origin;
    search.target = target;
    search.budget = options.max_search_nodes;
    search.run();
    result.paths = std::move(search.paths);
    result.truncated = search.truncated;
  }
  result.claim = result.paths.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;

  for (std::size_t first = 0; first < result.paths.size(); ++first) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> touched_first;
    for (const EdgeId& edge_id : result.paths[first].edges) {
      const Edge* edge = topology.find_edge(edge_id);
      if (edge == nullptr) {
        continue;
      }
      for (const auto& entry : changeover_membership(context, edge->from)) {
        touched_first.push_back(entry);
      }
      for (const auto& entry : changeover_membership(context, edge->to)) {
        touched_first.push_back(entry);
      }
    }
    for (std::size_t second = first + 1; second < result.paths.size(); ++second) {
      for (const EdgeId& edge_id : result.paths[second].edges) {
        const Edge* edge = topology.find_edge(edge_id);
        if (edge == nullptr) {
          continue;
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> touched_second;
        for (const auto& entry : changeover_membership(context, edge->from)) {
          touched_second.push_back(entry);
        }
        for (const auto& entry : changeover_membership(context, edge->to)) {
          touched_second.push_back(entry);
        }
        for (const auto& lhs : touched_first) {
          for (const auto& rhs : touched_second) {
            if (lhs.first == rhs.first && lhs.second != rhs.second) {
              PathPairExclusivity pair;
              pair.first = first;
              pair.second = second;
              pair.reason = ExclusivityReason::SharedChangeoverGroup;
              pair.witness = topology.changeovers()[lhs.first].id.str();
              result.exclusive_pairs.push_back(std::move(pair));
            }
          }
        }
      }
    }
  }
  return result;
}

Result<CircuitQueryResult> possible_circuits(const Topology& topology, const NodeId& from, const NodeId& to,
                                             const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(origin, context.lookup(from));
  CT_TRY(target, context.lookup(to));
  CircuitQueryResult result;
  result.from = context.node(origin).id;
  result.to = context.node(target).id;
  if (origin == target) {
    result.claim = ClaimClass::StructurallyImpossible;
    return result;
  }

  QueryOptions path_options = options;
  path_options.max_paths = std::min(options.max_paths, limits::kMaxPathCount);
  CT_TRY(supply, possible_supply_paths(topology, from, to, path_options));
  result.truncated = supply.truncated;

  for (const CoolingPath& path : supply.paths) {
    CT_TRY(tail, context.lookup(path.nodes.back()));
    CT_TRY(head, context.lookup(path.nodes.front()));
    std::vector<std::uint32_t> return_nodes;
    std::vector<std::uint32_t> return_edges;
    bool return_truncated = false;
    if (!find_return_path(context, tail, head, options, return_nodes, return_edges, &return_truncated)) {
      result.truncated = result.truncated || return_truncated;
      continue;
    }
    CoolingCircuit circuit;
    circuit.supply_nodes = path.nodes;
    circuit.supply_edges = path.edges;
    for (const std::uint32_t node : return_nodes) {
      circuit.return_nodes.push_back(context.node(node).id);
    }
    for (const std::uint32_t edge : return_edges) {
      circuit.return_edges.push_back(context.graph.edges()[edge].id);
    }
    circuit.crosses_changeover = path.crosses_changeover;
    for (const std::uint32_t edge : return_edges) {
      const Edge& record = context.graph.edges()[edge];
      circuit.crosses_changeover = circuit.crosses_changeover ||
                                   !changeover_membership(context, record.from).empty() ||
                                   !changeover_membership(context, record.to).empty();
    }
    result.circuits.push_back(std::move(circuit));
  }
  result.claim =
      result.circuits.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<IndependentPathResult> independent_supply_paths(const Topology& topology, const NodeId& from,
                                                       const NodeId& to, const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(origin, context.lookup(from));
  CT_TRY(target, context.lookup(to));
  IndependentPathResult result;
  result.from = context.node(origin).id;
  result.to = context.node(target).id;
  if (origin == target) {
    return result;
  }

  const std::vector<Edge>& table = context.graph.edges();
  std::vector<std::uint8_t> used(table.size(), 0);
  std::size_t budget = options.max_search_nodes;
  const std::size_t limit = std::min(options.max_paths, limits::kMaxPathCount);

  while (result.paths.size() < limit) {
    // Breadth-first search over supply edges that are not already used, so each
    // discovery is edge-disjoint from every path found before it.
    std::vector<std::uint32_t> parent_edge(context.node_count(), kNoNode);
    std::vector<std::uint32_t> parent_node(context.node_count(), kNoNode);
    std::vector<std::uint8_t> seen(context.node_count(), 0);
    std::vector<std::uint32_t> frontier;
    frontier.push_back(origin);
    seen[origin] = 1;
    bool found = false;
    while (!frontier.empty() && !found) {
      std::vector<std::uint32_t> next;
      for (const std::uint32_t node : frontier) {
        for (const std::uint32_t edge : context.graph.out_edges(node)) {
          if (budget == 0) {
            result.truncated = true;
            break;
          }
          --budget;
          if (used[edge] != 0 || table[edge].kind != EdgeKind::Supplies) {
            continue;
          }
          const std::uint32_t other = context.graph.to_of(edge);
          if (other == kNoNode || seen[other] != 0) {
            continue;
          }
          seen[other] = 1;
          parent_edge[other] = edge;
          parent_node[other] = node;
          if (other == target) {
            found = true;
            break;
          }
          next.push_back(other);
        }
        if (found || result.truncated) {
          break;
        }
      }
      if (found || result.truncated) {
        break;
      }
      std::sort(next.begin(), next.end());
      frontier.swap(next);
    }
    if (!found) {
      break;
    }
    CoolingPath path;
    std::vector<std::uint32_t> reversed_nodes;
    std::vector<std::uint32_t> reversed_edges;
    for (std::uint32_t node = target; node != origin;) {
      reversed_edges.push_back(parent_edge[node]);
      reversed_nodes.push_back(node);
      node = parent_node[node];
      if (node == kNoNode) {
        reversed_edges.clear();
        reversed_nodes.clear();
        break;
      }
    }
    if (reversed_edges.empty()) {
      break;
    }
    path.nodes.push_back(context.node(origin).id);
    for (auto it = reversed_nodes.rbegin(); it != reversed_nodes.rend(); ++it) {
      path.nodes.push_back(context.node(*it).id);
    }
    for (auto it = reversed_edges.rbegin(); it != reversed_edges.rend(); ++it) {
      path.edges.push_back(table[*it].id);
      used[*it] = 1;
      path.passes_pump_host = path.passes_pump_host || hosts_pump(context, context.graph.from_of(*it));
    }
    for (std::size_t index = 1; index < path.nodes.size(); ++index) {
      const Node* node = topology.find_node(path.nodes[index]);
      if (node == nullptr) {
        continue;
      }
      const auto index_in_graph = context.graph.identities().find_node(node->id.value());
      if (index_in_graph.has_value()) {
        path.passes_pump_host = path.passes_pump_host || hosts_pump(context, *index_in_graph);
      }
    }
    path.origin_source_count =
        internal::structural_sources_reaching(context.graph, origin, options.max_search_nodes).sources.size();
    result.paths.push_back(std::move(path));
  }

  result.path_count = result.paths.size();
  if (result.path_count > 0) {
    result.claim = ClaimClass::StructurallyPossible;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Dependency analysis
// ---------------------------------------------------------------------------

Result<CommonDependencyResult> common_dependencies(const Topology& topology,
                                                   const std::vector<NodeId>& subjects,
                                                   const QueryOptions& options) {
  if (subjects.empty()) {
    return Error(ErrorCode::EmptyInput, "common-dependency query needs at least one subject");
  }
  if (subjects.size() > limits::kMaxPathQuerySubjects) {
    return Error(ErrorCode::LimitExceeded, "common-dependency query names more subjects than the bound");
  }
  const QueryContext context = QueryContext::build(topology);
  CommonDependencyResult result;
  std::vector<std::uint32_t> resolved;
  for (const NodeId& subject : subjects) {
    CT_TRY(node, context.lookup(subject));
    resolved.push_back(node);
    result.subjects.push_back(context.node(node).id);
  }

  std::vector<std::uint32_t> common;
  for (std::size_t index = 0; index < resolved.size(); ++index) {
    const internal::TraversalResult traversal = internal::traverse(
        context.graph, resolved[index], kSuppliesKinds, true, options.max_depth, options.max_results);
    result.truncated = result.truncated || traversal.truncated;
    std::vector<std::uint32_t> ancestors = sorted_unique(traversal.visited);
    if (index == 0) {
      common = std::move(ancestors);
      continue;
    }
    std::vector<std::uint32_t> intersection;
    std::set_intersection(common.begin(), common.end(), ancestors.begin(), ancestors.end(),
                          std::back_inserter(intersection));
    common = std::move(intersection);
  }
  for (const std::uint32_t node : common) {
    if (std::find(resolved.begin(), resolved.end(), node) != resolved.end()) {
      continue;
    }
    result.dependencies.push_back(context.node(node).id);
  }
  result.claim =
      result.dependencies.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<SinglePointResult> single_points_of_structural_dependency(const Topology& topology,
                                                                const std::vector<NodeId>& subjects,
                                                                const QueryOptions& options) {
  if (subjects.empty()) {
    return Error(ErrorCode::EmptyInput, "dependency-point query needs at least one subject");
  }
  if (subjects.size() > limits::kMaxPathQuerySubjects) {
    return Error(ErrorCode::LimitExceeded, "dependency-point query names more subjects than the bound");
  }
  const QueryContext context = QueryContext::build(topology);
  SinglePointResult result;
  std::vector<std::uint32_t> resolved;
  for (const NodeId& subject : subjects) {
    CT_TRY(node, context.lookup(subject));
    resolved.push_back(node);
    result.subjects.push_back(context.node(node).id);
  }

  // Candidates are the structural ancestors of the subjects: only an element
  // that can reach every subject can be a shared dependency of all of them.
  std::vector<std::uint32_t> candidates;
  for (const std::uint32_t subject : resolved) {
    const internal::TraversalResult traversal = internal::traverse(
        context.graph, subject, kSuppliesKinds, true, options.max_depth, options.max_results);
    result.truncated = result.truncated || traversal.truncated;
    candidates.insert(candidates.end(), traversal.visited.begin(), traversal.visited.end());
  }
  candidates = sorted_unique(std::move(candidates));
  if (candidates.size() > options.max_candidates) {
    candidates.resize(options.max_candidates);
    result.truncated = true;
  }

  const std::vector<Edge>& table = context.graph.edges();
  const std::vector<std::uint32_t> origins = internal::origin_nodes(context.graph);
  for (const std::uint32_t candidate : candidates) {
    std::vector<std::uint8_t> reached(context.node_count(), 0);
    std::vector<std::uint32_t> frontier;
    for (const std::uint32_t origin : origins) {
      if (origin == candidate || reached[origin] != 0) {
        continue;
      }
      reached[origin] = 1;
      frontier.push_back(origin);
    }
    std::size_t budget = options.max_search_nodes;
    bool exhausted = false;
    while (!frontier.empty() && !exhausted) {
      std::vector<std::uint32_t> next;
      for (const std::uint32_t node : frontier) {
        for (const std::uint32_t edge : context.graph.out_edges(node)) {
          if (budget == 0) {
            exhausted = true;
            break;
          }
          --budget;
          if (table[edge].kind != EdgeKind::Supplies) {
            continue;
          }
          const std::uint32_t other = context.graph.to_of(edge);
          if (other == kNoNode || other == candidate || reached[other] != 0) {
            continue;
          }
          reached[other] = 1;
          next.push_back(other);
        }
        if (exhausted) {
          break;
        }
      }
      std::sort(next.begin(), next.end());
      frontier.swap(next);
    }
    if (exhausted) {
      result.truncated = true;
      continue;
    }
    DependencyPoint point;
    point.node = context.node(candidate).id;
    point.is_structural_source = std::find(origins.begin(), origins.end(), candidate) != origins.end();
    std::size_t probed = 0;
    for (const std::uint32_t subject : resolved) {
      if (subject == candidate) {
        continue;
      }
      const NodeKind kind = context.node(subject).kind();
      // A subject that can neither deliver nor receive medium has no supply
      // source to lose; reporting it as disconnected would be an unsupported
      // claim, so it is not probed at all.
      if (!is_supplier_kind(kind) && !is_consumer_kind(kind)) {
        continue;
      }
      ++probed;
      if (reached[subject] == 0) {
        point.disconnected_subjects.push_back(context.node(subject).id);
      }
    }
    if (!point.disconnected_subjects.empty()) {
      point.disconnects_all_subjects = probed > 0 && point.disconnected_subjects.size() == probed;
      result.points.push_back(std::move(point));
    }
  }
  result.claim = result.points.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

// ---------------------------------------------------------------------------
// Redundancy
// ---------------------------------------------------------------------------

Result<RedundancyMembershipResult> redundancy_membership(const Topology& topology, const NodeId& identity) {
  CT_TRY(canonical, topology.resolve(identity));
  RedundancyMembershipResult result;
  result.node = canonical;
  for (const RedundancyGroup& group : topology.groups()) {
    for (std::size_t index = 0; index < group.members.size(); ++index) {
      if (group.members[index].node == canonical) {
        GroupMembership membership;
        membership.group = group.id;
        membership.member_index = index;
        membership.declared = group.members[index].declared;
        result.memberships.push_back(std::move(membership));
      }
    }
  }
  return result;
}

Result<RedundancyGroupReport> redundancy_group_report(const Topology& topology,
                                                      const RedundancyGroupId& group_id,
                                                      const QueryOptions& options) {
  const RedundancyGroup* group = topology.find_group(group_id);
  if (group == nullptr) {
    return Error(ErrorCode::NotFound, "redundancy group is not present in this generation")
        .with_subject(group_id.str());
  }
  const QueryContext context = QueryContext::build(topology);
  RedundancyGroupReport report;
  report.group = group->id;
  report.scheme = group->scheme;
  report.scope = group->scope;

  std::vector<std::vector<std::uint32_t>> source_sets;
  for (const RedundancyMember& member : group->members) {
    RedundancyMemberReport entry;
    entry.node = member.node;
    entry.declared = member.declared;
    if (member.failure_domain.has_value()) {
      entry.failure_domain_declared = true;
      entry.failure_domain = member.failure_domain->identity;
    }
    const auto resolved = context.graph.identities().find(member.node.value());
    if (resolved.has_value()) {
      const SourceSet sources = internal::member_source_set(context.graph, *resolved, options.max_search_nodes);
      report.truncated = report.truncated || sources.truncated;
      entry.sources = to_ids(context, sources.sources);
      source_sets.push_back(sources.sources);
    } else {
      source_sets.emplace_back();
    }
    report.members.push_back(std::move(entry));
  }

  std::vector<std::uint32_t> shared;
  for (std::size_t first = 0; first < source_sets.size(); ++first) {
    for (std::size_t second = first + 1; second < source_sets.size(); ++second) {
      for (const std::uint32_t source : source_sets[first]) {
        if (std::find(source_sets[second].begin(), source_sets[second].end(), source) !=
            source_sets[second].end()) {
          shared.push_back(source);
        }
      }
    }
  }
  report.shared_sources = to_ids(context, sorted_unique(std::move(shared)));

  // A member that is itself a structural origin of another member is reported
  // as an ancestor. Any other structural ancestor necessarily shares a source
  // with its descendant, so it is already visible through shared_sources and
  // needs no separate traversal.
  for (std::size_t first = 0; first < source_sets.size(); ++first) {
    const auto member_index = context.graph.identities().find(group->members[first].node.value());
    if (!member_index.has_value()) {
      continue;
    }
    for (std::size_t second = 0; second < source_sets.size(); ++second) {
      if (first == second) {
        continue;
      }
      if (std::find(source_sets[second].begin(), source_sets[second].end(), *member_index) !=
          source_sets[second].end()) {
        report.ancestor_members.push_back(group->members[first].node);
        break;
      }
    }
  }

  std::vector<std::pair<std::string, std::size_t>> domains;
  for (std::size_t index = 0; index < group->members.size(); ++index) {
    if (group->members[index].failure_domain.has_value()) {
      domains.emplace_back(group->members[index].failure_domain->identity, index);
    }
  }
  std::sort(domains.begin(), domains.end());
  for (std::size_t index = 1; index < domains.size(); ++index) {
    if (domains[index].first == domains[index - 1].first) {
      report.duplicate_failure_domains.push_back(domains[index].first);
    }
  }

  const bool has_members = group->members.size() >= 1;
  const bool every_member_sourced =
      std::all_of(source_sets.begin(), source_sets.end(),
                  [](const std::vector<std::uint32_t>& sources) { return !sources.empty(); });
  const bool domains_ok = !group->require_distinct_failure_domains || report.duplicate_failure_domains.empty();
  const bool sources_ok = !group->require_independent_sources || report.shared_sources.empty();
  report.independence_holds = has_members && every_member_sourced && domains_ok && sources_ok;
  if (!has_members) {
    report.verdict = RedundancyVerdict::IndependenceUnproven;
  } else if (!report.independence_holds) {
    report.verdict = RedundancyVerdict::DeclarationViolated;
  } else if (group->require_distinct_failure_domains || group->require_independent_sources) {
    report.verdict = RedundancyVerdict::DeclarationConsistent;
  } else {
    report.verdict = RedundancyVerdict::IndependenceUnproven;
  }
  return report;
}

// ---------------------------------------------------------------------------
// Sinks and zones
// ---------------------------------------------------------------------------

Result<SinkServiceReport> sink_service_report(const Topology& topology, const NodeId& identity,
                                              const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(sink, context.lookup(identity));
  const Node& node = context.node(sink);
  if (node.kind() != NodeKind::CoolingSink) {
    return Error(ErrorCode::InvalidArgument, "sink service report requires a cooling-sink element")
        .with_subject(node.id.str());
  }
  SinkServiceReport report;
  report.sink = node.id;
  report.kind = node.as_cooling_sink()->kind;

  const std::vector<Edge>& table = context.graph.edges();
  std::vector<std::vector<std::uint32_t>> source_sets;
  for (const std::uint32_t edge : context.graph.in_edges(sink)) {
    if (table[edge].kind != EdgeKind::Supplies) {
      continue;
    }
    const std::uint32_t feeder = context.graph.from_of(edge);
    if (feeder == kNoNode) {
      continue;
    }
    SinkFeed feed;
    feed.feeder = context.node(feeder).id;
    feed.edge = table[edge].id;
    const SourceSet sources = internal::member_source_set(context.graph, feeder, options.max_search_nodes);
    feed.sources = to_ids(context, sources.sources);
    source_sets.push_back(sources.sources);
    report.feeds.push_back(std::move(feed));
  }
  std::vector<std::uint32_t> shared;
  for (std::size_t first = 0; first < source_sets.size(); ++first) {
    for (std::size_t second = first + 1; second < source_sets.size(); ++second) {
      for (const std::uint32_t source : source_sets[first]) {
        if (std::find(source_sets[second].begin(), source_sets[second].end(), source) !=
            source_sets[second].end()) {
          shared.push_back(source);
        }
      }
    }
  }
  report.shared_sources = to_ids(context, sorted_unique(std::move(shared)));
  if (report.feeds.empty()) {
    report.verdict = SinkFeedVerdict::NoStructuralFeed;
  } else if (report.feeds.size() == 1) {
    report.verdict = SinkFeedVerdict::SingleFeed;
  } else if (report.shared_sources.empty()) {
    report.verdict = SinkFeedVerdict::MultipleIndependentFeeds;
  } else {
    report.verdict = SinkFeedVerdict::MultipleFeedsSharingDependency;
  }
  return report;
}

Result<ZoneReport> zone_report(const Topology& topology, const NodeId& identity, const QueryOptions&) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(zone, context.lookup(identity));
  const Node& node = context.node(zone);
  if (node.kind() != NodeKind::ThermalZone) {
    return Error(ErrorCode::InvalidArgument, "zone report requires a thermal-zone element")
        .with_subject(node.id.str());
  }
  ZoneReport report;
  report.zone = node.id;
  report.zone_class = node.as_thermal_zone()->zone_class;

  const std::vector<Edge>& table = context.graph.edges();
  for (const std::uint32_t edge : context.graph.in_edges(zone)) {
    if (table[edge].kind != EdgeKind::Serves) {
      continue;
    }
    const std::uint32_t server = context.graph.from_of(edge);
    if (server != kNoNode) {
      report.serving_elements.push_back(context.node(server).id);
    }
  }
  for (const std::uint32_t edge : context.graph.out_edges(zone)) {
    if (table[edge].kind != EdgeKind::Contains) {
      continue;
    }
    const std::uint32_t contained = context.graph.to_of(edge);
    if (contained == kNoNode) {
      continue;
    }
    const NodeKind kind = context.node(contained).kind();
    if (kind == NodeKind::CoolingSink) {
      report.contained_sinks.push_back(context.node(contained).id);
    } else if (kind == NodeKind::Crah || kind == NodeKind::Crac) {
      report.contained_air_handlers.push_back(context.node(contained).id);
    }
  }
  std::sort(report.serving_elements.begin(), report.serving_elements.end());
  std::sort(report.contained_sinks.begin(), report.contained_sinks.end());
  std::sort(report.contained_air_handlers.begin(), report.contained_air_handlers.end());

  for (const RedundancyGroup& group : topology.groups()) {
    for (const RedundancyMember& member : group.members) {
      const bool touches =
          std::find(report.serving_elements.begin(), report.serving_elements.end(), member.node) !=
              report.serving_elements.end() ||
          std::find(report.contained_air_handlers.begin(), report.contained_air_handlers.end(), member.node) !=
              report.contained_air_handlers.end();
      if (touches) {
        report.groups.push_back(group.id);
        break;
      }
    }
  }

  if (report.serving_elements.empty() && report.contained_sinks.empty()) {
    report.verdict = ZoneVerdict::Unpopulated;
  } else if (report.serving_elements.empty()) {
    report.verdict = ZoneVerdict::NoServingElement;
  } else if (report.contained_sinks.empty()) {
    report.verdict = ZoneVerdict::NoContainedSink;
  } else {
    report.verdict = ZoneVerdict::Served;
  }
  return report;
}

// ---------------------------------------------------------------------------
// Blast radius and connectivity
// ---------------------------------------------------------------------------

Result<BlastRadiusResult> blast_radius(const Topology& topology, const NodeId& identity,
                                       const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  CT_TRY(origin, context.lookup(identity));
  BlastRadiusResult result;
  result.origin = context.node(origin).id;

  const internal::TraversalResult traversal = internal::traverse(context.graph, origin, kSuppliesKinds, false,
                                                                 options.max_depth, options.max_results);
  result.truncated = traversal.truncated;
  std::vector<std::uint32_t> affected;
  affected.push_back(origin);
  for (std::size_t index = 0; index < traversal.visited.size(); ++index) {
    ReachedElement element;
    element.node = context.node(traversal.visited[index]).id;
    element.depth = traversal.depth[index];
    element.via_edge = context.graph.edges()[traversal.via_edge[index]].id;
    element.entered_port = context.graph.edges()[traversal.via_edge[index]].to.port;
    result.supplied_downstream.push_back(std::move(element));
    affected.push_back(traversal.visited[index]);
  }
  affected = sorted_unique(std::move(affected));

  for (const std::uint32_t node : affected) {
    if (context.node(node).kind() == NodeKind::CoolingSink) {
      result.affected_sinks.push_back(context.node(node).id);
    }
    if (context.node(node).kind() == NodeKind::ThermalZone) {
      result.affected_zones.push_back(context.node(node).id);
    }
  }
  for (const std::uint32_t node : affected) {
    for (const std::uint32_t edge : context.graph.out_edges(node)) {
      if (context.graph.edges()[edge].kind != EdgeKind::Serves) {
        continue;
      }
      const std::uint32_t served = context.graph.to_of(edge);
      if (served != kNoNode && context.node(served).kind() == NodeKind::ThermalZone) {
        result.affected_zones.push_back(context.node(served).id);
      }
    }
  }
  std::sort(result.affected_zones.begin(), result.affected_zones.end());
  result.affected_zones.erase(std::unique(result.affected_zones.begin(), result.affected_zones.end()),
                              result.affected_zones.end());

  if (options.include_containment_peers) {
    for (const std::uint32_t edge : context.graph.in_edges(origin)) {
      if (context.graph.edges()[edge].kind != EdgeKind::Contains) {
        continue;
      }
      const std::uint32_t container = context.graph.from_of(edge);
      if (container == kNoNode) {
        continue;
      }
      ContainmentPeer peer;
      peer.node = context.node(container).id;
      peer.contains_origin = true;
      result.containment_peers.push_back(std::move(peer));
      for (const std::uint32_t sibling_edge : context.graph.out_edges(container)) {
        if (context.graph.edges()[sibling_edge].kind != EdgeKind::Contains) {
          continue;
        }
        const std::uint32_t sibling = context.graph.to_of(sibling_edge);
        if (sibling == kNoNode || sibling == origin) {
          continue;
        }
        ContainmentPeer sibling_peer;
        sibling_peer.node = context.node(sibling).id;
        sibling_peer.sibling = true;
        result.containment_peers.push_back(std::move(sibling_peer));
      }
    }
    for (const std::uint32_t edge : context.graph.out_edges(origin)) {
      if (context.graph.edges()[edge].kind != EdgeKind::Contains) {
        continue;
      }
      const std::uint32_t contained = context.graph.to_of(edge);
      if (contained == kNoNode) {
        continue;
      }
      ContainmentPeer peer;
      peer.node = context.node(contained).id;
      peer.contained_by_origin = true;
      result.containment_peers.push_back(std::move(peer));
    }
    std::sort(result.containment_peers.begin(), result.containment_peers.end(),
              [](const ContainmentPeer& lhs, const ContainmentPeer& rhs) {
                if (lhs.node != rhs.node) {
                  return lhs.node < rhs.node;
                }
                if (lhs.contains_origin != rhs.contains_origin) {
                  return lhs.contains_origin;
                }
                if (lhs.contained_by_origin != rhs.contained_by_origin) {
                  return lhs.contained_by_origin;
                }
                return lhs.sibling < rhs.sibling;
              });
    result.containment_peers.erase(
        std::unique(result.containment_peers.begin(), result.containment_peers.end(),
                    [](const ContainmentPeer& lhs, const ContainmentPeer& rhs) {
                      return lhs.node == rhs.node && lhs.contains_origin == rhs.contains_origin &&
                             lhs.contained_by_origin == rhs.contained_by_origin && lhs.sibling == rhs.sibling;
                    }),
        result.containment_peers.end());
  }

  result.claim = result.supplied_downstream.empty() ? ClaimClass::StructurallyImpossible
                                                    : ClaimClass::StructurallyPossible;
  return result;
}

Result<ComponentReport> connected_components(const Topology& topology, const QueryOptions& options) {
  const QueryContext context = QueryContext::build(topology);
  ComponentReport report;
  const std::size_t count = context.node_count();
  std::vector<std::uint32_t> parent(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    parent[index] = index;
  }
  const auto find_root = [&parent](std::uint32_t value) {
    while (parent[value] != value) {
      parent[value] = parent[parent[value]];
      value = parent[value];
    }
    return value;
  };
  std::vector<std::uint8_t> touched(count, 0);
  for (const Edge& edge : topology.edges()) {
    const auto from = context.graph.identities().find(edge.from.node.value());
    const auto to = context.graph.identities().find(edge.to.node.value());
    if (!from.has_value() || !to.has_value()) {
      continue;
    }
    touched[*from] = 1;
    touched[*to] = 1;
    const std::uint32_t left = find_root(*from);
    const std::uint32_t right = find_root(*to);
    if (left != right) {
      parent[right] = left;
    }
  }
  std::vector<std::vector<std::uint32_t>> groups(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    if (touched[index] == 0) {
      report.isolated.push_back(context.node(index).id);
      continue;
    }
    groups[find_root(index)].push_back(index);
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    if (groups[index].empty()) {
      continue;
    }
    std::sort(groups[index].begin(), groups[index].end());
    report.components.push_back(to_ids(context, groups[index]));
    if (report.components.size() > options.max_results) {
      report.components.resize(options.max_results);
      report.truncated = true;
      break;
    }
  }
  report.components.erase(
      std::remove_if(report.components.begin(), report.components.end(),
                     [](const std::vector<NodeId>& component) { return component.empty(); }),
      report.components.end());
  return report;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

namespace {

void append_temperature(std::string& out, const char* key, const std::optional<MilliCelsius>& value) {
  if (!value.has_value()) {
    return;
  }
  out += ' ';
  out += key;
  out += '=';
  out += value->to_string();
}

}  // namespace

std::string describe_node(const Topology&, const Node& node) {
  std::string out;
  out.reserve(160);
  out += "node ";
  out += node.id.str();
  out += " kind=";
  out += to_token(node.kind());
  switch (node.kind()) {
    case NodeKind::CoolingPlant: {
      const auto& value = *node.as_cooling_plant();
      out += " plant=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Chiller: {
      const auto& value = *node.as_chiller();
      out += " chiller=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Pump: {
      const auto& value = *node.as_pump();
      out += " pump=";
      out += to_token(value.kind);
      out += " role=";
      out += to_token(value.role);
      break;
    }
    case NodeKind::CoolingLoop: {
      const auto& value = *node.as_cooling_loop();
      out += " loop=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Cdu: {
      const auto& value = *node.as_cdu();
      out += " cdu=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Crah: {
      const auto& value = *node.as_crah();
      out += " placement=";
      out += to_token(value.placement);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Crac: {
      const auto& value = *node.as_crac();
      out += " placement=";
      out += to_token(value.placement);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Manifold: {
      const auto& value = *node.as_manifold();
      out += " manifold=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::Branch: {
      const auto& value = *node.as_branch();
      out += " branch=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::ThermalZone: {
      const auto& value = *node.as_thermal_zone();
      out += " class=";
      out += to_token(value.zone_class);
      break;
    }
    case NodeKind::CoolingSink: {
      const auto& value = *node.as_cooling_sink();
      out += " sink=";
      out += to_token(value.kind);
      out += " consumer=";
      out += to_token(value.consumer.kind);
      out += ':';
      out += value.consumer.identity;
      append_temperature(out, "max-supply", value.max_acceptable_supply_temperature);
      break;
    }
    case NodeKind::CoolingSource: {
      const auto& value = *node.as_cooling_source();
      out += " source=";
      out += to_token(value.kind);
      if (value.medium.has_value()) {
        out += " medium=";
        out += to_token(*value.medium);
      }
      append_temperature(out, "design-supply", value.design_supply_temperature);
      break;
    }
    default:
      break;
  }
  if (!node.display_name.empty()) {
    out += " name=";
    out += escape_text(node.display_name);
  }
  return out;
}

std::string describe_edge(const Topology&, const Edge& edge) {
  std::string out;
  out.reserve(128);
  out += "edge ";
  out += edge.id.str();
  out += ' ';
  out += to_token(edge.kind);
  out += ' ';
  out += edge.from.node.str();
  out += '.';
  out += to_token(edge.from.port);
  out += " -> ";
  out += edge.to.node.str();
  out += '.';
  out += to_token(edge.to.port);
  return out;
}

}  // namespace dccp::cooling_topology
