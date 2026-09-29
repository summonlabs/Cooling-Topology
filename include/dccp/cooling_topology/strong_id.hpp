// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_STRONG_ID_HPP
#define DCCP_COOLING_TOPOLOGY_STRONG_ID_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/result.hpp"

namespace dccp::cooling_topology {

/// Identifier syntax (canonical form):
///   - 1..128 bytes;
///   - first and last byte are ASCII alphanumeric;
///   - interior bytes are ASCII alphanumeric or one of '.', '_', ':', '-'.
///
/// The grammar deliberately contains no whitespace, quoting, path separators or
/// non-ASCII bytes, so canonical encoding is escape-free and an identifier can
/// never smuggle a path, a control character or a look-alike into durable
/// state. Identifiers are case-sensitive and never normalized.
bool is_valid_identifier_syntax(std::string_view raw) noexcept;

/// Explains why an identifier was rejected (empty view when it is valid).
std::string_view identifier_syntax_help() noexcept;

/// Tag types selecting a distinct StrongId instantiation. Unrelated identities
/// are distinct C++ types and cannot be converted into one another.
struct NodeIdTag {
  static constexpr std::string_view kind_name = "node";
};
struct EdgeIdTag {
  static constexpr std::string_view kind_name = "edge";
};
struct RedundancyGroupIdTag {
  static constexpr std::string_view kind_name = "redundancy-group";
};
struct AliasIdTag {
  static constexpr std::string_view kind_name = "alias";
};
struct ChangeoverGroupIdTag {
  static constexpr std::string_view kind_name = "changeover-group";
};
struct StoreIdTag {
  static constexpr std::string_view kind_name = "store";
};
struct MutationIdTag {
  static constexpr std::string_view kind_name = "mutation";
};

/// A validated, strongly typed identifier.
///
/// Construction only succeeds through parse(). The default-constructed value is
/// empty and exists only so identifiers can live in containers; an empty
/// identifier is never written to durable state and is rejected by every public
/// entry point that requires one.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  StrongId() noexcept = default;

  /// Parses and validates untrusted text.
  static Result<StrongId> parse(std::string_view raw) {
    if (!is_valid_identifier_syntax(raw)) {
      return Error(ErrorCode::MalformedIdentifier,
                   "identifier does not match the canonical grammar (1..128 bytes, ASCII "
                   "alphanumeric first/last byte, interior [A-Za-z0-9._:-])")
          .with_subject(std::string(raw.substr(0, 160)));
    }
    return StrongId(std::string(raw));
  }

  bool empty() const noexcept { return value_.empty(); }
  std::string_view value() const noexcept { return value_; }
  const std::string& str() const noexcept { return value_; }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept = default;

  /// Byte-wise ordering, identical on every platform.
  friend std::strong_ordering operator<=>(const StrongId& lhs, const StrongId& rhs) noexcept {
    const int cmp = lhs.value_.compare(rhs.value_);
    return cmp < 0 ? std::strong_ordering::less
                   : (cmp > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
  }

 private:
  explicit StrongId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

using NodeId = StrongId<NodeIdTag>;
using EdgeId = StrongId<EdgeIdTag>;
using RedundancyGroupId = StrongId<RedundancyGroupIdTag>;
using AliasId = StrongId<AliasIdTag>;
using ChangeoverGroupId = StrongId<ChangeoverGroupIdTag>;
using StoreId = StrongId<StoreIdTag>;

/// Caller-supplied idempotency key of one mutation. Deliberately distinct from
/// every other identity so a mutation key can never be passed where a node or
/// store identity is expected.
using MutationId = StrongId<MutationIdTag>;

// ---------------------------------------------------------------------------
// Counters that are semantically distinct and therefore distinct types
// ---------------------------------------------------------------------------

/// Monotonic generation counter of *published* cooling topology state.
///
/// Generation 0 means "no topology published yet"; published generations start
/// at 1. Distinct from every other counter: mixing two of them requires an
/// explicit conversion and no arithmetic is implicit.
class TopologyGeneration {
 public:
  static constexpr std::uint64_t kFirstPublished = 1;

  constexpr TopologyGeneration() noexcept = default;
  explicit constexpr TopologyGeneration(std::uint64_t value) noexcept : value_(value) {}

  static Result<TopologyGeneration> parse(std::uint64_t value) { return TopologyGeneration(value); }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool published() const noexcept { return value_ != 0; }

  /// Strictly increasing successor. Overflow is reported, never wrapped.
  Result<TopologyGeneration> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::LimitExceeded, "topology generation counter exhausted")
          .with_subject(std::to_string(value_));
    }
    return TopologyGeneration(value_ + 1);
  }

  friend constexpr bool operator==(const TopologyGeneration&, const TopologyGeneration&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const TopologyGeneration& lhs,
                                                    const TopologyGeneration& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Revision of an in-memory, un-published topology draft.
///
/// A draft revision is *not* a generation: drafts have no durable identity, no
/// digest of record and no authority. Revision 0 is the empty draft; every
/// accepted draft mutation advances the revision by one.
class DraftRevision {
 public:
  constexpr DraftRevision() noexcept = default;
  explicit constexpr DraftRevision(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }

  Result<DraftRevision> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::LimitExceeded, "draft revision counter exhausted");
    }
    return DraftRevision(value_ + 1);
  }

  friend constexpr bool operator==(const DraftRevision&, const DraftRevision&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const DraftRevision& lhs,
                                                    const DraftRevision& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Durable mutation-authority epoch of a store.
///
/// Opening a store for mutation reserves the next epoch durably before any
/// mutation is accepted. A mutation planned under an older epoch is fenced and
/// rejected instead of being applied to state the caller no longer owns.
class WriterEpoch {
 public:
  constexpr WriterEpoch() noexcept = default;
  explicit constexpr WriterEpoch(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool valid() const noexcept { return value_ != 0; }

  static Result<WriterEpoch> parse(std::uint64_t value) { return WriterEpoch(value); }

  Result<WriterEpoch> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::LimitExceeded, "writer epoch counter exhausted");
    }
    return WriterEpoch(value_ + 1);
  }

  friend constexpr bool operator==(const WriterEpoch&, const WriterEpoch&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const WriterEpoch& lhs,
                                                    const WriterEpoch& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Incarnation of a writer within its epoch.
///
/// A writer incarnation is the identity of one concrete mutation-authority
/// holder (one successful open-for-write). It is durably assigned, strictly
/// increasing within a store, and never derived from a process id, a path, a
/// clock or a random number, so it is reproducible in tests and meaningless to
/// an attacker. It appears in the head manifest only; it is never part of the
/// canonical bytes of a generation.
class WriterIncarnation {
 public:
  constexpr WriterIncarnation() noexcept = default;
  explicit constexpr WriterIncarnation(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool valid() const noexcept { return value_ != 0; }

  Result<WriterIncarnation> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::LimitExceeded, "writer incarnation counter exhausted");
    }
    return WriterIncarnation(value_ + 1);
  }

  friend constexpr bool operator==(const WriterIncarnation&, const WriterIncarnation&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const WriterIncarnation& lhs,
                                                    const WriterIncarnation& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Ordinal of one attempt at a mutation, starting at 1.
///
/// (MutationId, AttemptOrdinal) identifies one *attempt*: a first attempt and a
/// retry of it share the ordinal and are therefore replay, whereas an
/// intentional second mutation of the same shape must use a new MutationId.
class AttemptOrdinal {
 public:
  constexpr AttemptOrdinal() noexcept = default;
  explicit constexpr AttemptOrdinal(std::uint32_t value) noexcept : value_(value) {}

  static Result<AttemptOrdinal> parse(std::uint32_t value) {
    if (value == 0) {
      return Error(ErrorCode::InvalidArgument, "attempt ordinal is 1-based and must not be zero");
    }
    return AttemptOrdinal(value);
  }

  constexpr std::uint32_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const AttemptOrdinal&, const AttemptOrdinal&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const AttemptOrdinal& lhs,
                                                    const AttemptOrdinal& rhs) noexcept = default;

 private:
  std::uint32_t value_ = 0;
};

/// Monotonic commit sequence assigned by a store to each committed publication.
///
/// The commit sequence is a persistence counter: it is assigned at the commit
/// point of a publication and is never derived from, equal to, or
/// interchangeable with a topology generation.
class CommitSequence {
 public:
  constexpr CommitSequence() noexcept = default;
  explicit constexpr CommitSequence(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool committed() const noexcept { return value_ != 0; }

  Result<CommitSequence> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::LimitExceeded, "commit sequence counter exhausted");
    }
    return CommitSequence(value_ + 1);
  }

  friend constexpr bool operator==(const CommitSequence&, const CommitSequence&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const CommitSequence& lhs,
                                                    const CommitSequence& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Generation counter of an *external* registry binding (facility, rack, asset,
/// failure-domain, consumer or location registry). This library records the
/// binding so a reference can be checked for staleness by its owner; it never
/// advances the counter itself and never treats a binding as evidence of
/// authority.
class ExternalGeneration {
 public:
  constexpr ExternalGeneration() noexcept = default;
  explicit constexpr ExternalGeneration(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool bound() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(const ExternalGeneration&, const ExternalGeneration&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const ExternalGeneration& lhs,
                                                    const ExternalGeneration& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Authority epoch of an adjacent supervision plane that this library may be
/// told about (for example the epoch of the feed-authority service that
/// authorized an import). It is recorded as provenance only: it is never
/// interpreted as permission by this library.
class AuthorityEpoch {
 public:
  constexpr AuthorityEpoch() noexcept = default;
  explicit constexpr AuthorityEpoch(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool bound() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(const AuthorityEpoch&, const AuthorityEpoch&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const AuthorityEpoch& lhs,
                                                    const AuthorityEpoch& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Generation of an *external observation* that a producer supplies as
/// evidence. Distinct from ExternalGeneration (a registry binding) and from
/// AuthorityEpoch (a supervision epoch): evidence generation identifies one
/// version of an observation stream owned by another component.
class EvidenceGeneration {
 public:
  constexpr EvidenceGeneration() noexcept = default;
  explicit constexpr EvidenceGeneration(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool bound() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(const EvidenceGeneration&, const EvidenceGeneration&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const EvidenceGeneration& lhs,
                                                    const EvidenceGeneration& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Sequence number of one observation or effect owned by another component.
///
/// Recorded so that evidence can be ordered and checked for staleness by its
/// owner. This library never consumes an ObservationSequence as a permission,
/// a measurement or an operating state.
class ObservationSequence {
 public:
  constexpr ObservationSequence() noexcept = default;
  explicit constexpr ObservationSequence(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool present() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(const ObservationSequence&, const ObservationSequence&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const ObservationSequence& lhs,
                                                    const ObservationSequence& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

}  // namespace dccp::cooling_topology

namespace std {
template <class Tag>
struct hash<dccp::cooling_topology::StrongId<Tag>> {
  std::size_t operator()(const dccp::cooling_topology::StrongId<Tag>& id) const noexcept {
    return std::hash<std::string_view>{}(id.value());
  }
};
template <>
struct hash<dccp::cooling_topology::TopologyGeneration> {
  std::size_t operator()(const dccp::cooling_topology::TopologyGeneration& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
template <>
struct hash<dccp::cooling_topology::WriterEpoch> {
  std::size_t operator()(const dccp::cooling_topology::WriterEpoch& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
template <>
struct hash<dccp::cooling_topology::WriterIncarnation> {
  std::size_t operator()(const dccp::cooling_topology::WriterIncarnation& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
}  // namespace std

#endif  // DCCP_COOLING_TOPOLOGY_STRONG_ID_HPP
