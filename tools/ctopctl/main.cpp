// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// ctopctl: inspection CLI for the Cooling Topology library.
//
// The tool never reimplements library logic: every structural answer is
// produced by dccp::cooling_topology from the file it was given. Output is
// deterministic - the same file and the same arguments always produce
// byte-identical stdout. There are no clocks, no threads, no random values and
// no locale-dependent formatting.
//
// Every answer is a statement about declared structure in one generation of one
// facility. The tool never reports that a component is running, that coolant or
// air is flowing, that a path has usable capacity, that a redundant source is
// eligible, that a zone is thermally safe, or that anything is authorized.
// Every command that reports a structural answer ends with the claim boundary.
//
// Exit codes: 0 success, 1 structural or validation rejection, 2 usage error,
// 3 I/O or store error. A bare invocation, an unknown command and an unknown
// option are all usage errors and exit 2; there is no global option.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/diff.hpp"
#include "dccp/cooling_topology/digest.hpp"
#include "dccp/cooling_topology/import.hpp"
#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/query.hpp"
#include "dccp/cooling_topology/store.hpp"
#include "dccp/cooling_topology/text.hpp"
#include "dccp/cooling_topology/topology.hpp"
#include "dccp/cooling_topology/version.hpp"

namespace {

using namespace dccp::cooling_topology;

constexpr int kExitOk = 0;
constexpr int kExitStructure = 1;
constexpr int kExitUsage = 2;
constexpr int kExitPersistence = 3;

/// Result of one command: empty on success, otherwise the failure to report.
class CliError;

using Outcome = std::optional<CliError>;

std::string text_of(std::string_view value) { return std::string(value); }
std::string bool_text(bool value) { return value ? std::string("true") : std::string("false"); }
std::string number_text(std::uint64_t value) { return std::to_string(value); }

std::string bound_text(std::string_view value, std::size_t max_bytes) {
  return value.size() <= max_bytes ? std::string(value) : std::string(value.substr(0, max_bytes));
}

/// A CLI failure: an exit code plus the stable report shape "<code>: <message>".
class CliError {
 public:
  CliError() = default;
  CliError(int exit_code, std::string code, std::string message, std::string subject)
      : exit_code_(exit_code),
        code_(std::move(code)),
        message_(std::move(message)),
        subject_(bound_text(subject, 192)) {}

  int exit_code() const noexcept { return exit_code_; }
  const std::string& code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  const std::string& subject() const noexcept { return subject_; }
  std::vector<std::string>& details() noexcept { return details_; }
  const std::vector<std::string>& details() const noexcept { return details_; }

  /// True when the failure was already printed in full on stdout, so the
  /// reporter must not repeat it on stderr.
  bool already_reported() const noexcept { return already_reported_; }
  CliError& mark_reported() noexcept {
    already_reported_ = true;
    return *this;
  }

 private:
  int exit_code_ = kExitStructure;
  std::string code_ = "internal_error";
  std::string message_;
  std::string subject_;
  std::vector<std::string> details_;
  bool already_reported_ = false;
};

int exit_code_for(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Persistence:
    case ErrorCategory::Lifecycle:
      return kExitPersistence;
    case ErrorCategory::Ok:
    case ErrorCategory::Argument:
    case ErrorCategory::Structure:
    case ErrorCategory::Authority:
    case ErrorCategory::Limit:
    case ErrorCategory::Internal:
      return kExitStructure;
  }
  return kExitStructure;
}

CliError from_library(const Error& error) {
  CliError failure(exit_code_for(error.category()), text_of(error_code_name(error.code())), error.message(),
                   error.subject());
  for (const std::string& detail : error.details()) {
    failure.details().push_back(detail);
  }
  return failure;
}

CliError usage_error(std::string_view token, std::string message) {
  return CliError(kExitUsage, "invalid_argument", std::move(message), text_of(token));
}

CliError cli_failure(int exit_code, ErrorCode code, std::string message, std::string subject = std::string()) {
  return CliError(exit_code, text_of(error_code_name(code)), std::move(message), std::move(subject));
}

void report(const CliError& failure) {
  if (failure.already_reported()) {
    return;
  }
  std::cerr << failure.code() << ": " << failure.message();
  if (!failure.subject().empty()) {
    std::cerr << " [subject=" << failure.subject() << "]";
  }
  std::cerr << "\n";
  for (const std::string& detail : failure.details()) {
    std::cerr << "detail: " << detail << "\n";
  }
  std::cerr << "boundary: " << systems_boundary() << "\n";
}

void print_posture() { std::cout << "posture: " << posture_statement() << "\n"; }

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string format_extref(const ExternalRef& reference) {
  const bool quoted = reference.identity.find(':') != std::string::npos ||
                      reference.identity.find('@') != std::string::npos ||
                      reference.identity.find(' ') != std::string::npos;
  std::string out = text_of(to_token(reference.kind));
  out.push_back(':');
  out += quoted ? escape_text(reference.identity) : reference.identity;
  if (reference.generation.bound()) {
    out.push_back('@');
    out += number_text(reference.generation.value());
  }
  return out;
}

std::string format_digest(const Digest& digest) { return digest.is_zero() ? std::string("none") : digest.to_hex(); }

std::vector<std::string> id_texts(const std::vector<NodeId>& ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const NodeId& id : ids) {
    out.push_back(id.str());
  }
  return out;
}

std::string joined(const std::vector<std::string>& values, char separator) {
  std::string out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      out.push_back(separator);
    }
    out += values[index];
  }
  return out;
}

std::string joined_ids(const std::vector<NodeId>& ids) { return joined(id_texts(ids), ','); }

std::string joined_or_none(const std::vector<std::string>& values, char separator) {
  const std::string text = joined(values, separator);
  return text.empty() ? std::string("none") : text;
}

std::string joined_ids_or_none(const std::vector<NodeId>& ids) { return joined_or_none(id_texts(ids), ','); }

std::string path_text(const std::vector<NodeId>& nodes) { return joined_or_none(id_texts(nodes), '>'); }

std::vector<std::string> edge_id_texts(const std::vector<EdgeId>& ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const EdgeId& id : ids) {
    out.push_back(id.str());
  }
  return out;
}

std::string edge_chain_text(const std::vector<EdgeId>& edges) { return joined_or_none(edge_id_texts(edges), ','); }

// ---------------------------------------------------------------------------
// Bounded file access and identity parsing
// ---------------------------------------------------------------------------

Outcome read_input_file(const std::string& path, std::size_t max_bytes, std::string& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return cli_failure(kExitUsage, ErrorCode::InvalidArgument, "input file cannot be opened", path);
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) {
    return cli_failure(kExitUsage, ErrorCode::InvalidArgument, "input file size cannot be determined", path);
  }
  const std::uint64_t length = static_cast<std::uint64_t>(size);
  if (length > static_cast<std::uint64_t>(max_bytes)) {
    CliError failure =
        cli_failure(kExitStructure, ErrorCode::LimitExceeded, "input file exceeds the configured bound", path);
    failure.details().push_back("file_bytes=" + number_text(length));
    failure.details().push_back("bound=" + number_text(max_bytes));
    return failure;
  }
  input.seekg(0, std::ios::beg);
  bytes.assign(static_cast<std::size_t>(length), '\0');
  if (length != 0) {
    input.read(bytes.data(), static_cast<std::streamsize>(length));
    if (!input) {
      return cli_failure(kExitUsage, ErrorCode::InvalidArgument, "input file could not be read", path);
    }
  }
  return std::nullopt;
}

template <class Id>
Outcome parse_identifier(const std::string& text, std::string_view what, Id& value) {
  if (text.size() > limits::kMaxIdentifierBytes) {
    return usage_error(text, text_of(what) + " exceeds the configured identifier bound");
  }
  const auto parsed = Id::parse(text);
  if (!parsed.has_value()) {
    return usage_error(text, text_of(what) + ": " + parsed.error().message());
  }
  value = parsed.value();
  return std::nullopt;
}

/// Reads a document and builds its first generation. Parsing and validation are
/// the library's, never this tool's.
Outcome load_generation(const std::string& path, Topology& topology) {
  std::string document;
  if (auto error = read_input_file(path, limits::kMaxImportBytes, document)) {
    return error;
  }
  const auto parsed = parse_import(document, nullptr);
  if (!parsed.has_value()) {
    return from_library(parsed.error());
  }
  const auto created = Topology::create_first(parsed.value());
  if (!created.has_value()) {
    return from_library(created.error());
  }
  topology = std::move(created.value());
  return std::nullopt;
}

/// Resolves an identity (canonical node id or alias) to a node of the generation.
Outcome require_node(const Topology& topology, const std::string& spelling, const Node*& node) {
  NodeId identity;
  if (auto error = parse_identifier(spelling, "node identity", identity)) {
    return error;
  }
  const auto canonical = topology.resolve(identity);
  node = canonical.has_value() ? topology.find_node(canonical.value()) : nullptr;
  if (node == nullptr) {
    return cli_failure(kExitStructure, ErrorCode::NotFound, "node is not present in this generation", spelling);
  }
  return std::nullopt;
}

Outcome require_file_argument(const std::vector<std::string>& params, std::string_view verb, std::string& file,
                              std::vector<std::string>& rest) {
  if (params.empty()) {
    return usage_error(verb, text_of(verb) + " requires a <file.ctg> argument");
  }
  file = params.front();
  rest.assign(params.begin() + 1, params.end());
  return std::nullopt;
}

Outcome require_single_node(const std::vector<std::string>& rest, std::string_view verb, std::string& spelling) {
  if (rest.size() != 1) {
    return usage_error(verb, text_of(verb) + " requires exactly one <node> argument");
  }
  spelling = rest.front();
  return std::nullopt;
}

Outcome require_two_nodes(const std::vector<std::string>& rest, std::string_view verb, std::string& from,
                          std::string& to) {
  if (rest.size() != 2) {
    return usage_error(verb, text_of(verb) + " requires <from> <to> node arguments");
  }
  from = rest[0];
  to = rest[1];
  return std::nullopt;
}

std::string usage_text() {
  return "usage: ctopctl <command> [arguments]\n"
         "  version                       library version, systems boundary and claim posture\n"
         "  posture                       print the claim posture report verbatim\n"
         "  validate <file.ctg>           parse a draft and report every validation issue\n"
         "  show <file.ctg>               build the generation and render it for inspection\n"
         "  export <file.ctg>             build the generation and print it in the import grammar\n"
         "  sources <file.ctg>            structural sources of the generation\n"
         "  upstream <file.ctg> <node>    structural supply upstream closure of a node\n"
         "  downstream <file.ctg> <node>  structural supply downstream closure of a node\n"
         "  paths <file.ctg> <from> <to>  structurally possible supply paths\n"
         "  circuits <file.ctg> <a> <b>   complete supply-plus-return circuits\n"
         "  independent <file.ctg> <a> <b>  edge-disjoint supply paths between two elements\n"
         "  spof <file.ctg> <node>...     single points of structural dependency\n"
         "  components <file.ctg>         connected components of the structural graph\n"
         "  sink <file.ctg> <node>        how a cooling sink is structurally fed\n"
         "  zone <file.ctg> <node>        what a thermal zone serves and what serves it\n"
         "  blast <file.ctg> <node>       structural downstream radius of an element\n"
         "  group <file.ctg> <group-id>   structural verdict on a declared redundancy group\n"
         "  membership <file.ctg> <node>  redundancy groups a node participates in\n"
         "  diff <a.ctg> <b.ctg>          structural difference of two generations\n"
         "  store <dir> [head|verify|history|recover]  inspect or recover a durable store";
}

// ---------------------------------------------------------------------------
// Commands: preamble
// ---------------------------------------------------------------------------

Outcome command_version(const std::vector<std::string>& params) {
  if (!params.empty()) {
    return usage_error(params.front(), "version takes no arguments");
  }
  std::cout << "ctopctl " << version_string() << "\n";
  std::cout << "library " << version_string() << "\n";
  std::cout << "component " << component_id() << "\n";
  std::cout << "boundary: " << systems_boundary() << "\n";
  print_posture();
  return std::nullopt;
}

Outcome command_posture(const std::vector<std::string>& params) {
  if (!params.empty()) {
    return usage_error(params.front(), "posture takes no arguments");
  }
  std::cout << posture_report();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Commands: document inspection
// ---------------------------------------------------------------------------

Outcome command_validate(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "validate", file, rest)) {
    return error;
  }
  if (!rest.empty()) {
    return usage_error(rest.front(), "validate takes exactly one file argument");
  }
  std::string document;
  if (auto error = read_input_file(file, limits::kMaxImportBytes, document)) {
    return error;
  }
  ImportStats stats;
  const auto parsed = parse_import(document, &stats);
  if (!parsed.has_value()) {
    return from_library(parsed.error());
  }
  const ValidationReport validation = Topology::validate_draft(parsed.value());
  std::cout << "validate " << file << " lines=" << stats.lines << " nodes=" << stats.nodes
            << " edges=" << stats.edges << " groups=" << stats.groups << " aliases=" << stats.aliases
            << " changeovers=" << stats.changeovers << " evidence=" << stats.evidence << "\n";
  for (const ValidationIssue& issue : validation.issues) {
    std::cout << to_token(issue.severity) << " " << error_code_name(issue.code) << " " << issue.subject << ": "
              << issue.message << "\n";
  }
  std::cout << "issues errors=" << validation.error_count() << " warnings=" << validation.warning_count()
            << " valid=" << bool_text(validation.valid()) << "\n";
  const ValidationIssue* primary = validation.primary();
  if (primary == nullptr) {
    print_posture();
    return std::nullopt;
  }
  std::cout << "primary " << error_code_name(primary->code) << " "
            << (primary->subject.empty() ? std::string("-") : primary->subject) << ": " << primary->message << "\n";
  print_posture();
  // The report above already named the primary error, so the reporter is asked
  // not to repeat it on stderr; the exit code still carries the rejection.
  CliError failure = cli_failure(kExitStructure, primary->code, primary->message, primary->subject);
  return failure.mark_reported();
}

Outcome command_show(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "show", file, rest)) {
    return error;
  }
  if (!rest.empty()) {
    return usage_error(rest.front(), "show takes exactly one file argument");
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  // render_text() is derived from the validated value and ends with the claim
  // boundary; it is never accepted as input.
  std::cout << topology.render_text();
  return std::nullopt;
}

Outcome command_export(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "export", file, rest)) {
    return error;
  }
  if (!rest.empty()) {
    return usage_error(rest.front(), "export takes exactly one file argument");
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const std::string document = export_import(topology);
  std::cout << document;
  if (document.empty() || document.back() != '\n') {
    std::cout << "\n";
  }
  return std::nullopt;
}

Outcome command_sources(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "sources", file, rest)) {
    return error;
  }
  if (!rest.empty()) {
    return usage_error(rest.front(), "sources takes exactly one file argument");
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const std::vector<NodeId> sources = topology.structural_sources();
  std::cout << "sources generation=" << topology.generation().value() << " count=" << sources.size() << "\n";
  for (const NodeId& id : sources) {
    const Node* node = topology.find_node(id);
    std::cout << id.str() << " kind=" << (node == nullptr ? std::string("unknown") : text_of(to_token(node->kind())))
              << "\n";
  }
  print_posture();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Commands: reachability and paths
// ---------------------------------------------------------------------------

Outcome print_traversal(std::string_view label, const NodeId& origin, ClaimClass claim,
                        const std::vector<ReachedElement>& elements, bool truncated,
                        std::uint64_t generation) {
  std::cout << label << " generation=" << generation << " origin=" << origin.str()
            << " claim=" << to_token(claim) << " count=" << elements.size()
            << " truncated=" << bool_text(truncated) << "\n";
  for (const ReachedElement& element : elements) {
    std::cout << element.depth << " " << element.node.str() << " via="
              << (element.via_edge.empty() ? std::string("-") : element.via_edge.str())
              << " port=" << to_token(element.entered_port) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_upstream(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "upstream", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "upstream", spelling)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* node = nullptr;
  if (auto error = require_node(topology, spelling, node)) {
    return error;
  }
  const auto result = upstream_of(topology, node->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  return print_traversal("upstream", node->id, result.value().claim, result.value().elements,
                         result.value().truncated, topology.generation().value());
}

Outcome command_downstream(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "downstream", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "downstream", spelling)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* node = nullptr;
  if (auto error = require_node(topology, spelling, node)) {
    return error;
  }
  const auto result = downstream_of(topology, node->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  return print_traversal("downstream", node->id, result.value().claim, result.value().elements,
                         result.value().truncated, topology.generation().value());
}

Outcome command_paths(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "paths", file, rest)) {
    return error;
  }
  std::string from_text;
  std::string to_text;
  if (auto error = require_two_nodes(rest, "paths", from_text, to_text)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* from = nullptr;
  const Node* to = nullptr;
  if (auto error = require_node(topology, from_text, from)) {
    return error;
  }
  if (auto error = require_node(topology, to_text, to)) {
    return error;
  }
  const auto result = possible_supply_paths(topology, from->id, to->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const PathQueryResult& paths = result.value();
  std::cout << "paths from=" << paths.from.str() << " to=" << paths.to.str() << " claim=" << to_token(paths.claim)
            << " count=" << paths.paths.size() << " truncated=" << bool_text(paths.truncated)
            << " reachable_sources=" << paths.reachable_sources.size() << "\n";
  std::cout << "sources " << joined_ids_or_none(paths.reachable_sources) << "\n";
  for (std::size_t index = 0; index < paths.paths.size(); ++index) {
    const CoolingPath& path = paths.paths[index];
    std::cout << "path " << index << " nodes=" << path_text(path.nodes)
              << " edges=" << edge_chain_text(path.edges)
              << " passes_pump_host=" << bool_text(path.passes_pump_host)
              << " crosses_changeover=" << bool_text(path.crosses_changeover)
              << " origin_source_count=" << path.origin_source_count << "\n";
  }
  for (const PathPairExclusivity& pair : paths.exclusive_pairs) {
    std::cout << "exclusive " << pair.first << " " << pair.second << " reason=" << to_token(pair.reason)
              << " witness=" << escape_text(pair.witness) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_circuits(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "circuits", file, rest)) {
    return error;
  }
  std::string from_text;
  std::string to_text;
  if (auto error = require_two_nodes(rest, "circuits", from_text, to_text)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* from = nullptr;
  const Node* to = nullptr;
  if (auto error = require_node(topology, from_text, from)) {
    return error;
  }
  if (auto error = require_node(topology, to_text, to)) {
    return error;
  }
  const auto result = possible_circuits(topology, from->id, to->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const CircuitQueryResult& circuits = result.value();
  std::cout << "circuits from=" << circuits.from.str() << " to=" << circuits.to.str()
            << " claim=" << to_token(circuits.claim) << " count=" << circuits.circuits.size()
            << " truncated=" << bool_text(circuits.truncated) << "\n";
  for (std::size_t index = 0; index < circuits.circuits.size(); ++index) {
    const CoolingCircuit& circuit = circuits.circuits[index];
    std::cout << "circuit " << index << " supply_nodes=" << path_text(circuit.supply_nodes)
              << " supply_edges=" << edge_chain_text(circuit.supply_edges)
              << " return_nodes=" << path_text(circuit.return_nodes)
              << " return_edges=" << edge_chain_text(circuit.return_edges)
              << " crosses_changeover=" << bool_text(circuit.crosses_changeover) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_independent(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "independent", file, rest)) {
    return error;
  }
  std::string from_text;
  std::string to_text;
  if (auto error = require_two_nodes(rest, "independent", from_text, to_text)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* from = nullptr;
  const Node* to = nullptr;
  if (auto error = require_node(topology, from_text, from)) {
    return error;
  }
  if (auto error = require_node(topology, to_text, to)) {
    return error;
  }
  const auto result = independent_supply_paths(topology, from->id, to->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const IndependentPathResult& independent = result.value();
  std::cout << "independent from=" << independent.from.str() << " to=" << independent.to.str()
            << " claim=" << to_token(independent.claim)
            << " edge_disjoint_paths=" << independent.path_count
            << " truncated=" << bool_text(independent.truncated) << "\n";
  for (std::size_t index = 0; index < independent.paths.size(); ++index) {
    const CoolingPath& path = independent.paths[index];
    std::cout << "path " << index << " nodes=" << path_text(path.nodes)
              << " edges=" << edge_chain_text(path.edges)
              << " passes_pump_host=" << bool_text(path.passes_pump_host) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_spof(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "spof", file, rest)) {
    return error;
  }
  if (rest.empty()) {
    return usage_error("spof", "spof requires at least one <node> argument");
  }
  if (rest.size() > limits::kMaxPathQuerySubjects) {
    return usage_error(rest[limits::kMaxPathQuerySubjects], "spof names more subjects than the bound");
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  std::vector<NodeId> subjects;
  for (const std::string& spelling : rest) {
    const Node* node = nullptr;
    if (auto error = require_node(topology, spelling, node)) {
      return error;
    }
    subjects.push_back(node->id);
  }
  const auto result = single_points_of_structural_dependency(topology, subjects, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const SinglePointResult& spof = result.value();
  std::cout << "spof subjects=" << spof.subjects.size() << " claim=" << to_token(spof.claim)
            << " count=" << spof.points.size() << " truncated=" << bool_text(spof.truncated) << "\n";
  for (const DependencyPoint& point : spof.points) {
    std::cout << "point " << point.node.str() << " disconnects=" << point.disconnected_subjects.size()
              << " all=" << bool_text(point.disconnects_all_subjects)
              << " structural_source=" << bool_text(point.is_structural_source)
              << " subjects=" << joined_ids_or_none(point.disconnected_subjects) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_components(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "components", file, rest)) {
    return error;
  }
  if (!rest.empty()) {
    return usage_error(rest.front(), "components takes exactly one file argument");
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const auto result = connected_components(topology, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const ComponentReport& components = result.value();
  std::cout << "components count=" << components.components.size() << " isolated=" << components.isolated.size()
            << " truncated=" << bool_text(components.truncated) << "\n";
  for (std::size_t index = 0; index < components.components.size(); ++index) {
    std::cout << "component " << index << " size=" << components.components[index].size()
              << " nodes=" << joined_ids_or_none(components.components[index]) << "\n";
  }
  for (const NodeId& id : components.isolated) {
    std::cout << "isolated " << id.str() << "\n";
  }
  print_posture();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Commands: sinks, zones, blast radius
// ---------------------------------------------------------------------------

Outcome command_sink(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "sink", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "sink", spelling)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* node = nullptr;
  if (auto error = require_node(topology, spelling, node)) {
    return error;
  }
  const auto result = sink_service_report(topology, node->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const SinkServiceReport& service = result.value();
  std::cout << "sink " << service.sink.str() << " kind=" << to_token(service.kind)
            << " verdict=" << to_token(service.verdict) << " feeds=" << service.feeds.size()
            << " shared_sources=" << joined_ids_or_none(service.shared_sources) << "\n";
  for (const SinkFeed& feed : service.feeds) {
    std::cout << "feed " << feed.feeder.str() << " edge=" << feed.edge.str()
              << " sources=" << joined_ids_or_none(feed.sources) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_zone(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "zone", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "zone", spelling)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* node = nullptr;
  if (auto error = require_node(topology, spelling, node)) {
    return error;
  }
  const auto result = zone_report(topology, node->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const ZoneReport& facts = result.value();
  std::cout << "zone " << facts.zone.str() << " class=" << to_token(facts.zone_class)
            << " verdict=" << to_token(facts.verdict)
            << " serving_elements=" << facts.serving_elements.size()
            << " contained_sinks=" << facts.contained_sinks.size()
            << " contained_air_handlers=" << facts.contained_air_handlers.size()
            << " groups=" << facts.groups.size() << "\n";
  for (const NodeId& id : facts.serving_elements) {
    std::cout << "serving_element " << id.str() << "\n";
  }
  for (const NodeId& id : facts.contained_sinks) {
    std::cout << "contained_sink " << id.str() << "\n";
  }
  for (const NodeId& id : facts.contained_air_handlers) {
    std::cout << "contained_air_handler " << id.str() << "\n";
  }
  for (const RedundancyGroupId& id : facts.groups) {
    std::cout << "group " << id.str() << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_blast(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "blast", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "blast", spelling)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* node = nullptr;
  if (auto error = require_node(topology, spelling, node)) {
    return error;
  }
  const auto result = blast_radius(topology, node->id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const BlastRadiusResult& blast = result.value();
  std::cout << "blast origin=" << blast.origin.str() << " claim=" << to_token(blast.claim)
            << " downstream=" << blast.supplied_downstream.size()
            << " affected_sinks=" << blast.affected_sinks.size()
            << " affected_zones=" << blast.affected_zones.size()
            << " containment_peers=" << blast.containment_peers.size()
            << " truncated=" << bool_text(blast.truncated) << "\n";
  for (const ReachedElement& element : blast.supplied_downstream) {
    std::cout << "downstream " << element.depth << " " << element.node.str() << " via="
              << (element.via_edge.empty() ? std::string("-") : element.via_edge.str())
              << " port=" << to_token(element.entered_port) << "\n";
  }
  for (const NodeId& id : blast.affected_sinks) {
    std::cout << "affected_sink " << id.str() << "\n";
  }
  for (const NodeId& id : blast.affected_zones) {
    std::cout << "affected_zone " << id.str() << "\n";
  }
  for (const ContainmentPeer& peer : blast.containment_peers) {
    std::cout << "containment_peer " << peer.node.str()
              << " contains_origin=" << bool_text(peer.contains_origin)
              << " contained_by_origin=" << bool_text(peer.contained_by_origin)
              << " sibling=" << bool_text(peer.sibling) << "\n";
  }
  std::cout << "note: containment peers share an enclosure; containment is not a cooling relation\n";
  print_posture();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Commands: redundancy
// ---------------------------------------------------------------------------

Outcome command_group(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "group", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "group", spelling)) {
    return error;
  }
  RedundancyGroupId group_id;
  if (auto error = parse_identifier(spelling, "redundancy group identity", group_id)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const auto result = redundancy_group_report(topology, group_id, QueryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const RedundancyGroupReport& verdict = result.value();
  std::cout << "group " << verdict.group.str() << " scope=" << to_token(verdict.scope)
            << " scheme=" << to_token(verdict.scheme) << " verdict=" << to_token(verdict.verdict)
            << " independence_holds=" << bool_text(verdict.independence_holds)
            << " members=" << verdict.members.size()
            << " shared_sources=" << verdict.shared_sources.size()
            << " ancestor_members=" << verdict.ancestor_members.size()
            << " duplicate_failure_domains=" << verdict.duplicate_failure_domains.size()
            << " truncated=" << bool_text(verdict.truncated) << "\n";
  for (const RedundancyMemberReport& member : verdict.members) {
    std::cout << "member " << member.node.str() << " declared=" << escape_text(member.declared)
              << " sources=" << joined_ids_or_none(member.sources)
              << " failure_domain="
              << (member.failure_domain_declared ? escape_text(member.failure_domain) : std::string("none"))
              << "\n";
  }
  for (const NodeId& id : verdict.shared_sources) {
    std::cout << "shared_source " << id.str() << "\n";
  }
  for (const NodeId& id : verdict.ancestor_members) {
    std::cout << "ancestor_member " << id.str() << "\n";
  }
  for (const std::string& domain : verdict.duplicate_failure_domains) {
    std::cout << "duplicate_failure_domain " << escape_text(domain) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome command_membership(const std::vector<std::string>& params) {
  std::string file;
  std::vector<std::string> rest;
  if (auto error = require_file_argument(params, "membership", file, rest)) {
    return error;
  }
  std::string spelling;
  if (auto error = require_single_node(rest, "membership", spelling)) {
    return error;
  }
  Topology topology;
  if (auto error = load_generation(file, topology)) {
    return error;
  }
  const Node* node = nullptr;
  if (auto error = require_node(topology, spelling, node)) {
    return error;
  }
  const auto result = redundancy_membership(topology, node->id);
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const RedundancyMembershipResult& membership = result.value();
  std::cout << "membership " << membership.node.str() << " count=" << membership.memberships.size() << "\n";
  for (const GroupMembership& entry : membership.memberships) {
    std::cout << "group " << entry.group.str() << " index=" << entry.member_index
              << " declared=" << escape_text(entry.declared) << "\n";
  }
  print_posture();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Commands: diff and durable store
// ---------------------------------------------------------------------------

Outcome command_diff(const std::vector<std::string>& params) {
  if (params.size() != 2) {
    return usage_error("diff", "diff requires exactly two file arguments: <a.ctg> <b.ctg>");
  }
  const std::string& before_file = params[0];
  const std::string& after_file = params[1];
  Topology before;
  if (auto error = load_generation(before_file, before)) {
    return error;
  }
  // The second document is bound to the first as its parent, so the pair forms a
  // generation chain: 1 -> 2, with the parent digest taken from the first.
  std::string document;
  if (auto error = read_input_file(after_file, limits::kMaxImportBytes, document)) {
    return error;
  }
  const auto parsed = parse_import(document, nullptr);
  if (!parsed.has_value()) {
    return from_library(parsed.error());
  }
  const auto created = Topology::create(TopologyGeneration(2), before.generation(), before.digest(),
                                        parsed.value());
  if (!created.has_value()) {
    return from_library(created.error());
  }
  const Topology& after = created.value();
  const auto result = diff_topologies(before, after);
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const TopologyDiff& diff = result.value();
  std::cout << "diff before=" << before_file << " after=" << after_file
            << " before_generation=" << diff.before_generation.value()
            << " after_generation=" << diff.after_generation.value()
            << " before_digest=" << diff.before_digest.to_hex()
            << " after_digest=" << diff.after_digest.to_hex()
            << " entries=" << diff.entries.size() << " node_delta=" << diff.node_delta
            << " edge_delta=" << diff.edge_delta << " same_facility=" << bool_text(diff.same_facility) << "\n";
  for (const std::string& line : explain_diff(diff)) {
    std::cout << line << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome open_store(const std::string& root, StoreMode mode, Store& store) {
  StoreOptions options;
  options.root = root;
  options.mode = mode;
  options.create_if_missing = false;
  auto opened = Store::open(options);
  if (!opened.has_value()) {
    return from_library(opened.error());
  }
  store = std::move(opened.value());
  return std::nullopt;
}

Outcome print_store_context(const Store& store) {
  const auto info = store.info();
  if (!info.has_value()) {
    return from_library(info.error());
  }
  const StoreInfo& details = info.value();
  std::cout << "store " << details.store_id.str() << " root=" << details.root
            << " facility=" << format_extref(details.facility)
            << " mode=" << to_token(details.mode) << " open_state=" << to_token(details.open_state) << "\n";
  std::cout << "head=" << details.head.value() << " head_digest=" << format_digest(details.head_digest)
            << " floor=" << details.floor.value() << " epoch=" << details.epoch.value()
            << " incarnation=" << details.incarnation.value()
            << " retained_generations=" << details.retained_generations
            << " idempotency_records=" << details.idempotency_records
            << " writable=" << bool_text(details.writable)
            << " publication_allowed=" << bool_text(details.publication_allowed) << "\n";
  return std::nullopt;
}

Outcome store_head(const Store& store) {
  if (auto error = print_store_context(store)) {
    return error;
  }
  const auto head = store.head();
  if (!head.has_value()) {
    return from_library(head.error());
  }
  const Topology& topology = head.value();
  std::cout << "generation=" << topology.generation().value()
            << " parent=" << topology.parent_generation().value()
            << " parent_digest=" << format_digest(topology.parent_digest())
            << " digest=" << topology.digest().to_hex() << "\n";
  std::cout << "nodes=" << topology.node_count() << " edges=" << topology.edge_count()
            << " groups=" << topology.group_count() << " aliases=" << topology.aliases().size()
            << " changeovers=" << topology.changeovers().size() << "\n";
  print_posture();
  return std::nullopt;
}

Outcome store_history(const Store& store) {
  if (auto error = print_store_context(store)) {
    return error;
  }
  const auto history = store.history();
  if (!history.has_value()) {
    return from_library(history.error());
  }
  const std::vector<HistoryEntry>& entries = history.value();
  std::cout << "history count=" << entries.size() << "\n";
  for (const HistoryEntry& entry : entries) {
    std::cout << "generation " << entry.generation.value() << " digest=" << format_digest(entry.digest)
              << " parent=" << entry.parent_generation.value()
              << " parent_digest=" << format_digest(entry.parent_digest)
              << " commit_sequence=" << entry.commit_sequence.value()
              << " file_bytes=" << entry.file_bytes << " head=" << bool_text(entry.is_head)
              << " chain_verified=" << bool_text(entry.chain_verified) << "\n";
  }
  print_posture();
  return std::nullopt;
}

Outcome store_verify(const Store& store) {
  if (auto error = print_store_context(store)) {
    return error;
  }
  VerifyOptions options;
  options.deep = true;
  options.verify_idempotency = true;
  options.verify_canonical_fixed_point = true;
  const auto result = store.verify(options);
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const VerifyReport& verify = result.value();
  std::cout << "verify head=" << verify.head.value() << " head_digest=" << format_digest(verify.head_digest)
            << " ok=" << bool_text(verify.ok()) << "\n";
  std::cout << "verified head=" << bool_text(verify.head_verified)
            << " manifest=" << bool_text(verify.manifest_verified)
            << " floor=" << bool_text(verify.floor_verified)
            << " chain=" << bool_text(verify.chain_verified)
            << " canonical_fixed_point=" << bool_text(verify.canonical_fixed_point_verified)
            << " recovered_state=" << bool_text(verify.recovered_state)
            << " publication_allowed=" << bool_text(verify.publication_allowed) << "\n";
  std::cout << "generations_present=" << verify.generations_present
            << " generations_verified=" << verify.generations_verified
            << " staged_residue_found=" << verify.staged_residue_found
            << " orphan_generations_found=" << verify.orphan_generations_found
            << " unreferenced_generations_found=" << verify.unreferenced_generations_found
            << " quarantined_found=" << verify.quarantined_found
            << " findings=" << verify.findings.size() << "\n";
  std::size_t defects = 0;
  for (const VerifyFinding& finding : verify.findings) {
    if (finding.severity == VerifySeverity::Defect) {
      ++defects;
    }
    std::cout << "finding " << to_token(finding.severity) << " " << finding.code << " " << finding.subject
              << " " << finding.detail << "\n";
  }
  print_posture();
  if (defects != 0) {
    CliError failure = cli_failure(kExitPersistence, ErrorCode::IntegrityFailure,
                                   "store verification found defect findings", store.root());
    failure.details().push_back("defects=" + number_text(defects));
    return failure;
  }
  return std::nullopt;
}

Outcome store_recover(Store& store) {
  if (auto error = print_store_context(store)) {
    return error;
  }
  const auto result = store.recover(RecoveryOptions{});
  if (!result.has_value()) {
    return from_library(result.error());
  }
  const RecoveryReport& recovery = result.value();
  std::cout << "recover outcome=" << to_token(recovery.outcome)
            << " head_before=" << recovery.head_before.value()
            << " head_after=" << recovery.head_after.value()
            << " head_digest_after=" << format_digest(recovery.head_digest_after)
            << " residue_removed=" << recovery.residue_removed
            << " floor_respected=" << bool_text(recovery.floor_respected) << "\n";
  std::cout << "explanation " << recovery.explanation << "\n";
  for (std::size_t index = 0; index < recovery.steps.size(); ++index) {
    std::cout << "step " << index << ": " << recovery.steps[index] << "\n";
  }
  print_posture();
  // A refusal to recover is reported as an Error by Store::recover(), so this
  // point is only reached for a recovery that actually completed.
  return std::nullopt;
}

Outcome command_store(const std::vector<std::string>& params) {
  if (params.empty()) {
    return usage_error("store", "store requires a store directory argument");
  }
  if (params.size() > 2) {
    return usage_error(params[2], "unexpected argument for store");
  }
  const std::string& root = params[0];
  const std::string operation = params.size() == 2 ? params[1] : std::string("head");
  if (operation != "head" && operation != "verify" && operation != "history" && operation != "recover") {
    return usage_error(operation, "store subcommand must be head, verify, history or recover");
  }
  // Recovery rewrites the head manifest and therefore needs the writer lock;
  // every inspection subcommand opens the store read-only.
  const StoreMode mode = operation == "recover" ? StoreMode::ReadWrite : StoreMode::ReadOnly;
  Store store;
  if (auto error = open_store(root, mode, store)) {
    return error;
  }
  Outcome outcome;
  if (operation == "head") {
    outcome = store_head(store);
  } else if (operation == "history") {
    outcome = store_history(store);
  } else if (operation == "verify") {
    outcome = store_verify(store);
  } else {
    outcome = store_recover(store);
  }
  const auto closed = store.close();
  if (outcome.has_value()) {
    return outcome;
  }
  if (!closed.has_value()) {
    return from_library(closed.error());
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

struct CommandEntry {
  std::string_view name;
  Outcome (*run)(const std::vector<std::string>&);
};

constexpr CommandEntry kCommandTable[] = {
    {"version", &command_version},       {"posture", &command_posture},
    {"validate", &command_validate},     {"show", &command_show},
    {"export", &command_export},         {"sources", &command_sources},
    {"upstream", &command_upstream},     {"downstream", &command_downstream},
    {"paths", &command_paths},           {"circuits", &command_circuits},
    {"independent", &command_independent}, {"spof", &command_spof},
    {"components", &command_components}, {"sink", &command_sink},
    {"zone", &command_zone},             {"blast", &command_blast},
    {"group", &command_group},           {"membership", &command_membership},
    {"diff", &command_diff},             {"store", &command_store},
};

struct Invocation {
  std::string command;
  std::vector<std::string> params;
};

Outcome parse_command_line(int argc, char** argv, Invocation& invocation) {
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index] == nullptr ? std::string() : std::string(argv[index]);
    if (invocation.command.empty()) {
      if (!token.empty() && token.front() == '-') {
        return usage_error(token, "unknown option; ctopctl takes no global options");
      }
      invocation.command = token;
      continue;
    }
    invocation.params.push_back(token);
  }
  if (invocation.command.empty()) {
    return usage_error("", std::string("no command given\n") + usage_text());
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Invocation invocation;
    if (auto error = parse_command_line(argc, argv, invocation)) {
      report(*error);
      return error->exit_code();
    }
    for (const CommandEntry& entry : kCommandTable) {
      if (entry.name == invocation.command) {
        if (auto error = entry.run(invocation.params)) {
          report(*error);
          return error->exit_code();
        }
        return kExitOk;
      }
    }
    const CliError error = usage_error(invocation.command,
                                       std::string("unknown command\n") + usage_text());
    report(error);
    return error.exit_code();
  } catch (const std::exception& exception) {
    CliError error(kExitStructure, "internal_error", "unhandled exception", std::string());
    error.details().push_back(bound_text(exception.what(), 512));
    report(error);
    return error.exit_code();
  } catch (...) {
    const CliError error(kExitStructure, "internal_error", "unhandled non-standard exception", std::string());
    report(error);
    return error.exit_code();
  }
}
