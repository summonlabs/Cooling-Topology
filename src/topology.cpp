// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/topology.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/text.hpp"
#include "graph_internal.hpp"
#include "validate_internal.hpp"

namespace dccp::cooling_topology {
namespace {

using internal::kNoNode;

/// Sorted index vector over a table keyed by identity, used for every lookup so
/// that a lookup never depends on hash order.
template <class Record, class Key>
std::vector<std::uint32_t> sorted_slots(const std::vector<Record>& records, Key key) {
  std::vector<std::uint32_t> order(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    order[index] = static_cast<std::uint32_t>(index);
  }
  std::sort(order.begin(), order.end(), [&records, &key](std::uint32_t lhs, std::uint32_t rhs) {
    return key(records[lhs]) < key(records[rhs]);
  });
  return order;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Result<Topology> Topology::create(TopologyGeneration generation, TopologyGeneration parent_generation,
                                  const Digest& parent_digest, const TopologyDraft& draft) {
  if (!generation.published()) {
    return Error(ErrorCode::InvalidArgument,
                 "a published generation starts at 1; generation 0 means no topology published yet");
  }
  if (parent_generation.published()) {
    if (parent_digest.is_zero()) {
      return Error(ErrorCode::MissingField,
                   "a non-first generation must bind the digest of the generation it succeeds");
    }
    if (parent_generation >= generation) {
      return Error(ErrorCode::GenerationMismatch,
                   "the parent generation must be strictly older than the generation it precedes")
          .with_subject(std::to_string(parent_generation.value()));
    }
  } else if (!parent_digest.is_zero()) {
    return Error(ErrorCode::GenerationMismatch,
                 "a first generation must not bind a parent digest");
  }

  CT_TRY(tables, internal::validate_and_order(draft));

  Topology topology;
  topology.header_.schema_version = kCanonicalSchemaVersion;
  topology.header_.generation = generation;
  topology.header_.parent_generation = parent_generation;
  topology.header_.parent_digest = parent_digest;
  topology.header_.facility = draft.facility;
  topology.header_.provenance = draft.provenance;
  // The order in which a producer lists evidence bindings carries no meaning, so
  // the canonical image must not depend on it.
  std::sort(topology.header_.provenance.evidence.begin(), topology.header_.provenance.evidence.end());
  topology.nodes_ = std::move(tables.nodes);
  topology.edges_ = std::move(tables.edges);
  topology.groups_ = std::move(tables.groups);
  topology.aliases_ = std::move(tables.aliases);
  topology.changeovers_ = std::move(tables.changeovers);

  CT_TRY(bytes, encode_topology(topology.header_, topology.nodes_, topology.edges_, topology.groups_,
                                topology.aliases_, topology.changeovers_));
  topology.digest_ = digest_bytes(bytes);
  topology.build_indices();
  return topology;
}

Result<Topology> Topology::create_first(const TopologyDraft& draft) {
  return create(TopologyGeneration(TopologyGeneration::kFirstPublished), TopologyGeneration(), Digest(), draft);
}

void Topology::build_indices() {
  node_order_ = sorted_slots(nodes_, [](const Node& node) { return node.id.value(); });
  alias_order_ = sorted_slots(aliases_, [](const Alias& alias) { return alias.id.value(); });
  group_order_ = sorted_slots(groups_, [](const RedundancyGroup& group) { return group.id.value(); });
  changeover_order_ = sorted_slots(changeovers_, [](const ChangeoverGroup& group) { return group.id.value(); });

  edge_lookup_.clear();
  edge_lookup_.reserve(edges_.size());
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    edge_lookup_.emplace_back(edges_[index].id, static_cast<std::uint32_t>(index));
  }
  std::sort(edge_lookup_.begin(), edge_lookup_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  const std::size_t count = nodes_.size();
  out_edge_offset_.assign(count + 1, 0);
  in_edge_offset_.assign(count + 1, 0);
  if (count == 0) {
    out_edges_.clear();
    in_edges_.clear();
    return;
  }
  internal::NodeIndex identities;
  identities.build(nodes_, aliases_);
  std::vector<std::uint32_t> from(edges_.size(), kNoNode);
  std::vector<std::uint32_t> to(edges_.size(), kNoNode);
  std::vector<std::uint32_t> out_counts(count + 1, 0);
  std::vector<std::uint32_t> in_counts(count + 1, 0);
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    if (const auto resolved = identities.find(edges_[index].from.node.value())) {
      from[index] = *resolved;
      ++out_counts[*resolved + 1];
    }
    if (const auto resolved = identities.find(edges_[index].to.node.value())) {
      to[index] = *resolved;
      ++in_counts[*resolved + 1];
    }
  }
  for (std::size_t index = 1; index <= count; ++index) {
    out_counts[index] += out_counts[index - 1];
    in_counts[index] += in_counts[index - 1];
  }
  out_edge_offset_.assign(out_counts.begin(), out_counts.end());
  in_edge_offset_.assign(in_counts.begin(), in_counts.end());
  out_edges_.assign(edges_.size(), EdgeId());
  in_edges_.assign(edges_.size(), EdgeId());
  std::vector<std::size_t> out_cursor(out_edge_offset_.begin(), out_edge_offset_.end() - 1);
  std::vector<std::size_t> in_cursor(in_edge_offset_.begin(), in_edge_offset_.end() - 1);
  // edges_ is in canonical order, so appending in table order leaves every
  // adjacency list sorted by edge identity without a second sort.
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    if (from[index] != kNoNode) {
      out_edges_[out_cursor[from[index]]++] = edges_[index].id;
    }
    if (to[index] != kNoNode) {
      in_edges_[in_cursor[to[index]]++] = edges_[index].id;
    }
  }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

TopologyDraft Topology::to_draft() const {
  TopologyDraft draft;
  draft.facility = header_.facility;
  draft.provenance = header_.provenance;
  draft.nodes = nodes_;
  draft.edges = edges_;
  draft.groups = groups_;
  draft.aliases = aliases_;
  draft.changeovers = changeovers_;
  return draft;
}

Result<NodeId> Topology::resolve(const NodeId& identity) const {
  if (identity.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "identity to resolve must not be empty");
  }
  if (const Node* node = find_node(identity)) {
    return node->id;
  }
  return Error(ErrorCode::NotFound, "identity names neither a node nor an alias in this generation")
      .with_subject(identity.str());
}

const Node* Topology::find_node(const NodeId& identity) const noexcept {
  const auto it = std::lower_bound(node_order_.begin(), node_order_.end(), identity.value(),
                                   [this](std::uint32_t slot, std::string_view value) {
                                     return nodes_[slot].id.value() < value;
                                   });
  if (it != node_order_.end() && nodes_[*it].id.value() == identity.value()) {
    return &nodes_[*it];
  }
  const auto alias = std::lower_bound(alias_order_.begin(), alias_order_.end(), identity.value(),
                                      [this](std::uint32_t slot, std::string_view value) {
                                        return aliases_[slot].id.value() < value;
                                      });
  if (alias == alias_order_.end() || aliases_[*alias].id.value() != identity.value()) {
    return nullptr;
  }
  const NodeId& target = aliases_[*alias].target;
  const auto target_it = std::lower_bound(node_order_.begin(), node_order_.end(), target.value(),
                                          [this](std::uint32_t slot, std::string_view value) {
                                            return nodes_[slot].id.value() < value;
                                          });
  if (target_it == node_order_.end() || nodes_[*target_it].id.value() != target.value()) {
    return nullptr;
  }
  return &nodes_[*target_it];
}

const Edge* Topology::find_edge(const EdgeId& id) const noexcept {
  const auto it = std::lower_bound(edge_lookup_.begin(), edge_lookup_.end(), id,
                                   [](const auto& slot, const EdgeId& value) { return slot.first < value; });
  if (it == edge_lookup_.end() || !(it->first == id)) {
    return nullptr;
  }
  return &edges_[it->second];
}

const RedundancyGroup* Topology::find_group(const RedundancyGroupId& id) const noexcept {
  const auto it = std::lower_bound(group_order_.begin(), group_order_.end(), id.value(),
                                   [this](std::uint32_t slot, std::string_view value) {
                                     return groups_[slot].id.value() < value;
                                   });
  if (it == group_order_.end() || groups_[*it].id.value() != id.value()) {
    return nullptr;
  }
  return &groups_[*it];
}

const Alias* Topology::find_alias(const AliasId& id) const noexcept {
  const auto it = std::lower_bound(alias_order_.begin(), alias_order_.end(), id.value(),
                                   [this](std::uint32_t slot, std::string_view value) {
                                     return aliases_[slot].id.value() < value;
                                   });
  if (it == alias_order_.end() || aliases_[*it].id.value() != id.value()) {
    return nullptr;
  }
  return &aliases_[*it];
}

const ChangeoverGroup* Topology::find_changeover(const ChangeoverGroupId& id) const noexcept {
  const auto it = std::lower_bound(changeover_order_.begin(), changeover_order_.end(), id.value(),
                                   [this](std::uint32_t slot, std::string_view value) {
                                     return changeovers_[slot].id.value() < value;
                                   });
  if (it == changeover_order_.end() || changeovers_[*it].id.value() != id.value()) {
    return nullptr;
  }
  return &changeovers_[*it];
}

std::span<const EdgeId> Topology::out_edges(const NodeId& node) const noexcept {
  const Node* found = find_node(node);
  if (found == nullptr) {
    return {};
  }
  const auto index = static_cast<std::size_t>(found - nodes_.data());
  const std::size_t begin = out_edge_offset_[index];
  const std::size_t end = out_edge_offset_[index + 1];
  return std::span<const EdgeId>(out_edges_.data() + begin, end - begin);
}

std::span<const EdgeId> Topology::in_edges(const NodeId& node) const noexcept {
  const Node* found = find_node(node);
  if (found == nullptr) {
    return {};
  }
  const auto index = static_cast<std::size_t>(found - nodes_.data());
  const std::size_t begin = in_edge_offset_[index];
  const std::size_t end = in_edge_offset_[index + 1];
  return std::span<const EdgeId>(in_edges_.data() + begin, end - begin);
}

std::vector<EdgeId> Topology::edges_of_kind(EdgeKind kind) const {
  std::vector<EdgeId> result;
  for (const Edge& edge : edges_) {
    if (edge.kind == kind) {
      result.push_back(edge.id);
    }
  }
  return result;
}

std::vector<NodeId> Topology::structural_sources() const {
  internal::GraphIndex graph;
  graph.build(nodes_, edges_, aliases_);
  std::vector<NodeId> result;
  for (const std::uint32_t index : internal::origin_nodes(graph)) {
    result.push_back(nodes_[index].id);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Canonical form
// ---------------------------------------------------------------------------

Result<std::string> Topology::canonical_bytes() const {
  return encode_topology(header_, nodes_, edges_, groups_, aliases_, changeovers_);
}

Result<Digest> Topology::recompute_digest() const {
  CT_TRY(bytes, canonical_bytes());
  return digest_bytes(bytes);
}

Result<Topology> Topology::decode(std::string_view bytes) {
  if (bytes.empty()) {
    return Error(ErrorCode::EmptyInput, "generation file is empty");
  }
  CT_TRY(frame, decode_generation_file(bytes));
  const Digest payload_digest = digest_bytes(frame.payload);
  if (!(payload_digest == frame.payload_digest)) {
    return Error(ErrorCode::DigestMismatch, "generation frame digest does not match its payload");
  }
  CT_TRY(decoded, decode_topology(frame.payload));

  TopologyDraft draft;
  draft.facility = decoded.header.facility;
  draft.provenance = decoded.header.provenance;
  draft.nodes = decoded.tables.nodes;
  draft.edges = decoded.tables.edges;
  draft.groups = decoded.tables.groups;
  draft.aliases = decoded.tables.aliases;
  draft.changeovers = decoded.tables.changeovers;

  CT_TRY(topology, create(decoded.header.generation, decoded.header.parent_generation,
                          decoded.header.parent_digest, draft));

  // Canonical fixed point: a decoded image re-encodes to exactly the bytes it was
  // decoded from. A difference means the input was not a canonical image of the
  // state it claimed to describe, and it is refused rather than normalized.
  CT_TRY(reencoded, topology.canonical_bytes());
  if (reencoded != frame.payload) {
    return Error(ErrorCode::IntegrityFailure,
                 "generation payload is not a canonical image: re-encoding produced different bytes");
  }
  return topology;
}

std::string Topology::render_text() const {
  std::string out;
  out.reserve(4096);
  out += "cooling-topology";
  out += " generation=";
  out += std::to_string(header_.generation.value());
  out += " parent=";
  out += std::to_string(header_.parent_generation.value());
  out += " schema=";
  out += std::to_string(header_.schema_version);
  out += " digest=";
  out += digest_.to_hex();
  out += '\n';
  out += "facility ";
  out += std::string(to_token(header_.facility.kind));
  out += ':';
  out += header_.facility.identity;
  out += '@';
  out += std::to_string(header_.facility.generation.value());
  out += '\n';
  out += "provenance producer=";
  out += escape_text(header_.provenance.producer);
  out += " origin=";
  out += std::string(to_token(header_.provenance.origin));
  out += " witness=";
  out += escape_text(header_.provenance.witness);
  out += " authority-epoch=";
  out += std::to_string(header_.provenance.authority_epoch.value());
  out += '\n';
  for (const EvidenceBinding& binding : header_.provenance.evidence) {
    out += "evidence ";
    out += std::string(to_token(binding.kind));
    out += " producer=";
    out += escape_text(binding.producer);
    out += " subject=";
    out += std::string(to_token(binding.subject.kind));
    out += ':';
    out += binding.subject.identity;
    out += " generation=";
    out += std::to_string(binding.generation.value());
    out += " observation=";
    out += std::to_string(binding.observation.value());
    out += '\n';
  }
  out += "nodes ";
  out += std::to_string(nodes_.size());
  out += '\n';
  for (const Node& node : nodes_) {
    out += "  ";
    out += describe_node(*this, node);
    out += '\n';
  }
  out += "edges ";
  out += std::to_string(edges_.size());
  out += '\n';
  for (const Edge& edge : edges_) {
    out += "  ";
    out += describe_edge(*this, edge);
    out += '\n';
  }
  out += "redundancy-groups ";
  out += std::to_string(groups_.size());
  out += '\n';
  for (const RedundancyGroup& group : groups_) {
    out += "  group ";
    out += group.id.str();
    out += " scope=";
    out += std::string(to_token(group.scope));
    out += " scheme=";
    out += std::string(to_token(group.scheme));
    out += " members=";
    out += std::to_string(group.members.size());
    out += " flags=";
    out += group.require_distinct_failure_domains ? "distinct-failure-domains" : "-";
    out += ',';
    out += group.require_independent_sources ? "independent-sources" : "-";
    out += '\n';
  }
  out += "aliases ";
  out += std::to_string(aliases_.size());
  out += '\n';
  for (const Alias& alias : aliases_) {
    out += "  alias ";
    out += alias.id.str();
    out += " -> ";
    out += alias.target.str();
    out += '\n';
  }
  out += "changeover-groups ";
  out += std::to_string(changeovers_.size());
  out += '\n';
  for (const ChangeoverGroup& group : changeovers_) {
    out += "  changeover ";
    out += group.id.str();
    out += " max=";
    out += std::to_string(group.max_concurrent);
    out += " members=";
    out += std::to_string(group.members.size());
    out += '\n';
  }
  out += "claim-boundary ";
  out += posture_statement();
  out += '\n';
  return out;
}

}  // namespace dccp::cooling_topology
