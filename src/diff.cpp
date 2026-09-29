// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/diff.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/text.hpp"
#include "graph_internal.hpp"

namespace dccp::cooling_topology {
namespace {

using Field = std::pair<std::string, std::string>;

std::string field_value(const std::optional<CoolingMedium>& medium) {
  return medium.has_value() ? std::string(to_token(*medium)) : std::string("-");
}

std::string field_value(const std::optional<MilliCelsius>& temperature) {
  return temperature.has_value() ? temperature->to_string() : std::string("-");
}

std::vector<Field> node_fields(const Node& node) {
  std::vector<Field> fields;
  fields.emplace_back("kind", std::string(to_token(node.kind())));
  fields.emplace_back("name", escape_text(node.display_name));
  fields.emplace_back("medium", field_value(declared_medium(node)));
  fields.emplace_back("design-supply", field_value(declared_supply_temperature(node)));
  fields.emplace_back("max-supply", field_value(declared_max_supply_temperature(node)));
  fields.emplace_back("references", std::to_string(node.references.size()));
  return fields;
}

std::vector<Field> edge_fields(const Edge& edge) {
  std::vector<Field> fields;
  fields.emplace_back("kind", std::string(to_token(edge.kind)));
  fields.emplace_back("from", edge.from.node.str() + "." + std::string(to_token(edge.from.port)));
  fields.emplace_back("to", edge.to.node.str() + "." + std::string(to_token(edge.to.port)));
  return fields;
}

std::vector<Field> group_fields(const RedundancyGroup& group) {
  std::vector<Field> fields;
  fields.emplace_back("scheme", std::string(to_token(group.scheme)));
  fields.emplace_back("scope", std::string(to_token(group.scope)));
  fields.emplace_back("name", escape_text(group.display_name));
  fields.emplace_back("members", std::to_string(group.members.size()));
  fields.emplace_back("distinct-failure-domains",
                      group.require_distinct_failure_domains ? "true" : "false");
  fields.emplace_back("independent-sources", group.require_independent_sources ? "true" : "false");
  return fields;
}

std::vector<Field> changeover_fields(const ChangeoverGroup& group) {
  std::vector<Field> fields;
  fields.emplace_back("name", escape_text(group.display_name));
  fields.emplace_back("members", std::to_string(group.members.size()));
  fields.emplace_back("max-concurrent", std::to_string(group.max_concurrent));
  return fields;
}

std::string describe_field_change(const std::vector<Field>& before, const std::vector<Field>& after) {
  std::string detail;
  for (std::size_t index = 0; index < before.size() && index < after.size(); ++index) {
    if (before[index].second == after[index].second) {
      continue;
    }
    if (!detail.empty()) {
      detail += ", ";
    }
    detail += before[index].first;
    detail += '=';
    detail += before[index].second;
    detail += "->";
    detail += after[index].second;
  }
  return detail;
}

template <class Record, class IdOf, class FieldsOf>
void diff_table(const std::vector<Record>& before, const std::vector<Record>& after, IdOf id_of,
                FieldsOf fields_of, DiffChangeKind added, DiffChangeKind removed, DiffChangeKind changed,
                std::vector<DiffEntry>& entries, long long* delta) {
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < before.size() || right < after.size()) {
    if (right >= after.size() || (left < before.size() && id_of(before[left]) < id_of(after[right]))) {
      entries.push_back(DiffEntry{removed, id_of(before[left]).str(), {}});
      if (delta != nullptr) {
        --*delta;
      }
      ++left;
      continue;
    }
    if (left >= before.size() || (id_of(after[right]) < id_of(before[left]))) {
      entries.push_back(DiffEntry{added, id_of(after[right]).str(), {}});
      if (delta != nullptr) {
        ++*delta;
      }
      ++right;
      continue;
    }
    const std::vector<Field> before_fields = fields_of(before[left]);
    const std::vector<Field> after_fields = fields_of(after[right]);
    const std::string detail = describe_field_change(before_fields, after_fields);
    if (!detail.empty()) {
      entries.push_back(DiffEntry{changed, id_of(before[left]).str(), detail});
    }
    ++left;
    ++right;
  }
}

}  // namespace

std::string_view to_token(DiffChangeKind kind) noexcept {
  switch (kind) {
    case DiffChangeKind::NodeAdded:
      return "node_added";
    case DiffChangeKind::NodeRemoved:
      return "node_removed";
    case DiffChangeKind::NodeChanged:
      return "node_changed";
    case DiffChangeKind::EdgeAdded:
      return "edge_added";
    case DiffChangeKind::EdgeRemoved:
      return "edge_removed";
    case DiffChangeKind::EdgeChanged:
      return "edge_changed";
    case DiffChangeKind::GroupAdded:
      return "group_added";
    case DiffChangeKind::GroupRemoved:
      return "group_removed";
    case DiffChangeKind::GroupChanged:
      return "group_changed";
    case DiffChangeKind::AliasAdded:
      return "alias_added";
    case DiffChangeKind::AliasRemoved:
      return "alias_removed";
    case DiffChangeKind::ChangeoverAdded:
      return "changeover_added";
    case DiffChangeKind::ChangeoverRemoved:
      return "changeover_removed";
    case DiffChangeKind::ChangeoverChanged:
      return "changeover_changed";
    default:
      return "unknown";
  }
}

Result<TopologyDiff> diff_topologies(const Topology& before, const Topology& after) {
  TopologyDiff diff;
  diff.before_generation = before.generation();
  diff.after_generation = after.generation();
  diff.before_digest = before.digest();
  diff.after_digest = after.digest();
  diff.same_facility = before.facility().same_binding_as(after.facility());

  diff_table(before.nodes(), after.nodes(), [](const Node& node) { return node.id; }, node_fields,
             DiffChangeKind::NodeAdded, DiffChangeKind::NodeRemoved, DiffChangeKind::NodeChanged, diff.entries,
             &diff.node_delta);
  diff_table(before.edges(), after.edges(), [](const Edge& edge) { return edge.id; }, edge_fields,
             DiffChangeKind::EdgeAdded, DiffChangeKind::EdgeRemoved, DiffChangeKind::EdgeChanged, diff.entries,
             &diff.edge_delta);
  diff_table(before.groups(), after.groups(), [](const RedundancyGroup& group) { return group.id; }, group_fields,
             DiffChangeKind::GroupAdded, DiffChangeKind::GroupRemoved, DiffChangeKind::GroupChanged,
             diff.entries, nullptr);
  diff_table(before.aliases(), after.aliases(), [](const Alias& alias) { return alias.id; },
             [](const Alias& alias) { return std::vector<Field>{Field{"target", alias.target.str()}}; },
             DiffChangeKind::AliasAdded, DiffChangeKind::AliasRemoved, DiffChangeKind::AliasAdded,
             diff.entries, nullptr);
  diff_table(before.changeovers(), after.changeovers(),
             [](const ChangeoverGroup& group) { return group.id; }, changeover_fields,
             DiffChangeKind::ChangeoverAdded, DiffChangeKind::ChangeoverRemoved,
             DiffChangeKind::ChangeoverChanged, diff.entries, nullptr);

  if (diff.entries.size() > limits::kMaxDiffEntries) {
    diff.entries.resize(limits::kMaxDiffEntries);
    diff.impact.truncated = true;
  }
  std::sort(diff.entries.begin(), diff.entries.end(), [](const DiffEntry& lhs, const DiffEntry& rhs) {
    if (lhs.kind != rhs.kind) {
      return lhs.kind < rhs.kind;
    }
    if (lhs.subject != rhs.subject) {
      return lhs.subject < rhs.subject;
    }
    return lhs.detail < rhs.detail;
  });

  // Structural impact is evaluated on the "after" generation only: it answers
  // which elements are left without a structural supply or service route, and it
  // never claims that anything was or was not operating before the change.
  internal::GraphIndex graph;
  graph.build(after.nodes(), after.edges(), after.aliases());
  for (std::uint32_t index = 0; index < after.nodes().size(); ++index) {
    const NodeKind kind = after.nodes()[index].kind();
    const std::vector<Edge>& edges = after.edges();
    if (kind == NodeKind::CoolingSink) {
      bool fed = false;
      for (const std::uint32_t edge : graph.in_edges(index)) {
        if (edges[edge].kind == EdgeKind::Supplies) {
          fed = true;
          break;
        }
      }
      if (!fed) {
        diff.impact.unfed_sinks.push_back(after.nodes()[index].id);
      }
    } else if (kind == NodeKind::ThermalZone) {
      bool served = false;
      for (const std::uint32_t edge : graph.in_edges(index)) {
        if (edges[edge].kind == EdgeKind::Serves) {
          served = true;
          break;
        }
      }
      if (!served) {
        diff.impact.unserved_zones.push_back(after.nodes()[index].id);
      }
    }
    if (diff.impact.unfed_sinks.size() + diff.impact.unserved_zones.size() > limits::kMaxQueryResultCount) {
      diff.impact.truncated = true;
      break;
    }
  }
  return diff;
}

std::vector<std::string> explain_diff(const TopologyDiff& diff) {
  std::vector<std::string> lines;
  lines.push_back("generation " + std::to_string(diff.before_generation.value()) + " -> " +
                  std::to_string(diff.after_generation.value()));
  lines.push_back("digest " + diff.before_digest.to_hex() + " -> " + diff.after_digest.to_hex());
  if (!diff.same_facility) {
    lines.push_back("facility binding changed; the two generations do not describe the same facility");
  }
  lines.push_back("nodes delta " + std::to_string(diff.node_delta) + ", edges delta " +
                  std::to_string(diff.edge_delta));
  for (const DiffEntry& entry : diff.entries) {
    std::string line = std::string(to_token(entry.kind));
    line += ' ';
    line += entry.subject;
    if (!entry.detail.empty()) {
      line += " (";
      line += entry.detail;
      line += ')';
    }
    lines.push_back(std::move(line));
  }
  for (const NodeId& sink : diff.impact.unfed_sinks) {
    lines.push_back("impact: cooling sink " + sink.str() + " has no structural supply route");
  }
  for (const NodeId& zone : diff.impact.unserved_zones) {
    lines.push_back("impact: thermal zone " + zone.str() + " has no serving element");
  }
  if (diff.impact.truncated) {
    lines.push_back("impact: analysis stopped at a configured bound; the report is a prefix");
  }
  lines.push_back(std::string("claim boundary: ") + std::string(posture_statement()));
  return lines;
}

}  // namespace dccp::cooling_topology
