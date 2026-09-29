// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_CANONICAL_HPP
#define DCCP_COOLING_TOPOLOGY_CANONICAL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {

/// Canonical encoding rules (format version kCanonicalSchemaVersion):
///
///   * all integers are little-endian and fixed width (no varints, no native
///     widths, no padding);
///   * every table is written in canonical order (sorted by identity), so the
///     byte image does not depend on insertion order;
///   * strings are length-prefixed with a 32-bit byte count and contain no NUL;
///   * optional values are written as a presence byte followed by the value;
///   * variant payloads are written as a tag byte followed by the payload, and a
///     tag that does not name a member of the variant is rejected on decode;
///   * no timestamps, no memory addresses, no process ids, no random values and
///     no environment-dependent content appear anywhere in the image.
///
/// Equivalent logical state therefore produces identical bytes on every platform
/// and in every process, and encode -> decode -> encode is a fixed point.

/// Magic of a generation file frame, 8 bytes.
inline constexpr std::string_view kGenerationFileMagic = "CLDTOPG1";

/// A decoded generation file frame.
struct GenerationFile {
  std::uint16_t schema_version = 0;
  std::string payload;  ///< canonical generation image
  Digest payload_digest{};
};

/// Frames a canonical image for durable storage:
///   magic[8] | schema_version u16 | reserved u16 (0) | payload_len u64 |
///   payload | sha256(payload)[32]
Result<std::string> encode_generation_file(std::string_view payload);

/// Decodes and integrity-checks a generation file frame. Rejects a wrong magic,
/// an unsupported schema version, a non-zero reserved field, a truncated or
/// oversized frame, a length that disagrees with the actual byte count, and a
/// digest mismatch.
Result<GenerationFile> decode_generation_file(std::string_view bytes);

/// Canonical image of a topology generation. Kept here as well as on Topology so
/// callers can encode a header/table set without constructing a value.
Result<std::string> encode_topology(const TopologyHeader& header, const std::vector<Node>& nodes,
                                    const std::vector<Edge>& edges, const std::vector<RedundancyGroup>& groups,
                                    const std::vector<Alias>& aliases,
                                    const std::vector<ChangeoverGroup>& changeovers);

/// Sorts every table into canonical order and validates the identity uniqueness
/// that canonical order depends on. Returns the ordered tables. This is the same
/// ordering the encoder applies, exposed so that callers (the importer, the CLI
/// and the store) can compare logical state by digest without encoding twice.
struct CanonicalTables {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<RedundancyGroup> groups;
  std::vector<Alias> aliases;
  std::vector<ChangeoverGroup> changeovers;
};

Result<CanonicalTables> canonical_order(const TopologyDraft& draft);

/// A decoded canonical generation image.
struct DecodedGeneration {
  TopologyHeader header;
  CanonicalTables tables;
};

/// Decodes a canonical generation image.
///
/// Every declared count and length is validated against the remaining input and
/// against the corresponding limits:: bound before it is used, every enum byte
/// is checked against its type, every string is validated as UTF-8 with no NUL,
/// and the image must be consumed exactly (no trailing bytes). Returns the most
/// specific error code available; never throws and never allocates from an
/// unvalidated count.
Result<DecodedGeneration> decode_topology(std::string_view payload);

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_CANONICAL_HPP
