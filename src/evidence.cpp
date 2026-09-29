// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/evidence.hpp"

#include <string>
#include <utility>

#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/text.hpp"

namespace dccp::cooling_topology {
namespace {

/// Token table for ExcludedClaim. The order is the enumerator order and the
/// strings are part of the public contract.
constexpr std::string_view kExcludedClaimTokens[] = {
    "component_running",      "medium_flowing",        "path_usable_capacity",
    "redundant_source_eligible", "zone_thermally_safe", "actuation_authority",
    "failover_authority",     "airflow_policy",        "liquid_cooling_control",
    "facility_placement",     "power_state",
};
constexpr std::size_t kExcludedClaimCount = sizeof(kExcludedClaimTokens) / sizeof(kExcludedClaimTokens[0]);

constexpr std::string_view kExcludedClaimOwners[] = {
    "device and plant control (facility BMS/DCIM, Cooling-Failover)",
    "coolant and airflow instrumentation (facility BMS/DCIM)",
    "Cooling-Capacity, Cooling-Capacity-Accounting, Rack-Capacity",
    "Cooling-Failover",
    "Thermal-Zone-Manager, Thermal-Governor, Thermal-Control-Plane",
    "Liquid-Cooling-Control, Airflow-Control, PDU-Control, UPS-Control",
    "Cooling-Failover",
    "Airflow-Control",
    "Liquid-Cooling-Control",
    "Facility-Placement-Planner, Physical-Location-Registry",
    "Power-Topology, Power-Control-Plane, PDU-Control, UPS-Control",
};
constexpr std::size_t kExcludedClaimOwnerCount = sizeof(kExcludedClaimOwners) / sizeof(kExcludedClaimOwners[0]);

static_assert(kExcludedClaimCount == 11, "every excluded claim needs a token");
static_assert(kExcludedClaimOwnerCount == kExcludedClaimCount, "every excluded claim needs an owner");

constexpr std::string_view kEvidenceKindTokens[] = {
    "operating_state",     "flow_observation",       "capacity_statement",
    "source_eligibility",  "thermal_safety_statement", "actuation_command",
    "failover_decision",   "airflow_policy_statement", "liquid_cooling_policy",
    "placement_decision",  "power_state_observation",
};
constexpr std::size_t kEvidenceKindCount = sizeof(kEvidenceKindTokens) / sizeof(kEvidenceKindTokens[0]);
static_assert(kEvidenceKindCount == kExcludedClaimCount, "evidence kinds mirror the excluded claims");

constexpr ExcludedClaim kEvidenceKindClaims[] = {
    ExcludedClaim::ComponentRunning,     ExcludedClaim::MediumFlowing,
    ExcludedClaim::PathUsableCapacity,   ExcludedClaim::RedundantSourceEligible,
    ExcludedClaim::ZoneThermallySafe,    ExcludedClaim::ActuationAuthority,
    ExcludedClaim::FailoverAuthority,    ExcludedClaim::AirflowPolicy,
    ExcludedClaim::LiquidCoolingControl, ExcludedClaim::FacilityPlacement,
    ExcludedClaim::PowerState,
};

std::size_t excluded_claim_index(ExcludedClaim claim) noexcept {
  const auto raw = static_cast<std::size_t>(claim);
  return raw < kExcludedClaimCount ? raw : kExcludedClaimCount;
}

std::string_view unknown_token() noexcept { return "unknown"; }

}  // namespace

std::string_view to_token(ExcludedClaim claim) noexcept {
  const std::size_t index = excluded_claim_index(claim);
  return index < kExcludedClaimCount ? kExcludedClaimTokens[index] : unknown_token();
}

Result<ExcludedClaim> parse_excluded_claim(std::string_view token) {
  for (std::size_t index = 0; index < kExcludedClaimCount; ++index) {
    if (token == kExcludedClaimTokens[index]) {
      return static_cast<ExcludedClaim>(index);
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown excluded-claim token")
      .with_subject(std::string(token.substr(0, 64)));
}

std::string_view to_token(ClaimDisposition disposition) noexcept {
  return disposition == ClaimDisposition::NotOwned ? std::string_view("not_owned") : unknown_token();
}

std::string_view excluded_claim_owner(ExcludedClaim claim) noexcept {
  const std::size_t index = excluded_claim_index(claim);
  return index < kExcludedClaimCount ? kExcludedClaimOwners[index] : std::string_view("unknown owner");
}

std::string_view posture_statement() noexcept {
  return "structural connectivity only: operating state, flow, capacity, source eligibility, "
         "thermal safety, actuation, failover, airflow policy, liquid-cooling control, placement "
         "and power state are not owned by this component";
}

std::string posture_report() {
  std::string out;
  out.reserve(1024);
  out += "claim boundary of dccp-cooling-topology (every claim below is NotOwned)\n";
  for (std::size_t index = 0; index < kExcludedClaimCount; ++index) {
    out += "  not_owned  ";
    out += kExcludedClaimTokens[index];
    out += "  owner: ";
    out += kExcludedClaimOwners[index];
    out += '\n';
  }
  return out;
}

std::string_view to_token(EvidenceKind kind) noexcept {
  const auto raw = static_cast<std::size_t>(kind);
  return raw < kEvidenceKindCount ? kEvidenceKindTokens[raw] : unknown_token();
}

Result<EvidenceKind> parse_evidence_kind(std::string_view token) {
  for (std::size_t index = 0; index < kEvidenceKindCount; ++index) {
    if (token == kEvidenceKindTokens[index]) {
      return static_cast<EvidenceKind>(index);
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown evidence-kind token")
      .with_subject(std::string(token.substr(0, 64)));
}

ExcludedClaim claim_of(EvidenceKind kind) noexcept {
  const auto raw = static_cast<std::size_t>(kind);
  return raw < kEvidenceKindCount ? kEvidenceKindClaims[raw] : ExcludedClaim::ComponentRunning;
}

Result<EvidenceBinding> EvidenceBinding::create(EvidenceKind kind, std::string producer, const ExternalRef& subject,
                                                EvidenceGeneration generation,
                                                ObservationSequence observation) {
  if (producer.empty()) {
    return Error(ErrorCode::MissingField, "evidence producer must not be empty");
  }
  if (producer.size() > limits::kMaxProducerBytes) {
    return Error(ErrorCode::TextTooLong, "evidence producer exceeds the producer byte bound")
        .with_subject(producer.substr(0, 64));
  }
  if (!is_valid_display_text(producer, limits::kMaxProducerBytes)) {
    return Error(ErrorCode::InvalidUtf8,
                 "evidence producer must be valid UTF-8 display text without control characters")
        .with_subject(producer.substr(0, 64));
  }
  if (subject.identity.empty()) {
    return Error(ErrorCode::MissingField, "evidence subject reference must not be empty");
  }
  EvidenceBinding binding;
  binding.kind = kind;
  binding.producer = std::move(producer);
  binding.subject = subject;
  binding.generation = generation;
  binding.observation = observation;
  return binding;
}

bool EvidenceBinding::same_binding_as(const EvidenceBinding& other) const noexcept {
  return kind == other.kind && producer == other.producer && subject.same_binding_as(other.subject) &&
         generation == other.generation && observation == other.observation;
}

bool operator==(const EvidenceBinding& lhs, const EvidenceBinding& rhs) noexcept {
  return lhs.same_binding_as(rhs);
}

std::strong_ordering operator<=>(const EvidenceBinding& lhs, const EvidenceBinding& rhs) noexcept {
  if (const auto cmp = static_cast<std::uint8_t>(lhs.kind) <=> static_cast<std::uint8_t>(rhs.kind); cmp != 0) {
    return cmp;
  }
  if (const int cmp = lhs.producer.compare(rhs.producer); cmp != 0) {
    return cmp < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
  }
  if (const auto cmp = lhs.subject <=> rhs.subject; cmp != 0) {
    return cmp;
  }
  if (const auto cmp = lhs.generation <=> rhs.generation; cmp != 0) {
    return cmp;
  }
  return lhs.observation <=> rhs.observation;
}

}  // namespace dccp::cooling_topology
