// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared fixtures for the persistence, recovery, idempotency and multiprocess
// suites. Header only: every test file includes exactly what it needs.
//
// Nothing here is library state. The helpers create and remove throwaway
// directories under the system temporary directory so that no test ever writes
// inside the repository, and they inspect the durable store from the outside -
// by reading the files the store actually wrote - so that a proof never depends
// on the library agreeing with itself.

#ifndef COOLING_TOPOLOGY_TESTS_STORE_SUPPORT_HPP
#define COOLING_TOPOLOGY_TESTS_STORE_SUPPORT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "child_process.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/mutation.hpp"
#include "dccp/cooling_topology/store.hpp"

namespace ct_test {

using dccp::cooling_topology::AttemptOrdinal;
using dccp::cooling_topology::ExternalGeneration;
using dccp::cooling_topology::ExternalRef;
using dccp::cooling_topology::ExternalRefKind;
using dccp::cooling_topology::MutationId;
using dccp::cooling_topology::Node;
using dccp::cooling_topology::PublicationReceipt;
using dccp::cooling_topology::PublicationRequest;
using dccp::cooling_topology::Store;
using dccp::cooling_topology::StoreId;
using dccp::cooling_topology::StoreMode;
using dccp::cooling_topology::StoreOptions;
using dccp::cooling_topology::TopologyDraft;
using dccp::cooling_topology::TopologyGeneration;

/// True when the result failed with exactly this code. Failure is asserted on
/// the documented code alone; message text is never inspected.
template <class T>
inline bool failed_with(const Result<T>& result, dccp::cooling_topology::ErrorCode code) {
  return !result.has_value() && result.error().code() == code;
}

/// Renders the outcome of a result, so a failure message names the code that
/// was actually reported.
template <class T>
inline std::string reported_code(const Result<T>& result) {
  return result.has_value() ? std::string("ok")
                            : std::string(dccp::cooling_topology::error_code_name(result.error().code()));
}

// ---------------------------------------------------------------------------
// Scratch space
// ---------------------------------------------------------------------------

/// Process-wide monotonic counter, so two scratches in one process never clash.
inline std::uint64_t next_scratch_index() {
  static std::uint64_t counter = 0;
  return ++counter;
}

inline unsigned long current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

/// Unique directory path under the system temporary directory. The path is not
/// created by this function.
inline std::string temp_root(const std::string& tag) {
  std::filesystem::path base = std::filesystem::temp_directory_path();
  base /= "cooling-topology-tests";
  base /= tag + "-" + std::to_string(current_process_id()) + "-" + std::to_string(next_scratch_index());
  return base.string();
}

/// Owns a scratch directory: creates it on construction and removes the whole
/// tree on destruction. Cleanup errors are ignored: scratch cleanup must never
/// fail a test.
class ScratchDir {
 public:
  explicit ScratchDir(const std::string& tag) : path_(temp_root(tag)) {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
    std::filesystem::create_directories(path_, code);
  }

  ~ScratchDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }

  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  const std::string& path() const noexcept { return path_; }
  std::string child(const std::string& name) const { return (std::filesystem::path(path_) / name).string(); }

 private:
  std::string path_;
};
// ---------------------------------------------------------------------------
// Byte-level file helpers
// ---------------------------------------------------------------------------

inline std::string join_path(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

inline std::string read_binary_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

inline bool write_binary_file(const std::string& path, std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.close();
  return !stream.fail();
}

inline std::string read_text_file(const std::string& path) { return read_binary_file(path); }

inline bool write_text_file(const std::string& path, std::string_view text) { return write_binary_file(path, text); }

inline bool file_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::is_regular_file(path, code);
}

inline bool directory_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::is_directory(path, code);
}

inline std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> names;
  std::error_code code;
  for (const auto& entry : std::filesystem::directory_iterator(path, code)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

inline std::size_t count_entries(const std::string& path) { return list_dir(path).size(); }

inline bool remove_tree(const std::string& path) {
  std::error_code code;
  std::filesystem::remove_all(path, code);
  return !code;
}

/// Overwrites the file with its first keep bytes. A file that is not longer than
/// that is left alone, so a caller cannot accidentally truncate upwards.
inline bool truncate_file(const std::string& path, std::size_t keep) {
  const std::string content = read_binary_file(path);
  if (content.size() <= keep) {
    return false;
  }
  return write_binary_file(path, std::string_view(content).substr(0, keep));
}

/// Flips one byte of the file, which is the smallest possible content damage.
inline bool flip_file_byte(const std::string& path, std::size_t offset) {
  std::string content = read_binary_file(path);
  if (offset >= content.size()) {
    return false;
  }
  content[offset] = static_cast<char>(static_cast<unsigned char>(content[offset]) ^ 0x5Au);
  return write_binary_file(path, content);
}

/// True when the path itself carries the reparse-point attribute, which is
/// exactly the property the store refuses. The attributes are read with
/// GetFileAttributesW rather than through the CRT so that a path outside the
/// process code page is still inspected exactly.
inline bool path_is_reparse_point(const std::string& path) {
#if defined(_WIN32)
  if (path.empty()) {
    return false;
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (size <= 0) {
    return false;
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), size);
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  std::error_code code;
  return std::filesystem::is_symlink(std::filesystem::symlink_status(path, code));
#endif
}

/// Creates a directory junction - a real reparse point - without elevation, by
/// running the platform tool as an independent process with captured output and
/// no console. The result is verified by inspecting the created path rather than
/// by trusting the tool exit code.
inline bool create_directory_junction(const std::string& link, const std::string& target) {
#if defined(_WIN32)
  auto spawned = ChildProcess::spawn({"C:\\Windows\\System32\\cmd.exe", "/c", "mklink", "/J", link, target});
  if (!spawned.has_value()) {
    CT_CHECK_MSG(false, "junction helper could not start the platform tool: " + spawned.error().to_string());
    return false;
  }
  ChildProcess child = std::move(spawned.value());
  const ChildResult result = child.collect();
  if (!result.exited || result.exit_code != 0) {
    CT_CHECK_MSG(false, "junction helper: mklink exited " + std::to_string(result.exit_code) +
                            " output=" + result.output);
    return false;
  }
  return path_is_reparse_point(link);
#else
  (void)link;
  (void)target;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Whole-tree comparison
// ---------------------------------------------------------------------------

/// Every path below the root, relative and slash-separated, mapped to its
/// content. A directory maps to the empty string under a key that ends in a
/// slash, so a directory and an empty file can never be confused. Reading the
/// tree twice and comparing the maps is a byte-level comparison of the whole
/// store.
inline std::map<std::string, std::string> tree_snapshot(const std::string& root) {
  std::map<std::string, std::string> snapshot;
  std::error_code code;
  const std::filesystem::path base(root);
  const auto end = std::filesystem::recursive_directory_iterator();
  for (auto iterator = std::filesystem::recursive_directory_iterator(base, code); !code && iterator != end;
       iterator.increment(code)) {
    const std::filesystem::path relative = iterator->path().lexically_relative(base);
    const std::string key = relative.generic_string();
    if (key.empty()) {
      continue;
    }
    if (iterator->is_directory()) {
      snapshot[key + "/"] = std::string();
      continue;
    }
    snapshot[key] = read_binary_file(iterator->path().string());
  }
  return snapshot;
}

/// A short description of the first difference between two snapshots, or an
/// empty string when they are byte-identical.
inline std::string tree_difference(const std::map<std::string, std::string>& before,
                                   const std::map<std::string, std::string>& after) {
  for (const auto& entry : before) {
    const auto other = after.find(entry.first);
    if (other == after.end()) {
      return "removed " + entry.first;
    }
    if (other->second != entry.second) {
      return "changed " + entry.first;
    }
  }
  for (const auto& entry : after) {
    if (before.find(entry.first) == before.end()) {
      return "added " + entry.first;
    }
  }
  return std::string();
}

inline bool trees_equal(const std::map<std::string, std::string>& before,
                        const std::map<std::string, std::string>& after) {
  return tree_difference(before, after).empty();
}
// ---------------------------------------------------------------------------
// Store directory inspection
// ---------------------------------------------------------------------------

/// Generation file names, ascending. The documented file-name layout, sixteen
/// decimal digits after the g prefix and sixteen hex digits after the dash,
/// makes listing order generation order.
inline std::vector<std::string> generation_file_names(const std::string& root) {
  std::vector<std::string> names = list_dir(join_path(root, "generations"));
  std::sort(names.begin(), names.end());
  return names;
}

/// The generation number a generation file name declares, or 0 when the name
/// does not match the documented layout.
inline std::uint64_t generation_number_of_name(const std::string& name) {
  if (name.size() < 18 || name[0] != 'g' || name[17] != '-') {
    return 0;
  }
  std::uint64_t value = 0;
  for (std::size_t index = 1; index < 17; ++index) {
    const char character = name[index];
    if (character < '0' || character > '9') {
      return 0;
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

/// Full path of the generation file that declares this generation number, or an
/// empty string when the store holds no such file.
inline std::string generation_file_path(const std::string& root, std::uint64_t generation) {
  for (const std::string& name : generation_file_names(root)) {
    if (generation_number_of_name(name) == generation) {
      return join_path(join_path(root, "generations"), name);
    }
  }
  return std::string();
}

inline std::size_t generation_file_count(const std::string& root, std::uint64_t generation) {
  std::size_t count = 0;
  for (const std::string& name : generation_file_names(root)) {
    if (generation_number_of_name(name) == generation) {
      ++count;
    }
  }
  return count;
}

inline std::vector<std::string> quarantine_file_names(const std::string& root) {
  return list_dir(join_path(root, "quarantine"));
}

inline std::string quarantined_bytes(const std::string& root, const std::string& name) {
  return read_binary_file(join_path(join_path(root, "quarantine"), name));
}

inline std::vector<std::string> staging_file_names(const std::string& root) {
  return list_dir(join_path(root, "staging"));
}

inline std::vector<std::string> idempotency_file_names(const std::string& root) {
  return list_dir(join_path(root, "idem"));
}

/// One key/value line of a checksummed durable record. The key is matched
/// exactly, so head can never be satisfied by head-digest.
inline std::string record_field(const std::string& path, const std::string& key) {
  const std::string text = read_binary_file(path);
  const std::string prefix = key + " ";
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind(prefix, 0) == 0) {
      return line.substr(prefix.size());
    }
  }
  return std::string();
}

inline std::uint64_t record_field_u64(const std::string& path, const std::string& key) {
  const std::string text = record_field(path, key);
  if (text.empty()) {
    return 0;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return 0;
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

inline std::string manifest_field(const std::string& root, const std::string& key) {
  return record_field(join_path(root, "manifest"), key);
}

inline std::uint64_t manifest_head(const std::string& root) {
  return record_field_u64(join_path(root, "manifest"), "head");
}

inline std::uint64_t floor_file_value(const std::string& root) {
  return record_field_u64(join_path(root, "floor"), "floor");
}

/// Rewrites one field of a checksummed durable record and recomputes its
/// checksum, which is what a well-formed forgery looks like: the record still
/// verifies, so the store has to refuse it on its content rather than on its
/// integrity. Returns false when the field is not present.
inline bool rewrite_record_field(const std::string& path, const std::string& key, const std::string& value) {
  const std::string text = read_binary_file(path);
  const std::size_t checksum = text.rfind("checksum ");
  if (checksum == std::string::npos) {
    return false;
  }
  const std::string body = text.substr(0, checksum);
  const std::string prefix = key + " ";
  std::size_t position = 0;
  std::size_t line_end = std::string::npos;
  bool found = false;
  while (position < body.size()) {
    const std::size_t newline = body.find('\n', position);
    const std::size_t end = newline == std::string::npos ? body.size() : newline;
    const std::string_view line(body.data() + position, end - position);
    if (line.rfind(prefix, 0) == 0) {
      line_end = end;
      found = true;
      break;
    }
    if (newline == std::string::npos) {
      break;
    }
    position = newline + 1;
  }
  if (!found) {
    return false;
  }
  const std::string rebuilt = body.substr(0, position) + prefix + value + body.substr(line_end);
  const std::string forged = rebuilt + "checksum " + dccp::cooling_topology::digest_bytes(rebuilt).to_hex() + "\n";
  return write_binary_file(path, forged);
}
// ---------------------------------------------------------------------------
// Store fixtures
// ---------------------------------------------------------------------------

inline StoreOptions store_options(const std::string& root, StoreMode mode = StoreMode::ReadWrite,
                                  bool create_if_missing = false) {
  StoreOptions options;
  options.root = root;
  options.mode = mode;
  options.create_if_missing = create_if_missing;
  return options;
}

/// The facility binding the reference drafts declare. A store is bound to
/// exactly this reference, and a draft is accepted only when it agrees with the
/// binding in kind, identity and external generation.
inline ExternalRef facility_reference(std::string_view identity = "dc-1", std::uint64_t generation = 7) {
  const auto created =
      ExternalRef::create(ExternalRefKind::Facility, std::string(identity), ExternalGeneration(generation));
  CT_REQUIRE(created.has_value());
  return *created;
}

inline Result<Store> create_store_with(const StoreOptions& options, const ExternalRef& facility,
                                       std::string_view store_id = "test-store-1") {
  const auto id = StoreId::parse(store_id);
  if (!id.has_value()) {
    return id.error();
  }
  return Store::create(options, id.value(), facility);
}

inline Result<Store> create_store(const std::string& root, std::string_view identity = "dc-1") {
  return create_store_with(store_options(root, StoreMode::ReadWrite, true), facility_reference(identity));
}

inline Result<Store> open_store(const std::string& root, StoreMode mode = StoreMode::ReadWrite) {
  return Store::open(store_options(root, mode, false));
}

/// The reference facility with a deterministic, structurally harmless content
/// change: variation 0 is the reference document itself and every other
/// variation carries a different display name, so the canonical digest differs
/// while the structure stays valid.
inline TopologyDraft reference_variation(std::size_t variation) {
  TopologyDraft draft = reference_facility();
  if (variation == 0) {
    return draft;
  }
  const std::string name = "variation-" + std::to_string(variation);
  for (Node& node : draft.nodes) {
    if (node.id == nid("manifold:m-a")) {
      node.display_name = name;
      break;
    }
  }
  return draft;
}

/// A request authorized by the store current authority and planned against its
/// current head.
inline Result<PublicationRequest> make_request(const Store& store, const TopologyDraft& draft,
                                               std::string_view mutation, unsigned attempt = 1) {
  const auto info = store.info();
  if (!info.has_value()) {
    return info.error();
  }
  PublicationRequest request;
  request.authority.epoch = info.value().epoch;
  request.authority.incarnation = info.value().incarnation;
  request.authority.expected_base = info.value().head;
  const auto id = MutationId::parse(mutation);
  if (!id.has_value()) {
    return id.error();
  }
  request.mutation = id.value();
  const auto ordinal = AttemptOrdinal::parse(attempt);
  if (!ordinal.has_value()) {
    return ordinal.error();
  }
  request.attempt = ordinal.value();
  request.draft = draft;
  return request;
}

inline Result<PublicationReceipt> publish_draft(Store& store, std::string_view mutation, const TopologyDraft& draft,
                                                unsigned attempt = 1) {
  const auto request = make_request(store, draft, mutation, attempt);
  if (!request.has_value()) {
    return request.error();
  }
  return store.publish(request.value());
}

inline Result<PublicationReceipt> publish_variation(Store& store, std::string_view mutation, std::size_t variation,
                                                    unsigned attempt = 1) {
  return publish_draft(store, mutation, reference_variation(variation), attempt);
}

}  // namespace ct_test

#endif  // COOLING_TOPOLOGY_TESTS_STORE_SUPPORT_HPP
