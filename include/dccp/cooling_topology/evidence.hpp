// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_EVIDENCE_HPP
#define DCCP_COOLING_TOPOLOGY_EVIDENCE_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/strong_id.hpp"

namespace dccp::cooling_topology {

// ===========================================================================
// External references
// ===========================================================================

/// Which external registry or system owns the identity being referenced. This
/// library stores the reference verbatim and never resolves it.
enum class ExternalRefKind : std::uint8_t {
  Facility = 0,
  Rack = 1,
  Asset = 2,
  Location = 3,
  FailureDomain = 4,
  Consumer = 5,
  Registry = 6,
  Evidence = 7,
};

std::string_view to_token(ExternalRefKind value) noexcept;
Result<ExternalRefKind> parse_external_ref_kind(std::string_view token);

/// An opaque reference to an identity owned by another registry or component.
///
/// The bytes of the identity are preserved exactly as supplied: no case folding,
/// no Unicode normalization, no trimming. The only rejections are length,
/// invalid UTF-8 and embedded NUL. The generation field records the registry
/// generation the reference was taken from (0 = the producer did not bind a
/// generation, which is distinct from "generation zero exists"). Holding a
/// reference grants no authority over the referenced object.
struct ExternalRef {
  ExternalRefKind kind = ExternalRefKind::Registry;
  std::string identity;
  ExternalGeneration generation{};

  static Result<ExternalRef> create(ExternalRefKind kind, std::string identity, ExternalGeneration generation);

  /// Same kind, same bytes, same generation binding.
  bool same_binding_as(const ExternalRef& other) const noexcept;

  friend bool operator==(const ExternalRef& lhs, const ExternalRef& rhs) noexcept;
  friend std::strong_ordering operator<=>(const ExternalRef& lhs, const ExternalRef& rhs) noexcept;
};

// ===========================================================================
// The claim boundary
// ===========================================================================
//
// This library answers structural questions about cooling connectivity. It does
// not answer any operational, thermal, capacity or authority question. Those
// answers belong to adjacent DCCP components, to the facility BMS/DCIM or to the
// physical plant controllers, and they arrive here only as opaque references or
// as generation-stamped evidence that this library records without interpreting.
//
// Every public answer carries an EvidencePosture, and every claim in the list
// below is reported as NotOwned. The posture is a total function of the claim,
// not a mutable field, so no code path can accidentally produce an answer that
// looks like "running", "flowing", "available" or "healthy".

/// Claims that this library never makes.
enum class ExcludedClaim : std::uint8_t {
  ComponentRunning = 0,         ///< whether a plant, pump, chiller or fan is running
  MediumFlowing = 1,            ///< whether coolant or air is currently flowing
  PathUsableCapacity = 2,       ///< whether a path has usable cooling capacity
  RedundantSourceEligible = 3,  ///< whether a redundant source is operationally eligible
  ZoneThermallySafe = 4,        ///< whether a zone is thermally safe
  ActuationAuthority = 5,       ///< authority to actuate any component
  FailoverAuthority = 6,        ///< authority to fail over between sources
  AirflowPolicy = 7,            ///< airflow set points, pressurization or containment policy
  LiquidCoolingControl = 8,     ///< liquid-cooling control loops and set points
  FacilityPlacement = 9,        ///< where equipment is or may be placed
  PowerState = 10,              ///< electrical power state of any element
};

std::string_view to_token(ExcludedClaim claim) noexcept;
Result<ExcludedClaim> parse_excluded_claim(std::string_view token);

/// Disposition of a claim with respect to this library. There is exactly one
/// value: this library never owns an operational claim.
enum class ClaimDisposition : std::uint8_t {
  NotOwned = 0,
};

std::string_view to_token(ClaimDisposition disposition) noexcept;

/// The adjacent component, system or authority that owns the claim. Recorded so
/// a caller knows where the question must be asked.
std::string_view excluded_claim_owner(ExcludedClaim claim) noexcept;

/// One-line statement of the whole posture, e.g. "structural connectivity only:
/// operating state, flow, capacity, eligibility, thermal safety, actuation,
/// failover, airflow policy, liquid-cooling control, placement and power state
/// are not owned by this component".
std::string_view posture_statement() noexcept;

/// Deterministic rendering of every excluded claim and its owner, one per line.
std::string posture_report();

/// The immutable posture attached to every public answer.
class EvidencePosture {
 public:
  constexpr EvidencePosture() noexcept = default;

  /// Always ClaimDisposition::NotOwned. The result does not depend on any
  /// member, argument or global: it is impossible for this library to report
  /// ownership of an operational claim.
  static constexpr ClaimDisposition claim_disposition(ExcludedClaim) noexcept {
    return ClaimDisposition::NotOwned;
  }

  friend constexpr bool operator==(const EvidencePosture&, const EvidencePosture&) noexcept = default;
};

// ===========================================================================
// External evidence
// ===========================================================================

/// Kinds of evidence owned by other components that this library may be *told
/// about* and must record without interpreting.
enum class EvidenceKind : std::uint8_t {
  OperatingState = 0,
  FlowObservation = 1,
  CapacityStatement = 2,
  SourceEligibility = 3,
  ThermalSafetyStatement = 4,
  ActuationCommand = 5,
  FailoverDecision = 6,
  AirflowPolicyStatement = 7,
  LiquidCoolingPolicy = 8,
  PlacementDecision = 9,
  PowerStateObservation = 10,
};

std::string_view to_token(EvidenceKind kind) noexcept;
Result<EvidenceKind> parse_evidence_kind(std::string_view token);

/// The excluded claim an evidence kind belongs to.
ExcludedClaim claim_of(EvidenceKind kind) noexcept;

/// A generation-stamped reference to evidence owned by another component.
///
/// The record exists so that provenance can name the evidence a topology
/// generation was composed against. The library never reads the evidence, never
/// orders it as authority and never turns it into a claim: a recorded
/// EvidenceBinding proves only that the producer stated it. Recovered evidence is
/// not fresh evidence, so a binding carries the generation it was taken from and
/// its producer must revalidate it.
struct EvidenceBinding {
  EvidenceKind kind = EvidenceKind::OperatingState;
  /// Component that produced the evidence, e.g. "airflow-control/1.0.0".
  std::string producer;
  /// Opaque identity of the subject the evidence is about, owned elsewhere.
  ExternalRef subject;
  EvidenceGeneration generation{};
  ObservationSequence observation{};

  static Result<EvidenceBinding> create(EvidenceKind kind, std::string producer, const ExternalRef& subject,
                                        EvidenceGeneration generation, ObservationSequence observation);

  /// Same kind, same producer bytes, same subject binding, same generations.
  bool same_binding_as(const EvidenceBinding& other) const noexcept;

  friend bool operator==(const EvidenceBinding& lhs, const EvidenceBinding& rhs) noexcept;
  friend std::strong_ordering operator<=>(const EvidenceBinding& lhs, const EvidenceBinding& rhs) noexcept;
};

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_EVIDENCE_HPP
