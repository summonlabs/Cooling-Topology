// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Proof obligations for the canonical layer: table ordering independent of
// input order, duplicate and empty identity rejection, member-order
// independence, the generation-file frame (constructed here by hand as an
// independent reference) and every decoder rejection path, asserted by
// ErrorCode only.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;

using ct_test::Rng;

void expect_code(const ct::Error& error, ct::ErrorCode expected, const std::string& what) {
  CT_CHECK_MSG(error.code() == expected,
               what + ": expected " + std::string(ct::error_code_name(expected)) + ", observed " +
                   std::string(ct::error_code_name(error.code())) + " [" + error.to_string() + "]");
}

void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32(std::string& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void put_raw(std::string& out, const std::array<std::uint8_t, ct::Digest::kBytes>& bytes) {
  for (const std::uint8_t value : bytes) {
    out.push_back(static_cast<char>(value));
  }
}

/// The header of a canonical generation image, written byte by byte from the
/// documented encoding rules rather than produced by the encoder.
std::string hand_written_header_bytes() {
  std::string out;
  put_u16(out, ct::kCanonicalSchemaVersion);
  put_u64(out, 1);  // generation
  put_u64(out, 0);  // parent generation
  out.append(ct::Digest::kBytes, '\0');  // zero parent digest
  // facility reference: kind facility(0), identity "dc-1", generation 7
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(ct::ExternalRefKind::Facility)));
  put_u32(out, 4);
  out += "dc-1";
  put_u64(out, 7);
  // provenance: producer, origin authored(0), empty witness, no source binding,
  // no authority epoch, no evidence bindings
  const std::string producer = "dccp-cooling-topology/1.0.0";
  put_u32(out, static_cast<std::uint32_t>(producer.size()));
  out += producer;
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(ct::ProvenanceOrigin::Authored)));
  put_u32(out, 0);
  out.push_back('\0');
  put_u64(out, 0);
  put_u32(out, 0);
  return out;
}

ct::TopologyHeader header_for(const ct::TopologyDraft& draft) {
  ct::TopologyHeader header;
  header.schema_version = ct::kCanonicalSchemaVersion;
  header.generation = ct::TopologyGeneration(1);
  header.parent_generation = ct::TopologyGeneration(0);
  header.parent_digest = ct::Digest();
  header.facility = draft.facility;
  header.provenance = draft.provenance;
  return header;
}

template <class T>
void shuffle(std::vector<T>& values, Rng& rng) {
  for (std::size_t index = values.size(); index > 1; --index) {
    const std::size_t other = static_cast<std::size_t>(rng.below(static_cast<std::uint32_t>(index)));
    std::swap(values[index - 1], values[other]);
  }
}

template <class Record, class Key>
std::vector<std::string> identity_sequence(const std::vector<Record>& records, Key key) {
  std::vector<std::string> out;
  out.reserve(records.size());
  for (const Record& record : records) {
    out.push_back(std::string(key(record).value()));
  }
  return out;
}

bool is_sorted_identities(const std::vector<std::string>& values) {
  return std::is_sorted(values.begin(), values.end());
}

/// A draft with every table populated, used by the ordering proofs.
ct::TopologyDraft ordered_draft() {
  ct::TopologyDraft draft = ct_test::reference_facility();
  draft.aliases = {
      ct::Alias{ct_test::aid("alias:legacy-a"), ct_test::nid("plant:chp-a")},
      ct::Alias{ct_test::aid("alias:aaa"), ct_test::nid("plant:chp-b")},
  };
  auto first = ct::ChangeoverGroup::create(
      ct_test::cid("co:valve-b"), "Valve B",
      {ct_test::endpoint("manifold:m-a", ct::PortRole::SourceIn),
       ct_test::endpoint("manifold:m-b", ct::PortRole::SourceIn)},
      1);
  CT_REQUIRE(first.has_value());
  auto second = ct::ChangeoverGroup::create(
      ct_test::cid("co:valve-a"), "Valve A",
      {ct_test::endpoint("plant:chp-a", ct::PortRole::SourceIn),
       ct_test::endpoint("plant:chp-b", ct::PortRole::SourceIn)},
      1);
  CT_REQUIRE(second.has_value());
  draft.changeovers = {*first, *second};
  return draft;
}

/// A framed generation file built from explicitly written bytes.
std::string hand_written_frame(std::string_view payload) {
  std::string out(ct::kGenerationFileMagic);
  put_u16(out, ct::kCanonicalSchemaVersion);
  put_u16(out, 0);
  put_u64(out, static_cast<std::uint64_t>(payload.size()));
  out += payload;
  put_raw(out, ct::digest_bytes(payload).bytes());
  return out;
}

}  // namespace

CT_TEST(canonical_order_sorts_every_table_by_identity) {
  const ct::TopologyDraft draft = ordered_draft();
  auto ordered = ct::canonical_order(draft);
  CT_REQUIRE(ordered.has_value());

  const std::vector<std::string> nodes = identity_sequence(ordered->nodes, [](const ct::Node& node) -> const ct::NodeId& {
    return node.id;
  });
  const std::vector<std::string> edges = identity_sequence(ordered->edges, [](const ct::Edge& edge) -> const ct::EdgeId& {
    return edge.id;
  });
  const std::vector<std::string> groups =
      identity_sequence(ordered->groups, [](const ct::RedundancyGroup& group) -> const ct::RedundancyGroupId& {
        return group.id;
      });
  const std::vector<std::string> aliases =
      identity_sequence(ordered->aliases, [](const ct::Alias& alias) -> const ct::AliasId& { return alias.id; });
  const std::vector<std::string> changeovers = identity_sequence(
      ordered->changeovers, [](const ct::ChangeoverGroup& group) -> const ct::ChangeoverGroupId& { return group.id; });

  CT_CHECK(is_sorted_identities(nodes));
  CT_CHECK(is_sorted_identities(edges));
  CT_CHECK(is_sorted_identities(groups));
  CT_CHECK(is_sorted_identities(aliases));
  CT_CHECK(is_sorted_identities(changeovers));

  // The ordered sequence is a permutation of the input identities: compare with
  // an independent std::sort of the same keys.
  const auto same_multiset = [](std::vector<std::string> lhs, std::vector<std::string> rhs) {
    std::sort(lhs.begin(), lhs.end());
    std::sort(rhs.begin(), rhs.end());
    return lhs == rhs;
  };
  CT_CHECK(same_multiset(nodes, identity_sequence(draft.nodes, [](const ct::Node& node) -> const ct::NodeId& {
                           return node.id;
                         })));
  CT_CHECK(same_multiset(edges, identity_sequence(draft.edges, [](const ct::Edge& edge) -> const ct::EdgeId& {
                           return edge.id;
                         })));
  CT_CHECK(same_multiset(groups, identity_sequence(draft.groups, [](const ct::RedundancyGroup& group) -> const ct::RedundancyGroupId& {
                           return group.id;
                         })));
  CT_CHECK(same_multiset(aliases, identity_sequence(draft.aliases, [](const ct::Alias& alias) -> const ct::AliasId& {
                           return alias.id;
                         })));
  CT_CHECK(same_multiset(changeovers, identity_sequence(draft.changeovers, [](const ct::ChangeoverGroup& group) -> const ct::ChangeoverGroupId& {
                           return group.id;
                         })));

  // The sequence is exactly std::sort of the input keys, element for element.
  std::vector<std::string> expected_nodes = identity_sequence(draft.nodes, [](const ct::Node& node) -> const ct::NodeId& {
    return node.id;
  });
  std::sort(expected_nodes.begin(), expected_nodes.end());
  CT_CHECK(nodes == expected_nodes);

  auto bytes = ct::encode_topology(header_for(draft), ordered->nodes, ordered->edges, ordered->groups,
                                   ordered->aliases, ordered->changeovers);
  CT_REQUIRE(bytes.has_value());
  auto built = ct_test::try_build(draft);
  CT_REQUIRE(built.has_value());
  const std::string ordered_bytes = *bytes;
  const std::string built_bytes = built->canonical_bytes().value();
  if (ordered_bytes != built_bytes) {
    std::size_t offset = 0;
    while (offset < ordered_bytes.size() && offset < built_bytes.size() &&
           ordered_bytes[offset] == built_bytes[offset]) {
      ++offset;
    }
    const std::size_t begin = offset > 8u ? offset - 8u : 0u;
    ct_test::report_note("canonical bytes differ at offset " + std::to_string(offset) + " of " +
                         std::to_string(ordered_bytes.size()) + "/" + std::to_string(built_bytes.size()) +
                         " encoded=" +
                         ct::to_hex(std::string_view(ordered_bytes).substr(begin, 24u)) + " built=" +
                         ct::to_hex(std::string_view(built_bytes).substr(begin, 24u)));
  }
  CT_CHECK(ordered_bytes == built_bytes);
  CT_CHECK_EQ(ct::digest_bytes(ordered_bytes), built->digest());
}

CT_TEST(canonical_bytes_is_independent_of_input_order) {
  const std::string test_name = "canonical_bytes_is_independent_of_input_order";
  Rng rng(ct_test::case_seed(test_name));

  const ct::TopologyDraft baseline_draft = ordered_draft();
  auto baseline = ct_test::try_build(baseline_draft);
  CT_REQUIRE(baseline.has_value());
  const std::string baseline_bytes = baseline->canonical_bytes().value();
  const ct::Digest baseline_digest = baseline->digest();

  bool order_actually_changed = false;
  const std::string original_first_node = baseline_draft.nodes.front().id.str();

  for (int permutation = 0; permutation < 24; ++permutation) {
    ct::TopologyDraft shuffled = baseline_draft;
    shuffle(shuffled.nodes, rng);
    shuffle(shuffled.edges, rng);
    shuffle(shuffled.groups, rng);
    shuffle(shuffled.aliases, rng);
    shuffle(shuffled.changeovers, rng);
    for (ct::RedundancyGroup& group : shuffled.groups) {
      shuffle(group.members, rng);
    }
    for (ct::ChangeoverGroup& group : shuffled.changeovers) {
      shuffle(group.members, rng);
    }
    order_actually_changed = order_actually_changed || shuffled.nodes.front().id.str() != original_first_node;

    auto built = ct_test::try_build(shuffled);
    if (!built.has_value()) {
      ct_test::report_note("seed=" + std::to_string(ct_test::run_seed()) + " case=" + test_name +
                           " permutation=" + std::to_string(permutation) + " build " +
                           std::string(ct::error_code_name(built.error().code())));
    }
    CT_REQUIRE(built.has_value());
    const std::string bytes = built->canonical_bytes().value();
    if (bytes != baseline_bytes) {
      ct_test::report_note("seed=" + std::to_string(ct_test::run_seed()) + " case=" + test_name +
                           " permutation=" + std::to_string(permutation) + " bytes differ");
    }
    CT_CHECK_MSG(bytes == baseline_bytes, "permutation " + std::to_string(permutation));
    CT_CHECK_MSG(built->digest() == baseline_digest, "digest permutation " + std::to_string(permutation));
  }
  CT_CHECK(order_actually_changed);
  CT_CHECK_EQ(baseline_bytes.size() > 0, true);
}

CT_TEST(canonical_order_rejects_duplicate_and_empty_identities) {
  const ct::TopologyDraft base = ordered_draft();

  const auto rejects = [](const ct::TopologyDraft& draft, ct::ErrorCode expected, const char* what) {
    auto ordered = ct::canonical_order(draft);
    CT_CHECK_MSG(!ordered.has_value(), std::string("expected rejection: ") + what);
    if (!ordered.has_value()) {
      expect_code(ordered.error(), expected, what);
    }
  };

  ct::TopologyDraft duplicate_nodes = base;
  duplicate_nodes.nodes.push_back(duplicate_nodes.nodes.front());
  rejects(duplicate_nodes, ct::ErrorCode::DuplicateIdentifier, "duplicate node identity");

  ct::TopologyDraft duplicate_edges = base;
  duplicate_edges.edges.push_back(duplicate_edges.edges.front());
  rejects(duplicate_edges, ct::ErrorCode::DuplicateIdentifier, "duplicate edge identity");

  ct::TopologyDraft duplicate_groups = base;
  duplicate_groups.groups.push_back(duplicate_groups.groups.front());
  rejects(duplicate_groups, ct::ErrorCode::DuplicateIdentifier, "duplicate group identity");

  ct::TopologyDraft duplicate_aliases = base;
  duplicate_aliases.aliases.push_back(duplicate_aliases.aliases.front());
  rejects(duplicate_aliases, ct::ErrorCode::DuplicateIdentifier, "duplicate alias identity");

  ct::TopologyDraft duplicate_changeovers = base;
  duplicate_changeovers.changeovers.push_back(duplicate_changeovers.changeovers.front());
  rejects(duplicate_changeovers, ct::ErrorCode::DuplicateIdentifier, "duplicate changeover identity");

  ct::TopologyDraft empty_node = base;
  empty_node.nodes.push_back(ct::Node());
  rejects(empty_node, ct::ErrorCode::MalformedIdentifier, "empty node identity");

  ct::TopologyDraft empty_edge = base;
  empty_edge.edges.push_back(ct::Edge());
  rejects(empty_edge, ct::ErrorCode::MalformedIdentifier, "empty edge identity");

  ct::TopologyDraft empty_group = base;
  empty_group.groups.push_back(ct::RedundancyGroup());
  rejects(empty_group, ct::ErrorCode::MalformedIdentifier, "empty group identity");

  ct::TopologyDraft empty_alias = base;
  empty_alias.aliases.push_back(ct::Alias());
  rejects(empty_alias, ct::ErrorCode::MalformedIdentifier, "empty alias identity");

  ct::TopologyDraft empty_changeover = base;
  empty_changeover.changeovers.push_back(ct::ChangeoverGroup());
  rejects(empty_changeover, ct::ErrorCode::MalformedIdentifier, "empty changeover identity");

  // The same identities reach the same codes through full validation.
  auto built_duplicate = ct_test::try_build(duplicate_nodes);
  CT_REQUIRE(!built_duplicate.has_value());
  expect_code(built_duplicate.error(), ct::ErrorCode::DuplicateIdentifier, "duplicate node through create");

  auto built_empty = ct_test::try_build(empty_node);
  CT_REQUIRE(!built_empty.has_value());
  expect_code(built_empty.error(), ct::ErrorCode::MalformedIdentifier, "empty node through create");
}

CT_TEST(canonical_member_and_reference_order_do_not_change_bytes) {
  const ct::TopologyDraft base = ordered_draft();
  auto baseline = ct::canonical_order(base);
  CT_REQUIRE(baseline.has_value());
  auto baseline_bytes = ct::encode_topology(header_for(base), baseline->nodes, baseline->edges, baseline->groups,
                                            baseline->aliases, baseline->changeovers);
  CT_REQUIRE(baseline_bytes.has_value());

  ct::TopologyDraft reversed = base;
  for (ct::RedundancyGroup& group : reversed.groups) {
    std::reverse(group.members.begin(), group.members.end());
  }
  for (ct::ChangeoverGroup& group : reversed.changeovers) {
    std::reverse(group.members.begin(), group.members.end());
  }
  auto reversed_order = ct::canonical_order(reversed);
  CT_REQUIRE(reversed_order.has_value());
  auto reversed_bytes =
      ct::encode_topology(header_for(reversed), reversed_order->nodes, reversed_order->edges, reversed_order->groups,
                          reversed_order->aliases, reversed_order->changeovers);
  CT_REQUIRE(reversed_bytes.has_value());
  CT_CHECK(*reversed_bytes == *baseline_bytes);
  CT_CHECK(ct::digest_bytes(*reversed_bytes) == ct::digest_bytes(*baseline_bytes));

  // Reversed group members must survive canonical ordering sorted, not reversed.
  for (const ct::RedundancyGroup& group : reversed_order->groups) {
    std::vector<std::string> names;
    for (const ct::RedundancyMember& member : group.members) {
      names.push_back(member.node.str());
    }
    CT_CHECK_MSG(std::is_sorted(names.begin(), names.end()), "group members sorted after canonical_order");
  }

  // Node reference order is equally without meaning.
  ct::TopologyDraft references = base;
  for (ct::Node& node : references.nodes) {
    std::reverse(node.references.begin(), node.references.end());
  }
  auto reference_order = ct::canonical_order(references);
  CT_REQUIRE(reference_order.has_value());
  auto reference_bytes = ct::encode_topology(header_for(references), reference_order->nodes, reference_order->edges,
                                             reference_order->groups, reference_order->aliases,
                                             reference_order->changeovers);
  CT_REQUIRE(reference_bytes.has_value());
  CT_CHECK(*reference_bytes == *baseline_bytes);

  // Through the public constructor the digests agree as well.
  auto built_base = ct_test::try_build(base);
  auto built_reversed = ct_test::try_build(reversed);
  CT_REQUIRE(built_base.has_value());
  CT_REQUIRE(built_reversed.has_value());
  CT_CHECK(built_base->digest() == built_reversed->digest());
  CT_CHECK(built_base->canonical_bytes().value() == built_reversed->canonical_bytes().value());
}

CT_TEST(canonical_empty_topology_encoding_matches_hand_written_bytes) {
  ct::TopologyHeader header;
  header.generation = ct::TopologyGeneration(1);
  header.facility = ct_test::extref("dc-1", ct::ExternalRefKind::Facility, 7);
  auto provenance = ct::Provenance::create("dccp-cooling-topology/1.0.0", ct::ProvenanceOrigin::Authored, "",
                                           std::nullopt, ct::AuthorityEpoch(), {});
  CT_REQUIRE(provenance.has_value());
  header.provenance = *provenance;

  std::string expected = hand_written_header_bytes();
  put_u32(expected, 0);
  put_u32(expected, 0);
  put_u32(expected, 0);
  put_u32(expected, 0);
  put_u32(expected, 0);

  auto encoded = ct::encode_topology(header, {}, {}, {}, {}, {});
  CT_REQUIRE(encoded.has_value());
  CT_CHECK_MSG(*encoded == expected,
               "hand-written encoding differs: " + ct::to_hex(*encoded) + " vs " + ct::to_hex(expected));

  auto decoded = ct::decode_topology(expected);
  CT_REQUIRE(decoded.has_value());
  CT_CHECK_EQ(decoded->header.schema_version, ct::kCanonicalSchemaVersion);
  CT_CHECK_EQ(decoded->header.generation.value(), static_cast<std::uint64_t>(1));
  CT_CHECK_EQ(decoded->header.parent_generation.value(), static_cast<std::uint64_t>(0));
  CT_CHECK(decoded->header.parent_digest.is_zero());
  CT_CHECK_EQ(decoded->header.facility.identity, std::string("dc-1"));
  CT_CHECK_EQ(decoded->header.facility.generation.value(), static_cast<std::uint64_t>(7));
  CT_CHECK(decoded->tables.nodes.empty());
  CT_CHECK(decoded->tables.edges.empty());
  CT_CHECK(decoded->tables.groups.empty());
  CT_CHECK(decoded->tables.aliases.empty());
  CT_CHECK(decoded->tables.changeovers.empty());
  CT_CHECK_EQ(decoded->header.provenance.producer, std::string("dccp-cooling-topology/1.0.0"));
  CT_CHECK_EQ(ct::digest_bytes(expected), ct::digest_bytes(*encoded));
}

CT_TEST(canonical_generation_frame_round_trips) {
  auto built = ct_test::try_build(ct_test::reference_facility());
  CT_REQUIRE(built.has_value());
  const std::string payload = built->canonical_bytes().value();

  auto framed = ct::encode_generation_file(payload);
  CT_REQUIRE(framed.has_value());
  // Independent frame construction: magic | schema | reserved | length | payload | digest.
  CT_CHECK_MSG(*framed == hand_written_frame(payload), "frame layout differs from the hand-written reference");

  auto decoded = ct::decode_generation_file(*framed);
  CT_REQUIRE(decoded.has_value());
  CT_CHECK_EQ(decoded->schema_version, ct::kCanonicalSchemaVersion);
  CT_CHECK_EQ(decoded->payload, payload);
  CT_CHECK(decoded->payload_digest == ct::digest_bytes(payload));
  CT_CHECK(*framed == hand_written_frame(decoded->payload));

  auto hand_decoded = ct::decode_generation_file(hand_written_frame(payload));
  CT_REQUIRE(hand_decoded.has_value());
  CT_CHECK_EQ(hand_decoded->payload, payload);

  auto empty = ct::encode_generation_file(std::string_view());
  CT_REQUIRE(!empty.has_value());
  expect_code(empty.error(), ct::ErrorCode::EmptyInput, "empty payload framing");
}

CT_TEST(canonical_generation_frame_rejects_each_defect) {
  auto built = ct_test::try_build(ct_test::reference_facility());
  CT_REQUIRE(built.has_value());
  const std::string payload = built->canonical_bytes().value();
  const std::string frame = hand_written_frame(payload);
  CT_CHECK_EQ(frame.size(), payload.size() + 52u);

  struct Case {
    const char* label;
    std::string bytes;
    ct::ErrorCode code;
  };
  std::string wrong_magic = frame;
  wrong_magic[0] = 'X';
  std::string reserved = frame;
  reserved[10] = 1;
  std::string schema_high = frame;
  schema_high[8] = 2;  // schema version 2
  std::string schema_zero = frame;
  schema_zero[8] = 0;
  const auto patch_length = [](const std::string& source, std::uint64_t length) {
    std::string out = source;
    for (int index = 0; index < 8; ++index) {
      out[12 + static_cast<std::size_t>(index)] =
          static_cast<char>((length >> (index * 8)) & 0xFFu);
    }
    return out;
  };
  std::string short_length = patch_length(frame, payload.size() - 1u);
  std::string long_length = patch_length(frame, payload.size() + 1u);
  std::string flipped_payload = frame;
  flipped_payload[20 + payload.size() / 2u] = static_cast<char>(flipped_payload[20 + payload.size() / 2u] ^ 0x01);
  std::string flipped_digest = frame;
  flipped_digest[frame.size() - 1u] = static_cast<char>(flipped_digest[frame.size() - 1u] ^ 0x01);
  std::string header_only = frame.substr(0, 20);

  const std::vector<Case> cases = {
      {"wrong magic", wrong_magic, ct::ErrorCode::MalformedRecord},
      {"non-zero reserved field", reserved, ct::ErrorCode::MalformedRecord},
      {"unsupported schema version", schema_high, ct::ErrorCode::UnsupportedSchemaVersion},
      {"schema version zero", schema_zero, ct::ErrorCode::UnsupportedSchemaVersion},
      {"payload length one short", short_length, ct::ErrorCode::CountMismatch},
      {"payload length one long", long_length, ct::ErrorCode::CountMismatch},
      {"digest mismatch", flipped_payload, ct::ErrorCode::DigestMismatch},
      {"recorded digest edited", flipped_digest, ct::ErrorCode::DigestMismatch},
      {"truncated frame", header_only, ct::ErrorCode::TruncatedInput},
      {"truncated by one byte", frame.substr(0, frame.size() - 1u), ct::ErrorCode::CountMismatch},
      {"trailing byte", frame + "x", ct::ErrorCode::CountMismatch},
      {"empty frame", std::string(), ct::ErrorCode::TruncatedInput},
  };
  for (const Case& item : cases) {
    auto decoded = ct::decode_generation_file(item.bytes);
    CT_CHECK_MSG(!decoded.has_value(), std::string("expected rejection: ") + item.label);
    if (!decoded.has_value()) {
      expect_code(decoded.error(), item.code, item.label);
    }
  }
}

CT_TEST(canonical_topology_decode_rejects_edited_frames) {
  auto built = ct_test::try_build(ct_test::reference_facility());
  CT_REQUIRE(built.has_value());
  const std::string payload = built->canonical_bytes().value();
  const std::string frame = hand_written_frame(payload);

  auto decoded = ct::Topology::decode(frame);
  CT_REQUIRE(decoded.has_value());
  CT_CHECK(decoded->digest() == ct::digest_bytes(payload));
  CT_CHECK(decoded->canonical_bytes().value() == payload);
  CT_CHECK(decoded->node_count() == built->node_count());
  CT_CHECK(decoded->edge_count() == built->edge_count());
  CT_CHECK(decoded->group_count() == built->group_count());

  auto empty = ct::Topology::decode(std::string_view());
  CT_REQUIRE(!empty.has_value());
  expect_code(empty.error(), ct::ErrorCode::EmptyInput, "empty generation file");

  // One byte flipped in the middle of the payload.
  std::string middle = frame;
  const std::size_t middle_index = 20 + payload.size() / 2u;
  middle[middle_index] = static_cast<char>(middle[middle_index] ^ 0x01);
  auto edited_middle = ct::Topology::decode(middle);
  CT_REQUIRE(!edited_middle.has_value());
  expect_code(edited_middle.error(), ct::ErrorCode::DigestMismatch, "payload byte flipped");

  // One byte flipped in the digest region.
  std::string digest_edit = frame;
  digest_edit[frame.size() - 17u] = static_cast<char>(digest_edit[frame.size() - 17u] ^ 0x80);
  auto edited_digest = ct::Topology::decode(digest_edit);
  CT_REQUIRE(!edited_digest.has_value());
  expect_code(edited_digest.error(), ct::ErrorCode::DigestMismatch, "digest byte flipped");

  // Truncated payload: the frame is intact but the payload is one byte short.
  std::string truncated_payload = payload.substr(0, payload.size() - 1u);
  auto rebuilt_frame = ct::encode_generation_file(truncated_payload);
  CT_REQUIRE(rebuilt_frame.has_value());
  auto truncated = ct::Topology::decode(*rebuilt_frame);
  CT_REQUIRE(!truncated.has_value());
  expect_code(truncated.error(), ct::ErrorCode::TruncatedInput, "payload one byte short");
}

CT_TEST(canonical_topology_decode_rejects_non_canonical_payload) {
  auto built = ct_test::try_build(ct_test::reference_facility());
  CT_REQUIRE(built.has_value());
  const std::string payload = built->canonical_bytes().value();

  auto decoded = ct::decode_topology(payload);
  CT_REQUIRE(decoded.has_value());
  CT_REQUIRE(!decoded->tables.groups.empty());
  CT_REQUIRE(decoded->tables.groups.front().members.size() >= 2u);

  // Mutate a decoded field that canonical order normalizes: the payload stays
  // structurally valid, but it is no longer a canonical image of its state.
  ct::DecodedGeneration mutated = *decoded;
  std::reverse(mutated.tables.groups.front().members.begin(), mutated.tables.groups.front().members.end());
  auto mutated_bytes = ct::encode_topology(mutated.header, mutated.tables.nodes, mutated.tables.edges,
                                           mutated.tables.groups, mutated.tables.aliases,
                                           mutated.tables.changeovers);
  CT_REQUIRE(mutated_bytes.has_value());
  CT_CHECK_MSG(*mutated_bytes != payload, "the mutation must change the bytes");

  auto frame = ct::encode_generation_file(*mutated_bytes);
  CT_REQUIRE(frame.has_value());
  auto rejected = ct::Topology::decode(*frame);
  CT_REQUIRE(!rejected.has_value());
  expect_code(rejected.error(), ct::ErrorCode::IntegrityFailure, "non-canonical re-encoding");

  // The pristine payload framed the same way is accepted, so the rejection is
  // caused by the mutation and not by the framing.
  auto pristine_frame = ct::encode_generation_file(payload);
  CT_REQUIRE(pristine_frame.has_value());
  CT_CHECK(ct::Topology::decode(*pristine_frame).has_value());
}

CT_TEST(canonical_decode_never_allocates_from_a_declared_count) {
  const std::string header = hand_written_header_bytes();

  std::string huge = header;
  put_u32(huge, 0xFFFFFFFFu);
  auto huge_decoded = ct::decode_topology(huge);
  CT_REQUIRE(!huge_decoded.has_value());
  CT_CHECK_MSG(huge_decoded.error().code() == ct::ErrorCode::LimitExceeded ||
                   huge_decoded.error().code() == ct::ErrorCode::CountMismatch,
               "huge node count must be refused with a shape or bound error, observed " +
                   std::string(ct::error_code_name(huge_decoded.error().code())));
  expect_code(huge_decoded.error(), ct::ErrorCode::LimitExceeded, "node count 0xFFFFFFFF");

  // A count exactly at the documented bound but with no bytes behind it must be
  // refused by the remaining-input check, before any allocation.
  std::string at_bound = header;
  put_u32(at_bound, static_cast<std::uint32_t>(ct::limits::kMaxNodeCount));
  auto at_bound_decoded = ct::decode_topology(at_bound);
  CT_REQUIRE(!at_bound_decoded.has_value());
  expect_code(at_bound_decoded.error(), ct::ErrorCode::CountMismatch, "node count at the bound, no payload");

  std::string small = header;
  put_u32(small, 2);
  auto small_decoded = ct::decode_topology(small);
  CT_REQUIRE(!small_decoded.has_value());
  expect_code(small_decoded.error(), ct::ErrorCode::CountMismatch, "node count 2, no payload");

  // The same trick on a later table: a valid empty generation declaring two
  // edges must be refused rather than trusted.
  std::string edges = header;
  put_u32(edges, 0);
  put_u32(edges, 2);
  auto edges_decoded = ct::decode_topology(edges);
  CT_REQUIRE(!edges_decoded.has_value());
  expect_code(edges_decoded.error(), ct::ErrorCode::CountMismatch, "edge count 2, no payload");
}
