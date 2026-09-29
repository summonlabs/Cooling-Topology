// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Cross-process proof obligations for the durability and crash-consistency
// surface. Every claim below is made about REAL independent operating-system
// processes started through tests/child_process.hpp: writer exclusion through a
// real OS lock, release of that lock by process death, fencing of a superseded
// writer authority, publication killed at every documented crash point, and
// concurrent readers that never observe a partial generation.
//
// No wait in this file is bounded by a clock. A child is waited for until it
// exits, its exit code is asserted, and a child that would never finish is
// terminated non-interactively through TerminateProcess (exit code 97) so no
// Windows Error Reporting dialog can appear.
//
// Child roles are selected through the child's environment, because the frozen
// runner rejects any command-line argument it does not define; see
// child_process.hpp. The four child cases below are no-ops in an ordinary run
// and do the work only in the process that was started for them.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "child_process.hpp"
#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/cooling_topology/store.hpp"

namespace {

namespace ct = dccp::cooling_topology;

using namespace ct_test;

// ---------------------------------------------------------------------------
// Child roles
// ---------------------------------------------------------------------------

/// Case names the child runner selects with --filter=; each one names exactly
/// one CT_TEST below.
constexpr const char* kHoldLockCase = "multiprocess_child_hold_lock";
constexpr const char* kPublishSequenceCase = "multiprocess_child_publish_sequence";
constexpr const char* kPublishCrashCase = "multiprocess_child_publish_crash";
constexpr const char* kResolveStoreCase = "multiprocess_child_resolve_store";

/// Role names carried in the child environment.
constexpr const char* kHoldLockRole = "hold-lock";
constexpr const char* kPublishSequenceRole = "publish-sequence";
constexpr const char* kPublishCrashRole = "publish-crash";
constexpr const char* kResolveStoreRole = "resolve-store";

/// The mutation identity and attempt the crash child publishes; the parent
/// retries exactly this identity, which is what makes the interrupted attempt
/// resolvable.
constexpr const char* kCrashMutation = "child-crash-publish";
constexpr unsigned kCrashAttempt = 1;

/// Generations the sequence child publishes, and how many of them are paced by
/// a handshake with the parent. The handshake rounds make the interleaving
/// deterministic; the remaining rounds are published back to back while the
/// parent keeps opening the store, so the reader also overlaps publications
/// that are in progress.
constexpr unsigned kSequenceRounds = 6;
constexpr unsigned kHandshakeRounds = 2;

/// A bounded spin that never bounds a correctness-relevant wait by a clock: it
/// ends as soon as the file appears, and the iteration budget only exists so a
/// child whose parent has died still terminates on its own.
bool wait_for_release(const std::string& path) {
  for (int iteration = 0; iteration < 400000; ++iteration) {
    if (file_exists(path)) {
      return true;
    }
    std::this_thread::yield();
    if (iteration % 100 == 99) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return false;
}

/// Publishes a file atomically (staged write plus rename), so a parent that
/// polls for it can never read a half-written line.
bool publish_text_file(const std::string& path, const std::string& text) {
  const std::string staged = path + ".staged";
  if (!write_text_file(staged, text)) {
    return false;
  }
  std::error_code code;
  std::filesystem::rename(staged, path, code);
  return !code;
}

/// Decimal value written after a marker in a diagnostic line, or 0 when the
/// marker or the digits are absent.
std::uint64_t value_after(const std::string& text, const std::string& marker) {
  const std::size_t position = text.find(marker);
  if (position == std::string::npos) {
    return 0;
  }
  std::uint64_t value = 0;
  for (std::size_t index = position + marker.size(); index < text.size(); ++index) {
    const char character = text[index];
    if (character < '0' || character > '9') {
      break;
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

/// Decimal argument a role was given, or 0 when it is not a plain decimal.
std::uint64_t decimal_argument(const std::string& text) {
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

/// Renders the finding codes of a report, for a failure message.
std::string finding_codes(const ct::VerifyReport& report) {
  std::string text;
  for (const ct::VerifyFinding& finding : report.findings) {
    if (!text.empty()) {
      text += ", ";
    }
    text += finding.code;
  }
  return text.empty() ? std::string("no findings") : text;
}

/// True when the only defect findings are the documented staleness report of a
/// handle whose store moved on while it was open. A concurrent writer may
/// commit between an open and its verification, which makes that handle stale;
/// it does not damage the durable state, and every other defect is a failure.
bool only_stale_handle_defects(const ct::VerifyReport& report) {
  for (const ct::VerifyFinding& finding : report.findings) {
    if (finding.severity == ct::VerifySeverity::Defect && finding.code != "manifest_diverged") {
      return false;
    }
  }
  return true;
}

/// Waits for a file a child publishes atomically, without any timed wait: the
/// loop ends when the file appears with the expected content or when the child
/// is gone.
bool wait_for_file(const std::string& path, ChildProcess& child, const std::string& expected_prefix) {
  const auto ready = [&path, &expected_prefix]() {
    const std::string text = read_text_file(path);
    return !text.empty() && text.rfind(expected_prefix, 0) == 0;
  };
  for (int iteration = 0; iteration < 400000; ++iteration) {
    if (ready()) {
      return true;
    }
    if (child.has_exited()) {
      return ready();
    }
    std::this_thread::yield();
    if (iteration % 100 == 99) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return false;
}

/// Opens the store for writing, reports the authority it acquired in the ready
/// file and holds the writer lock until the release file appears.
void hold_lock_child() {
  const std::vector<std::string>& arguments = child_invocation().arguments;
  CT_REQUIRE(arguments.size() == 3);
  auto opened = open_store(arguments[0]);
  if (!opened.has_value()) {
    (void)publish_text_file(arguments[1], "ERROR " + std::string(ct::error_code_name(opened.error().code())) + "\n");
    CT_CHECK_MSG(false, "the lock holder could not open the store: " + reported_code(opened));
    return;
  }
  Store store = std::move(opened.value());
  const std::string ready = "READY epoch=" + std::to_string(store.epoch().value()) +
                            " incarnation=" + std::to_string(store.incarnation().value()) + "\n";
  CT_REQUIRE(publish_text_file(arguments[1], ready));
  CT_CHECK_MSG(wait_for_release(arguments[2]), "the parent never released the lock holder");
  CT_CHECK(store.close().has_value());
}

/// Publishes one generation per handshake round: the round file appears after
/// the publication was committed, and the next round waits for the parent's
/// acknowledgement. The reader therefore interleaves with a publication in
/// progress instead of only with a quiet store.
void publish_sequence_child() {
  const std::vector<std::string>& arguments = child_invocation().arguments;
  CT_REQUIRE(arguments.size() == 4);
  const std::string& root = arguments[0];
  const std::string& round_prefix = arguments[1];
  const std::string& ack_prefix = arguments[2];
  const std::string& done = arguments[3];
  auto opened = open_store(root);
  CT_REQUIRE(opened.has_value());
  Store store = std::move(opened.value());
  for (unsigned round = 1; round <= kSequenceRounds; ++round) {
    const auto receipt = publish_variation(store, "sequence-" + std::to_string(round), round);
    CT_CHECK_MSG(receipt.has_value(),
                 "the sequence child failed to publish: " + reported_code(receipt) + " " +
                     (receipt.has_value() ? std::string() : receipt.error().to_string()));
    if (!receipt.has_value()) {
      return;
    }
    CT_CHECK_EQ(receipt.value().generation.value(), static_cast<std::uint64_t>(round));
    if (round <= kHandshakeRounds) {
      CT_REQUIRE(publish_text_file(round_prefix + std::to_string(round) + ".ok", std::to_string(round) + "\n"));
      CT_CHECK_MSG(wait_for_release(ack_prefix + std::to_string(round) + ".ok"),
                   "the parent never acknowledged round " + std::to_string(round));
    }
  }
  CT_CHECK(store.close().has_value());
  CT_REQUIRE(publish_text_file(done, "done\n"));
}

/// Opens the store with fault injection enabled and publishes one generation.
/// The process is terminated by the store itself at the selected crash point,
/// through TerminateProcess, so it can never reach an interactive error path.
void publish_crash_child() {
  const std::vector<std::string>& arguments = child_invocation().arguments;
  CT_REQUIRE(arguments.size() == 4);
  const std::uint64_t attempt = decimal_argument(arguments[2]);
  const std::uint64_t variation = decimal_argument(arguments[3]);
  CT_REQUIRE(attempt > 0);
  StoreOptions options = store_options(arguments[0], ct::StoreMode::ReadWrite, false);
  options.enable_fault_injection = true;
  auto opened = ct::Store::open(options);
  CT_REQUIRE(opened.has_value());
  Store store = std::move(opened.value());
  const auto request =
      make_request(store, reference_variation(static_cast<std::size_t>(variation)), arguments[1],
                   static_cast<unsigned>(attempt));
  CT_REQUIRE(request.has_value());
  const auto receipt = store.publish(request.value());
  // Reaching this line means the selected crash point did not fire, which the
  // parent reports as a failure of the fault-injection contract.
  CT_CHECK_MSG(!receipt.has_value(),
               "the instrumented publication completed instead of terminating: generation " +
                   std::to_string(receipt.has_value() ? receipt.value().generation.value() : 0));
  if (!receipt.has_value()) {
    report_note("publish reported " + reported_code(receipt));
  }
}

/// Resolves a store that an interrupted publication left behind, in a process
/// that has never touched it: the whole state is read back from disk, the head
/// must decode and match its manifest digest, every retained generation must
/// decode and link to its parent, and there must be exactly one file per
/// retained generation - never a hybrid.
void resolve_store_child() {
  const std::vector<std::string>& arguments = child_invocation().arguments;
  CT_REQUIRE(arguments.size() == 3);
  const std::string& root = arguments[0];
  const std::uint64_t expected_head = decimal_argument(arguments[1]);
  const std::uint64_t expected_commit_sequence = decimal_argument(arguments[2]);
  CT_REQUIRE(expected_head > 0);

  // A read-only handle resolves the state without writing a single byte: no
  // writer epoch is reserved, nothing is repaired, nothing is adopted.
  auto opened = open_store(root, ct::StoreMode::ReadOnly);
  CT_CHECK_MSG(opened.has_value(), "the fresh process could not open the store: " + reported_code(opened));
  if (!opened.has_value()) {
    return;
  }
  Store store = std::move(opened.value());
  CT_CHECK_EQ(store.open_state(), ct::StoreOpenState::Reopened);
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), expected_head);
  CT_CHECK_EQ(info.value().commit_sequence.value(), expected_commit_sequence);
  const auto head = store.head();
  CT_CHECK_MSG(head.has_value(), "the head did not decode: " + reported_code(head));
  if (!head.has_value()) {
    return;
  }
  CT_CHECK_EQ(head.value().generation().value(), expected_head);
  CT_CHECK(info.value().head_digest == head.value().digest());

  const auto report = store.verify(ct::VerifyOptions{});
  CT_REQUIRE(report.has_value());
  for (const ct::VerifyFinding& finding : report.value().findings) {
    CT_CHECK_MSG(finding.severity != ct::VerifySeverity::Defect, "defect " + finding.code + ": " + finding.detail);
  }
  CT_CHECK_MSG(report.value().ok(), "the resolved store did not verify cleanly");
  CT_CHECK(report.value().head_verified);
  CT_CHECK(report.value().chain_verified);
  CT_CHECK(report.value().generations_present >= static_cast<std::size_t>(expected_head));

  for (std::uint64_t generation = 1; generation <= expected_head; ++generation) {
    const auto loaded = store.load(ct::TopologyGeneration(generation));
    CT_CHECK_MSG(loaded.has_value(), "generation " + std::to_string(generation) + " did not decode: " +
                                         reported_code(loaded));
    if (!loaded.has_value()) {
      continue;
    }
    CT_CHECK_EQ(generation_file_count(root, generation), std::size_t{1});
    if (generation > 1) {
      const auto parent = store.load(ct::TopologyGeneration(generation - 1));
      CT_REQUIRE(parent.has_value());
      CT_CHECK(loaded.value().parent_digest() == parent.value().digest());
    } else {
      CT_CHECK(!loaded.value().parent_generation().published());
    }
  }
  // No gap: every generation number up to the head exists exactly once, and no
  // generation beyond the head is reachable through the store.
  const auto beyond = store.load(ct::TopologyGeneration(expected_head + 1));
  CT_CHECK(!beyond.has_value());
  report_note("resolved head " + std::to_string(expected_head) + " from " +
              std::to_string(report.value().generations_present) + " generation file(s)");
}

CT_TEST(multiprocess_child_hold_lock) {
  if (!is_child_role(kHoldLockRole)) {
    return;
  }
  hold_lock_child();
}

CT_TEST(multiprocess_child_publish_sequence) {
  if (!is_child_role(kPublishSequenceRole)) {
    return;
  }
  publish_sequence_child();
}

CT_TEST(multiprocess_child_publish_crash) {
  if (!is_child_role(kPublishCrashRole)) {
    return;
  }
  publish_crash_child();
}

CT_TEST(multiprocess_child_resolve_store) {
  if (!is_child_role(kResolveStoreRole)) {
    return;
  }
  resolve_store_child();
}
// ---------------------------------------------------------------------------
// Parent side: writer exclusion, process death and authority fencing
// ---------------------------------------------------------------------------

CT_TEST(multiprocess_a_second_writer_is_excluded_by_a_real_os_lock) {
  ScratchDir scratch("mp-exclusion");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store setup = std::move(created.value());
  const auto published = publish_variation(setup, "exclusion-1", 0);
  CT_REQUIRE(published.has_value());
  CT_REQUIRE(setup.close().has_value());

  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  auto spawned = spawn_child(kHoldLockCase, kHoldLockRole, {scratch.path(), ready, release});
  CT_REQUIRE(spawned.has_value());
  ChildProcess child = std::move(spawned.value());
  // The lock is held by a different operating-system process, not by a second
  // handle in this one.
  CT_CHECK_EQ(child.process_id() != current_process_id(), true);
  CT_REQUIRE(wait_for_file(ready, child, "READY"));
  const std::string ready_text = read_text_file(ready);
  const std::uint64_t holder_epoch = value_after(ready_text, "epoch=");
  const std::uint64_t holder_incarnation = value_after(ready_text, "incarnation=");
  CT_CHECK(holder_epoch > 1);
  CT_CHECK(holder_incarnation > 1);

  // A second writer in this process is refused while the child holds the lock.
  const auto excluded = open_store(scratch.path());
  CT_CHECK_MSG(failed_with(excluded, ct::ErrorCode::StoreLocked), "reported " + reported_code(excluded));

  // A read-only handle may coexist with the writer, and it observes the very
  // authority that other process reserved durably.
  auto reader = open_store(scratch.path(), ct::StoreMode::ReadOnly);
  CT_REQUIRE(reader.has_value());
  CT_CHECK_EQ(reader.value().epoch().value(), holder_epoch);
  CT_CHECK_EQ(reader.value().incarnation().value(), holder_incarnation);
  const auto head = reader.value().head();
  CT_REQUIRE(head.has_value());
  CT_CHECK(head.value().digest() == published.value().digest);
  CT_REQUIRE(reader.value().close().has_value());

  // The holder is released in an orderly way and the lock becomes free.
  CT_REQUIRE(write_text_file(release, "release\n"));
  const ChildResult result = child.collect();
  CT_CHECK_MSG(result.exit_code == 0, "lock holder exit " + std::to_string(result.exit_code) + ": " + result.output);
  CT_CHECK(result.exited);
  CT_CHECK(!result.terminated);

  auto taken_over = open_store(scratch.path());
  CT_CHECK_MSG(taken_over.has_value(), "the lock was not released: " + reported_code(taken_over));
  if (taken_over.has_value()) {
    CT_CHECK(taken_over.value().epoch().value() > holder_epoch);
    const auto report = taken_over.value().verify(ct::VerifyOptions{});
    CT_REQUIRE(report.has_value());
    CT_CHECK(report.value().ok());
    CT_REQUIRE(taken_over.value().close().has_value());
  }
}

CT_TEST(multiprocess_process_death_releases_the_writer_lock) {
  ScratchDir scratch("mp-death");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store setup = std::move(created.value());
  const auto published = publish_variation(setup, "death-1", 0);
  CT_REQUIRE(published.has_value());
  const std::uint64_t epoch_before = setup.epoch().value();
  CT_REQUIRE(setup.close().has_value());

  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  auto spawned = spawn_child(kHoldLockCase, kHoldLockRole, {scratch.path(), ready, release});
  CT_REQUIRE(spawned.has_value());
  ChildProcess child = std::move(spawned.value());
  CT_REQUIRE(wait_for_file(ready, child, "READY"));
  const std::uint64_t holder_epoch = value_after(read_text_file(ready), "epoch=");
  CT_CHECK(holder_epoch > epoch_before);

  const auto excluded = open_store(scratch.path());
  CT_CHECK_MSG(failed_with(excluded, ct::ErrorCode::StoreLocked), "reported " + reported_code(excluded));

  // Abrupt, non-interactive termination: no shutdown path is given the chance
  // to release the lock.
  CT_CHECK(child.terminate().has_value());
  const ChildResult result = child.collect();
  CT_CHECK(result.terminated);
  CT_CHECK_MSG(result.exit_code == 97, "killed holder reported exit " + std::to_string(result.exit_code));

  // The operating system released the lock, the store is still whole, and the
  // next writer takes a strictly newer authority epoch.
  auto reopened = open_store(scratch.path());
  CT_CHECK_MSG(reopened.has_value(), "the lock outlived the process that held it: " + reported_code(reopened));
  if (reopened.has_value()) {
    CT_CHECK(reopened.value().epoch().value() > holder_epoch);
    const auto head = reopened.value().head();
    CT_REQUIRE(head.has_value());
    CT_CHECK(head.value().digest() == published.value().digest);
    const auto report = reopened.value().verify(ct::VerifyOptions{});
    CT_REQUIRE(report.has_value());
    CT_CHECK(report.value().ok());
    CT_CHECK_EQ(report.value().generations_present, std::size_t{1});
    CT_REQUIRE(reopened.value().close().has_value());
  }
  CT_CHECK(file_exists(scratch.child("lock")));
}

CT_TEST(multiprocess_a_superseded_writer_epoch_is_fenced) {
  ScratchDir scratch("mp-fencing");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_REQUIRE(store.close().has_value());

  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  auto spawned = spawn_child(kHoldLockCase, kHoldLockRole, {scratch.path(), ready, release});
  CT_REQUIRE(spawned.has_value());
  ChildProcess child = std::move(spawned.value());
  CT_REQUIRE(wait_for_file(ready, child, "READY"));
  const std::string ready_text = read_text_file(ready);
  const std::uint64_t dead_epoch = value_after(ready_text, "epoch=");
  const std::uint64_t dead_incarnation = value_after(ready_text, "incarnation=");
  CT_REQUIRE(dead_epoch > 0);
  CT_REQUIRE(dead_incarnation > 0);
  CT_REQUIRE(write_text_file(release, "release\n"));
  const ChildResult result = child.collect();
  CT_CHECK_MSG(result.exit_code == 0, "lock holder exit " + std::to_string(result.exit_code) + ": " + result.output);

  auto writer = open_store(scratch.path());
  CT_REQUIRE(writer.has_value());
  CT_CHECK(writer.value().epoch().value() > dead_epoch);
  CT_CHECK(writer.value().incarnation().value() > dead_incarnation);

  // A request planned by the writer that no longer exists is fenced instead of
  // being merged into the state the new writer owns.
  ct::PublicationRequest stale;
  stale.authority.epoch = ct::WriterEpoch(dead_epoch);
  stale.authority.incarnation = ct::WriterIncarnation(dead_incarnation);
  stale.authority.expected_base = ct::TopologyGeneration{};
  stale.mutation = mid("dead-writer");
  stale.attempt = *ct::AttemptOrdinal::parse(1);
  stale.draft = reference_variation(0);
  const auto fenced = writer.value().publish(stale);
  CT_CHECK_MSG(failed_with(fenced, ct::ErrorCode::StaleAuthorityEpoch), "reported " + reported_code(fenced));

  // The same request with the current epoch is fenced on the incarnation, so
  // the epoch is what refused the stale plan and not some other precondition.
  ct::PublicationRequest stale_incarnation = stale;
  stale_incarnation.authority.epoch = writer.value().epoch();
  const auto fenced_incarnation = writer.value().publish(stale_incarnation);
  CT_CHECK_MSG(failed_with(fenced_incarnation, ct::ErrorCode::StaleWriterIncarnation),
               "reported " + reported_code(fenced_incarnation));

  // Nothing was actuated by either refusal, and the live writer publishes.
  const auto info = writer.value().info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
  CT_CHECK(generation_file_names(scratch.path()).empty());
  const auto live = publish_variation(writer.value(), "live-writer", 0);
  CT_REQUIRE(live.has_value());
  CT_CHECK_EQ(live.value().generation.value(), std::uint64_t{1});
  CT_CHECK_EQ(live.value().commit_sequence.value(), std::uint64_t{1});
  CT_REQUIRE(writer.value().close().has_value());
}
// ---------------------------------------------------------------------------
// Parent side: publication killed at every documented crash point
// ---------------------------------------------------------------------------

/// One documented publication crash point, in both of its documented spellings.
struct PublicationStage {
  const char* reference;  ///< historical hyphenated stage name
  const char* dotted;     ///< dotted "publish." stage name
  /// True when the publication site is the second occurrence in this child: the
  /// first occurrence is the manifest write performed by opening the store for
  /// writing, which happens before any publication.
  bool second_occurrence;
  /// The head a fresh process must resolve: the previous committed generation
  /// when the crash happened before the commit point, the new one after it.
  unsigned head_after_crash;
};

const PublicationStage kPublicationStages[] = {
    {"partial-manifest-write", "publish.partial_manifest_write", true, 1},
    {"before-manifest-prev-update", "publish.before_manifest_previous_update", true, 1},
    {"after-manifest-prev-update", "publish.after_manifest_previous_update", true, 1},
    {"before-manifest-commit", "publish.before_manifest_write", true, 1},
    {"after-manifest-commit", "publish.after_manifest_write", true, 2},
    {"after-staging-flush", "publish.after_stage", false, 1},
    {"before-generation-rename", "publish.before_generation_rename", false, 1},
    {"after-generation-rename", "publish.after_generation_rename", false, 1},
    {"after-idempotency-pending", "publish.after_idempotency_pending", false, 1},
    {"before-floor", "publish.before_floor", false, 2},
    {"after-idempotency-accept", "publish.after_idempotency_accept", false, 2},
    {"after-commit", "publish.after_commit", false, 2},
};

CT_TEST(multiprocess_a_crashed_publication_never_leaves_a_hybrid_generation) {
  for (const PublicationStage& stage : kPublicationStages) {
    const std::string spellings[] = {stage.reference, stage.dotted};
    for (const std::string& spelling : spellings) {
      const std::string selector = stage.second_occurrence ? spelling + "#2" : spelling;
      ScratchDir scratch("mp-crash-" + spelling);
      auto created = create_store(scratch.path());
      CT_REQUIRE(created.has_value());
      Store setup = std::move(created.value());
      const auto baseline = publish_variation(setup, "crash-baseline", 0);
      CT_REQUIRE(baseline.has_value());
      CT_CHECK_EQ(baseline.value().generation.value(), std::uint64_t{1});
      CT_REQUIRE(setup.close().has_value());

      // A real independent process publishes the next generation with fault
      // injection enabled and terminates itself at the selected crash point.
      auto crashed = spawn_child(kPublishCrashCase, kPublishCrashRole,
                                 {scratch.path(), kCrashMutation, std::to_string(kCrashAttempt), "1"},
                                 {{"COOLING_TOPOLOGY_FAULT_STAGE", selector}});
      CT_CHECK_MSG(crashed.has_value(), "stage " + selector + ": " + reported_code(crashed));
      if (!crashed.has_value()) {
        continue;
      }
      ChildProcess crash_child = std::move(crashed.value());
      const ChildResult crash_result = crash_child.collect();
      CT_CHECK_MSG(crash_result.exit_code == 97,
                   "stage " + selector + ": the instrumented process exited " +
                       std::to_string(crash_result.exit_code) +
                       " instead of terminating at the crash point: " + crash_result.output);
      CT_CHECK(crash_result.exited);

      // A fresh process resolves the store before this one touches it: the head
      // must decode and match its manifest, every retained generation must
      // decode and link to its parent, and there must be exactly one file per
      // retained generation.
      const std::string expected = std::to_string(stage.head_after_crash);
      auto resolved = spawn_child(kResolveStoreCase, kResolveStoreRole, {scratch.path(), expected, expected});
      CT_CHECK_MSG(resolved.has_value(), "stage " + selector + ": " + reported_code(resolved));
      if (!resolved.has_value()) {
        continue;
      }
      ChildProcess resolve_child = std::move(resolved.value());
      const ChildResult resolve_result = resolve_child.collect();
      CT_CHECK_MSG(resolve_result.exit_code == 0,
                   "stage " + selector + ": a fresh process did not resolve the store: " + resolve_result.output);

      // The retry of the interrupted attempt is resolvable: it either replays
      // the generation the crash committed or publishes that same generation
      // number. Never a duplicate and never a gap.
      auto store = open_store(scratch.path());
      CT_CHECK_MSG(store.has_value(), "stage " + selector + ": " + reported_code(store));
      if (!store.has_value()) {
        continue;
      }
      const bool committed_before_crash = stage.head_after_crash == 2u;
      const auto retry = publish_variation(store.value(), kCrashMutation, 1, kCrashAttempt);
      CT_CHECK_MSG(retry.has_value(), "stage " + selector + ": the retry failed: " + reported_code(retry));
      if (retry.has_value()) {
        CT_CHECK_MSG(retry.value().replayed == committed_before_crash,
                     "stage " + selector + ": replayed=" + (retry.value().replayed ? "true" : "false") +
                         " although the crash was " +
                         (committed_before_crash ? "after" : "before") + " the commit point");
        CT_CHECK_EQ(retry.value().generation.value(), std::uint64_t{2});
        CT_CHECK_EQ(retry.value().commit_sequence.value(), std::uint64_t{2});
      }

      CT_CHECK_EQ(generation_file_count(scratch.path(), 1), std::size_t{1});
      CT_CHECK_EQ(generation_file_count(scratch.path(), 2), std::size_t{1});
      CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
      CT_CHECK(staging_file_names(scratch.path()).empty());
      const auto info = store.value().info();
      CT_REQUIRE(info.has_value());
      CT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
      CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{2});
      const auto report = store.value().verify(ct::VerifyOptions{});
      CT_REQUIRE(report.has_value());
      CT_CHECK_MSG(report.value().ok(), "stage " + selector + ": the recovered store did not verify cleanly");
      const auto second = store.value().load(ct::TopologyGeneration(2));
      CT_REQUIRE(second.has_value());
      CT_CHECK(second.value().parent_generation() == baseline.value().generation);
      CT_CHECK(second.value().parent_digest() == baseline.value().digest);
      CT_REQUIRE(store.value().close().has_value());
    }
  }
}

// ---------------------------------------------------------------------------
// Parent side: a concurrent reader while a writer publishes
// ---------------------------------------------------------------------------

CT_TEST(multiprocess_a_concurrent_reader_never_sees_a_partial_generation) {
  ScratchDir scratch("mp-concurrent-reader");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto empty = store.info();
  CT_REQUIRE(empty.has_value());
  CT_CHECK_EQ(empty.value().head.value(), std::uint64_t{0});
  CT_REQUIRE(store.close().has_value());

  const std::string round_prefix = scratch.child("round-");
  const std::string ack_prefix = scratch.child("ack-");
  const std::string done = scratch.child("done.txt");
  auto spawned =
      spawn_child(kPublishSequenceCase, kPublishSequenceRole, {scratch.path(), round_prefix, ack_prefix, done});
  CT_REQUIRE(spawned.has_value());
  ChildProcess child = std::move(spawned.value());
  CT_CHECK_EQ(child.process_id() != current_process_id(), true);

  std::uint64_t previous_head = 0;
  std::uint64_t observations = 0;
  // Every observed head must decode, must agree with the manifest digest and
  // must verify; the head may never move backwards.
  for (unsigned round = 1; round <= kHandshakeRounds; ++round) {
    const std::string round_file = round_prefix + std::to_string(round) + ".ok";
    CT_CHECK_MSG(wait_for_file(round_file, child, std::to_string(round)),
                 "round " + std::to_string(round) + " was never committed");
    auto reader = open_store(scratch.path(), ct::StoreMode::ReadOnly);
    CT_CHECK_MSG(reader.has_value(), "round " + std::to_string(round) + ": " + reported_code(reader));
    if (reader.has_value()) {
      const auto info = reader.value().info();
      CT_REQUIRE(info.has_value());
      CT_CHECK(info.value().head.value() >= static_cast<std::uint64_t>(round));
      CT_CHECK(info.value().head.value() >= previous_head);
      previous_head = info.value().head.value();
      const auto head = reader.value().head();
      CT_CHECK_MSG(head.has_value(),
                   "round " + std::to_string(round) + ": the observed head did not decode: " + reported_code(head));
      if (head.has_value()) {
        CT_CHECK(info.value().head_digest == head.value().digest());
        const auto report = reader.value().verify(ct::VerifyOptions{});
        CT_CHECK_MSG(report.has_value(), "round " + std::to_string(round) + ": " + reported_code(report));
        if (report.has_value()) {
          CT_CHECK_MSG(report.value().ok(),
                       "round " + std::to_string(round) + ": an observed state did not verify cleanly");
        }
      }
      CT_REQUIRE(reader.value().close().has_value());
    }
    ++observations;
    CT_REQUIRE(publish_text_file(ack_prefix + std::to_string(round) + ".ok", "ack\n"));
  }

  // Continuous phase: the child publishes without waiting for this process, and
  // this process opens the store read-only, reads the head and verifies it for
  // as long as the child is alive.
  std::uint64_t overlapping_observations = 0;
  for (int iteration = 0; iteration < 200000; ++iteration) {
    if (child.has_exited()) {
      break;
    }
    auto reader = open_store(scratch.path(), ct::StoreMode::ReadOnly);
    if (reader.has_value()) {
      const auto info = reader.value().info();
      const auto head = reader.value().head();
      const auto report = reader.value().verify(ct::VerifyOptions{});
      CT_CHECK_MSG(info.has_value() && head.has_value() && report.has_value(),
                   "an observation made while the writer was publishing failed to resolve");
      if (info.has_value() && head.has_value() && report.has_value()) {
        CT_CHECK(info.value().head_digest == head.value().digest());
        CT_CHECK_MSG(report.value().head_verified && only_stale_handle_defects(report.value()),
                     "an observed state reported a defect that is not the staleness of this handle: " +
                         finding_codes(report.value()));
        CT_CHECK(info.value().head.value() >= previous_head);
        previous_head = info.value().head.value();
      }
      CT_REQUIRE(reader.value().close().has_value());
      ++overlapping_observations;
    }
    std::this_thread::yield();
    if (iteration % 100 == 99) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  CT_CHECK_MSG(child.has_exited(), "the publishing child never finished its work");
  CT_CHECK_MSG(wait_for_file(done, child, "done"), "the publishing child never reported completion");
  const ChildResult result = child.collect();
  CT_CHECK_MSG(result.exit_code == 0, "publishing child exit " + std::to_string(result.exit_code) + ": " + result.output);
  CT_CHECK_EQ(observations, static_cast<std::uint64_t>(kHandshakeRounds));
  report_note("observed " + std::to_string(overlapping_observations) + " additional heads while the writer published");

  // The store the writer left behind is whole, complete and current.
  auto final_store = open_store(scratch.path());
  CT_REQUIRE(final_store.has_value());
  const auto info = final_store.value().info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), static_cast<std::uint64_t>(kSequenceRounds));
  CT_CHECK_EQ(info.value().commit_sequence.value(), static_cast<std::uint64_t>(kSequenceRounds));
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), static_cast<std::size_t>(kSequenceRounds));
  CT_CHECK(staging_file_names(scratch.path()).empty());
  const auto report = final_store.value().verify(ct::VerifyOptions{});
  CT_REQUIRE(report.has_value());
  CT_CHECK(report.value().ok());
  CT_CHECK(report.value().chain_verified);
  CT_CHECK(report.value().canonical_fixed_point_verified);
  const auto head = final_store.value().head();
  CT_REQUIRE(head.has_value());
  CT_CHECK_EQ(head.value().generation().value(), static_cast<std::uint64_t>(kSequenceRounds));
  CT_REQUIRE(final_store.value().close().has_value());
}

}  // namespace
