// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal validation contract. Not installed and not part of the public API.

#ifndef DCCP_COOLING_TOPOLOGY_VALIDATE_INTERNAL_HPP
#define DCCP_COOLING_TOPOLOGY_VALIDATE_INTERNAL_HPP

#include "dccp/cooling_topology/canonical.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology::internal {

/// Validates a draft and returns its canonically ordered tables.
///
/// Returns the primary (first, deterministic) error when the draft is
/// structurally invalid. Warning-severity findings do not make a draft invalid
/// and are not reported here; use Topology::validate_draft for the full report.
Result<CanonicalTables> validate_and_order(const TopologyDraft& draft);

/// Converts a validation report into the error a caller sees. The error carries
/// the primary issue code, message, subject and the remaining issues as details,
/// so the machine-readable contract is the ErrorCode and never the message text.
Error primary_error(const ValidationReport& report);

}  // namespace dccp::cooling_topology::internal

#endif  // DCCP_COOLING_TOPOLOGY_VALIDATE_INTERNAL_HPP
