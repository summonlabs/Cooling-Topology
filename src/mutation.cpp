// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/mutation.hpp"

#include <string>

#include "dccp/cooling_topology/canonical.hpp"

namespace dccp::cooling_topology {

Digest mutation_content_digest(const TopologyDraft& draft) {
  // The digest covers the *logical content* of the mutation: the facility
  // binding, the provenance and every table in canonical order. It excludes the
  // authority fields (epoch, incarnation, expected base) on purpose, so that a
  // retry of an already accepted attempt is recognized as the same mutation even
  // when it is re-planned against a newer head.
  TopologyHeader header;
  header.schema_version = kCanonicalSchemaVersion;
  header.facility = draft.facility;
  header.provenance = draft.provenance;

  const Result<CanonicalTables> tables = canonical_order(draft);
  if (tables.has_value()) {
    const Result<std::string> bytes =
        encode_topology(header, tables->nodes, tables->edges, tables->groups, tables->aliases,
                        tables->changeovers);
    if (bytes.has_value()) {
      return digest_bytes(*bytes);
    }
  }

  // A draft that cannot be canonicalized has no content identity. It is digested
  // from its own rejection so that two different malformed drafts still produce
  // different content digests and can never collide into a replay.
  const Error& error = tables.has_value() ? Error(ErrorCode::InternalError, "encode failed") : tables.error();
  std::string marker = "invalid-draft\n";
  marker += std::string(error_code_name(error.code()));
  marker += '\n';
  marker += error.message();
  marker += '\n';
  marker += error.subject();
  marker += '\n';
  marker += std::to_string(draft.nodes.size());
  marker += ',';
  marker += std::to_string(draft.edges.size());
  marker += ',';
  marker += std::to_string(draft.groups.size());
  marker += ',';
  marker += std::to_string(draft.aliases.size());
  marker += ',';
  marker += std::to_string(draft.changeovers.size());
  marker += '\n';
  for (const Node& node : draft.nodes) {
    marker += node.id.str();
    marker += '\n';
  }
  for (const Edge& edge : draft.edges) {
    marker += edge.id.str();
    marker += '\n';
  }
  return digest_bytes(marker);
}

}  // namespace dccp::cooling_topology
