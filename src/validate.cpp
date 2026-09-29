// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "validate_internal.hpp"

#include <algorithm>
#include <cstddef>
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

constexpr ValidationSeverity kError = ValidationSeverity::Error;
constexpr ValidationSeverity kWarning = ValidationSeverity::Warning;

constexpr EdgeKind kSuppliesKinds[] = {EdgeKind::Supplies};
constexpr EdgeKind kReturnsKinds[] = {EdgeKind::Returns};
constexpr EdgeKind kServesKinds[] = {EdgeKind::Serves};
constexpr EdgeKind kContainsKinds[] = {EdgeKind::Contains};
constexpr EdgeKind kDependsKinds[] = {EdgeKind::DependsOn};

bool port_in_range(PortRole role) noexcept {
  return static_cast<std::size_t>(role) <= static_cast<std::size_t>(PortRole::Served);
}

bool scheme_in_range(RedundancyScheme scheme) noexcept {
  return static_cast<std::size_t>(scheme) <= static_cast<std::size_t>(RedundancyScheme::ConcurrentlyMaintainable);
}

bool scope_in_range(RedundancyScope scope) noexcept {
  return static_cast<std::size_t>(scope) <= static_cast<std::size_t>(RedundancyScope::AirHandling);
}

bool origin_in_range(ProvenanceOrigin origin) noexcept {
  return static_cast<std::size_t>(origin) <= static_cast<std::size_t>(ProvenanceOrigin::Recovered);
}

bool evidence_kind_in_range(EvidenceKind kind) noexcept {
  return static_cast<std::size_t>(kind) <= static_cast<std::size_t>(EvidenceKind::PowerStateObservation);
}

bool ref_kind_in_range(ExternalRefKind kind) noexcept {
  return static_cast<std::size_t>(kind) <= static_cast<std::size_t>(ExternalRefKind::Evidence);
}

std::string bounded(std::string_view text) { return std::string(text.substr(0, 96)); }

std::string join_ids(const std::vector<std::string>& ids) {
  std::string out;
  for (const std::string& id : ids) {
    if (!out.empty()) {
      out += ", ";
    }
    out += id;
  }
  return out;
}

/// Whole validation pass. Issues are grouped by stage and each stage is sorted
/// before being appended, so the issue list and the primary error are a pure
/// function of the draft.
class Validator {
 public:
  explicit Validator(const TopologyDraft& draft) : draft_(draft) {}

  ValidationReport run() {
    shape();
    identity();
    endpoint();
    role();
    circuit();
    medium();
    containment();
    attachment();
    service();
    redundancy();
    changeover();
    dependency();
    binding();
    completeness();
    return std::move(report_);
  }

 private:
  void note(ValidationSeverity severity, ErrorCode code, std::string message, std::string subject) {
    pending_.push_back(ValidationIssue{severity, code, std::move(message), std::move(subject)});
  }

  void flush() {
    std::sort(pending_.begin(), pending_.end(), [](const ValidationIssue& lhs, const ValidationIssue& rhs) {
      if (lhs.subject != rhs.subject) {
        return lhs.subject < rhs.subject;
      }
      if (lhs.code != rhs.code) {
        return static_cast<std::uint16_t>(lhs.code) < static_cast<std::uint16_t>(rhs.code);
      }
      return lhs.message < rhs.message;
    });
    for (ValidationIssue& issue : pending_) {
      report_.issues.push_back(std::move(issue));
    }
    pending_.clear();
  }

  // -- stage 0: shape ------------------------------------------------------
  void shape() {
    struct Bound {
      std::size_t count;
      std::size_t limit;
      const char* what;
    };
    const Bound bounds[] = {
        {draft_.nodes.size(), limits::kMaxNodeCount, "node"},
        {draft_.edges.size(), limits::kMaxEdgeCount, "edge"},
        {draft_.groups.size(), limits::kMaxRedundancyGroupCount, "redundancy group"},
        {draft_.aliases.size(), limits::kMaxAliasCount, "alias"},
        {draft_.changeovers.size(), limits::kMaxChangeoverGroupCount, "changeover group"},
        {draft_.provenance.evidence.size(), limits::kMaxEvidenceBindings, "evidence binding"},
    };
    bool oversized = false;
    for (const Bound& bound : bounds) {
      if (bound.count > bound.limit) {
        oversized = true;
        note(kError, ErrorCode::LimitExceeded,
             std::string("draft declares more ") + bound.what + " records than the documented bound",
             std::string("table:") + bound.what);
      }
    }
    if (oversized) {
      flush();
      return;
    }

    for (const Node& node : draft_.nodes) {
      const std::string subject = node.id.str();
      if (node.id.empty() || !is_valid_identifier_syntax(node.id.value())) {
        note(kError, ErrorCode::MalformedIdentifier, "node identity does not match the identifier grammar",
             subject);
      }
      if (!node.display_name.empty() && !is_valid_display_text(node.display_name, limits::kMaxDisplayNameBytes)) {
        note(kError, ErrorCode::InvalidUtf8,
             "node display name must be valid UTF-8 display text without control characters", subject);
      }
      if (node.references.size() > limits::kMaxNodeReferences) {
        note(kError, ErrorCode::LimitExceeded, "node carries more external references than the bound", subject);
      }
      if (const auto* sink = node.as_cooling_sink()) {
        if (sink->consumer.identity.empty()) {
          note(kError, ErrorCode::MissingField, "cooling sink must reference the consumer it belongs to", subject);
        } else if (sink->consumer.kind != ExternalRefKind::Consumer) {
          note(kError, ErrorCode::InvalidArgument,
               "cooling sink consumer reference must be an ExternalRefKind::Consumer identity", subject);
        }
      }
      const std::optional<MilliCelsius> temperatures[] = {declared_supply_temperature(node),
                                                          declared_max_supply_temperature(node)};
      for (const std::optional<MilliCelsius>& value : temperatures) {
        if (value.has_value() && !temperature_in_range(value->milli_celsius())) {
          note(kError, ErrorCode::QuantityOutOfRange,
               "declared design temperature is outside the representable range", subject);
        }
      }
    }

    for (const Edge& edge : draft_.edges) {
      const std::string subject = edge.id.str();
      if (edge.id.empty() || !is_valid_identifier_syntax(edge.id.value())) {
        note(kError, ErrorCode::MalformedIdentifier, "edge identity does not match the identifier grammar",
             subject);
      }
      if (edge.from.node.empty() || edge.to.node.empty()) {
        note(kError, ErrorCode::EndpointMissing, "edge endpoint must name a node", subject);
      }
      if (!port_in_range(edge.from.port) || !port_in_range(edge.to.port)) {
        note(kError, ErrorCode::InvalidPortForKind, "edge endpoint names an unknown port role", subject);
      }
    }

    for (const RedundancyGroup& group : draft_.groups) {
      const std::string subject = group.id.str();
      if (group.id.empty() || !is_valid_identifier_syntax(group.id.value())) {
        note(kError, ErrorCode::MalformedIdentifier,
             "redundancy group identity does not match the identifier grammar", subject);
      }
      if (!group.display_name.empty() &&
          !is_valid_display_text(group.display_name, limits::kMaxDisplayNameBytes)) {
        note(kError, ErrorCode::InvalidUtf8, "redundancy group display name must be valid UTF-8 display text",
             subject);
      }
      if (!group.basis.empty() && !is_valid_display_text(group.basis, limits::kMaxBasisBytes)) {
        note(kError, ErrorCode::InvalidUtf8, "redundancy group basis must be valid UTF-8 display text", subject);
      }
      if (group.members.empty()) {
        note(kError, ErrorCode::GroupEmpty, "redundancy group must declare at least one member", subject);
      }
      if (group.members.size() > limits::kMaxGroupMemberCount) {
        note(kError, ErrorCode::LimitExceeded, "redundancy group declares more members than the bound", subject);
      }
      if (!scheme_in_range(group.scheme)) {
        note(kError, ErrorCode::UnknownEnumToken, "redundancy scheme is not a declared value", subject);
      }
      if (!scope_in_range(group.scope)) {
        note(kError, ErrorCode::UnknownEnumToken, "redundancy scope is not a declared value", subject);
      }
      for (const RedundancyMember& member : group.members) {
        if (member.node.empty() || member.declared.empty()) {
          note(kError, ErrorCode::GroupMemberMissing, "redundancy member must name a node and its spelling",
               subject);
        } else if (!is_valid_display_text(member.declared, limits::kMaxIdentifierBytes)) {
          note(kError, ErrorCode::MalformedIdentifier,
               "redundancy member declared spelling must be valid identifier text", subject);
        }
        if (member.failure_domain.has_value() &&
            (member.failure_domain->identity.empty() ||
             !is_valid_external_identity(member.failure_domain->identity, limits::kMaxExternalIdentityBytes))) {
          note(kError, ErrorCode::GroupMemberMissing,
               "redundancy member failure domain must be a valid non-empty external identity", subject);
        }
      }
    }

    for (const Alias& alias : draft_.aliases) {
      const std::string subject = alias.id.str();
      if (alias.id.empty() || !is_valid_identifier_syntax(alias.id.value())) {
        note(kError, ErrorCode::MalformedIdentifier, "alias identity does not match the identifier grammar",
             subject);
      }
      if (alias.target.empty() || !is_valid_identifier_syntax(alias.target.value())) {
        note(kError, ErrorCode::MalformedIdentifier, "alias target does not match the identifier grammar",
             subject);
      }
    }

    for (const ChangeoverGroup& group : draft_.changeovers) {
      const std::string subject = group.id.str();
      if (group.id.empty() || !is_valid_identifier_syntax(group.id.value())) {
        note(kError, ErrorCode::MalformedIdentifier,
             "changeover group identity does not match the identifier grammar", subject);
      }
      if (!group.display_name.empty() &&
          !is_valid_display_text(group.display_name, limits::kMaxDisplayNameBytes)) {
        note(kError, ErrorCode::InvalidUtf8, "changeover group display name must be valid UTF-8 display text",
             subject);
      }
      if (group.members.size() < 2) {
        note(kError, ErrorCode::ChangeoverCardinality,
             "a changeover arrangement needs at least two declared members", subject);
      } else if (group.members.size() > limits::kMaxChangeoverMemberCount) {
        note(kError, ErrorCode::LimitExceeded, "changeover group declares more members than the bound", subject);
      } else if (group.max_concurrent == 0 ||
                 static_cast<std::size_t>(group.max_concurrent) >= group.members.size()) {
        note(kError, ErrorCode::ChangeoverCardinality,
             "max_concurrent must be at least 1 and strictly below the member count", subject);
      }
      for (const Endpoint& member : group.members) {
        if (member.node.empty() || !port_in_range(member.port)) {
          note(kError, ErrorCode::ChangeoverMemberInvalid,
               "changeover member must name a node and a declared port role", subject);
        }
      }
    }

    const Provenance& provenance = draft_.provenance;
    if (provenance.producer.empty()) {
      note(kError, ErrorCode::MissingField, "provenance producer must not be empty", "provenance");
    } else if (!is_valid_display_text(provenance.producer, limits::kMaxProducerBytes)) {
      note(kError, ErrorCode::InvalidUtf8,
           "provenance producer must be valid UTF-8 display text without control characters", "provenance");
    }
    if (!provenance.witness.empty() && !is_valid_display_text(provenance.witness, limits::kMaxWitnessBytes)) {
      note(kError, ErrorCode::InvalidUtf8, "provenance witness must be valid UTF-8 display text", "provenance");
    }
    if (!origin_in_range(provenance.origin)) {
      note(kError, ErrorCode::UnknownEnumToken, "provenance origin is not a declared value", "provenance");
    }
    for (const EvidenceBinding& binding : provenance.evidence) {
      const std::string subject = binding.producer;
      if (!evidence_kind_in_range(binding.kind)) {
        note(kError, ErrorCode::UnknownEnumToken, "evidence kind is not a declared value", subject);
      }
      if (binding.producer.empty() || !is_valid_display_text(binding.producer, limits::kMaxProducerBytes)) {
        note(kError, ErrorCode::InvalidUtf8, "evidence producer must be non-empty UTF-8 display text", subject);
      }
      if (binding.subject.identity.empty() || !ref_kind_in_range(binding.subject.kind)) {
        note(kError, ErrorCode::MissingField, "evidence subject must be a non-empty external reference",
             subject);
      }
    }
    if (draft_.facility.identity.empty()) {
      note(kError, ErrorCode::MissingField, "draft must declare the facility it describes", "facility");
    }
    flush();
  }

  // -- stage 1: identity ---------------------------------------------------
  void identity() {
    detect_duplicates(draft_.nodes, [](const Node& node) { return node.id.value(); }, "node");
    detect_duplicates(draft_.edges, [](const Edge& edge) { return edge.id.value(); }, "edge");
    detect_duplicates(draft_.groups, [](const RedundancyGroup& group) { return group.id.value(); },
                      "redundancy group");
    detect_duplicates(draft_.aliases, [](const Alias& alias) { return alias.id.value(); }, "alias");
    detect_duplicates(draft_.changeovers, [](const ChangeoverGroup& group) { return group.id.value(); },
                      "changeover group");

    // An alias identity must not collide with a node identity: the two would
    // name different objects with the same spelling.
    std::vector<std::pair<std::string_view, std::string>> node_ids;
    node_ids.reserve(draft_.nodes.size());
    for (const Node& node : draft_.nodes) {
      node_ids.emplace_back(node.id.value(), node.id.str());
    }
    std::sort(node_ids.begin(), node_ids.end());
    for (const Alias& alias : draft_.aliases) {
      const auto it = std::lower_bound(node_ids.begin(), node_ids.end(), alias.id.value(),
                                       [](const auto& entry, std::string_view value) {
                                         return entry.first < value;
                                       });
      if (it != node_ids.end() && it->first == alias.id.value()) {
        note(kError, ErrorCode::IdentityConflict,
             "alias identity collides with a node identity", alias.id.str());
      }
    }

    // An alias must target a canonical node identity: no chains, no cycles.
    std::vector<std::pair<std::string_view, std::string>> alias_ids;
    alias_ids.reserve(draft_.aliases.size());
    for (const Alias& alias : draft_.aliases) {
      alias_ids.emplace_back(alias.id.value(), alias.id.str());
    }
    std::sort(alias_ids.begin(), alias_ids.end());
    for (const Alias& alias : draft_.aliases) {
      if (alias.target.empty()) {
        continue;
      }
      const auto target_node =
          std::lower_bound(node_ids.begin(), node_ids.end(), alias.target.value(),
                           [](const auto& entry, std::string_view value) { return entry.first < value; });
      if (target_node != node_ids.end() && target_node->first == alias.target.value()) {
        continue;
      }
      const auto target_alias =
          std::lower_bound(alias_ids.begin(), alias_ids.end(), alias.target.value(),
                           [](const auto& entry, std::string_view value) { return entry.first < value; });
      if (target_alias != alias_ids.end() && target_alias->first == alias.target.value()) {
        note(kError, ErrorCode::AliasCycle,
             "alias targets another alias; aliases must resolve to a canonical node identity in one step",
             alias.id.str());
      } else {
        note(kError, ErrorCode::AliasTargetMissing, "alias target does not name a node in this generation",
             alias.id.str());
      }
    }
    flush();
  }

  template <class Record, class Key>
  void detect_duplicates(const std::vector<Record>& records, Key key, const char* what) {
    std::vector<std::pair<std::string, std::size_t>> entries;
    entries.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
      entries.emplace_back(std::string(key(records[index])), index);
    }
    std::sort(entries.begin(), entries.end());
    for (std::size_t index = 1; index < entries.size(); ++index) {
      if (entries[index].first == entries[index - 1].first) {
        note(kError, ErrorCode::DuplicateIdentifier,
             std::string("two ") + what + " records declare the same identity", entries[index].first);
        while (index + 1 < entries.size() && entries[index + 1].first == entries[index].first) {
          ++index;
        }
      }
    }
  }

  // -- resolution ----------------------------------------------------------
  void build_index() {
    index_.build(draft_.nodes, draft_.aliases);
    graph_.build(draft_.nodes, draft_.edges, draft_.aliases);
    edge_from_.assign(draft_.edges.size(), kNoNode);
    edge_to_.assign(draft_.edges.size(), kNoNode);
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      edge_from_[i] = graph_.from_of(static_cast<std::uint32_t>(i));
      edge_to_[i] = graph_.to_of(static_cast<std::uint32_t>(i));
    }
  }

  // -- stage 2: endpoint ---------------------------------------------------
  void endpoint() {
    build_index();
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      const Edge& edge = draft_.edges[i];
      const std::string subject = edge.id.str();
      if (edge_from_[i] == kNoNode) {
        note(kError, ErrorCode::EndpointMissing, "edge source endpoint does not name a node in this generation",
             subject);
      }
      if (edge_to_[i] == kNoNode) {
        note(kError, ErrorCode::EndpointMissing, "edge target endpoint does not name a node in this generation",
             subject);
      }
      if (edge_from_[i] != kNoNode && edge_from_[i] == edge_to_[i]) {
        note(kError, ErrorCode::SelfEdge, "an edge must not connect a node to itself", subject);
      }
    }
    std::vector<std::pair<EdgeKey, std::size_t>> keys;
    keys.reserve(draft_.edges.size());
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      keys.emplace_back(EdgeKey::of(draft_.edges[i]), i);
    }
    std::sort(keys.begin(), keys.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first.kind != rhs.first.kind) {
        return lhs.first.kind < rhs.first.kind;
      }
      if (lhs.first.first != rhs.first.first) {
        return lhs.first.first < rhs.first.first;
      }
      return lhs.first.second < rhs.first.second;
    });
    for (std::size_t i = 1; i < keys.size(); ++i) {
      if (keys[i].first == keys[i - 1].first) {
        note(kError, ErrorCode::DuplicateEdge,
             "two edges declare the same kind and endpoint pair", draft_.edges[keys[i].second].id.str());
      }
    }
    flush();
  }

  // -- stage 3: role -------------------------------------------------------
  //
  // Endpoint compatibility is decided in two steps and the first failure wins:
  // the *kind* relation between the two endpoints, then the per-kind port
  // legality, then the port roles the edge kind requires. Deciding the kind
  // first makes the diagnosis specific ("a thermal zone can never deliver
  // cooled medium") instead of reporting a port mismatch that only exists
  // because the kind was wrong.
  void role() {
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      const Edge& edge = draft_.edges[i];
      if (edge_from_[i] == kNoNode || edge_to_[i] == kNoNode || edge_from_[i] == edge_to_[i]) {
        continue;
      }
      const NodeKind from_kind = draft_.nodes[edge_from_[i]].kind();
      const NodeKind to_kind = draft_.nodes[edge_to_[i]].kind();
      const std::string subject = edge.id.str();

      auto decision = classify_kinds(edge.kind, from_kind, to_kind);
      if (decision.first == ErrorCode::Ok && !port_allowed_for_kind(from_kind, edge.from.port)) {
        decision = {ErrorCode::InvalidPortForKind,
                    std::string("port ") + std::string(to_token(edge.from.port)) +
                        " is not a port of node kind " + std::string(to_token(from_kind))};
      }
      if (decision.first == ErrorCode::Ok && !port_allowed_for_kind(to_kind, edge.to.port)) {
        decision = {ErrorCode::InvalidPortForKind,
                    std::string("port ") + std::string(to_token(edge.to.port)) +
                        " is not a port of node kind " + std::string(to_token(to_kind))};
      }
      if (decision.first == ErrorCode::Ok) {
        decision = classify_ports(edge);
      }
      if (decision.first != ErrorCode::Ok) {
        note(kError, decision.first, std::move(decision.second), subject);
      }
    }
    flush();
  }

  /// The kind relation an edge of this kind requires, independent of ports.
  static std::pair<ErrorCode, std::string> classify_kinds(EdgeKind kind, NodeKind from_kind, NodeKind to_kind) {
    switch (kind) {
      case EdgeKind::Supplies:
        if (!is_supplier_kind(from_kind)) {
          return {ErrorCode::InvalidEdgeKindPair,
                  std::string("node kind ") + std::string(to_token(from_kind)) +
                      " can never deliver cooled medium"};
        }
        if (to_kind == NodeKind::CoolingSource) {
          return {ErrorCode::SourceTerminalInvalid,
                  "a cooling source delivers medium and never receives a supplies edge"};
        }
        if (!is_consumer_kind(to_kind)) {
          return {ErrorCode::InvalidEdgeKindPair,
                  std::string("node kind ") + std::string(to_token(to_kind)) +
                      " can never receive cooled medium"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Returns:
        if (!is_heat_rejector_kind(from_kind)) {
          return {ErrorCode::InvalidEdgeKindPair,
                  std::string("node kind ") + std::string(to_token(from_kind)) +
                      " can never reject warmed medium"};
        }
        if (!is_return_target_kind(to_kind)) {
          return {ErrorCode::ReturnTargetInvalid,
                  std::string("node kind ") + std::string(to_token(to_kind)) +
                      " can never receive a returns edge"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Serves:
        if (from_kind == NodeKind::CoolingPlant) {
          return {ErrorCode::PlantBoundaryInvalid,
                  "a cooling plant delivers medium through loops and never serves a zone or sink directly"};
        }
        if (!is_server_kind(from_kind)) {
          return {ErrorCode::ZoneServiceInvalid,
                  std::string("node kind ") + std::string(to_token(from_kind)) +
                      " can never provide thermal service"};
        }
        if (!is_served_kind(to_kind)) {
          return {ErrorCode::ZoneServiceInvalid,
                  std::string("node kind ") + std::string(to_token(to_kind)) +
                      " can never receive thermal service"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Pumps:
        if (!is_pump_host_kind(from_kind)) {
          return {ErrorCode::PumpAttachmentInvalid,
                  std::string("node kind ") + std::string(to_token(from_kind)) +
                      " can never host a pump installation"};
        }
        if (to_kind != NodeKind::Pump) {
          return {ErrorCode::PumpAttachmentInvalid,
                  std::string("node kind ") + std::string(to_token(to_kind)) +
                      " is not a pump and cannot be installed by a pumps edge"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Contains:
        if (!is_container_kind(from_kind)) {
          return {ErrorCode::ContainmentKindInvalid,
                  std::string("node kind ") + std::string(to_token(from_kind)) + " can never contain anything"};
        }
        if (!containment_pair_allowed(from_kind, to_kind)) {
          return {ErrorCode::ContainmentKindInvalid,
                  std::string("node kind ") + std::string(to_token(to_kind)) + " cannot be contained in a " +
                      std::string(to_token(from_kind))};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::DependsOn:
        return {ErrorCode::Ok, {}};
      default:
        return {ErrorCode::UnknownEnumToken, "edge kind is not a declared value"};
    }
  }

  /// The port roles an edge of this kind requires on each side.
  static std::pair<ErrorCode, std::string> classify_ports(const Edge& edge) {
    switch (edge.kind) {
      case EdgeKind::Supplies:
        if (edge.from.port != PortRole::SupplyOut) {
          return {ErrorCode::InvalidEdgeEndpointPair,
                  "a supplies edge must leave a supply_out port of its source"};
        }
        if (edge.to.port != PortRole::SourceIn) {
          return {ErrorCode::InvalidEdgeEndpointPair,
                  "a supplies edge must enter a source_in port of its target"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Returns:
        if (edge.from.port != PortRole::HeatOut) {
          return {ErrorCode::InvalidEdgeEndpointPair,
                  "a returns edge must leave a heat_out port of its source"};
        }
        if (edge.to.port != PortRole::ReturnIn) {
          return {ErrorCode::InvalidEdgeEndpointPair,
                  "a returns edge must enter a return_in port of its target"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Serves:
        if (edge.from.port != PortRole::Server) {
          return {ErrorCode::InvalidEdgeEndpointPair, "a serves edge must leave a server port of its source"};
        }
        if (edge.to.port != PortRole::Served) {
          return {ErrorCode::InvalidEdgeEndpointPair, "a serves edge must enter a served port of its target"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Pumps:
        if (edge.from.port != PortRole::SupplyOut && edge.from.port != PortRole::ReturnIn) {
          return {ErrorCode::PumpAttachmentInvalid,
                  "a pump installation must name the supply_out or return_in header of its host"};
        }
        if (edge.to.port != PortRole::Terminal) {
          return {ErrorCode::PumpAttachmentInvalid, "a pump installation must enter the terminal port of a pump"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::Contains:
        if (edge.from.port != PortRole::Container) {
          return {ErrorCode::InvalidEdgeEndpointPair, "a contains edge must leave a container port"};
        }
        if (edge.to.port != PortRole::Contained) {
          return {ErrorCode::InvalidEdgeEndpointPair, "a contains edge must enter a contained port"};
        }
        return {ErrorCode::Ok, {}};
      case EdgeKind::DependsOn:
        if (edge.from.port != PortRole::Terminal || edge.to.port != PortRole::Terminal) {
          return {ErrorCode::InvalidEdgeEndpointPair,
                  "a dependency edge connects the terminal port of both endpoints"};
        }
        return {ErrorCode::Ok, {}};
      default:
        return {ErrorCode::UnknownEnumToken, "edge kind is not a declared value"};
    }
  }

  // -- stage 4: circuit ----------------------------------------------------
  void circuit() {
    std::vector<std::string> cycle;
    if (internal::find_cycle(graph_, kSuppliesKinds, cycle)) {
      note(kError, ErrorCode::SupplyCycle,
           "supply edges close a directed cycle; cooled medium cannot be delivered around a loop: " +
               join_ids(cycle),
           cycle.empty() ? std::string() : cycle.front());
    }
    cycle.clear();
    if (internal::find_cycle(graph_, kReturnsKinds, cycle)) {
      note(kError, ErrorCode::ReturnCycle,
           "return edges close a directed cycle; warmed medium cannot return around a loop: " + join_ids(cycle),
           cycle.empty() ? std::string() : cycle.front());
    }
    flush();
  }

  // -- stage 5: medium -----------------------------------------------------
  void medium() {
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      const Edge& edge = draft_.edges[i];
      if (edge.kind != EdgeKind::Supplies && edge.kind != EdgeKind::Returns) {
        continue;
      }
      if (edge_from_[i] == kNoNode || edge_to_[i] == kNoNode) {
        continue;
      }
      const Node& source = draft_.nodes[edge_from_[i]];
      const Node& target = draft_.nodes[edge_to_[i]];
      const std::string subject = edge.id.str();
      const std::optional<CoolingMedium> source_medium = declared_medium(source);
      const std::optional<CoolingMedium> target_medium = declared_medium(target);
      if (source_medium.has_value() && target_medium.has_value() && *source_medium != *target_medium) {
        note(kError, ErrorCode::MediumMismatch,
             std::string("medium continuity: ") + std::string(to_token(*source_medium)) + " cannot feed " +
                 std::string(to_token(*target_medium)),
             subject);
        continue;
      }
      if (edge.kind == EdgeKind::Supplies) {
        const std::optional<MilliCelsius> supply = declared_supply_temperature(source);
        const std::optional<MilliCelsius> limit = declared_max_supply_temperature(target);
        if (temperature_relation_declared(supply, limit) && *supply > *limit) {
          note(kError, ErrorCode::TemperatureIncompatible,
               "declared design supply temperature " + supply->to_string() +
                   " is above the highest design supply temperature " + limit->to_string() +
                   " the target declares it accepts",
               subject);
          continue;
        }
      }
      if (edge.kind == EdgeKind::Supplies && source.kind() == NodeKind::CoolingLoop &&
          target.kind() == NodeKind::CoolingLoop) {
        const auto* source_loop = source.as_cooling_loop();
        const auto* target_loop = target.as_cooling_loop();
        if (source_loop != nullptr && target_loop != nullptr && source_loop->kind == LoopKind::Secondary &&
            target_loop->kind == LoopKind::Primary) {
          note(kError, ErrorCode::LoopKindMismatch,
               "a secondary loop cannot supply a primary loop; the primary side is the upstream side of the "
               "arrangement",
               subject);
        }
      }
    }
    flush();
  }

  // -- stage 6: containment ------------------------------------------------
  void containment() {
    std::vector<std::uint32_t> parents(draft_.nodes.size(), kNoNode);
    std::vector<std::string> parent_edge(draft_.nodes.size());
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      if (draft_.edges[i].kind != EdgeKind::Contains || edge_from_[i] == kNoNode || edge_to_[i] == kNoNode) {
        continue;
      }
      if (parents[edge_to_[i]] != kNoNode) {
        note(kError, ErrorCode::AmbiguousParentage,
             "an element declares more than one containing element; containment must be a forest",
             draft_.nodes[edge_to_[i]].id.str());
      } else {
        parents[edge_to_[i]] = edge_from_[i];
        parent_edge[edge_to_[i]] = draft_.edges[i].id.str();
      }
    }
    for (std::size_t i = 0; i < draft_.nodes.size(); ++i) {
      if (requires_container(draft_.nodes[i].kind()) && parents[i] == kNoNode) {
        note(kError, ErrorCode::MissingContainer,
             std::string("a ") + std::string(to_token(draft_.nodes[i].kind())) +
                 " must be contained in exactly one containing element",
             draft_.nodes[i].id.str());
      }
    }
    std::vector<std::string> cycle;
    if (internal::find_cycle(graph_, kContainsKinds, cycle)) {
      note(kError, ErrorCode::ContainmentCycle,
           "containment edges close a cycle: " + join_ids(cycle), cycle.empty() ? std::string() : cycle.front());
    }
    flush();
  }

  // -- stage 7: attachment -------------------------------------------------
  void attachment() {
    std::vector<std::uint32_t> host_count(draft_.nodes.size(), 0);
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      if (draft_.edges[i].kind != EdgeKind::Pumps || edge_to_[i] == kNoNode) {
        continue;
      }
      ++host_count[edge_to_[i]];
    }
    for (std::size_t i = 0; i < draft_.nodes.size(); ++i) {
      if (!requires_pump_host(draft_.nodes[i].kind())) {
        continue;
      }
      if (host_count[i] == 0) {
        note(kError, ErrorCode::PumpAttachmentInvalid,
             "a pump must be installed on exactly one distribution element", draft_.nodes[i].id.str());
      } else if (host_count[i] > 1) {
        note(kError, ErrorCode::PumpAttachmentInvalid,
             "a pump declares more than one installation site", draft_.nodes[i].id.str());
      }
    }
    flush();
  }

  // -- stage 8: service ----------------------------------------------------
  void service() {
    std::vector<std::string> cycle;
    if (internal::find_cycle(graph_, kServesKinds, cycle)) {
      note(kError, ErrorCode::ServiceCycle,
           "service edges close a cycle: " + join_ids(cycle), cycle.empty() ? std::string() : cycle.front());
    }
    flush();
  }

  // -- stage 9: redundancy -------------------------------------------------
  void redundancy() {
    if (draft_.groups.empty()) {
      flush();
      return;
    }
    const std::size_t budget = limits::kMaxTraversalNodes;
    for (const RedundancyGroup& group : draft_.groups) {
      const std::string subject = group.id.str();
      std::vector<std::uint32_t> resolved;
      for (const RedundancyMember& member : group.members) {
        const std::string spelling = member.declared.empty() ? member.node.str() : member.declared;
        const auto found = index_.find(member.node.value());
        if (!found.has_value()) {
          note(kError, ErrorCode::GroupMemberMissing, "redundancy member names an unknown node or alias",
               subject + "/" + bounded(spelling));
          continue;
        }
        const NodeKind kind = draft_.nodes[*found].kind();
        if (!is_redundancy_member_kind(kind)) {
          note(kError, ErrorCode::GroupMemberKindInvalid,
               std::string("node kind ") + std::string(to_token(kind)) +
                   " can never be a redundancy member",
               subject + "/" + bounded(spelling));
          continue;
        }
        if (!redundancy_scope_allows(group.scope, kind)) {
          note(kError, ErrorCode::GroupScopeInvalid,
               std::string("node kind ") + std::string(to_token(kind)) + " is not a member kind of scope " +
                   std::string(to_token(group.scope)),
               subject + "/" + bounded(spelling));
          continue;
        }
        if (std::find(resolved.begin(), resolved.end(), *found) != resolved.end()) {
          note(kError, ErrorCode::GroupMemberDuplicate,
               "redundancy group counts the same node twice (directly or through an alias)",
               subject + "/" + bounded(spelling));
          continue;
        }
        resolved.push_back(*found);
      }
      if (group.require_distinct_failure_domains) {
        std::vector<std::pair<std::string, std::string>> domains;
        for (const RedundancyMember& member : group.members) {
          if (!member.failure_domain.has_value()) {
            note(kError, ErrorCode::GroupRedundancyUnproven,
                 "group declares distinct failure domains but a member declares none",
                 subject + "/" + bounded(member.declared));
            continue;
          }
          domains.emplace_back(member.failure_domain->identity, member.declared);
        }
        std::sort(domains.begin(), domains.end());
        for (std::size_t i = 1; i < domains.size(); ++i) {
          if (domains[i].first == domains[i - 1].first) {
            note(kError, ErrorCode::GroupRedundancyUnproven,
                 "two members declare the same failure domain, so the declared independence does not hold",
                 subject + "/" + bounded(domains[i].second));
          }
        }
      }
      if (group.require_independent_sources && resolved.size() >= 2) {
        std::vector<std::vector<std::uint32_t>> source_sets;
        source_sets.reserve(resolved.size());
        bool truncated = false;
        for (const std::uint32_t node : resolved) {
          const internal::SourceSet sources = internal::member_source_set(graph_, node, budget);
          truncated = truncated || sources.truncated;
          source_sets.push_back(sources.sources);
        }
        if (truncated) {
          note(kError, ErrorCode::GroupIndependentPathUnproven,
               "independence cannot be established within the traversal bound", subject);
        } else {
          for (std::size_t i = 0; i < resolved.size(); ++i) {
            if (source_sets[i].empty()) {
              note(kError, ErrorCode::GroupIndependentPathUnproven,
                   "member has no structural source, so the declared independence does not hold",
                   subject + "/" + bounded(group.members[i].declared));
            }
            for (std::size_t j = i + 1; j < resolved.size(); ++j) {
              for (const std::uint32_t shared : source_sets[i]) {
                if (std::find(source_sets[j].begin(), source_sets[j].end(), shared) != source_sets[j].end()) {
                  note(kError, ErrorCode::GroupIndependentPathUnproven,
                       "members share structural source " + draft_.nodes[shared].id.str() +
                           ", so the declared independence does not hold",
                       subject + "/" + bounded(group.members[j].declared));
                  break;
                }
              }
            }
          }
        }
      }
    }
    flush();
  }

  // -- stage 10: changeover ------------------------------------------------
  void changeover() {
    if (draft_.changeovers.empty()) {
      flush();
      return;
    }
    std::vector<std::pair<std::uint32_t, std::uint8_t>> used;
    used.reserve(draft_.edges.size() * 2);
    for (std::size_t i = 0; i < draft_.edges.size(); ++i) {
      if (edge_from_[i] != kNoNode) {
        used.emplace_back(edge_from_[i], static_cast<std::uint8_t>(draft_.edges[i].from.port));
      }
      if (edge_to_[i] != kNoNode) {
        used.emplace_back(edge_to_[i], static_cast<std::uint8_t>(draft_.edges[i].to.port));
      }
    }
    std::sort(used.begin(), used.end());
    for (const ChangeoverGroup& group : draft_.changeovers) {
      const std::string subject = group.id.str();
      std::vector<std::pair<std::uint32_t, std::uint8_t>> seen;
      for (const Endpoint& member : group.members) {
        const auto found = index_.find(member.node.value());
        if (!found.has_value()) {
          note(kError, ErrorCode::ChangeoverMemberInvalid, "changeover member names an unknown node or alias",
               subject + "/" + bounded(member.node.str()));
          continue;
        }
        if (!port_allowed_for_kind(draft_.nodes[*found].kind(), member.port)) {
          note(kError, ErrorCode::ChangeoverMemberInvalid,
               std::string("port ") + std::string(to_token(member.port)) + " is not a port of node kind " +
                   std::string(to_token(draft_.nodes[*found].kind())),
               subject + "/" + bounded(member.node.str()));
          continue;
        }
        if (member.port != PortRole::SourceIn) {
          note(kError, ErrorCode::ChangeoverMemberInvalid,
               "a changeover arrangement selects between source_in connections", subject + "/" +
                   bounded(member.node.str()));
          continue;
        }
        const auto key = std::make_pair(*found, static_cast<std::uint8_t>(member.port));
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
          note(kError, ErrorCode::ChangeoverMemberDuplicate, "changeover group lists the same endpoint twice",
               subject + "/" + bounded(member.node.str()));
          continue;
        }
        seen.push_back(key);
        if (!std::binary_search(used.begin(), used.end(), key)) {
          note(kError, ErrorCode::ConstraintUnsatisfied,
               "changeover member is not an endpoint of any edge, so the declared arrangement is not attached "
               "to a connection",
               subject + "/" + bounded(member.node.str()));
        }
      }
    }
    flush();
  }

  // -- stage 11: dependency ------------------------------------------------
  void dependency() {
    std::vector<std::string> cycle;
    if (internal::find_cycle(graph_, kDependsKinds, cycle)) {
      note(kError, ErrorCode::DependencyCycle,
           "dependency edges close a cycle; a structural dependency order must be acyclic: " + join_ids(cycle),
           cycle.empty() ? std::string() : cycle.front());
    }
    cycle.clear();
    if (internal::find_cycle_union(graph_, kDependsKinds, kContainsKinds, cycle)) {
      note(kError, ErrorCode::DependencyCycle,
           "dependency and containment edges together close a cycle; a contained element cannot be a "
           "structural dependency of its container: " +
               join_ids(cycle),
           cycle.empty() ? std::string() : cycle.front());
    }
    flush();
  }

  // -- stage 12: binding ---------------------------------------------------
  void binding() {
    std::vector<std::pair<EvidenceBinding, std::string>> bindings;
    for (const EvidenceBinding& binding : draft_.provenance.evidence) {
      bindings.emplace_back(binding, binding.producer);
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first.producer != rhs.first.producer) {
        return lhs.first.producer < rhs.first.producer;
      }
      return static_cast<std::uint8_t>(lhs.first.kind) < static_cast<std::uint8_t>(rhs.first.kind);
    });
    for (std::size_t i = 1; i < bindings.size(); ++i) {
      if (bindings[i].first.same_binding_as(bindings[i - 1].first)) {
        note(kError, ErrorCode::DuplicateField, "the same evidence binding is recorded twice",
             bounded(bindings[i].first.producer));
      }
    }
    flush();
  }

  // -- stage 13: completeness (warnings only) ------------------------------
  void completeness() {
    for (std::uint32_t i = 0; i < draft_.nodes.size(); ++i) {
      if (graph_.in_edges(i).empty() && graph_.out_edges(i).empty()) {
        note(kWarning, ErrorCode::IsolatedElement,
             std::string("element of kind ") + std::string(to_token(draft_.nodes[i].kind())) +
                 " declares no connection of any kind and is structurally isolated",
             draft_.nodes[i].id.str());
      }
    }
    for (std::uint32_t i = 0; i < draft_.nodes.size(); ++i) {
      if (draft_.nodes[i].kind() != NodeKind::CoolingSink) {
        continue;
      }
      bool fed = false;
      for (const std::uint32_t edge_index : graph_.in_edges(i)) {
        if (draft_.edges[edge_index].kind == EdgeKind::Supplies) {
          fed = true;
          break;
        }
      }
      if (!fed) {
        note(kWarning, ErrorCode::SinkFeedAbsent,
             "cooling sink has no incoming supplies edge in this generation", draft_.nodes[i].id.str());
      }
    }
    for (std::uint32_t i = 0; i < draft_.nodes.size(); ++i) {
      if (draft_.nodes[i].kind() != NodeKind::ThermalZone) {
        continue;
      }
      bool served = false;
      for (const std::uint32_t edge_index : graph_.in_edges(i)) {
        if (draft_.edges[edge_index].kind == EdgeKind::Serves) {
          served = true;
          break;
        }
      }
      if (!served) {
        note(kWarning, ErrorCode::ZoneServiceAbsent,
             "thermal zone has no incoming serves edge in this generation", draft_.nodes[i].id.str());
      }
    }
    flush();
  }

  const TopologyDraft& draft_;
  NodeIndex index_;
  GraphIndex graph_;
  std::vector<std::uint32_t> edge_from_;
  std::vector<std::uint32_t> edge_to_;
  std::vector<ValidationIssue> pending_;
  ValidationReport report_;
};

}  // namespace

ValidationReport Topology::validate_draft(const TopologyDraft& draft) {
  Validator validator(draft);
  return validator.run();
}

bool ValidationReport::valid() const noexcept { return primary() == nullptr; }

std::size_t ValidationReport::error_count() const noexcept {
  std::size_t count = 0;
  for (const ValidationIssue& issue : issues) {
    if (issue.severity == ValidationSeverity::Error) {
      ++count;
    }
  }
  return count;
}

std::size_t ValidationReport::warning_count() const noexcept {
  std::size_t count = 0;
  for (const ValidationIssue& issue : issues) {
    if (issue.severity == ValidationSeverity::Warning) {
      ++count;
    }
  }
  return count;
}

const ValidationIssue* ValidationReport::primary() const noexcept {
  for (const ValidationIssue& issue : issues) {
    if (issue.severity == ValidationSeverity::Error) {
      return &issue;
    }
  }
  return nullptr;
}

std::string_view to_token(ValidationSeverity severity) noexcept {
  return severity == ValidationSeverity::Error ? std::string_view("error") : std::string_view("warning");
}

std::string_view to_token(ValidationStage stage) noexcept {
  switch (stage) {
    case ValidationStage::Shape:
      return "shape";
    case ValidationStage::Identity:
      return "identity";
    case ValidationStage::Endpoint:
      return "endpoint";
    case ValidationStage::Role:
      return "role";
    case ValidationStage::Circuit:
      return "circuit";
    case ValidationStage::Medium:
      return "medium";
    case ValidationStage::Containment:
      return "containment";
    case ValidationStage::Attachment:
      return "attachment";
    case ValidationStage::Service:
      return "service";
    case ValidationStage::Redundancy:
      return "redundancy";
    case ValidationStage::Changeover:
      return "changeover";
    case ValidationStage::Dependency:
      return "dependency";
    case ValidationStage::Binding:
      return "binding";
    case ValidationStage::Completeness:
      return "completeness";
    default:
      return "unknown";
  }
}

namespace internal {

Error primary_error(const ValidationReport& report) {
  const ValidationIssue* issue = report.primary();
  if (issue == nullptr) {
    return Error(ErrorCode::InternalError, "primary_error called on a valid report");
  }
  Error error(issue->code, issue->message);
  if (!issue->subject.empty()) {
    error.with_subject(issue->subject);
  }
  std::size_t reported = 0;
  for (const ValidationIssue& other : report.issues) {
    if (&other == issue || other.severity != ValidationSeverity::Error || reported >= 8) {
      continue;
    }
    error.with_detail(std::string(error_code_name(other.code)) + ": " + other.message +
                      (other.subject.empty() ? std::string() : " [" + other.subject + "]"));
    ++reported;
  }
  return error;
}

Result<CanonicalTables> validate_and_order(const TopologyDraft& draft) {
  const ValidationReport report = Topology::validate_draft(draft);
  if (!report.valid()) {
    return primary_error(report);
  }
  return canonical_order(draft);
}

}  // namespace internal
}  // namespace dccp::cooling_topology
