// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store proof obligations: creation and reopening, the info surface,
// publication of a chain of generations with the documented commit sequence,
// head/manifest agreement, load and history, read-only and closed handles, move
// semantics, facility binding and the exact on-disk layout.
//
// Every assertion below is about durable structure. Nothing here asks whether a
// component is running, whether coolant is flowing, whether a path has capacity
// or whether a zone is thermally safe.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/store.hpp"
#include "dccp/cooling_topology/version.hpp"

namespace {

namespace ct = dccp::cooling_topology;

using namespace ct_test;

CT_TEST(store_create_reports_identity_binding_and_boundary) {
  ScratchDir scratch("store-create");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_CHECK(store.is_open());

  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().store_id.str(), std::string("test-store-1"));
  CT_CHECK(info.value().facility.same_binding_as(facility_reference()));
  CT_CHECK_EQ(info.value().facility.identity, std::string("dc-1"));
  CT_CHECK(!info.value().head.published());
  CT_CHECK(!info.value().floor.published());
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{0});
  CT_CHECK_EQ(info.value().epoch.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().incarnation.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().mode, ct::StoreMode::ReadWrite);
  CT_CHECK_EQ(info.value().open_state, ct::StoreOpenState::Fresh);
  CT_CHECK_EQ(info.value().retained_generations, std::size_t{0});
  CT_CHECK_EQ(info.value().idempotency_records, std::size_t{0});
  CT_CHECK(info.value().writable);
  CT_CHECK(info.value().publication_allowed);
  CT_CHECK_EQ(info.value().root, std::filesystem::path(scratch.path()).lexically_normal().string());

  // The systems boundary is reported with every info surface, so a consumer
  // cannot mistake this component for a control system.
  CT_CHECK_EQ(info.value().boundary, std::string(ct::systems_boundary()));
  CT_CHECK(info.value().boundary.find("no operating state") != std::string::npos);
  CT_CHECK(info.value().boundary.find("no thermal safety") != std::string::npos);
  CT_CHECK(info.value().boundary.find("no actuation") != std::string::npos);

  // An empty store has no head, and the store says so instead of inventing one.
  const auto head = store.head();
  CT_CHECK_MSG(failed_with(head, ct::ErrorCode::HeadMissing), "reported " + reported_code(head));

  CT_CHECK(store.close().has_value());
  CT_CHECK(!store.is_open());
}

CT_TEST(store_create_refuses_a_directory_that_already_holds_a_store) {
  ScratchDir scratch("store-nonempty-store");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  CT_CHECK(created.value().close().has_value());

  const auto refused = create_store(scratch.path());
  CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::StoreNotEmpty), "reported " + reported_code(refused));

  // An unrelated directory is refused for the same reason.
  ScratchDir other("store-nonempty-unrelated");
  CT_REQUIRE(write_text_file(other.child("occupant"), "unrelated content\n"));
  const auto unrelated = create_store(other.path());
  CT_CHECK_MSG(failed_with(unrelated, ct::ErrorCode::StoreNotEmpty), "reported " + reported_code(unrelated));
  // The occupant is untouched, and the only entry the refused creation added
  // is the writer-lock file it took before it inspected the directory: no store
  // record was written.
  CT_CHECK_EQ(read_text_file(other.child("occupant")), std::string("unrelated content\n"));
  for (const std::string& name : list_dir(other.path())) {
    CT_CHECK_MSG(name == "occupant" || name == "lock", "unexpected entry after a refused creation: " + name);
  }
  CT_CHECK(!file_exists(join_path(other.path(), "manifest")));
  CT_CHECK(!file_exists(join_path(other.path(), "floor")));
}

CT_TEST(store_create_refuses_a_missing_directory_when_it_must_not_create_one) {
  ScratchDir scratch("store-missing");
  const std::string missing = scratch.child("absent");
  const auto refused =
      create_store_with(store_options(missing, ct::StoreMode::ReadWrite, false), facility_reference());
  CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::StoreNotFound), "reported " + reported_code(refused));
  CT_CHECK(!directory_exists(missing));

  // Opening a store that does not exist is refused the same way.
  const auto opened = open_store(missing);
  CT_CHECK_MSG(failed_with(opened, ct::ErrorCode::StoreNotFound), "reported " + reported_code(opened));
  CT_CHECK(!directory_exists(missing));
}

CT_TEST(store_create_refuses_a_root_that_is_a_file) {
  ScratchDir scratch("store-root-file");
  const std::string file = scratch.child("a-file");
  CT_REQUIRE(write_text_file(file, "not a directory\n"));

  const auto refused = create_store_with(store_options(file, ct::StoreMode::ReadWrite, true), facility_reference());
  CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::PathNotRegular), "reported " + reported_code(refused));
  const auto opened = open_store(file);
  CT_CHECK_MSG(failed_with(opened, ct::ErrorCode::PathNotRegular), "reported " + reported_code(opened));

  // A path whose *parent* is a file cannot be a store root either.
  const auto below = create_store_with(store_options(join_path(file, "below"), ct::StoreMode::ReadWrite, true),
                                       facility_reference());
  CT_CHECK_MSG(failed_with(below, ct::ErrorCode::PathNotRegular), "reported " + reported_code(below));
}

CT_TEST(store_create_refuses_a_root_that_cannot_address_one_store) {
  ScratchDir scratch("store-root-paths");
  const std::string real = scratch.child("real");
  auto created = create_store(real);
  CT_REQUIRE(created.has_value());
  CT_CHECK(created.value().close().has_value());

  // A relative root: two processes could obtain two locks for one store.
  const auto relative =
      create_store_with(store_options("cooling-topology-relative-store", ct::StoreMode::ReadWrite, true),
                        facility_reference());
  CT_CHECK_MSG(failed_with(relative, ct::ErrorCode::PathInvalid), "reported " + reported_code(relative));

  // A root with a parent-directory component: the spelling itself is refused,
  // before any file system call, because two processes could otherwise obtain
  // two locks for one logical store.
  const std::string with_parent =
      (std::filesystem::path(scratch.path()) / ".." / "elsewhere" / "store").string();
  const auto traversing =
      create_store_with(store_options(with_parent, ct::StoreMode::ReadWrite, true), facility_reference());
  CT_CHECK_MSG(failed_with(traversing, ct::ErrorCode::PathTraversal), "reported " + reported_code(traversing));

  // A root that resolves through a reparse point: the check is exercised with a
  // real directory junction, and a helper that cannot create one fails loudly
  // rather than passing silently.
  const std::string link = scratch.child("link");
  CT_REQUIRE(create_directory_junction(link, real));
  CT_CHECK(path_is_reparse_point(link));
  const auto through_link = open_store(link);
  CT_CHECK_MSG(failed_with(through_link, ct::ErrorCode::PathTraversal), "reported " + reported_code(through_link));
  const auto below_link = open_store(join_path(link, "nested"));
  CT_CHECK_MSG(failed_with(below_link, ct::ErrorCode::PathTraversal), "reported " + reported_code(below_link));

  // The store itself is untouched and still opens through its real path.
  const auto direct = open_store(real);
  CT_REQUIRE(direct.has_value());
  CT_CHECK(!direct.value().head().has_value());
}

CT_TEST(store_publish_advances_the_head_and_the_commit_sequence) {
  ScratchDir scratch("store-publish");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  for (std::uint64_t index = 1; index <= 3; ++index) {
    const auto receipt = publish_variation(store, "pub-" + std::to_string(index), static_cast<std::size_t>(index));
    CT_REQUIRE(receipt.has_value());
    CT_CHECK_EQ(receipt.value().generation.value(), index);
    CT_CHECK_EQ(receipt.value().commit_sequence.value(), index);
    CT_CHECK_EQ(receipt.value().parent_generation.value(), index - 1);
    CT_CHECK_EQ(receipt.value().durability, ct::PublicationDurability::Durable);
    CT_CHECK(!receipt.value().replayed);
    CT_CHECK(receipt.value().head_after == receipt.value().generation);
    CT_CHECK(receipt.value().head_digest_after == receipt.value().digest);
    if (index > 1) {
      // The committed chain links each generation to the one before it.
      const auto older = store.load(ct::TopologyGeneration(index - 1));
      CT_REQUIRE(older.has_value());
      const auto newer = store.load(ct::TopologyGeneration(index));
      CT_REQUIRE(newer.has_value());
      CT_CHECK(newer.value().parent_digest() == older.value().digest());
    }
  }

  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{3});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{3});
  CT_CHECK_EQ(info.value().floor.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().retained_generations, std::size_t{3});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{3});
  CT_CHECK(staging_file_names(scratch.path()).empty());
  CT_CHECK_EQ(count_entries(scratch.child("idem")), std::size_t{3});

  const auto head = store.head();
  CT_REQUIRE(head.has_value());
  CT_CHECK_EQ(head.value().generation().value(), std::uint64_t{3});
  CT_CHECK_EQ(info.value().head_digest.to_hex(), head.value().digest().to_hex());
  CT_CHECK_EQ(head.value().parent_generation().value(), std::uint64_t{2});

  const auto second = store.load(ct::TopologyGeneration(2));
  CT_REQUIRE(second.has_value());
  CT_CHECK(head.value().parent_digest() == second.value().digest());

  // The manifest on disk is the authority the store is serving.
  CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{3});
  CT_CHECK_EQ(manifest_field(scratch.path(), "head-digest"), head.value().digest().to_hex());
  CT_CHECK_EQ(manifest_field(scratch.path(), "commit-sequence"), std::string("3"));
  CT_CHECK_EQ(floor_file_value(scratch.path()), std::uint64_t{1});
}
CT_TEST(store_head_matches_the_newest_committed_generation_and_its_manifest_digest) {
  ScratchDir scratch("store-head");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  const auto first = publish_variation(store, "head-1", 0);
  CT_REQUIRE(first.has_value());
  const auto second = publish_variation(store, "head-2", 1);
  CT_REQUIRE(second.has_value());

  const auto head = store.head();
  CT_REQUIRE(head.has_value());
  CT_CHECK_EQ(head.value().generation().value(), std::uint64_t{2});
  CT_CHECK(head.value().digest() == second.value().digest);
  CT_CHECK(head.value().parent_digest() == first.value().digest);
  CT_CHECK(head.value().facility().same_binding_as(facility_reference()));

  // The manifest that is on disk right now names exactly this head, and the
  // newest generation file on disk is exactly this generation.
  CT_CHECK_EQ(manifest_field(scratch.path(), "head"), std::string("2"));
  CT_CHECK_EQ(manifest_field(scratch.path(), "head-digest"), head.value().digest().to_hex());
  CT_CHECK_EQ(manifest_field(scratch.path(), "parent"), std::string("1"));
  const std::vector<std::string> files = generation_file_names(scratch.path());
  CT_REQUIRE(files.size() == 2);
  CT_CHECK_EQ(generation_number_of_name(files.back()), std::uint64_t{2});
  CT_CHECK_EQ(generation_number_of_name(files.front()), std::uint64_t{1});
  CT_CHECK(generation_file_path(scratch.path(), 2) == join_path(scratch.child("generations"), files.back()));

  // A generation file whose frame digest is the digest the store serves: the
  // refusal of a tampered one is proven in the adversarial and recovery suites.
  const std::string frame = read_binary_file(generation_file_path(scratch.path(), 2));
  CT_REQUIRE(frame.size() > 40);
  CT_CHECK_EQ(frame.substr(0, 8), std::string("CLDTOPG1"));
  CT_CHECK_EQ(ct::to_hex(std::string_view(frame).substr(frame.size() - 32)), head.value().digest().to_hex());
}

CT_TEST(store_load_serves_retained_generations_and_refuses_retired_ones) {
  ScratchDir scratch("store-load");
  StoreOptions options = store_options(scratch.path(), ct::StoreMode::ReadWrite, true);
  options.retained_generations = 2;
  auto created = create_store_with(options, facility_reference());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  for (std::uint64_t index = 1; index <= 4; ++index) {
    const auto receipt = publish_variation(store, "load-" + std::to_string(index), static_cast<std::size_t>(index));
    CT_REQUIRE(receipt.has_value());
  }

  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{4});
  CT_CHECK_EQ(info.value().floor.value(), std::uint64_t{3});
  CT_CHECK_EQ(info.value().retained_generations, std::size_t{2});
  CT_CHECK_EQ(floor_file_value(scratch.path()), std::uint64_t{3});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{2});
  CT_CHECK_EQ(generation_file_count(scratch.path(), 1), std::size_t{0});
  CT_CHECK_EQ(generation_file_count(scratch.path(), 4), std::size_t{1});

  const auto newest = store.load(ct::TopologyGeneration(4));
  CT_REQUIRE(newest.has_value());
  CT_CHECK_EQ(newest.value().generation().value(), std::uint64_t{4});
  const auto retained = store.load(ct::TopologyGeneration(3));
  CT_REQUIRE(retained.has_value());
  CT_CHECK(newest.value().parent_digest() == retained.value().digest());

  const auto retired = store.load(ct::TopologyGeneration(2));
  CT_CHECK_MSG(failed_with(retired, ct::ErrorCode::GenerationNotRetained), "reported " + reported_code(retired));
  const auto never = store.load(ct::TopologyGeneration(5));
  CT_CHECK_MSG(failed_with(never, ct::ErrorCode::GenerationNotRetained), "reported " + reported_code(never));
  const auto zero = store.load(ct::TopologyGeneration{});
  CT_CHECK_MSG(failed_with(zero, ct::ErrorCode::InvalidArgument), "reported " + reported_code(zero));
}

CT_TEST(store_history_is_newest_first_with_verified_chain_links) {
  ScratchDir scratch("store-history");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  for (std::uint64_t index = 1; index <= 3; ++index) {
    const auto receipt = publish_variation(store, "history-" + std::to_string(index), static_cast<std::size_t>(index));
    CT_REQUIRE(receipt.has_value());
  }

  const auto history = store.history();
  CT_REQUIRE(history.has_value());
  CT_CHECK_EQ(history.value().size(), std::size_t{3});
  const std::vector<ct::HistoryEntry>& entries = history.value();
  CT_CHECK_EQ(entries.front().generation.value(), std::uint64_t{3});
  CT_CHECK(entries.front().is_head);
  CT_CHECK(entries.front().chain_verified);
  CT_CHECK(entries.front().file_bytes > 0);
  CT_CHECK_EQ(entries.front().commit_sequence.value(), std::uint64_t{3});
  CT_CHECK_EQ(entries[1].commit_sequence.value(), std::uint64_t{2});
  CT_CHECK_EQ(entries[2].commit_sequence.value(), std::uint64_t{1});
  CT_CHECK(!entries[1].is_head);
  CT_CHECK(!entries[2].is_head);
  CT_CHECK(entries[1].chain_verified);
  CT_CHECK(entries[2].chain_verified);
  // Each entry links to the older one through its own parent binding.
  CT_CHECK_EQ(entries[0].parent_generation.value(), std::uint64_t{2});
  CT_CHECK(entries[0].parent_digest == entries[1].digest);
  CT_CHECK(entries[1].parent_digest == entries[2].digest);
  CT_CHECK_EQ(entries[2].parent_generation.value(), std::uint64_t{0});
  CT_CHECK(entries[2].parent_digest.is_zero());
}

CT_TEST(store_read_only_handle_refuses_publication_and_coexists_with_a_writer) {
  ScratchDir scratch("store-readonly");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store writer = std::move(created.value());
  const auto published = publish_variation(writer, "readonly-1", 0);
  CT_REQUIRE(published.has_value());

  // The reader opens while the writer still holds the exclusive writer lock.
  auto reader = open_store(scratch.path(), ct::StoreMode::ReadOnly);
  CT_REQUIRE(reader.has_value());
  const auto reader_info = reader.value().info();
  CT_REQUIRE(reader_info.has_value());
  CT_CHECK(!reader_info.value().writable);
  CT_CHECK(!reader_info.value().publication_allowed);
  CT_CHECK_EQ(reader_info.value().mode, ct::StoreMode::ReadOnly);
  CT_CHECK_EQ(reader_info.value().open_state, ct::StoreOpenState::Reopened);
  CT_CHECK(reader_info.value().facility.same_binding_as(facility_reference()));
  const auto head = reader.value().head();
  CT_REQUIRE(head.has_value());
  CT_CHECK(head.value().digest() == published.value().digest);

  const auto request = make_request(reader.value(), reference_variation(1), "read-only-mutation");
  CT_REQUIRE(request.has_value());
  const auto refused = reader.value().publish(request.value());
  CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::StoreReadOnly), "reported " + reported_code(refused));

  // Reading is not mutation: the writer's committed state is untouched.
  const auto writer_info = writer.info();
  CT_REQUIRE(writer_info.has_value());
  CT_CHECK_EQ(writer_info.value().head.value(), std::uint64_t{1});
  CT_CHECK_EQ(writer_info.value().commit_sequence.value(), std::uint64_t{1});
  CT_CHECK_EQ(generation_file_names(scratch.path()).size(), std::size_t{1});
  CT_CHECK_EQ(idempotency_file_names(scratch.path()).size(), std::size_t{1});
  CT_CHECK(reader.value().close().has_value());
  CT_CHECK_EQ(writer.info().value().head.value(), std::uint64_t{1});
}

CT_TEST(store_closed_handle_refuses_every_operation_except_close) {
  ScratchDir scratch("store-closed");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_REQUIRE(store.close().has_value());
  CT_CHECK(!store.is_open());

  CT_CHECK_MSG(failed_with(store.info(), ct::ErrorCode::StoreClosed), "reported " + reported_code(store.info()));
  CT_CHECK_MSG(failed_with(store.head(), ct::ErrorCode::StoreClosed), "reported " + reported_code(store.head()));
  CT_CHECK_MSG(failed_with(store.load(ct::TopologyGeneration(1)), ct::ErrorCode::StoreClosed),
               "reported " + reported_code(store.load(ct::TopologyGeneration(1))));
  CT_CHECK_MSG(failed_with(store.history(), ct::ErrorCode::StoreClosed), "reported " + reported_code(store.history()));
  CT_CHECK_MSG(failed_with(store.verify(ct::VerifyOptions{}), ct::ErrorCode::StoreClosed),
               "reported " + reported_code(store.verify(ct::VerifyOptions{})));
  CT_CHECK_MSG(failed_with(store.recover(ct::RecoveryOptions{}), ct::ErrorCode::StoreClosed),
               "reported " + reported_code(store.recover(ct::RecoveryOptions{})));

  ct::PublicationRequest request;
  request.mutation = mid("closed-mutation");
  request.attempt = *ct::AttemptOrdinal::parse(1);
  request.draft = reference_variation(0);
  CT_CHECK_MSG(failed_with(store.publish(request), ct::ErrorCode::StoreClosed),
               "reported " + reported_code(store.publish(request)));

  // Closing is idempotent, and a handle that was never opened reports the
  // absence of a store rather than a closed one.
  CT_CHECK(store.close().has_value());
  CT_CHECK(store.close().has_value());
  Store never;
  CT_CHECK(!never.is_open());
  CT_CHECK_MSG(failed_with(never.info(), ct::ErrorCode::NotInitialized), "reported " + reported_code(never.info()));
  CT_CHECK_MSG(failed_with(never.head(), ct::ErrorCode::NotInitialized), "reported " + reported_code(never.head()));
  CT_CHECK_MSG(failed_with(never.publish(request), ct::ErrorCode::NotInitialized),
               "reported " + reported_code(never.publish(request)));
  CT_CHECK(never.close().has_value());
}

CT_TEST(store_reopen_after_an_orderly_close_preserves_head_and_binding) {
  ScratchDir scratch("store-reopen");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto published = publish_variation(store, "reopen-1", 0);
  CT_REQUIRE(published.has_value());
  const ct::WriterEpoch first_epoch = store.epoch();
  CT_REQUIRE(store.close().has_value());

  auto reopened = open_store(scratch.path());
  CT_REQUIRE(reopened.has_value());
  CT_CHECK_EQ(reopened.value().open_state(), ct::StoreOpenState::Reopened);
  const auto reopened_info = reopened.value().info();
  CT_REQUIRE(reopened_info.has_value());
  CT_CHECK_EQ(reopened_info.value().store_id.str(), std::string("test-store-1"));
  CT_CHECK(reopened.value().facility().same_binding_as(facility_reference()));
  CT_CHECK_EQ(reopened.value().epoch().value(), first_epoch.value() + 1);
  CT_CHECK_EQ(reopened.value().incarnation().value(), std::uint64_t{2});
  CT_CHECK(reopened.value().is_open());
  const auto info = reopened.value().info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{1});
  CT_CHECK(info.value().head_digest == published.value().digest);
  CT_CHECK(info.value().writable);
  CT_CHECK(info.value().publication_allowed);

  const auto head = reopened.value().head();
  CT_REQUIRE(head.has_value());
  CT_CHECK(head.value().digest() == published.value().digest);

  // The next writer epoch was reserved durably before the handle was returned,
  // so a second writer is excluded even while this handle is open.
  const auto blocked = open_store(scratch.path());
  CT_CHECK_MSG(failed_with(blocked, ct::ErrorCode::StoreLocked), "reported " + reported_code(blocked));

  const auto second = publish_variation(reopened.value(), "reopen-2", 1);
  CT_REQUIRE(second.has_value());
  CT_CHECK_EQ(second.value().generation.value(), std::uint64_t{2});
  CT_CHECK_EQ(second.value().commit_sequence.value(), std::uint64_t{2});
  CT_REQUIRE(reopened.value().close().has_value());
}
CT_TEST(store_move_construction_leaves_the_moved_from_handle_safe) {
  ScratchDir scratch("store-move");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store original = std::move(created.value());
  const auto published = publish_variation(original, "move-1", 0);
  CT_REQUIRE(published.has_value());
  const std::uint64_t epoch_before = original.epoch().value();

  Store moved = std::move(original);
  CT_CHECK(moved.is_open());
  CT_CHECK(!original.is_open());
  CT_CHECK_MSG(failed_with(original.info(), ct::ErrorCode::NotInitialized),
               "reported " + reported_code(original.info()));
  CT_CHECK_MSG(failed_with(original.head(), ct::ErrorCode::NotInitialized),
               "reported " + reported_code(original.head()));
  const auto head = moved.head();
  CT_REQUIRE(head.has_value());
  CT_CHECK(head.value().digest() == published.value().digest);

  // Closing the moved-from handle must not release the live handle's lock.
  CT_CHECK(original.close().has_value());
  CT_CHECK(moved.is_open());
  CT_CHECK_EQ(moved.epoch().value(), epoch_before);
  const auto second = publish_variation(moved, "move-2", 1);
  CT_CHECK_MSG(second.has_value(), "publishing through the live handle failed: " + reported_code(second));
  if (second.has_value()) {
    CT_CHECK_EQ(second.value().generation.value(), std::uint64_t{2});
  }

  // The writer lock is still held by the live handle: a second writer is
  // excluded, and the exclusion ends only when the live handle closes.
  const auto blocked = open_store(scratch.path());
  CT_CHECK_MSG(failed_with(blocked, ct::ErrorCode::StoreLocked), "reported " + reported_code(blocked));
  CT_REQUIRE(moved.close().has_value());
  auto reopened = open_store(scratch.path());
  CT_REQUIRE(reopened.has_value());
  CT_CHECK(reopened.value().epoch().value() > epoch_before);
  CT_CHECK_EQ(reopened.value().info().value().head.value(), std::uint64_t{2});
  CT_REQUIRE(reopened.value().close().has_value());
}

CT_TEST(store_move_assignment_releases_the_replaced_handle) {
  ScratchDir first_scratch("store-move-assign-a");
  ScratchDir second_scratch("store-move-assign-b");
  auto first = create_store(first_scratch.path());
  CT_REQUIRE(first.has_value());
  Store target = std::move(first.value());
  CT_REQUIRE(publish_variation(target, "assign-1", 0).has_value());

  auto second = create_store(second_scratch.path());
  CT_REQUIRE(second.has_value());
  Store source = std::move(second.value());
  const std::uint64_t source_epoch = source.epoch().value();

  target = std::move(source);
  CT_CHECK(target.is_open());
  CT_CHECK(!source.is_open());
  CT_CHECK_MSG(failed_with(source.info(), ct::ErrorCode::NotInitialized),
               "reported " + reported_code(source.info()));
  CT_CHECK_EQ(target.epoch().value(), source_epoch);
  const auto taken_over = target.info();
  CT_REQUIRE(taken_over.has_value());
  CT_CHECK_EQ(taken_over.value().head.value(), std::uint64_t{0});
  CT_CHECK(taken_over.value().facility.same_binding_as(facility_reference()));

  // The replaced handle was destroyed, so its writer lock is gone: the store it
  // owned can be opened again, and the store that was moved in is unaffected.
  auto reclaimed = open_store(first_scratch.path());
  CT_CHECK_MSG(reclaimed.has_value(), "the replaced handle still holds its lock: " + reported_code(reclaimed));
  if (reclaimed.has_value()) {
    CT_CHECK(reclaimed.value().info().value().head.value() == std::uint64_t{1});
    CT_REQUIRE(reclaimed.value().close().has_value());
  }
  const auto still_locked = open_store(second_scratch.path());
  CT_CHECK_MSG(failed_with(still_locked, ct::ErrorCode::StoreLocked), "reported " + reported_code(still_locked));
  CT_REQUIRE(target.close().has_value());
}

CT_TEST(store_a_draft_bound_to_another_facility_is_refused) {
  ScratchDir scratch("store-facility");
  auto created = create_store(scratch.path(), "dc-1");
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  // The same identity at a different external generation is a different
  // binding, and a draft must agree with the store in kind, identity and
  // generation.
  TopologyDraft other_generation = reference_variation(0);
  other_generation.facility = facility_reference("dc-1", 8);
  const auto generation_request = make_request(store, other_generation, "wrong-facility-generation");
  CT_REQUIRE(generation_request.has_value());
  const auto generation_refused = store.publish(generation_request.value());
  CT_CHECK_MSG(failed_with(generation_refused, ct::ErrorCode::StoreMismatch),
               "reported " + reported_code(generation_refused));

  TopologyDraft other_facility = reference_variation(0);
  other_facility.facility = facility_reference("dc-2", 7);
  const auto facility_request = make_request(store, other_facility, "wrong-facility-identity");
  CT_REQUIRE(facility_request.has_value());
  const auto facility_refused = store.publish(facility_request.value());
  CT_CHECK_MSG(failed_with(facility_refused, ct::ErrorCode::StoreMismatch),
               "reported " + reported_code(facility_refused));

  // A refusal leaves no trace: no head, no committed sequence, no generation.
  CT_CHECK_EQ(manifest_head(scratch.path()), std::uint64_t{0});
  CT_CHECK_EQ(manifest_field(scratch.path(), "commit-sequence"), std::string("0"));
  CT_CHECK(generation_file_names(scratch.path()).empty());
  CT_CHECK(idempotency_file_names(scratch.path()).empty());
  CT_CHECK(staging_file_names(scratch.path()).empty());
}

CT_TEST(store_a_structurally_invalid_draft_never_publishes) {
  ScratchDir scratch("store-invalid-draft");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());

  // A second edge with the same kind and the same endpoint pair is a structural
  // defect, and it is refused before anything durable is created.
  TopologyDraft invalid = reference_variation(0);
  invalid.edges.push_back(edge("e:duplicate-supply", ct::EdgeKind::Supplies, "source:facility-water",
                               ct::PortRole::SupplyOut, "plant:chp-a", ct::PortRole::SourceIn));
  const auto request = make_request(store, invalid, "invalid-draft");
  CT_REQUIRE(request.has_value());
  const auto refused = store.publish(request.value());
  CT_CHECK_MSG(failed_with(refused, ct::ErrorCode::DuplicateEdge), "reported " + reported_code(refused));

  const auto info = store.info();
  CT_REQUIRE(info.has_value());
  CT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
  CT_CHECK_EQ(info.value().commit_sequence.value(), std::uint64_t{0});
  CT_CHECK_EQ(info.value().epoch.value(), std::uint64_t{1});
  CT_CHECK(!info.value().head.published());
  CT_CHECK_EQ(floor_file_value(scratch.path()), std::uint64_t{0});
  CT_CHECK(generation_file_names(scratch.path()).empty());
  CT_CHECK(idempotency_file_names(scratch.path()).empty());
  CT_CHECK(staging_file_names(scratch.path()).empty());

  // A valid draft still publishes afterwards.
  const auto accepted = publish_variation(store, "valid-after-invalid", 0);
  CT_REQUIRE(accepted.has_value());
  CT_CHECK_EQ(accepted.value().generation.value(), std::uint64_t{1});
  CT_CHECK_EQ(accepted.value().commit_sequence.value(), std::uint64_t{1});
}

CT_TEST(store_directory_layout_is_exact) {
  ScratchDir scratch("store-layout");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  CT_REQUIRE(publish_variation(store, "layout-1", 0).has_value());

  const std::vector<std::string> expected = {"floor", "generations", "idem", "lock", "manifest",
                                             "manifest.prev", "staging"};
  const std::vector<std::string> entries = list_dir(scratch.path());
  CT_CHECK_EQ(entries.size(), expected.size());
  CT_CHECK(entries == expected);
  // quarantine is created on demand only, and an ordinary run never creates it.
  CT_CHECK(!directory_exists(scratch.child("quarantine")));
  CT_CHECK(directory_exists(scratch.child("generations")));
  CT_CHECK_EQ(count_entries(scratch.child("generations")), std::size_t{1});
  CT_CHECK_EQ(count_entries(scratch.child("staging")), std::size_t{0});
  CT_CHECK_EQ(count_entries(scratch.child("idem")), std::size_t{1});

  // Every durable record carries its documented magic and version.
  CT_CHECK(read_text_file(scratch.child("manifest")).rfind("CLDT-MANIFEST 1\n", 0) == 0);
  CT_CHECK(read_text_file(scratch.child("manifest.prev")).rfind("CLDT-MANIFEST 1\n", 0) == 0);
  CT_CHECK(read_text_file(scratch.child("floor")).rfind("CLDT-FLOOR 1\n", 0) == 0);
  const std::vector<std::string> records = idempotency_file_names(scratch.path());
  CT_REQUIRE(records.size() == 1);
  CT_CHECK(records.front().rfind("r", 0) == 0);
  CT_CHECK(records.front().size() > 66);
  CT_CHECK(read_text_file(join_path(scratch.child("idem"), records.front())).rfind("CLDT-IDEM 1\n", 0) == 0);
  CT_CHECK_EQ(record_field_u64(join_path(scratch.child("idem"), records.front()), "generation"), std::uint64_t{1});

  // The generation file name carries the generation number and the leading
  // digest bytes, and the frame records the payload digest.
  const std::vector<std::string> files = generation_file_names(scratch.path());
  CT_REQUIRE(files.size() == 1);
  CT_CHECK_EQ(files.front().size(), std::size_t{40});
  CT_CHECK(files.front().rfind("g0000000000000001-", 0) == 0);
  CT_CHECK_EQ(files.front().substr(files.front().size() - 6), std::string(".ctgen"));
  const auto head = store.head();
  CT_REQUIRE(head.has_value());
  CT_CHECK_EQ(files.front().substr(18, 16), head.value().digest().to_hex().substr(0, 16));
}

CT_TEST(store_verify_reports_a_healthy_store) {
  ScratchDir scratch("store-verify");
  auto created = create_store(scratch.path());
  CT_REQUIRE(created.has_value());
  Store store = std::move(created.value());
  const auto first = publish_variation(store, "verify-1", 0);
  CT_REQUIRE(first.has_value());
  CT_REQUIRE(publish_variation(store, "verify-2", 1).has_value());

  const auto report = store.verify(ct::VerifyOptions{});
  CT_REQUIRE(report.has_value());
  CT_CHECK(report.value().ok());
  CT_CHECK(report.value().head_verified);
  CT_CHECK(report.value().manifest_verified);
  CT_CHECK(report.value().floor_verified);
  CT_CHECK(report.value().chain_verified);
  CT_CHECK(report.value().canonical_fixed_point_verified);
  CT_CHECK(report.value().findings.empty());
  CT_CHECK(report.value().publication_allowed);
  CT_CHECK(!report.value().recovered_state);
  CT_CHECK_EQ(report.value().store_id.str(), std::string("test-store-1"));
  const auto head = store.head();
  CT_REQUIRE(head.has_value());
  CT_CHECK(report.value().head == ct::TopologyGeneration(2));
  CT_CHECK(report.value().head_digest == head.value().digest());
  CT_CHECK_EQ(report.value().generations_present, std::size_t{2});
  CT_CHECK_EQ(report.value().generations_verified, std::size_t{2});
  CT_CHECK_EQ(report.value().staged_residue_found, std::size_t{0});
  CT_CHECK_EQ(report.value().orphan_generations_found, std::size_t{0});
  CT_CHECK_EQ(report.value().unreferenced_generations_found, std::size_t{0});
  CT_CHECK_EQ(report.value().quarantined_found, std::size_t{0});

  // A shallow verification is honest about what it did not check.
  ct::VerifyOptions shallow;
  shallow.deep = false;
  const auto shallow_report = store.verify(shallow);
  CT_REQUIRE(shallow_report.has_value());
  CT_CHECK(shallow_report.value().head_verified);
  CT_CHECK(!shallow_report.value().chain_verified);
}

}  // namespace
