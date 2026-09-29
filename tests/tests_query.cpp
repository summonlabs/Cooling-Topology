// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_query.cpp - structural query proof obligations.
//
// Every answer checked here is a statement about connectivity in one generation.
// Where a query claims a set, a distance, a count or a path, an independent
// reference (a plain BFS, a DFS enumeration, a unit-capacity max flow, a
// union-find) computes the same answer from the raw edge table. Nothing below
// asserts that a component is running, that medium is flowing, that a route has
// capacity or that a source is eligible: those claims are NotOwned.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace ct_test;
namespace ct = dccp::cooling_topology;

/// Asserts that an operation was rejected with a documented code. The error
/// message is never inspected: the stable ErrorCode is the contract.
#define CT_CHECK_CODE(expression, expected)                                                        \
  do {                                                                                             \
    const auto& ct_outcome_ = (expression);                                                        \
    if (ct_outcome_.has_value() || ct_outcome_.error().code() != (expected)) {                     \
      ::ct_test::report_failure(                                                                   \
          __FILE__, __LINE__, #expression " rejected with " #expected,                             \
          ct_outcome_.has_value()                                                                  \
              ? std::string("accepted")                                                            \
              : std::string(::dccp::cooling_topology::error_code_name(ct_outcome_.error().code()))); \
    }                                                                                              \
  } while (false)

// ---------------------------------------------------------------------------
// The claim boundary
// ---------------------------------------------------------------------------

/// The eleven excluded claims are exactly values 0..10 and every one of them is
/// NotOwned. This is a total function, so no result can carry an operational
/// claim.
void check_claim_boundary() {
  static_assert(static_cast<std::uint8_t>(ct::ExcludedClaim::PowerState) == 10);
  static_assert(static_cast<std::uint8_t>(ct::ClaimDisposition::NotOwned) == 0);
  static_assert(static_cast<std::uint8_t>(ct::ClaimClass::StructurallyPossible) == 0);
  static_assert(static_cast<std::uint8_t>(ct::ClaimClass::StructurallyImpossible) == 1);
  CT_CHECK(!ct::posture_statement().empty());
  for (std::uint8_t index = 0; index < 11; ++index) {
    const auto claim = static_cast<ct::ExcludedClaim>(index);
    CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(claim), ct::ClaimDisposition::NotOwned);
    CT_CHECK(!ct::to_token(claim).empty());
  }
}

/// Every public answer carries an EvidencePosture; referencing the member in a
/// template is a compile-time proof that the field exists on every result type
/// this test suite instantiates.
template <class Answer>
void check_posture(const Answer& answer) {
  CT_CHECK_EQ(answer.posture, ct::EvidencePosture{});
}

/// Every answer that classifies a structural finding uses one of exactly two
/// claim classes, never a third operational one.
template <class Answer>
void check_claim_class(const Answer& answer) {
  const bool possible = answer.claim == ct::ClaimClass::StructurallyPossible;
  const bool impossible = answer.claim == ct::ClaimClass::StructurallyImpossible;
  CT_CHECK(possible != impossible);
}

// ---------------------------------------------------------------------------
// Independent reference machinery
// ---------------------------------------------------------------------------

std::string join_strings(const std::vector<std::string>& values) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) {
      out += ',';
    }
    out += value;
  }
  return out;
}

template <class Value>
std::string join_ids(const std::vector<Value>& values) {
  std::string out;
  for (const Value& value : values) {
    if (!out.empty()) {
      out += ',';
    }
    out += value.str();
  }
  return out;
}

std::string canonical_text(const ct::Topology& topology, const ct::NodeId& identity) {
  const auto resolved = topology.resolve(identity);
  return resolved.has_value() ? resolved.value().str() : identity.str();
}

bool reference_supplier_kind(ct::NodeKind kind) {
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

bool reference_consumer_kind(ct::NodeKind kind) {
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

ct::NodeKind kind_of_node(const ct::Topology& topology, const std::string& text) {
  const auto parsed = ct::NodeId::parse(text);
  const ct::Node* node = parsed.has_value() ? topology.find_node(parsed.value()) : nullptr;
  return node == nullptr ? ct::NodeKind::ThermalZone : node->kind();
}

/// Directed adjacency over Supplies edges, keyed by canonical identity text.
/// Built from the public edge table only.
std::vector<std::pair<std::string, std::vector<std::string>>> supplies_adjacency(const ct::Topology& topology,
                                                                                bool incoming) {
  std::vector<std::pair<std::string, std::vector<std::string>>> adjacency;
  for (const ct::Edge& edge : topology.edges()) {
    if (edge.kind != ct::EdgeKind::Supplies) {
      continue;
    }
    const std::string from = canonical_text(topology, edge.from.node);
    const std::string to = canonical_text(topology, edge.to.node);
    const std::string key = incoming ? to : from;
    const std::string value = incoming ? from : to;
    auto slot = std::find_if(adjacency.begin(), adjacency.end(),
                             [&key](const auto& entry) { return entry.first == key; });
    if (slot == adjacency.end()) {
      adjacency.push_back({key, {value}});
    } else {
      slot->second.push_back(value);
    }
  }
  for (auto& entry : adjacency) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  std::sort(adjacency.begin(), adjacency.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  return adjacency;
}

std::vector<std::string> neighbours_of(const std::vector<std::pair<std::string, std::vector<std::string>>>& adjacency,
                                       const std::string& node) {
  const auto slot = std::find_if(adjacency.begin(), adjacency.end(),
                                 [&node](const auto& entry) { return entry.first == node; });
  return slot == adjacency.end() ? std::vector<std::string>{} : slot->second;
}

/// Plain breadth-first closure over the supplies edges: identity plus shortest
/// depth, sorted by identity. The origin is excluded.
struct ReferenceClosure {
  std::vector<std::pair<std::string, std::size_t>> reached;  ///< (identity, shortest depth), sorted
};

ReferenceClosure reference_closure(const ct::Topology& topology, const ct::NodeId& origin, bool incoming,
                                   std::size_t depth_bound = std::numeric_limits<std::size_t>::max()) {
  const auto adjacency = supplies_adjacency(topology, incoming);
  const std::string start = canonical_text(topology, origin);
  ReferenceClosure closure;
  std::vector<std::pair<std::string, std::size_t>> frontier{{start, 0}};
  std::vector<std::string> seen{start};
  for (std::size_t depth = 0; depth < depth_bound && !frontier.empty(); ++depth) {
    std::vector<std::pair<std::string, std::size_t>> next;
    for (const auto& current : frontier) {
      for (const std::string& neighbour : neighbours_of(adjacency, current.first)) {
        if (std::find(seen.begin(), seen.end(), neighbour) != seen.end()) {
          continue;
        }
        seen.push_back(neighbour);
        closure.reached.emplace_back(neighbour, depth + 1);
        next.emplace_back(neighbour, depth + 1);
      }
    }
    std::sort(next.begin(), next.end());
    frontier.swap(next);
  }
  std::sort(closure.reached.begin(), closure.reached.end());
  return closure;
}

std::string closure_signature(const ReferenceClosure& closure) {
  std::vector<std::string> parts;
  parts.reserve(closure.reached.size());
  for (const auto& entry : closure.reached) {
    parts.push_back(entry.first + "@" + std::to_string(entry.second));
  }
  return join_strings(parts);
}

std::string reached_signature(const std::vector<ct::ReachedElement>& elements, bool sorted) {
  std::vector<std::string> parts;
  parts.reserve(elements.size());
  for (const ct::ReachedElement& element : elements) {
    parts.push_back(element.node.str() + "@" + std::to_string(element.depth));
  }
  if (sorted) {
    std::sort(parts.begin(), parts.end());
  }
  return join_strings(parts);
}

/// All simple supplies paths from origin to target, enumerated by a plain DFS
/// over the raw edge table. Empty when the origin is the target: a supply path
/// has at least one edge.
std::vector<std::string> reference_simple_paths(const ct::Topology& topology, const ct::NodeId& from,
                                                const ct::NodeId& to) {
  const auto adjacency = supplies_adjacency(topology, false);
  const std::string start = canonical_text(topology, from);
  const std::string target = canonical_text(topology, to);
  std::vector<std::string> found;
  if (start == target) {
    return found;
  }
  std::vector<std::string> stack{start};
  const auto expand = [&](auto&& self, const std::string& current) -> void {
    if (current == target) {
      found.push_back(join_strings(stack));
      return;
    }
    for (const std::string& next : neighbours_of(adjacency, current)) {
      if (std::find(stack.begin(), stack.end(), next) != stack.end()) {
        continue;
      }
      stack.push_back(next);
      self(self, next);
      stack.pop_back();
    }
  };
  expand(expand, start);
  std::sort(found.begin(), found.end());
  return found;
}

std::string path_signature(const ct::CoolingPath& path) { return join_ids(path.nodes); }

std::vector<std::string> paths_signature(const std::vector<ct::CoolingPath>& paths) {
  std::vector<std::string> parts;
  parts.reserve(paths.size());
  for (const ct::CoolingPath& path : paths) {
    parts.push_back(path_signature(path));
  }
  std::sort(parts.begin(), parts.end());
  return parts;
}

/// A returned path must be a walkable, simple schedule of Supplies edges.
void check_supply_path(const ct::Topology& topology, const ct::CoolingPath& path, const ct::NodeId& from,
                       const ct::NodeId& to) {
  CT_REQUIRE(!path.nodes.empty());
  CT_CHECK_EQ(path.nodes.front().str(), canonical_text(topology, from));
  CT_CHECK_EQ(path.nodes.back().str(), canonical_text(topology, to));
  CT_CHECK_EQ(path.nodes.size(), path.edges.size() + 1);
  for (std::size_t index = 0; index < path.edges.size(); ++index) {
    const ct::Edge* edge = topology.find_edge(path.edges[index]);
    CT_REQUIRE(edge != nullptr);
    CT_CHECK_EQ(edge->kind, ct::EdgeKind::Supplies);
    CT_CHECK_EQ(canonical_text(topology, edge->from.node), path.nodes[index].str());
    CT_CHECK_EQ(canonical_text(topology, edge->to.node), path.nodes[index + 1].str());
  }
  std::vector<std::string> visited;
  for (const ct::NodeId& node : path.nodes) {
    CT_CHECK(std::find(visited.begin(), visited.end(), node.str()) == visited.end());
    visited.push_back(node.str());
  }
}

/// Maximum number of edge-disjoint supplies routes, computed as a unit-capacity
/// max flow (BFS augmenting paths over a residual network). Deliberately a
/// different algorithm from the greedy search the library uses.
std::size_t reference_edge_disjoint_paths(const ct::Topology& topology, const ct::NodeId& from,
                                          const ct::NodeId& to) {
  const std::vector<ct::Node>& nodes = topology.nodes();
  const auto index_of = [&nodes](const std::string& text) -> std::size_t {
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (nodes[index].id.value() == text) {
        return index;
      }
    }
    return nodes.size();
  };
  const std::size_t source = index_of(canonical_text(topology, from));
  const std::size_t sink = index_of(canonical_text(topology, to));
  if (source >= nodes.size() || sink >= nodes.size() || source == sink) {
    return 0;
  }
  struct Arc {
    std::size_t edge;
    bool forward;
  };
  std::vector<std::size_t> tails;
  std::vector<std::size_t> heads;
  std::vector<std::vector<Arc>> adjacency(nodes.size());
  for (const ct::Edge& edge : topology.edges()) {
    if (edge.kind != ct::EdgeKind::Supplies) {
      continue;
    }
    const std::size_t tail = index_of(canonical_text(topology, edge.from.node));
    const std::size_t head = index_of(canonical_text(topology, edge.to.node));
    if (tail >= nodes.size() || head >= nodes.size() || tail == head) {
      continue;
    }
    const std::size_t id = tails.size();
    tails.push_back(tail);
    heads.push_back(head);
    adjacency[tail].push_back(Arc{id, true});
    adjacency[head].push_back(Arc{id, false});
  }
  std::vector<std::uint8_t> capacity(tails.size() * 2, 0);
  for (std::size_t index = 0; index < tails.size(); ++index) {
    capacity[index * 2] = 1;
  }
  std::size_t flow = 0;
  while (true) {
    std::vector<std::size_t> parent_slot(nodes.size(), std::numeric_limits<std::size_t>::max());
    std::vector<std::uint8_t> seen(nodes.size(), 0);
    std::vector<std::size_t> queue{source};
    seen[source] = 1;
    for (std::size_t head = 0; head < queue.size() && seen[sink] == 0; ++head) {
      for (const Arc& arc : adjacency[queue[head]]) {
        const std::size_t slot = arc.edge * 2 + (arc.forward ? 0 : 1);
        if (capacity[slot] == 0) {
          continue;
        }
        const std::size_t other = arc.forward ? heads[arc.edge] : tails[arc.edge];
        if (seen[other] != 0) {
          continue;
        }
        seen[other] = 1;
        parent_slot[other] = slot;
        queue.push_back(other);
        if (other == sink) {
          break;
        }
      }
    }
    if (seen[sink] == 0) {
      break;
    }
    for (std::size_t node = sink; node != source;) {
      const std::size_t slot = parent_slot[node];
      capacity[slot] = 0;
      capacity[slot ^ 1] = 1;
      node = (slot % 2 == 0) ? tails[slot / 2] : heads[slot / 2];
    }
    ++flow;
    if (flow > tails.size()) {
      break;  // unit capacities bound the flow; a longer run would be a defect
    }
  }
  return flow;
}

/// Structural origins over the raw table: supplier kinds with no incoming
/// Supplies edge, in canonical order.
std::vector<std::string> reference_origins(const ct::Topology& topology) {
  std::vector<std::string> origins;
  for (const ct::Node& node : topology.nodes()) {
    if (!reference_supplier_kind(node.kind())) {
      continue;
    }
    bool fed = false;
    for (const ct::Edge& edge : topology.edges()) {
      if (edge.kind == ct::EdgeKind::Supplies && canonical_text(topology, edge.to.node) == node.id.str()) {
        fed = true;
        break;
      }
    }
    if (!fed) {
      origins.push_back(node.id.str());
    }
  }
  std::sort(origins.begin(), origins.end());
  return origins;
}

/// Structural sources that can reach one element, by backwards traversal.
std::vector<std::string> reference_sources_reaching(const ct::Topology& topology, const ct::NodeId& target) {
  const auto adjacency = supplies_adjacency(topology, true);
  const std::string start = canonical_text(topology, target);
  std::vector<std::string> seen{start};
  std::vector<std::string> frontier{start};
  std::vector<std::string> sources;
  while (!frontier.empty()) {
    std::vector<std::string> next;
    for (const std::string& current : frontier) {
      const ct::NodeKind kind = kind_of_node(topology, current);
      if (reference_supplier_kind(kind)) {
        bool fed = false;
        for (const ct::Edge& edge : topology.edges()) {
          if (edge.kind == ct::EdgeKind::Supplies && canonical_text(topology, edge.to.node) == current) {
            fed = true;
            break;
          }
        }
        if (!fed) {
          sources.push_back(current);
        }
      }
      for (const std::string& upstream : neighbours_of(adjacency, current)) {
        if (std::find(seen.begin(), seen.end(), upstream) != seen.end()) {
          continue;
        }
        seen.push_back(upstream);
        next.push_back(upstream);
      }
    }
    std::sort(next.begin(), next.end());
    frontier.swap(next);
  }
  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return sources;
}

/// Reachable set from the structural origins when one candidate is removed.
std::vector<std::string> reference_reachable_without(const ct::Topology& topology, const std::string& removed) {
  const auto adjacency = supplies_adjacency(topology, false);
  std::vector<std::string> reached;
  std::vector<std::string> frontier;
  for (const std::string& origin : reference_origins(topology)) {
    if (origin == removed) {
      continue;
    }
    reached.push_back(origin);
    frontier.push_back(origin);
  }
  while (!frontier.empty()) {
    std::vector<std::string> next;
    for (const std::string& current : frontier) {
      for (const std::string& neighbour : neighbours_of(adjacency, current)) {
        if (neighbour == removed || std::find(reached.begin(), reached.end(), neighbour) != reached.end()) {
          continue;
        }
        reached.push_back(neighbour);
        next.push_back(neighbour);
      }
    }
    std::sort(next.begin(), next.end());
    frontier.swap(next);
  }
  return reached;
}

struct ReferenceDependencyPoint {
  std::string node;
  std::vector<std::string> disconnected;
  bool disconnects_all = false;
  bool is_structural_source = false;
};

/// Every element whose removal leaves at least one subject without any
/// structural origin, computed from the raw table.
std::vector<ReferenceDependencyPoint> reference_dependency_points(const ct::Topology& topology,
                                                                 const std::vector<ct::NodeId>& subjects) {
  std::vector<std::string> candidates;
  for (const ct::NodeId& subject : subjects) {
    for (const auto& entry : reference_closure(topology, subject, true).reached) {
      candidates.push_back(entry.first);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  const std::vector<std::string> origins = reference_origins(topology);

  std::vector<ReferenceDependencyPoint> points;
  for (const std::string& candidate : candidates) {
    const std::vector<std::string> reached = reference_reachable_without(topology, candidate);
    ReferenceDependencyPoint point;
    point.node = candidate;
    point.is_structural_source = std::find(origins.begin(), origins.end(), candidate) != origins.end();
    std::size_t probed = 0;
    for (const ct::NodeId& subject : subjects) {
      const std::string spelling = canonical_text(topology, subject);
      if (spelling == candidate) {
        continue;
      }
      const ct::NodeKind kind = kind_of_node(topology, spelling);
      if (!reference_supplier_kind(kind) && !reference_consumer_kind(kind)) {
        continue;
      }
      ++probed;
      if (std::find(reached.begin(), reached.end(), spelling) == reached.end()) {
        point.disconnected.push_back(spelling);
      }
    }
    std::sort(point.disconnected.begin(), point.disconnected.end());
    point.disconnects_all = probed > 0 && point.disconnected.size() == probed;
    if (!point.disconnected.empty()) {
      points.push_back(point);
    }
  }
  return points;
}

std::string dependency_points_signature(const std::vector<ct::DependencyPoint>& points) {
  std::vector<std::string> parts;
  parts.reserve(points.size());
  for (const ct::DependencyPoint& point : points) {
    parts.push_back(point.node.str() + "[" + join_ids(point.disconnected_subjects) + "]" +
                    (point.disconnects_all_subjects ? "all" : "some") +
                    (point.is_structural_source ? "/origin" : "/internal"));
  }
  std::sort(parts.begin(), parts.end());
  return join_strings(parts);
}

std::string reference_dependency_signature(const std::vector<ReferenceDependencyPoint>& points) {
  std::vector<std::string> parts;
  parts.reserve(points.size());
  for (const ReferenceDependencyPoint& point : points) {
    parts.push_back(point.node + "[" + join_strings(point.disconnected) + "]" +
                    (point.disconnects_all ? "all" : "some") +
                    (point.is_structural_source ? "/origin" : "/internal"));
  }
  std::sort(parts.begin(), parts.end());
  return join_strings(parts);
}

/// Undirected connected components over every edge kind, by union-find.
struct ReferenceComponents {
  std::vector<std::string> components;  ///< joined sorted members, list sorted
  std::vector<std::string> isolated;
};

ReferenceComponents reference_components(const ct::Topology& topology) {
  const std::size_t count = topology.nodes().size();
  std::vector<std::size_t> parent(count);
  for (std::size_t index = 0; index < count; ++index) {
    parent[index] = index;
  }
  const auto root = [&parent](std::size_t value) {
    while (parent[value] != value) {
      parent[value] = parent[parent[value]];
      value = parent[value];
    }
    return value;
  };
  const auto index_of = [&topology](const std::string& text) -> std::size_t {
    for (std::size_t index = 0; index < topology.nodes().size(); ++index) {
      if (topology.nodes()[index].id.value() == text) {
        return index;
      }
    }
    return topology.nodes().size();
  };
  std::vector<std::uint8_t> touched(count, 0);
  for (const ct::Edge& edge : topology.edges()) {
    const std::size_t left = index_of(canonical_text(topology, edge.from.node));
    const std::size_t right = index_of(canonical_text(topology, edge.to.node));
    if (left >= count || right >= count) {
      continue;
    }
    touched[left] = 1;
    touched[right] = 1;
    const std::size_t left_root = root(left);
    const std::size_t right_root = root(right);
    if (left_root != right_root) {
      parent[right_root] = left_root;
    }
  }
  std::vector<std::vector<std::string>> groups(count);
  ReferenceComponents result;
  for (std::size_t index = 0; index < count; ++index) {
    if (touched[index] == 0) {
      result.isolated.push_back(topology.nodes()[index].id.str());
      continue;
    }
    groups[root(index)].push_back(topology.nodes()[index].id.str());
  }
  for (std::vector<std::string>& group : groups) {
    if (group.empty()) {
      continue;
    }
    std::sort(group.begin(), group.end());
    result.components.push_back(join_strings(group));
  }
  std::sort(result.components.begin(), result.components.end());
  std::sort(result.isolated.begin(), result.isolated.end());
  return result;
}

struct ReferenceComponentReport {
  std::vector<std::string> components;
  std::vector<std::string> isolated;
};

ReferenceComponentReport component_signature(const ct::ComponentReport& report) {
  ReferenceComponentReport out;
  for (const std::vector<ct::NodeId>& component : report.components) {
    std::vector<std::string> members;
    members.reserve(component.size());
    for (const ct::NodeId& node : component) {
      members.push_back(node.str());
    }
    std::sort(members.begin(), members.end());
    out.components.push_back(join_strings(members));
  }
  std::sort(out.components.begin(), out.components.end());
  for (const ct::NodeId& node : report.isolated) {
    out.isolated.push_back(node.str());
  }
  std::sort(out.isolated.begin(), out.isolated.end());
  return out;
}

bool has_text(const std::vector<std::string>& values, std::string_view text) {
  return std::find(values.begin(), values.end(), text) != values.end();
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// The single-path facility with a second, parallel route from the loop to the
/// same sink. The two routes share no edge but trace back to one source.
ct::TopologyDraft parallel_routes_facility() {
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(manifold_node("manifold:mf2"));
  draft.nodes.push_back(branch_node("branch:br2"));
  draft.nodes.push_back(cdu_node("cdu:c2"));
  draft.edges.push_back(edge("e20", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "manifold:mf2",
                             ct::PortRole::SourceIn));
  draft.edges.push_back(edge("e21", ct::EdgeKind::Supplies, "manifold:mf2", ct::PortRole::SupplyOut, "branch:br2",
                             ct::PortRole::SourceIn));
  draft.edges.push_back(edge("e22", ct::EdgeKind::Supplies, "branch:br2", ct::PortRole::SupplyOut, "cdu:c2",
                             ct::PortRole::SourceIn));
  draft.edges.push_back(edge("e23", ct::EdgeKind::Supplies, "cdu:c2", ct::PortRole::SupplyOut, "sink:r1",
                             ct::PortRole::SourceIn));
  draft.edges.push_back(edge("e24", ct::EdgeKind::Contains, "manifold:mf2", ct::PortRole::Container, "branch:br2",
                             ct::PortRole::Contained));
  return draft;
}

/// Two chains with no structural element in common, sharing one thermal zone.
ct::TopologyDraft independent_pair_facility() {
  ct::TopologyDraft draft = base_draft("dc-pair");
  draft.nodes = {
      source_node("source:fw-a"),    source_node("source:fw-b"),   plant_node("plant:pa"),
      plant_node("plant:pb"),        manifold_node("manifold:ma"), manifold_node("manifold:mb"),
      branch_node("branch:ba"),      branch_node("branch:bb"),     cdu_node("cdu:ca"),
      cdu_node("cdu:cb"),            sink_node("sink:ra", "rack-ra"), sink_node("sink:rb", "rack-rb"),
      zone_node("zone:z"),
  };
  draft.edges = {
      edge("e1", ct::EdgeKind::Supplies, "source:fw-a", ct::PortRole::SupplyOut, "plant:pa", ct::PortRole::SourceIn),
      edge("e2", ct::EdgeKind::Supplies, "source:fw-b", ct::PortRole::SupplyOut, "plant:pb", ct::PortRole::SourceIn),
      edge("e3", ct::EdgeKind::Supplies, "plant:pa", ct::PortRole::SupplyOut, "manifold:ma", ct::PortRole::SourceIn),
      edge("e4", ct::EdgeKind::Supplies, "plant:pb", ct::PortRole::SupplyOut, "manifold:mb", ct::PortRole::SourceIn),
      edge("e5", ct::EdgeKind::Supplies, "manifold:ma", ct::PortRole::SupplyOut, "branch:ba",
           ct::PortRole::SourceIn),
      edge("e6", ct::EdgeKind::Supplies, "manifold:mb", ct::PortRole::SupplyOut, "branch:bb",
           ct::PortRole::SourceIn),
      edge("e7", ct::EdgeKind::Supplies, "branch:ba", ct::PortRole::SupplyOut, "cdu:ca", ct::PortRole::SourceIn),
      edge("e8", ct::EdgeKind::Supplies, "branch:bb", ct::PortRole::SupplyOut, "cdu:cb", ct::PortRole::SourceIn),
      edge("e9", ct::EdgeKind::Supplies, "cdu:ca", ct::PortRole::SupplyOut, "sink:ra", ct::PortRole::SourceIn),
      edge("e10", ct::EdgeKind::Supplies, "cdu:cb", ct::PortRole::SupplyOut, "sink:rb", ct::PortRole::SourceIn),
      edge("e11", ct::EdgeKind::Contains, "manifold:ma", ct::PortRole::Container, "branch:ba",
           ct::PortRole::Contained),
      edge("e12", ct::EdgeKind::Contains, "manifold:mb", ct::PortRole::Container, "branch:bb",
           ct::PortRole::Contained),
      edge("e13", ct::EdgeKind::Contains, "zone:z", ct::PortRole::Container, "sink:ra", ct::PortRole::Contained),
      edge("e14", ct::EdgeKind::Contains, "zone:z", ct::PortRole::Container, "sink:rb", ct::PortRole::Contained),
  };
  return draft;
}

/// One sink fed directly by two plants with different structural sources.
ct::TopologyDraft dual_independent_feed_facility() {
  ct::TopologyDraft draft = base_draft("dc-dual");
  draft.nodes = {source_node("source:fw-a"), source_node("source:fw-b"), plant_node("plant:pa"),
                 plant_node("plant:pb"),   sink_node("sink:r1", "rack-r1"), zone_node("zone:z1")};
  draft.edges = {
      edge("e1", ct::EdgeKind::Supplies, "source:fw-a", ct::PortRole::SupplyOut, "plant:pa", ct::PortRole::SourceIn),
      edge("e2", ct::EdgeKind::Supplies, "source:fw-b", ct::PortRole::SupplyOut, "plant:pb", ct::PortRole::SourceIn),
      edge("e3", ct::EdgeKind::Supplies, "plant:pa", ct::PortRole::SupplyOut, "sink:r1", ct::PortRole::SourceIn),
      edge("e4", ct::EdgeKind::Supplies, "plant:pb", ct::PortRole::SupplyOut, "sink:r1", ct::PortRole::SourceIn),
      edge("e5", ct::EdgeKind::Contains, "zone:z1", ct::PortRole::Container, "sink:r1", ct::PortRole::Contained),
  };
  return draft;
}

/// The single-path facility plus a sink that nothing feeds.
ct::TopologyDraft unfed_sink_facility() {
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(sink_node("sink:r2", "rack-r2"));
  draft.edges.push_back(edge("e12", ct::EdgeKind::Contains, "zone:z1", ct::PortRole::Container, "sink:r2",
                             ct::PortRole::Contained));
  return draft;
}

/// A supply chain with no Returns edge at all.
ct::TopologyDraft no_return_facility() {
  ct::TopologyDraft draft = base_draft("dc-noreturn");
  draft.nodes = {source_node("source:fw"), plant_node("plant:p1"), loop_node("loop:l1"),
                 sink_node("sink:r1", "rack-r1"), zone_node("zone:z1")};
  draft.edges = {
      edge("e1", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut, "plant:p1",
           ct::PortRole::SourceIn),
      edge("e2", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut, "loop:l1", ct::PortRole::SourceIn),
      edge("e3", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "sink:r1", ct::PortRole::SourceIn),
      edge("e4", ct::EdgeKind::Contains, "zone:z1", ct::PortRole::Container, "sink:r1", ct::PortRole::Contained),
  };
  return draft;
}

/// A served zone that contains no sink, and a zone that contains a sink and has
/// no serving element.
ct::TopologyDraft zone_facility() {
  ct::TopologyDraft draft = base_draft("dc-zones");
  draft.nodes = {source_node("source:fw"), plant_node("plant:p1"), loop_node("loop:l1"), crah_node("crah:ah1"),
                 zone_node("zone:served"), zone_node("zone:holds"), sink_node("sink:r1", "rack-r1")};
  draft.edges = {
      edge("e1", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut, "plant:p1",
           ct::PortRole::SourceIn),
      edge("e2", ct::EdgeKind::Supplies, "plant:p1", ct::PortRole::SupplyOut, "loop:l1", ct::PortRole::SourceIn),
      edge("e3", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "crah:ah1", ct::PortRole::SourceIn),
      edge("e4", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut, "sink:r1", ct::PortRole::SourceIn),
      edge("e5", ct::EdgeKind::Serves, "crah:ah1", ct::PortRole::Server, "zone:served", ct::PortRole::Served),
      edge("e6", ct::EdgeKind::Contains, "zone:served", ct::PortRole::Container, "crah:ah1",
           ct::PortRole::Contained),
      edge("e7", ct::EdgeKind::Contains, "zone:holds", ct::PortRole::Container, "sink:r1",
           ct::PortRole::Contained),
  };
  return draft;
}

/// The single-path facility plus an element of any kind with no edge at all, and
/// plus a zone that nothing serves and that contains nothing.
ct::TopologyDraft isolated_element_facility() {
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(plant_node("plant:spare"));
  draft.nodes.push_back(zone_node("zone:empty"));
  return draft;
}

/// Two disjoint subgraphs inside one generation.
ct::TopologyDraft two_subgraphs_facility() {
  ct::TopologyDraft draft = single_path_facility();
  draft.nodes.push_back(source_node("source:fw-b"));
  draft.nodes.push_back(plant_node("plant:p2"));
  draft.edges.push_back(edge("e20", ct::EdgeKind::Supplies, "source:fw-b", ct::PortRole::SupplyOut, "plant:p2",
                             ct::PortRole::SourceIn));
  return draft;
}

// ---------------------------------------------------------------------------
// Closures
// ---------------------------------------------------------------------------

CT_TEST(query_upstream_and_downstream_match_an_independent_closure) {
  const std::string case_name = "query_upstream_and_downstream_match_an_independent_closure";
  const ct::Topology topology = build(reference_facility());

  const auto upstream = ct::upstream_of(topology, nid("sink:rack-a1"));
  CT_REQUIRE(upstream.has_value());
  CT_CHECK(upstream.value().origin == nid("sink:rack-a1"));
  CT_CHECK(!upstream.value().truncated);
  check_claim_class(upstream.value());
  check_posture(upstream.value());
  CT_CHECK_EQ(reached_signature(upstream.value().elements, true),
              closure_signature(reference_closure(topology, nid("sink:rack-a1"), true)));
  // The exact shortest-distance closure: cdu, branch, manifold, secondary,
  // primary, plant and the facility-water source.
  CT_CHECK_EQ(reached_signature(upstream.value().elements, true),
              std::string("branch:b-a1@2,cdu:cdu-a1@1,loop:primary@5,loop:secondary@4,manifold:m-a@3,"
                          "plant:chp-a@6,source:facility-water@7"));
  CT_CHECK(upstream.value().claim == ct::ClaimClass::StructurallyPossible);

  const auto downstream = ct::downstream_of(topology, nid("loop:secondary"));
  CT_REQUIRE(downstream.has_value());
  CT_CHECK(!downstream.value().truncated);
  check_posture(downstream.value());
  CT_CHECK_EQ(reached_signature(downstream.value().elements, true),
              closure_signature(reference_closure(topology, nid("loop:secondary"), false)));
  CT_CHECK_EQ(reached_signature(downstream.value().elements, true),
              std::string("branch:b-a1@2,branch:b-b1@2,cdu:cdu-a1@3,cdu:cdu-b1@3,crah:ir-1@1,manifold:m-a@1,"
                          "manifold:m-b@1,sink:rack-a1@4,sink:rack-b1@4"));

  // The origin is never part of its own closure, and each element is witnessed
  // by a real edge whose other endpoint sits one level closer to the origin.
  for (const ct::ReachedElement& element : upstream.value().elements) {
    CT_CHECK(!(element.node == upstream.value().origin));
    const ct::Edge* edge = topology.find_edge(element.via_edge);
    CT_REQUIRE(edge != nullptr);
    CT_CHECK_EQ(edge->kind, ct::EdgeKind::Supplies);
    CT_CHECK_EQ(edge->from.node.str(), element.node.str());
    CT_CHECK_EQ(element.entered_port, edge->from.port);
  }
  for (const ct::ReachedElement& element : downstream.value().elements) {
    CT_CHECK(!(element.node == downstream.value().origin));
    const ct::Edge* edge = topology.find_edge(element.via_edge);
    CT_REQUIRE(edge != nullptr);
    CT_CHECK_EQ(edge->kind, ct::EdgeKind::Supplies);
    CT_CHECK_EQ(edge->to.node.str(), element.node.str());
    CT_CHECK_EQ(element.entered_port, edge->to.port);
  }

  // Determinism across repeated calls, including the discovery order.
  const auto upstream_again = ct::upstream_of(topology, nid("sink:rack-a1"));
  CT_REQUIRE(upstream_again.has_value());
  CT_CHECK_EQ(reached_signature(upstream_again.value().elements, false),
              reached_signature(upstream.value().elements, false));
  const auto downstream_again = ct::downstream_of(topology, nid("loop:secondary"));
  CT_REQUIRE(downstream_again.has_value());
  CT_CHECK_EQ(reached_signature(downstream_again.value().elements, false),
              reached_signature(downstream.value().elements, false));

  // Nothing upstream of a structural origin: the well-formed question has a
  // structurally impossible answer rather than an empty "possible" one.
  const auto nothing = ct::upstream_of(topology, nid("source:facility-water"));
  CT_REQUIRE(nothing.has_value());
  CT_CHECK(nothing.value().elements.empty());
  CT_CHECK(!nothing.value().truncated);
  CT_CHECK(nothing.value().claim == ct::ClaimClass::StructurallyImpossible);
  const auto dead_end = ct::downstream_of(topology, nid("sink:rack-a1"));
  CT_REQUIRE(dead_end.has_value());
  CT_CHECK(dead_end.value().elements.empty());
  CT_CHECK(dead_end.value().claim == ct::ClaimClass::StructurallyImpossible);

  CT_CHECK_CODE(ct::upstream_of(topology, nid("plant:nobody")), ct::ErrorCode::NotFound);
  CT_CHECK_CODE(ct::downstream_of(topology, nid("plant:nobody")), ct::ErrorCode::NotFound);

  // Seeded permutation of the draft tables must not change any answer: the
  // closure is a property of the structure, not of the input order.
  Rng rng(case_seed(case_name));
  const ct::TopologyDraft base = reference_facility();
  for (std::size_t round = 0; round < 8; ++round) {
    ct::TopologyDraft shuffled = base;
    for (std::size_t remaining = shuffled.nodes.size(); remaining > 1; --remaining) {
      std::swap(shuffled.nodes[remaining - 1], shuffled.nodes[rng.below(static_cast<std::uint32_t>(remaining))]);
    }
    for (std::size_t remaining = shuffled.edges.size(); remaining > 1; --remaining) {
      std::swap(shuffled.edges[remaining - 1], shuffled.edges[rng.below(static_cast<std::uint32_t>(remaining))]);
    }
    const ct::Topology permuted = build(shuffled);
    const auto permuted_upstream = ct::upstream_of(permuted, nid("sink:rack-a1"));
    CT_REQUIRE(permuted_upstream.has_value());
    if (reached_signature(permuted_upstream.value().elements, true) !=
        reached_signature(upstream.value().elements, true)) {
      report_note("round " + std::to_string(round) + " diverged; rerun with --seed=" + std::to_string(run_seed()));
    }
    CT_CHECK_EQ(reached_signature(permuted_upstream.value().elements, true),
                reached_signature(upstream.value().elements, true));
  }
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

CT_TEST(query_possible_supply_paths_are_walkable_and_complete) {
  const ct::Topology parallel = build(parallel_routes_facility());
  const auto two = ct::possible_supply_paths(parallel, nid("loop:l1"), nid("sink:r1"));
  CT_REQUIRE(two.has_value());
  check_posture(two.value());
  check_claim_class(two.value());
  CT_CHECK(two.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK(!two.value().truncated);
  CT_CHECK(two.value().paths.size() >= std::size_t{2});
  for (const ct::CoolingPath& path : two.value().paths) {
    check_supply_path(parallel, path, nid("loop:l1"), nid("sink:r1"));
  }
  // The enumeration is complete: a plain DFS finds exactly the same paths.
  const std::vector<std::string> enumerated = reference_simple_paths(parallel, nid("loop:l1"), nid("sink:r1"));
  CT_CHECK_EQ(join_strings(paths_signature(two.value().paths)), join_strings(enumerated));
  CT_CHECK_EQ(two.value().paths.size(), enumerated.size());
  // Both routes reach the sink through distinct manifolds.
  CT_CHECK(has_text(enumerated, "loop:l1,manifold:mf1,branch:br1,cdu:c1,sink:r1"));
  CT_CHECK(has_text(enumerated, "loop:l1,manifold:mf2,branch:br2,cdu:c2,sink:r1"));
  // The structural sources that can reach the destination.
  CT_CHECK_EQ(join_ids(two.value().reachable_sources),
              join_strings(reference_sources_reaching(parallel, nid("sink:r1"))));
  CT_CHECK_EQ(join_ids(two.value().reachable_sources), std::string("source:fw"));

  const ct::Topology single = build(single_path_facility());
  const auto one = ct::possible_supply_paths(single, nid("source:fw"), nid("sink:r1"));
  CT_REQUIRE(one.has_value());
  CT_CHECK(one.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK_EQ(one.value().paths.size(), std::size_t{1});
  CT_CHECK_EQ(path_signature(one.value().paths.front()),
              std::string("source:fw,plant:p1,loop:l1,manifold:mf1,branch:br1,cdu:c1,sink:r1"));
  CT_CHECK_EQ(join_ids(one.value().reachable_sources), std::string("source:fw"));

  // A dead end has no path, and the claim says so.
  const ct::Topology reference = build(reference_facility());
  const auto none = ct::possible_supply_paths(reference, nid("plant:chp-b"), nid("sink:rack-b1"));
  CT_REQUIRE(none.has_value());
  CT_CHECK(none.value().paths.empty());
  CT_CHECK(none.value().claim == ct::ClaimClass::StructurallyImpossible);
  CT_CHECK_EQ(join_ids(none.value().reachable_sources), std::string("source:facility-water"));

  // The origin is not a path from itself.
  const auto self = ct::possible_supply_paths(single, nid("loop:l1"), nid("loop:l1"));
  CT_REQUIRE(self.has_value());
  CT_CHECK(self.value().paths.empty());
  CT_CHECK(self.value().claim == ct::ClaimClass::StructurallyImpossible);

  // The claim is Possible exactly when at least one path was returned.
  for (const ct::PathQueryResult* result : {&two.value(), &one.value(), &none.value(), &self.value()}) {
    const bool possible = result->claim == ct::ClaimClass::StructurallyPossible;
    CT_CHECK_EQ(possible, !result->paths.empty());
  }

  // A smaller max_paths bounds the enumeration; the caller asked for the bound.
  ct::QueryOptions bounded;
  bounded.max_paths = 1;
  const auto capped = ct::possible_supply_paths(parallel, nid("loop:l1"), nid("sink:r1"), bounded);
  CT_REQUIRE(capped.has_value());
  CT_CHECK_EQ(capped.value().paths.size(), std::size_t{1});
  for (const ct::CoolingPath& path : capped.value().paths) {
    check_supply_path(parallel, path, nid("loop:l1"), nid("sink:r1"));
  }

  CT_CHECK_CODE(ct::possible_supply_paths(reference, nid("plant:nobody"), nid("sink:rack-a1")),
                ct::ErrorCode::NotFound);
  CT_CHECK_CODE(ct::possible_supply_paths(reference, nid("sink:rack-a1"), nid("plant:nobody")),
                ct::ErrorCode::NotFound);
}

CT_TEST(query_possible_circuits_close_a_supply_path_with_a_return_path) {
  const ct::Topology single = build(single_path_facility());
  const auto circuits = ct::possible_circuits(single, nid("cdu:c1"), nid("sink:r1"));
  CT_REQUIRE(circuits.has_value());
  check_posture(circuits.value());
  check_claim_class(circuits.value());
  CT_CHECK(circuits.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_REQUIRE(!circuits.value().circuits.empty());
  for (const ct::CoolingCircuit& circuit : circuits.value().circuits) {
    CT_REQUIRE(!circuit.supply_nodes.empty());
    CT_REQUIRE(!circuit.return_nodes.empty());
    CT_CHECK_EQ(circuit.supply_nodes.front().str(), std::string("cdu:c1"));
    CT_CHECK_EQ(circuit.supply_nodes.back().str(), std::string("sink:r1"));
    CT_CHECK_EQ(circuit.return_nodes.front().str(), std::string("sink:r1"));
    CT_CHECK_EQ(circuit.return_nodes.back().str(), std::string("cdu:c1"));
    CT_CHECK_EQ(circuit.supply_nodes.size(), circuit.supply_edges.size() + 1);
    CT_CHECK_EQ(circuit.return_nodes.size(), circuit.return_edges.size() + 1);
    ct::CoolingPath supply;
    supply.nodes = circuit.supply_nodes;
    supply.edges = circuit.supply_edges;
    check_supply_path(single, supply, nid("cdu:c1"), nid("sink:r1"));
    for (std::size_t index = 0; index < circuit.return_edges.size(); ++index) {
      const ct::Edge* edge = single.find_edge(circuit.return_edges[index]);
      CT_REQUIRE(edge != nullptr);
      CT_CHECK_EQ(edge->kind, ct::EdgeKind::Returns);
      CT_CHECK_EQ(edge->from.node.str(), circuit.return_nodes[index].str());
      CT_CHECK_EQ(edge->to.node.str(), circuit.return_nodes[index + 1].str());
    }
  }

  // A facility with no Returns edge at all can never close a circuit, even
  // though the supply half of the same pair is structurally possible.
  const ct::Topology no_return = build(no_return_facility());
  const auto empty = ct::possible_circuits(no_return, nid("plant:p1"), nid("sink:r1"));
  CT_REQUIRE(empty.has_value());
  CT_CHECK(empty.value().circuits.empty());
  CT_CHECK(empty.value().claim == ct::ClaimClass::StructurallyImpossible);
  const auto open_supply = ct::possible_supply_paths(no_return, nid("plant:p1"), nid("sink:r1"));
  CT_REQUIRE(open_supply.has_value());
  CT_CHECK(open_supply.value().claim == ct::ClaimClass::StructurallyPossible);

  const auto self = ct::possible_circuits(single, nid("sink:r1"), nid("sink:r1"));
  CT_REQUIRE(self.has_value());
  CT_CHECK(self.value().circuits.empty());
  CT_CHECK(self.value().claim == ct::ClaimClass::StructurallyImpossible);
  CT_CHECK_CODE(ct::possible_circuits(single, nid("cdu:c1"), nid("plant:nobody")), ct::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Independent paths
// ---------------------------------------------------------------------------

CT_TEST(query_independent_paths_match_a_max_flow_reference) {
  const ct::Topology parallel = build(parallel_routes_facility());
  const auto two = ct::independent_supply_paths(parallel, nid("loop:l1"), nid("sink:r1"));
  CT_REQUIRE(two.has_value());
  check_posture(two.value());
  check_claim_class(two.value());
  CT_CHECK(two.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK_EQ(two.value().path_count, std::size_t{2});
  CT_CHECK_EQ(two.value().path_count, two.value().paths.size());
  CT_CHECK_EQ(two.value().path_count, reference_edge_disjoint_paths(parallel, nid("loop:l1"), nid("sink:r1")));

  // The returned routes share no edge, and each one is walkable.
  std::vector<std::string> used_edges;
  for (const ct::CoolingPath& path : two.value().paths) {
    check_supply_path(parallel, path, nid("loop:l1"), nid("sink:r1"));
    for (const ct::EdgeId& edge : path.edges) {
      CT_CHECK(!has_text(used_edges, edge.str()));
      used_edges.push_back(edge.str());
    }
  }
  CT_CHECK_EQ(used_edges.size(), std::size_t{8});

  const ct::Topology single = build(single_path_facility());
  const auto one = ct::independent_supply_paths(single, nid("source:fw"), nid("sink:r1"));
  CT_REQUIRE(one.has_value());
  CT_CHECK_EQ(one.value().path_count, std::size_t{1});
  CT_CHECK_EQ(one.value().path_count, reference_edge_disjoint_paths(single, nid("source:fw"), nid("sink:r1")));

  const ct::Topology reference = build(reference_facility());
  const auto reference_count =
      ct::independent_supply_paths(reference, nid("source:facility-water"), nid("sink:rack-a1"));
  CT_REQUIRE(reference_count.has_value());
  CT_CHECK_EQ(reference_count.value().path_count,
              reference_edge_disjoint_paths(reference, nid("source:facility-water"), nid("sink:rack-a1")));
  CT_CHECK_EQ(reference_count.value().path_count, std::size_t{1});

  // A sink nothing feeds has no route and no possibility.
  const ct::Topology unfed = build(unfed_sink_facility());
  const auto none = ct::independent_supply_paths(unfed, nid("loop:l1"), nid("sink:r2"));
  CT_REQUIRE(none.has_value());
  CT_CHECK_EQ(none.value().path_count, std::size_t{0});
  CT_CHECK(none.value().paths.empty());
  CT_CHECK(none.value().claim == ct::ClaimClass::StructurallyImpossible);
  CT_CHECK_EQ(reference_edge_disjoint_paths(unfed, nid("loop:l1"), nid("sink:r2")), std::size_t{0});

  // The origin is not a route to itself.
  const auto self = ct::independent_supply_paths(single, nid("sink:r1"), nid("sink:r1"));
  CT_REQUIRE(self.has_value());
  CT_CHECK_EQ(self.value().path_count, std::size_t{0});
  CT_CHECK(self.value().claim == ct::ClaimClass::StructurallyImpossible);

  // Two feeds from two different plants still give one route each.
  const ct::Topology dual = build(dual_independent_feed_facility());
  const auto dual_count = ct::independent_supply_paths(dual, nid("source:fw-a"), nid("sink:r1"));
  CT_REQUIRE(dual_count.has_value());
  CT_CHECK_EQ(dual_count.value().path_count, std::size_t{1});
  CT_CHECK_EQ(dual_count.value().path_count, reference_edge_disjoint_paths(dual, nid("source:fw-a"), nid("sink:r1")));

  // A search budget that cannot finish reports truncation instead of presenting
  // a partial answer as the graph's structural count.
  ct::QueryOptions starved;
  starved.max_search_nodes = 1;
  const auto starved_result = ct::independent_supply_paths(parallel, nid("loop:l1"), nid("sink:r1"), starved);
  CT_REQUIRE(starved_result.has_value());
  CT_CHECK(starved_result.value().truncated);
  CT_CHECK(starved_result.value().paths.empty());
  CT_CHECK(starved_result.value().claim == ct::ClaimClass::StructurallyImpossible);

  // A caller-supplied path bound is honoured exactly.
  ct::QueryOptions bounded;
  bounded.max_paths = 1;
  const auto capped = ct::independent_supply_paths(parallel, nid("loop:l1"), nid("sink:r1"), bounded);
  CT_REQUIRE(capped.has_value());
  CT_CHECK_EQ(capped.value().path_count, std::size_t{1});
  CT_CHECK_EQ(capped.value().paths.size(), std::size_t{1});

  CT_CHECK_CODE(ct::independent_supply_paths(parallel, nid("plant:nobody"), nid("sink:r1")),
                ct::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Shared dependencies
// ---------------------------------------------------------------------------

CT_TEST(query_common_dependencies_are_the_closure_intersection) {
  const ct::Topology topology = build(reference_facility());
  const std::vector<ct::NodeId> subjects{nid("sink:rack-a1"), nid("sink:rack-b1")};
  const auto common = ct::common_dependencies(topology, subjects);
  CT_REQUIRE(common.has_value());
  check_posture(common.value());
  check_claim_class(common.value());
  CT_CHECK(common.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK(!common.value().truncated);
  CT_CHECK_EQ(join_ids(common.value().subjects), std::string("sink:rack-a1,sink:rack-b1"));

  // Independent reference: the intersection of the two supply closures, minus
  // the subjects themselves.
  std::vector<std::string> intersection;
  for (std::size_t index = 0; index < subjects.size(); ++index) {
    std::vector<std::string> closure;
    for (const auto& entry : reference_closure(topology, subjects[index], true).reached) {
      closure.push_back(entry.first);
    }
    if (index == 0) {
      intersection = closure;
      continue;
    }
    std::vector<std::string> keep;
    for (const std::string& value : intersection) {
      if (std::find(closure.begin(), closure.end(), value) != closure.end()) {
        keep.push_back(value);
      }
    }
    intersection = keep;
  }
  intersection.erase(std::remove_if(intersection.begin(), intersection.end(),
                                    [&topology, &subjects](const std::string& value) {
                                      for (const ct::NodeId& subject : subjects) {
                                        if (canonical_text(topology, subject) == value) {
                                          return true;
                                        }
                                      }
                                      return false;
                                    }),
                     intersection.end());
  CT_CHECK_EQ(join_ids(common.value().dependencies), join_strings(intersection));
  CT_CHECK_EQ(join_ids(common.value().dependencies),
              std::string("loop:primary,loop:secondary,plant:chp-a,source:facility-water"));

  // A single subject has its whole closure as its dependencies.
  const auto single = ct::common_dependencies(topology, {nid("sink:rack-a1")});
  CT_REQUIRE(single.has_value());
  std::vector<std::string> closure;
  for (const auto& entry : reference_closure(topology, nid("sink:rack-a1"), true).reached) {
    closure.push_back(entry.first);
  }
  CT_CHECK_EQ(join_ids(single.value().dependencies), join_strings(closure));

  // Two independent chains share no dependency at all.
  const ct::Topology pair = build(independent_pair_facility());
  const auto disjoint = ct::common_dependencies(pair, {nid("sink:ra"), nid("sink:rb")});
  CT_REQUIRE(disjoint.has_value());
  CT_CHECK(disjoint.value().dependencies.empty());
  CT_CHECK(disjoint.value().claim == ct::ClaimClass::StructurallyImpossible);
  // The independent reference agrees: no subject's closure covers the other.
  for (const ct::NodeId& subject : {nid("sink:ra"), nid("sink:rb")}) {
    const ReferenceClosure pair_closure = reference_closure(pair, subject, true);
    CT_CHECK_EQ(pair_closure.reached.size(), std::size_t{5});
  }

  // Documented rejections: no subjects, an unknown subject, and one past the
  // documented subject bound. Exactly the bound is accepted (and then fails on
  // the unknown identity), which fixes the boundary at 64.
  CT_CHECK_CODE(ct::common_dependencies(topology, {}), ct::ErrorCode::EmptyInput);
  CT_CHECK_CODE(ct::common_dependencies(topology, {nid("plant:nobody")}), ct::ErrorCode::NotFound);
  std::vector<ct::NodeId> at_bound;
  std::vector<ct::NodeId> past_bound;
  for (int index = 0; index < 64; ++index) {
    at_bound.push_back(nid("subject:" + std::to_string(index)));
  }
  past_bound = at_bound;
  past_bound.push_back(nid("subject:64"));
  CT_CHECK_EQ(at_bound.size(), ct::limits::kMaxPathQuerySubjects);
  CT_CHECK_CODE(ct::common_dependencies(topology, past_bound), ct::ErrorCode::LimitExceeded);
  CT_CHECK_CODE(ct::common_dependencies(topology, at_bound), ct::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Single points of structural dependency
// ---------------------------------------------------------------------------

CT_TEST(query_single_points_on_a_single_path_facility) {
  const ct::Topology single = build(single_path_facility());
  const auto points = ct::single_points_of_structural_dependency(single, {nid("sink:r1")});
  CT_REQUIRE(points.has_value());
  check_posture(points.value());
  check_claim_class(points.value());
  CT_CHECK(points.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK(!points.value().truncated);
  CT_CHECK_EQ(join_ids(points.value().subjects), std::string("sink:r1"));

  // Every element on the only route to the sink is a dependency point of it.
  CT_CHECK_EQ(dependency_points_signature(points.value().points),
              reference_dependency_signature(reference_dependency_points(single, {nid("sink:r1")})));
  CT_CHECK_EQ(dependency_points_signature(points.value().points),
              std::string("branch:br1[sink:r1]all/internal,cdu:c1[sink:r1]all/internal,"
                          "loop:l1[sink:r1]all/internal,manifold:mf1[sink:r1]all/internal,"
                          "plant:p1[sink:r1]all/internal,source:fw[sink:r1]all/origin"));
  for (const ct::DependencyPoint& point : points.value().points) {
    CT_CHECK(point.disconnects_all_subjects);
    CT_CHECK_EQ(point.disconnected_subjects.size(), std::size_t{1});
    CT_CHECK_EQ(join_ids(point.disconnected_subjects), std::string("sink:r1"));
    CT_CHECK_EQ(point.is_structural_source, point.node == nid("source:fw"));
  }

  // Documented rejections and the exact subject bound.
  CT_CHECK_CODE(ct::single_points_of_structural_dependency(single, {}), ct::ErrorCode::EmptyInput);
  CT_CHECK_CODE(ct::single_points_of_structural_dependency(single, {nid("plant:nobody")}),
                ct::ErrorCode::NotFound);
  std::vector<ct::NodeId> past_bound;
  for (int index = 0; index < 65; ++index) {
    past_bound.push_back(nid("subject:" + std::to_string(index)));
  }
  CT_CHECK_CODE(ct::single_points_of_structural_dependency(single, past_bound), ct::ErrorCode::LimitExceeded);
}

CT_TEST(query_single_points_do_not_disconnect_independent_subjects) {
  // Two chains that share no structural element: removing one element can never
  // disconnect both sinks, so nothing disconnects all subjects.
  const ct::Topology pair = build(independent_pair_facility());
  const std::vector<ct::NodeId> subjects{nid("sink:ra"), nid("sink:rb")};
  const auto points = ct::single_points_of_structural_dependency(pair, subjects);
  CT_REQUIRE(points.has_value());
  CT_CHECK(!points.value().points.empty());
  CT_CHECK_EQ(dependency_points_signature(points.value().points),
              reference_dependency_signature(reference_dependency_points(pair, subjects)));
  for (const ct::DependencyPoint& point : points.value().points) {
    CT_CHECK(!point.disconnects_all_subjects);
    CT_CHECK(point.disconnected_subjects.size() < subjects.size());
  }

  // The reference facility feeds both manifolds from one shared trunk, so every
  // element of that trunk is a single point for both sinks; nothing downstream
  // of the split is.
  const ct::Topology reference = build(reference_facility());
  const std::vector<ct::NodeId> sinks{nid("sink:rack-a1"), nid("sink:rack-b1")};
  const auto reference_points = ct::single_points_of_structural_dependency(reference, sinks);
  CT_REQUIRE(reference_points.has_value());
  CT_CHECK_EQ(dependency_points_signature(reference_points.value().points),
              reference_dependency_signature(reference_dependency_points(reference, sinks)));
  std::vector<std::string> disconnecting_all;
  for (const ct::DependencyPoint& point : reference_points.value().points) {
    if (point.disconnects_all_subjects) {
      disconnecting_all.push_back(point.node.str());
    }
  }
  CT_CHECK_EQ(join_strings(disconnecting_all),
              std::string("loop:primary,loop:secondary,plant:chp-a,source:facility-water"));
  // Everything downstream of the split feeds exactly one sink, so no manifold,
  // branch or CDU is a single point for both.
  for (const ct::DependencyPoint& point : reference_points.value().points) {
    const ct::NodeKind kind = kind_of_node(reference, point.node.str());
    if (kind == ct::NodeKind::Manifold || kind == ct::NodeKind::Branch || kind == ct::NodeKind::Cdu) {
      CT_CHECK(!point.disconnects_all_subjects);
    }
  }
  // The distribution element that feeds only one sink is a point for that sink.
  bool saw_manifold_a = false;
  for (const ct::DependencyPoint& point : reference_points.value().points) {
    if (point.node == nid("manifold:m-a")) {
      saw_manifold_a = true;
      CT_CHECK_EQ(join_ids(point.disconnected_subjects), std::string("sink:rack-a1"));
      CT_CHECK(!point.disconnects_all_subjects);
    }
  }
  CT_CHECK(saw_manifold_a);
}

// ---------------------------------------------------------------------------
// Connectivity
// ---------------------------------------------------------------------------

CT_TEST(query_connected_components_match_a_union_find_reference) {
  const ct::Topology reference = build(reference_facility());
  const auto components = ct::connected_components(reference);
  CT_REQUIRE(components.has_value());
  check_posture(components.value());
  CT_CHECK(!components.value().truncated);
  const ReferenceComponentReport signature = component_signature(components.value());
  const ReferenceComponents expected = reference_components(reference);
  CT_CHECK_EQ(join_strings(signature.components), join_strings(expected.components));
  CT_CHECK_EQ(join_strings(signature.isolated), join_strings(expected.isolated));
  CT_CHECK_EQ(components.value().components.size(), std::size_t{1});
  CT_CHECK(components.value().isolated.empty());
  CT_CHECK_EQ(components.value().components.front().size(), reference.node_count());

  // One unconnected element is reported as isolated, never as a component.
  const ct::Topology isolated = build(isolated_element_facility());
  const auto with_isolated = ct::connected_components(isolated);
  CT_REQUIRE(with_isolated.has_value());
  const ReferenceComponents isolated_expected = reference_components(isolated);
  CT_CHECK_EQ(join_strings(component_signature(with_isolated.value()).isolated),
              join_strings(isolated_expected.isolated));
  CT_CHECK_EQ(join_strings(component_signature(with_isolated.value()).components),
              join_strings(isolated_expected.components));
  CT_CHECK_EQ(join_ids(with_isolated.value().isolated), std::string("plant:spare,zone:empty"));
  CT_CHECK_EQ(with_isolated.value().components.size(), std::size_t{1});

  // Two disjoint subgraphs are two components.
  const ct::Topology split = build(two_subgraphs_facility());
  const auto two = ct::connected_components(split);
  CT_REQUIRE(two.has_value());
  const ReferenceComponents split_expected = reference_components(split);
  CT_CHECK_EQ(join_strings(component_signature(two.value()).components), join_strings(split_expected.components));
  CT_CHECK_EQ(join_strings(component_signature(two.value()).isolated), join_strings(split_expected.isolated));
  CT_CHECK_EQ(two.value().components.size(), std::size_t{2});
  CT_CHECK(two.value().isolated.empty());

  // Deterministic ordering across repeated calls.
  const auto again = ct::connected_components(split);
  CT_REQUIRE(again.has_value());
  CT_CHECK_EQ(join_strings(component_signature(again.value()).components),
              join_strings(component_signature(two.value()).components));

  // A component bound is reported as truncation, including the zero bound.
  ct::QueryOptions one_component;
  one_component.max_results = 1;
  const auto capped = ct::connected_components(split, one_component);
  CT_REQUIRE(capped.has_value());
  CT_CHECK(capped.value().truncated);
  CT_CHECK_EQ(capped.value().components.size(), std::size_t{1});
  ct::QueryOptions no_component;
  no_component.max_results = 0;
  const auto empty = ct::connected_components(split, no_component);
  CT_REQUIRE(empty.has_value());
  CT_CHECK(empty.value().truncated);
  CT_CHECK(empty.value().components.empty());
}

// ---------------------------------------------------------------------------
// Blast radius
// ---------------------------------------------------------------------------

CT_TEST(query_blast_radius_separates_supply_from_containment) {
  const ct::Topology topology = build(reference_facility());

  const auto radius = ct::blast_radius(topology, nid("manifold:m-a"));
  CT_REQUIRE(radius.has_value());
  check_posture(radius.value());
  check_claim_class(radius.value());
  CT_CHECK(radius.value().claim == ct::ClaimClass::StructurallyPossible);
  const ReferenceClosure downstream = reference_closure(topology, nid("manifold:m-a"), false);
  CT_CHECK_EQ(reached_signature(radius.value().supplied_downstream, true), closure_signature(downstream));
  CT_CHECK_EQ(reached_signature(radius.value().supplied_downstream, true),
              std::string("branch:b-a1@1,cdu:cdu-a1@2,sink:rack-a1@3"));
  CT_CHECK_EQ(join_ids(radius.value().affected_sinks), std::string("sink:rack-a1"));
  CT_CHECK(radius.value().affected_zones.empty());

  // Containment peers are reported separately and are never mixed into the
  // supplied set: the loop and the sibling manifold share an enclosure with the
  // origin but are not downstream of it.
  CT_CHECK_EQ(radius.value().containment_peers.size(), std::size_t{3});
  bool saw_container = false;
  bool saw_sibling = false;
  bool saw_contained = false;
  for (const ct::ContainmentPeer& peer : radius.value().containment_peers) {
    const bool downstream_peer =
        std::find_if(downstream.reached.begin(), downstream.reached.end(), [&peer](const auto& entry) {
          return entry.first == peer.node.str();
        }) != downstream.reached.end();
    if (peer.node == nid("loop:secondary")) {
      saw_container = true;
      CT_CHECK(peer.contains_origin);
      CT_CHECK(!downstream_peer);
    }
    if (peer.node == nid("manifold:m-b")) {
      saw_sibling = true;
      CT_CHECK(peer.sibling);
      CT_CHECK(!downstream_peer);
    }
    if (peer.node == nid("branch:b-a1")) {
      saw_contained = true;
      CT_CHECK(peer.contained_by_origin);
      CT_CHECK(downstream_peer);
    }
  }
  CT_CHECK(saw_container && saw_sibling && saw_contained);

  // A thermal zone is affected, but the sinks it contains are reached through
  // containment alone, so they are peers and not supplied downstream.
  const auto zone_radius = ct::blast_radius(topology, nid("zone:cold-aisle-1"));
  CT_REQUIRE(zone_radius.has_value());
  CT_CHECK(zone_radius.value().claim == ct::ClaimClass::StructurallyImpossible);
  CT_CHECK(zone_radius.value().supplied_downstream.empty());
  CT_CHECK(zone_radius.value().affected_sinks.empty());
  CT_CHECK_EQ(join_ids(zone_radius.value().affected_zones), std::string("zone:cold-aisle-1"));
  CT_CHECK_EQ(zone_radius.value().containment_peers.size(), std::size_t{3});
  for (const ct::ContainmentPeer& peer : zone_radius.value().containment_peers) {
    CT_CHECK(peer.contained_by_origin);
  }

  // A sink has no downstream at all; its containment peers are the zone and the
  // siblings in that zone.
  const auto sink_radius = ct::blast_radius(topology, nid("sink:rack-a1"));
  CT_REQUIRE(sink_radius.has_value());
  CT_CHECK(sink_radius.value().supplied_downstream.empty());
  // The origin is part of its own blast radius; the sinks in the same enclosure
  // are peers and not affected sinks.
  CT_CHECK_EQ(join_ids(sink_radius.value().affected_sinks), std::string("sink:rack-a1"));
  CT_CHECK(sink_radius.value().affected_zones.empty());
  CT_CHECK_EQ(sink_radius.value().containment_peers.size(), std::size_t{3});

  // Containment peers are optional; the supply answer must not change.
  ct::QueryOptions without_peers;
  without_peers.include_containment_peers = false;
  const auto no_peers = ct::blast_radius(topology, nid("manifold:m-a"), without_peers);
  CT_REQUIRE(no_peers.has_value());
  CT_CHECK(no_peers.value().containment_peers.empty());
  CT_CHECK_EQ(reached_signature(no_peers.value().supplied_downstream, true),
              reached_signature(radius.value().supplied_downstream, true));

  CT_CHECK_CODE(ct::blast_radius(topology, nid("plant:nobody")), ct::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Sinks and zones
// ---------------------------------------------------------------------------

CT_TEST(query_sink_service_report_verdicts) {
  const ct::Topology single = build(single_path_facility());
  const auto one_feed = ct::sink_service_report(single, nid("sink:r1"));
  CT_REQUIRE(one_feed.has_value());
  check_posture(one_feed.value());
  CT_CHECK(one_feed.value().verdict == ct::SinkFeedVerdict::SingleFeed);
  CT_CHECK_EQ(one_feed.value().feeds.size(), std::size_t{1});
  CT_CHECK_EQ(one_feed.value().feeds.front().feeder.str(), std::string("cdu:c1"));
  CT_CHECK_EQ(one_feed.value().feeds.front().edge.str(), std::string("e6"));
  CT_CHECK_EQ(join_ids(one_feed.value().feeds.front().sources), std::string("source:fw"));
  CT_CHECK(one_feed.value().shared_sources.empty());

  // Two feeders that trace back to the same structural source.
  const ct::Topology parallel = build(parallel_routes_facility());
  const auto shared = ct::sink_service_report(parallel, nid("sink:r1"));
  CT_REQUIRE(shared.has_value());
  CT_CHECK(shared.value().verdict == ct::SinkFeedVerdict::MultipleFeedsSharingDependency);
  CT_CHECK_EQ(shared.value().feeds.size(), std::size_t{2});
  CT_CHECK_EQ(join_ids(shared.value().shared_sources), std::string("source:fw"));
  for (const ct::SinkFeed& feed : shared.value().feeds) {
    CT_CHECK_EQ(join_ids(feed.sources), join_strings(reference_sources_reaching(parallel, feed.feeder)));
  }

  // Two feeders with disjoint source sets: two independent structural feeds.
  const ct::Topology dual = build(dual_independent_feed_facility());
  const auto independent = ct::sink_service_report(dual, nid("sink:r1"));
  CT_REQUIRE(independent.has_value());
  CT_CHECK(independent.value().verdict == ct::SinkFeedVerdict::MultipleIndependentFeeds);
  CT_CHECK_EQ(independent.value().feeds.size(), std::size_t{2});
  CT_CHECK(independent.value().shared_sources.empty());
  CT_CHECK_EQ(join_ids(independent.value().feeds[0].sources), std::string("source:fw-a"));
  CT_CHECK_EQ(join_ids(independent.value().feeds[1].sources), std::string("source:fw-b"));

  // A sink nothing feeds.
  const ct::Topology unfed = build(unfed_sink_facility());
  const auto none = ct::sink_service_report(unfed, nid("sink:r2"));
  CT_REQUIRE(none.has_value());
  CT_CHECK(none.value().verdict == ct::SinkFeedVerdict::NoStructuralFeed);
  CT_CHECK(none.value().feeds.empty());
  CT_CHECK(none.value().shared_sources.empty());
  CT_CHECK(none.value().kind == ct::SinkKind::RackLoad);

  CT_CHECK_CODE(ct::sink_service_report(single, nid("loop:l1")), ct::ErrorCode::InvalidArgument);
  CT_CHECK_CODE(ct::sink_service_report(single, nid("sink:nobody")), ct::ErrorCode::NotFound);
}

CT_TEST(query_zone_report_verdicts) {
  const ct::Topology topology = build(reference_facility());
  const auto served = ct::zone_report(topology, nid("zone:cold-aisle-1"));
  CT_REQUIRE(served.has_value());
  check_posture(served.value());
  CT_CHECK(served.value().verdict == ct::ZoneVerdict::Served);
  CT_CHECK_EQ(served.value().zone_class, ct::ZoneClass::ColdAisle);
  CT_CHECK_EQ(join_ids(served.value().serving_elements), std::string("crah:ir-1"));
  CT_CHECK_EQ(join_ids(served.value().contained_sinks), std::string("sink:rack-a1,sink:rack-b1"));
  CT_CHECK_EQ(join_ids(served.value().contained_air_handlers), std::string("crah:ir-1"));
  CT_CHECK(served.value().groups.empty());

  // A redundancy group whose member serves the zone is reported on the zone.
  ct::TopologyDraft with_group = reference_facility();
  auto air_group = ct::RedundancyGroup::create(gid("group:air"), ct::RedundancyScheme::NPlusOne,
                                               ct::RedundancyScope::AirHandling, "air handlers", "one crah",
                                               {ct::RedundancyMember{nid("crah:ir-1"), "crah:ir-1", std::nullopt}},
                                               false, false);
  CT_REQUIRE(air_group.has_value());
  with_group.groups.push_back(*air_group);
  const auto grouped = ct::zone_report(build(with_group), nid("zone:cold-aisle-1"));
  CT_REQUIRE(grouped.has_value());
  CT_CHECK_EQ(join_ids(grouped.value().groups), std::string("group:air"));

  // A zone that contains a sink and nothing serves it.
  const auto unserved = ct::zone_report(build(single_path_facility()), nid("zone:z1"));
  CT_REQUIRE(unserved.has_value());
  CT_CHECK(unserved.value().verdict == ct::ZoneVerdict::NoServingElement);
  CT_CHECK(unserved.value().serving_elements.empty());
  CT_CHECK_EQ(join_ids(unserved.value().contained_sinks), std::string("sink:r1"));

  // A served zone that contains no sink.
  const ct::Topology zones = build(zone_facility());
  const auto no_sink = ct::zone_report(zones, nid("zone:served"));
  CT_REQUIRE(no_sink.has_value());
  CT_CHECK(no_sink.value().verdict == ct::ZoneVerdict::NoContainedSink);
  CT_CHECK_EQ(join_ids(no_sink.value().serving_elements), std::string("crah:ah1"));
  CT_CHECK(no_sink.value().contained_sinks.empty());
  CT_CHECK_EQ(join_ids(no_sink.value().contained_air_handlers), std::string("crah:ah1"));

  // Neither a serving element nor a contained sink.
  const auto unpopulated = ct::zone_report(build(isolated_element_facility()), nid("zone:empty"));
  CT_REQUIRE(unpopulated.has_value());
  CT_CHECK(unpopulated.value().verdict == ct::ZoneVerdict::Unpopulated);
  CT_CHECK(unpopulated.value().serving_elements.empty());
  CT_CHECK(unpopulated.value().contained_sinks.empty());
  CT_CHECK(unpopulated.value().contained_air_handlers.empty());

  CT_CHECK_CODE(ct::zone_report(topology, nid("loop:secondary")), ct::ErrorCode::InvalidArgument);
  CT_CHECK_CODE(ct::zone_report(topology, nid("zone:nobody")), ct::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

CT_TEST(query_options_bounds_report_truncation_and_defaults) {
  // Documented defaults.
  const ct::QueryOptions defaults;
  CT_CHECK_EQ(defaults.max_depth, ct::limits::kMaxQueryDepth);
  CT_CHECK_EQ(defaults.max_results, ct::limits::kMaxQueryResultCount);
  CT_CHECK_EQ(defaults.max_paths, ct::limits::kMaxPathCount);
  CT_CHECK_EQ(defaults.max_candidates, std::size_t{4096});
  CT_CHECK_EQ(defaults.max_search_nodes, ct::limits::kMaxIndependentPathSearch);
  CT_CHECK(defaults.include_containment_peers);

  const ct::Topology topology = build(reference_facility());
  const auto complete = ct::upstream_of(topology, nid("sink:rack-a1"));
  CT_REQUIRE(complete.has_value());
  CT_CHECK(!complete.value().truncated);

  // A depth bound truncates and says so.
  ct::QueryOptions shallow;
  shallow.max_depth = 1;
  const auto one_level = ct::upstream_of(topology, nid("sink:rack-a1"), shallow);
  CT_REQUIRE(one_level.has_value());
  CT_CHECK(one_level.value().truncated);
  CT_CHECK_EQ(reached_signature(one_level.value().elements, true), std::string("cdu:cdu-a1@1"));

  // A zero depth bound reaches nothing and still reports the bound as hit.
  ct::QueryOptions no_depth;
  no_depth.max_depth = 0;
  const auto nothing = ct::upstream_of(topology, nid("sink:rack-a1"), no_depth);
  CT_REQUIRE(nothing.has_value());
  CT_CHECK(nothing.value().truncated);
  CT_CHECK(nothing.value().elements.empty());

  // A result bound truncates to a prefix, never past it.
  ct::QueryOptions few;
  few.max_results = 1;
  const auto one_result = ct::upstream_of(topology, nid("sink:rack-a1"), few);
  CT_REQUIRE(one_result.has_value());
  CT_CHECK(one_result.value().truncated);
  CT_CHECK_EQ(one_result.value().elements.size(), std::size_t{1});
  CT_CHECK_EQ(reached_signature(one_result.value().elements, true), std::string("cdu:cdu-a1@1"));

  // A zero result bound returns no element at all and is flagged, not silently
  // presented as "nothing upstream exists".
  ct::QueryOptions none;
  none.max_results = 0;
  const auto zero = ct::downstream_of(topology, nid("loop:secondary"), none);
  CT_REQUIRE(zero.has_value());
  CT_CHECK(zero.value().truncated);
  CT_CHECK(zero.value().elements.empty());

  // The truncated prefix is a subset of the complete answer, and the complete
  // answer is not truncated.
  for (const ct::ReachedElement& element : one_result.value().elements) {
    bool present = false;
    for (const ct::ReachedElement& full : complete.value().elements) {
      present = present || full.node == element.node;
    }
    CT_CHECK(present);
  }

  ct::QueryOptions depth_two;
  depth_two.max_depth = 2;
  const auto two_levels = ct::downstream_of(topology, nid("loop:secondary"), depth_two);
  CT_REQUIRE(two_levels.has_value());
  CT_CHECK(two_levels.value().truncated);
  for (const ct::ReachedElement& element : two_levels.value().elements) {
    CT_CHECK(element.depth <= 2);
  }
  CT_CHECK_EQ(two_levels.value().elements.size(), std::size_t{5});

  // The whole reference facility is reachable from the source within bounds.
  const auto all = ct::downstream_of(topology, nid("source:facility-water"));
  CT_REQUIRE(all.has_value());
  CT_CHECK(!all.value().truncated);
  const ReferenceClosure full = reference_closure(topology, nid("source:facility-water"), false);
  CT_CHECK_EQ(all.value().elements.size(), full.reached.size());
  CT_CHECK_EQ(all.value().elements.size(), std::size_t{13});
  CT_CHECK_EQ(reached_signature(all.value().elements, true), closure_signature(full));
}

// ---------------------------------------------------------------------------
// Posture on every answer type
// ---------------------------------------------------------------------------

CT_TEST(query_every_answer_type_carries_the_claim_boundary) {
  check_claim_boundary();
  const ct::Topology topology = build(reference_facility());
  const std::vector<ct::NodeId> one{nid("sink:rack-a1")};

  const auto upstream = ct::upstream_of(topology, nid("sink:rack-a1"));
  const auto downstream = ct::downstream_of(topology, nid("sink:rack-a1"));
  const auto paths = ct::possible_supply_paths(topology, nid("source:facility-water"), nid("sink:rack-a1"));
  const auto circuits = ct::possible_circuits(topology, nid("cdu:cdu-a1"), nid("sink:rack-a1"));
  const auto independent = ct::independent_supply_paths(topology, nid("source:facility-water"), nid("sink:rack-a1"));
  const auto common = ct::common_dependencies(topology, one);
  const auto points = ct::single_points_of_structural_dependency(topology, one);
  const auto membership = ct::redundancy_membership(topology, nid("plant:chp-a"));
  const auto group = ct::redundancy_group_report(topology, gid("group:plants"));
  const auto sink = ct::sink_service_report(topology, nid("sink:rack-a1"));
  const auto zone = ct::zone_report(topology, nid("zone:cold-aisle-1"));
  const auto radius = ct::blast_radius(topology, nid("manifold:m-a"));
  const auto components = ct::connected_components(topology);

  CT_REQUIRE(upstream.has_value() && downstream.has_value() && paths.has_value() && circuits.has_value() &&
             independent.has_value() && common.has_value() && points.has_value() && membership.has_value() &&
             group.has_value() && sink.has_value() && zone.has_value() && radius.has_value() &&
             components.has_value());

  check_posture(upstream.value());
  check_posture(downstream.value());
  check_posture(paths.value());
  check_posture(circuits.value());
  check_posture(independent.value());
  check_posture(common.value());
  check_posture(points.value());
  check_posture(membership.value());
  check_posture(group.value());
  check_posture(sink.value());
  check_posture(zone.value());
  check_posture(radius.value());
  check_posture(components.value());

  check_claim_class(upstream.value());
  check_claim_class(downstream.value());
  check_claim_class(paths.value());
  check_claim_class(circuits.value());
  check_claim_class(independent.value());
  check_claim_class(common.value());
  check_claim_class(points.value());
  check_claim_class(radius.value());

  // A cooled path never claims more than structural possibility.
  CT_CHECK(paths.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK(circuits.value().claim == ct::ClaimClass::StructurallyPossible);
  CT_CHECK(independent.value().claim == ct::ClaimClass::StructurallyPossible);
}

}  // namespace
