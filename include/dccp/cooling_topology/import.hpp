// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_IMPORT_HPP
#define DCCP_COOLING_TOPOLOGY_IMPORT_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "dccp/cooling_topology/result.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {

/// Text import grammar "ctg" (cooling topology grammar), version 1.
///
/// The grammar is line oriented and strictly parsed:
///
///   * one command per line; '#' starts a comment; blank lines are ignored; LF
///     and CRLF line endings are both accepted; a trailing CR is stripped;
///   * every line is bounded by limits::kMaxImportLineBytes and the whole input
///     by limits::kMaxImportBytes and limits::kMaxImportLines;
///   * tokens are separated by single spaces or tabs; a token containing spaces
///     must be quoted with the escape syntax of escape_text();
///   * the grammar is ASCII: any non-ASCII byte in a command keyword, key or
///     unquoted value is rejected;
///   * unknown keys, duplicate keys, missing required keys and out-of-range
///     numbers are rejected with a stable error code, never ignored.
///
/// Commands:
///
///   facility <extref>                       (exactly once, required)
///   provenance producer=<text> origin=<token> witness=<text>
///              [source=<extref>] [authority-epoch=<n>]
///   evidence <kind> producer=<text> subject=<extref>
///              [generation=<n>] [observation=<n>]
///   node <id> <kind> [key=value ...]
///   edge <edge-id> supplies <node>.<port> -> <node>.<port>
///   edge <edge-id> returns  <node>.<port> -> <node>.<port>
///   edge <edge-id> serves   <node>.<port> -> <node>.<port>
///   edge <edge-id> pumps    <node>.<port> -> <node>.<port>
///   edge <edge-id> contains <node>.<port> -> <node>.<port>
///   edge <edge-id> depends  <node>.<port> -> <node>.<port>
///   alias <alias-id> <node-id-or-alias-id>
///   group <group-id> scope=<token> [scheme=<token>] [name=<text>] [basis=<text>]
///         [require-distinct-failure-domains] [require-independent-sources]
///         <member>[@<failure-domain-extref>] ...
///   changeover <group-id> max=<n> [name=<text>] <node>.<port> ...
///
/// where an external reference is written as <identity>, <kind>:<identity>,
/// <identity>@<generation> or <kind>:<identity>@<generation>. The kind prefix is
/// recognised only when the text before the first ':' is a declared
/// ExternalRefKind token; otherwise the whole token is the identity. When the
/// token contains '@', the text after the last '@' must be a canonical unsigned
/// decimal generation, otherwise the reference is rejected rather than guessed.
/// <kind> for nodes is a NodeKind token, and the per-kind keys are:
///
///   cooling_plant   kind=<plant-kind> [medium=<token>]
///                   [design-supply=<temp>] [max-supply=<temp>]
///   chiller         kind=<chiller-kind> [medium=<token>]
///                   [design-supply=<temp>] [max-supply=<temp>]
///   pump            kind=<pump-kind> role=<duty|standby|jockey>
///   cooling_loop    kind=<loop-kind> [medium=<token>]
///                   [design-supply=<temp>] [max-supply=<temp>]
///   cdu             kind=<cdu-kind> [medium=<token>]
///                   [design-supply=<temp>] [max-supply=<temp>]
///   crah            placement=<token> [medium=<token>] [max-supply=<temp>]
///   crac            placement=<token> [medium=<token>] [max-supply=<temp>]
///   manifold        kind=<manifold-kind> [medium=<token>]
///                   [design-supply=<temp>] [max-supply=<temp>]
///   branch          kind=<branch-kind> [medium=<token>]
///                   [design-supply=<temp>] [max-supply=<temp>]
///   thermal_zone    class=<zone-class>
///   cooling_sink    kind=<sink-kind> consumer=<text>
///                   [consumer-kind=<external-ref-kind>] [max-supply=<temp>]
///   cooling_source  kind=<source-kind> [medium=<token>] [design-supply=<temp>]
///
/// plus, for every node kind: [name=<text>] [ref=<extref>] (repeatable).
///
/// <temp> is decimal degrees Celsius with exactly three fraction digits and an
/// optional trailing 'C', e.g. 7.000, -12.500 or 7.000C; the value is an exact
/// integer in millidegrees and is bounded by
/// limits::kMinTemperatureMilliCelsius and limits::kMaxTemperatureMilliCelsius.
///
/// A token that contains spaces, '#', '=', ':' or '@' must be quoted with the
/// escape syntax of escape_text(); the exporter quotes exactly those tokens.
///
/// A group member is written as the member spelling optionally followed by '@'
/// and the failure-domain reference. '@' can never appear inside a canonical
/// identifier, so the separator is unambiguous even though canonical identities
/// themselves contain ':'. The bare spelling is exported whenever the member has
/// no declared failure domain, so an exported document always re-parses to the
/// same member identity.
///
/// Only the key 'ref' may repeat inside one node record; every other key is
/// unique and a repetition is DuplicateField.
///
/// Parsing never validates structure: the draft is validated by
/// Topology::create, which reports the primary structural error. Parse errors are
/// reported with the offending line number in Error::subject as "line <n>".
struct ImportStats {
  std::size_t lines = 0;
  std::size_t nodes = 0;
  std::size_t edges = 0;
  std::size_t groups = 0;
  std::size_t aliases = 0;
  std::size_t changeovers = 0;
  std::size_t evidence = 0;
};

Result<TopologyDraft> parse_import(std::string_view text, ImportStats* stats = nullptr);

/// Renders a generation in the import grammar. Used by the CLI for export, for
/// round-trip checks and for diff diagnostics. The rendering is deterministic
/// and re-parses to an equivalent draft (same canonical digest); it is a
/// diagnostic form, never an authority.
std::string export_import(const Topology& topology);

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_IMPORT_HPP
