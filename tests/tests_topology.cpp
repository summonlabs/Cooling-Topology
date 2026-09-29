// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_topology.cpp - proof obligations for Topology itself: generation
// binding, canonical order, identity lookup, adjacency, structural origins,
// canonical bytes and the human rendering that is never accepted as input.
//
// Nothing below asks whether a component is running, whether medium is flowing,
// whether a path has capacity or whether a source is eligible: every question is
// about the structure this library owns, and the claim boundary is asserted
// explicitly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace ct_test;
namespace ct = dccp::cooling_topology;

/// Asserts that an operation was rejected with a documented code. The error
/// message is never inspected: the stable ErrorCode is the contract.
#define CT_CHECK_CODE(expression, expected)                                                        \
  do {                                                                                             \
    const auto& ct_outcome_ = (expression);                                                        \
    if (ct_outcome_.has_value() || ct_outcome_.error().code() != (expected)) {                     \
      ::ct_test::report_failure(                                                                   \
          __FILE__, __LINE__, #expression " rejected with " #expected,                             \
          ct_outcome_.has_value()                                                                  \
              ? std::string("accepted")                                                            \
              : std::string(::dccp::cooling_topology::error_code_name(ct_outcome_.error().code()))); \
    }                                                                                              \
  } while (false)

/// Compares two canonical byte images without printing binary content. The
/// failure detail names the sizes and the first differing byte, which is what a
/// digest or order defect needs to be diagnosed. Both arguments must be named
/// objects: taking the value() of a temporary Result would dangle.
#define CT_CHECK_BYTES(actual, expected)                                                          \
  do {                                                                                            \
    const std::string& ct_actual_ = (actual);                                                     \
    const std::string& ct_expected_ = (expected);                                                 \
    if (ct_actual_ != ct_expected_) {                                                             \
      std::size_t ct_at_ = 0;                                                                     \
      while (ct_at_ < ct_actual_.size() && ct_at_ < ct_expected_.size() &&                        \
             ct_actual_[ct_at_] == ct_expected_[ct_at_]) {                                        \
        ++ct_at_;                                                                                 \
      }                                                                                           \
      ::ct_test::report_failure(__FILE__, __LINE__, #actual " == " #expected,                     \
                                "byte images differ: sizes " + std::to_string(ct_actual_.size()) + \
                                    " vs " + std::to_string(ct_expected_.size()) +                \
                                    ", first difference at byte " + std::to_string(ct_at_));      \
    }                                                                                             \
  } while (false)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string join_strings(const std::vector<std::string>& values) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) {
      out += ',';
    }
    out += value;
  }
  return out;
}

/// Comma-joined identity of a table, in the order the table currently holds it.
template <class Record>
std::vector<std::string> identity_text(const std::vector<Record>& records) {
  std::vector<std::string> ids;
  ids.reserve(records.size());
  for (const Record& record : records) {
    ids.push_back(record.id.str());
  }
  return ids;
}

/// The same identities sorted by std::sort: the independent reference for the
/// canonical order the value must impose.
template <class Record>
std::vector<std::string> sorted_identity_text(const std::vector<Record>& records) {
  std::vector<std::string> ids = identity_text(records);
  std::sort(ids.begin(), ids.end());
  return ids;
}

template <class Value>
std::string join_ids(const std::vector<Value>& values) {
  std::string out;
  for (const Value& value : values) {
    if (!out.empty()) {
      out += ',';
    }
    out += value.str();
  }
  return out;
}

/// Single-hop alias resolution over a draft: the canonical identity a spelling
/// denotes. Chains are a validation error, so one hop is the whole relation.
ct::NodeId draft_resolve(const ct::TopologyDraft& draft, const ct::NodeId& identity) {
  for (const ct::Alias& alias : draft.aliases) {
    if (alias.id.value() == identity.value()) {
      return alias.target;
    }
  }
  return identity;
}

/// The supplier kinds, written out by hand: the documented structural rule for
/// what may be the origin of a supply path.
bool reference_supplier_kind(ct::NodeKind kind) {
  switch (kind) {
    case ct::NodeKind::CoolingSource:
    case ct::NodeKind::CoolingPlant:
    case ct::NodeKind::Chiller:
    case ct::NodeKind::CoolingLoop:
    case ct::NodeKind::Manifold:
    case ct::NodeKind::Branch:
    case ct::NodeKind::Cdu:
      return true;
    default:
      return false;
  }
}

/// Structural origins computed from the raw draft: supplier kinds with no
/// incoming Supplies edge, in canonical identity order.
std::vector<std::string> reference_structural_sources(const ct::TopologyDraft& draft) {
  std::vector<std::string> result;
  for (const ct::Node& node : draft.nodes) {
    if (!reference_supplier_kind(node.kind())) {
      continue;
    }
    bool fed = false;
    for (const ct::Edge& edge : draft.edges) {
      if (edge.kind == ct::EdgeKind::Supplies && draft_resolve(draft, edge.to.node) == node.id) {
        fed = true;
        break;
      }
    }
    if (!fed) {
      result.push_back(node.id.str());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool has_node(const std::vector<ct::NodeId>& ids, std::string_view text) {
  for (const ct::NodeId& id : ids) {
    if (id.value() == text) {
      return true;
    }
  }
  return false;
}

template <class Record>
void shuffle_table(std::vector<Record>& records, Rng& rng) {
  for (std::size_t remaining = records.size(); remaining > 1; --remaining) {
    const std::size_t other = rng.below(static_cast<std::uint32_t>(remaining));
    std::swap(records[remaining - 1], records[other]);
  }
}

/// The reference facility plus the two tables it does not exercise: aliases and
/// declared changeover arrangements. Every table therefore has a non-trivial
/// order to disturb.
ct::TopologyDraft extended_reference() {
  ct::TopologyDraft draft = reference_facility();
  ct::Alias loop_alias;
  loop_alias.id = aid("alias:loop-secondary");
  loop_alias.target = nid("loop:secondary");
  ct::Alias plant_alias;
  plant_alias.id = aid("alias:plant-a");
  plant_alias.target = nid("plant:chp-a");
  draft.aliases = {loop_alias, plant_alias};

  auto manifold_changeover =
      ct::ChangeoverGroup::create(cid("changeover:manifolds"), "manifold changeover",
                                  {ct::Endpoint{nid("manifold:m-a"), ct::PortRole::SourceIn},
                                   ct::Endpoint{nid("manifold:m-b"), ct::PortRole::SourceIn}},
                                  1);
  CT_REQUIRE(manifold_changeover.has_value());
  auto branch_changeover =
      ct::ChangeoverGroup::create(cid("changeover:branches"), "branch changeover",
                                  {ct::Endpoint{nid("branch:b-a1"), ct::PortRole::SourceIn},
                                   ct::Endpoint{nid("branch:b-b1"), ct::PortRole::SourceIn}},
                                  1);
  CT_REQUIRE(branch_changeover.has_value());
  draft.changeovers = {*manifold_changeover, *branch_changeover};
  return draft;
}

// ---------------------------------------------------------------------------
// The claim boundary
// ---------------------------------------------------------------------------

CT_TEST(topology_claim_boundary_posture_is_total) {
  // The eleven excluded claims are exactly the values 0..10, the only
  // disposition is NotOwned, and the two claim classes are exactly the values 0
  // and 1: no code path can produce an operational claim.
  static_assert(static_cast<std::uint8_t>(ct::ExcludedClaim::PowerState) == 10);
  static_assert(static_cast<std::uint8_t>(ct::ClaimDisposition::NotOwned) == 0);
  static_assert(static_cast<std::uint8_t>(ct::ClaimClass::StructurallyPossible) == 0);
  static_assert(static_cast<std::uint8_t>(ct::ClaimClass::StructurallyImpossible) == 1);

  CT_CHECK(!ct::posture_statement().empty());
  CT_CHECK(!ct::posture_report().empty());
  for (std::uint8_t index = 0; index < 11; ++index) {
    const auto claim = static_cast<ct::ExcludedClaim>(index);
    CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(claim), ct::ClaimDisposition::NotOwned);
    CT_CHECK(!ct::to_token(claim).empty());
    CT_CHECK(!ct::excluded_claim_owner(claim).empty());
  }
  CT_CHECK(!ct::claim_class_statement(ct::ClaimClass::StructurallyPossible).empty());
  CT_CHECK(!ct::claim_class_statement(ct::ClaimClass::StructurallyImpossible).empty());
}

// ---------------------------------------------------------------------------
// Generation binding
// ---------------------------------------------------------------------------

CT_TEST(topology_create_rejects_impossible_generation_bindings) {
  const ct::TopologyDraft draft = single_path_facility();
  const ct::Digest parent_digest = ct::digest_bytes("a retained parent generation");

  // Generation 0 means "no topology published yet" and can never be published.
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(0), ct::TopologyGeneration(), ct::Digest(), draft),
                ct::ErrorCode::InvalidArgument);

  // A successor must bind the digest of the generation it succeeds.
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(1), ct::Digest(), draft),
                ct::ErrorCode::MissingField);

  // The parent must be strictly older than the generation it precedes.
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(2), parent_digest, draft),
                ct::ErrorCode::GenerationMismatch);
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(1), ct::TopologyGeneration(2), parent_digest, draft),
                ct::ErrorCode::GenerationMismatch);
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(7), ct::TopologyGeneration(9), parent_digest, draft),
                ct::ErrorCode::GenerationMismatch);

  // A first generation must not bind a parent digest.
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(1), ct::TopologyGeneration(), parent_digest, draft),
                ct::ErrorCode::GenerationMismatch);
  CT_CHECK_CODE(ct::Topology::create(ct::TopologyGeneration(9), ct::TopologyGeneration(), parent_digest, draft),
                ct::ErrorCode::GenerationMismatch);

  // The well-formed successor is accepted and carries its binding.
  const auto successor =
      ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(1), parent_digest, draft);
  CT_REQUIRE(successor.has_value());
  CT_CHECK_EQ(successor.value().generation().value(), std::uint64_t{2});
  CT_CHECK_EQ(successor.value().parent_generation().value(), std::uint64_t{1});
  CT_CHECK(successor.value().parent_digest() == parent_digest);
  CT_CHECK(!successor.value().digest().is_zero());
}

CT_TEST(topology_create_first_is_generation_one_without_a_parent) {
  const ct::TopologyDraft draft = single_path_facility();
  const ct::Topology topology = build(draft);

  CT_CHECK_EQ(topology.generation().value(), ct::TopologyGeneration::kFirstPublished);
  CT_CHECK_EQ(topology.parent_generation().value(), std::uint64_t{0});
  CT_CHECK(!topology.parent_generation().published());
  CT_CHECK(topology.parent_digest().is_zero());
  CT_CHECK(!topology.digest().is_zero());
  CT_CHECK_EQ(topology.header().schema_version, ct::kCanonicalSchemaVersion);
  CT_CHECK(topology.facility().same_binding_as(draft.facility));
  CT_CHECK_EQ(topology.header().provenance.producer, std::string("dccp-cooling-topology/1.0.0"));

  // Boundary: the largest representable generation is publishable, and the
  // counter beyond it is reported rather than wrapped.
  auto last = ct::Topology::create(ct::TopologyGeneration(std::numeric_limits<std::uint64_t>::max()),
                                   ct::TopologyGeneration(1), ct::digest_bytes("parent"), draft);
  CT_REQUIRE(last.has_value());
  CT_CHECK_EQ(last.value().generation().value(), std::numeric_limits<std::uint64_t>::max());
  const auto exhausted = last.value().generation().next();
  CT_CHECK(!exhausted.has_value());
  CT_CHECK_EQ(exhausted.error().code(), ct::ErrorCode::LimitExceeded);
}

CT_TEST(topology_generation_binding_changes_the_digest_and_carries_the_parent) {
  const ct::TopologyDraft draft = extended_reference();
  const ct::Topology first = build(draft);
  auto second = ct::Topology::create(ct::TopologyGeneration(2), ct::TopologyGeneration(1), first.digest(), draft);
  CT_REQUIRE(second.has_value());
  auto third =
      ct::Topology::create(ct::TopologyGeneration(3), ct::TopologyGeneration(2), second.value().digest(), draft);
  CT_REQUIRE(third.has_value());

  // The generation number is part of the canonical image, so three generations
  // of one draft have three digests.
  CT_CHECK(!(first.digest() == second.value().digest()));
  CT_CHECK(!(second.value().digest() == third.value().digest()));
  CT_CHECK(!(first.digest() == third.value().digest()));
  const std::string first_bytes = first.canonical_bytes().value();
  const std::string second_bytes = second.value().canonical_bytes().value();
  CT_CHECK(!(first_bytes == second_bytes));

  // The parent digest is recorded byte for byte, never recomputed.
  CT_CHECK(second.value().parent_digest() == first.digest());
  CT_CHECK(third.value().parent_digest() == second.value().digest());
  CT_CHECK_EQ(second.value().parent_digest().to_hex(), first.digest().to_hex());
  CT_CHECK_EQ(third.value().parent_digest().to_hex(), second.value().digest().to_hex());
  CT_CHECK_EQ(third.value().parent_generation().value(), std::uint64_t{2});
  CT_CHECK(second.value().recompute_digest().value() == second.value().digest());
}

// ---------------------------------------------------------------------------
// Canonical order
// ---------------------------------------------------------------------------

CT_TEST(topology_canonical_tables_are_sorted_and_order_independent) {
  const std::string case_name = "topology_canonical_tables_are_sorted_and_order_independent";
  const ct::TopologyDraft base = extended_reference();
  const ct::Topology reference = build(base);
  const std::string reference_bytes = reference.canonical_bytes().value();

  // Every table really has an order that can be disturbed.
  CT_CHECK(base.nodes.size() > std::size_t{1});
  CT_CHECK(base.edges.size() > std::size_t{1});
  CT_CHECK(base.groups.size() > std::size_t{1});
  CT_CHECK(base.aliases.size() > std::size_t{1});
  CT_CHECK(base.changeovers.size() > std::size_t{1});

  // The returned tables are in canonical order, checked against std::sort of the
  // raw draft tables rather than against the value itself.
  CT_CHECK_EQ(join_strings(identity_text(reference.nodes())), join_strings(sorted_identity_text(base.nodes)));
  CT_CHECK_EQ(join_strings(identity_text(reference.edges())), join_strings(sorted_identity_text(base.edges)));
  CT_CHECK_EQ(join_strings(identity_text(reference.groups())), join_strings(sorted_identity_text(base.groups)));
  CT_CHECK_EQ(join_strings(identity_text(reference.aliases())), join_strings(sorted_identity_text(base.aliases)));
  CT_CHECK_EQ(join_strings(identity_text(reference.changeovers())),
              join_strings(sorted_identity_text(base.changeovers)));
  CT_CHECK(std::is_sorted(reference.groups().begin(), reference.groups().end(),
                          [](const ct::RedundancyGroup& lhs, const ct::RedundancyGroup& rhs) {
                            return lhs.id < rhs.id;
                          }));
  for (const ct::RedundancyGroup& group : reference.groups()) {
    CT_CHECK(std::is_sorted(group.members.begin(), group.members.end(),
                            [](const ct::RedundancyMember& lhs, const ct::RedundancyMember& rhs) {
                              if (!(lhs.node == rhs.node)) {
                                return lhs.node < rhs.node;
                              }
                              return lhs.declared < rhs.declared;
                            }));
  }
  for (const ct::ChangeoverGroup& group : reference.changeovers()) {
    CT_CHECK(std::is_sorted(group.members.begin(), group.members.end()));
  }
  for (const ct::Node& node : reference.nodes()) {
    CT_CHECK(std::is_sorted(node.references.begin(), node.references.end()));
  }

  // Sixteen seeded permutations of all five tables produce one digest, one
  // canonical image and one adjacency order every time.
  Rng rng(case_seed(case_name));
  std::size_t reordered_rounds = 0;
  for (std::size_t round = 0; round < 16; ++round) {
    ct::TopologyDraft shuffled = base;
    shuffle_table(shuffled.nodes, rng);
    shuffle_table(shuffled.edges, rng);
    shuffle_table(shuffled.groups, rng);
    shuffle_table(shuffled.aliases, rng);
    shuffle_table(shuffled.changeovers, rng);
    if (identity_text(shuffled.nodes) != identity_text(base.nodes) ||
        identity_text(shuffled.edges) != identity_text(base.edges) ||
        identity_text(shuffled.groups) != identity_text(base.groups) ||
        identity_text(shuffled.aliases) != identity_text(base.aliases) ||
        identity_text(shuffled.changeovers) != identity_text(base.changeovers)) {
      ++reordered_rounds;
    }

    const auto created = ct::Topology::create_first(shuffled);
    CT_REQUIRE(created.has_value());
    const ct::Topology& permuted = created.value();
    if (!(permuted.digest() == reference.digest())) {
      report_note("round " + std::to_string(round) + " diverged; rerun with --seed=" +
                  std::to_string(run_seed()) + " (case seed " + std::to_string(case_seed(case_name)) + ")");
    }
    CT_CHECK(permuted.digest() == reference.digest());
    const std::string permuted_bytes = permuted.canonical_bytes().value();
    CT_CHECK_BYTES(permuted_bytes, reference_bytes);
    CT_CHECK_EQ(join_strings(identity_text(permuted.nodes())), join_strings(identity_text(reference.nodes())));
    CT_CHECK_EQ(join_strings(identity_text(permuted.edges())), join_strings(identity_text(reference.edges())));
    CT_CHECK_EQ(join_strings(identity_text(permuted.groups())), join_strings(identity_text(reference.groups())));
    CT_CHECK_EQ(join_strings(identity_text(permuted.aliases())), join_strings(identity_text(reference.aliases())));
    CT_CHECK_EQ(join_strings(identity_text(permuted.changeovers())),
                join_strings(identity_text(reference.changeovers())));
  }
  // The permutations really did permute the input tables.
  CT_CHECK(reordered_rounds > 0);
}

// ---------------------------------------------------------------------------
// Identity lookup
// ---------------------------------------------------------------------------

CT_TEST(topology_identity_lookup_resolves_nodes_aliases_and_absence) {
  const ct::Topology topology = build(extended_reference());
  const ct::NodeId canonical = nid("plant:chp-a");
  const ct::NodeId alias = nid("alias:plant-a");

  const auto resolved_node = topology.resolve(canonical);
  CT_REQUIRE(resolved_node.has_value());
  CT_CHECK(resolved_node.value() == canonical);
  const auto resolved_alias = topology.resolve(alias);
  CT_REQUIRE(resolved_alias.has_value());
  CT_CHECK(resolved_alias.value() == canonical);

  CT_CHECK_CODE(topology.resolve(nid("plant:nobody")), ct::ErrorCode::NotFound);
  CT_CHECK_CODE(topology.resolve(ct::NodeId()), ct::ErrorCode::MalformedIdentifier);

  const ct::Node* by_id = topology.find_node(canonical);
  const ct::Node* by_alias = topology.find_node(alias);
  CT_REQUIRE(by_id != nullptr);
  CT_REQUIRE(by_alias != nullptr);
  CT_CHECK(by_id == by_alias);
  CT_CHECK_EQ(by_id->id.str(), std::string("plant:chp-a"));
  CT_CHECK_EQ(by_id->kind(), ct::NodeKind::CoolingPlant);
  CT_CHECK(topology.find_node(nid("plant:nobody")) == nullptr);

  const ct::Edge* edge = topology.find_edge(eid("e:plant-primary"));
  CT_REQUIRE(edge != nullptr);
  CT_CHECK_EQ(edge->kind, ct::EdgeKind::Supplies);
  CT_CHECK(edge->from.node == nid("plant:chp-a"));
  CT_CHECK(edge->to.node == nid("loop:primary"));
  CT_CHECK_EQ(edge->from.port, ct::PortRole::SupplyOut);
  CT_CHECK_EQ(edge->to.port, ct::PortRole::SourceIn);
  CT_CHECK(topology.find_edge(eid("e:absent")) == nullptr);

  const ct::RedundancyGroup* group = topology.find_group(gid("group:plants"));
  CT_REQUIRE(group != nullptr);
  CT_CHECK_EQ(group->scheme, ct::RedundancyScheme::NPlusOne);
  CT_CHECK_EQ(group->scope, ct::RedundancyScope::Plant);
  CT_CHECK_EQ(group->members.size(), std::size_t{2});
  CT_CHECK(topology.find_group(gid("group:absent")) == nullptr);

  const ct::Alias* alias_record = topology.find_alias(aid("alias:plant-a"));
  CT_REQUIRE(alias_record != nullptr);
  CT_CHECK(alias_record->target == canonical);
  CT_CHECK(topology.find_alias(aid("alias:absent")) == nullptr);

  const ct::ChangeoverGroup* changeover = topology.find_changeover(cid("changeover:manifolds"));
  CT_REQUIRE(changeover != nullptr);
  CT_CHECK_EQ(changeover->max_concurrent, std::uint32_t{1});
  CT_CHECK_EQ(changeover->members.size(), std::size_t{2});
  CT_CHECK(topology.find_changeover(cid("changeover:absent")) == nullptr);
}

// ---------------------------------------------------------------------------
// Adjacency
// ---------------------------------------------------------------------------

CT_TEST(topology_adjacency_matches_an_independent_filter) {
  const ct::TopologyDraft draft = extended_reference();
  const ct::Topology topology = build(draft);

  // The independent reference filters the raw edge table and sorts by identity.
  const auto expected_edges = [&draft](const ct::NodeId& node, bool outgoing) {
    std::vector<ct::EdgeId> ids;
    for (const ct::Edge& edge : draft.edges) {
      const ct::NodeId endpoint = outgoing ? edge.from.node : edge.to.node;
      if (draft_resolve(draft, endpoint) == node) {
        ids.push_back(edge.id);
      }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  };

  for (const ct::Node& node : topology.nodes()) {
    const auto out = topology.out_edges(node.id);
    const auto in = topology.in_edges(node.id);
    CT_CHECK(std::is_sorted(out.begin(), out.end()));
    CT_CHECK(std::is_sorted(in.begin(), in.end()));
    const std::vector<ct::EdgeId> out_view(out.begin(), out.end());
    const std::vector<ct::EdgeId> in_view(in.begin(), in.end());
    CT_CHECK_EQ(join_ids(out_view), join_ids(expected_edges(node.id, true)));
    CT_CHECK_EQ(join_ids(in_view), join_ids(expected_edges(node.id, false)));
  }

  // An alias spelling answers exactly like the canonical identity.
  const std::vector<ct::EdgeId> via_alias(topology.out_edges(nid("alias:loop-secondary")).begin(),
                                          topology.out_edges(nid("alias:loop-secondary")).end());
  CT_CHECK_EQ(join_ids(via_alias), join_ids(expected_edges(nid("loop:secondary"), true)));

  // A node with no outgoing edge has an empty view, and so has an absent node.
  CT_CHECK_EQ(topology.out_edges(nid("pump:p-1")).size(), std::size_t{0});
  CT_CHECK_EQ(topology.out_edges(nid("plant:nobody")).size(), std::size_t{0});
  CT_CHECK_EQ(topology.in_edges(nid("plant:nobody")).size(), std::size_t{0});
  CT_CHECK_EQ(topology.in_edges(nid("source:facility-water")).size(), std::size_t{1});

  // edges_of_kind returns exactly the edges of that kind, in canonical order.
  const ct::EdgeKind kinds[] = {ct::EdgeKind::Supplies, ct::EdgeKind::Returns,   ct::EdgeKind::Serves,
                                ct::EdgeKind::Pumps,    ct::EdgeKind::Contains,  ct::EdgeKind::DependsOn};
  for (const ct::EdgeKind kind : kinds) {
    std::vector<ct::EdgeId> expected;
    for (const ct::Edge& edge : draft.edges) {
      if (edge.kind == kind) {
        expected.push_back(edge.id);
      }
    }
    std::sort(expected.begin(), expected.end());
    const std::vector<ct::EdgeId> found = topology.edges_of_kind(kind);
    CT_CHECK(std::is_sorted(found.begin(), found.end()));
    CT_CHECK_EQ(join_ids(found), join_ids(expected));
  }
  CT_CHECK_EQ(topology.edges_of_kind(ct::EdgeKind::Supplies).size(), std::size_t{13});
  CT_CHECK_EQ(topology.edges_of_kind(ct::EdgeKind::Returns).size(), std::size_t{11});
  CT_CHECK_EQ(topology.edges_of_kind(ct::EdgeKind::Serves).size(), std::size_t{1});
  CT_CHECK_EQ(topology.edges_of_kind(ct::EdgeKind::Pumps).size(), std::size_t{2});
  CT_CHECK_EQ(topology.edges_of_kind(ct::EdgeKind::Contains).size(), std::size_t{8});
  CT_CHECK_EQ(topology.edges_of_kind(ct::EdgeKind::DependsOn).size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Structural origins
// ---------------------------------------------------------------------------

CT_TEST(topology_structural_sources_are_supplier_kinds_without_incoming_supplies) {
  // The public predicate agrees with the documented list for every node kind.
  for (std::uint8_t index = 0; index <= static_cast<std::uint8_t>(ct::NodeKind::CoolingSource); ++index) {
    const auto kind = static_cast<ct::NodeKind>(index);
    CT_CHECK_EQ(ct::is_supplier_kind(kind), reference_supplier_kind(kind));
  }

  const ct::TopologyDraft draft = reference_facility();
  const ct::Topology topology = build(draft);
  const std::vector<ct::NodeId> sources = topology.structural_sources();
  CT_CHECK_EQ(join_ids(sources), join_strings(reference_structural_sources(draft)));
  CT_CHECK_EQ(join_ids(sources), std::string("chiller:ch-1,source:facility-water"));
  CT_CHECK(has_node(sources, "source:facility-water"));
  // Every other node has an incoming supplies edge.
  for (const ct::Node& node : topology.nodes()) {
    if (!has_node(sources, node.id.value())) {
      continue;
    }
    for (const ct::EdgeId& edge : topology.in_edges(node.id)) {
      CT_CHECK(topology.find_edge(edge)->kind != ct::EdgeKind::Supplies);
    }
  }

  // Removing the only incoming supplies edge of the secondary loop makes it an
  // origin of the supply graph.
  ct::TopologyDraft cut = draft;
  cut.edges.erase(std::remove_if(cut.edges.begin(), cut.edges.end(),
                                 [](const ct::Edge& edge) { return edge.id == eid("e:primary-secondary"); }),
                  cut.edges.end());
  CT_CHECK(!has_node(topology.structural_sources(), "loop:secondary"));
  const ct::Topology without = build(cut);
  const std::vector<ct::NodeId> cut_sources = without.structural_sources();
  CT_CHECK(has_node(cut_sources, "loop:secondary"));
  CT_CHECK_EQ(join_ids(cut_sources), join_strings(reference_structural_sources(cut)));
  CT_CHECK_EQ(join_ids(cut_sources), std::string("chiller:ch-1,loop:secondary,source:facility-water"));
}

// ---------------------------------------------------------------------------
// Canonical bytes
// ---------------------------------------------------------------------------

CT_TEST(topology_canonical_bytes_and_digest_are_stable_and_round_trip) {
  const ct::Topology topology = build(extended_reference());

  const auto bytes = topology.canonical_bytes();
  CT_REQUIRE(bytes.has_value());
  const std::string& image = bytes.value();
  CT_CHECK(!image.empty());
  CT_CHECK(image.size() <= ct::limits::kMaxGenerationBytes);

  // Repeated calls answer identically and the digest is the digest of exactly
  // those bytes, checked with the standalone SHA-256 helper.
  const std::string repeated = topology.canonical_bytes().value();
  CT_CHECK_BYTES(repeated, image);
  const auto recomputed = topology.recompute_digest();
  CT_REQUIRE(recomputed.has_value());
  CT_CHECK(recomputed.value() == topology.digest());
  CT_CHECK(ct::digest_bytes(image) == topology.digest());
  CT_CHECK_EQ(ct::digest_bytes(image).to_hex(), topology.digest().to_hex());
  CT_CHECK(!topology.digest().is_zero());

  // to_draft() loses nothing that the digest covers.
  const ct::TopologyDraft round_trip_draft = topology.to_draft();
  CT_CHECK(round_trip_draft.facility.same_binding_as(topology.facility()));
  CT_CHECK_EQ(round_trip_draft.nodes.size(), topology.node_count());
  CT_CHECK_EQ(round_trip_draft.edges.size(), topology.edge_count());
  CT_CHECK_EQ(round_trip_draft.groups.size(), topology.group_count());
  const ct::Topology round_tripped = build(round_trip_draft);
  CT_CHECK_EQ(round_tripped.digest().to_hex(), topology.digest().to_hex());
  const std::string round_tripped_bytes = round_tripped.canonical_bytes().value();
  CT_CHECK_BYTES(round_tripped_bytes, image);
}

// ---------------------------------------------------------------------------
// Rendering is never input
// ---------------------------------------------------------------------------

CT_TEST(topology_render_text_is_deterministic_and_never_parsed_back) {
  const ct::Topology topology = build(extended_reference());
  const std::string first = topology.render_text();
  const std::string second = topology.render_text();
  CT_CHECK_EQ(first, second);
  CT_CHECK(!first.empty());

  // The rendering names the generation, the digest and the claim boundary.
  CT_CHECK(first.find("digest=" + topology.digest().to_hex()) != std::string::npos);
  CT_CHECK(first.find("nodes " + std::to_string(topology.node_count())) != std::string::npos);
  CT_CHECK(first.find("edges " + std::to_string(topology.edge_count())) != std::string::npos);
  CT_CHECK(first.find(std::string(ct::posture_statement())) != std::string::npos);
  CT_CHECK(first.find("claim-boundary ") != std::string::npos);

  // There is no parser for the rendering: the import grammar refuses it, so a
  // rendering can never be fed back as topology content.
  const auto parsed = ct::parse_import(first);
  CT_CHECK(!parsed.has_value());
  if (!parsed.has_value()) {
    CT_CHECK_EQ(parsed.error().code(), ct::ErrorCode::UnknownEnumToken);
  }
  // A different generation renders differently.
  const ct::Topology other = build(single_path_facility());
  CT_CHECK(other.render_text() != first);
}

}  // namespace
