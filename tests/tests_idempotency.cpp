// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Idempotency, authority and retention proof obligations: an accepted attempt is
// replayed before any authority check and without re-actuation, a lost response
// survives close and reopen, a reused identity with different content is a
// conflict, a stale authority is fenced, and accepted-attempt retention is
// bounded on disk with the documented eviction semantics.
//
// A replay reports what was committed. It never claims that the state it
// recorded is current, running or safe.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/mutation.hpp"
#include "dccp/cooling_topology/store.hpp"

namespace {

namespace ct = dccp::cooling_topology;

using namespace ct_test;

CT_TEST(idempotency_replay_returns_the_identical_receipt) {
  ScratchDir scratch("idem-replay");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  const auto first = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(first.has_value());
  CT_CHECK(!first.value().replayed);
  const std::size_t files_before = generation_file_names(scratch.path()).size();
  const std::size_t records_before = idempotency_file_names(scratch.path()).size();

  // A retry re-planned against the current head.
  const auto replay = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(replay.has_value());
  CT_CHECK(replay.value().replayed);
  CT_CHECK(replay.value().generation == first.value().generation);
  CT_CHECK(replay.value().digest == first.value().digest);
  CT_CHECK(replay.value().parent_generation == first.value().parent_generation);
  CT_CHECK(replay.value().commit_sequence == first.value().commit_sequence);
  CT_CHECK(replay.value().head_after == first.value().head_after);
  CT_CHECK(replay.value().head_digest_after == first.value().head_digest_after);
  CT_CHECK_EQ(replay.value().durability, ct::PublicationDurability::Durable);

  // A replay is not a re-actuation: no second generation, no second record, no
  // advanced head, no advanced commit sequence.
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().retained_generations, std::size_t{1});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), files_before);
  CT_CHECK_EQ(idempotency_file_names(scratch.path()).size(), records_before);
  CT_CHECK(staging_file_names(scratch.path()).empty());
}

CT_TEST(idempotency_replay_survives_close_and_reopen_with_stale_authority) {
  ScratchDir scratch("idem-reopen");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  const ct::WriterEpoch epoch_when_planned = store.epoch();
  const ct::WriterIncarnation incarnation_when_planned = store.incarnation();
  const auto first = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(first.has_value());
  const std::size_t files_before = generation_file_names(scratch.path()).size();
  const std::size_t records_before = idempotency_file_names(scratch.path()).size();
  CT_REQUIRE(store.close().has_value());

  auto reopened = open_store(scratch.path());
  CT_REQUIRE(reopened.has_value());
  CT_CHECK(reopened.value().epoch().value() > epoch_when_planned.value());

  // The retry carries the authority the mutation was planned under - epoch,
  // incarnation and base generation are all superseded now. This is the lost
  // response case: the response never arrived, so the caller retries blindly.
  ct::PublicationRequest request;
  request.authority.epoch = epoch_when_planned;
  request.authority.incarnation = incarnation_when_planned;
  request.authority.expected_base = ct::TopologyGeneration{};
  request.mutation = mid("mut-1");
  request.attempt = *ct::AttemptOrdinal::parse(1);
  request.draft = reference_variation(0);

  const auto replay = reopened.value().publish(request);
  CT_REQUIRE(replay.has_value());
  CT_CHECK(replay.value().replayed);
  CT_CHECK_EQ(replay.value().generation.value(), first.value().generation.value());
  CT_CHECK(replay.value().digest == first.value().digest);
  CT_CHECK_EQ(replay.value().commit_sequence.value(), first.value().commit_sequence.value());
  CT_CHECK(replay.value().parent_generation == first.value().parent_generation);

  const auto info = reopened.value().info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{1});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), files_before);
  CT_CHECK_EQ(idempotency_file_names(scratch.path()).size(), records_before);
  const auto resumed = publish_variation(reopened.value(), "mut-2", 1);
  CT_REQUIRE(resumed.has_value());
  CT_CHECK_EQ(resumed.value().generation.value(), std::uint64_t{2});
}

CT_TEST(idempotency_replay_wins_over_a_stale_base_generation) {
  ScratchDir scratch("idem-stale-base");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(first.has_value());
  const auto second = publish_variation(store, "mut-2", 1);
  CT_REQUIRE(second.has_value());
  CT_CHECK_EQ(second.value().generation.value(), std::uint64_t{2});

  // The retry of the first mutation is planned against the state it was
  // accepted under, which is two publications out of date.
  ct::PublicationRequest request;
  request.authority.epoch = ct::WriterEpoch(1);
  request.authority.incarnation = ct::WriterIncarnation(1);
  request.authority.expected_base = ct::TopologyGeneration{};
  request.mutation = mid("mut-1");
  request.attempt = *ct::AttemptOrdinal::parse(1);
  request.draft = reference_variation(0);

  const auto replay = store.publish(request);
  CT_REQUIRE(replay.has_value());
  CT_CHECK(replay.value().replayed);
  CT_CHECK_EQ(replay.value().generation.value(), std::uint64_t{1});
  CT_CHECK(replay.value().digest == first.value().digest);
  // The head is untouched by a replay of an older publication.
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{2});
  CT_CHECK_EQ(info.value().head_digest.to_hex(), second.value().digest.to_hex());
}

CT_TEST(idempotency_same_identity_with_different_content_conflicts) {
  ScratchDir scratch("idem-conflict");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(first.has_value());

  const auto ambiguous = publish_variation(store, "mut-1", 1);
  CT_CHECK_MSG(failed_with(ambiguous, ct::ErrorCode::IdempotencyConflict), "reported " + reported_code(ambiguous));

  // A conflict changes nothing: the recorded attempt and the head are intact.
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{1});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{1});
  CT_CHECK_EQ(idempotency_file_names(scratch.path()).size(), std::size_t{1});
  CT_CHECK(staging_file_names(scratch.path()).empty());

  // The original content still replays.
  const auto replay = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(replay.has_value());
  CT_CHECK(replay.value().replayed);
}
CT_TEST(idempotency_a_different_attempt_ordinal_is_an_ordinary_mutation) {
  ScratchDir scratch("idem-attempts");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  const auto first = publish_variation(store, "mut-1", 0, 1);
  CT_REQUIRE(first.has_value());
  CT_CHECK_EQ(first.value().generation.value(), std::uint64_t{1});
  CT_CHECK_EQ(first.value().attempt.value(), 1u);

  // A second attempt of the same mutation with different content is a new
  // mutation, not a replay and not a conflict.
  const auto second = publish_variation(store, "mut-1", 1, 2);
  CT_REQUIRE(second.has_value());
  CT_CHECK(!second.value().replayed);
  CT_CHECK_EQ(second.value().generation.value(), std::uint64_t{2});
  CT_CHECK_EQ(second.value().attempt.value(), 2u);
  CT_CHECK_EQ(second.value().parent_generation.value(), std::uint64_t{1});

  // ... and that second attempt now replays on its own identity.
  const auto replay = publish_variation(store, "mut-1", 1, 2);
  CT_REQUIRE(replay.has_value());
  CT_CHECK(replay.value().replayed);
  CT_CHECK_EQ(replay.value().generation.value(), std::uint64_t{2});
  CT_CHECK(replay.value().digest == second.value().digest);

  // An attempt ordinal of zero and an absent mutation identity are not valid
  // requests at all, and they are refused before anything is written.
  const auto well_formed = make_request(store, reference_variation(2), "mut-3");
  CT_REQUIRE(well_formed.has_value());
  ct::PublicationRequest zero_attempt = well_formed.value();
  zero_attempt.attempt = ct::AttemptOrdinal{};
  const auto refused_attempt = store.publish(zero_attempt);
  CT_CHECK_MSG(failed_with(refused_attempt, ct::ErrorCode::InvalidArgument),
               "reported " + reported_code(refused_attempt));
  ct::PublicationRequest empty_mutation = well_formed.value();
  empty_mutation.mutation = ct::MutationId{};
  const auto refused_mutation = store.publish(empty_mutation);
  CT_CHECK_MSG(failed_with(refused_mutation, ct::ErrorCode::InvalidArgument),
               "reported " + reported_code(refused_mutation));

  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
}

CT_TEST(idempotency_stale_authority_is_refused_before_any_actuation) {
  ScratchDir scratch("idem-stale-authority");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto request = make_request(store, reference_variation(0), "mut-1");
  CT_REQUIRE(request.has_value());

  ct::PublicationRequest stale = request.value();
  stale.authority.epoch = ct::WriterEpoch(store.epoch().value() + 1);
  const auto fenced_epoch = store.publish(stale);
  CT_CHECK_MSG(failed_with(fenced_epoch, ct::ErrorCode::StaleAuthorityEpoch),
               "reported " + reported_code(fenced_epoch));

  stale = request.value();
  stale.authority.incarnation = ct::WriterIncarnation(store.incarnation().value() + 1);
  const auto fenced_incarnation = store.publish(stale);
  CT_CHECK_MSG(failed_with(fenced_incarnation, ct::ErrorCode::StaleWriterIncarnation),
               "reported " + reported_code(fenced_incarnation));

  stale = request.value();
  stale.authority.expected_base = ct::TopologyGeneration(7);
  const auto fenced_base = store.publish(stale);
  CT_CHECK_MSG(failed_with(fenced_base, ct::ErrorCode::StaleBaseGeneration),
               "reported " + reported_code(fenced_base));

  // A refusal is not an actuation: nothing was staged, recorded or committed.
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{0});
  CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{0});
  CT_CHECK(generation_file_names(scratch.path()).empty());
  CT_CHECK(idempotency_file_names(scratch.path()).empty());
  CT_CHECK(staging_file_names(scratch.path()).empty());

  // The same, still unused identity is accepted afterwards and then replays.
  const auto accepted = store.publish(request.value());
  CT_REQUIRE(accepted.has_value());
  CT_CHECK(!accepted.value().replayed);
  CT_CHECK_EQ(accepted.value().generation.value(), std::uint64_t{1});
  const auto replay = store.publish(request.value());
  CT_REQUIRE(replay.has_value());
  CT_CHECK(replay.value().replayed);
  CT_CHECK_EQ(replay.value().commit_sequence.value(), std::uint64_t{1});
}

CT_TEST(idempotency_retention_bounds_the_recorded_attempts) {
  ScratchDir scratch("idem-bounded");
  StoreOptions options = store_options(scratch.path(), ct::StoreMode::ReadWrite, true);
  options.idempotency_retention = 3;
  auto created = create_store_with(options, facility_reference());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  for (std::uint64_t index = 1; index <= 6; ++index) {
    const auto receipt = publish_variation(store, "mut-" + std::to_string(index), static_cast<std::size_t>(index));
    CT_REQUIRE(receipt.has_value());
    CT_CHECK_EQ(receipt.value().generation.value(), index);
  }
  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{6});
  // The bound holds on disk, not only in the reported count.
  CT_CHECK(info.value().idempotency_records <= std::size_t{3});
  CT_CHECK(idempotency_file_names(scratch.path()).size() <= std::size_t{3});

  // The newest three attempts remain replayable, and replaying them changes
  // nothing on disk.
  for (std::uint64_t index = 4; index <= 6; ++index) {
    const auto replay = publish_variation(store, "mut-" + std::to_string(index), static_cast<std::size_t>(index));
    CT_CHECK_MSG(replay.has_value(), "the newest attempts must replay: " + reported_code(replay));
    if (replay.has_value()) {
      CT_CHECK(replay.value().replayed);
      CT_CHECK_EQ(replay.value().generation.value(), index);
    }
  }
  CT_CHECK(idempotency_file_names(scratch.path()).size() <= std::size_t{3});
  CT_CHECK_EQ(store.info().value().head.value(), std::uint64_t{6});

  // An evicted attempt is no longer recognizable: it is treated as a new
  // mutation. The store never claims to have detected the eviction, because it
  // cannot distinguish an evicted attempt from an unseen one.
  const auto evicted = publish_variation(store, "mut-1", 1);
  CT_CHECK_MSG(evicted.has_value(), "an evicted attempt must be an ordinary mutation: " + reported_code(evicted));
  if (evicted.has_value()) {
    CT_CHECK(!evicted.value().replayed);
    CT_CHECK_EQ(evicted.value().generation.value(), std::uint64_t{7});
    CT_CHECK_EQ(evicted.value().commit_sequence.value(), std::uint64_t{7});
  }
  CT_CHECK(idempotency_file_names(scratch.path()).size() <= std::size_t{3});
  // The window slid: the record for generation 4 has been evicted and a newer
  // one is replayable.
  const auto still_recorded = publish_variation(store, "mut-6", 6);
  CT_CHECK_MSG(still_recorded.has_value(), "reported " + reported_code(still_recorded));
  if (still_recorded.has_value()) {
    CT_CHECK(still_recorded.value().replayed);
    CT_CHECK_EQ(still_recorded.value().generation.value(), std::uint64_t{6});
  }
}

CT_TEST(idempotency_content_digest_is_logical_not_positional) {
  const ct::TopologyDraft baseline = reference_variation(0);
  const ct::Digest content = ct::mutation_content_digest(baseline);

  // Table order is not content: the same logical draft permuted is the same
  // mutation and therefore replays.
  ct::TopologyDraft permuted = baseline;
  std::reverse(permuted.nodes.begin(), permuted.nodes.end());
  std::reverse(permuted.edges.begin(), permuted.edges.end());
  std::reverse(permuted.groups.begin(), permuted.groups.end());
  CT_CHECK(ct::mutation_content_digest(permuted) == content);

  // A real content change is a different mutation.
  ct::TopologyDraft renamed = baseline;
  renamed.nodes.front().display_name = "renamed-for-content-digest";
  CT_CHECK(!(ct::mutation_content_digest(renamed) == content));

  // The facility binding is content, so a draft for another facility can never
  // collide with this one.
  ct::TopologyDraft other_facility = baseline;
  other_facility.facility = facility_reference("dc-2", 7);
  CT_CHECK(!(ct::mutation_content_digest(other_facility) == content));

  // The content digest is not the generation digest: a generation digest also
  // covers the generation number and the parent binding.
  ScratchDir scratch("idem-content-digest");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto published = publish_variation(store, "mut-1", 0);
  CT_REQUIRE(published.has_value());
  CT_CHECK(!(published.value().digest == content));
  CT_CHECK_EQ(store.info().value().head_digest.to_hex(), published.value().digest.to_hex());
}

}  // namespace
