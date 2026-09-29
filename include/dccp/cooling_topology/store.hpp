// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_STORE_HPP
#define DCCP_COOLING_TOPOLOGY_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/mutation.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {

/// How a store is opened.
enum class StoreMode : std::uint8_t {
  ReadOnly = 0,   ///< no writer lock, no epoch reservation, no mutation
  ReadWrite = 1,  ///< exclusive writer lock plus a durably reserved epoch
};

std::string_view to_token(StoreMode mode) noexcept;

/// Freshness of the state a store handle is serving. "Recovered" is never
/// reported as "fresh": recovered state must be revalidated before it is used as
/// authority again.
enum class StoreOpenState : std::uint8_t {
  Fresh = 0,     ///< store created by this call; no prior generation existed
  Reopened = 1,  ///< the head was read from a valid committed publication
  Recovered = 2, ///< the head was adopted from the retained previous publication
};

std::string_view to_token(StoreOpenState state) noexcept;

/// Store identity, binding, head and authority state.
struct StoreInfo {
  StoreId store_id;
  ExternalRef facility;
  TopologyGeneration head{};
  Digest head_digest{};
  TopologyGeneration floor{};
  CommitSequence commit_sequence{};
  WriterEpoch epoch{};
  WriterIncarnation incarnation{};
  StoreMode mode = StoreMode::ReadOnly;
  StoreOpenState open_state = StoreOpenState::Reopened;
  std::size_t retained_generations = 0;
  std::size_t idempotency_records = 0;
  bool writable = false;
  /// True when the handle may publish: writable, open, and the head verified.
  bool publication_allowed = false;
  std::string root;
  std::string boundary;
};

struct HistoryEntry {
  TopologyGeneration generation{};
  Digest digest{};
  TopologyGeneration parent_generation{};
  Digest parent_digest{};
  CommitSequence commit_sequence{};
  std::uint64_t file_bytes = 0;
  bool is_head = false;
  /// Chain link to the next newer retained generation verified.
  bool chain_verified = false;
};

/// A state-dependent mutation request.
struct PublicationRequest {
  MutationAuthority authority;
  /// Idempotency identity of the mutation; a retry reuses it.
  MutationId mutation;
  /// 1-based attempt ordinal of this mutation.
  AttemptOrdinal attempt;
  /// The complete topology body to publish.
  TopologyDraft draft;
};

enum class PublicationDurability : std::uint8_t {
  Durable = 0,     ///< content and head were flushed to the storage device
  NotDurable = 1,  ///< durability was not requested or could not be established
};

std::string_view to_token(PublicationDurability durability) noexcept;

struct PublicationReceipt {
  TopologyGeneration generation{};
  TopologyGeneration parent_generation{};
  Digest digest{};
  MutationId mutation;
  AttemptOrdinal attempt;
  CommitSequence commit_sequence{};
  /// True when the result was served from an accepted-attempt record because
  /// this exact attempt had already been accepted.
  bool replayed = false;
  TopologyGeneration head_after{};
  Digest head_digest_after{};
  PublicationDurability durability = PublicationDurability::NotDurable;
};

enum class VerifySeverity : std::uint8_t {
  Info = 0,
  Warning = 1,
  Defect = 2,
};

std::string_view to_token(VerifySeverity severity) noexcept;

struct VerifyFinding {
  VerifySeverity severity = VerifySeverity::Info;
  std::string code;
  std::string subject;
  std::string detail;
};

struct VerifyOptions {
  /// Re-read and re-validate every retained generation file, not just the head.
  bool deep = true;
  /// Check accepted-attempt records for shape and digest agreement.
  bool verify_idempotency = true;
  /// Recompute digests from decoded payloads and compare (canonical fixed point
  /// check).
  bool verify_canonical_fixed_point = true;
};

struct VerifyReport {
  StoreId store_id;
  TopologyGeneration head{};
  Digest head_digest{};
  bool head_verified = false;
  bool manifest_verified = false;
  bool floor_verified = false;
  bool chain_verified = false;
  bool canonical_fixed_point_verified = false;
  bool recovered_state = false;
  bool publication_allowed = false;
  std::size_t generations_present = 0;
  std::size_t generations_verified = 0;
  /// Uncommitted staged content found. It is never authoritative and is removed
  /// by the next publication or recovery; verification only reports it.
  std::size_t staged_residue_found = 0;
  /// Generation files newer than the committed head. They were never committed
  /// and are reported, never adopted.
  std::size_t orphan_generations_found = 0;
  /// Retained generation files that are not part of the committed chain.
  std::size_t unreferenced_generations_found = 0;
  /// Unverifiable generation files that recovery moved aside.
  std::size_t quarantined_found = 0;
  std::vector<VerifyFinding> findings;

  bool ok() const noexcept;
};

struct RecoveryOptions {
  /// Adopt the retained previous publication when the head cannot be verified.
  bool adopt_previous = true;
  /// Re-verify every retained generation after adopting.
  bool deep_verify = true;
};

/// Outcome of a recovery that completed.
///
/// A refusal to recover is not an outcome: when nothing can be adopted the call
/// fails with RecoveryUnavailable, HeadCorrupt or GenerationFloorViolation and
/// the store stays unverifiable, so a successful report never describes a store
/// that is still broken.
enum class RecoveryOutcome : std::uint8_t {
  NoAction = 0,        ///< the head was already valid
  AdoptedPrevious = 1, ///< the previous committed publication became the head
};

std::string_view to_token(RecoveryOutcome outcome) noexcept;

struct RecoveryReport {
  RecoveryOutcome outcome = RecoveryOutcome::NoAction;
  TopologyGeneration head_before{};
  TopologyGeneration head_after{};
  Digest head_digest_after{};
  std::size_t residue_removed = 0;
  bool floor_respected = true;
  std::vector<std::string> steps;
  std::string explanation;
};

/// How a store handle is opened.
struct StoreOptions {
  /// Authoritative store directory. Must not be a reparse point (symlink or
  /// junction), must not contain parent-directory components and must be
  /// absolute, so that two processes cannot obtain different locks for the same
  /// logical store.
  std::string root;
  StoreMode mode = StoreMode::ReadWrite;
  /// Create the store directory and initialize an empty store when missing.
  bool create_if_missing = false;
  /// Whole prior generations kept for recovery and history. Bounded.
  std::size_t retained_generations = limits::kMaxRetainedGenerations;
  /// Accepted-attempt records kept for idempotent replay. Bounded; the oldest
  /// records are evicted in publication order.
  std::size_t idempotency_retention = limits::kDefaultIdempotencyRetention;
  /// Durable flush of staged content and directories. When false the store
  /// reports PublicationDurability::NotDurable instead of claiming durability.
  bool durable_flush = true;
  /// Enables the documented fault-injection points (process self-termination).
  /// Off by default; a production store must never be opened with this on.
  bool enable_fault_injection = false;
};

/// A durable store of immutable topology generations with one authoritative head
/// manifest.
///
/// On-disk layout (all file names ASCII, all content length-bounded):
///
///   manifest           authoritative head record (text, checksummed)
///   manifest.prev      previous committed head record
///   floor              durable monotone generation floor
///   lock               writer lock (OS-level exclusive lock, diagnostic text)
///   generations/       immutable generation files, one per published state
///   idem/              accepted-attempt records for idempotent replay
///   staging/           transient staging area, empty between publications
///
/// Publication protocol and its single commit point are documented in the
/// README. In short: validate, reserve generation, stage, flush, read back and
/// verify, atomically rename into generations/, commit the head manifest (the
/// commit point), advance the floor, retire residue.
///
/// Idempotency is bounded. The store keeps the most recent
/// StoreOptions::idempotency_retention accepted attempts. An attempt whose
/// record has been evicted is no longer recognizable as a replay: the next
/// request that reuses that mutation identity and attempt ordinal is treated as
/// a new mutation, unless its content digest collides with a still-retained
/// record. A caller that needs the replay guarantee must retry within the
/// retention window; a caller that cannot know the window must use a fresh
/// mutation identity rather than rely on replay.
///
/// ErrorCode::IdempotencyEvicted is reserved for a store format that records
/// eviction tombstones and is deliberately not produced by this format: this
/// implementation never claims to have detected an eviction, because it cannot
/// distinguish an evicted attempt from an unseen one.
class Store {
 public:
  Store() noexcept;
  ~Store();
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  /// Initializes a new store. Fails with StoreNotEmpty when the directory
  /// already holds a store, and with StoreNotFound when create_if_missing is
  /// false and the directory does not exist.
  static Result<Store> create(const StoreOptions& options, StoreId store_id, const ExternalRef& facility);

  /// Opens an existing store. In ReadWrite mode the exclusive writer lock is
  /// taken and the next writer epoch is reserved durably before the handle is
  /// returned; a second concurrent writer fails with StoreLocked.
  static Result<Store> open(const StoreOptions& options);

  /// Releases the writer lock. Idempotent. A closed store refuses every
  /// operation with StoreClosed.
  Result<void> close();
  bool is_open() const noexcept;

  /// Store identity, binding, head and authority state.
  Result<StoreInfo> info() const;

  StoreId store_id() const noexcept;
  const ExternalRef& facility() const noexcept;
  WriterEpoch epoch() const noexcept;
  WriterIncarnation incarnation() const noexcept;
  StoreMode mode() const noexcept;
  StoreOpenState open_state() const noexcept;
  const std::string& root() const noexcept;

  /// The current head generation. Never returns an unverified payload: the
  /// canonical image is re-read, integrity-checked and re-validated.
  Result<Topology> head() const;
  /// A retained generation by number. GenerationNotRetained when it has been
  /// retired by the retention policy.
  Result<Topology> load(TopologyGeneration generation) const;
  /// Retained history, newest first.
  Result<std::vector<HistoryEntry>> history() const;

  /// Publishes a new generation. See PublicationRequest for authority and
  /// idempotency rules.
  Result<PublicationReceipt> publish(const PublicationRequest& request);

  /// Full or shallow store verification.
  Result<VerifyReport> verify(const VerifyOptions& options = {}) const;

  /// Conservative recovery. Adopts the retained previous publication only when
/// the current head cannot be verified, and never adopts anything below the
/// durable generation floor. When the head is healthy the call succeeds with
/// RecoveryOutcome::NoAction and changes nothing.
  Result<RecoveryReport> recover(const RecoveryOptions& options = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_STORE_HPP
