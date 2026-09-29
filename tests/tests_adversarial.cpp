// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial obligations: malformed, truncated, corrupt, oversized,
// pathologically ordered and deliberately hostile input. Every case must be
// rejected with a stable code or answered within a documented bound - never a
// crash, a hang, an unbounded allocation or a partial answer presented as
// complete.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace ct_test;
namespace ct = dccp::cooling_topology;

ct::TopologyDraft wide_facility(std::uint32_t width) {
  ct::TopologyDraft draft = base_draft("dc-wide");
  draft.nodes.push_back(source_node("source:fw"));
  draft.nodes.push_back(loop_node("loop:l1"));
  draft.edges.push_back(edge("e:src-loop", ct::EdgeKind::Supplies, "source:fw", ct::PortRole::SupplyOut,
                             "loop:l1", ct::PortRole::SourceIn));
  draft.nodes.push_back(manifold_node("manifold:m0"));
  draft.edges.push_back(edge("e:loop-m0", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut,
                             "manifold:m0", ct::PortRole::SourceIn));
  draft.edges.push_back(edge("e:m0-in", ct::EdgeKind::Contains, "loop:l1", ct::PortRole::Container,
                             "manifold:m0", ct::PortRole::Contained));
  for (std::uint32_t index = 0; index < width; ++index) {
    const std::string branch = "branch:b" + std::to_string(index);
    draft.nodes.push_back(branch_node(branch));
    draft.edges.push_back(edge("e:" + branch + "-feed", ct::EdgeKind::Supplies, "manifold:m0",
                               ct::PortRole::SupplyOut, branch, ct::PortRole::SourceIn));
    draft.edges.push_back(edge("e:" + branch + "-in", ct::EdgeKind::Contains, "manifold:m0",
                               ct::PortRole::Container, branch, ct::PortRole::Contained));
  }
  return draft;
}

ct::TopologyDraft deep_facility(std::uint32_t depth) {
  ct::TopologyDraft draft = base_draft("dc-deep");
  draft.nodes.push_back(source_node("chain:0"));
  for (std::uint32_t index = 1; index <= depth; ++index) {
    draft.nodes.push_back(loop_node("chain:" + std::to_string(index)));
    draft.edges.push_back(edge("e:chain-" + std::to_string(index), ct::EdgeKind::Supplies,
                               "chain:" + std::to_string(index - 1), ct::PortRole::SupplyOut,
                               "chain:" + std::to_string(index), ct::PortRole::SourceIn));
  }
  return draft;
}

}  // namespace

CT_TEST(adversarial_deep_and_wide_graphs_stay_within_bounds) {
  {
    // A 4000 element supply chain is a legal DAG and must build, encode,
    // traverse and re-validate without unbounded recursion: every traversal in
    // the library is iterative.
    ct::TopologyDraft draft = deep_facility(4000);
    const auto topology = try_build(draft);
    CT_REQUIRE(topology.has_value());
    auto bytes = topology->canonical_bytes();
    CT_REQUIRE(bytes.has_value());
    auto framed = ct::encode_generation_file(*bytes);
    CT_REQUIRE(framed.has_value());
    auto decoded = ct::Topology::decode(*framed);
    CT_CHECK(decoded.has_value());
    // The default depth bound is documented, so a 4000 element chain is
    // reported as a truncated prefix rather than silently as a complete answer.
    auto bounded = ct::downstream_of(*topology, nid("chain:0"));
    CT_REQUIRE(bounded.has_value());
    CT_CHECK_EQ(bounded->elements.size(), ct::limits::kMaxQueryDepth);
    CT_CHECK(bounded->truncated);
    // With the depth bound raised, the whole closure is reached: the traversal is
    // iterative, so 4000 levels neither overflow the stack nor stop early.
    ct::QueryOptions deep;
    deep.max_depth = 8000;
    auto reached = ct::downstream_of(*topology, nid("chain:0"), deep);
    CT_REQUIRE(reached.has_value());
    CT_CHECK_EQ(reached->elements.size(), std::size_t{4000});
    CT_CHECK(reached->truncated == false);
    auto upstream = ct::upstream_of(*topology, nid("chain:4000"), deep);
    CT_REQUIRE(upstream.has_value());
    CT_CHECK_EQ(upstream->elements.size(), std::size_t{4000});
  }
  {
    ct::TopologyDraft draft = wide_facility(4000);
    const auto topology = try_build(draft);
    CT_REQUIRE(topology.has_value());
    auto reached = ct::downstream_of(*topology, nid("manifold:m0"));
    CT_REQUIRE(reached.has_value());
    CT_CHECK_EQ(reached->elements.size(), std::size_t{4000});
  }
  {
    ct::Topology topology = build(deep_facility(2000));
    ct::QueryOptions options;
    options.max_depth = 10;
    auto bounded = ct::downstream_of(topology, nid("chain:0"), options);
    CT_REQUIRE(bounded.has_value());
    CT_CHECK_EQ(bounded->elements.size(), std::size_t{10});
    CT_CHECK(bounded->truncated);
    ct::QueryOptions wide_options;
    wide_options.max_results = 25;
    auto truncated = ct::downstream_of(topology, nid("chain:0"), wide_options);
    CT_REQUIRE(truncated.has_value());
    CT_CHECK_EQ(truncated->elements.size(), std::size_t{25});
    CT_CHECK(truncated->truncated);
  }
}

CT_TEST(adversarial_table_bounds_reject_before_allocation) {
  {
    ct::TopologyDraft draft = base_draft("dc-over");
    draft.nodes.resize(ct::limits::kMaxNodeCount + 1);
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_REQUIRE(report.primary() != nullptr);
    CT_CHECK_EQ(report.primary()->code, ct::ErrorCode::LimitExceeded);
  }
  {
    ct::TopologyDraft draft = base_draft("dc-over");
    draft.edges.resize(ct::limits::kMaxEdgeCount + 1);
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_REQUIRE(report.primary() != nullptr);
    CT_CHECK_EQ(report.primary()->code, ct::ErrorCode::LimitExceeded);
  }
  {
    ct::TopologyDraft draft = base_draft("dc-over");
    draft.groups.resize(ct::limits::kMaxRedundancyGroupCount + 1);
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_REQUIRE(report.primary() != nullptr);
    CT_CHECK_EQ(report.primary()->code, ct::ErrorCode::LimitExceeded);
  }
}

CT_TEST(adversarial_crafted_counts_never_drive_an_allocation) {
  const ct::Topology topology = build(reference_facility());
  auto payload = topology.canonical_bytes();
  CT_REQUIRE(payload.has_value());
  // Every seventh four-byte window of the payload is overwritten with a maximal
  // count in turn. The decoder must reject or accept deterministically - it must
  // never allocate from a declared count, run unboundedly or crash.
  const std::string original = *payload;
  for (std::size_t offset = 0; offset + 4 <= original.size() && offset < 4096; offset += 7) {
    std::string mutated = original;
    mutated[offset] = static_cast<char>(0xFF);
    mutated[offset + 1] = static_cast<char>(0xFF);
    mutated[offset + 2] = static_cast<char>(0xFF);
    mutated[offset + 3] = static_cast<char>(0xFF);
    auto framed = ct::encode_generation_file(mutated);
    CT_REQUIRE(framed.has_value());
    const auto decoded = ct::Topology::decode(*framed);
    if (decoded.has_value()) {
      CT_CHECK_EQ(decoded->node_count(), topology.node_count());
    } else {
      CT_CHECK(decoded.error().code() != ct::ErrorCode::Ok);
    }
  }
}

CT_TEST(adversarial_truncation_sweep_and_single_byte_corruption) {
  const ct::Topology topology = build(reference_facility());
  auto payload = topology.canonical_bytes();
  CT_REQUIRE(payload.has_value());
  auto framed = ct::encode_generation_file(*payload);
  CT_REQUIRE(framed.has_value());

  for (std::size_t length = 0; length < framed->size(); ++length) {
    const auto decoded = ct::Topology::decode(std::string_view(*framed).substr(0, length));
    CT_CHECK_MSG(decoded.has_value() == false,
                 "prefix of length " + std::to_string(length) + " was accepted");
  }
  CT_CHECK(ct::Topology::decode(*framed).has_value());

  Rng rng(case_seed("adversarial_truncation_sweep_and_single_byte_corruption"));
  for (int round = 0; round < 400; ++round) {
    const std::size_t position = rng.below(static_cast<std::uint32_t>(framed->size()));
    std::string mutated = *framed;
    const char previous = mutated[position];
    mutated[position] = static_cast<char>(previous ^ static_cast<char>(1 + rng.below(255)));
    const auto decoded = ct::Topology::decode(mutated);
    if (decoded.has_value()) {
      report_note("byte at " + std::to_string(position) + " could be changed without detection");
      CT_CHECK(false);
    }
  }
  std::string extended = *framed;
  extended.push_back('\0');
  CT_CHECK(ct::Topology::decode(extended).has_value() == false);
  std::string wrong_magic = *framed;
  wrong_magic[0] = 'X';
  const auto rejected = ct::Topology::decode(wrong_magic);
  CT_REQUIRE(rejected.has_value() == false);
  CT_CHECK_EQ(rejected.error().code(), ct::ErrorCode::MalformedRecord);
  std::string wrong_schema = *framed;
  wrong_schema[8] = static_cast<char>(9);
  const auto schema_rejected = ct::Topology::decode(wrong_schema);
  CT_REQUIRE(schema_rejected.has_value() == false);
  CT_CHECK_EQ(schema_rejected.error().code(), ct::ErrorCode::UnsupportedSchemaVersion);
  std::string reserved = *framed;
  reserved[10] = static_cast<char>(1);
  const auto reserved_rejected = ct::Topology::decode(reserved);
  CT_REQUIRE(reserved_rejected.has_value() == false);
  CT_CHECK_EQ(reserved_rejected.error().code(), ct::ErrorCode::MalformedRecord);
  const auto empty = ct::Topology::decode("");
  CT_REQUIRE(empty.has_value() == false);
  CT_CHECK_EQ(empty.error().code(), ct::ErrorCode::EmptyInput);
}

CT_TEST(adversarial_pathologically_ordered_identities) {
  ct::TopologyDraft draft = base_draft("dc-order");
  const char* names[] = {"zzz", "yyy", "xxx", "node:10", "node:9", "node:1", "node:0"};
  draft.nodes.push_back(source_node(names[0]));
  draft.nodes.push_back(loop_node(names[1]));
  for (std::size_t index = 2; index < 7; ++index) {
    draft.nodes.push_back(manifold_node(names[index]));
    draft.edges.push_back(edge(std::string("e:") + names[index], ct::EdgeKind::Contains, names[1],
                               ct::PortRole::Container, names[index], ct::PortRole::Contained));
  }
  draft.edges.push_back(edge("e:feed", ct::EdgeKind::Supplies, names[0], ct::PortRole::SupplyOut, names[1],
                             ct::PortRole::SourceIn));
  const ct::Topology topology = build(draft);
  std::vector<std::string> ids;
  for (const ct::Node& node : topology.nodes()) {
    ids.push_back(node.id.str());
  }
  std::vector<std::string> expected = ids;
  std::sort(expected.begin(), expected.end());
  CT_CHECK_EQ(ids, expected);
  for (const std::string& id : ids) {
    CT_CHECK(topology.resolve(ct::NodeId::parse(id).value()).has_value());
  }
}

CT_TEST(adversarial_duplicate_edges_and_self_loops) {
  {
    ct::TopologyDraft draft = single_path_facility();
    for (int round = 0; round < 32; ++round) {
      draft.edges.push_back(edge("e:dup-" + std::to_string(round), ct::EdgeKind::Supplies, "plant:p1",
                                 ct::PortRole::SupplyOut, "loop:l1", ct::PortRole::SourceIn));
    }
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_REQUIRE(report.primary() != nullptr);
    CT_CHECK_EQ(report.primary()->code, ct::ErrorCode::DuplicateEdge);
  }
  for (std::size_t index = 0; index < 9; ++index) {
    const auto port = static_cast<ct::PortRole>(index);
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:self", ct::EdgeKind::Supplies, "loop:l1", port, "loop:l1", port));
    const ct::ValidationReport report = ct::Topology::validate_draft(draft);
    CT_REQUIRE(report.primary() != nullptr);
    CT_CHECK_EQ(report.primary()->code, ct::ErrorCode::SelfEdge);
  }
}

CT_TEST(adversarial_malformed_references_are_refused_not_repaired) {
  const auto accepted = [](std::string_view identity) {
    return ct::ExternalRef::create(ct::ExternalRefKind::Facility, std::string(identity), ct::ExternalGeneration())
        .has_value();
  };
  CT_CHECK(accepted("dc-1"));
  CT_CHECK(accepted(std::string("a\0b", 3)) == false);
  CT_CHECK(accepted(std::string(ct::limits::kMaxExternalIdentityBytes + 1, 'x')) == false);
  CT_CHECK(accepted("") == false);
  CT_CHECK(accepted("\xC3\x28") == false);
  const auto padded = ct::ExternalRef::create(ct::ExternalRefKind::Facility, "  Dc-1  ", ct::ExternalGeneration());
  CT_REQUIRE(padded.has_value());
  CT_CHECK_EQ(padded->identity, std::string("  Dc-1  "));
  const auto plain = ct::ExternalRef::create(ct::ExternalRefKind::Facility, "dc-1", ct::ExternalGeneration());
  CT_REQUIRE(plain.has_value());
  CT_CHECK(padded->same_binding_as(*plain) == false);
}

CT_TEST(adversarial_counter_and_generation_boundaries) {
  CT_CHECK(ct::TopologyGeneration(UINT64_MAX).next().has_value() == false);
  CT_CHECK(ct::WriterEpoch(UINT64_MAX).next().has_value() == false);
  CT_CHECK(ct::WriterIncarnation(UINT64_MAX).next().has_value() == false);
  CT_CHECK(ct::CommitSequence(UINT64_MAX).next().has_value() == false);
  CT_CHECK(ct::DraftRevision(UINT64_MAX).next().has_value() == false);
  CT_CHECK_EQ(ct::TopologyGeneration().published(), false);
  CT_CHECK_EQ(ct::AttemptOrdinal::parse(0).has_value(), false);
  CT_CHECK_EQ(ct::AttemptOrdinal::parse(1).value().value(), std::uint32_t{1});
  CT_CHECK_EQ(ct::AttemptOrdinal::parse(UINT32_MAX).value().value(), UINT32_MAX);

  ct::TopologyDraft draft = single_path_facility();
  CT_CHECK(ct::Topology::create(ct::TopologyGeneration(0), ct::TopologyGeneration(), ct::Digest(), draft)
               .has_value() == false);
  const auto first = ct::Topology::create_first(draft);
  CT_REQUIRE(first.has_value());
  CT_CHECK(ct::Topology::create(ct::TopologyGeneration(1), ct::TopologyGeneration(1), first->digest(), draft)
               .has_value() == false);
  CT_CHECK(ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(1), ct::Digest(), draft)
               .has_value() == false);
  CT_CHECK(ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(), first->digest(), draft)
               .has_value() == false);
}

CT_TEST(adversarial_query_bounds_and_unknown_subjects) {
  const ct::Topology topology = build(reference_facility());
  const auto unknown = ct::NodeId::parse("does:not-exist").value();
  CT_CHECK(ct::upstream_of(topology, unknown).has_value() == false);
  CT_CHECK(ct::downstream_of(topology, unknown).has_value() == false);
  CT_CHECK(ct::possible_supply_paths(topology, unknown, nid("sink:rack-a1")).has_value() == false);
  CT_CHECK(ct::blast_radius(topology, unknown).has_value() == false);
  CT_CHECK(ct::sink_service_report(topology, nid("branch:b-a1")).has_value() == false);
  CT_CHECK(ct::zone_report(topology, nid("branch:b-a1")).has_value() == false);
  CT_CHECK(ct::common_dependencies(topology, {}).has_value() == false);
  CT_CHECK(ct::redundancy_group_report(topology, gid("group:absent")).has_value() == false);

  ct::QueryOptions options;
  options.max_paths = 1;
  auto paths = ct::possible_supply_paths(topology, nid("source:facility-water"), nid("sink:rack-a1"), options);
  CT_REQUIRE(paths.has_value());
  CT_CHECK(paths->paths.size() <= 1);
}

CT_TEST(adversarial_import_hostile_documents) {
  const std::string valid =
      "facility facility:dc-1@7\n"
      "provenance producer=dccp-cooling-topology/1.0.0 origin=authored witness=\"fixture\"\n"
      "node plant:p1 cooling_plant kind=chiller_plant\n";
  CT_CHECK(ct::parse_import(valid).has_value());

  const char* hostile[] = {
      "",
      "\n\n",
      "facility\n",
      "provenance producer=x origin=authored witness=y\n",
      "facility facility:dc-1\nfacility facility:dc-1\nprovenance producer=x origin=authored witness=y\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 unknown_kind\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 cooling_plant\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 pump kind=centrifugal\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nedge e1 supplies p1\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 cooling_plant kind=chiller_plant name=\"unterminated\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 cooling_plant kind=nonsense\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 cooling_plant kind=chiller_plant design-supply=999999999.000\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 cooling_plant kind=chiller_plant bogus=1\n",
      "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode p1 cooling_plant kind=chiller_plant kind=chiller_plant\n",
  };
  for (const char* document : hostile) {
    const auto parsed = ct::parse_import(document);
    if (parsed.has_value()) {
      report_note(std::string("hostile document was accepted: ") + document);
      CT_CHECK(false);
    } else {
      CT_CHECK(parsed.error().code() != ct::ErrorCode::Ok);
      CT_CHECK(parsed.error().category() == ct::ErrorCategory::Argument);
    }
  }
  std::string long_line = "facility facility:dc-1\nprovenance producer=x origin=authored witness=y\nnode ";
  long_line.append(ct::limits::kMaxImportLineBytes + 1, 'a');
  long_line.push_back('\n');
  const auto bounded = ct::parse_import(long_line);
  CT_REQUIRE(bounded.has_value() == false);
  CT_CHECK_EQ(bounded.error().code(), ct::ErrorCode::LimitExceeded);
}

CT_TEST(adversarial_group_and_changeover_cardinality_edges) {
  const auto two = ct::ChangeoverGroup::create(cid("co:two"), "two",
                                               {endpoint("plant:p1", ct::PortRole::SourceIn),
                                                endpoint("loop:l1", ct::PortRole::SourceIn)},
                                               1);
  CT_CHECK(two.has_value());
  CT_CHECK(ct::ChangeoverGroup::create(cid("co:zero"), "zero",
                                       {endpoint("plant:p1", ct::PortRole::SourceIn),
                                        endpoint("loop:l1", ct::PortRole::SourceIn)},
                                       0)
               .has_value() == false);
  CT_CHECK(ct::ChangeoverGroup::create(cid("co:full"), "full",
                                       {endpoint("plant:p1", ct::PortRole::SourceIn),
                                        endpoint("loop:l1", ct::PortRole::SourceIn)},
                                       2)
               .has_value() == false);
  CT_CHECK(ct::ChangeoverGroup::create(cid("co:one"), "one", {endpoint("plant:p1", ct::PortRole::SourceIn)}, 1)
               .has_value() == false);
  {
    std::vector<ct::RedundancyMember> members(ct::limits::kMaxGroupMemberCount + 1,
                                              ct::RedundancyMember{nid("plant:p1"), "plant:p1", std::nullopt});
    CT_CHECK(ct::RedundancyGroup::create(gid("g:big"), ct::RedundancyScheme::N, ct::RedundancyScope::Plant, "",
                                         "", members, false, false)
                 .has_value() == false);
  }
  {
    // A changeover member that is an endpoint of some edge is accepted; one that
    // no edge uses is a declared arrangement with nothing to select between.
    ct::TopologyDraft attached = single_path_facility();
    attached.changeovers.push_back(ct::ChangeoverGroup::create(cid("co:attached"), "attached",
                                                               {endpoint("plant:p1", ct::PortRole::SourceIn),
                                                                endpoint("loop:l1", ct::PortRole::SourceIn)},
                                                               1)
                                     .value());
    CT_CHECK(ct::Topology::validate_draft(attached).valid());

    ct::TopologyDraft unattached = single_path_facility();
    unattached.nodes.push_back(plant_node("plant:spare"));
    unattached.changeovers.push_back(ct::ChangeoverGroup::create(cid("co:unused"), "unused",
                                                                 {endpoint("plant:p1", ct::PortRole::SourceIn),
                                                                  endpoint("plant:spare", ct::PortRole::SourceIn)},
                                                                 1)
                                     .value());
    const ct::ValidationReport report = ct::Topology::validate_draft(unattached);
    CT_REQUIRE(report.primary() != nullptr);
    CT_CHECK_EQ(report.primary()->code, ct::ErrorCode::ConstraintUnsatisfied);
  }
}

CT_TEST(adversarial_repeated_operations_are_stable) {
  const ct::TopologyDraft draft = reference_facility();
  const ct::Topology first = build(draft);
  for (int round = 0; round < 8; ++round) {
    const ct::Topology again = build(draft);
    CT_CHECK_EQ(again.digest().to_hex(), first.digest().to_hex());
    auto bytes = again.canonical_bytes();
    CT_REQUIRE(bytes.has_value());
    auto baseline = first.canonical_bytes();
    CT_REQUIRE(baseline.has_value());
    CT_CHECK_EQ(*bytes, *baseline);
    CT_CHECK_EQ(again.render_text(), first.render_text());
    CT_CHECK_EQ(again.structural_sources().size(), first.structural_sources().size());
  }
}
