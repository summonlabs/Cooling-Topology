// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/result.hpp"

#include <cstdint>

namespace dccp::cooling_topology {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

// The table is the single source of truth for code names. Codes are never
// renumbered or repurposed and a token is never changed once published; new
// tokens are appended inside the section of the code they name. The token is
// stable lower_snake_case.
constexpr CodeName kCodeNames[] = {
    {ErrorCode::Ok, "ok"},

    // ---- Input shape and encoding (untrusted data) -------------------------
    {ErrorCode::InvalidArgument, "invalid_argument"},
    {ErrorCode::MalformedIdentifier, "malformed_identifier"},
    {ErrorCode::InvalidUtf8, "invalid_utf8"},
    {ErrorCode::TextTooLong, "text_too_long"},
    {ErrorCode::UnknownEnumToken, "unknown_enum_token"},
    {ErrorCode::MissingField, "missing_field"},
    {ErrorCode::DuplicateField, "duplicate_field"},
    {ErrorCode::MalformedRecord, "malformed_record"},
    {ErrorCode::UnsupportedSchemaVersion, "unsupported_schema_version"},
    {ErrorCode::CountMismatch, "count_mismatch"},
    {ErrorCode::TruncatedInput, "truncated_input"},
    {ErrorCode::DigestMismatch, "digest_mismatch"},
    {ErrorCode::LimitExceeded, "limit_exceeded"},
    {ErrorCode::EmptyInput, "empty_input"},
    {ErrorCode::MalformedNumber, "malformed_number"},
    {ErrorCode::QuantityOutOfRange, "quantity_out_of_range"},

    // ---- Structural and semantic rejection ---------------------------------
    {ErrorCode::NotFound, "not_found"},
    {ErrorCode::IdentityConflict, "identity_conflict"},
    {ErrorCode::DuplicateIdentifier, "duplicate_identifier"},
    {ErrorCode::AliasCycle, "alias_cycle"},
    {ErrorCode::AliasTargetMissing, "alias_target_missing"},
    {ErrorCode::SelfEdge, "self_edge"},
    {ErrorCode::DuplicateEdge, "duplicate_edge"},
    {ErrorCode::EndpointMissing, "endpoint_missing"},
    {ErrorCode::InvalidPortForKind, "invalid_port_for_kind"},
    {ErrorCode::InvalidEdgeEndpointPair, "invalid_edge_endpoint_pair"},
    {ErrorCode::InvalidEdgeKindPair, "invalid_edge_kind_pair"},
    {ErrorCode::AmbiguousParentage, "ambiguous_parentage"},
    {ErrorCode::ContainmentCycle, "containment_cycle"},
    {ErrorCode::ContainmentKindInvalid, "containment_kind_invalid"},
    {ErrorCode::MissingContainer, "missing_container"},
    {ErrorCode::GroupMemberKindInvalid, "group_member_kind_invalid"},
    {ErrorCode::GroupMemberDuplicate, "group_member_duplicate"},
    {ErrorCode::GroupMemberMissing, "group_member_missing"},
    {ErrorCode::GroupEmpty, "group_empty"},
    {ErrorCode::GroupRedundancyUnproven, "group_redundancy_unproven"},
    {ErrorCode::GroupScopeInvalid, "group_scope_invalid"},
    {ErrorCode::GroupIndependentPathUnproven, "group_independent_path_unproven"},
    {ErrorCode::ChangeoverMemberInvalid, "changeover_member_invalid"},
    {ErrorCode::ChangeoverMemberDuplicate, "changeover_member_duplicate"},
    {ErrorCode::ChangeoverCardinality, "changeover_cardinality"},
    {ErrorCode::ConstraintUnsatisfied, "constraint_unsatisfied"},
    {ErrorCode::IsolatedElement, "isolated_element"},
    {ErrorCode::ZoneServiceAbsent, "zone_service_absent"},

    // ---- Cooling-specific structural rejection -----------------------------
    {ErrorCode::MediumMismatch, "medium_mismatch"},
    {ErrorCode::TemperatureIncompatible, "temperature_incompatible"},
    {ErrorCode::LoopKindMismatch, "loop_kind_mismatch"},
    {ErrorCode::ZoneServiceInvalid, "zone_service_invalid"},
    {ErrorCode::SupplyCycle, "supply_cycle"},
    {ErrorCode::ReturnCycle, "return_cycle"},
    {ErrorCode::ServiceCycle, "service_cycle"},
    {ErrorCode::DependencyCycle, "dependency_cycle"},
    {ErrorCode::PumpAttachmentInvalid, "pump_attachment_invalid"},
    {ErrorCode::SinkFeedAbsent, "sink_feed_absent"},
    {ErrorCode::PlantBoundaryInvalid, "plant_boundary_invalid"},
    {ErrorCode::SourceTerminalInvalid, "source_terminal_invalid"},
    {ErrorCode::ReturnTargetInvalid, "return_target_invalid"},

    // ---- Authority, generations and lifecycle ------------------------------
    {ErrorCode::GenerationMismatch, "generation_mismatch"},
    {ErrorCode::StaleBaseGeneration, "stale_base_generation"},
    {ErrorCode::StaleAuthorityEpoch, "stale_authority_epoch"},
    {ErrorCode::StaleWriterIncarnation, "stale_writer_incarnation"},
    {ErrorCode::StoreLocked, "store_locked"},
    {ErrorCode::StoreClosed, "store_closed"},
    {ErrorCode::StoreNotFound, "store_not_found"},
    {ErrorCode::StoreNotEmpty, "store_not_empty"},
    {ErrorCode::StoreMismatch, "store_mismatch"},
    {ErrorCode::StoreReadOnly, "store_read_only"},
    {ErrorCode::NotInitialized, "not_initialized"},
    {ErrorCode::GenerationAlreadyExists, "generation_already_exists"},
    {ErrorCode::GenerationNotRetained, "generation_not_retained"},
    {ErrorCode::GenerationFloorViolation, "generation_floor_violation"},
    {ErrorCode::HeadMissing, "head_missing"},
    {ErrorCode::HeadCorrupt, "head_corrupt"},
    {ErrorCode::RecoveryRequired, "recovery_required"},
    {ErrorCode::RecoveryUnavailable, "recovery_unavailable"},
    {ErrorCode::IntegrityFailure, "integrity_failure"},
    {ErrorCode::PublicationIncomplete, "publication_incomplete"},
    {ErrorCode::IdempotencyConflict, "idempotency_conflict"},
    {ErrorCode::IdempotencyEvicted, "idempotency_evicted"},
    {ErrorCode::IoError, "io_error"},
    {ErrorCode::PathInvalid, "path_invalid"},
    {ErrorCode::PathTraversal, "path_traversal"},
    {ErrorCode::PathNotRegular, "path_not_regular"},
    {ErrorCode::PathUnsafeName, "path_unsafe_name"},
    {ErrorCode::InternalError, "internal_error"},

};

constexpr std::string_view name_of(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown";
}

// The name table is total over the enumerators: every value from Ok to the last
// declared code resolves to a token. An enumerator added without a token fails
// this compile-time check instead of surfacing as "unknown" at run time.
constexpr bool names_every_code() noexcept {
  for (std::uint16_t value = 0; value <= static_cast<std::uint16_t>(ErrorCode::InternalError); ++value) {
    if (name_of(static_cast<ErrorCode>(value)) == "unknown") {
      return false;
    }
  }
  return true;
}

static_assert(names_every_code(), "kCodeNames does not name every ErrorCode enumerator");

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept { return name_of(code); }

ErrorCategory error_category(ErrorCode code) noexcept {
  // The switch is exhaustive over ErrorCode and carries no default label, so an
  // enumerator without a category is a compile-time diagnostic rather than a
  // silent classification.
  switch (code) {
    case ErrorCode::Ok:
      return ErrorCategory::Ok;

    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedIdentifier:
    case ErrorCode::InvalidUtf8:
    case ErrorCode::TextTooLong:
    case ErrorCode::UnknownEnumToken:
    case ErrorCode::MissingField:
    case ErrorCode::DuplicateField:
    case ErrorCode::MalformedRecord:
    case ErrorCode::UnsupportedSchemaVersion:
    case ErrorCode::CountMismatch:
    case ErrorCode::TruncatedInput:
    case ErrorCode::DigestMismatch:
    case ErrorCode::EmptyInput:
    case ErrorCode::MalformedNumber:
    case ErrorCode::QuantityOutOfRange:
    case ErrorCode::PathInvalid:
    case ErrorCode::PathTraversal:
    case ErrorCode::PathNotRegular:
    case ErrorCode::PathUnsafeName:
      return ErrorCategory::Argument;

    case ErrorCode::NotFound:
    case ErrorCode::IdentityConflict:
    case ErrorCode::DuplicateIdentifier:
    case ErrorCode::AliasCycle:
    case ErrorCode::AliasTargetMissing:
    case ErrorCode::SelfEdge:
    case ErrorCode::DuplicateEdge:
    case ErrorCode::EndpointMissing:
    case ErrorCode::InvalidPortForKind:
    case ErrorCode::InvalidEdgeEndpointPair:
    case ErrorCode::InvalidEdgeKindPair:
    case ErrorCode::AmbiguousParentage:
    case ErrorCode::ContainmentCycle:
    case ErrorCode::ContainmentKindInvalid:
    case ErrorCode::MissingContainer:
    case ErrorCode::GroupMemberKindInvalid:
    case ErrorCode::GroupMemberDuplicate:
    case ErrorCode::GroupMemberMissing:
    case ErrorCode::GroupEmpty:
    case ErrorCode::GroupRedundancyUnproven:
    case ErrorCode::GroupScopeInvalid:
    case ErrorCode::GroupIndependentPathUnproven:
    case ErrorCode::ChangeoverMemberInvalid:
    case ErrorCode::ChangeoverMemberDuplicate:
    case ErrorCode::ChangeoverCardinality:
    case ErrorCode::ConstraintUnsatisfied:
    case ErrorCode::IsolatedElement:
    case ErrorCode::ZoneServiceAbsent:
    case ErrorCode::MediumMismatch:
    case ErrorCode::TemperatureIncompatible:
    case ErrorCode::LoopKindMismatch:
    case ErrorCode::ZoneServiceInvalid:
    case ErrorCode::SupplyCycle:
    case ErrorCode::ReturnCycle:
    case ErrorCode::ServiceCycle:
    case ErrorCode::DependencyCycle:
    case ErrorCode::PumpAttachmentInvalid:
    case ErrorCode::SinkFeedAbsent:
    case ErrorCode::PlantBoundaryInvalid:
    case ErrorCode::SourceTerminalInvalid:
    case ErrorCode::ReturnTargetInvalid:
      return ErrorCategory::Structure;

    case ErrorCode::GenerationMismatch:
    case ErrorCode::StaleBaseGeneration:
    case ErrorCode::StaleAuthorityEpoch:
    case ErrorCode::StaleWriterIncarnation:
    case ErrorCode::GenerationAlreadyExists:
    case ErrorCode::GenerationFloorViolation:
    case ErrorCode::IdempotencyConflict:
    case ErrorCode::IdempotencyEvicted:
      return ErrorCategory::Authority;

    case ErrorCode::StoreLocked:
    case ErrorCode::StoreClosed:
    case ErrorCode::StoreNotFound:
    case ErrorCode::StoreNotEmpty:
    case ErrorCode::StoreMismatch:
    case ErrorCode::StoreReadOnly:
    case ErrorCode::NotInitialized:
      return ErrorCategory::Lifecycle;

    case ErrorCode::GenerationNotRetained:
    case ErrorCode::HeadMissing:
    case ErrorCode::HeadCorrupt:
    case ErrorCode::RecoveryRequired:
    case ErrorCode::RecoveryUnavailable:
    case ErrorCode::IntegrityFailure:
    case ErrorCode::PublicationIncomplete:
    case ErrorCode::IoError:
      return ErrorCategory::Persistence;

    case ErrorCode::LimitExceeded:
      return ErrorCategory::Limit;



    case ErrorCode::InternalError:
      return ErrorCategory::Internal;
  }
  // A value outside the enumeration is a defect in the library, never a
  // structural outcome, and is reported as such instead of crashing.
  return ErrorCategory::Internal;
}

std::string_view error_category_name(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok:
      return "OK";
    case ErrorCategory::Argument:
      return "ARGUMENT";
    case ErrorCategory::Structure:
      return "STRUCTURE";
    case ErrorCategory::Authority:
      return "AUTHORITY";
    case ErrorCategory::Persistence:
      return "PERSISTENCE";
    case ErrorCategory::Lifecycle:
      return "LIFECYCLE";
    case ErrorCategory::Limit:
      return "LIMIT";
    case ErrorCategory::Internal:
      return "INTERNAL";
  }
  return "UNKNOWN";
}

std::string Error::to_string() const {
  std::string out(error_code_name(code_));
  out.append(": ");
  out.append(message_);
  if (!subject_.empty()) {
    out.append(" [subject=");
    out.append(subject_);
    out.push_back(']');
  }
  for (const std::string& detail : details_) {
    out.append(" [");
    out.append(detail);
    out.push_back(']');
  }
  return out;
}

}  // namespace dccp::cooling_topology
