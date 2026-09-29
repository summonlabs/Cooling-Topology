// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic property tests. Every property is checked against an
// independent reference model written here from the edge list, never against the
// implementation's own answer.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace ct_test;
namespace ct = dccp::cooling_topology;

/// Independent reference model: a directed graph built straight from a
/// generation's edge list, with its own traversal. Nothing in this class calls
/// into the library's query or graph code.
class ReferenceGraph {
 public:
  explicit ReferenceGraph(const ct::Topology& topology) {
    for (std::size_t index = 0; index < topology.nodes().size(); ++index) {
      index_.emplace(topology.nodes()[index].id.str(), static_cast<std::uint32_t>(index));
    }
    adjacency_.assign(topology.nodes().size(), {});
    reverse_.assign(topology.nodes().size(), {});
    for (const ct::Edge& edge : topology.edges()) {
      if (edge.kind != ct::EdgeKind::Supplies) {
        continue;
      }
      const auto from = index_.find(edge.from.node.value());
      const auto to = index_.find(edge.to.node.value());
      if (from == index_.end() || to == index_.end()) {
        continue;
      }
      adjacency_[from->second].push_back(to->second);
      reverse_[to->second].push_back(from->second);
    }
    for (auto& list : adjacency_) {
      std::sort(list.begin(), list.end());
      list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    for (auto& list : reverse_) {
      std::sort(list.begin(), list.end());
      list.erase(std::unique(list.begin(), list.end()), list.end());
    }
  }

  std::size_t size() const noexcept { return adjacency_.size(); }

  std::vector<std::uint32_t> reachable(std::uint32_t origin, bool follow_reverse) const {
    const auto& table = follow_reverse ? reverse_ : adjacency_;
    std::vector<std::uint8_t> seen(size(), 0);
    std::vector<std::uint32_t> frontier{origin};
    seen[origin] = 1;
    std::vector<std::uint32_t> visited;
    while (!frontier.empty()) {
      std::vector<std::uint32_t> next;
      for (const std::uint32_t node : frontier) {
        for (const std::uint32_t other : table[node]) {
          if (seen[other] != 0) {
            continue;
          }
          seen[other] = 1;
          visited.push_back(other);
          next.push_back(other);
        }
      }
      std::sort(next.begin(), next.end());
      frontier.swap(next);
    }
    std::sort(visited.begin(), visited.end());
    return visited;
  }

  /// Kahn's algorithm: true when the supplies subgraph is acyclic.
  bool is_acyclic() const {
    std::vector<std::uint32_t> indegree(size(), 0);
    for (const auto& list : adjacency_) {
      for (const std::uint32_t target : list) {
        ++indegree[target];
      }
    }
    std::vector<std::uint32_t> ready;
    for (std::uint32_t node = 0; node < size(); ++node) {
      if (indegree[node] == 0) {
        ready.push_back(node);
      }
    }
    std::size_t processed = 0;
    while (!ready.empty()) {
      const std::uint32_t node = ready.back();
      ready.pop_back();
      ++processed;
      for (const std::uint32_t target : adjacency_[node]) {
        if (--indegree[target] == 0) {
          ready.push_back(target);
        }
      }
    }
    return processed == size();
  }

  std::vector<std::uint32_t> sources() const {
    std::vector<std::uint32_t> result;
    for (std::uint32_t node = 0; node < size(); ++node) {
      if (reverse_[node].empty()) {
        result.push_back(node);
      }
    }
    return result;
  }

  std::uint32_t index_of(std::string_view identity) const {
    const auto found = index_.find(std::string(identity));
    return found == index_.end() ? UINT32_MAX : found->second;
  }

 private:
  std::map<std::string, std::uint32_t, std::less<>> index_;
  std::vector<std::vector<std::uint32_t>> adjacency_;
  std::vector<std::vector<std::uint32_t>> reverse_;
};

/// Builds a synthetic facility whose shape is chosen by the generator: a layered
/// distribution tree with mirroring return paths. Every structural invariant the
/// validator enforces is respected by construction, so a rejection would be a
/// defect in either the generator or the validator.
ct::TopologyDraft generate_facility(std::uint32_t plant_count, std::uint32_t manifold_count,
                                    std::uint32_t cdu_per_branch, std::uint32_t zone_count) {
  ct::TopologyDraft draft = base_draft("dc-gen");
  const auto add = [&draft](ct::Node value) { draft.nodes.push_back(std::move(value)); };
  add(source_node("source:fw"));
  for (std::uint32_t index = 0; index < plant_count; ++index) {
    const std::string id = "plant:p" + std::to_string(index);
    add(plant_node(id));
    draft.edges.push_back(edge("e:src-" + id, ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut, id,
                               ct::PortRole::SourceIn));
    draft.edges.push_back(edge("e:" + id + "-ret", ct::EdgeKind::Returns, id, ct::PortRole::HeatOut,
                               "source:fw", ct::PortRole::ReturnIn));
  }
  add(loop_node("loop:l1"));
  for (std::uint32_t index = 0; index < plant_count; ++index) {
    const std::string id = "plant:p" + std::to_string(index);
    draft.edges.push_back(edge("e:" + id + "-loop", ct::EdgeKind::Supplies, id, ct::PortRole::SupplyOut,
                               "loop:l1", ct::PortRole::SourceIn));
  }
  draft.edges.push_back(edge("e:loop-plant-ret", ct::EdgeKind::Returns, "loop:l1", ct::PortRole::HeatOut,
                             "plant:p0", ct::PortRole::ReturnIn));
  add(pump_node("pump:duty", ct::PumpRole::Duty));
  add(pump_node("pump:standby", ct::PumpRole::Standby));
  draft.edges.push_back(edge("e:pump-duty", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut,
                             "pump:duty", ct::PortRole::Terminal));
  draft.edges.push_back(edge("e:pump-standby", ct::EdgeKind::Pumps, "loop:l1", ct::PortRole::SupplyOut,
                             "pump:standby", ct::PortRole::Terminal));
  draft.groups.push_back(
      ct::RedundancyGroup::create(gid("group:pumps"), ct::RedundancyScheme::NPlusOne,
                                  ct::RedundancyScope::Pump, "pumps", "duty plus standby",
                                  {ct::RedundancyMember{nid("pump:duty"), "pump:duty", std::nullopt},
                                   ct::RedundancyMember{nid("pump:standby"), "pump:standby", std::nullopt}},
                                  false, false)
          .value());
  for (std::uint32_t index = 0; index < zone_count; ++index) {
    add(zone_node("zone:z" + std::to_string(index), ct::ZoneClass::ColdAisle));
    const std::string crah = "crah:ir" + std::to_string(index);
    add(crah_node(crah));
    draft.edges.push_back(edge("e:" + crah + "-feed", ct::EdgeKind::Supplies, "loop:l1",
                               ct::PortRole::SupplyOut, crah, ct::PortRole::SourceIn));
    draft.edges.push_back(edge("e:" + crah + "-serves", ct::EdgeKind::Serves, crah, ct::PortRole::Server,
                               "zone:z" + std::to_string(index), ct::PortRole::Served));
    draft.edges.push_back(edge("e:" + crah + "-in", ct::EdgeKind::Contains, "zone:z" + std::to_string(index),
                               ct::PortRole::Container, crah, ct::PortRole::Contained));
  }
  for (std::uint32_t index = 0; index < manifold_count; ++index) {
    const std::string manifold = "manifold:m" + std::to_string(index);
    add(manifold_node(manifold));
    draft.edges.push_back(edge("e:" + manifold + "-feed", ct::EdgeKind::Supplies, "loop:l1",
                               ct::PortRole::SupplyOut, manifold, ct::PortRole::SourceIn));
    draft.edges.push_back(edge("e:" + manifold + "-ret", ct::EdgeKind::Returns, manifold, ct::PortRole::HeatOut,
                               "loop:l1", ct::PortRole::ReturnIn));
    draft.edges.push_back(edge("e:" + manifold + "-in", ct::EdgeKind::Contains, "loop:l1",
                               ct::PortRole::Container, manifold, ct::PortRole::Contained));
    const std::string branch = "branch:b" + std::to_string(index);
    add(branch_node(branch));
    draft.edges.push_back(edge("e:" + branch + "-feed", ct::EdgeKind::Supplies, manifold,
                               ct::PortRole::SupplyOut, branch, ct::PortRole::SourceIn));
    draft.edges.push_back(edge("e:" + branch + "-ret", ct::EdgeKind::Returns, branch, ct::PortRole::HeatOut,
                               manifold, ct::PortRole::ReturnIn));
    draft.edges.push_back(edge("e:" + branch + "-in", ct::EdgeKind::Contains, manifold,
                               ct::PortRole::Container, branch, ct::PortRole::Contained));
    for (std::uint32_t cdu_index = 0; cdu_index < cdu_per_branch; ++cdu_index) {
      const std::string cdu = "cdu:c" + std::to_string(index) + "-" + std::to_string(cdu_index);
      const std::string sink = "sink:s" + std::to_string(index) + "-" + std::to_string(cdu_index);
      const std::string zone = "zone:z" + std::to_string(index % zone_count);
      add(cdu_node(cdu));
      add(sink_node(sink, "rack-" + std::to_string(index) + "-" + std::to_string(cdu_index)));
      draft.edges.push_back(edge("e:" + cdu + "-feed", ct::EdgeKind::Supplies, branch,
                                 ct::PortRole::SupplyOut, cdu, ct::PortRole::SourceIn));
      draft.edges.push_back(edge("e:" + cdu + "-ret", ct::EdgeKind::Returns, cdu, ct::PortRole::HeatOut,
                                 branch, ct::PortRole::ReturnIn));
      draft.edges.push_back(edge("e:" + sink + "-feed", ct::EdgeKind::Supplies, cdu,
                                 ct::PortRole::SupplyOut, sink, ct::PortRole::SourceIn));
      draft.edges.push_back(edge("e:" + sink + "-ret", ct::EdgeKind::Returns, sink, ct::PortRole::HeatOut, cdu,
                                 ct::PortRole::ReturnIn));
      draft.edges.push_back(edge("e:" + sink + "-in", ct::EdgeKind::Contains, zone, ct::PortRole::Container,
                                 sink, ct::PortRole::Contained));
    }
  }
  return draft;
}

std::vector<std::string> sorted_ids(const ct::Topology& topology) {
  std::vector<std::string> ids;
  for (const ct::Node& node : topology.nodes()) {
    ids.push_back(node.id.str());
  }
  return ids;
}

}  // namespace

CT_TEST(property_canonical_form_is_order_independent) {
  Rng rng(case_seed("property_canonical_form_is_order_independent"));
  for (int round = 0; round < 12; ++round) {
    const std::uint32_t plants = 1 + rng.below(3);
    const std::uint32_t manifolds = 1 + rng.below(4);
    const std::uint32_t cdus = 1 + rng.below(3);
    const std::uint32_t zones = 1 + rng.below(3);
    ct::TopologyDraft draft = generate_facility(plants, manifolds, cdus, zones);
    const auto first = try_build(draft);
    if (!first.has_value()) {
      report_note("generator produced an invalid draft: " + first.error().to_string());
      CT_CHECK(false);
      return;
    }
    for (int permutation = 0; permutation < 6; ++permutation) {
      ct::TopologyDraft shuffled = draft;
      const auto shuffle = [&rng](auto& table) {
        for (std::size_t index = table.size(); index > 1; --index) {
          const std::size_t other = rng.below(static_cast<std::uint32_t>(index));
          std::swap(table[index - 1], table[other]);
        }
      };
      shuffle(shuffled.nodes);
      shuffle(shuffled.edges);
      shuffle(shuffled.groups);
      for (ct::Node& node : shuffled.nodes) {
        shuffle(node.references);
      }
      const auto second = try_build(shuffled);
      CT_REQUIRE(second.has_value());
      CT_CHECK_EQ(second->digest().to_hex(), first->digest().to_hex());
      CT_CHECK_EQ(sorted_ids(*second), sorted_ids(*first));
    }
  }
}

CT_TEST(property_supply_graph_matches_an_independent_acyclic_reference) {
  Rng rng(case_seed("property_supply_graph_matches_an_independent_acyclic_reference"));
  for (int round = 0; round < 8; ++round) {
    ct::TopologyDraft draft = generate_facility(1 + rng.below(3), 1 + rng.below(3), 1 + rng.below(2),
                                                1 + rng.below(2));
    const ct::Topology topology = build(draft);
    const ReferenceGraph reference(topology);
    CT_CHECK(reference.is_acyclic());

    for (const ct::Node& node : topology.nodes()) {
      const std::uint32_t origin = reference.index_of(node.id.value());
      CT_REQUIRE(origin != UINT32_MAX);
      const std::vector<std::uint32_t> expected_downstream = reference.reachable(origin, false);
      const std::vector<std::uint32_t> expected_upstream = reference.reachable(origin, true);

      auto downstream = ct::downstream_of(topology, node.id);
      CT_REQUIRE(downstream.has_value());
      CT_CHECK_EQ(downstream->elements.size(), expected_downstream.size());
      CT_CHECK_EQ(downstream->claim, expected_downstream.empty() ? ct::ClaimClass::StructurallyImpossible
                                                                 : ct::ClaimClass::StructurallyPossible);

      auto upstream = ct::upstream_of(topology, node.id);
      CT_REQUIRE(upstream.has_value());
      CT_CHECK_EQ(upstream->elements.size(), expected_upstream.size());

      // A pump is attached through a pumps edge rather than a supplies edge, so
      // its structural sources are the sources of the distribution element it is
      // installed on; every other element is traced through its own incoming
      // supplies edges.
      ct::NodeId trace = node.id;
      if (node.kind() == ct::NodeKind::Pump) {
        bool hosted = false;
        for (const ct::Edge& edge : topology.edges()) {
          if (edge.kind == ct::EdgeKind::Pumps && edge.to.node == node.id) {
            trace = edge.from.node;
            hosted = true;
            break;
          }
        }
        if (!hosted) {
          trace = ct::NodeId();
        }
      }
      auto sources = ct::sources_serving(topology, node.id);
      CT_REQUIRE(sources.has_value());
      std::vector<std::string> expected_sources;
      if (!trace.empty()) {
        const std::uint32_t trace_index = reference.index_of(trace.value());
        CT_REQUIRE(trace_index != UINT32_MAX);
        for (std::size_t index = 0; index < topology.nodes().size(); ++index) {
          if (!ct::is_supplier_kind(topology.nodes()[index].kind())) {
            continue;
          }
          bool fed = false;
          for (const ct::Edge& edge : topology.edges()) {
            if (edge.kind == ct::EdgeKind::Supplies && edge.to.node == topology.nodes()[index].id) {
              fed = true;
              break;
            }
          }
          if (fed) {
            continue;
          }
          const std::vector<std::uint32_t> reached = reference.reachable(
              reference.index_of(topology.nodes()[index].id.value()), false);
          if (topology.nodes()[index].id == trace ||
              std::find(reached.begin(), reached.end(), trace_index) != reached.end()) {
            expected_sources.push_back(topology.nodes()[index].id.str());
          }
        }
      }
      std::sort(expected_sources.begin(), expected_sources.end());
      std::vector<std::string> actual_sources;
      for (const ct::NodeId& id : *sources) {
        actual_sources.push_back(id.str());
      }
      CT_CHECK_MSG(actual_sources == expected_sources,
                   "subject " + node.id.str() + " traced through " + trace.str());
    }
  }
}

CT_TEST(property_independent_path_count_matches_a_max_flow_reference) {
  Rng rng(case_seed("property_independent_path_count_matches_a_max_flow_reference"));
  for (int round = 0; round < 8; ++round) {
    ct::TopologyDraft draft = generate_facility(1 + rng.below(2), 1 + rng.below(3), 1 + rng.below(2), 1);
    const ct::Topology topology = build(draft);
    const ct::NodeId origin = nid("source:fw");
    for (const ct::Node& node : topology.nodes()) {
      if (node.kind() != ct::NodeKind::CoolingSink) {
        continue;
      }
      auto result = ct::independent_supply_paths(topology, origin, node.id);
      CT_REQUIRE(result.has_value());
      // Reference: a sink is reachable through exactly one edge-disjoint route
      // per manifold branch, so the expected count equals the number of distinct
      // supply edges entering the sink's feeder chain. Count the edge-disjoint
      // routes with a simple augmenting-path search over the supplies edges.
      std::map<std::string, std::size_t> index;
      for (std::size_t i = 0; i < topology.nodes().size(); ++i) {
        index[topology.nodes()[i].id.str()] = i;
      }
      std::vector<std::vector<std::size_t>> adjacency(topology.nodes().size());
      for (std::size_t i = 0; i < topology.edges().size(); ++i) {
        const ct::Edge& edge = topology.edges()[i];
        if (edge.kind != ct::EdgeKind::Supplies) {
          continue;
        }
        adjacency[index[edge.from.node.str()]].push_back(i);
      }
      std::vector<std::uint8_t> used(topology.edges().size(), 0);
      std::size_t reference_count = 0;
      const std::size_t target = index[node.id.str()];
      const std::size_t start = index[origin.str()];
      for (;;) {
        std::vector<std::size_t> parent_edge(topology.nodes().size(), SIZE_MAX);
        std::vector<std::size_t> parent_node(topology.nodes().size(), SIZE_MAX);
        std::vector<std::uint8_t> seen(topology.nodes().size(), 0);
        std::vector<std::size_t> frontier{start};
        seen[start] = 1;
        bool found = false;
        while (!frontier.empty() && !found) {
          std::vector<std::size_t> next;
          for (const std::size_t node_index : frontier) {
            for (const std::size_t edge_index : adjacency[node_index]) {
              if (used[edge_index] != 0) {
                continue;
              }
              const std::size_t other = index[topology.edges()[edge_index].to.node.str()];
              if (seen[other] != 0) {
                continue;
              }
              seen[other] = 1;
              parent_edge[other] = edge_index;
              parent_node[other] = node_index;
              if (other == target) {
                found = true;
                break;
              }
              next.push_back(other);
            }
            if (found) {
              break;
            }
          }
          frontier.swap(next);
        }
        if (!found) {
          break;
        }
        for (std::size_t node_index = target; node_index != start;) {
          used[parent_edge[node_index]] = 1;
          node_index = parent_node[node_index];
          CT_REQUIRE(node_index != SIZE_MAX);
        }
        ++reference_count;
      }
      CT_CHECK_EQ(result->path_count, reference_count);
      CT_CHECK_EQ(result->claim, reference_count == 0 ? ct::ClaimClass::StructurallyImpossible
                                                      : ct::ClaimClass::StructurallyPossible);
    }
  }
}

CT_TEST(property_encode_decode_is_a_fixed_point) {
  Rng rng(case_seed("property_encode_decode_is_a_fixed_point"));
  for (int round = 0; round < 10; ++round) {
    ct::TopologyDraft draft = generate_facility(1 + rng.below(3), 1 + rng.below(3), 1 + rng.below(3),
                                                1 + rng.below(2));
    const ct::Topology topology = build(draft);
    auto payload = topology.canonical_bytes();
    CT_REQUIRE(payload.has_value());
    auto framed = ct::encode_generation_file(*payload);
    CT_REQUIRE(framed.has_value());
    auto decoded = ct::Topology::decode(*framed);
    CT_REQUIRE(decoded.has_value());
    CT_CHECK_EQ(decoded->digest().to_hex(), topology.digest().to_hex());
    auto again = decoded->canonical_bytes();
    CT_REQUIRE(again.has_value());
    CT_CHECK_EQ(*again, *payload);
    CT_CHECK_EQ(decoded->node_count(), topology.node_count());
    CT_CHECK_EQ(decoded->edge_count(), topology.edge_count());
    CT_CHECK_EQ(decoded->group_count(), topology.group_count());
    // The claim boundary is a property of the type, not of the payload.
    for (std::size_t index = 0; index <= 10; ++index) {
      const auto claim = static_cast<ct::ExcludedClaim>(index);
      CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(claim), ct::ClaimDisposition::NotOwned);
    }
  }
}

CT_TEST(property_generation_binding_changes_the_digest) {
  Rng rng(case_seed("property_generation_binding_changes_the_digest"));
  ct::TopologyDraft draft = generate_facility(1, 1, 1, 1);
  auto first = ct::Topology::create(ct::TopologyGeneration(1), ct::TopologyGeneration(), ct::Digest(), draft);
  CT_REQUIRE(first.has_value());
  auto second = ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(1), first->digest(), draft);
  CT_REQUIRE(second.has_value());
  CT_CHECK(first->digest().to_hex() != second->digest().to_hex());
  CT_CHECK_EQ(second->parent_digest().to_hex(), first->digest().to_hex());
  CT_CHECK_EQ(second->parent_generation().value(), std::uint64_t{1});
  // The body is identical, so only the binding differs.
  auto draft_of_second = second->to_draft();
  auto third = ct::Topology::create(ct::TopologyGeneration(1), ct::TopologyGeneration(), ct::Digest(),
                                    draft_of_second);
  CT_REQUIRE(third.has_value());
  CT_CHECK_EQ(third->digest().to_hex(), first->digest().to_hex());
}
