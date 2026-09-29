// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_diff.cpp - proof obligations for the structural diff between two
// published generations: entry kinds, field-level detail, ordering, impact and
// the fact that the diff is a pure function of its two inputs.
//
// A diff entry states that structure was added, removed or changed. It never
// states that anything stopped working, that a zone is unsafe or that a change
// was authorized: the impact section is evaluated on the "after" generation and
// the rendering carries the claim-boundary statement.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/diff.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace ct_test;
namespace ct = dccp::cooling_topology;

// ---------------------------------------------------------------------------
// The claim boundary
// ---------------------------------------------------------------------------

void check_claim_boundary() {
  static_assert(static_cast<std::uint8_t>(ct::ExcludedClaim::PowerState) == 10);
  static_assert(static_cast<std::uint8_t>(ct::ClaimDisposition::NotOwned) == 0);
  CT_CHECK(!ct::posture_statement().empty());
  for (std::uint8_t index = 0; index < 11; ++index) {
    CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(static_cast<ct::ExcludedClaim>(index)),
                ct::ClaimDisposition::NotOwned);
  }
}

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

/// One line per entry, so a failure shows kind, subject and detail together.
std::string entry_signature(const ct::TopologyDiff& diff) {
  std::string out;
  for (const ct::DiffEntry& entry : diff.entries) {
    out += std::string(ct::to_token(entry.kind));
    out += '|';
    out += entry.subject;
    out += '|';
    out += entry.detail;
    out += '\n';
  }
  return out;
}

std::string impact_signature(const ct::TopologyDiff& diff) {
  std::string out = join_ids(diff.impact.unfed_sinks);
  out += ";";
  out += join_ids(diff.impact.unserved_zones);
  out += ";";
  out += diff.impact.truncated ? "truncated" : "complete";
  return out;
}

const ct::DiffEntry* find_entry(const ct::TopologyDiff& diff, ct::DiffChangeKind kind,
                                std::string_view subject) {
  for (const ct::DiffEntry& entry : diff.entries) {
    if (entry.kind == kind && entry.subject == subject) {
      return &entry;
    }
  }
  return nullptr;
}

/// The next published generation of the same line: generation n+1 bound to the
/// digest of the generation it succeeds.
ct::Topology next_generation(const ct::Topology& before, const ct::TopologyDraft& draft) {
  auto created = ct::Topology::create(ct::TopologyGeneration(before.generation().value() + 1),
                                      before.generation(), before.digest(), draft);
  CT_REQUIRE(created.has_value());
  return *created;
}

void erase_edge(ct::TopologyDraft& draft, std::string_view identity) {
  const auto id = ct::EdgeId::parse(identity);
  CT_REQUIRE(id.has_value());
  draft.edges.erase(std::remove_if(draft.edges.begin(), draft.edges.end(),
                                   [&id](const ct::Edge& edge) { return edge.id == id.value(); }),
                    draft.edges.end());
}

/// Replaces one node with a rebuilt element that declares a design supply
/// temperature.
ct::TopologyDraft with_plant_temperature(std::string_view identity, std::int64_t milli_celsius) {
  ct::TopologyDraft draft = single_path_facility();
  for (ct::Node& node : draft.nodes) {
    if (node.id == nid(identity)) {
      auto* attributes = std::get_if<ct::CoolingPlantAttributes>(&node.attributes);
      CT_REQUIRE(attributes != nullptr);
      attributes->design_supply_temperature = ct::MilliCelsius(milli_celsius);
    }
  }
  return draft;
}

// ---------------------------------------------------------------------------
// Identity of a generation with itself
// ---------------------------------------------------------------------------

CT_TEST(diff_of_a_generation_with_itself_is_empty) {
  const ct::Topology single = build(single_path_facility());
  const auto diff = ct::diff_topologies(single, single);
  CT_REQUIRE(diff.has_value());
  CT_CHECK(diff.value().entries.empty());
  CT_CHECK_EQ(diff.value().node_delta, 0);
  CT_CHECK_EQ(diff.value().edge_delta, 0);
  CT_CHECK(diff.value().same_facility);
  CT_CHECK(diff.value().before_digest == diff.value().after_digest);
  CT_CHECK(diff.value().before_generation == diff.value().after_generation);
  // The impact section reports the structural state of the after generation:
  // this fixture has no serves edge at all, so its one zone is unserved even
  // when nothing changed.
  CT_CHECK(diff.value().impact.unfed_sinks.empty());
  CT_CHECK_EQ(join_ids(diff.value().impact.unserved_zones), std::string("zone:z1"));
  CT_CHECK(!diff.value().impact.truncated);

  // The same holds for the richer fixture, and twice in a row: the diff is a
  // pure function of its inputs.
  const ct::Topology reference = build(reference_facility());
  const auto first = ct::diff_topologies(reference, reference);
  const auto second = ct::diff_topologies(reference, reference);
  CT_REQUIRE(first.has_value() && second.has_value());
  CT_CHECK(first.value().entries.empty());
  CT_CHECK(first.value().impact.unfed_sinks.empty());
  CT_CHECK(first.value().impact.unserved_zones.empty());
  CT_CHECK_EQ(entry_signature(first.value()), entry_signature(second.value()));
  CT_CHECK_EQ(impact_signature(first.value()), impact_signature(second.value()));
}

// ---------------------------------------------------------------------------
// Additions
// ---------------------------------------------------------------------------

CT_TEST(diff_reports_each_added_object_with_its_documented_kind) {
  const ct::Topology before = build(single_path_facility());

  {
    ct::TopologyDraft draft = single_path_facility();
    draft.nodes.push_back(plant_node("plant:spare"));
    const auto diff = ct::diff_topologies(before, next_generation(before, draft));
    CT_REQUIRE(diff.has_value());
    CT_REQUIRE(diff.value().entries.size() == std::size_t{1});
    CT_CHECK(diff.value().entries.front().kind == ct::DiffChangeKind::NodeAdded);
    CT_CHECK_EQ(diff.value().entries.front().subject, std::string("plant:spare"));
    CT_CHECK(diff.value().entries.front().detail.empty());
    CT_CHECK_EQ(diff.value().node_delta, 1);
    CT_CHECK_EQ(diff.value().edge_delta, 0);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    draft.edges.push_back(edge("e:extra", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut,
                               "branch:br1", ct::PortRole::SourceIn));
    const auto diff = ct::diff_topologies(before, next_generation(before, draft));
    CT_REQUIRE(diff.has_value());
    CT_REQUIRE(diff.value().entries.size() == std::size_t{1});
    CT_CHECK(diff.value().entries.front().kind == ct::DiffChangeKind::EdgeAdded);
    CT_CHECK_EQ(diff.value().entries.front().subject, std::string("e:extra"));
    CT_CHECK(diff.value().entries.front().detail.empty());
    CT_CHECK_EQ(diff.value().node_delta, 0);
    CT_CHECK_EQ(diff.value().edge_delta, 1);
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    auto group = ct::RedundancyGroup::create(gid("group:loop"), ct::RedundancyScheme::N,
                                             ct::RedundancyScope::CoolingLoop, "loops", "one loop",
                                             {ct::RedundancyMember{nid("loop:l1"), "loop:l1", std::nullopt}}, false,
                                             false);
    CT_REQUIRE(group.has_value());
    draft.groups.push_back(*group);
    const auto diff = ct::diff_topologies(before, next_generation(before, draft));
    CT_REQUIRE(diff.has_value());
    CT_REQUIRE(diff.value().entries.size() == std::size_t{1});
    CT_CHECK(diff.value().entries.front().kind == ct::DiffChangeKind::GroupAdded);
    CT_CHECK_EQ(diff.value().entries.front().subject, std::string("group:loop"));
    CT_CHECK(diff.value().entries.front().detail.empty());
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    ct::Alias alias;
    alias.id = aid("alias:l1-legacy");
    alias.target = nid("loop:l1");
    draft.aliases.push_back(alias);
    const auto diff = ct::diff_topologies(before, next_generation(before, draft));
    CT_REQUIRE(diff.has_value());
    CT_REQUIRE(diff.value().entries.size() == std::size_t{1});
    CT_CHECK(diff.value().entries.front().kind == ct::DiffChangeKind::AliasAdded);
    CT_CHECK_EQ(diff.value().entries.front().subject, std::string("alias:l1-legacy"));
    CT_CHECK(diff.value().entries.front().detail.empty());
  }
  {
    ct::TopologyDraft draft = single_path_facility();
    auto changeover = ct::ChangeoverGroup::create(cid("changeover:select"), "source selection",
                                                  {ct::Endpoint{nid("plant:p1"), ct::PortRole::SourceIn},
                                                   ct::Endpoint{nid("loop:l1"), ct::PortRole::SourceIn}},
                                                  1);
    CT_REQUIRE(changeover.has_value());
    draft.changeovers.push_back(*changeover);
    const auto diff = ct::diff_topologies(before, next_generation(before, draft));
    CT_REQUIRE(diff.has_value());
    CT_REQUIRE(diff.value().entries.size() == std::size_t{1});
    CT_CHECK(diff.value().entries.front().kind == ct::DiffChangeKind::ChangeoverAdded);
    CT_CHECK_EQ(diff.value().entries.front().subject, std::string("changeover:select"));
    CT_CHECK(diff.value().entries.front().detail.empty());
  }

  // All five at once, plus a second node: the entry list is sorted by
  // (kind, subject), checked against std::sort of the very same pairs.
  ct::TopologyDraft everything = single_path_facility();
  everything.nodes.push_back(plant_node("plant:zz-spare"));
  everything.nodes.push_back(plant_node("plant:aa-spare"));
  everything.edges.push_back(edge("e:extra", ct::EdgeKind::Supplies, "loop:l1", ct::PortRole::SupplyOut,
                                  "branch:br1", ct::PortRole::SourceIn));
  auto group = ct::RedundancyGroup::create(gid("group:loop"), ct::RedundancyScheme::N,
                                           ct::RedundancyScope::CoolingLoop, "loops", "one loop",
                                           {ct::RedundancyMember{nid("loop:l1"), "loop:l1", std::nullopt}}, false,
                                           false);
  CT_REQUIRE(group.has_value());
  everything.groups.push_back(*group);
  ct::Alias alias;
  alias.id = aid("alias:l1-legacy");
  alias.target = nid("loop:l1");
  everything.aliases.push_back(alias);
  auto changeover = ct::ChangeoverGroup::create(cid("changeover:select"), "source selection",
                                                {ct::Endpoint{nid("plant:p1"), ct::PortRole::SourceIn},
                                                 ct::Endpoint{nid("loop:l1"), ct::PortRole::SourceIn}},
                                                1);
  CT_REQUIRE(changeover.has_value());
  everything.changeovers.push_back(*changeover);

  const auto combined = ct::diff_topologies(before, next_generation(before, everything));
  CT_REQUIRE(combined.has_value());
  CT_CHECK_EQ(combined.value().entries.size(), std::size_t{6});
  std::vector<std::pair<int, std::string>> pairs;
  for (const ct::DiffEntry& entry : combined.value().entries) {
    pairs.emplace_back(static_cast<int>(entry.kind), entry.subject);
  }
  std::vector<std::pair<int, std::string>> sorted = pairs;
  std::sort(sorted.begin(), sorted.end());
  CT_CHECK(pairs == sorted);
  CT_CHECK_EQ(combined.value().node_delta, 2);
  CT_CHECK_EQ(combined.value().edge_delta, 1);
  CT_CHECK(find_entry(combined.value(), ct::DiffChangeKind::NodeAdded, "plant:aa-spare") != nullptr);
  CT_CHECK(find_entry(combined.value(), ct::DiffChangeKind::NodeAdded, "plant:zz-spare") != nullptr);
  CT_CHECK(find_entry(combined.value(), ct::DiffChangeKind::EdgeAdded, "e:extra") != nullptr);
  CT_CHECK(find_entry(combined.value(), ct::DiffChangeKind::GroupAdded, "group:loop") != nullptr);
  CT_CHECK(find_entry(combined.value(), ct::DiffChangeKind::AliasAdded, "alias:l1-legacy") != nullptr);
  CT_CHECK(find_entry(combined.value(), ct::DiffChangeKind::ChangeoverAdded, "changeover:select") != nullptr);
  // The two node additions are ordered by subject, not by insertion order.
  CT_CHECK_EQ(combined.value().entries[0].subject, std::string("plant:aa-spare"));
  CT_CHECK_EQ(combined.value().entries[1].subject, std::string("plant:zz-spare"));
}

// ---------------------------------------------------------------------------
// Attribute changes
// ---------------------------------------------------------------------------

CT_TEST(diff_reports_attribute_changes_with_field_level_detail) {
  const ct::Topology before = build(single_path_facility());

  // A node attribute change names the field and both values.
  const ct::MilliCelsius temperature(7000);
  const auto node_diff = ct::diff_topologies(before, next_generation(before, with_plant_temperature("plant:p1", 7000)));
  CT_REQUIRE(node_diff.has_value());
  CT_REQUIRE(node_diff.value().entries.size() == std::size_t{1});
  CT_CHECK(node_diff.value().entries.front().kind == ct::DiffChangeKind::NodeChanged);
  CT_CHECK_EQ(node_diff.value().entries.front().subject, std::string("plant:p1"));
  CT_CHECK_EQ(node_diff.value().entries.front().detail,
              std::string("design-supply=-" "->") + temperature.to_string());
  CT_CHECK_EQ(node_diff.value().node_delta, 0);
  CT_CHECK_EQ(node_diff.value().edge_delta, 0);

  // The field-level detail names the changed field, not the record.
  CT_CHECK(node_diff.value().entries.front().detail.find("design-supply") != std::string::npos);
  CT_CHECK(node_diff.value().entries.front().detail.find("display") == std::string::npos);

  // An edge attribute change: the same identity with a different target.
  ct::TopologyDraft retargeted = single_path_facility();
  for (ct::Edge& edge : retargeted.edges) {
    if (edge.id == eid("e4")) {
      edge.to.node = nid("cdu:c1");
    }
  }
  const auto edge_diff = ct::diff_topologies(before, next_generation(before, retargeted));
  CT_REQUIRE(edge_diff.has_value());
  CT_REQUIRE(edge_diff.value().entries.size() == std::size_t{1});
  CT_CHECK(edge_diff.value().entries.front().kind == ct::DiffChangeKind::EdgeChanged);
  CT_CHECK_EQ(edge_diff.value().entries.front().subject, std::string("e4"));
  CT_CHECK_EQ(edge_diff.value().entries.front().detail,
              std::string("to=branch:br1.") + std::string(ct::to_token(ct::PortRole::SourceIn)) + "->cdu:c1." +
                  std::string(ct::to_token(ct::PortRole::SourceIn)));
  CT_CHECK_EQ(edge_diff.value().node_delta, 0);
  CT_CHECK_EQ(edge_diff.value().edge_delta, 0);

  // A group attribute change: the declared scheme.
  const ct::Topology reference = build(reference_facility());
  ct::TopologyDraft regrouped = reference_facility();
  for (ct::RedundancyGroup& group : regrouped.groups) {
    if (group.id == gid("group:pumps")) {
      group.scheme = ct::RedundancyScheme::TwoN;
    }
  }
  const auto group_diff = ct::diff_topologies(reference, next_generation(reference, regrouped));
  CT_REQUIRE(group_diff.has_value());
  CT_REQUIRE(group_diff.value().entries.size() == std::size_t{1});
  CT_CHECK(group_diff.value().entries.front().kind == ct::DiffChangeKind::GroupChanged);
  CT_CHECK_EQ(group_diff.value().entries.front().subject, std::string("group:pumps"));
  CT_CHECK_EQ(group_diff.value().entries.front().detail,
              std::string("scheme=") + std::string(ct::to_token(ct::RedundancyScheme::NPlusOne)) + "->" +
                  std::string(ct::to_token(ct::RedundancyScheme::TwoN)));
  CT_CHECK_EQ(group_diff.value().node_delta, 0);
  CT_CHECK_EQ(group_diff.value().edge_delta, 0);
}

// ---------------------------------------------------------------------------
// Removals and impact
// ---------------------------------------------------------------------------

CT_TEST(diff_impact_reports_unfed_sinks_and_unserved_zones) {
  // Removing the only supplies edge into a sink leaves it without a feed.
  const ct::Topology fed = build(single_path_facility());
  ct::TopologyDraft unfed_draft = single_path_facility();
  erase_edge(unfed_draft, "e6");
  const ct::Topology unfed = next_generation(fed, unfed_draft);
  const auto unfed_diff = ct::diff_topologies(fed, unfed);
  CT_REQUIRE(unfed_diff.has_value());
  CT_CHECK_EQ(join_ids(unfed_diff.value().impact.unfed_sinks), std::string("sink:r1"));
  CT_CHECK_EQ(join_ids(unfed_diff.value().impact.unserved_zones), std::string("zone:z1"));
  CT_CHECK(!unfed_diff.value().impact.truncated);
  CT_REQUIRE(unfed_diff.value().entries.size() == std::size_t{1});
  CT_CHECK(unfed_diff.value().entries.front().kind == ct::DiffChangeKind::EdgeRemoved);
  CT_CHECK_EQ(unfed_diff.value().entries.front().subject, std::string("e6"));
  CT_CHECK_EQ(unfed_diff.value().edge_delta, -1);

  // The impact is evaluated on the after generation, so the reverse diff of the
  // same pair reports no impact at all.
  const auto reversed = ct::diff_topologies(unfed, fed);
  CT_REQUIRE(reversed.has_value());
  CT_CHECK(reversed.value().impact.unfed_sinks.empty());
  CT_CHECK_EQ(join_ids(reversed.value().impact.unserved_zones), std::string("zone:z1"));
  CT_CHECK(find_entry(reversed.value(), ct::DiffChangeKind::EdgeAdded, "e6") != nullptr);

  // Removing the only serves edge into a zone leaves it without a serving
  // element.
  const ct::Topology served = build(reference_facility());
  ct::TopologyDraft unserved_draft = reference_facility();
  erase_edge(unserved_draft, "e:crah-zone");
  const ct::Topology unserved = next_generation(served, unserved_draft);
  const auto unserved_diff = ct::diff_topologies(served, unserved);
  CT_REQUIRE(unserved_diff.has_value());
  CT_CHECK_EQ(join_ids(unserved_diff.value().impact.unserved_zones), std::string("zone:cold-aisle-1"));
  CT_CHECK(unserved_diff.value().impact.unfed_sinks.empty());
  CT_CHECK(!unserved_diff.value().impact.truncated);
  CT_CHECK_EQ(join_ids(unserved_diff.value().impact.unserved_zones),
              std::string("zone:cold-aisle-1"));
  CT_REQUIRE(unserved_diff.value().entries.size() == std::size_t{1});
  CT_CHECK(unserved_diff.value().entries.front().kind == ct::DiffChangeKind::EdgeRemoved);

  // With a second serving element the zone stays served: the impact is about
  // the generation that remains, not about the number of removed edges.
  ct::TopologyDraft two_servers = reference_facility();
  two_servers.nodes.push_back(crah_node("crah:ir-2"));
  two_servers.edges.push_back(edge("e:secondary-crah-2", ct::EdgeKind::Supplies, "loop:secondary",
                                   ct::PortRole::SupplyOut, "crah:ir-2", ct::PortRole::SourceIn));
  two_servers.edges.push_back(edge("e:crah-2-zone", ct::EdgeKind::Serves, "crah:ir-2", ct::PortRole::Server,
                                   "zone:cold-aisle-1", ct::PortRole::Served));
  const ct::Topology served_twice = build(two_servers);
  ct::TopologyDraft one_server = two_servers;
  erase_edge(one_server, "e:crah-zone");
  const auto still_served = ct::diff_topologies(served_twice, next_generation(served_twice, one_server));
  CT_REQUIRE(still_served.has_value());
  CT_CHECK(still_served.value().impact.unserved_zones.empty());
  CT_CHECK(find_entry(still_served.value(), ct::DiffChangeKind::EdgeRemoved, "e:crah-zone") != nullptr);

  // Removing a node removes its edges too, and the sink it fed becomes unfed.
  ct::TopologyDraft without_cdu = single_path_facility();
  without_cdu.nodes.erase(std::remove_if(without_cdu.nodes.begin(), without_cdu.nodes.end(),
                                         [](const ct::Node& node) { return node.id == nid("cdu:c1"); }),
                          without_cdu.nodes.end());
  without_cdu.edges.erase(std::remove_if(without_cdu.edges.begin(), without_cdu.edges.end(),
                                         [](const ct::Edge& value) {
                                           return value.from.node == nid("cdu:c1") ||
                                                  value.to.node == nid("cdu:c1");
                                         }),
                          without_cdu.edges.end());
  const auto removed = ct::diff_topologies(fed, next_generation(fed, without_cdu));
  CT_REQUIRE(removed.has_value());
  CT_CHECK(find_entry(removed.value(), ct::DiffChangeKind::NodeRemoved, "cdu:c1") != nullptr);
  CT_CHECK_EQ(join_ids(removed.value().impact.unfed_sinks), std::string("sink:r1"));
  CT_CHECK_EQ(removed.value().node_delta, -1);
  CT_CHECK(removed.value().edge_delta < 0);
}

// ---------------------------------------------------------------------------
// Facility binding
// ---------------------------------------------------------------------------

CT_TEST(diff_reports_a_changed_facility_binding) {
  const ct::Topology single = build(single_path_facility());
  const ct::Topology reference = build(reference_facility());

  const auto different = ct::diff_topologies(single, reference);
  CT_REQUIRE(different.has_value());
  CT_CHECK(!different.value().same_facility);
  CT_CHECK_EQ(different.value().before_generation.value(), std::uint64_t{1});
  CT_CHECK_EQ(different.value().after_generation.value(), std::uint64_t{1});
  CT_CHECK(different.value().before_digest == single.digest());
  CT_CHECK(different.value().after_digest == reference.digest());

  // The same facility identity with a different registry generation is a
  // different binding.
  ct::TopologyDraft rebound = single_path_facility();
  rebound.facility = extref("dc-single", ct::ExternalRefKind::Facility, 8);
  const auto rebound_diff = ct::diff_topologies(single, next_generation(single, rebound));
  CT_REQUIRE(rebound_diff.has_value());
  CT_CHECK(!rebound_diff.value().same_facility);
  CT_CHECK_EQ(rebound_diff.value().after_generation.value(), std::uint64_t{2});
  CT_CHECK(rebound_diff.value().entries.empty());

  // A successor of the same facility keeps the binding.
  ct::TopologyDraft successor = single_path_facility();
  successor.nodes.push_back(plant_node("plant:spare"));
  const auto same = ct::diff_topologies(single, next_generation(single, successor));
  CT_REQUIRE(same.has_value());
  CT_CHECK(same.value().same_facility);
  CT_CHECK_EQ(same.value().node_delta, 1);
}

// ---------------------------------------------------------------------------
// Explanation and purity
// ---------------------------------------------------------------------------

CT_TEST(diff_explanation_is_deterministic_and_orientation_sensitive) {
  check_claim_boundary();
  const ct::Topology before = build(single_path_facility());
  ct::TopologyDraft after_draft = single_path_facility();
  after_draft.nodes.push_back(plant_node("plant:spare"));
  const ct::Topology after = next_generation(before, after_draft);

  const auto forward = ct::diff_topologies(before, after);
  const auto backward = ct::diff_topologies(after, before);
  CT_REQUIRE(forward.has_value() && backward.has_value());

  // The diff is a pure function of the two inputs.
  const auto forward_again = ct::diff_topologies(before, after);
  CT_REQUIRE(forward_again.has_value());
  CT_CHECK_EQ(entry_signature(forward_again.value()), entry_signature(forward.value()));
  CT_CHECK_EQ(impact_signature(forward_again.value()), impact_signature(forward.value()));
  CT_CHECK_EQ(forward_again.value().node_delta, forward.value().node_delta);
  CT_CHECK_EQ(forward_again.value().edge_delta, forward.value().edge_delta);

  // Seeded permutation of the after draft tables must not change the diff: the
  // answer is a property of the two generations, not of the producer's listing
  // order.
  const std::string case_name = "diff_explanation_is_deterministic_and_orientation_sensitive";
  Rng rng(case_seed(case_name));
  for (std::size_t round = 0; round < 8; ++round) {
    ct::TopologyDraft shuffled = after_draft;
    for (std::size_t remaining = shuffled.nodes.size(); remaining > 1; --remaining) {
      std::swap(shuffled.nodes[remaining - 1], shuffled.nodes[rng.below(static_cast<std::uint32_t>(remaining))]);
    }
    for (std::size_t remaining = shuffled.edges.size(); remaining > 1; --remaining) {
      std::swap(shuffled.edges[remaining - 1], shuffled.edges[rng.below(static_cast<std::uint32_t>(remaining))]);
    }
    const ct::Topology permuted = next_generation(before, shuffled);
    CT_CHECK(permuted.digest() == after.digest());
    const auto permuted_diff = ct::diff_topologies(before, permuted);
    CT_REQUIRE(permuted_diff.has_value());
    if (entry_signature(permuted_diff.value()) != entry_signature(forward.value())) {
      report_note("round " + std::to_string(round) + " diverged; rerun with --seed=" + std::to_string(run_seed()) +
                  " (case seed " + std::to_string(case_seed(case_name)) + ")");
    }
    CT_CHECK_EQ(entry_signature(permuted_diff.value()), entry_signature(forward.value()));
  }

  // The two orientations are not the same list: the same subject changes kind.
  CT_REQUIRE(forward.value().entries.size() == std::size_t{1});
  CT_REQUIRE(backward.value().entries.size() == std::size_t{1});
  CT_CHECK(forward.value().entries.front().kind == ct::DiffChangeKind::NodeAdded);
  CT_CHECK(backward.value().entries.front().kind == ct::DiffChangeKind::NodeRemoved);
  CT_CHECK_EQ(forward.value().entries.front().subject, backward.value().entries.front().subject);
  CT_CHECK(forward.value().entries.front().kind != backward.value().entries.front().kind);
  CT_CHECK_EQ(forward.value().node_delta, 1);
  CT_CHECK_EQ(backward.value().node_delta, -1);

  // The explanation is deterministic, non-empty and carries the claim boundary.
  const std::vector<std::string> lines = ct::explain_diff(forward.value());
  CT_CHECK(!lines.empty());
  CT_CHECK_EQ(join_strings(lines), join_strings(ct::explain_diff(forward.value())));
  CT_CHECK_EQ(join_strings(ct::explain_diff(forward.value())), join_strings(ct::explain_diff(forward_again.value())));
  bool saw_entry = false;
  bool saw_boundary = false;
  bool saw_generation = false;
  for (const std::string& line : lines) {
    saw_entry = saw_entry || line == "node_added plant:spare";
    saw_boundary = saw_boundary || line.find(std::string(ct::posture_statement())) != std::string::npos;
    saw_generation = saw_generation || line.rfind("generation ", 0) == 0;
  }
  CT_CHECK(saw_entry);
  CT_CHECK(saw_boundary);
  CT_CHECK(saw_generation);

  // A removal case also differs by kind in the two orientations.
  const auto removal_forward = ct::diff_topologies(after, before);
  CT_REQUIRE(removal_forward.has_value());
  CT_CHECK(find_entry(removal_forward.value(), ct::DiffChangeKind::NodeRemoved, "plant:spare") != nullptr);
  CT_CHECK(find_entry(forward.value(), ct::DiffChangeKind::NodeRemoved, "plant:spare") == nullptr);

  // Deltas are the signed difference of the two tables and sum to zero across
  // the two orientations.
  CT_CHECK_EQ(forward.value().node_delta + backward.value().node_delta, 0);
  CT_CHECK_EQ(forward.value().edge_delta + backward.value().edge_delta, 0);

  // The explanation of an empty diff still explains the two generation bindings.
  const auto identical = ct::diff_topologies(before, before);
  CT_REQUIRE(identical.has_value());
  const std::vector<std::string> identical_lines = ct::explain_diff(identical.value());
  CT_CHECK(identical_lines.size() < lines.size());
  for (const std::string& line : identical_lines) {
    CT_CHECK(!line.empty());
  }
}

}  // namespace
