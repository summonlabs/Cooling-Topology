// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_TOPOLOGY_HPP
#define DCCP_COOLING_TOPOLOGY_TOPOLOGY_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/evidence.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/strong_id.hpp"

namespace dccp::cooling_topology {

/// Version of the canonical generation encoding. Bumping it invalidates every
/// previously written generation file; a store refuses generations it cannot
/// decode rather than guessing.
inline constexpr std::uint16_t kCanonicalSchemaVersion = 1;

/// Where a generation's content came from. Provenance is descriptive: it is
/// recorded, never interpreted as permission.
enum class ProvenanceOrigin : std::uint8_t {
  Authored = 0,    ///< composed directly by an operator or a tool
  Imported = 1,    ///< imported from an external inventory or drawing
  Reconciled = 2,  ///< reconciled against another registry
  Recovered = 3,   ///< reconstructed by store recovery from a retained generation
};

std::string_view to_token(ProvenanceOrigin value) noexcept;
Result<ProvenanceOrigin> parse_provenance_origin(std::string_view token);

/// Source provenance of one topology generation.
struct Provenance {
  /// Component that produced the generation, e.g. "dccp-cooling-topology/1.0.0".
  std::string producer;
  ProvenanceOrigin origin = ProvenanceOrigin::Authored;
  /// Free-form witness of the source (drawing revision, import job, ticket).
  /// Descriptive text only; it confers nothing.
  std::string witness;
  /// Optional binding to an external registry generation the content was taken
  /// from. Recorded verbatim; freshness is owned by the referenced registry.
  std::optional<ExternalRef> source_reference;
  /// Optional epoch of a supervision plane that authorized the import. Stored as
  /// provenance; this library never turns it into permission.
  AuthorityEpoch authority_epoch{};
  /// Generation-stamped references to evidence owned by other components. Each
  /// binding is recorded verbatim and never interpreted, ordered as authority or
  /// turned into a claim. Recovered evidence is not fresh evidence: a binding
  /// names the generation it was taken from and its producer must revalidate it.
  std::vector<EvidenceBinding> evidence;

  static Result<Provenance> create(std::string producer, ProvenanceOrigin origin, std::string witness,
                                   std::optional<ExternalRef> source_reference, AuthorityEpoch authority_epoch,
                                   std::vector<EvidenceBinding> evidence);
};

/// Identity and binding fields of one published generation.
///
/// The digest of the generation covers the header (except the digest field
/// itself) and every table, so any change to identity, binding, provenance or
/// content changes the digest.
struct TopologyHeader {
  std::uint16_t schema_version = kCanonicalSchemaVersion;
  TopologyGeneration generation{};
  TopologyGeneration parent_generation{};
  /// Digest of the parent generation, or the zero digest for the first
  /// generation. Verified when the parent is retained; a mismatch breaks the
  /// chain and is reported rather than silently accepted.
  Digest parent_digest{};
  /// Facility this generation describes. Owning registry: external.
  ExternalRef facility;
  Provenance provenance;

  friend bool operator==(const TopologyHeader&, const TopologyHeader&) noexcept = default;
};

/// An un-published topology body plus its non-generation header fields.
///
/// A draft has no durable identity, no digest of record and no authority. It is
/// validated and canonicalized exactly like a published generation so that the
/// only difference between a draft and a generation is the generation number and
/// parent binding assigned by the store at publication time.
struct TopologyDraft {
  ExternalRef facility;
  Provenance provenance;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<RedundancyGroup> groups;
  std::vector<Alias> aliases;
  std::vector<ChangeoverGroup> changeovers;
};

/// Severity of one validation issue.
///
/// Error issues make a draft structurally invalid and are the only issues that
/// can be the primary outcome of validation. Warning issues report structural
/// *incompleteness* that is not impossible (an element with no connection at
/// all, a sink with no structural feed, a zone nothing serves); they never gate
/// publication, and they are always reported after every error issue.
enum class ValidationSeverity : std::uint8_t {
  Error = 0,
  Warning = 1,
};

std::string_view to_token(ValidationSeverity severity) noexcept;

/// One validation finding, in deterministic precedence order.
struct ValidationIssue {
  ValidationSeverity severity = ValidationSeverity::Error;
  ErrorCode code = ErrorCode::Ok;
  /// Human explanation.
  std::string message;
  /// Identity of the offending object, when there is one.
  std::string subject;
};

/// Validation stages, in the order they are applied. The first failing stage
/// wins, so a request with several defects always reports the same primary
/// error.
enum class ValidationStage : std::uint8_t {
  Shape = 0,         ///< bounds, enum payload validity, text shape
  Identity = 1,      ///< uniqueness, alias resolution and conflict
  Endpoint = 2,      ///< endpoint existence, port legality, self links, duplicates
  Role = 3,          ///< per-kind endpoint roles and forbidden kind combinations
  Circuit = 4,       ///< supply and return acyclicity (independent DAGs)
  Medium = 5,        ///< medium continuity and declared design-temperature compatibility
  Containment = 6,   ///< containment pairs, single container, mandatory container
  Attachment = 7,    ///< pump installation cardinality and legality
  Service = 8,       ///< Serves endpoint legality and acyclicity
  Redundancy = 9,    ///< group membership, scope, alias double counting, independence
  Changeover = 10,   ///< changeover group shape and membership
  Dependency = 11,   ///< DependsOn acyclicity alone and together with containment
  Binding = 12,      ///< facility reference, provenance and evidence bounds
  Completeness = 13, ///< warning-only structural completeness findings
};

std::string_view to_token(ValidationStage stage) noexcept;

/// Result of a full validation pass.
///
/// Issues are ordered by the documented validation stage order and, within a
/// stage, by subject identity. The same draft always produces the same issue
/// list in the same order, and the same primary error.
struct ValidationReport {
  std::vector<ValidationIssue> issues;

  /// True when no Error-severity issue was found. Warnings do not make a draft
  /// invalid.
  bool valid() const noexcept;
  std::size_t error_count() const noexcept;
  std::size_t warning_count() const noexcept;
  /// First Error-severity issue, or nullptr when the draft is valid.
  const ValidationIssue* primary() const noexcept;
};

/// An immutable, validated topology generation.
///
/// The value owns its tables in canonical order (sorted by identity) and a
/// read-only adjacency index. Copies are cheap enough to be used as snapshots
/// and every query is a pure function of the value: no global state, no clocks,
/// no randomness, no iteration over unordered containers in observable order.
class Topology {
 public:
  Topology() = default;

  /// Builds a generation. Validates the whole draft; on failure returns the
  /// primary error as a Result error. Never throws.
  static Result<Topology> create(TopologyGeneration generation, TopologyGeneration parent_generation,
                                 const Digest& parent_digest, const TopologyDraft& draft);

  /// Builds the first generation (generation 1, no parent).
  static Result<Topology> create_first(const TopologyDraft& draft);

  /// Validates a draft without building it, collecting every issue.
  static ValidationReport validate_draft(const TopologyDraft& draft);

  // -- header and identity ------------------------------------------------
  const TopologyHeader& header() const noexcept { return header_; }
  TopologyGeneration generation() const noexcept { return header_.generation; }
  TopologyGeneration parent_generation() const noexcept { return header_.parent_generation; }
  const Digest& parent_digest() const noexcept { return header_.parent_digest; }
  const ExternalRef& facility() const noexcept { return header_.facility; }
  const Provenance& provenance() const noexcept { return header_.provenance; }
  /// Canonical digest of this generation.
  const Digest& digest() const noexcept { return digest_; }

  /// The draft view of this generation (facility, provenance and tables).
  TopologyDraft to_draft() const;

  // -- tables (canonical order) -------------------------------------------
  const std::vector<Node>& nodes() const noexcept { return nodes_; }
  const std::vector<Edge>& edges() const noexcept { return edges_; }
  const std::vector<RedundancyGroup>& groups() const noexcept { return groups_; }
  const std::vector<Alias>& aliases() const noexcept { return aliases_; }
  const std::vector<ChangeoverGroup>& changeovers() const noexcept { return changeovers_; }

  std::size_t node_count() const noexcept { return nodes_.size(); }
  std::size_t edge_count() const noexcept { return edges_.size(); }
  std::size_t group_count() const noexcept { return groups_.size(); }

  // -- lookup --------------------------------------------------------------
  /// Canonical node for an identity that may be an alias. Returns NotFound when
  /// neither a node nor an alias carries the identity.
  Result<NodeId> resolve(const NodeId& identity) const;
  const Node* find_node(const NodeId& identity) const noexcept;
  const Edge* find_edge(const EdgeId& id) const noexcept;
  const RedundancyGroup* find_group(const RedundancyGroupId& id) const noexcept;
  const Alias* find_alias(const AliasId& id) const noexcept;
  const ChangeoverGroup* find_changeover(const ChangeoverGroupId& id) const noexcept;

  /// Outgoing edges of a node in canonical order. The view aliases immutable
  /// storage owned by this value and must not outlive it.
  std::span<const EdgeId> out_edges(const NodeId& node) const noexcept;
  /// Incoming edges of a node in canonical order.
  std::span<const EdgeId> in_edges(const NodeId& node) const noexcept;
  /// Edges of one kind in canonical order.
  std::vector<EdgeId> edges_of_kind(EdgeKind kind) const;

  /// Nodes that can deliver cooled medium and have no incoming Supplies edge:
  /// the structural origins of the supply graph. Canonical order.
  std::vector<NodeId> structural_sources() const;

  // -- canonical form ------------------------------------------------------
  /// Canonical little-endian encoding of the whole generation.
  Result<std::string> canonical_bytes() const;
  /// Recomputes the digest from canonical bytes; used by verification.
  Result<Digest> recompute_digest() const;

  /// Decodes a canonical generation. Rejects malformed, truncated, oversized
  /// and digest-mismatched input; a decoded topology is re-validated so a file
  /// that was edited to be structurally impossible is refused.
  static Result<Topology> decode(std::string_view bytes);

  /// Deterministic human-readable rendering for inspection. Not authoritative:
  /// it is derived from the validated value and is never accepted as input.
  std::string render_text() const;

 private:
  /// Builds the adjacency index and the sorted identity slot tables. Called once
  /// by every constructor; never called after the value is observable.
  void build_indices();

  TopologyHeader header_;
  Digest digest_{};
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<RedundancyGroup> groups_;
  std::vector<Alias> aliases_;
  std::vector<ChangeoverGroup> changeovers_;

  std::vector<std::uint32_t> node_order_;   // node indices sorted by canonical identity
  std::vector<std::uint32_t> alias_order_;  // alias indices sorted by alias identity
  std::vector<std::uint32_t> group_order_;  // group indices sorted by canonical identity
  std::vector<std::uint32_t> changeover_order_;
  std::vector<std::pair<EdgeId, std::uint32_t>> edge_lookup_;
  std::vector<std::size_t> out_edge_offset_;
  std::vector<std::size_t> in_edge_offset_;
  std::vector<EdgeId> out_edges_;
  std::vector<EdgeId> in_edges_;
};

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_TOPOLOGY_HPP
