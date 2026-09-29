// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_LIMITS_HPP
#define DCCP_COOLING_TOPOLOGY_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace dccp::cooling_topology::limits {

/// Every externally influenced size is bounded before allocation. The bounds
/// below are the documented contract; exceeding one is a LimitExceeded (or a
/// shape) error, never a silent truncation or a wrapped counter.

/// Identity and text bounds (bytes).
inline constexpr std::size_t kMaxIdentifierBytes = 128;
inline constexpr std::size_t kMaxDisplayNameBytes = 192;
inline constexpr std::size_t kMaxWitnessBytes = 256;
inline constexpr std::size_t kMaxProducerBytes = 128;
inline constexpr std::size_t kMaxBasisBytes = 512;             // declared redundancy basis text
inline constexpr std::size_t kMaxExternalIdentityBytes = 512;  // opaque external identity, preserved verbatim
inline constexpr std::size_t kMaxExternalKindBytes = 64;

/// Topology table bounds.
inline constexpr std::size_t kMaxNodeCount = 100000;
inline constexpr std::size_t kMaxEdgeCount = 200000;
inline constexpr std::size_t kMaxRedundancyGroupCount = 4096;
inline constexpr std::size_t kMaxGroupMemberCount = 4096;
inline constexpr std::size_t kMaxAliasCount = 32768;
inline constexpr std::size_t kMaxNodeReferences = 16;
inline constexpr std::size_t kMaxChangeoverGroupCount = 4096;
inline constexpr std::size_t kMaxChangeoverMemberCount = 1024;
inline constexpr std::size_t kMaxEvidenceBindings = 64;

/// Canonical generation encoding bounds.
inline constexpr std::size_t kMaxGenerationBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxCanonicalStringBytes = 65535;

/// Query bounds. Queries are bounded work, never unbounded traversal.
inline constexpr std::size_t kMaxTraversalNodes = 100000;
inline constexpr std::size_t kMaxQueryDepth = 512;
inline constexpr std::size_t kMaxQueryResultCount = 16384;
inline constexpr std::size_t kMaxPathCount = 512;
inline constexpr std::size_t kMaxPathLength = 128;
inline constexpr std::size_t kMaxPathQuerySubjects = 64;
inline constexpr std::size_t kMaxDiffEntries = 200000;
inline constexpr std::size_t kMaxIndependentPathSearch = 4096;
inline constexpr std::size_t kMaxAlternateSourceCount = 4096;

/// Persistence bounds.
inline constexpr std::size_t kMaxHistoryEntries = 64;
inline constexpr std::size_t kMaxIdempotencyRecords = 64;
inline constexpr std::size_t kMaxRetainedGenerations = 8;
inline constexpr std::size_t kMaxManifestBytes = 64u * 1024u;
inline constexpr std::size_t kMaxIdempotencyRecordBytes = 4096;
inline constexpr std::size_t kMaxStorePathBytes = 4096;
inline constexpr std::size_t kMaxGenerationFileBytes = kMaxGenerationBytes + 4096;

/// Import (text) format bounds.
inline constexpr std::size_t kMaxImportBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kMaxImportLineBytes = 8192;
inline constexpr std::size_t kMaxImportLines = 400000;

/// Default retention of accepted-attempt records: the most recent N accepted
/// attempts are kept; older records are evicted in publication order.
inline constexpr std::size_t kDefaultIdempotencyRetention = 64;

/// Physical quantity bounds. Declared design temperatures are exact integers in
/// millidegrees Celsius and are bounded so that checked arithmetic can never be
/// defeated by a declared extreme.
inline constexpr std::int64_t kMinTemperatureMilliCelsius = -10000000;  // -10 000.000 C
inline constexpr std::int64_t kMaxTemperatureMilliCelsius = 10000000;   // +10 000.000 C
inline constexpr std::int64_t kMilliCelsiusPerCelsius = 1000;

}  // namespace dccp::cooling_topology::limits

#endif  // DCCP_COOLING_TOPOLOGY_LIMITS_HPP
