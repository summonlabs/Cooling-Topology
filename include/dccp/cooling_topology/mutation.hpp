// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_MUTATION_HPP
#define DCCP_COOLING_TOPOLOGY_MUTATION_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/strong_id.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {

/// A mutation planned against a specific authority and base generation.
///
/// Every state-dependent request states the authority it was planned under:
///   * epoch - the writer epoch that was valid when the request was planned;
///   * incarnation - the writer incarnation that planned it;
///   * expected_base - the head generation the request was planned against
///     (0 means "the store must still be empty").
///
/// A request whose authority no longer holds is refused; it is never merged into
/// newer state. A replay of an already accepted attempt returns the recorded
/// outcome *before* the base-generation check, so a retry of a lost response
/// cannot be mistaken for a stale write.
struct MutationAuthority {
  WriterEpoch epoch{};
  WriterIncarnation incarnation{};
  TopologyGeneration expected_base{};

  friend bool operator==(const MutationAuthority&, const MutationAuthority&) noexcept = default;
};

/// Digest identifying the logical content of a mutation, used to tell a replay
/// (same identity, same content) from a conflicting reuse of an identity (same
/// identity, different content).
///
/// The digest covers the canonical image of the draft body only. It deliberately
/// excludes the authority (epoch, incarnation, expected base): a retry of an
/// accepted attempt that is now planned against a newer head must still be
/// recognized as a replay of the same mutation, not as a conflicting one.
Digest mutation_content_digest(const TopologyDraft& draft);

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_MUTATION_HPP
