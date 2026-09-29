// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store of immutable topology generations with one authoritative head
// manifest and exactly one commit point.
//
// Publication protocol (the crash-consistency contract of this component):
//   1. validate the request shape and consult the accepted-attempt record; a
//      replay of an accepted attempt returns the recorded receipt and performs
//      no re-actuation;
//   2. fence the request authority and the expected base generation;
//   3. reserve the next generation number and the next commit sequence;
//   4. build and validate the draft as that generation;
//   5. stage the framed generation under staging/ and flush it;
//   6. read the staged file back, decode it and re-verify it;
//   7. atomically rename it into generations/;
//   8. write the accepted-attempt record as pending;
//   9. atomically replace the head manifest - the single commit point;
//  10. advance the durable floor;
//  11. retire generations outside the retention window and evict accepted-attempt
//      records beyond the idempotency retention.
//
// Every step before the commit point leaves the previous head authoritative and
// every step after it leaves the new generation authoritative, so recovery can
// always adopt exactly one whole verified state and never a partial one. The
// generation file is never authoritative on its own: it becomes a generation of
// this store only when the manifest that names it has been committed.

#include "dccp/cooling_topology/store.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/version.hpp"
#include "file_ops.hpp"

namespace dccp::cooling_topology {
namespace {

// ---------------------------------------------------------------------------
// Durable record grammar
// ---------------------------------------------------------------------------
//
// Every durable record is a checksummed text record: a magic/version header
// line, key/value lines, and a final "checksum <hex>" line whose digest covers
// every byte written before it. The grammar is deliberately identical for the
// head manifest, the durable floor and the accepted-attempt records so that one
// reader, and one set of rejections, serves all three.

constexpr std::string_view kManifestMagic = "CLDT-MANIFEST";
constexpr std::string_view kFloorMagic = "CLDT-FLOOR";
constexpr std::string_view kIdempotencyMagic = "CLDT-IDEM";
constexpr std::uint32_t kRecordVersion = 1;

/// On-disk names, exactly as documented in store.hpp.
constexpr std::string_view kManifestFile = "manifest";
constexpr std::string_view kManifestPreviousFile = "manifest.prev";
constexpr std::string_view kFloorFile = "floor";
constexpr std::string_view kLockFile = "lock";
constexpr std::string_view kGenerationsDir = "generations";
constexpr std::string_view kIdempotencyDir = "idem";
constexpr std::string_view kStagingDir = "staging";
constexpr std::string_view kQuarantineDir = "quarantine";

/// The retained-generation list is the only key a manifest may repeat.
constexpr std::string_view kRetainedKey = "retained";
constexpr std::string_view kChecksumLinePrefix = "checksum ";
constexpr std::string_view kGenerationFileSuffix = ".ctgen";

/// Upper bound of the checksum line, so a serializer can prove that the record
/// it is about to write still fits the bound its reader enforces.
constexpr std::size_t kMaxChecksumLineBytes = kChecksumLinePrefix.size() + Digest::kBytes * 2 + 1;

// ---------------------------------------------------------------------------
// Crash points
// ---------------------------------------------------------------------------
//
// Fault injection terminates the process at a documented publication stage. Each
// publication crash point is named twice: the historical hyphenated stage name
// and the dotted "publish." name documented for this component. A selector
// written in either spelling terminates the process at the same place, and each
// spelling keeps its own occurrence counter inside the file_ops hook, so an
// "#n" selector fires exactly as often as it would without the alias.

constexpr const char* kCrashPartialManifest = "partial-manifest-write";
constexpr const char* kCrashPartialManifestPublication = "publish.partial_manifest_write";
constexpr const char* kCrashBeforeManifestPrevious = "before-manifest-prev-update";
constexpr const char* kCrashBeforeManifestPreviousPublication = "publish.before_manifest_previous_update";
constexpr const char* kCrashAfterManifestPrevious = "after-manifest-prev-update";
constexpr const char* kCrashAfterManifestPreviousPublication = "publish.after_manifest_previous_update";
constexpr const char* kCrashBeforeManifestCommit = "before-manifest-commit";
constexpr const char* kCrashBeforeManifestCommitPublication = "publish.before_manifest_write";
constexpr const char* kCrashAfterManifestCommit = "after-manifest-commit";
constexpr const char* kCrashAfterManifestCommitPublication = "publish.after_manifest_write";
constexpr const char* kCrashAfterStaging = "after-staging-flush";
constexpr const char* kCrashAfterStagingPublication = "publish.after_stage";
constexpr const char* kCrashBeforeGenerationRename = "before-generation-rename";
constexpr const char* kCrashBeforeGenerationRenamePublication = "publish.before_generation_rename";
constexpr const char* kCrashAfterGenerationRename = "after-generation-rename";
constexpr const char* kCrashAfterGenerationRenamePublication = "publish.after_generation_rename";
constexpr const char* kCrashAfterIdempotencyPending = "after-idempotency-pending";
constexpr const char* kCrashAfterIdempotencyPendingPublication = "publish.after_idempotency_pending";
constexpr const char* kCrashBeforeFloor = "before-floor";
constexpr const char* kCrashBeforeFloorPublication = "publish.before_floor";
constexpr const char* kCrashAfterIdempotencyAccept = "after-idempotency-accept";
constexpr const char* kCrashAfterIdempotencyAcceptPublication = "publish.after_idempotency_accept";
constexpr const char* kCrashAfterCommit = "after-commit";
constexpr const char* kCrashAfterCommitPublication = "publish.after_commit";

#if defined(_WIN32)
constexpr char kSeparator = '\\';
#else
constexpr char kSeparator = '/';
#endif

std::string join(const std::string& root, std::string_view name) {
  std::string out = root;
  out.push_back(kSeparator);
  out.append(name);
  return out;
}

/// Fires one crash point under both of its documented names. The process is
/// terminated immediately and non-interactively, never through a CRT abort.
void fault_here(bool enabled, const char* stage, const char* publication_stage) {
  internal::fault_point(enabled, stage);
  internal::fault_point(enabled, publication_stage);
}

/// True when the partial-manifest-write crash point was selected under either of
/// its names. The caller writes a truncated staging record and then terminates.
bool partial_manifest_crash_selected(bool enabled) {
  return internal::fault_selected(enabled, kCrashPartialManifest) ||
         internal::fault_selected(enabled, kCrashPartialManifestPublication);
}

/// Appends the magic and record-version header line.
void append_header(std::string& out, std::string_view magic) {
  out.append(magic);
  out.push_back(' ');
  out.append(std::to_string(kRecordVersion));
  out.push_back('\n');
}

/// Appends the checksum line. The digest is computed into a local first: the
/// evaluation order of a chained append is unspecified, so the digest must never
/// be an argument of an expression that also extends the same buffer.
void append_checksum(std::string& out) {
  const std::string checksum = digest_bytes(out).to_hex();
  out.append(kChecksumLinePrefix);
  out.append(checksum);
  out.push_back('\n');
}

/// Parses a checksummed text record. The checksum covers every byte before the
/// checksum line, so a record edited anywhere - including a record whose header
/// or key lines were rewritten - is refused rather than reinterpreted.
Result<std::vector<std::pair<std::string, std::string>>> parse_record(std::string_view bytes,
                                                                     std::string_view magic,
                                                                     std::size_t max_bytes) {
  if (bytes.empty()) {
    return Error(ErrorCode::EmptyInput, "record is empty");
  }
  if (bytes.size() > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "record exceeds the configured bound");
  }
  const std::size_t checksum_marker = bytes.rfind(kChecksumLinePrefix);
  if (checksum_marker == std::string_view::npos) {
    return Error(ErrorCode::HeadCorrupt, "record has no checksum line");
  }
  const std::size_t newline_after = bytes.find('\n', checksum_marker);
  if (newline_after == std::string_view::npos || newline_after + 1 != bytes.size()) {
    return Error(ErrorCode::HeadCorrupt, "record checksum line is not the last line");
  }
  std::string_view checksum_text = bytes.substr(checksum_marker + kChecksumLinePrefix.size());
  checksum_text = checksum_text.substr(0, checksum_text.size() - 1);
  CT_TRY(expected, Digest::parse_hex(checksum_text));
  const Digest actual = digest_bytes(bytes.substr(0, checksum_marker));
  if (expected != actual) {
    return Error(ErrorCode::DigestMismatch, "record checksum does not match its content");
  }

  std::vector<std::pair<std::string, std::string>> fields;
  std::size_t cursor = 0;
  bool first = true;
  while (cursor < checksum_marker) {
    const std::size_t newline = bytes.find('\n', cursor);
    if (newline == std::string_view::npos || newline > checksum_marker) {
      return Error(ErrorCode::HeadCorrupt, "record line is not terminated");
    }
    const std::string_view line = bytes.substr(cursor, newline - cursor);
    cursor = newline + 1;
    if (first) {
      first = false;
      std::istringstream header_stream{std::string(line)};
      std::string token;
      std::uint32_t version = 0;
      header_stream >> token >> version;
      if (token != magic) {
        return Error(ErrorCode::HeadCorrupt, "record magic does not match").with_subject(std::string(line));
      }
      if (version != kRecordVersion) {
        return Error(ErrorCode::UnsupportedSchemaVersion, "record version is not supported")
            .with_subject(std::to_string(version));
      }
      continue;
    }
    if (line.empty()) {
      continue;
    }
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos || space == 0) {
      return Error(ErrorCode::HeadCorrupt, "record line is not a key/value pair");
    }
    fields.emplace_back(std::string(line.substr(0, space)), std::string(line.substr(space + 1)));
  }
  return fields;
}

/// Reads one required decimal field. The magnitude is accumulated with a checked
/// multiply so an oversized value is refused instead of wrapping.
Result<std::uint64_t> field_u64(const std::vector<std::pair<std::string, std::string>>& fields,
                                std::string_view key) {
  for (const auto& field : fields) {
    if (field.first != key) {
      continue;
    }
    const std::string& text = field.second;
    if (text.empty()) {
      return Error(ErrorCode::MalformedNumber, "empty numeric field").with_subject(std::string(key));
    }
    std::uint64_t value = 0;
    for (const char character : text) {
      if (character < '0' || character > '9') {
        return Error(ErrorCode::MalformedNumber, "malformed numeric field").with_subject(std::string(key));
      }
      const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
      if (value > (UINT64_MAX - digit) / 10u) {
        return Error(ErrorCode::MalformedNumber, "numeric field overflows").with_subject(std::string(key));
      }
      value = value * 10u + digit;
    }
    return value;
  }
  return Error(ErrorCode::MissingField, "required record field is absent").with_subject(std::string(key));
}

/// Reads one required text field.
Result<std::string> field_text(const std::vector<std::pair<std::string, std::string>>& fields,
                               std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      return field.second;
    }
  }
  return Error(ErrorCode::MissingField, "required record field is absent").with_subject(std::string(key));
}

/// Reads one optional text field; absence is the empty string.
Result<std::string> field_optional_text(const std::vector<std::pair<std::string, std::string>>& fields,
                                        std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      return field.second;
    }
  }
  return std::string();
}

/// Reads every value recorded under one key, in file order. Used only for the
/// retained-generation list, which is the only repeated key in the grammar.
Result<std::vector<std::string>> field_all(const std::vector<std::pair<std::string, std::string>>& fields,
                                          std::string_view key) {
  std::vector<std::string> values;
  for (const auto& field : fields) {
    if (field.first == key) {
      values.push_back(field.second);
    }
  }
  return values;
}

std::string hex_encode(std::string_view bytes) { return to_hex(bytes); }

Result<std::string> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return Error(ErrorCode::MalformedRecord, "hexadecimal field has an odd length");
  }
  std::string out;
  out.reserve(hex.size() / 2);
  const auto value_of = [](char digit) -> int {
    if (digit >= '0' && digit <= '9') {
      return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
      return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
      return digit - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const int high = value_of(hex[index]);
    const int low = value_of(hex[index + 1]);
    if (high < 0 || low < 0) {
      return Error(ErrorCode::MalformedRecord, "hexadecimal field contains a non-hex character");
    }
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Head manifest
// ---------------------------------------------------------------------------

/// One generation recorded as retained by the committed head manifest. The
/// recorded digest and commit sequence are the values that were committed with
/// that generation; they are never recomputed from the current head.
struct RetainedGeneration {
  TopologyGeneration generation{};
  Digest digest{};
  CommitSequence commit_sequence{};
};

/// The authoritative head record. The commit sequence is the store-level
/// counter: it starts at zero, strictly increases by one at every committed
/// publication, and is persisted here so a reopened or recovered store reports
/// the committed value instead of inventing one.
struct Manifest {
  StoreId store_id;
  ExternalRef facility;
  TopologyGeneration head{};
  Digest head_digest{};
  TopologyGeneration parent_generation{};
  Digest parent_digest{};
  TopologyGeneration floor{};
  CommitSequence commit_sequence{};
  WriterEpoch epoch{};
  WriterIncarnation incarnation{};
  std::uint64_t idempotency_records = 0;
  std::vector<RetainedGeneration> retained;
  std::uint16_t schema = kCanonicalSchemaVersion;
};

Result<std::string> serialize_manifest(const Manifest& manifest) {
  std::string out;
  append_header(out, kManifestMagic);
  out.append("schema ").append(std::to_string(manifest.schema)).append("\n");
  out.append("store ").append(manifest.store_id.str()).append("\n");
  out.append("facility ")
      .append(to_token(manifest.facility.kind))
      .append(" ")
      .append(hex_encode(manifest.facility.identity))
      .append(" ")
      .append(std::to_string(manifest.facility.generation.value()))
      .append("\n");
  out.append("head ").append(std::to_string(manifest.head.value())).append("\n");
  out.append("head-digest ").append(manifest.head_digest.to_hex()).append("\n");
  out.append("parent ").append(std::to_string(manifest.parent_generation.value())).append("\n");
  out.append("parent-digest ").append(manifest.parent_digest.to_hex()).append("\n");
  out.append("floor ").append(std::to_string(manifest.floor.value())).append("\n");
  out.append("commit-sequence ").append(std::to_string(manifest.commit_sequence.value())).append("\n");
  out.append("epoch ").append(std::to_string(manifest.epoch.value())).append("\n");
  out.append("incarnation ").append(std::to_string(manifest.incarnation.value())).append("\n");
  for (const RetainedGeneration& entry : manifest.retained) {
    out.append(kRetainedKey)
        .append(" ")
        .append(std::to_string(entry.generation.value()))
        .append(" ")
        .append(entry.digest.to_hex())
        .append(" ")
        .append(std::to_string(entry.commit_sequence.value()))
        .append("\n");
  }
  out.append("idempotency ").append(std::to_string(manifest.idempotency_records)).append("\n");
  if (out.size() + kMaxChecksumLineBytes > limits::kMaxManifestBytes) {
    return Error(ErrorCode::LimitExceeded, "manifest exceeds the documented bound");
  }
  append_checksum(out);
  return out;
}

/// Parses and cross-checks a head manifest. Every field a decision depends on is
/// validated here: an unsupported schema, a repeated key, a head without a
/// digest or commit sequence, a retained list that is not strictly ascending, or
/// a retained generation newer than the head is refused rather than repaired.
Result<Manifest> parse_manifest(std::string_view bytes) {
  CT_TRY(fields, parse_record(bytes, kManifestMagic, limits::kMaxManifestBytes));
  std::unordered_set<std::string> seen;
  for (const auto& field : fields) {
    if (field.first == kRetainedKey) {
      continue;  // the retained-generation list is the only repeated key
    }
    if (!seen.insert(field.first).second) {
      return Error(ErrorCode::DuplicateField, "manifest repeats a key").with_subject(field.first);
    }
  }
  Manifest manifest;
  CT_TRY(schema, field_u64(fields, "schema"));
  if (schema != kCanonicalSchemaVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "manifest schema version is not supported")
        .with_subject(std::to_string(schema));
  }
  manifest.schema = static_cast<std::uint16_t>(schema);

  CT_TRY(store_text, field_text(fields, "store"));
  CT_TRY(store_id, StoreId::parse(store_text));
  manifest.store_id = std::move(store_id);

  CT_TRY(facility_text, field_text(fields, "facility"));
  {
    std::istringstream stream(facility_text);
    std::string kind_token;
    std::string identity_hex;
    std::uint64_t generation = 0;
    stream >> kind_token >> identity_hex >> generation;
    if (stream.fail() || !stream.eof()) {
      return Error(ErrorCode::MalformedRecord, "manifest facility field is malformed");
    }
    CT_TRY(kind, parse_external_ref_kind(kind_token));
    CT_TRY(identity, hex_decode(identity_hex));
    CT_TRY(reference, ExternalRef::create(kind, std::move(identity), ExternalGeneration(generation)));
    manifest.facility = std::move(reference);
  }

  CT_TRY(head, field_u64(fields, "head"));
  manifest.head = TopologyGeneration(head);
  CT_TRY(head_digest, field_text(fields, "head-digest"));
  CT_TRY(head_digest_value, Digest::parse_hex(head_digest));
  manifest.head_digest = head_digest_value;
  CT_TRY(parent, field_u64(fields, "parent"));
  manifest.parent_generation = TopologyGeneration(parent);
  CT_TRY(parent_digest, field_text(fields, "parent-digest"));
  CT_TRY(parent_digest_value, Digest::parse_hex(parent_digest));
  manifest.parent_digest = parent_digest_value;
  CT_TRY(floor, field_u64(fields, "floor"));
  manifest.floor = TopologyGeneration(floor);
  CT_TRY(commit_sequence, field_u64(fields, "commit-sequence"));
  manifest.commit_sequence = CommitSequence(commit_sequence);
  CT_TRY(epoch, field_u64(fields, "epoch"));
  manifest.epoch = WriterEpoch(epoch);
  CT_TRY(incarnation, field_u64(fields, "incarnation"));
  manifest.incarnation = WriterIncarnation(incarnation);
  CT_TRY(idempotency, field_u64(fields, "idempotency"));
  manifest.idempotency_records = idempotency;

  // The retained list is bounded before it is parsed, then read entry by entry:
  // a hostile manifest can never ask for an unbounded allocation.
  CT_TRY(retained_values, field_all(fields, kRetainedKey));
  if (retained_values.size() > limits::kMaxRetainedGenerations) {
    return Error(ErrorCode::LimitExceeded, "manifest records more retained generations than the documented bound")
        .with_subject(std::to_string(retained_values.size()));
  }
  TopologyGeneration previous{};
  for (const std::string& value : retained_values) {
    std::istringstream stream(value);
    std::uint64_t generation = 0;
    std::string digest_hex;
    std::uint64_t entry_commit_sequence = 0;
    stream >> generation >> digest_hex >> entry_commit_sequence;
    if (stream.fail() || !stream.eof()) {
      return Error(ErrorCode::MalformedRecord, "manifest retained field is malformed").with_subject(value);
    }
    RetainedGeneration entry;
    entry.generation = TopologyGeneration(generation);
    CT_TRY(digest, Digest::parse_hex(digest_hex));
    entry.digest = digest;
    entry.commit_sequence = CommitSequence(entry_commit_sequence);
    if (!entry.generation.published() || entry.digest.is_zero() || !entry.commit_sequence.committed()) {
      return Error(ErrorCode::HeadCorrupt, "manifest retained entry is incomplete").with_subject(value);
    }
    if (entry.generation <= previous) {
      return Error(ErrorCode::HeadCorrupt, "manifest retained list is not strictly ascending").with_subject(value);
    }
    previous = entry.generation;
    manifest.retained.push_back(entry);
  }

  // The retained list is validated for shape only. It is deliberately not
  // required to agree with the head here: a manifest that was forged to a lower
  // head must reach the durable-floor check, which refuses a rollback, instead
  // of being classified as an unreadable record that open() would then retry
  // against the retained previous publication.
  if (manifest.head.published()) {
    if (manifest.head_digest.is_zero()) {
      return Error(ErrorCode::HeadCorrupt, "manifest declares a head without a digest");
    }
    if (!manifest.commit_sequence.committed()) {
      return Error(ErrorCode::HeadCorrupt, "manifest declares a head without a commit sequence");
    }
  } else if (!manifest.head_digest.is_zero() || manifest.parent_generation.published() ||
             manifest.commit_sequence.committed() || !manifest.retained.empty()) {
    return Error(ErrorCode::HeadCorrupt,
                 "empty manifest declares a head digest, a parent, a commit sequence or a retained generation");
  }
  if (manifest.epoch.value() == 0) {
    return Error(ErrorCode::HeadCorrupt, "manifest declares a zero writer epoch");
  }
  return manifest;
}

// ---------------------------------------------------------------------------
// Durable floor
// ---------------------------------------------------------------------------

Result<std::string> serialize_floor(const TopologyGeneration& floor) {
  std::string out;
  append_header(out, kFloorMagic);
  out.append("floor ").append(std::to_string(floor.value())).append("\n");
  if (out.size() + kMaxChecksumLineBytes > limits::kMaxManifestBytes) {
    return Error(ErrorCode::LimitExceeded, "floor record exceeds the documented bound");
  }
  append_checksum(out);
  return out;
}

Result<TopologyGeneration> parse_floor(std::string_view bytes) {
  CT_TRY(fields, parse_record(bytes, kFloorMagic, limits::kMaxManifestBytes));
  CT_TRY(floor, field_u64(fields, "floor"));
  return TopologyGeneration(floor);
}

// ---------------------------------------------------------------------------
// Accepted-attempt records
// ---------------------------------------------------------------------------

enum class IdempotencyState : std::uint8_t { Pending = 0, Accepted = 1 };

/// The full recorded outcome of one attempt at a mutation. The record carries
/// the receipt values, so a replay reports what was committed even after the
/// head has moved on.
struct IdempotencyRecord {
  MutationId mutation;
  AttemptOrdinal attempt;
  IdempotencyState state = IdempotencyState::Pending;
  Digest content_digest{};
  TopologyGeneration generation{};
  Digest generation_digest{};
  TopologyGeneration parent_generation{};
  Digest parent_digest{};
  CommitSequence commit_sequence{};
  PublicationDurability durability = PublicationDurability::NotDurable;
};

/// File name of an accepted-attempt record. The name is the digest of the
/// (mutation, attempt) identity, so the identity is never spelled into a path
/// component and cannot collide with a reserved file name.
std::string idempotency_file_name(const MutationId& mutation, const AttemptOrdinal& attempt) {
  std::string key = mutation.str();
  key.push_back('/');
  key.append(std::to_string(attempt.value()));
  return std::string("r") + digest_bytes(key).to_hex() + ".rec";
}

Result<std::string> serialize_idempotency(const IdempotencyRecord& record) {
  std::string out;
  append_header(out, kIdempotencyMagic);
  out.append("mutation ").append(record.mutation.str()).append("\n");
  out.append("attempt ").append(std::to_string(record.attempt.value())).append("\n");
  out.append("state ").append(record.state == IdempotencyState::Accepted ? "accepted" : "pending").append("\n");
  out.append("content-digest ").append(record.content_digest.to_hex()).append("\n");
  out.append("generation ").append(std::to_string(record.generation.value())).append("\n");
  out.append("generation-digest ").append(record.generation_digest.to_hex()).append("\n");
  out.append("parent ").append(std::to_string(record.parent_generation.value())).append("\n");
  out.append("parent-digest ").append(record.parent_digest.to_hex()).append("\n");
  out.append("commit-sequence ").append(std::to_string(record.commit_sequence.value())).append("\n");
  out.append("durability ")
      .append(record.durability == PublicationDurability::Durable ? "durable" : "not_durable")
      .append("\n");
  if (out.size() + kMaxChecksumLineBytes > limits::kMaxIdempotencyRecordBytes) {
    return Error(ErrorCode::LimitExceeded, "accepted-attempt record exceeds the documented bound");
  }
  append_checksum(out);
  return out;
}

Result<IdempotencyRecord> parse_idempotency(std::string_view bytes) {
  CT_TRY(fields, parse_record(bytes, kIdempotencyMagic, limits::kMaxIdempotencyRecordBytes));
  IdempotencyRecord record;
  CT_TRY(mutation_text, field_text(fields, "mutation"));
  CT_TRY(mutation, MutationId::parse(mutation_text));
  record.mutation = std::move(mutation);
  CT_TRY(attempt, field_u64(fields, "attempt"));
  if (attempt > 0xFFFFFFFFull) {
    return Error(ErrorCode::MalformedRecord, "idempotency record attempt ordinal is out of range");
  }
  CT_TRY(ordinal, AttemptOrdinal::parse(static_cast<std::uint32_t>(attempt)));
  record.attempt = ordinal;
  CT_TRY(state_text, field_text(fields, "state"));
  if (state_text == "accepted") {
    record.state = IdempotencyState::Accepted;
  } else if (state_text == "pending") {
    record.state = IdempotencyState::Pending;
  } else {
    return Error(ErrorCode::MalformedRecord, "idempotency record has an unknown state").with_subject(state_text);
  }
  CT_TRY(content_digest, field_text(fields, "content-digest"));
  CT_TRY(content_value, Digest::parse_hex(content_digest));
  record.content_digest = content_value;
  CT_TRY(generation, field_u64(fields, "generation"));
  record.generation = TopologyGeneration(generation);
  CT_TRY(generation_digest, field_text(fields, "generation-digest"));
  CT_TRY(generation_value, Digest::parse_hex(generation_digest));
  record.generation_digest = generation_value;
  CT_TRY(parent, field_u64(fields, "parent"));
  record.parent_generation = TopologyGeneration(parent);
  CT_TRY(parent_digest, field_text(fields, "parent-digest"));
  CT_TRY(parent_value, Digest::parse_hex(parent_digest));
  record.parent_digest = parent_value;
  CT_TRY(commit_sequence, field_u64(fields, "commit-sequence"));
  record.commit_sequence = CommitSequence(commit_sequence);
  if (!record.generation.published() || record.generation_digest.is_zero() ||
      !record.commit_sequence.committed()) {
    return Error(ErrorCode::MalformedRecord, "idempotency record is incomplete");
  }
  CT_TRY(durability, field_optional_text(fields, "durability"));
  record.durability = durability == "durable" ? PublicationDurability::Durable : PublicationDurability::NotDurable;
  return record;
}

// ---------------------------------------------------------------------------
// Generation files
// ---------------------------------------------------------------------------

/// File name of a generation: the generation number in fixed width, so listing
/// order is generation order, and the leading digest bytes, so that a name and a
/// digest cannot disagree silently.
std::string generation_file_name(const TopologyGeneration& generation, const Digest& digest) {
  std::string name = "g";
  const std::string number = std::to_string(generation.value());
  name.append(16 - std::min<std::size_t>(16, number.size()), '0');
  name.append(number);
  name.push_back('-');
  name.append(digest.to_hex().substr(0, 16));
  name.append(kGenerationFileSuffix);
  return name;
}

Result<TopologyGeneration> generation_of_file_name(std::string_view name) {
  if (name.size() < 18 || name[0] != 'g' || name[17] != '-') {
    return Error(ErrorCode::MalformedRecord, "generation file name does not match the layout")
        .with_subject(std::string(name));
  }
  std::uint64_t value = 0;
  for (std::size_t index = 1; index < 17; ++index) {
    const char character = name[index];
    if (character < '0' || character > '9') {
      return Error(ErrorCode::MalformedRecord, "generation file name is not numeric").with_subject(std::string(name));
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
  }
  return TopologyGeneration(value);
}

/// Reads, integrity-checks, decodes and re-validates one generation file.
/// Topology::decode consumes the whole framed file: it checks the frame and the
/// framed payload digest, re-validates the decoded generation (a file edited to
/// be structurally impossible is refused) and requires the canonical image to be
/// a fixed point of re-encoding, so nothing that is not exactly what was
/// committed can be adopted. The path is attached to a failure so a defect names
/// the file it was found in.
Result<Topology> load_generation_file(const std::string& path) {
  CT_TRY(bytes, internal::read_file(path, limits::kMaxGenerationFileBytes));
  const auto decoded = Topology::decode(bytes);
  if (!decoded.has_value()) {
    Error error = decoded.error();
    return error.with_subject(path);
  }
  return decoded.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Store::Impl
// ---------------------------------------------------------------------------

struct Store::Impl {
  StoreOptions options;
  std::string root;
  std::string generations_path;
  std::string idempotency_path;
  std::string staging_path;
  std::string quarantine_path;
  internal::FileLock lock;
  Manifest manifest;
  bool open = false;
  bool writable = false;
  StoreOpenState open_state = StoreOpenState::Reopened;
  /// Set while the handle serves state adopted from the retained previous
  /// publication and the repaired head manifest has not been committed yet. A
  /// handle is never returned in this state: open() either commits the repaired
  /// manifest or fails, and recover() clears it once the adoption is committed.
  bool recovering_manifest = false;

  std::string manifest_path() const { return join(root, kManifestFile); }
  std::string manifest_previous_path() const { return join(root, kManifestPreviousFile); }
  std::string floor_path() const { return join(root, kFloorFile); }
  std::string lock_path() const { return join(root, kLockFile); }
  std::string generation_path(const TopologyGeneration& generation, const Digest& digest) const {
    return join(generations_path, generation_file_name(generation, digest));
  }
  std::string idempotency_path_of(const MutationId& mutation, const AttemptOrdinal& attempt) const {
    return join(idempotency_path, idempotency_file_name(mutation, attempt));
  }
  std::string staging_path_of(const TopologyGeneration& generation) const {
    return join(staging_path, "staged-" + std::to_string(generation.value()) + ".tmp");
  }

  Result<void> require_open() const {
    if (!open) {
      return Error(ErrorCode::StoreClosed, "store handle is closed");
    }
    return ok();
  }

  Result<void> require_writable() const {
    CT_TRYV(require_open());
    if (!writable) {
      return Error(ErrorCode::StoreReadOnly, "store handle was opened read-only");
    }
    return ok();
  }

  /// Moves generation content that cannot be verified into the quarantine
  /// directory. Quarantined content is preserved for diagnosis and is never
  /// adopted, never loaded and never counted as a retained generation. Returns
  /// true when the content really was moved aside.
  bool quarantine_generation(const std::string& name) {
    if (!internal::create_directory(quarantine_path).has_value()) {
      return false;
    }
    return internal::atomic_replace(join(quarantine_path, name), join(generations_path, name), false).has_value();
  }

  /// Every generation file present, ascending. A file whose name does not match
  /// the layout is a refusal, not something to skip silently: it can never be
  /// mistaken for a generation of this store.
  Result<std::vector<std::pair<TopologyGeneration, Digest>>> list_generations() const {
    CT_TRY(names, internal::list_directory(generations_path));
    std::vector<std::pair<TopologyGeneration, Digest>> result;
    for (const std::string& name : names) {
      CT_TRY(generation, generation_of_file_name(name));
      const std::size_t dash = name.find('-');
      CT_TRY(short_digest, Digest::parse_hex(name.substr(dash + 1, 16) + std::string(48, '0')));
      result.emplace_back(generation, short_digest);
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first < rhs.first;
      }
      return lhs.second < rhs.second;
    });
    return result;
  }

  Result<std::string> find_generation_path(const TopologyGeneration& generation) const {
    CT_TRY(names, internal::list_directory(generations_path));
    for (const std::string& name : names) {
      CT_TRY(number, generation_of_file_name(name));
      if (number == generation) {
        return join(generations_path, name);
      }
    }
    return Error(ErrorCode::GenerationNotRetained, "the requested generation is not retained by this store")
        .with_subject(std::to_string(generation.value()));
  }

  /// Writes the head record atomically. The commit point of a publication is
  /// the atomic replacement of the head manifest; everything else here only
  /// prepares for it.
  Result<void> write_manifest(const Manifest& value, bool keep_previous, bool durable) {
    CT_TRY(content, serialize_manifest(value));
    const std::string staged = join(root, "manifest.tmp");
    if (partial_manifest_crash_selected(options.enable_fault_injection)) {
      // A partial staging write. The committed manifest is untouched, so the
      // store must reopen from the last whole publication.
      CT_TRYV(internal::write_file(staged, std::string_view(content).substr(0, content.size() / 2), false));
      internal::terminate_process_now(97);
    }
    CT_TRYV(internal::write_file(staged, content, durable));
    if (keep_previous) {
      fault_here(options.enable_fault_injection, kCrashBeforeManifestPrevious,
                 kCrashBeforeManifestPreviousPublication);
      const auto existing = internal::read_file(manifest_path(), limits::kMaxManifestBytes);
      // The previous-publication slot is only ever filled with a manifest that
      // parses: copying damaged content there would destroy the very fallback
      // that recovery depends on.
      if (existing.has_value() && parse_manifest(existing.value()).has_value()) {
        const std::string previous_staged = join(root, "manifest.prev.tmp");
        CT_TRYV(internal::write_file(previous_staged, existing.value(), durable));
        CT_TRYV(internal::atomic_replace(manifest_previous_path(), previous_staged, durable));
      }
      fault_here(options.enable_fault_injection, kCrashAfterManifestPrevious,
                 kCrashAfterManifestPreviousPublication);
    }
    fault_here(options.enable_fault_injection, kCrashBeforeManifestCommit, kCrashBeforeManifestCommitPublication);
    CT_TRYV(internal::atomic_replace(manifest_path(), staged, durable));
    fault_here(options.enable_fault_injection, kCrashAfterManifestCommit, kCrashAfterManifestCommitPublication);
    return ok();
  }

  /// Advances the durable floor. The floor is monotone: a publication can raise
  /// it, and nothing in this component can lower it.
  Result<void> write_floor(const TopologyGeneration& floor, bool durable) {
    if (floor.value() < manifest.floor.value()) {
      return Error(ErrorCode::GenerationFloorViolation, "the generation floor is monotone and cannot decrease");
    }
    CT_TRY(content, serialize_floor(floor));
    const std::string staged = join(root, "floor.tmp");
    CT_TRYV(internal::write_file(staged, content, durable));
    CT_TRYV(internal::atomic_replace(floor_path(), staged, durable));
    manifest.floor = floor;
    return ok();
  }

  /// Writes one durable record atomically.
  ///
  /// Idempotency records are read by concurrent read-only handles, so writing
  /// them in place would let a reader observe a half-written record and would
  /// also fail outright on a platform that refuses to open a file another
  /// process holds with a conflicting share mode. The content is therefore
  /// staged in the staging directory, read back, and renamed into place, which
  /// is the same discipline the manifest and the floor already follow. The
  /// staging name is derived from the target name, which is safe because a
  /// store has exactly one writer at a time.
  Result<void> write_record_atomically(const std::string& target, std::string_view content, bool durable) {
    const std::size_t separator = target.find_last_of("/\\");
    const std::string name = separator == std::string::npos ? target : target.substr(separator + 1);
    const std::string staged = join(staging_path, name + ".tmp");
    CT_TRYV(internal::write_file(staged, content, durable));
    CT_TRY(readback, internal::read_file(staged, limits::kMaxIdempotencyRecordBytes));
    if (readback != content) {
      (void)internal::remove_file(staged);
      return Error(ErrorCode::PublicationIncomplete, "staged record does not match what was written")
          .with_subject(target);
    }
    CT_TRYV(internal::atomic_replace(target, staged, durable));
    return ok();
  }

  Result<TopologyGeneration> read_floor() const {
    CT_TRY(present, internal::path_exists(floor_path()));
    if (!present) {
      return TopologyGeneration{};
    }
    CT_TRY(bytes, internal::read_file(floor_path(), limits::kMaxManifestBytes));
    return parse_floor(bytes);
  }

  /// Removes staging residue. Staged content is never authoritative: a
  /// publication always writes a fresh staging file and renames it into place.
  void remove_staging_residue(std::size_t* removed) {
    const Result<std::vector<std::string>> names = internal::list_directory(staging_path);
    if (!names.has_value()) {
      return;
    }
    for (const std::string& name : names.value()) {
      if (internal::remove_file(join(staging_path, name)).has_value() && removed != nullptr) {
        ++(*removed);
      }
    }
  }

  /// The committed commit sequence recorded for a retained generation, or the
  /// zero sequence when this manifest does not record one.
  CommitSequence recorded_commit_sequence(const TopologyGeneration& generation) const {
    for (const RetainedGeneration& entry : manifest.retained) {
      if (entry.generation == generation) {
        return entry.commit_sequence;
      }
    }
    return CommitSequence{};
  }

  /// The retained window a publication of the new generation records: the new
  /// head plus every generation the previous manifest recorded that is still at
  /// or above the floor that publication establishes. The result stays strictly
  /// ascending and is bounded by the retention window, because the window is
  /// exactly the generation range from that floor to the new head.
  std::vector<RetainedGeneration> retained_after(const TopologyGeneration& floor_candidate,
                                                 const TopologyGeneration& generation, const Digest& digest,
                                                 const CommitSequence& commit_sequence) const {
    std::vector<RetainedGeneration> retained;
    retained.reserve(limits::kMaxRetainedGenerations);
    for (const RetainedGeneration& entry : manifest.retained) {
      if (entry.generation >= floor_candidate && entry.generation < generation) {
        retained.push_back(entry);
      }
    }
    RetainedGeneration newest;
    newest.generation = generation;
    newest.digest = digest;
    newest.commit_sequence = commit_sequence;
    retained.push_back(newest);
    return retained;
  }

  /// The receipt of an attempt that was already accepted. It reports the
  /// recorded outcome and the current head; it never re-derives the generation.
  PublicationReceipt replay_receipt(const IdempotencyRecord& record, const PublicationRequest& request) const {
    PublicationReceipt receipt;
    receipt.generation = record.generation;
    receipt.parent_generation = record.parent_generation;
    receipt.digest = record.generation_digest;
    receipt.mutation = request.mutation;
    receipt.attempt = request.attempt;
    receipt.commit_sequence = record.commit_sequence;
    receipt.replayed = true;
    receipt.head_after = manifest.head;
    receipt.head_digest_after = manifest.head_digest;
    receipt.durability = record.durability;
    return receipt;
  }
};

namespace {

/// Bound checks shared by create() and open(). A retention window of zero would
/// leave the store unable to keep even its own head, and a window above the
/// documented bound would let a caller ask for unbounded durable state.
Result<void> validate_store_bounds(const StoreOptions& options) {
  if (options.retained_generations == 0 || options.retained_generations > limits::kMaxRetainedGenerations) {
    return Error(ErrorCode::LimitExceeded, "retained_generations must be between 1 and the documented bound");
  }
  if (options.idempotency_retention == 0 || options.idempotency_retention > limits::kMaxIdempotencyRecords) {
    return Error(ErrorCode::LimitExceeded, "idempotency_retention must be between 1 and the documented bound");
  }
  return ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// Store lifecycle
// ---------------------------------------------------------------------------

Store::Store() noexcept = default;
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

Result<Store> Store::create(const StoreOptions& options, StoreId store_id, const ExternalRef& facility) {
  if (store_id.empty()) {
    return Error(ErrorCode::InvalidArgument, "store identity must not be empty");
  }
  if (facility.kind != ExternalRefKind::Facility) {
    return Error(ErrorCode::InvalidArgument, "a store is bound to a facility reference");
  }
  CT_TRYV(validate_store_bounds(options));
  // The root is canonicalized for the lifetime of the handle: absolute, one
  // separator, no parent component and no reparse point on any existing
  // component, so that two processes cannot obtain different locks for the same
  // logical store.
  CT_TRY(canonical_root, internal::canonicalize_store_root(options.root));

  Store store;
  store.impl_ = std::make_unique<Impl>();
  Impl& impl = *store.impl_;
  impl.options = options;
  impl.root = canonical_root;
  impl.generations_path = join(impl.root, kGenerationsDir);
  impl.idempotency_path = join(impl.root, kIdempotencyDir);
  impl.staging_path = join(impl.root, kStagingDir);
  impl.quarantine_path = join(impl.root, kQuarantineDir);
  impl.writable = options.mode == StoreMode::ReadWrite;

  CT_TRY(existing, internal::inspect_path(impl.root));
  if (existing.exists && !existing.is_directory) {
    return Error(ErrorCode::PathNotRegular, "the store root exists and is not a directory").with_subject(impl.root);
  }
  if (!existing.exists && !options.create_if_missing) {
    return Error(ErrorCode::StoreNotFound, "the store root does not exist and create_if_missing is not set")
        .with_subject(impl.root);
  }
  // The root and any missing parent of it are created through the checked
  // primitive: a component that exists as a file or as a reparse point is
  // refused rather than followed.
  CT_TRYV(internal::create_directories(impl.root));

  // The writer lock is taken once, before any durable record is written, and is
  // released by close(). It is never taken recursively.
  if (impl.writable) {
    CT_TRY(lock, internal::FileLock::acquire(impl.lock_path(), "fresh"));
    impl.lock = std::move(lock);
  }
  CT_TRY(names, internal::list_directory(impl.root));
  for (const std::string& name : names) {
    if (name != kLockFile) {
      return Error(ErrorCode::StoreNotEmpty, "the store directory already holds content").with_subject(name);
    }
  }

  CT_TRYV(internal::create_directory(impl.generations_path));
  CT_TRYV(internal::create_directory(impl.idempotency_path));
  CT_TRYV(internal::create_directory(impl.staging_path));

  // An empty store has no head, no parent, no commit sequence and no retained
  // generation; its writer epoch starts reserved at 1 so that a mutation planned
  // against epoch 0 is always refused.
  impl.manifest = Manifest{};
  impl.manifest.store_id = store_id;
  impl.manifest.facility = facility;
  impl.manifest.epoch = WriterEpoch(1);
  impl.manifest.incarnation = WriterIncarnation(1);
  CT_TRYV(impl.write_manifest(impl.manifest, false, options.durable_flush));
  CT_TRYV(impl.write_floor(TopologyGeneration{}, options.durable_flush));
  impl.open = true;
  impl.open_state = StoreOpenState::Fresh;
  return store;
}

Result<Store> Store::open(const StoreOptions& options) {
  CT_TRYV(validate_store_bounds(options));
  // The same canonical root the store was created with, so that a reopen - or a
  // second process using a different spelling of the same directory - addresses
  // exactly one lock file.
  CT_TRY(canonical_root, internal::canonicalize_store_root(options.root));

  Store store;
  store.impl_ = std::make_unique<Impl>();
  Impl& impl = *store.impl_;
  impl.options = options;
  impl.root = canonical_root;
  impl.generations_path = join(impl.root, kGenerationsDir);
  impl.idempotency_path = join(impl.root, kIdempotencyDir);
  impl.staging_path = join(impl.root, kStagingDir);
  impl.quarantine_path = join(impl.root, kQuarantineDir);
  impl.writable = options.mode == StoreMode::ReadWrite;

  CT_TRY(root_info, internal::inspect_path(impl.root));
  if (!root_info.exists) {
    return Error(ErrorCode::StoreNotFound, "store root does not exist").with_subject(impl.root);
  }
  if (!root_info.is_directory) {
    return Error(ErrorCode::PathNotRegular, "store root is not a directory").with_subject(impl.root);
  }

  if (impl.writable) {
    CT_TRY(lock, internal::FileLock::acquire(impl.lock_path(), "writer"));
    impl.lock = std::move(lock);
  }

  // The committed manifest is read first; the floor is then required to be
  // present once a generation has been published, because a missing rollback
  // guard must never be silently replaced by "no floor at all".
  std::string manifest_error;
  bool manifest_loaded = false;
  const auto manifest_bytes = internal::read_file(impl.manifest_path(), limits::kMaxManifestBytes);
  if (manifest_bytes.has_value()) {
    const auto parsed = parse_manifest(manifest_bytes.value());
    if (parsed.has_value()) {
      impl.manifest = parsed.value();
      manifest_loaded = true;
    } else {
      manifest_error = parsed.error().to_string();
    }
  } else {
    manifest_error = manifest_bytes.error().to_string();
  }

  CT_TRY(floor_file_present, internal::path_exists(impl.floor_path()));
  if (floor_file_present) {
    CT_TRY(floor, impl.read_floor());
    impl.manifest.floor = floor;
  } else if (manifest_loaded && impl.manifest.head.published()) {
    return Error(ErrorCode::IntegrityFailure,
                 "the durable generation floor is missing while a published head exists; the rollback guard cannot "
                 "be established");
  }

  bool head_verified = false;
  if (manifest_loaded && impl.manifest.head.published() && impl.manifest.head < impl.manifest.floor) {
    return Error(ErrorCode::GenerationFloorViolation,
                 "the committed head is below the durable generation floor; this state is a rollback")
        .with_subject(std::to_string(impl.manifest.head.value()));
  }
  if (manifest_loaded && impl.manifest.head.published()) {
    const auto head_path = impl.find_generation_path(impl.manifest.head);
    if (head_path.has_value()) {
      const auto head_topology = load_generation_file(head_path.value());
      if (head_topology.has_value() && head_topology.value().digest() == impl.manifest.head_digest &&
          head_topology.value().generation() == impl.manifest.head) {
        head_verified = true;
      } else {
        manifest_error = head_topology.has_value() ? "the head generation digest does not match the manifest"
                                                   : head_topology.error().to_string();
      }
    } else {
      manifest_error = head_path.error().to_string();
    }
  } else if (manifest_loaded) {
    head_verified = true;  // an empty store has no head to verify
  }

  if (!manifest_loaded || !head_verified) {
    // Conservative recovery: adopt the retained previous *committed*
    // publication, never an uncommitted file, never a partial generation and
    // never anything below the durable floor.
    const auto previous_bytes = internal::read_file(impl.manifest_previous_path(), limits::kMaxManifestBytes);
    if (!previous_bytes.has_value()) {
      CT_TRY(manifest_present, internal::path_exists(impl.manifest_path()));
      if (manifest_loaded || manifest_present) {
        return Error(ErrorCode::HeadCorrupt,
                     "the committed publication could not be verified and no retained previous publication exists")
            .with_detail(manifest_error);
      }
      return Error(ErrorCode::HeadMissing, "the store has no manifest").with_detail(manifest_error);
    }
    CT_TRY(previous, parse_manifest(previous_bytes.value()));
    if (previous.head.published() && !floor_file_present) {
      return Error(ErrorCode::IntegrityFailure,
                   "the durable generation floor is missing, so the retained previous publication cannot be checked "
                   "against it");
    }
    if (previous.head.published() && previous.head < impl.manifest.floor) {
      return Error(ErrorCode::GenerationFloorViolation,
                   "the retained previous publication is below the durable generation floor and was refused")
          .with_subject(std::to_string(previous.head.value()));
    }
    if (previous.head.published()) {
      CT_TRY(previous_path, impl.find_generation_path(previous.head));
      const auto recovered = load_generation_file(previous_path);
      if (!recovered.has_value()) {
        return Error(ErrorCode::RecoveryUnavailable, "the retained previous publication could not be verified")
            .with_detail(recovered.error().to_string());
      }
      if (recovered.value().digest() != previous.head_digest) {
        return Error(ErrorCode::DigestMismatch,
                     "the retained previous publication digest does not match its manifest record");
      }
    }
    // The adopted publication keeps its own recorded commit sequence: an
    // adoption restores committed state and never invents a new value.
    const TopologyGeneration effective_floor = impl.manifest.floor;
    impl.manifest = previous;
    impl.manifest.floor = effective_floor;
    impl.open_state = StoreOpenState::Recovered;
    impl.recovering_manifest = true;

    // Content that cannot be verified is moved into quarantine rather than
    // deleted: the evidence is preserved, the adoption is unambiguous, and a
    // later publication of the same generation number cannot collide with
    // residue that no version of the store could ever load. Content that does
    // verify is left in place even when it is not part of the adopted chain,
    // because it may be the committed state this recovery could not confirm.
    if (impl.writable) {
      const auto names = internal::list_directory(impl.generations_path);
      if (names.has_value()) {
        for (const std::string& name : names.value()) {
          if (!generation_of_file_name(name).has_value()) {
            (void)impl.quarantine_generation(name);
            continue;
          }
          if (!load_generation_file(join(impl.generations_path, name)).has_value()) {
            (void)impl.quarantine_generation(name);
          }
        }
      }
    }
  }

  if (impl.writable) {
    CT_TRY(next_epoch, impl.manifest.epoch.next());
    CT_TRY(next_incarnation, impl.manifest.incarnation.next());
    impl.manifest.epoch = next_epoch;
    impl.manifest.incarnation = next_incarnation;
    // While repairing, the current manifest is the damaged one: it must never be
    // promoted into the previous-publication slot.
    CT_TRYV(impl.write_manifest(impl.manifest, !impl.recovering_manifest, options.durable_flush));
    // The repaired manifest is committed, so the adopted state is authoritative
    // again: the store may publish, while still reporting Recovered rather than
    // Reopened so that "adopted" is never mistaken for "fresh".
    impl.recovering_manifest = false;
    impl.remove_staging_residue(nullptr);
    // Best-effort removal of staging leftovers from an interrupted publication.
    // They are never read as authoritative content: a publication always writes
    // a fresh staging file and renames it into place.
    (void)internal::remove_file(join(impl.root, "manifest.tmp"));
    (void)internal::remove_file(join(impl.root, "manifest.prev.tmp"));
    (void)internal::remove_file(join(impl.root, "floor.tmp"));
  }
  impl.open = true;
  return store;
}

Result<void> Store::close() {
  if (impl_ == nullptr || !impl_->open) {
    return ok();
  }
  impl_->open = false;
  return impl_->lock.release();
}

bool Store::is_open() const noexcept { return impl_ != nullptr && impl_->open; }

StoreId Store::store_id() const noexcept {
  static const StoreId kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->manifest.store_id;
}

const ExternalRef& Store::facility() const noexcept {
  static const ExternalRef kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->manifest.facility;
}

WriterEpoch Store::epoch() const noexcept { return impl_ == nullptr ? WriterEpoch{} : impl_->manifest.epoch; }

WriterIncarnation Store::incarnation() const noexcept {
  return impl_ == nullptr ? WriterIncarnation{} : impl_->manifest.incarnation;
}

StoreMode Store::mode() const noexcept { return impl_ == nullptr ? StoreMode::ReadOnly : impl_->options.mode; }

StoreOpenState Store::open_state() const noexcept {
  return impl_ == nullptr ? StoreOpenState::Reopened : impl_->open_state;
}

const std::string& Store::root() const noexcept {
  static const std::string kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->root;
}

// ---------------------------------------------------------------------------
// Read side
// ---------------------------------------------------------------------------

Result<StoreInfo> Store::info() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  CT_TRYV(impl_->require_open());
  StoreInfo info;
  info.store_id = impl_->manifest.store_id;
  info.facility = impl_->manifest.facility;
  info.head = impl_->manifest.head;
  info.head_digest = impl_->manifest.head_digest;
  info.floor = impl_->manifest.floor;
  info.commit_sequence = impl_->manifest.commit_sequence;
  info.epoch = impl_->manifest.epoch;
  info.incarnation = impl_->manifest.incarnation;
  info.mode = impl_->options.mode;
  info.open_state = impl_->open_state;
  const auto generations = impl_->list_generations();
  info.retained_generations = generations.has_value() ? generations.value().size() : 0;
  const auto records = internal::list_directory(impl_->idempotency_path);
  info.idempotency_records = records.has_value() ? records.value().size() : 0;
  info.writable = impl_->writable;
  info.publication_allowed = impl_->writable && !impl_->recovering_manifest;
  info.root = impl_->root;
  info.boundary = std::string(systems_boundary());
  return info;
}

Result<Topology> Store::head() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  CT_TRYV(impl_->require_open());
  if (!impl_->manifest.head.published()) {
    return Error(ErrorCode::HeadMissing, "the store has no published generation yet");
  }
  // The head is never served from the manifest alone: the generation file is
  // re-read, integrity-checked, decoded and re-validated, and the recomputed
  // digest and generation number are required to agree with the manifest.
  CT_TRY(path, impl_->find_generation_path(impl_->manifest.head));
  CT_TRY(topology, load_generation_file(path));
  if (topology.digest() != impl_->manifest.head_digest) {
    return Error(ErrorCode::HeadCorrupt, "the head generation digest does not match the manifest");
  }
  if (topology.generation() != impl_->manifest.head) {
    return Error(ErrorCode::HeadCorrupt, "the head generation number does not match the manifest");
  }
  return topology;
}

Result<Topology> Store::load(TopologyGeneration generation) const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  CT_TRYV(impl_->require_open());
  if (!generation.published()) {
    return Error(ErrorCode::InvalidArgument, "generation 0 is not a published generation");
  }
  if (generation < impl_->manifest.floor) {
    // The generation floor is the lower bound of the retention window, so a
    // generation below it has been retired rather than merely being absent.
    return Error(ErrorCode::GenerationNotRetained, "the generation is below the durable generation floor")
        .with_subject(std::to_string(generation.value()));
  }
  if (generation > impl_->manifest.head) {
    // A file for a newer generation may exist as uncommitted residue. It was
    // never committed and is therefore not a generation of this store.
    return Error(ErrorCode::GenerationNotRetained,
                 "the generation is newer than the committed head and was never published")
        .with_subject(std::to_string(generation.value()));
  }
  CT_TRY(path, impl_->find_generation_path(generation));
  CT_TRY(topology, load_generation_file(path));
  if (topology.generation() != generation) {
    return Error(ErrorCode::HeadCorrupt, "the retained generation number does not match the requested generation")
        .with_subject(std::to_string(generation.value()));
  }
  return topology;
}

Result<std::vector<HistoryEntry>> Store::history() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  CT_TRYV(impl_->require_open());
  CT_TRY(list, impl_->list_generations());
  std::vector<HistoryEntry> entries;
  // The entries are visited newest first. The chain link of an older entry is
  // established by the *newer* generation's parent binding pointing at it, so
  // the newer binding is carried forward as the walk descends.
  TopologyGeneration newer_parent{};
  Digest newer_parent_digest{};
  bool has_newer = false;
  for (auto iterator = list.rbegin(); iterator != list.rend(); ++iterator) {
    HistoryEntry entry;
    entry.generation = iterator->first;
    entry.commit_sequence = impl_->recorded_commit_sequence(iterator->first);
    const std::string path = impl_->generation_path(iterator->first, iterator->second);
    if (const auto info = internal::inspect_path(path); info.has_value() && info.value().exists) {
      entry.file_bytes = info.value().size;
    }
    const auto topology = load_generation_file(path);
    if (topology.has_value()) {
      entry.digest = topology.value().digest();
      entry.parent_generation = topology.value().parent_generation();
      entry.parent_digest = topology.value().parent_digest();
      if (has_newer) {
        entry.chain_verified = newer_parent == entry.generation && newer_parent_digest == entry.digest;
      } else {
        // The newest retained generation has no newer neighbour inside the
        // retention window; there is no link to break.
        entry.chain_verified = true;
      }
      newer_parent = topology.value().parent_generation();
      newer_parent_digest = topology.value().parent_digest();
      has_newer = true;
    }
    entry.is_head = entry.generation == impl_->manifest.head;
    entries.push_back(std::move(entry));
  }
  if (entries.empty() && impl_->manifest.head.published()) {
    return Error(ErrorCode::HeadMissing, "the head generation file is missing");
  }
  return entries;
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

Result<PublicationReceipt> Store::publish(const PublicationRequest& request) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  Impl& impl = *impl_;
  CT_TRYV(impl.require_writable());
  if (impl.recovering_manifest) {
    // Guard for the invariant that a handle never serves adopted state as
    // mutable state: open() either commits the repaired head manifest or fails.
    return Error(ErrorCode::RecoveryRequired,
                 "the store adopted a retained publication and must be recovered before it can be mutated");
  }
  if (request.mutation.empty()) {
    return Error(ErrorCode::InvalidArgument, "a publication request requires a mutation identity");
  }
  if (request.attempt.value() == 0) {
    return Error(ErrorCode::InvalidArgument, "a publication request requires a 1-based attempt ordinal");
  }

  const Digest content_digest = mutation_content_digest(request.draft);

  // Idempotency is consulted before any authority check: a retry of an accepted
  // attempt returns the recorded outcome even when its authority and its base
  // generation have since moved on, and it performs no re-actuation.
  const std::string record_path = impl.idempotency_path_of(request.mutation, request.attempt);
  CT_TRY(record_present, internal::path_exists(record_path));
  if (record_present) {
    CT_TRY(bytes, internal::read_file(record_path, limits::kMaxIdempotencyRecordBytes));
    CT_TRY(record, parse_idempotency(bytes));
    if (record.content_digest != content_digest) {
      return Error(ErrorCode::IdempotencyConflict, "the mutation identity was already used for different content")
          .with_subject(request.mutation.str());
    }
    if (record.state == IdempotencyState::Accepted) {
      return impl.replay_receipt(record, request);
    }
    // A pending record means the previous attempt was interrupted before it
    // recorded its acceptance. It is a replay only when the store really did
    // commit that generation.
    if (record.generation == impl.manifest.head && record.generation_digest == impl.manifest.head_digest) {
      record.state = IdempotencyState::Accepted;
      CT_TRY(replay_bytes, serialize_idempotency(record));
      CT_TRYV(impl.write_record_atomically(record_path, replay_bytes, impl.options.durable_flush));
      return impl.replay_receipt(record, request);
    }
  }

  // Authority fencing: every check happens after the replay check and before
  // anything is staged, and a refusal leaves no trace in the store.
  if (request.authority.epoch != impl.manifest.epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch, "the request was planned under a superseded writer epoch")
        .with_subject(std::to_string(request.authority.epoch.value()))
        .with_detail("current epoch " + std::to_string(impl.manifest.epoch.value()));
  }
  if (request.authority.incarnation != impl.manifest.incarnation) {
    return Error(ErrorCode::StaleWriterIncarnation, "the request was planned under a superseded writer incarnation")
        .with_subject(std::to_string(request.authority.incarnation.value()))
        .with_detail("current incarnation " + std::to_string(impl.manifest.incarnation.value()));
  }
  if (request.authority.expected_base != impl.manifest.head) {
    return Error(ErrorCode::StaleBaseGeneration, "the request was planned against a different base generation")
        .with_subject(std::to_string(request.authority.expected_base.value()))
        .with_detail("current head " + std::to_string(impl.manifest.head.value()));
  }
  if (!request.draft.facility.same_binding_as(impl.manifest.facility)) {
    return Error(ErrorCode::StoreMismatch, "the draft describes a different facility than the store is bound to")
        .with_subject(request.draft.facility.identity);
  }

  // The generation number and the commit sequence are reserved before anything
  // is written: an exhausted counter refuses the publication without leaving a
  // staged generation or a pending attempt behind.
  CT_TRY(next_generation, impl.manifest.head.published()
                              ? impl.manifest.head.next()
                              : Result<TopologyGeneration>(TopologyGeneration(TopologyGeneration::kFirstPublished)));
  CT_TRY(next_commit_sequence, impl.manifest.commit_sequence.next());
  CT_TRY(topology, Topology::create(next_generation, impl.manifest.head, impl.manifest.head_digest, request.draft));
  CT_TRY(payload, topology.canonical_bytes());
  CT_TRY(frame, encode_generation_file(payload));

  const std::string generation_path = impl.generation_path(next_generation, topology.digest());
  const std::string staging = impl.staging_path_of(next_generation);
  {
    // A file that claims the generation number being published but was never
    // committed (for example the residue of an interrupted publication) is
    // quarantined first, so that after this publication the number resolves to
    // exactly one generation and no residue can shadow it.
    const auto names = internal::list_directory(impl.generations_path);
    if (names.has_value()) {
      for (const std::string& name : names.value()) {
        const auto number = generation_of_file_name(name);
        if (number.has_value() && number.value() == next_generation) {
          (void)impl.quarantine_generation(name);
        }
      }
    }
  }
  CT_TRY(generation_present, internal::path_exists(generation_path));
  if (generation_present) {
    return Error(ErrorCode::GenerationAlreadyExists, "the generation is already present")
        .with_subject(std::to_string(next_generation.value()));
  }

  CT_TRYV(internal::write_file(staging, frame, impl.options.durable_flush));
  fault_here(impl.options.enable_fault_injection, kCrashAfterStaging, kCrashAfterStagingPublication);

  // The staged content is read back and re-verified before it can become a
  // generation: a torn or substituted staging file is refused here.
  CT_TRY(readback, internal::read_file(staging, limits::kMaxGenerationFileBytes));
  if (readback != frame) {
    CT_TRYV(internal::remove_file(staging));
    return Error(ErrorCode::PublicationIncomplete, "staged content does not match what was written");
  }
  CT_TRY(readback_topology, Topology::decode(readback));
  if (readback_topology.digest() != topology.digest()) {
    CT_TRYV(internal::remove_file(staging));
    return Error(ErrorCode::PublicationIncomplete, "staged content decodes to a different generation");
  }

  fault_here(impl.options.enable_fault_injection, kCrashBeforeGenerationRename,
             kCrashBeforeGenerationRenamePublication);
  CT_TRYV(internal::atomic_replace(generation_path, staging, impl.options.durable_flush));
  fault_here(impl.options.enable_fault_injection, kCrashAfterGenerationRename,
             kCrashAfterGenerationRenamePublication);

  const PublicationDurability durability =
      impl.options.durable_flush ? PublicationDurability::Durable : PublicationDurability::NotDurable;

  // A pending record is written before the commit so that an interrupted
  // publication is unambiguously resolvable on retry.
  IdempotencyRecord record;
  record.mutation = request.mutation;
  record.attempt = request.attempt;
  record.state = IdempotencyState::Pending;
  record.content_digest = content_digest;
  record.generation = next_generation;
  record.generation_digest = topology.digest();
  record.parent_generation = impl.manifest.head;
  record.parent_digest = impl.manifest.head_digest;
  record.commit_sequence = next_commit_sequence;
  record.durability = durability;
  CT_TRY(pending_bytes, serialize_idempotency(record));
  CT_TRYV(impl.write_record_atomically(record_path, pending_bytes, impl.options.durable_flush));
  fault_here(impl.options.enable_fault_injection, kCrashAfterIdempotencyPending,
             kCrashAfterIdempotencyPendingPublication);

  // The retention window this publication establishes, together with the durable
  // floor that bounds it. Both are known before the commit point, so the
  // committed manifest already records the window it leaves behind.
  const std::size_t retention = impl.options.retained_generations;
  const std::uint64_t oldest_kept =
      next_generation.value() > retention ? next_generation.value() - retention + 1 : 1;
  CT_TRY(floor_candidate, TopologyGeneration::parse(std::max(impl.manifest.floor.value(), oldest_kept)));

  // ---- the single commit point: the atomic replacement of the head manifest ----
  Manifest committed = impl.manifest;
  committed.head = next_generation;
  committed.head_digest = topology.digest();
  committed.parent_generation = impl.manifest.head;
  committed.parent_digest = impl.manifest.head_digest;
  committed.commit_sequence = next_commit_sequence;
  committed.retained = impl.retained_after(floor_candidate, next_generation, topology.digest(), next_commit_sequence);
  CT_TRYV(impl.write_manifest(committed, true, impl.options.durable_flush));
  impl.manifest = committed;

  fault_here(impl.options.enable_fault_injection, kCrashBeforeFloor, kCrashBeforeFloorPublication);
  CT_TRYV(impl.write_floor(floor_candidate, impl.options.durable_flush));

  // Retire content outside the retention window; the head and its whole recorded
  // window are always kept.
  CT_TRY(present, impl.list_generations());
  for (const auto& entry : present) {
    if (entry.first >= floor_candidate) {
      continue;
    }
    CT_TRYV(internal::remove_file(impl.generation_path(entry.first, entry.second)));
  }
  impl.remove_staging_residue(nullptr);

  record.state = IdempotencyState::Accepted;
  CT_TRY(accepted_bytes, serialize_idempotency(record));
  CT_TRYV(impl.write_record_atomically(record_path, accepted_bytes, impl.options.durable_flush));
  fault_here(impl.options.enable_fault_injection, kCrashAfterIdempotencyAccept,
             kCrashAfterIdempotencyAcceptPublication);

  // Bound the retained accepted-attempt records, evicting the oldest publication
  // first; the file name is a deterministic tie-break so the eviction order does
  // not depend on directory enumeration.
  CT_TRY(records, internal::list_directory(impl.idempotency_path));
  if (records.size() > impl.options.idempotency_retention) {
    std::vector<std::pair<TopologyGeneration, std::string>> ordered;
    ordered.reserve(records.size());
    for (const std::string& name : records) {
      const auto bytes = internal::read_file(join(impl.idempotency_path, name), limits::kMaxIdempotencyRecordBytes);
      if (!bytes.has_value()) {
        continue;
      }
      const auto parsed = parse_idempotency(bytes.value());
      if (!parsed.has_value()) {
        continue;
      }
      ordered.emplace_back(parsed.value().generation, name);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first < rhs.first;
      }
      return lhs.second < rhs.second;
    });
    while (ordered.size() > impl.options.idempotency_retention) {
      CT_TRYV(internal::remove_file(join(impl.idempotency_path, ordered.front().second)));
      ordered.erase(ordered.begin());
    }
  }
  const auto retained_records = internal::list_directory(impl.idempotency_path);
  impl.manifest.idempotency_records = retained_records.has_value() ? retained_records.value().size() : 0;
  fault_here(impl.options.enable_fault_injection, kCrashAfterCommit, kCrashAfterCommitPublication);

  PublicationReceipt receipt;
  receipt.generation = next_generation;
  receipt.parent_generation = committed.parent_generation;
  receipt.digest = topology.digest();
  receipt.mutation = request.mutation;
  receipt.attempt = request.attempt;
  receipt.commit_sequence = next_commit_sequence;
  receipt.replayed = false;
  receipt.head_after = impl.manifest.head;
  receipt.head_digest_after = impl.manifest.head_digest;
  receipt.durability = durability;
  return receipt;
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

Result<VerifyReport> Store::verify(const VerifyOptions& options) const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  const Impl& impl = *impl_;
  CT_TRYV(impl.require_open());
  VerifyReport report;
  report.store_id = impl.manifest.store_id;
  report.head = impl.manifest.head;
  report.head_digest = impl.manifest.head_digest;
  report.recovered_state = impl.open_state == StoreOpenState::Recovered;

  // The manifest on disk must still be the manifest this handle is serving: a
  // manifest that no longer parses, or that names a different head or a
  // different commit sequence, is reported instead of being adopted silently.
  bool manifest_parsed = false;
  TopologyGeneration recorded_floor{};
  const auto manifest_bytes = internal::read_file(impl.manifest_path(), limits::kMaxManifestBytes);
  if (!manifest_bytes.has_value()) {
    report.findings.push_back(
        {VerifySeverity::Defect, "manifest_unreadable", "manifest", manifest_bytes.error().to_string()});
  } else {
    const auto parsed = parse_manifest(manifest_bytes.value());
    if (!parsed.has_value()) {
      report.findings.push_back({VerifySeverity::Defect, "manifest_invalid", "manifest", parsed.error().to_string()});
    } else if (parsed.value().head != impl.manifest.head ||
               parsed.value().head_digest != impl.manifest.head_digest ||
               parsed.value().commit_sequence != impl.manifest.commit_sequence) {
      report.findings.push_back({VerifySeverity::Defect, "manifest_diverged", "manifest",
                                 "the committed manifest no longer matches the open handle"});
    } else {
      manifest_parsed = true;
      recorded_floor = parsed.value().floor;
      report.manifest_verified = true;
    }
  }

  const auto floor = impl.read_floor();
  if (!floor.has_value()) {
    report.findings.push_back({VerifySeverity::Defect, "floor_invalid", "floor", floor.error().to_string()});
  } else if (impl.manifest.head.published() && impl.manifest.head < floor.value()) {
    report.findings.push_back({VerifySeverity::Defect, "floor_above_head", "floor",
                               "the durable floor is above the committed head"});
  } else if (manifest_parsed && recorded_floor > floor.value()) {
    report.findings.push_back({VerifySeverity::Defect, "floor_rolled_back", "floor",
                               "the durable floor is below the floor the committed manifest recorded"});
  } else {
    report.floor_verified = true;
  }

  CT_TRY(list, impl.list_generations());
  report.generations_present = list.size();
  if (impl.manifest.head.published()) {
    const auto path = impl.find_generation_path(impl.manifest.head);
    if (!path.has_value()) {
      report.findings.push_back({VerifySeverity::Defect, "head_file_missing", "head", path.error().to_string()});
    } else {
      const auto topology = load_generation_file(path.value());
      if (!topology.has_value()) {
        report.findings.push_back({VerifySeverity::Defect, "head_unverified", "head", topology.error().to_string()});
      } else {
        report.head_verified = topology.value().digest() == impl.manifest.head_digest &&
                               topology.value().generation() == impl.manifest.head;
        if (!report.head_verified) {
          report.findings.push_back({VerifySeverity::Defect, "head_digest_mismatch", "head",
                                     "the head payload digest or generation number differs from the manifest"});
        }
      }
    }
  } else {
    report.head_verified = true;
  }

  // The manifest records the committed retention window: every generation the
  // store committed and still retains, with its digest and commit sequence. A
  // recorded generation whose file is gone is lost committed state while it is
  // still at or above the durable floor; below the floor it is a publication
  // that has since been retired, for example after the retained previous
  // publication was adopted.
  std::unordered_set<std::uint64_t> present_generations;
  present_generations.reserve(list.size());
  for (const auto& entry : list) {
    present_generations.insert(entry.first.value());
  }
  if (impl.manifest.head.published() &&
      (impl.manifest.retained.empty() || impl.manifest.retained.back().generation != impl.manifest.head)) {
    report.findings.push_back({VerifySeverity::Defect, "head_not_recorded", "head",
                               "the head manifest does not record the committed head as a retained generation"});
  }
  for (const RetainedGeneration& recorded : impl.manifest.retained) {
    if (present_generations.count(recorded.generation.value()) != 0) {
      continue;
    }
    const std::string subject = std::to_string(recorded.generation.value());
    if (recorded.generation >= impl.manifest.floor) {
      report.findings.push_back({VerifySeverity::Defect, "retained_generation_missing", subject,
                                 "the manifest records a retained generation whose file is absent"});
    } else {
      report.findings.push_back({VerifySeverity::Info, "retained_generation_retired", subject,
                                 "the manifest records a generation that has been retired below the durable floor"});
    }
  }

  if (options.deep) {
    // Only the retained window is checked. The oldest retained generation's
    // parent has normally been retired; that is a retention boundary, not a
    // broken chain, and it is reported as information.
    //
    // The committed chain is walked from the head down through parent links.
    // Files that are not part of that chain are residue: newer ones were never
    // committed, older ones are unreferenced, and neither is ever adopted.
    std::unordered_map<std::uint64_t, std::string> path_by_generation;
    for (const auto& entry : list) {
      path_by_generation.emplace(entry.first.value(), impl.generation_path(entry.first, entry.second));
      if (impl.manifest.head.published() && entry.first > impl.manifest.head) {
        report.findings.push_back({VerifySeverity::Warning, "orphan_generation", std::to_string(entry.first.value()),
                                   "a generation newer than the head exists and was never committed"});
        ++report.orphan_generations_found;
      }
    }

    bool chain_ok = true;
    std::unordered_set<std::uint64_t> chain_members;
    TopologyGeneration cursor = impl.manifest.head;
    Digest expected_digest = impl.manifest.head_digest;
    while (cursor.published()) {
      const auto located = path_by_generation.find(cursor.value());
      if (located == path_by_generation.end()) {
        chain_ok = false;
        report.findings.push_back({VerifySeverity::Defect, "committed_generation_missing",
                                   std::to_string(cursor.value()),
                                   "a committed generation of the chain is not present in the store"});
        break;
      }
      const auto topology = load_generation_file(located->second);
      if (!topology.has_value()) {
        chain_ok = false;
        report.findings.push_back({VerifySeverity::Defect, "generation_unverified",
                                   std::to_string(cursor.value()), topology.error().to_string()});
        break;
      }
      if (topology.value().digest() != expected_digest) {
        chain_ok = false;
        report.findings.push_back({VerifySeverity::Defect, "parent_chain_broken", std::to_string(cursor.value()),
                                   "the generation digest does not match the digest recorded by its child"});
        break;
      }
      ++report.generations_verified;
      chain_members.insert(cursor.value());
      if (options.verify_canonical_fixed_point) {
        const auto recomputed = topology.value().recompute_digest();
        if (!recomputed.has_value() || recomputed.value() != topology.value().digest()) {
          chain_ok = false;
          report.findings.push_back({VerifySeverity::Defect, "canonical_not_fixed_point",
                                     std::to_string(cursor.value()),
                                     "re-encoding the decoded generation changed its digest"});
          break;
        }
        report.canonical_fixed_point_verified = true;
      }
      const TopologyGeneration parent = topology.value().parent_generation();
      if (!parent.published()) {
        break;
      }
      if (path_by_generation.find(parent.value()) == path_by_generation.end()) {
        report.findings.push_back({VerifySeverity::Info, "chain_starts_at_retention_boundary",
                                   std::to_string(cursor.value()),
                                   "the parent of the oldest retained generation has been retired"});
        break;
      }
      expected_digest = topology.value().parent_digest();
      cursor = parent;
    }
    for (const auto& entry : list) {
      if (chain_members.count(entry.first.value()) != 0) {
        continue;
      }
      if (impl.manifest.head.published() && entry.first > impl.manifest.head) {
        continue;  // already reported as an uncommitted orphan
      }
      report.findings.push_back({VerifySeverity::Defect, "unreferenced_generation",
                                 std::to_string(entry.first.value()),
                                 "a retained generation is not part of the committed chain"});
      ++report.unreferenced_generations_found;
      chain_ok = false;
    }
    report.chain_verified = chain_ok;
  }

  const auto quarantine = internal::list_directory(impl.quarantine_path);
  if (quarantine.has_value() && !quarantine.value().empty()) {
    report.findings.push_back({VerifySeverity::Warning, "quarantined_content", "quarantine",
                               "unverifiable generation content was quarantined during recovery and is never "
                               "adopted"});
    report.quarantined_found = quarantine.value().size();
  }

  if (options.verify_idempotency) {
    const auto records = internal::list_directory(impl.idempotency_path);
    if (!records.has_value()) {
      report.findings.push_back(
          {VerifySeverity::Defect, "idempotency_directory", "idem", records.error().to_string()});
    } else {
      for (const std::string& name : records.value()) {
        const auto bytes = internal::read_file(join(impl.idempotency_path, name), limits::kMaxIdempotencyRecordBytes);
        if (!bytes.has_value()) {
          report.findings.push_back(
              {VerifySeverity::Defect, "idempotency_unreadable", name, bytes.error().to_string()});
          continue;
        }
        const auto parsed = parse_idempotency(bytes.value());
        if (!parsed.has_value()) {
          report.findings.push_back(
              {VerifySeverity::Defect, "idempotency_invalid", name, parsed.error().to_string()});
          continue;
        }
        if (parsed.value().generation > impl.manifest.head) {
          report.findings.push_back({VerifySeverity::Warning, "idempotency_ahead_of_head", name,
                                     "an accepted-attempt record names a generation newer than the head"});
        }
      }
    }
  }

  const auto staging = internal::list_directory(impl.staging_path);
  if (staging.has_value()) {
    report.staged_residue_found = staging.value().size();
    if (report.staged_residue_found > 0) {
      report.findings.push_back({VerifySeverity::Warning, "staging_residue", "staging",
                                 "uncommitted staged content is present; it is never authoritative and is "
                                 "removed by the next publication or recovery"});
    }
  }

  report.publication_allowed = impl.writable && !impl.recovering_manifest && report.head_verified &&
                               report.manifest_verified;
  if (report.recovered_state) {
    report.findings.push_back({VerifySeverity::Warning, "recovered_state", "head",
                               "this store adopted a retained publication; recovered state is not fresh state"});
  }
  return report;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Result<RecoveryReport> Store::recover(const RecoveryOptions& options) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  Impl& impl = *impl_;
  CT_TRYV(impl.require_open());
  RecoveryReport report;
  report.head_before = impl.manifest.head;

  if (!impl.manifest.head.published()) {
    // Nothing has been published, so there is no committed head to verify and
    // nothing to adopt; the store is already in its whole verified state.
    report.outcome = RecoveryOutcome::NoAction;
    report.head_after = impl.manifest.head;
    report.head_digest_after = impl.manifest.head_digest;
    report.explanation = "the store has published no generation; nothing was changed";
    impl.remove_staging_residue(&report.residue_removed);
    return report;
  }
  {
    const auto head_path = impl.find_generation_path(impl.manifest.head);
    if (head_path.has_value()) {
      const auto topology = load_generation_file(head_path.value());
      if (topology.has_value() && topology.value().digest() == impl.manifest.head_digest) {
        report.outcome = RecoveryOutcome::NoAction;
        report.head_after = impl.manifest.head;
        report.head_digest_after = impl.manifest.head_digest;
        report.explanation = "the committed head verified; nothing was changed";
        // Staged content is never authoritative, so retiring it is not a change
        // to committed state.
        impl.remove_staging_residue(&report.residue_removed);
        return report;
      }
    }
  }

  if (!options.adopt_previous) {
    return Error(ErrorCode::RecoveryUnavailable,
                 "the committed head could not be verified and adoption of the previous publication is disabled");
  }
  if (!impl.writable) {
    return Error(ErrorCode::StoreReadOnly, "recovery rewrites the head manifest and requires a writable store");
  }

  const auto previous_bytes = internal::read_file(impl.manifest_previous_path(), limits::kMaxManifestBytes);
  if (!previous_bytes.has_value()) {
    return Error(ErrorCode::RecoveryUnavailable, "no retained previous publication exists")
        .with_detail(previous_bytes.error().to_string());
  }
  CT_TRY(previous, parse_manifest(previous_bytes.value()));
  if (previous.head.published() && previous.head < impl.manifest.floor) {
    report.floor_respected = false;
    return Error(ErrorCode::GenerationFloorViolation,
                 "the retained previous publication is below the durable generation floor and was refused")
        .with_subject(std::to_string(previous.head.value()));
  }
  if (previous.head.published()) {
    CT_TRY(previous_path, impl.find_generation_path(previous.head));
    const auto previous_topology = load_generation_file(previous_path);
    if (!previous_topology.has_value()) {
      return Error(ErrorCode::RecoveryUnavailable, "the retained previous publication could not be verified")
          .with_detail(previous_topology.error().to_string());
    }
    if (previous_topology.value().digest() != previous.head_digest) {
      return Error(ErrorCode::DigestMismatch,
                   "the retained previous publication digest does not match its manifest record");
    }
    report.steps.push_back("verified the retained previous publication generation " +
                           std::to_string(previous.head.value()));
  } else {
    report.steps.push_back("verified the retained previous publication: the store had published nothing");
  }

  // The adopted publication keeps the commit sequence it was committed with: a
  // recovery restores committed state and never invents a counter value. The
  // durable floor is carried forward, so adoption can never lower it.
  Manifest adopted = previous;
  CT_TRY(next_epoch, impl.manifest.epoch.next());
  adopted.epoch = next_epoch;
  adopted.floor = impl.manifest.floor;
  CT_TRYV(impl.write_manifest(adopted, false, impl.options.durable_flush));
  impl.manifest = adopted;
  impl.recovering_manifest = false;
  impl.open_state = StoreOpenState::Recovered;
  impl.remove_staging_residue(&report.residue_removed);
  report.steps.push_back("committed the adopted publication as the authoritative head");
  report.steps.push_back("advanced the mutation-authority epoch to fence the superseded writer");

  // Generation content that cannot be verified is moved aside, never deleted:
  // the evidence is preserved, and a later publication of the same generation
  // number cannot collide with residue that no version of the store could load.
  std::size_t quarantined = 0;
  const auto names = internal::list_directory(impl.generations_path);
  if (names.has_value()) {
    for (const std::string& name : names.value()) {
      if (!generation_of_file_name(name).has_value()) {
        if (impl.quarantine_generation(name)) {
          ++quarantined;
        }
        continue;
      }
      if (!load_generation_file(join(impl.generations_path, name)).has_value() && impl.quarantine_generation(name)) {
        ++quarantined;
      }
    }
  }
  if (quarantined > 0) {
    report.steps.push_back("quarantined " + std::to_string(quarantined) +
                           " unverifiable generation file(s) instead of deleting evidence");
  }
  if (options.deep_verify) {
    CT_TRY(verification, verify(VerifyOptions{}));
    if (!verification.head_verified) {
      return Error(ErrorCode::IntegrityFailure, "the adopted publication failed verification after adoption");
    }
    report.steps.push_back("re-verified every retained generation");
  }
  report.outcome = RecoveryOutcome::AdoptedPrevious;
  report.head_after = impl.manifest.head;
  report.head_digest_after = impl.manifest.head_digest;
  report.explanation =
      "the committed head was unusable; the retained previous publication was adopted, verified and fenced";
  return report;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::string_view to_token(StoreMode mode) noexcept {
  switch (mode) {
    case StoreMode::ReadOnly:
      return "read_only";
    case StoreMode::ReadWrite:
      return "read_write";
  }
  return "unknown";
}

std::string_view to_token(StoreOpenState state) noexcept {
  switch (state) {
    case StoreOpenState::Fresh:
      return "fresh";
    case StoreOpenState::Reopened:
      return "reopened";
    case StoreOpenState::Recovered:
      return "recovered";
  }
  return "unknown";
}

std::string_view to_token(PublicationDurability durability) noexcept {
  switch (durability) {
    case PublicationDurability::Durable:
      return "durable";
    case PublicationDurability::NotDurable:
      return "not_durable";
  }
  return "unknown";
}

std::string_view to_token(VerifySeverity severity) noexcept {
  switch (severity) {
    case VerifySeverity::Info:
      return "info";
    case VerifySeverity::Warning:
      return "warning";
    case VerifySeverity::Defect:
      return "defect";
  }
  return "unknown";
}

std::string_view to_token(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::NoAction:
      return "no_action";
    case RecoveryOutcome::AdoptedPrevious:
      return "adopted_previous";
  }
  return "unknown";
}

bool VerifyReport::ok() const noexcept {
  for (const VerifyFinding& finding : findings) {
    if (finding.severity == VerifySeverity::Defect) {
      return false;
    }
  }
  return head_verified && manifest_verified && floor_verified;
}

}  // namespace dccp::cooling_topology
