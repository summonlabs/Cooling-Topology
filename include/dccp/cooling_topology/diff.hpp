// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_DIFF_HPP
#define DCCP_COOLING_TOPOLOGY_DIFF_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {

enum class DiffChangeKind : std::uint8_t {
  NodeAdded = 0,
  NodeRemoved = 1,
  NodeChanged = 2,
  EdgeAdded = 3,
  EdgeRemoved = 4,
  EdgeChanged = 5,
  GroupAdded = 6,
  GroupRemoved = 7,
  GroupChanged = 8,
  AliasAdded = 9,
  AliasRemoved = 10,
  ChangeoverAdded = 11,
  ChangeoverRemoved = 12,
  ChangeoverChanged = 13,
};

std::string_view to_token(DiffChangeKind kind) noexcept;

struct DiffEntry {
  DiffChangeKind kind = DiffChangeKind::NodeAdded;
  /// Identity of the changed object (node, edge, group, alias, changeover).
  std::string subject;
  /// Field-level description, e.g. "medium=chilled_water->condenser_water".
  std::string detail;
};

/// Structural consequence of a change, computed on the "after" generation.
struct DiffImpact {
  /// Sinks that are left without any structural supply route by this change.
  std::vector<NodeId> unfed_sinks;
  /// Thermal zones that are left without any serving element.
  std::vector<NodeId> unserved_zones;
  /// True when the impact analysis hit a configured bound.
  bool truncated = false;
};

struct TopologyDiff {
  TopologyGeneration before_generation{};
  TopologyGeneration after_generation{};
  Digest before_digest{};
  Digest after_digest{};
  std::vector<DiffEntry> entries;
  DiffImpact impact;
  /// True when the two generations describe the same facility binding.
  bool same_facility = true;
  /// Sum of added minus removed nodes, edges and group memberships.
  long long node_delta = 0;
  long long edge_delta = 0;
};

/// Deterministic diff of two generations. Entries are ordered by
/// (kind, subject); the same pair of generations always yields the same list.
Result<TopologyDiff> diff_topologies(const Topology& before, const Topology& after);

/// Human-readable explanation lines for a diff, including the structural impact.
/// Deterministic; safe to print.
std::vector<std::string> explain_diff(const TopologyDiff& diff);

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_DIFF_HPP
