// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Crash and recovery proof obligations for the durable store: a healthy store
// is left byte-identical, a damaged head is repaired by adopting the retained
// previous *committed* publication, an unrepairable store stays unverifiable
// instead of becoming a hybrid, the durable floor is a rollback guard that
// cannot be removed, and unverifiable generation content is quarantined rather
// than deleted.
//
// Recovery restores committed structural state. It never claims that anything
// is running, flowing, available or thermally safe.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/store.hpp"

namespace {

namespace ct = dccp::cooling_topology;

using namespace ct_test;

/// True when the report carries this finding code at this severity. The code is
/// the documented contract; message text is never inspected.
bool has_finding(const ct::VerifyReport& report, std::string_view code, ct::VerifySeverity severity) {
  for (const ct::VerifyFinding& finding : report.findings) {
    if (finding.code == code && finding.severity == severity) {
      return true;
    }
  }
  return false;
}

bool has_defect(const ct::VerifyReport& report) {
  for (const ct::VerifyFinding& finding : report.findings) {
    if (finding.severity == ct::VerifySeverity::Defect) {
      return true;
    }
  }
  return false;
}

/// The three ways a generation file can be damaged that the suite proves.
enum class Damage { Truncated, PayloadByte, DigestByte };

std::string damage_tag(Damage damage) {
  switch (damage) {
    case Damage::Truncated:
      return "truncated";
    case Damage::PayloadByte:
      return "payload-byte";
    case Damage::DigestByte:
      return "digest-byte";
  }
  return "unknown";
}

/// Damages one generation file in place. A truncated frame, a payload byte
/// change and a recorded-digest byte change all leave a file that cannot be
/// verified, in three different ways: the frame is short, the payload digest
/// does not match, and the recorded digest does not match its payload.
bool damage_generation_file(const std::string& path, Damage damage) {
  const std::string content = read_binary_file(path);
  if (content.size() <= 128) {
    return false;
  }
  switch (damage) {
    case Damage::Truncated:
      return truncate_file(path, content.size() / 2);
    case Damage::PayloadByte:
      return flip_file_byte(path, content.size() / 2);
    case Damage::DigestByte:
      return flip_file_byte(path, content.size() - 1);
  }
  return false;
}

CT_TEST(recovery_of_a_healthy_store_reports_no_action_and_changes_nothing) {
  ScratchDir scratch("recovery-noaction");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "recovery-1", 0);
  CT_REQUIRE(first.has_value());
  const auto second = publish_variation(store, "recovery-2", 1);
  CT_REQUIRE(second.has_value());

  // The snapshot is taken while the store is open, so the writer-epoch manifest
  // write of this very open is already part of it. Recovery must add nothing.
  const std::map<std::string, std::string> before = tree_snapshot(scratch.path());
  CT_REQUIRE(!before.empty());
  const auto report = store.recover(ct::RecoveryOptions{});
  CT_REQUIRE(report.has_value());
  CT_CHECK_EQ(report.value().outcome, ct::RecoveryOutcome::NoAction);
  CT_CHECK_EQ(report.value().head_before.value(), std::uint64_t{2});
  CT_CHECK_EQ(report.value().head_after.value(), std::uint64_t{2});
  CT_CHECK(report.value().head_digest_after == second.value().digest);
  CT_CHECK(report.value().floor_respected);
  CT_CHECK_EQ(report.value().residue_removed, std::size_t{0});
  CT_CHECK(report.value().steps.empty());

  const std::map<std::string, std::string> after = tree_snapshot(scratch.path());
  const std::string difference = tree_difference(before, after);
  CT_CHECK_MSG(difference.empty(), "a healthy store was changed by recovery: " + difference);
  CT_CHECK_EQ(store.info().value().head.value(), std::uint64_t{2});
}

CT_TEST(recovery_of_an_empty_store_reports_no_action) {
  ScratchDir scratch("recovery-empty");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  const std::map<std::string, std::string> before = tree_snapshot(scratch.path());
  const auto report = store.recover(ct::RecoveryOptions{});
  CT_REQUIRE(report.has_value());
  CT_CHECK_EQ(report.value().outcome, ct::RecoveryOutcome::NoAction);
  CT_CHECK(!report.value().head_before.published());
  CT_CHECK(!report.value().head_after.published());
  CT_CHECK(report.value().head_digest_after.is_zero());
  CT_CHECK_EQ(report.value().residue_removed, std::size_t{0});
  const std::map<std::string, std::string> after = tree_snapshot(scratch.path());
  CT_CHECK_MSG(trees_equal(before, after), "an empty store was changed by recovery: " + tree_difference(before, after));
}

CT_TEST(recovery_adopts_the_retained_previous_publication_when_the_head_is_damaged) {
  const Damage damages[] = {Damage::Truncated, Damage::PayloadByte, Damage::DigestByte};
  for (const Damage damage : damages) {
    ScratchDir scratch("recovery-adopt-" + damage_tag(damage));
    auto created = create_store(scratch.path());
    CT_REQUIRE(created.has_value());
    Store store = std::move(created.value());
    const auto first = publish_variation(store, "adopt-1", 0);
    CT_REQUIRE(first.has_value());
    const auto second = publish_variation(store, "adopt-2", 1);
    CT_REQUIRE(second.has_value());
    const auto third = publish_variation(store, "adopt-3", 2);
    CT_REQUIRE(third.has_value());

    // The head generation file is damaged while the handle is open, so the
    // retained previous publication is still recorded in manifest.prev.
    CT_REQUIRE(generation_file_names(scratch.path()).size() == 3);
    const std::string damaged_name = generation_file_names(scratch.path()).back();
    const std::string head_file = generation_file_path(scratch.path(), 3);
    CT_REQUIRE(!head_file.empty());
    CT_CHECK(damage_generation_file(head_file, damage));
    const std::string damaged_bytes = read_binary_file(head_file);

    const auto report = store.recover(ct::RecoveryOptions{});
    CT_REQUIRE(report.has_value());
    CT_CHECK_EQ(report.value().outcome, ct::RecoveryOutcome::AdoptedPrevious);
    CT_CHECK_EQ(report.value().head_before.value(), std::uint64_t{3});
    CT_CHECK_EQ(report.value().head_after.value(), std::uint64_t{2});
    CT_CHECK(report.value().head_digest_after == second.value().digest);
    CT_CHECK(report.value().floor_respected);
    CT_CHECK(!report.value().steps.empty());

    // Exactly one authoritative head, and it is the adopted generation with its
    // own digest.
    const auto info = store.info();
    CT_REQUIRE(info.has_value());
    CT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
    CT_CHECK(info.value().head_digest == second.value().digest);
    CT_CHECK_EQ(info.value().open_state, ct::StoreOpenState::Recovered);
    const auto head = store.head();
    CT_REQUIRE(head.has_value());
    CT_CHECK_EQ(head.value().generation().value(), std::uint64_t{2});
    CT_CHECK(head.value().digest() == second.value().digest);
    CT_CHECK(head.value().parent_digest() == first.value().digest);
    CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{2});
    CT_CHECK_EQ(manifest_field(scratch.path(), "head-digest"), second.value().digest.to_hex());

    // The damaged generation is gone from the generations directory, is not
    // reachable as a generation, and is preserved byte for byte in quarantine:
    // moved aside, never deleted.
    CT_CHECK_EQ(generation_file_count(scratch.path(), 3), std::size_t{0});
    CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
    const auto unreachable = store.load(ct::TopologyGeneration(3));
    CT_CHECK_MSG(failed_with(unreachable, ct::ErrorCode::GenerationNotRetained),
                 "reported " + reported_code(unreachable));
    const std::vector<std::string> quarantined = quarantine_file_names(scratch.path());
    CT_REQUIRE(quarantined.size() == 1);
    CT_CHECK_EQ(quarantined.front(), damaged_name);
    CT_CHECK_EQ(quarantined_bytes(scratch.path(), damaged_name), damaged_bytes);

    // Every retained generation still decodes, and verification reports the
    // quarantined evidence without a defect.
    CT_CHECK(store.load(ct::TopologyGeneration(1)).has_value());
    CT_CHECK(store.load(ct::TopologyGeneration(2)).has_value());
    const auto verification = store.verify(ct::VerifyOptions{});
    CT_REQUIRE(verification.has_value());
    CT_CHECK_MSG(verification.value().ok(), "the adopted store did not verify cleanly");
    CT_CHECK(!has_defect(verification.value()));
    CT_CHECK_EQ(verification.value().quarantined_found, std::size_t{1});
    CT_CHECK(has_finding(verification.value(), "quarantined_content", ct::VerifySeverity::Warning));
    CT_CHECK(verification.value().head_verified);
    CT_CHECK(verification.value().chain_verified);

    // The repaired store keeps publishing, at the generation number the damaged
    // file claimed, with the next commit sequence.
    const auto resumed = publish_variation(store, "adopt-resumed", 3);
    CT_CHECK_MSG(resumed.has_value(), "the repaired store refused to publish: " + reported_code(resumed));
    if (resumed.has_value()) {
      CT_CHECK_EQ(resumed.value().generation.value(), std::uint64_t{3});
      CT_CHECK_EQ(resumed.value().commit_sequence.value(), std::uint64_t{3});
      CT_CHECK_EQ(resumed.value().parent_generation.value(), std::uint64_t{2});
    }
  }
}
CT_TEST(recovery_refuses_when_no_previous_publication_can_be_adopted) {
  ScratchDir scratch("recovery-no-previous");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_REQUIRE(publish_variation(store, "refuse-1", 0).has_value());
  CT_REQUIRE(publish_variation(store, "refuse-2", 1).has_value());

  const std::string head_file = generation_file_path(scratch.path(), 2);
  CT_REQUIRE(!head_file.empty());
  CT_CHECK(damage_generation_file(head_file, Damage::Truncated));
  // The only fallback this store had recorded is removed as well.
  CT_REQUIRE(remove_tree(scratch.child("manifest.prev")));
  CT_CHECK(!file_exists(scratch.child("manifest.prev")));

  const auto report = store.recover(ct::RecoveryOptions{});
  CT_CHECK_MSG(!report.has_value(), "recovery adopted a publication although no previous record existed");
  if (!report.has_value()) {
    CT_CHECK_MSG(report.error().code() == ct::ErrorCode::RecoveryUnavailable,
                 "reported " + reported_code(report));
  }

  // The store stays unverifiable: no invented head, no hybrid chain, and
  // nothing was deleted or moved aside by the refusal.
  const auto head = store.head();
  CT_CHECK(!head.has_value());
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{2});
  const auto verification = store.verify(ct::VerifyOptions{});
  CT_REQUIRE(verification.has_value());
  CT_CHECK(!verification.value().ok());
  CT_CHECK(!verification.value().head_verified);
  CT_CHECK(has_finding(verification.value(), "head_unverified", ct::VerifySeverity::Defect));
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
  CT_CHECK(quarantine_file_names(scratch.path()).empty());
  CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{2});
}

CT_TEST(recovery_refuses_when_the_previous_publication_is_unreadable) {
  ScratchDir scratch("recovery-unreadable-previous");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_REQUIRE(publish_variation(store, "unreadable-1", 0).has_value());
  CT_REQUIRE(publish_variation(store, "unreadable-2", 1).has_value());

  const std::string head_file = generation_file_path(scratch.path(), 2);
  CT_REQUIRE(!head_file.empty());
  CT_CHECK(damage_generation_file(head_file, Damage::PayloadByte));
  const std::string previous_file = scratch.child("manifest.prev");
  const std::string previous_bytes = read_binary_file(previous_file);
  CT_REQUIRE(!previous_bytes.empty());
  CT_REQUIRE(truncate_file(previous_file, previous_bytes.size() / 2));

  const auto report = store.recover(ct::RecoveryOptions{});
  CT_CHECK_MSG(!report.has_value(), "recovery adopted a previous publication that does not parse");
  if (!report.has_value()) {
    CT_CHECK_MSG(report.error().code() == ct::ErrorCode::HeadCorrupt, "reported " + reported_code(report));
  }

  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
  CT_CHECK(!store.head().has_value());
  const auto verification = store.verify(ct::VerifyOptions{});
  CT_REQUIRE(verification.has_value());
  CT_CHECK(!verification.value().ok());
  CT_CHECK(has_finding(verification.value(), "head_unverified", ct::VerifySeverity::Defect));
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
  CT_CHECK(quarantine_file_names(scratch.path()).empty());
}

CT_TEST(recovery_needs_a_writable_store_and_serves_adopted_state_read_only) {
  ScratchDir scratch("recovery-read-only");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "readonly-1", 0);
  CT_REQUIRE(first.has_value());
  const auto second = publish_variation(store, "readonly-2", 1);
  CT_REQUIRE(second.has_value());
  CT_REQUIRE(store.close().has_value());

  // The committed head is damaged, so the reader can only be served by the
  // retained previous publication - which a read-only handle may neither commit
  // nor repair.
  const std::string head_file = generation_file_path(scratch.path(), 2);
  CT_REQUIRE(!head_file.empty());
  CT_CHECK(damage_generation_file(head_file, Damage::Truncated));

  auto reader = open_store(scratch.path(), ct::StoreMode::ReadOnly);
  CT_CHECK_MSG(reader.has_value(), "the read-only handle did not open: " + reported_code(reader));
  if (reader.has_value()) {
    CT_CHECK_EQ(reader.value().open_state(), ct::StoreOpenState::Recovered);
    const auto info = reader.value().info();
    CT_REQUIRE(info.has_value());
    CT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
    CT_CHECK(!info.value().publication_allowed);
    CT_CHECK(!info.value().writable);
    const auto head = reader.value().head();
    CT_REQUIRE(head.has_value());
    CT_CHECK(head.value().digest() == first.value().digest);

    // The handle serves adopted state that was never committed; verification
    // says so instead of pretending the manifest agrees with it. The adopted
    // head itself verifies, which is why recovery has nothing to do yet.
    const auto verification = reader.value().verify(ct::VerifyOptions{});
    CT_REQUIRE(verification.has_value());
    CT_CHECK(!verification.value().ok());
    CT_CHECK(!verification.value().manifest_verified);
    CT_CHECK(verification.value().head_verified);
    CT_CHECK(verification.value().recovered_state);
    CT_CHECK(has_finding(verification.value(), "manifest_diverged", ct::VerifySeverity::Defect));
    CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{2});
    const auto nothing_to_do = reader.value().recover(ct::RecoveryOptions{});
    CT_REQUIRE(nothing_to_do.has_value());
    CT_CHECK_EQ(nothing_to_do.value().outcome, ct::RecoveryOutcome::NoAction);

    // Once even the adopted head is unverifiable, recovery would have to rewrite
    // the head manifest - and a read-only handle may not.
    const std::string adopted_file = generation_file_path(scratch.path(), 1);
    CT_REQUIRE(!adopted_file.empty());
    CT_CHECK(damage_generation_file(adopted_file, Damage::Truncated));
    const auto refused = reader.value().recover(ct::RecoveryOptions{});
    CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::StoreReadOnly), "reported " + reported_code(refused));
    CT_CHECK(!reader.value().head().has_value());
    CT_REQUIRE(reader.value().close().has_value());
  }
}

CT_TEST(recovery_commits_the_repair_when_the_store_is_writable) {
  ScratchDir scratch("recovery-writable");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "writable-1", 0);
  CT_REQUIRE(first.has_value());
  CT_REQUIRE(publish_variation(store, "writable-2", 1).has_value());
  CT_REQUIRE(store.close().has_value());

  const std::string head_file = generation_file_path(scratch.path(), 2);
  CT_REQUIRE(!head_file.empty());
  CT_CHECK(damage_generation_file(head_file, Damage::Truncated));

  // A writable handle adopts the retained publication and commits the repair, so
  // the adopted state becomes authoritative again rather than staying a
  // read-only view of an uncommitted head.
  auto writer = open_store(scratch.path());
  CT_CHECK_MSG(writer.has_value(), "the writable handle did not open: " + reported_code(writer));
  if (!writer.has_value()) {
    return;
  }
  CT_CHECK_EQ(writer.value().open_state(), ct::StoreOpenState::Recovered);
  const auto info = writer.value().info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  CT_CHECK(info.value().publication_allowed);
  CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{1});
  CT_CHECK_EQ(manifest_field(scratch.path(), "head-digest"), first.value().digest.to_hex());
  const auto verification = writer.value().verify(ct::VerifyOptions{});
  CT_REQUIRE(verification.has_value());
  CT_CHECK_MSG(verification.value().ok(), "the committed repair did not verify cleanly");
  CT_CHECK(verification.value().manifest_verified);
  CT_CHECK(verification.value().head_verified);
  CT_CHECK(verification.value().chain_verified);
  // The repaired store keeps publishing at the next generation number.
  const auto resumed = publish_variation(writer.value(), "writable-resumed", 2);
  CT_CHECK_MSG(resumed.has_value(), "the repaired store refused to publish: " + reported_code(resumed));
  if (resumed.has_value()) {
    CT_CHECK_EQ(resumed.value().generation.value(), std::uint64_t{2});
    CT_CHECK_EQ(resumed.value().commit_sequence.value(), std::uint64_t{2});
  }
  CT_REQUIRE(writer.value().close().has_value());
}

CT_TEST(deleting_the_durable_floor_is_refused_by_the_rollback_guard) {
  ScratchDir scratch("recovery-floor-missing");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_REQUIRE(publish_variation(store, "floor-1", 0).has_value());
  CT_REQUIRE(store.close().has_value());
  CT_REQUIRE(remove_tree(scratch.child("floor")));
  CT_CHECK(!file_exists(scratch.child("floor")));

  const auto writable = open_store(scratch.path());
  CT_CHECK_MSG(failed_with(writable, ct::ErrorCode::IntegrityFailure), "reported " + reported_code(writable));
  const auto read_only = open_store(scratch.path(), ct::StoreMode::ReadOnly);
  CT_CHECK_MSG(failed_with(read_only, ct::ErrorCode::IntegrityFailure), "reported " + reported_code(read_only));

  // The same guard protects the recovery path: a damaged head plus a missing
  // floor cannot be resolved by adopting anything.
  ScratchDir damaged("recovery-floor-missing-damaged");
  auto other = create_store(damaged.path());
  CT_REQUIRE(other.has_value());
  Store second = std::move(other.value());
  CT_REQUIRE(publish_variation(second, "floor-2a", 0).has_value());
  CT_REQUIRE(publish_variation(second, "floor-2b", 1).has_value());
  CT_REQUIRE(second.close().has_value());
  const std::string head_file = generation_file_path(damaged.path(), 2);
  CT_REQUIRE(!head_file.empty());
  CT_CHECK(damage_generation_file(head_file, Damage::Truncated));
  CT_REQUIRE(remove_tree(damaged.child("floor")));
  const auto refused = open_store(damaged.path());
  CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::IntegrityFailure), "reported " + reported_code(refused));
}

CT_TEST(a_manifest_below_the_durable_floor_is_refused_as_a_rollback) {
  ScratchDir scratch("recovery-floor-forged");
  StoreOptions options = store_options(scratch.path(), ct::StoreMode::ReadWrite, true);
  options.retained_generations = 2;
  auto created = create_store_with(options, facility_reference());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  for (std::uint64_t index = 1; index <= 4; ++index) {
    CT_REQUIRE(publish_variation(store, "rollback-" + std::to_string(index), static_cast<std::size_t>(index))
                   .has_value());
  }
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{4});
  CT_CHECK_EQ(info.value().floor.value(), std::uint64_t{3});
  CT_REQUIRE(store.close().has_value());

  // An internally consistent, correctly checksummed manifest whose head is
  // below the durable floor: exactly the rollback a careless restore performs.
  CT_REQUIRE(rewrite_record_field(scratch.child("manifest"), "head", "2"));
  CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{2});
  const std::string forged = read_binary_file(scratch.child("manifest"));

  const auto writable = open_store(scratch.path());
  CT_CHECK_MSG(failed_with(writable, ct::ErrorCode::GenerationFloorViolation),
               "reported " + reported_code(writable));
  const auto read_only = open_store(scratch.path(), ct::StoreMode::ReadOnly);
  CT_CHECK_MSG(failed_with(read_only, ct::ErrorCode::GenerationFloorViolation),
               "reported " + reported_code(read_only));

  // The refusal repaired nothing: the forged record is byte-identical and the
  // rollback guard still records the higher floor.
  CT_CHECK_EQ(read_binary_file(scratch.child("manifest")), forged);
  CT_CHECK_EQ(floor_file_value(scratch.path()), std::uint64_t{3});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
  CT_CHECK(quarantine_file_names(scratch.path()).empty());
}

CT_TEST(a_damaged_non_head_generation_is_reported_but_never_adopted) {
  ScratchDir scratch("recovery-non-head");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "nonhead-1", 0);
  CT_REQUIRE(first.has_value());
  const auto second = publish_variation(store, "nonhead-2", 1);
  CT_REQUIRE(second.has_value());

  // The older retained generation is damaged, not the head.
  const std::string older = generation_file_path(scratch.path(), 1);
  CT_REQUIRE(!older.empty());
  CT_CHECK(damage_generation_file(older, Damage::PayloadByte));

  // Recovery is conservative: the head verifies, so nothing is adopted and the
  // damaged file is not moved aside. It is reported instead.
  const auto report = store.recover(ct::RecoveryOptions{});
  CT_REQUIRE(report.has_value());
  CT_CHECK_EQ(report.value().outcome, ct::RecoveryOutcome::NoAction);
  CT_CHECK_EQ(report.value().head_after.value(), std::uint64_t{2});
  CT_CHECK(quarantine_file_names(scratch.path()).empty());
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});

  const auto damaged_retained = store.load(ct::TopologyGeneration(1));
  CT_CHECK_MSG(failed_with(damaged_retained, ct::ErrorCode::DigestMismatch),
               "reported " + reported_code(damaged_retained));
  const auto verification = store.verify(ct::VerifyOptions{});
  CT_REQUIRE(verification.has_value());
  CT_CHECK(!verification.value().ok());
  CT_CHECK(verification.value().head_verified);
  CT_CHECK(!verification.value().chain_verified);
  CT_CHECK_EQ(verification.value().quarantined_found, std::size_t{0});
  CT_CHECK(has_finding(verification.value(), "generation_unverified", ct::VerifySeverity::Defect));
  CT_CHECK(has_finding(verification.value(), "unreferenced_generation", ct::VerifySeverity::Defect));

  // The head itself is still served exactly.
  const auto head = store.head();
  CT_REQUIRE(head.has_value());
  CT_CHECK(head.value().digest() == second.value().digest);
}

}  // namespace
