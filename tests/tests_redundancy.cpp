// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_redundancy.cpp - proof obligations for declared redundancy: membership,
// structural source sets, the declared independence properties and the verdicts.
//
// A redundancy declaration is a statement about what is built. Nothing here
// asserts that a member is ready, that a standby is eligible or that a group can
// carry load: the report is checked to be a structural evaluation of the
// declaration, and every excluded claim stays NotOwned.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/query.hpp"
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

// ---------------------------------------------------------------------------
// The claim boundary
// ---------------------------------------------------------------------------

void check_claim_boundary() {
  static_assert(static_cast<std::uint8_t>(ct::ExcludedClaim::RedundantSourceEligible) == 3);
  static_assert(static_cast<std::uint8_t>(ct::ExcludedClaim::PowerState) == 10);
  static_assert(static_cast<std::uint8_t>(ct::ClaimDisposition::NotOwned) == 0);
  CT_CHECK(!ct::posture_statement().empty());
  for (std::uint8_t index = 0; index < 11; ++index) {
    CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(static_cast<ct::ExcludedClaim>(index)),
                ct::ClaimDisposition::NotOwned);
  }
  CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(ct::ExcludedClaim::RedundantSourceEligible),
              ct::ClaimDisposition::NotOwned);
}

template <class Answer>
void check_posture(const Answer& answer) {
  CT_CHECK_EQ(answer.posture, ct::EvidencePosture{});
}

// ---------------------------------------------------------------------------
// Helpers and independent references
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

const ct::Node* node_of_text(const ct::Topology& topology, const std::string& text) {
  const auto parsed = ct::NodeId::parse(text);
  return parsed.has_value() ? topology.find_node(parsed.value()) : nullptr;
}

/// Structural sources of one element, computed from the raw edge table: supplier
/// kinds with no incoming Supplies edge that can reach the element by following
/// Supplies edges backwards.
std::vector<std::string> reference_sources_of_text(const ct::Topology& topology, const std::string& start) {
  std::vector<std::string> seen{start};
  std::vector<std::string> frontier{start};
  std::vector<std::string> sources;
  while (!frontier.empty()) {
    std::vector<std::string> next;
    for (const std::string& current : frontier) {
      const ct::Node* node = node_of_text(topology, current);
      if (node == nullptr) {
        continue;
      }
      if (reference_supplier_kind(node->kind())) {
        bool fed = false;
        for (const ct::Edge& edge : topology.edges()) {
          if (edge.kind == ct::EdgeKind::Supplies && edge.to.node.str() == current) {
            fed = true;
            break;
          }
        }
        if (!fed) {
          sources.push_back(current);
        }
      }
      for (const ct::Edge& edge : topology.edges()) {
        if (edge.kind != ct::EdgeKind::Supplies || edge.to.node.str() != current) {
          continue;
        }
        const std::string upstream = edge.from.node.str();
        if (std::find(seen.begin(), seen.end(), upstream) != seen.end()) {
          continue;
        }
        seen.push_back(upstream);
        next.push_back(upstream);
      }
    }
    std::sort(next.begin(), next.end());
    frontier.swap(next);
  }
  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return sources;
}

/// Sources of one redundancy member: a pump is installed on a distribution
/// element through a Pumps edge, so its sources are that element's sources.
std::vector<std::string> reference_member_sources(const ct::Topology& topology, const std::string& member) {
  const ct::Node* node = node_of_text(topology, member);
  if (node == nullptr) {
    return {};
  }
  std::string start = node->id.str();
  if (node->kind() == ct::NodeKind::Pump) {
    const std::string pump = start;
    start.clear();
    for (const ct::EdgeId& edge_id : topology.in_edges(node->id)) {
      const ct::Edge* edge = topology.find_edge(edge_id);
      if (edge != nullptr && edge->kind == ct::EdgeKind::Pumps) {
        start = edge->from.node.str();
        break;
      }
    }
    if (start.empty() || start == pump) {
      return {};
    }
  }
  return reference_sources_of_text(topology, start);
}

/// Sources shared by two or more members: the union of the pairwise
/// intersections, sorted.
std::vector<std::string> reference_shared_sources(const ct::Topology& topology,
                                                  const std::vector<std::string>& members) {
  std::vector<std::string> shared;
  for (std::size_t first = 0; first < members.size(); ++first) {
    const std::vector<std::string> left = reference_member_sources(topology, members[first]);
    for (std::size_t second = first + 1; second < members.size(); ++second) {
      const std::vector<std::string> right = reference_member_sources(topology, members[second]);
      for (const std::string& source : left) {
        if (std::find(right.begin(), right.end(), source) != right.end()) {
          shared.push_back(source);
        }
      }
    }
  }
  std::sort(shared.begin(), shared.end());
  shared.erase(std::unique(shared.begin(), shared.end()), shared.end());
  return shared;
}

std::vector<std::string> reference_duplicate_domains(const ct::RedundancyGroup& group) {
  std::vector<std::string> domains;
  for (const ct::RedundancyMember& member : group.members) {
    if (member.failure_domain.has_value()) {
      domains.push_back(member.failure_domain->identity);
    }
  }
  std::sort(domains.begin(), domains.end());
  std::vector<std::string> duplicates;
  for (std::size_t index = 1; index < domains.size(); ++index) {
    if (domains[index] == domains[index - 1]) {
      duplicates.push_back(domains[index]);
    }
  }
  return duplicates;
}

// -- draft builders ---------------------------------------------------------

ct::RedundancyMember member(std::string_view node, std::string_view declared,
                            std::optional<ct::ExternalRef> domain = std::nullopt) {
  ct::RedundancyMember value;
  value.node = nid(node);
  value.declared = std::string(declared);
  value.failure_domain = std::move(domain);
  return value;
}

ct::RedundancyGroup group_of(std::string_view id, ct::RedundancyScheme scheme, ct::RedundancyScope scope,
                             std::vector<ct::RedundancyMember> members, bool distinct_domains,
                             bool independent_sources) {
  auto created = ct::RedundancyGroup::create(gid(id), scheme, scope, "declared group", "structural declaration",
                                             std::move(members), distinct_domains, independent_sources);
  CT_REQUIRE(created.has_value());
  return *created;
}

/// The node kind of one draft element, for the hand-written scope table.
ct::NodeKind draft_kind(const ct::TopologyDraft& draft, std::string_view identity) {
  for (const ct::Node& node : draft.nodes) {
    if (node.id.value() == identity) {
      return node.kind();
    }
  }
  return ct::NodeKind::ThermalZone;
}

void replace_group(ct::TopologyDraft& draft, const ct::RedundancyGroup& replacement) {
  for (ct::RedundancyGroup& group : draft.groups) {
    if (group.id == replacement.id) {
      group = replacement;
      return;
    }
  }
  draft.groups.push_back(replacement);
}

/// The reference facility plus an alias for the first plant.
ct::TopologyDraft reference_with_plant_alias() {
  ct::TopologyDraft draft = reference_facility();
  ct::Alias alias;
  alias.id = aid("alias:plant-a");
  alias.target = nid("plant:chp-a");
  draft.aliases.push_back(alias);
  return draft;
}

/// A plant scope group over the two reference plants.
ct::RedundancyGroup plant_group(bool distinct_domains, bool independent_sources,
                                std::optional<ct::ExternalRef> domain_a = std::nullopt,
                                std::optional<ct::ExternalRef> domain_b = std::nullopt) {
  return group_of("group:plants", ct::RedundancyScheme::NPlusOne, ct::RedundancyScope::Plant,
                  {member("plant:chp-a", "plant:chp-a", std::move(domain_a)),
                   member("plant:chp-b", "plant:chp-b", std::move(domain_b))},
                  distinct_domains, independent_sources);
}

/// Gives the second plant its own facility-water source: the shared feed is
/// removed and replaced, so the two plants no longer trace back to one source.
void give_second_plant_its_own_source(ct::TopologyDraft& draft) {
  draft.edges.erase(std::remove_if(draft.edges.begin(), draft.edges.end(),
                                   [](const ct::Edge& edge) { return edge.id == eid("e:src-plant-b"); }),
                    draft.edges.end());
  draft.nodes.push_back(source_node("source:fw-b"));
  draft.edges.push_back(edge("e:fw-b-plant-b", ct::EdgeKind::Supplies, "source:fw-b", ct::PortRole::SupplyOut,
                             "plant:chp-b", ct::PortRole::SourceIn));
}

// ---------------------------------------------------------------------------
// Membership
// ---------------------------------------------------------------------------

CT_TEST(redundancy_membership_lists_groups_in_canonical_order) {
  ct::TopologyDraft draft = reference_with_plant_alias();
  draft.groups.push_back(group_of("group:alpha-plants", ct::RedundancyScheme::TwoN, ct::RedundancyScope::Plant,
                                  {member("plant:chp-a", "alias:plant-a")}, false, false));
  const ct::Topology topology = build(draft);

  const auto membership = ct::redundancy_membership(topology, nid("plant:chp-a"));
  CT_REQUIRE(membership.has_value());
  check_posture(membership.value());
  CT_CHECK(membership.value().node == nid("plant:chp-a"));
  CT_REQUIRE(membership.value().memberships.size() == std::size_t{2});
  // Canonical group order, member index and declared spelling.
  CT_CHECK_EQ(membership.value().memberships[0].group.str(), std::string("group:alpha-plants"));
  CT_CHECK_EQ(membership.value().memberships[0].member_index, std::size_t{0});
  CT_CHECK_EQ(membership.value().memberships[0].declared, std::string("alias:plant-a"));
  CT_CHECK_EQ(membership.value().memberships[1].group.str(), std::string("group:plants"));
  CT_CHECK_EQ(membership.value().memberships[1].member_index, std::size_t{0});
  CT_CHECK_EQ(membership.value().memberships[1].declared, std::string("plant:chp-a"));

  // An alias spelling resolves to the canonical node and answers identically.
  const auto via_alias = ct::redundancy_membership(topology, nid("alias:plant-a"));
  CT_REQUIRE(via_alias.has_value());
  CT_CHECK(via_alias.value().node == nid("plant:chp-a"));
  CT_CHECK_EQ(via_alias.value().memberships.size(), membership.value().memberships.size());

  // The member index is the position inside the group, not a constant.
  const auto second = ct::redundancy_membership(topology, nid("plant:chp-b"));
  CT_REQUIRE(second.has_value());
  CT_REQUIRE(second.value().memberships.size() == std::size_t{1});
  CT_CHECK_EQ(second.value().memberships.front().member_index, std::size_t{1});
  CT_CHECK_EQ(second.value().memberships.front().declared, std::string("plant:chp-b"));
  const auto pump = ct::redundancy_membership(topology, nid("pump:p-1"));
  CT_REQUIRE(pump.has_value());
  CT_REQUIRE(pump.value().memberships.size() == std::size_t{1});
  CT_CHECK_EQ(pump.value().memberships.front().group.str(), std::string("group:pumps"));
  CT_CHECK_EQ(pump.value().memberships.front().declared, std::string("pump:p-1"));

  // A node in no group has an empty membership list, and an absent identity is
  // not found at all.
  const auto lonely = ct::redundancy_membership(topology, nid("sink:rack-a1"));
  CT_REQUIRE(lonely.has_value());
  CT_CHECK(lonely.value().memberships.empty());
  CT_CHECK(lonely.value().node == nid("sink:rack-a1"));
  CT_CHECK_CODE(ct::redundancy_membership(topology, nid("plant:nobody")), ct::ErrorCode::NotFound);
  CT_CHECK_CODE(ct::redundancy_group_report(topology, gid("group:nobody")), ct::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Group report
// ---------------------------------------------------------------------------

CT_TEST(redundancy_group_report_agrees_with_an_independent_source_set) {
  const ct::Topology topology = build(reference_facility());
  const auto report = ct::redundancy_group_report(topology, gid("group:plants"));
  CT_REQUIRE(report.has_value());
  check_posture(report.value());
  CT_CHECK_EQ(report.value().group.str(), std::string("group:plants"));
  CT_CHECK_EQ(report.value().scheme, ct::RedundancyScheme::NPlusOne);
  CT_CHECK_EQ(report.value().scope, ct::RedundancyScope::Plant);
  CT_CHECK(!report.value().truncated);
  CT_REQUIRE(report.value().members.size() == std::size_t{2});

  const std::vector<std::string> names{"plant:chp-a", "plant:chp-b"};
  for (std::size_t index = 0; index < report.value().members.size(); ++index) {
    CT_CHECK_EQ(report.value().members[index].node.str(), names[index]);
    CT_CHECK_EQ(report.value().members[index].declared, names[index]);
    CT_CHECK_EQ(join_ids(report.value().members[index].sources),
                join_strings(reference_member_sources(topology, names[index])));
    CT_CHECK(!report.value().members[index].failure_domain_declared);
    CT_CHECK(report.value().members[index].failure_domain.empty());
  }
  CT_CHECK_EQ(join_ids(report.value().members[0].sources), std::string("source:facility-water"));
  CT_CHECK_EQ(join_ids(report.value().shared_sources),
              join_strings(reference_shared_sources(topology, names)));
  CT_CHECK_EQ(join_ids(report.value().shared_sources), std::string("source:facility-water"));
  CT_CHECK(report.value().ancestor_members.empty());
  CT_CHECK(report.value().duplicate_failure_domains.empty());
  CT_CHECK(report.value().independence_holds);
  // No independence property is declared, so the graph cannot decide: the
  // verdict is deliberately not "consistent".
  CT_CHECK(report.value().verdict == ct::RedundancyVerdict::IndependenceUnproven);

  // The pump group resolves each pump through its installation site, and the
  // distribution group through its manifold; both are computed independently.
  const auto pump_report = ct::redundancy_group_report(topology, gid("group:pumps"));
  CT_REQUIRE(pump_report.has_value());
  CT_CHECK_EQ(join_ids(pump_report.value().members[0].sources),
              join_strings(reference_member_sources(topology, "pump:p-1")));
  CT_CHECK_EQ(join_ids(pump_report.value().members[1].sources),
              join_strings(reference_member_sources(topology, "pump:p-2")));
  CT_CHECK_EQ(join_ids(pump_report.value().shared_sources), std::string("source:facility-water"));
  CT_CHECK(pump_report.value().verdict == ct::RedundancyVerdict::IndependenceUnproven);

  const auto distribution = ct::redundancy_group_report(topology, gid("group:manifolds"));
  CT_REQUIRE(distribution.has_value());
  CT_CHECK_EQ(join_ids(distribution.value().members[0].sources),
              join_strings(reference_member_sources(topology, "manifold:m-a")));
  CT_CHECK_EQ(join_ids(distribution.value().members[1].sources),
              join_strings(reference_member_sources(topology, "manifold:m-b")));
  CT_CHECK(distribution.value().verdict == ct::RedundancyVerdict::IndependenceUnproven);

  // Declared duplicate failure domains are reported even when the group does not
  // require them to be distinct.
  ct::TopologyDraft duplicated = reference_facility();
  const ct::ExternalRef domain = extref("fd-a", ct::ExternalRefKind::FailureDomain);
  replace_group(duplicated, plant_group(false, false, domain, domain));
  const ct::Topology with_duplicates = build(duplicated);
  const auto duplicate_report = ct::redundancy_group_report(with_duplicates, gid("group:plants"));
  CT_REQUIRE(duplicate_report.has_value());
  CT_CHECK_EQ(join_strings(duplicate_report.value().duplicate_failure_domains),
              join_strings(reference_duplicate_domains(*with_duplicates.find_group(gid("group:plants")))));
  CT_CHECK_EQ(join_strings(duplicate_report.value().duplicate_failure_domains), std::string("fd-a"));
  CT_CHECK(duplicate_report.value().members[0].failure_domain_declared);
  CT_CHECK_EQ(duplicate_report.value().members[0].failure_domain, std::string("fd-a"));
  CT_CHECK(duplicate_report.value().independence_holds);

  // A member without a structural source contradicts the declaration, so the
  // verdict is a violation.
  ct::TopologyDraft unproven = reference_facility();
  unproven.nodes.push_back(crac_node("crac:spare"));
  unproven.groups.push_back(group_of("group:air", ct::RedundancyScheme::NPlusOne,
                                     ct::RedundancyScope::AirHandling,
                                     {member("crah:ir-1", "crah:ir-1"), member("crac:spare", "crac:spare")},
                                     false, false));
  const ct::Topology with_unproven = build(unproven);
  const auto violation = ct::redundancy_group_report(with_unproven, gid("group:air"));
  CT_REQUIRE(violation.has_value());
  CT_CHECK_EQ(violation.value().members[0].node.str(), std::string("crac:spare"));
  CT_CHECK(violation.value().members[0].sources.empty());
  CT_CHECK(!violation.value().independence_holds);
  CT_CHECK(violation.value().verdict == ct::RedundancyVerdict::DeclarationViolated);
  CT_CHECK(report.value().verdict != violation.value().verdict);
}

// ---------------------------------------------------------------------------
// require_independent_sources
// ---------------------------------------------------------------------------

CT_TEST(redundancy_independent_sources_need_two_distinct_sources) {
  // Both plants trace back to the same facility-water source: the declared
  // independence is unproven and the draft is rejected.
  ct::TopologyDraft same_source = reference_facility();
  replace_group(same_source, plant_group(false, true));
  CT_CHECK_CODE(try_build(same_source), ct::ErrorCode::GroupIndependentPathUnproven);

  // A second, independent source makes the declaration true and the same group
  // is accepted and reported as consistent.
  ct::TopologyDraft different_sources = reference_facility();
  give_second_plant_its_own_source(different_sources);
  replace_group(different_sources, plant_group(false, true));
  const auto accepted = try_build(different_sources);
  CT_REQUIRE(accepted.has_value());
  const auto report = ct::redundancy_group_report(accepted.value(), gid("group:plants"));
  CT_REQUIRE(report.has_value());
  CT_CHECK(report.value().shared_sources.empty());
  CT_CHECK(report.value().independence_holds);
  CT_CHECK(report.value().verdict == ct::RedundancyVerdict::DeclarationConsistent);
  CT_CHECK_EQ(join_ids(report.value().members[0].sources), std::string("source:facility-water"));
  CT_CHECK_EQ(join_ids(report.value().members[1].sources), std::string("source:fw-b"));

  // The same draft without the declaration flag is accepted either way; the
  // flag is what makes the difference, not the graph.
  ct::TopologyDraft same_source_no_flag = reference_facility();
  replace_group(same_source_no_flag, plant_group(false, false));
  CT_CHECK(try_build(same_source_no_flag).has_value());

  // A member with no structural source at all also makes the declaration
  // unprovable.
  ct::TopologyDraft sourceless = reference_facility();
  sourceless.nodes.push_back(crac_node("crac:spare"));
  sourceless.groups.push_back(group_of("group:air", ct::RedundancyScheme::NPlusOne,
                                       ct::RedundancyScope::AirHandling,
                                       {member("crah:ir-1", "crah:ir-1"), member("crac:spare", "crac:spare")},
                                       false, true));
  CT_CHECK_CODE(try_build(sourceless), ct::ErrorCode::GroupIndependentPathUnproven);
}

// ---------------------------------------------------------------------------
// require_distinct_failure_domains
// ---------------------------------------------------------------------------

CT_TEST(redundancy_distinct_failure_domains_are_enforced) {
  const ct::ExternalRef domain_a = extref("fd-a", ct::ExternalRefKind::FailureDomain);
  const ct::ExternalRef domain_b = extref("fd-b", ct::ExternalRefKind::FailureDomain);

  // Declaring the property while a member declares no domain: rejected.
  ct::TopologyDraft missing = reference_facility();
  replace_group(missing, plant_group(true, false, domain_a, std::nullopt));
  CT_CHECK_CODE(try_build(missing), ct::ErrorCode::GroupRedundancyUnproven);

  // Two members in the same declared failure domain: rejected.
  ct::TopologyDraft duplicated = reference_facility();
  replace_group(duplicated, plant_group(true, false, domain_a, domain_a));
  CT_CHECK_CODE(try_build(duplicated), ct::ErrorCode::GroupRedundancyUnproven);

  // Distinct declared domains satisfy the property.
  ct::TopologyDraft distinct = reference_facility();
  replace_group(distinct, plant_group(true, false, domain_a, domain_b));
  const auto accepted = try_build(distinct);
  CT_REQUIRE(accepted.has_value());
  const auto report = ct::redundancy_group_report(accepted.value(), gid("group:plants"));
  CT_REQUIRE(report.has_value());
  CT_CHECK(report.value().duplicate_failure_domains.empty());
  CT_CHECK(report.value().independence_holds);
  CT_CHECK(report.value().verdict == ct::RedundancyVerdict::DeclarationConsistent);
  CT_CHECK_EQ(report.value().members[0].failure_domain, std::string("fd-a"));
  CT_CHECK_EQ(report.value().members[1].failure_domain, std::string("fd-b"));
  // Distinct failure domains do not make the source sets independent: the two
  // properties are checked separately.
  CT_CHECK_EQ(join_ids(report.value().shared_sources), std::string("source:facility-water"));

  // The boundary between the two properties: distinct domains and independent
  // sources together are consistent only with two real sources.
  ct::TopologyDraft both = reference_facility();
  give_second_plant_its_own_source(both);
  replace_group(both, plant_group(true, true, domain_a, domain_b));
  const auto both_accepted = try_build(both);
  CT_REQUIRE(both_accepted.has_value());
  const auto both_report = ct::redundancy_group_report(both_accepted.value(), gid("group:plants"));
  CT_REQUIRE(both_report.has_value());
  CT_CHECK(both_report.value().duplicate_failure_domains.empty());
  CT_CHECK(both_report.value().shared_sources.empty());
  CT_CHECK(both_report.value().independence_holds);
  CT_CHECK(both_report.value().verdict == ct::RedundancyVerdict::DeclarationConsistent);
}

// ---------------------------------------------------------------------------
// Scope and member kind
// ---------------------------------------------------------------------------

/// The documented (scope, member kind) relation, written out by hand.
bool reference_scope_allows(ct::RedundancyScope scope, ct::NodeKind kind) {
  switch (scope) {
    case ct::RedundancyScope::Plant:
      return kind == ct::NodeKind::CoolingPlant;
    case ct::RedundancyScope::Chiller:
      return kind == ct::NodeKind::Chiller;
    case ct::RedundancyScope::Pump:
      return kind == ct::NodeKind::Pump;
    case ct::RedundancyScope::CoolingLoop:
      return kind == ct::NodeKind::CoolingLoop;
    case ct::RedundancyScope::Distribution:
      return kind == ct::NodeKind::Manifold || kind == ct::NodeKind::Branch || kind == ct::NodeKind::Cdu;
    case ct::RedundancyScope::SourcePath:
      return kind == ct::NodeKind::CoolingSource;
    case ct::RedundancyScope::AirHandling:
      return kind == ct::NodeKind::Crah || kind == ct::NodeKind::Crac;
    default:
      return false;
  }
}

CT_TEST(redundancy_scope_member_kind_table_is_total) {
  // Every declared scope against every declared node kind.
  for (std::uint8_t scope_index = 0; scope_index <= static_cast<std::uint8_t>(ct::RedundancyScope::AirHandling);
       ++scope_index) {
    const auto scope = static_cast<ct::RedundancyScope>(scope_index);
    for (std::uint8_t kind_index = 0; kind_index <= static_cast<std::uint8_t>(ct::NodeKind::CoolingSource);
         ++kind_index) {
      const auto kind = static_cast<ct::NodeKind>(kind_index);
      CT_CHECK_EQ(ct::redundancy_scope_allows(scope, kind), reference_scope_allows(scope, kind));
    }
    // A kind that no scope allows is nonetheless a legal member kind unless it
    // is a thermal zone.
    CT_CHECK(!ct::redundancy_scope_allows(scope, ct::NodeKind::ThermalZone));
  }
  for (std::uint8_t kind_index = 0; kind_index <= static_cast<std::uint8_t>(ct::NodeKind::CoolingSource);
       ++kind_index) {
    const auto kind = static_cast<ct::NodeKind>(kind_index);
    CT_CHECK_EQ(ct::is_redundancy_member_kind(kind), kind != ct::NodeKind::ThermalZone);
  }
}

CT_TEST(redundancy_scope_violations_are_rejected) {
  // One violating (scope, member kind) pair per declared scope. The member kind
  // is a legal member kind and exists in the reference facility, so the only
  // defect is the scope.
  struct Case {
    ct::RedundancyScope scope;
    std::string_view member;
  };
  const Case cases[] = {
      {ct::RedundancyScope::Plant, "manifold:m-a"},
      {ct::RedundancyScope::Chiller, "manifold:m-a"},
      {ct::RedundancyScope::Pump, "manifold:m-a"},
      {ct::RedundancyScope::CoolingLoop, "manifold:m-a"},
      {ct::RedundancyScope::Distribution, "plant:chp-a"},
      {ct::RedundancyScope::SourcePath, "plant:chp-a"},
      {ct::RedundancyScope::AirHandling, "plant:chp-a"},
  };
  for (const Case& entry : cases) {
    ct::TopologyDraft draft = reference_facility();
    draft.groups = {group_of("group:scope", ct::RedundancyScheme::NPlusOne, entry.scope,
                             {member(entry.member, entry.member)}, false, false)};
    // The hand-written table agrees that this pair is not allowed.
    CT_CHECK(!reference_scope_allows(entry.scope, draft_kind(draft, entry.member)));
    CT_CHECK_CODE(try_build(draft), ct::ErrorCode::GroupScopeInvalid);
  }

  // A group that mixes an allowed member with a disallowed one is rejected too.
  ct::TopologyDraft mixed = reference_facility();
  mixed.groups.push_back(group_of("group:mixed", ct::RedundancyScheme::TwoN, ct::RedundancyScope::Plant,
                                  {member("plant:chp-a", "plant:chp-a"), member("manifold:m-a", "manifold:m-a")},
                                  false, false));
  CT_CHECK_CODE(try_build(mixed), ct::ErrorCode::GroupScopeInvalid);

  // The allowed pair is accepted, so the rejection above is the scope and not
  // some other defect of the fixture.
  ct::TopologyDraft allowed = reference_facility();
  allowed.groups.push_back(group_of("group:allowed", ct::RedundancyScheme::TwoN, ct::RedundancyScope::Plant,
                                    {member("plant:chp-a", "plant:chp-a")}, false, false));
  CT_CHECK(try_build(allowed).has_value());
}

// ---------------------------------------------------------------------------
// Member identity rules
// ---------------------------------------------------------------------------

CT_TEST(redundancy_member_identity_defects_are_rejected) {
  // A thermal zone can never be a member, whatever the scope.
  ct::TopologyDraft zone_member = reference_facility();
  zone_member.groups.push_back(group_of("group:zone", ct::RedundancyScheme::N, ct::RedundancyScope::Plant,
                                        {member("zone:cold-aisle-1", "zone:cold-aisle-1")}, false, false));
  CT_CHECK_CODE(try_build(zone_member), ct::ErrorCode::GroupMemberKindInvalid);

  // The same node counted twice - once directly and once through an alias - is
  // one member, not two.
  ct::TopologyDraft double_count = reference_with_plant_alias();
  double_count.groups.push_back(group_of("group:double", ct::RedundancyScheme::TwoN, ct::RedundancyScope::Plant,
                                         {member("plant:chp-a", "plant:chp-a"),
                                          member("alias:plant-a", "alias:plant-a")},
                                         false, false));
  CT_CHECK_CODE(try_build(double_count), ct::ErrorCode::GroupMemberDuplicate);

  // The same canonical identity spelled twice is the same defect.
  ct::TopologyDraft same_spelling = reference_facility();
  same_spelling.groups.push_back(group_of("group:same", ct::RedundancyScheme::TwoN, ct::RedundancyScope::Plant,
                                          {member("plant:chp-a", "plant:chp-a"),
                                           member("plant:chp-a", "plant:chp-a")},
                                          false, false));
  CT_CHECK_CODE(try_build(same_spelling), ct::ErrorCode::GroupMemberDuplicate);

  // A member that names nothing at all is rejected before any graph question.
  ct::TopologyDraft missing_member = reference_facility();
  missing_member.groups.push_back(group_of("group:missing", ct::RedundancyScheme::TwoN,
                                           ct::RedundancyScope::Plant,
                                           {member("plant:chp-a", "plant:chp-a"),
                                            member("plant:nobody", "plant:nobody")},
                                           false, false));
  CT_CHECK_CODE(try_build(missing_member), ct::ErrorCode::GroupMemberMissing);

  // A group with no members cannot even be constructed.
  CT_CHECK_CODE(ct::RedundancyGroup::create(gid("group:empty"), ct::RedundancyScheme::N,
                                            ct::RedundancyScope::Plant, "empty", "no members", {}, false, false),
                ct::ErrorCode::GroupEmpty);

  // The same double count through two distinct aliases of one node.
  ct::TopologyDraft two_aliases = reference_with_plant_alias();
  ct::Alias second_alias;
  second_alias.id = aid("alias:plant-a-legacy");
  second_alias.target = nid("plant:chp-a");
  two_aliases.aliases.push_back(second_alias);
  two_aliases.groups.push_back(group_of("group:two-aliases", ct::RedundancyScheme::TwoN,
                                        ct::RedundancyScope::Plant,
                                        {member("alias:plant-a", "alias:plant-a"),
                                         member("alias:plant-a-legacy", "alias:plant-a-legacy")},
                                        false, false));
  CT_CHECK_CODE(try_build(two_aliases), ct::ErrorCode::GroupMemberDuplicate);
}

// ---------------------------------------------------------------------------
// Structural only
// ---------------------------------------------------------------------------

CT_TEST(redundancy_verdicts_are_structural_only) {
  check_claim_boundary();
  // The verdict vocabulary has exactly three values, all structural: there is no
  // value that could mean "ready", "available" or "eligible".
  static_assert(static_cast<std::uint8_t>(ct::RedundancyVerdict::DeclarationConsistent) == 0);
  static_assert(static_cast<std::uint8_t>(ct::RedundancyVerdict::DeclarationViolated) == 1);
  static_assert(static_cast<std::uint8_t>(ct::RedundancyVerdict::IndependenceUnproven) == 2);
  for (std::uint8_t index = 0; index < 3; ++index) {
    const auto verdict = static_cast<ct::RedundancyVerdict>(index);
    CT_CHECK(!ct::to_token(verdict).empty());
    CT_CHECK(ct::to_token(verdict) != std::string_view("unknown"));
  }

  // The group report itself carries the posture and nothing operational.
  const ct::Topology topology = build(reference_facility());
  const auto report = ct::redundancy_group_report(topology, gid("group:plants"));
  CT_REQUIRE(report.has_value());
  check_posture(report.value());
  // Structural statements only: membership, sources, shared sources, ancestor
  // members, duplicate domain identities and the declaration verdict.
  CT_CHECK_EQ(report.value().members.size(), std::size_t{2});
  CT_CHECK(!report.value().independence_holds || report.value().members.size() >= std::size_t{1});
  CT_CHECK_EQ(ct::EvidencePosture::claim_disposition(ct::ExcludedClaim::RedundantSourceEligible),
              ct::ClaimDisposition::NotOwned);
}

// ---------------------------------------------------------------------------
// Order independence
// ---------------------------------------------------------------------------

CT_TEST(redundancy_group_member_order_does_not_change_the_report) {
  const std::string case_name = "redundancy_group_member_order_does_not_change_the_report";
  const ct::TopologyDraft base = reference_facility();
  const ct::Topology reference = build(base);
  const auto reference_report = ct::redundancy_group_report(reference, gid("group:plants"));
  CT_REQUIRE(reference_report.has_value());

  const auto signature = [](const ct::RedundancyGroupReport& report) {
    std::string out;
    out += report.group.str();
    out += '|';
    for (const ct::RedundancyMemberReport& entry : report.members) {
      out += entry.node.str();
      out += ':';
      out += entry.declared;
      out += ':';
      out += join_ids(entry.sources);
      out += ';';
    }
    out += '|';
    out += join_ids(report.shared_sources);
    out += '|';
    out += join_strings(report.duplicate_failure_domains);
    out += '|';
    out += report.independence_holds ? "holds" : "unproven";
    out += '|';
    out += std::string(ct::to_token(report.verdict));
    return out;
  };

  Rng rng(case_seed(case_name));
  const std::string expected = signature(reference_report.value());
  for (std::size_t round = 0; round < 8; ++round) {
    ct::TopologyDraft shuffled = base;
    for (ct::RedundancyGroup& group : shuffled.groups) {
      std::vector<ct::RedundancyMember>& members = group.members;
      for (std::size_t remaining = members.size(); remaining > 1; --remaining) {
        std::swap(members[remaining - 1], members[rng.below(static_cast<std::uint32_t>(remaining))]);
      }
    }
    const ct::Topology permuted = build(shuffled);
    const auto permuted_report = ct::redundancy_group_report(permuted, gid("group:plants"));
    CT_REQUIRE(permuted_report.has_value());
    if (signature(permuted_report.value()) != expected) {
      report_note("round " + std::to_string(round) + " diverged; rerun with --seed=" + std::to_string(run_seed()));
    }
    CT_CHECK_EQ(signature(permuted_report.value()), expected);
    CT_CHECK(permuted.digest() == reference.digest());
  }
}

}  // namespace
