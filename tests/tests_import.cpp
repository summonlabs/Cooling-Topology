// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Proof obligations for the ctg1 text grammar: every documented command and key
// is exercised, the exporter re-parses to the same canonical digest, quoted
// tokens carrying spaces, '#' and '=' survive, every malformed shape is
// rejected with its stable ErrorCode and the offending line, and every bound is
// enforced before any work proportional to a declared count is done.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;

void expect_code(const ct::Error& error, ct::ErrorCode expected, const std::string& what) {
  CT_CHECK_MSG(error.code() == expected,
               what + ": expected " + std::string(ct::error_code_name(expected)) + ", observed " +
                   std::string(ct::error_code_name(error.code())) + " [" + error.to_string() + "]");
}

void expect_line(const ct::Error& error, std::size_t line, const std::string& what) {
  const std::string expected = "line " + std::to_string(line);
  CT_CHECK_MSG(error.subject() == expected, what + ": expected subject \"" + expected + "\", observed \"" +
                                                error.subject() + "\" (" +
                                                std::string(ct::error_code_name(error.code())) + ": " +
                                                error.message() + ")");
}

std::string raw_bytes(std::initializer_list<unsigned int> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned int value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFFu)));
  }
  return out;
}

/// Two well-formed header lines: a defect appended after them sits on line 3.
std::string header_lines() {
  return std::string("facility facility:dc-import@3\n") +
         "provenance producer=\"dccp-cooling-topology/1.0.0\" origin=authored witness=\"fixture\"\n";
}

/// One well-formed header line, so a provenance defect appended after it sits on
/// line 2 and is not shadowed by the duplicate-provenance check.
std::string facility_line() { return std::string("facility facility:dc-import@3\n"); }

/// A document that exercises every documented command and every documented key:
/// all twelve node kinds, all six edge kinds, an alias, a group with both flags,
/// a scheme, a name, a basis and per-member failure domains, a changeover group
/// and two evidence bindings. Node identities avoid the ':' separator inside
/// group member tokens so that a member spelling is unambiguous.
std::string every_command_document() {
  return std::string("facility facility:dc-import@3\n") +
         "provenance producer=\"dccp-cooling-topology/1.0.0\" origin=authored witness=\"fixture\" "
         "source=registry:base@9 authority-epoch=12\n" +
         "evidence operating_state producer=\"device-control/1.0.0\" subject=asset:ch-1 generation=5 "
         "observation=6\n" +
         "evidence power_state_observation producer=\"power-topology/1.0.0\" subject=asset:pdu-1\n" +
         "node plant-a cooling_plant kind=chiller_plant medium=chilled_water design-supply=7.000 "
         "max-supply=12.500 name=\"PlantA\" ref=location:row-1\n" +
         "node chiller-c1 chiller kind=centrifugal medium=chilled_water design-supply=7.000 max-supply=12.500\n" +
         "node pump-p1 pump kind=centrifugal role=duty\n" +
         "node loop-sec cooling_loop kind=secondary medium=chilled_water design-supply=7.000 max-supply=12.500\n" +
         "node cdu-c1 cdu kind=rack_cdu medium=chilled_water design-supply=7.000 max-supply=12.500\n" +
         "node crah-ir1 crah placement=in_row medium=chilled_water max-supply=18.000\n" +
         "node crac-cr1 crac placement=perimeter medium=chilled_water max-supply=18.000\n" +
         "node manifold-m1 manifold kind=combined medium=chilled_water design-supply=7.000 max-supply=12.500\n" +
         "node branch-b1 branch kind=rack_branch medium=chilled_water design-supply=7.000 max-supply=12.500\n" +
         "node zone-z1 thermal_zone class=cold_aisle\n" +
         "node sink-r1 cooling_sink kind=rack_load consumer=rack-a1 consumer-kind=rack max-supply=25.000\n" +
         "node source-fw cooling_source kind=facility_water medium=facility_water design-supply=30.000\n" +
         "edge e-supply supplies source-fw.supply_out -> plant-a.source_in\n" +
         "edge e-return returns plant-a.heat_out -> source-fw.return_in\n" +
         "edge e-serve serves crah-ir1.server -> zone-z1.served\n" +
         "edge e-pump pumps loop-sec.supply_out -> pump-p1.terminal\n" +
         "edge e-contain contains plant-a.container -> chiller-c1.contained\n" +
         "edge e-depend depends_on sink-r1.terminal -> branch-b1.terminal\n" +
         "alias a-legacy plant-a\n" +
         "group g-pumps scope=pump scheme=n_plus_one name=\"Pumps\" basis=\"duty+standby\" "
         "require-distinct-failure-domains require-independent-sources "
         "pump-p1@failure_domain:fd-a pump-p2@failure_domain:fd-b\n" +
         "changeover co-1 max=1 name=\"Valve\" manifold-m1.supply_out branch-b1.source_in\n";
}

/// Malformed documents whose defect sits on one known line, used both for the
/// stable-code proof and for the reported-line proof.
struct LineCase {
  const char* label;
  std::string document;
  std::size_t line;
  ct::ErrorCode code;
};

std::vector<LineCase> malformed_line_cases() {
  std::vector<LineCase> cases;
  const std::string header = header_lines();
  const std::string facility = facility_line();
  const auto add = [&cases](const std::string& prefix, const char* label, const std::string& body,
                            std::size_t line, ct::ErrorCode code) {
    cases.push_back(LineCase{label, prefix + body, line, code});
  };
  add(header, "unknown command", "bogus x\n", 3, ct::ErrorCode::UnknownEnumToken);
  add(header, "unknown key", "node n-1 chiller kind=centrifugal bogus=1\n", 3, ct::ErrorCode::UnknownEnumToken);
  add(header, "duplicate key", "node n-1 chiller kind=centrifugal kind=screw\n", 3, ct::ErrorCode::DuplicateField);
  add(header, "missing required key", "node n-1 chiller\n", 3, ct::ErrorCode::MissingField);
  add(header, "empty key value", "node n-1 chiller kind=\n", 3, ct::ErrorCode::MissingField);
  add(header, "unknown node kind", "node n-1 chillerx kind=centrifugal\n", 3, ct::ErrorCode::UnknownEnumToken);
  add(header, "unknown kind token", "node n-1 chiller kind=bogus\n", 3, ct::ErrorCode::UnknownEnumToken);
  add(header, "endpoint without a port", "edge e-1 supplies n-1 -> n-2.source_in\n", 3,
      ct::ErrorCode::MalformedRecord);
  add(header, "endpoint with an unknown port", "edge e-1 supplies n-1.bogus -> n-2.source_in\n", 3,
      ct::ErrorCode::UnknownEnumToken);
  add(header, "endpoint with a malformed node", "edge e-1 supplies n/1.source_in -> n-2.source_in\n", 3,
      ct::ErrorCode::MalformedIdentifier);
  add(header, "edge with too many tokens", "edge e-1 supplies n-1.supply_out -> n-2.source_in extra\n", 3,
      ct::ErrorCode::MalformedRecord);
  add(header, "unknown edge kind", "edge e-1 flows n-1.supply_out -> n-2.source_in\n", 3,
      ct::ErrorCode::UnknownEnumToken);
  add(header, "temperature out of range", "node n-1 chiller kind=centrifugal design-supply=10000.001\n", 3,
      ct::ErrorCode::QuantityOutOfRange);
  add(header, "malformed temperature", "node n-1 chiller kind=centrifugal design-supply=7.00\n", 3,
      ct::ErrorCode::MalformedNumber);
  add(header, "generation out of range", "node n-1 chiller kind=centrifugal ref=asset:a@99999999999999999999\n", 3,
      ct::ErrorCode::LimitExceeded);
  add(header, "unterminated quote", "node n-1 chiller kind=centrifugal name=\"oops\n", 3,
      ct::ErrorCode::MalformedRecord);
  add(header, "non-ASCII unquoted byte", "node n" + raw_bytes({0xC3, 0xA9}) + " chiller kind=centrifugal\n", 3,
      ct::ErrorCode::InvalidUtf8);
  add(header, "control byte in an unquoted token", "node n-1" + raw_bytes({0x01}) + " chiller kind=centrifugal\n", 3,
      ct::ErrorCode::InvalidUtf8);
  add(header, "alias with too many tokens", "alias a-1 a-2 a-3\n", 3, ct::ErrorCode::MalformedRecord);
  add(header, "alias with a malformed target", "alias a-1 n/1\n", 3, ct::ErrorCode::MalformedIdentifier);
  add(header, "group without a scope", "group g-1 pump-p1 pump-p2\n", 3, ct::ErrorCode::MissingField);
  add(header, "group with an unknown scope", "group g-1 scope=bogus pump-p1 pump-p2\n", 3,
      ct::ErrorCode::UnknownEnumToken);
  add(header, "group with an unknown key", "group g-1 scope=pump bogus=1 pump-p1 pump-p2\n", 3,
      ct::ErrorCode::UnknownEnumToken);
  add(header, "group with no members", "group g-1 scope=pump\n", 3, ct::ErrorCode::GroupEmpty);
  add(header, "group with a malformed member", "group g-1 scope=pump -pump p2\n", 3,
      ct::ErrorCode::MalformedIdentifier);
  add(header, "changeover without max", "changeover co-1 a.source_in b.source_in\n", 3, ct::ErrorCode::MissingField);
  add(header, "changeover max not below the member count", "changeover co-1 max=2 a.source_in b.source_in\n", 3,
      ct::ErrorCode::ChangeoverCardinality);
  add(header, "changeover with one member", "changeover co-1 max=1 a.source_in\n", 3,
      ct::ErrorCode::ChangeoverCardinality);
  add(header, "unknown evidence kind", "evidence bogus producer=\"p\" subject=asset:a\n", 3,
      ct::ErrorCode::UnknownEnumToken);
  add(header, "evidence without a subject", "evidence operating_state producer=\"p\"\n", 3,
      ct::ErrorCode::MissingField);
  add(header, "facility with too many tokens", "facility a b\n", 3, ct::ErrorCode::MalformedRecord);
  add(facility, "provenance with a bare token", "provenance producer=\"p\" authored\n", 2,
      ct::ErrorCode::MalformedRecord);
  add(facility, "provenance with an unknown origin", "provenance producer=\"p\" origin=bogus\n", 2,
      ct::ErrorCode::UnknownEnumToken);
  add(facility, "provenance missing the origin key", "provenance producer=\"p\"\n", 2,
      ct::ErrorCode::MissingField);
  add(header, "duplicate facility", "facility facility:dc-import@3\n", 3, ct::ErrorCode::DuplicateField);
  add(header, "duplicate provenance", "provenance producer=\"p\" origin=authored\n", 3,
      ct::ErrorCode::DuplicateField);
  add(header, "duplicate node identity",
      "node n-1 chiller kind=centrifugal\nnode n-1 chiller kind=screw\n", 4, ct::ErrorCode::DuplicateIdentifier);
  add(header, "duplicate evidence binding",
      "evidence operating_state producer=\"p\" subject=asset:a\n"
      "evidence operating_state producer=\"p\" subject=asset:a\n",
      4, ct::ErrorCode::DuplicateField);
  add(header, "line above the byte bound", std::string(ct::limits::kMaxImportLineBytes + 1u, 'x') + "\n", 3,
      ct::ErrorCode::LimitExceeded);
  return cases;
}

}  // namespace

CT_TEST(import_reference_facility_round_trip_preserves_the_digest) {
  const ct::TopologyDraft draft = ct_test::reference_facility();
  auto built = ct_test::try_build(draft);
  CT_REQUIRE(built.has_value());
  const std::string exported = ct::export_import(*built);

  CT_CHECK_MSG(exported.find(std::string(ct::posture_statement())) != std::string::npos,
               "export must print the claim boundary");

  ct::ImportStats stats;
  auto reparsed = ct::parse_import(exported, &stats);
  if (!reparsed.has_value()) {
    ct_test::report_note("re-parse of the exported reference facility failed: " +
                         std::string(ct::error_code_name(reparsed.error().code())) + " subject=\"" +
                         reparsed.error().subject() + "\" " + reparsed.error().message());
  }
  CT_REQUIRE(reparsed.has_value());

  CT_CHECK_EQ(stats.nodes, built->node_count());
  CT_CHECK_EQ(stats.edges, built->edge_count());
  CT_CHECK_EQ(stats.groups, built->group_count());
  CT_CHECK_EQ(stats.aliases, built->aliases().size());
  CT_CHECK_EQ(stats.changeovers, built->changeovers().size());
  CT_CHECK_EQ(stats.evidence, built->provenance().evidence.size());
  CT_CHECK_EQ(stats.lines, 2u + built->node_count() + built->edge_count() + built->group_count() +
                               built->aliases().size() + built->changeovers().size() +
                               built->provenance().evidence.size());

  auto rebuilt = ct_test::try_build(*reparsed);
  if (!rebuilt.has_value()) {
    ct_test::report_note("rebuild of the re-parsed reference facility failed: " +
                         std::string(ct::error_code_name(rebuilt.error().code())) + " subject=\"" +
                         rebuilt.error().subject() + "\" " + rebuilt.error().message());
  }
  CT_REQUIRE(rebuilt.has_value());
  CT_CHECK_MSG(rebuilt->digest() == built->digest(),
               "re-parsed generation digest " + rebuilt->digest().to_hex() + " != " + built->digest().to_hex());
  CT_CHECK(rebuilt->canonical_bytes().value() == built->canonical_bytes().value());
  CT_CHECK(rebuilt->node_count() == built->node_count());
  CT_CHECK(rebuilt->edge_count() == built->edge_count());
  CT_CHECK(rebuilt->group_count() == built->group_count());

  // The rendered form re-parses to the same document body, so a second export is
  // byte-identical.
  CT_CHECK_EQ(ct::export_import(*rebuilt), exported);
}

CT_TEST(import_exercises_every_command_and_key) {
  const std::string document = every_command_document();
  ct::ImportStats stats;
  auto parsed = ct::parse_import(document, &stats);
  if (!parsed.has_value()) {
    ct_test::report_note("every-command document rejected: " +
                         std::string(ct::error_code_name(parsed.error().code())) + " subject=\"" +
                         parsed.error().subject() + "\" " + parsed.error().message());
  }
  CT_REQUIRE(parsed.has_value());

  CT_CHECK_EQ(stats.lines, static_cast<std::size_t>(25));
  CT_CHECK_EQ(stats.nodes, static_cast<std::size_t>(12));
  CT_CHECK_EQ(stats.edges, static_cast<std::size_t>(6));
  CT_CHECK_EQ(stats.groups, static_cast<std::size_t>(1));
  CT_CHECK_EQ(stats.aliases, static_cast<std::size_t>(1));
  CT_CHECK_EQ(stats.changeovers, static_cast<std::size_t>(1));
  CT_CHECK_EQ(stats.evidence, static_cast<std::size_t>(2));

  CT_CHECK_EQ(parsed->facility.kind, ct::ExternalRefKind::Facility);
  CT_CHECK_EQ(parsed->facility.identity, std::string("dc-import"));
  CT_CHECK_EQ(parsed->facility.generation.value(), static_cast<std::uint64_t>(3));
  CT_CHECK_EQ(parsed->provenance.producer, std::string("dccp-cooling-topology/1.0.0"));
  CT_CHECK_EQ(parsed->provenance.origin, ct::ProvenanceOrigin::Authored);
  CT_CHECK_EQ(parsed->provenance.witness, std::string("fixture"));
  CT_REQUIRE(parsed->provenance.source_reference.has_value());
  CT_CHECK_EQ(parsed->provenance.source_reference->identity, std::string("base"));
  CT_CHECK_EQ(parsed->provenance.source_reference->generation.value(), static_cast<std::uint64_t>(9));
  CT_CHECK_EQ(parsed->provenance.authority_epoch.value(), static_cast<std::uint64_t>(12));
  CT_REQUIRE(parsed->provenance.evidence.size() == 2u);
  CT_CHECK_EQ(parsed->provenance.evidence[0].kind, ct::EvidenceKind::OperatingState);
  CT_CHECK_EQ(parsed->provenance.evidence[0].producer, std::string("device-control/1.0.0"));
  CT_CHECK_EQ(parsed->provenance.evidence[0].subject.identity, std::string("ch-1"));
  CT_CHECK_EQ(parsed->provenance.evidence[0].generation.value(), static_cast<std::uint64_t>(5));
  CT_CHECK_EQ(parsed->provenance.evidence[0].observation.value(), static_cast<std::uint64_t>(6));
  CT_CHECK_EQ(parsed->provenance.evidence[1].kind, ct::EvidenceKind::PowerStateObservation);
  CT_CHECK_EQ(parsed->provenance.evidence[1].subject.kind, ct::ExternalRefKind::Asset);

  CT_REQUIRE(parsed->nodes.size() == 12u);
  const auto find = [&parsed](std::string_view id) -> const ct::Node* {
    for (const ct::Node& node : parsed->nodes) {
      if (node.id.value() == id) {
        return &node;
      }
    }
    return nullptr;
  };
  const ct::Node* plant = find("plant-a");
  CT_REQUIRE(plant != nullptr);
  CT_CHECK_EQ(plant->kind(), ct::NodeKind::CoolingPlant);
  CT_CHECK_EQ(plant->display_name, std::string("PlantA"));
  CT_CHECK_EQ(plant->references.size(), static_cast<std::size_t>(1));
  CT_CHECK_EQ(plant->references.front().identity, std::string("row-1"));
  CT_REQUIRE(plant->as_cooling_plant() != nullptr);
  CT_CHECK_EQ(plant->as_cooling_plant()->kind, ct::PlantKind::ChillerPlant);
  CT_CHECK_EQ(plant->as_cooling_plant()->medium.value(), ct::CoolingMedium::ChilledWater);
  CT_CHECK_EQ(plant->as_cooling_plant()->design_supply_temperature->milli_celsius(),
              static_cast<std::int64_t>(7000));
  CT_CHECK_EQ(plant->as_cooling_plant()->max_acceptable_supply_temperature->milli_celsius(),
              static_cast<std::int64_t>(12500));

  const ct::Node* pump = find("pump-p1");
  CT_REQUIRE(pump != nullptr);
  CT_CHECK_EQ(pump->kind(), ct::NodeKind::Pump);
  CT_REQUIRE(pump->as_pump() != nullptr);
  CT_CHECK_EQ(pump->as_pump()->kind, ct::PumpKind::Centrifugal);
  CT_CHECK_EQ(pump->as_pump()->role, ct::PumpRole::Duty);

  const ct::Node* crah = find("crah-ir1");
  CT_REQUIRE(crah != nullptr);
  CT_REQUIRE(crah->as_crah() != nullptr);
  CT_CHECK_EQ(crah->as_crah()->placement, ct::AirHandlerPlacement::InRow);
  CT_CHECK_EQ(crah->as_crah()->max_acceptable_supply_temperature->milli_celsius(),
              static_cast<std::int64_t>(18000));

  const ct::Node* crac = find("crac-cr1");
  CT_REQUIRE(crac != nullptr);
  CT_REQUIRE(crac->as_crac() != nullptr);
  CT_CHECK_EQ(crac->as_crac()->placement, ct::AirHandlerPlacement::Perimeter);

  const ct::Node* zone = find("zone-z1");
  CT_REQUIRE(zone != nullptr);
  CT_REQUIRE(zone->as_thermal_zone() != nullptr);
  CT_CHECK_EQ(zone->as_thermal_zone()->zone_class, ct::ZoneClass::ColdAisle);

  const ct::Node* sink = find("sink-r1");
  CT_REQUIRE(sink != nullptr);
  CT_REQUIRE(sink->as_cooling_sink() != nullptr);
  CT_CHECK_EQ(sink->as_cooling_sink()->kind, ct::SinkKind::RackLoad);
  CT_CHECK_EQ(sink->as_cooling_sink()->consumer.kind, ct::ExternalRefKind::Rack);
  CT_CHECK_EQ(sink->as_cooling_sink()->consumer.identity, std::string("rack-a1"));
  CT_CHECK_EQ(sink->as_cooling_sink()->max_acceptable_supply_temperature->milli_celsius(),
              static_cast<std::int64_t>(25000));

  const ct::Node* source = find("source-fw");
  CT_REQUIRE(source != nullptr);
  CT_REQUIRE(source->as_cooling_source() != nullptr);
  CT_CHECK_EQ(source->as_cooling_source()->kind, ct::SourceKind::FacilityWater);
  CT_CHECK_EQ(source->as_cooling_source()->medium.value(), ct::CoolingMedium::FacilityWater);
  CT_CHECK_EQ(source->as_cooling_source()->design_supply_temperature->milli_celsius(),
              static_cast<std::int64_t>(30000));

  const ct::Node* chiller = find("chiller-c1");
  CT_REQUIRE(chiller != nullptr);
  CT_REQUIRE(chiller->as_chiller() != nullptr);
  CT_CHECK_EQ(chiller->as_chiller()->kind, ct::ChillerKind::Centrifugal);
  const ct::Node* loop = find("loop-sec");
  CT_REQUIRE(loop != nullptr);
  CT_REQUIRE(loop->as_cooling_loop() != nullptr);
  CT_CHECK_EQ(loop->as_cooling_loop()->kind, ct::LoopKind::Secondary);
  const ct::Node* cdu = find("cdu-c1");
  CT_REQUIRE(cdu != nullptr);
  CT_REQUIRE(cdu->as_cdu() != nullptr);
  CT_CHECK_EQ(cdu->as_cdu()->kind, ct::CduKind::RackCdu);
  const ct::Node* manifold = find("manifold-m1");
  CT_REQUIRE(manifold != nullptr);
  CT_REQUIRE(manifold->as_manifold() != nullptr);
  CT_CHECK_EQ(manifold->as_manifold()->kind, ct::ManifoldKind::Combined);
  const ct::Node* branch = find("branch-b1");
  CT_REQUIRE(branch != nullptr);
  CT_REQUIRE(branch->as_branch() != nullptr);
  CT_CHECK_EQ(branch->as_branch()->kind, ct::BranchKind::RackBranch);

  CT_REQUIRE(parsed->edges.size() == 6u);
  for (const ct::EdgeKind expected : {ct::EdgeKind::Supplies, ct::EdgeKind::Returns, ct::EdgeKind::Serves,
                                      ct::EdgeKind::Pumps, ct::EdgeKind::Contains, ct::EdgeKind::DependsOn}) {
    bool found = false;
    for (const ct::Edge& edge : parsed->edges) {
      found = found || edge.kind == expected;
    }
    CT_CHECK_MSG(found, std::string("edge kind missing: ") + std::string(ct::to_token(expected)));
  }
  for (const ct::Edge& edge : parsed->edges) {
    if (edge.id.value() == "e-serve") {
      CT_CHECK_EQ(edge.from.port, ct::PortRole::Server);
      CT_CHECK_EQ(edge.to.port, ct::PortRole::Served);
      CT_CHECK_EQ(edge.to.node.str(), std::string("zone-z1"));
    }
    if (edge.id.value() == "e-pump") {
      CT_CHECK_EQ(edge.from.port, ct::PortRole::SupplyOut);
      CT_CHECK_EQ(edge.to.port, ct::PortRole::Terminal);
    }
    if (edge.id.value() == "e-contain") {
      CT_CHECK_EQ(edge.from.port, ct::PortRole::Container);
      CT_CHECK_EQ(edge.to.port, ct::PortRole::Contained);
    }
    if (edge.id.value() == "e-depend") {
      CT_CHECK_EQ(edge.kind, ct::EdgeKind::DependsOn);
      CT_CHECK_EQ(edge.from.port, ct::PortRole::Terminal);
    }
  }

  CT_REQUIRE(parsed->aliases.size() == 1u);
  CT_CHECK_EQ(parsed->aliases.front().id.str(), std::string("a-legacy"));
  CT_CHECK_EQ(parsed->aliases.front().target.str(), std::string("plant-a"));

  CT_REQUIRE(parsed->groups.size() == 1u);
  const ct::RedundancyGroup& group = parsed->groups.front();
  CT_CHECK_EQ(group.id.str(), std::string("g-pumps"));
  CT_CHECK_EQ(group.scope, ct::RedundancyScope::Pump);
  CT_CHECK_EQ(group.scheme, ct::RedundancyScheme::NPlusOne);
  CT_CHECK_EQ(group.display_name, std::string("Pumps"));
  CT_CHECK_EQ(group.basis, std::string("duty+standby"));
  CT_CHECK(group.require_distinct_failure_domains);
  CT_CHECK(group.require_independent_sources);
  CT_REQUIRE(group.members.size() == 2u);
  for (const ct::RedundancyMember& member : group.members) {
    CT_CHECK_MSG(member.failure_domain.has_value(), "member " + member.declared + " has no failure domain");
    if (member.failure_domain.has_value()) {
      CT_CHECK_EQ(member.failure_domain->kind, ct::ExternalRefKind::FailureDomain);
      CT_CHECK_EQ(member.declared, member.node.str());
    }
  }

  CT_REQUIRE(parsed->changeovers.size() == 1u);
  CT_CHECK_EQ(parsed->changeovers.front().id.str(), std::string("co-1"));
  CT_CHECK_EQ(parsed->changeovers.front().max_concurrent, static_cast<std::uint32_t>(1));
  CT_CHECK_EQ(parsed->changeovers.front().display_name, std::string("Valve"));
  CT_REQUIRE(parsed->changeovers.front().members.size() == 2u);
  CT_CHECK_EQ(parsed->changeovers.front().members.front().node.str(), std::string("manifold-m1"));
}

CT_TEST(import_rejects_malformed_documents_with_stable_codes) {
  for (const LineCase& item : malformed_line_cases()) {
    auto parsed = ct::parse_import(item.document);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection: ") + item.label);
    if (!parsed.has_value()) {
      expect_code(parsed.error(), item.code, item.label);
    }
  }
}

CT_TEST(import_parse_errors_carry_the_offending_line) {
  for (const LineCase& item : malformed_line_cases()) {
    auto parsed = ct::parse_import(item.document);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection: ") + item.label);
    if (!parsed.has_value()) {
      expect_line(parsed.error(), item.line, item.label);
    }
  }
}

CT_TEST(import_rejects_document_level_defects) {
  struct Case {
    const char* label;
    std::string document;
    ct::ErrorCode code;
  };
  const std::vector<Case> cases = {
      {"empty document", std::string(), ct::ErrorCode::MissingField},
      {"comments only", "# nothing here\n# and nothing here\n", ct::ErrorCode::MissingField},
      {"blank lines only", "\n\n   \n\t\n", ct::ErrorCode::MissingField},
      {"provenance without a facility", "provenance producer=\"p\" origin=authored\n", ct::ErrorCode::MissingField},
      {"facility without provenance", "facility facility:dc-1@7\n", ct::ErrorCode::MissingField},
  };
  for (const Case& item : cases) {
    auto parsed = ct::parse_import(item.document);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection: ") + item.label);
    if (!parsed.has_value()) {
      expect_code(parsed.error(), item.code, item.label);
    }
  }
}

CT_TEST(import_handles_comments_crlf_blank_lines_and_tabs) {
  const std::string document =
      std::string("# ctg1 document with every line-ending shape\r\n") +
      "facility facility:dc-1@7\r\n" +
      "\r\n" +
      "provenance producer=\"p\" origin=authored witness=\"w\"   \r\n" +
      "   \r\n" +
      "\t\r\n" +
      "# a comment line only\r\n" +
      "node\tn-1\tchiller\tkind=centrifugal # trailing comment\n" +
      "node n-2 chiller kind=centrifugal#comment without a space\n" +
      "# final comment with no line ending";
  ct::ImportStats stats;
  auto parsed = ct::parse_import(document, &stats);
  CT_REQUIRE(parsed.has_value());
  CT_CHECK_EQ(parsed->nodes.size(), static_cast<std::size_t>(2));
  CT_CHECK_EQ(stats.lines, static_cast<std::size_t>(4));
  CT_CHECK_EQ(stats.nodes, static_cast<std::size_t>(2));
  CT_CHECK_EQ(parsed->provenance.witness, std::string("w"));
  CT_CHECK_EQ(parsed->nodes[0].id.str(), std::string("n-1"));
  CT_CHECK_EQ(parsed->nodes[1].id.str(), std::string("n-2"));
  for (const ct::Node& node : parsed->nodes) {
    CT_CHECK(node.display_name.empty());
  }
}

CT_TEST(import_quoted_value_with_equals_is_carried) {
  const std::string document =
      header_lines() + "node n-1 chiller kind=centrifugal name=\"Rack=A\" ref=\"asset:row=1\"\n";
  auto parsed = ct::parse_import(document);
  if (!parsed.has_value()) {
    ct_test::report_note("quoted '=' rejected: " + std::string(ct::error_code_name(parsed.error().code())) + " " +
                         parsed.error().message());
  }
  CT_REQUIRE(parsed.has_value());
  CT_REQUIRE(parsed->nodes.size() == 1u);
  CT_CHECK_EQ(parsed->nodes.front().display_name, std::string("Rack=A"));
  CT_REQUIRE(parsed->nodes.front().references.size() == 1u);
  CT_CHECK_EQ(parsed->nodes.front().references.front().kind, ct::ExternalRefKind::Asset);
  CT_CHECK_EQ(parsed->nodes.front().references.front().identity, std::string("row=1"));
}

CT_TEST(import_quoted_value_with_spaces_is_carried) {
  // import.hpp: a token that contains spaces must be quoted with the escape
  // syntax of escape_text(); export_import() emits exactly that shape for every
  // display name, witness, basis and external identity that contains a space.
  const std::string document = header_lines() + "node n-1 chiller kind=centrifugal name=\"Rack A 1\"\n";
  auto parsed = ct::parse_import(document);
  if (!parsed.has_value()) {
    ct_test::report_note("quoted token with a space rejected: " +
                         std::string(ct::error_code_name(parsed.error().code())) + " " + parsed.error().message());
  }
  CT_REQUIRE(parsed.has_value());
  CT_REQUIRE(parsed->nodes.size() == 1u);
  CT_CHECK_EQ(parsed->nodes.front().display_name, std::string("Rack A 1"));
}

CT_TEST(import_quoted_value_with_hash_is_carried) {
  // '#' starts a comment outside a quoted token; inside a quoted token it is
  // data and must survive.
  const std::string document = header_lines() + "node n-1 chiller kind=centrifugal name=\"Rack #1\"\n";
  auto parsed = ct::parse_import(document);
  if (!parsed.has_value()) {
    ct_test::report_note("quoted token with '#' rejected: " +
                         std::string(ct::error_code_name(parsed.error().code())) + " " + parsed.error().message());
  }
  CT_REQUIRE(parsed.has_value());
  CT_REQUIRE(parsed->nodes.size() == 1u);
  CT_CHECK_EQ(parsed->nodes.front().display_name, std::string("Rack #1"));
}

CT_TEST(import_export_round_trip_of_quoted_text_and_references) {
  ct::TopologyDraft draft = ct_test::single_path_facility();
  auto location = ct::ExternalRef::create(ct::ExternalRefKind::Location, "row 1 = a#b",
                                          ct::ExternalGeneration(4));
  CT_REQUIRE(location.has_value());
  auto asset = ct::ExternalRef::create(ct::ExternalRefKind::Asset, "rack/1", ct::ExternalGeneration(0));
  CT_REQUIRE(asset.has_value());
  draft.nodes.front().display_name = "Rack A #1 = primary";
  draft.nodes.front().references = {*asset, *location};
  auto built = ct_test::try_build(draft);
  CT_REQUIRE(built.has_value());

  const std::string exported = ct::export_import(*built);
  CT_CHECK_MSG(exported.find("name=\"Rack A #1 = primary\"") != std::string::npos,
               "exporter must quote a display name containing spaces, '#' and '='");

  auto reparsed = ct::parse_import(exported);
  if (!reparsed.has_value()) {
    ct_test::report_note("export with quoted text did not re-parse: " +
                         std::string(ct::error_code_name(reparsed.error().code())) + " subject=\"" +
                         reparsed.error().subject() + "\" " + reparsed.error().message());
  }
  CT_REQUIRE(reparsed.has_value());
  auto rebuilt = ct_test::try_build(*reparsed);
  CT_REQUIRE(rebuilt.has_value());
  CT_CHECK_MSG(rebuilt->digest() == built->digest(),
               "quoted text changed the digest: " + rebuilt->digest().to_hex() + " != " + built->digest().to_hex());
  const ct::Node* node = rebuilt->find_node(ct_test::nid("source:fw"));
  CT_REQUIRE(node != nullptr);
  CT_CHECK_EQ(node->display_name, std::string("Rack A #1 = primary"));
  CT_REQUIRE(node->references.size() == 2u);
  CT_CHECK_EQ(node->references.back().identity, std::string("row 1 = a#b"));
}

CT_TEST(import_repeatable_reference_keys_are_accepted) {
  // import.hpp documents "[ref=<extref>] (repeatable)" for every node kind, and
  // a node may carry up to limits::kMaxNodeReferences references.
  const std::string document =
      header_lines() +
      "node n-1 chiller kind=centrifugal ref=location:row-1 ref=failure_domain:fd-a ref=asset:a@4\n";
  auto parsed = ct::parse_import(document);
  if (!parsed.has_value()) {
    ct_test::report_note("repeated ref key rejected: " + std::string(ct::error_code_name(parsed.error().code())) +
                         " subject=\"" + parsed.error().subject() + "\" " + parsed.error().message());
  }
  CT_REQUIRE(parsed.has_value());
  CT_REQUIRE(parsed->nodes.size() == 1u);
  CT_CHECK_EQ(parsed->nodes.front().references.size(), static_cast<std::size_t>(3));
  if (parsed->nodes.front().references.size() == 3u) {
    CT_CHECK_EQ(parsed->nodes.front().references[0].kind, ct::ExternalRefKind::Location);
    CT_CHECK_EQ(parsed->nodes.front().references[0].identity, std::string("row-1"));
    CT_CHECK_EQ(parsed->nodes.front().references[1].kind, ct::ExternalRefKind::FailureDomain);
    CT_CHECK_EQ(parsed->nodes.front().references[2].generation.value(), static_cast<std::uint64_t>(4));
  }
  // A key that is not repeatable is still a duplicate.
  auto duplicate = ct::parse_import(header_lines() +
                                    "node n-1 chiller kind=centrifugal kind=screw ref=asset:a ref=asset:b\n");
  CT_REQUIRE(!duplicate.has_value());
  expect_code(duplicate.error(), ct::ErrorCode::DuplicateField, "duplicate non-repeatable key");
}

CT_TEST(import_group_member_spellings_are_canonical_node_ids) {
  // Canonical cooling identities contain ':', and a member without a failure
  // domain is written as its declared spelling. The exporter therefore emits
  // "plant:chp-a", which must re-parse as that node identity.
  const std::string document = header_lines() +
                               "node plant:chp-a cooling_plant kind=chiller_plant\n" +
                               "node plant:chp-b cooling_plant kind=chiller_plant\n" +
                               "group g-1 scope=plant plant:chp-a plant:chp-b\n";
  auto parsed = ct::parse_import(document);
  CT_REQUIRE(parsed.has_value());
  CT_REQUIRE(parsed->groups.size() == 1u);
  CT_REQUIRE(parsed->groups.front().members.size() == 2u);
  CT_CHECK_EQ(parsed->groups.front().members[0].node.str(), std::string("plant:chp-a"));
  CT_CHECK_EQ(parsed->groups.front().members[1].node.str(), std::string("plant:chp-b"));
  CT_CHECK_EQ(parsed->groups.front().members[0].declared, std::string("plant:chp-a"));
  CT_CHECK(!parsed->groups.front().members[0].failure_domain.has_value());
  CT_CHECK(!parsed->groups.front().members[1].failure_domain.has_value());
}

CT_TEST(import_group_failure_domain_uses_the_at_separator) {
  // import.hpp documents the member syntax as <member>[@<failure-domain-extref>].
  // '@' can never appear inside a canonical identifier, so the separator is
  // unambiguous even though ordinary cooling identities contain ':'.
  const std::string document = header_lines() +
                               "group g-1 scope=pump require-distinct-failure-domains "
                               "pump-p1@failure_domain:fd-a pump-p2@failure_domain:fd-b\n";
  auto parsed = ct::parse_import(document);
  CT_REQUIRE(parsed.has_value());
  CT_REQUIRE(parsed->groups.size() == 1u);
  CT_REQUIRE(parsed->groups.front().members.size() == 2u);
  CT_CHECK_EQ(parsed->groups.front().members[0].node.str(), std::string("pump-p1"));
  CT_CHECK_EQ(parsed->groups.front().members[0].declared, std::string("pump-p1"));
  CT_REQUIRE(parsed->groups.front().members[0].failure_domain.has_value());
  CT_CHECK_EQ(parsed->groups.front().members[0].failure_domain->kind, ct::ExternalRefKind::FailureDomain);
  CT_CHECK_EQ(parsed->groups.front().members[0].failure_domain->identity, std::string("fd-a"));

  // A ':' is not a separator: the whole token is one identity, and because
  // canonical identities may contain ':' it parses as an identity and is then
  // rejected by resolution rather than being silently split.
  const std::string colon_document =
      header_lines() + "group g-1 scope=pump pump-p1:failure_domain:fd-a\n";
  auto colon_parsed = ct::parse_import(colon_document);
  CT_REQUIRE(colon_parsed.has_value());
  CT_REQUIRE(colon_parsed->groups.front().members.size() == 1u);
  CT_CHECK_EQ(colon_parsed->groups.front().members.front().node.str(),
              std::string("pump-p1:failure_domain:fd-a"));
  CT_CHECK(!colon_parsed->groups.front().members.front().failure_domain.has_value());
}

CT_TEST(import_bounds_are_enforced_without_a_large_document) {
  std::string over_long = header_lines() + std::string(ct::limits::kMaxImportLineBytes + 1u, 'x') + "\n";
  auto long_line = ct::parse_import(over_long);
  CT_REQUIRE(!long_line.has_value());
  expect_code(long_line.error(), ct::ErrorCode::LimitExceeded, "line above the byte bound");
  expect_line(long_line.error(), 3, "line above the byte bound");

  // The document line bound is enforced while scanning; the document that
  // exceeds it is only a few hundred kilobytes of ignored blank lines.
  std::string many_lines = header_lines();
  many_lines.append(ct::limits::kMaxImportLines, '\n');
  auto lines = ct::parse_import(many_lines);
  CT_REQUIRE(!lines.has_value());
  expect_code(lines.error(), ct::ErrorCode::LimitExceeded, "document above the line bound");

  // A document just below the line bound still parses, so the bound is not off
  // by one in the other direction.
  std::string at_bound = header_lines();
  at_bound.append(ct::limits::kMaxImportLines - 3u, '\n');
  auto accepted = ct::parse_import(at_bound);
  CT_CHECK_MSG(accepted.has_value(), "document below the line bound must parse");
}
