// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/import.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/model.hpp"
#include "dccp/cooling_topology/text.hpp"
#include "dccp/cooling_topology/units.hpp"

namespace dccp::cooling_topology {
namespace {

constexpr std::size_t kMaxKeysPerRecord = 24;

/// Attaches the offending line to an error without losing the original subject.
Error line_error(Error error, std::size_t line) {
  const std::string marker = "line " + std::to_string(line);
  if (error.subject() == marker) {
    return error;
  }
  if (!error.subject().empty()) {
    error.with_detail("subject: " + error.subject());
  }
  return error.with_subject(marker);
}

/// Runs a parse step and attaches the offending line to any failure, so every
/// error a caller sees names the line it came from.
template <class T>
Result<T> with_line(Result<T> result, std::size_t line) {
  if (!result.has_value()) {
    return line_error(result.error(), line);
  }
  return std::move(result).value();
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

/// Length of the quoted segment starting at begin (which must be '"'), counting
/// both quotes. Returns npos when the segment is unterminated.
std::size_t quoted_segment_length(std::string_view line, std::size_t begin) noexcept {
  std::size_t end = begin + 1;
  while (end < line.size()) {
    if (line[end] == '\\') {
      end += 2;
      continue;
    }
    if (line[end] == '"') {
      return end - begin + 1;
    }
    ++end;
  }
  return std::string_view::npos;
}

/// Index of the first comment marker that is not inside a quoted segment.
std::size_t comment_start(std::string_view line) noexcept {
  std::size_t index = 0;
  while (index < line.size()) {
    if (line[index] == '"') {
      const std::size_t length = quoted_segment_length(line, index);
      if (length == std::string_view::npos) {
        return std::string_view::npos;
      }
      index += length;
      continue;
    }
    if (line[index] == '#') {
      return index;
    }
    ++index;
  }
  return std::string_view::npos;
}

/// Splits one line into tokens. A quoted segment may appear inside a token, so
/// name="two words" is one token whose value is already decoded; the parser
/// therefore never unescapes a second time. Quoted segments are decoded with the
/// same syntax escape_text() produces, so a rendered document re-parses exactly.
Result<std::vector<std::string>> tokenize(std::string_view line) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
      ++index;
    }
    if (index >= line.size()) {
      break;
    }
    if (tokens.size() > limits::kMaxImportLineBytes) {
      return Error(ErrorCode::LimitExceeded, "line declares more tokens than the documented bound");
    }
    std::string token;
    while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
      if (line[index] == '"') {
        const std::size_t length = quoted_segment_length(line, index);
        if (length == std::string_view::npos) {
          return Error(ErrorCode::MalformedRecord, "unterminated quoted segment in a token");
        }
        CT_TRY(value,
               unescape_text(line.substr(index, length), limits::kMaxCanonicalStringBytes));
        token += value;
        index += length;
        continue;
      }
      const auto byte = static_cast<unsigned char>(line[index]);
      if (byte > 0x7Eu || byte < 0x21u) {
        return Error(ErrorCode::InvalidUtf8,
                     "an unquoted byte must be printable ASCII; quote it to carry other bytes")
            .with_subject(std::string(line.substr(index, 32)));
      }
      token.push_back(line[index]);
      ++index;
    }
    tokens.push_back(std::move(token));
  }
  return tokens;
}

// ---------------------------------------------------------------------------
// key=value records
// ---------------------------------------------------------------------------

struct KeyValues {
  std::vector<std::pair<std::string, std::string>> pairs;
  std::vector<std::string> flags;

  const std::string* find(std::string_view key) const {
    for (const auto& pair : pairs) {
      if (pair.first == key) {
        return &pair.second;
      }
    }
    return nullptr;
  }
  bool has_flag(std::string_view flag) const {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
  }
};

/// The one key that may legitimately repeat inside a record.
constexpr std::string_view kRepeatableKey = "ref";

Result<KeyValues> split_key_values(const std::vector<std::string>& tokens, std::size_t first,
                                   bool allow_flags) {
  KeyValues values;
  for (std::size_t index = first; index < tokens.size(); ++index) {
    const std::string& token = tokens[index];
    const std::size_t equals = token.find('=');
    if (equals == std::string::npos || equals == 0) {
      if (!allow_flags) {
        return Error(ErrorCode::MalformedRecord, "expected a key=value token, found a bare token")
            .with_subject(token.substr(0, 64));
      }
      values.flags.push_back(token);
      continue;
    }
    const std::string key = token.substr(0, equals);
    const std::string value = token.substr(equals + 1);
    if (key != kRepeatableKey && values.find(key) != nullptr) {
      return Error(ErrorCode::DuplicateField, "duplicate key in one record").with_subject(key);
    }
    if (value.empty()) {
      return Error(ErrorCode::MissingField, "key=value token has an empty value").with_subject(key);
    }
    if (values.pairs.size() >= kMaxKeysPerRecord) {
      return Error(ErrorCode::LimitExceeded, "record declares more keys than the documented bound");
    }
    values.pairs.emplace_back(key, value);
  }
  return values;
}

Result<std::string> require_key(const KeyValues& values, std::string_view key) {
  const std::string* value = values.find(key);
  if (value == nullptr) {
    return Error(ErrorCode::MissingField, "required key is missing").with_subject(std::string(key));
  }
  return *value;
}

constexpr std::string_view kCommonKeys[] = {"name", "ref"};

bool key_is_common(std::string_view key) noexcept {
  for (const std::string_view candidate : kCommonKeys) {
    if (candidate == key) {
      return true;
    }
  }
  return false;
}

Result<void> reject_unknown_keys(const KeyValues& values, const std::vector<std::string_view>& allowed) {
  for (const auto& pair : values.pairs) {
    const std::string_view key(pair.first);
    if (key_is_common(key)) {
      continue;
    }
    bool known = false;
    for (const std::string_view candidate : allowed) {
      known = known || candidate == key;
    }
    if (!known) {
      return Error(ErrorCode::UnknownEnumToken, "unknown key for this record").with_subject(pair.first);
    }
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

Result<ExternalRef> parse_extref(std::string_view text) {
  std::string_view body = text;
  ExternalRefKind kind = ExternalRefKind::Registry;
  const std::size_t colon = body.find(':');
  if (colon != std::string_view::npos) {
    const Result<ExternalRefKind> parsed = parse_external_ref_kind(body.substr(0, colon));
    if (parsed.has_value()) {
      kind = *parsed;
      body = body.substr(colon + 1);
    }
  }
  ExternalGeneration generation;
  const std::size_t at = body.rfind('@');
  if (at != std::string_view::npos) {
    CT_TRY(value, parse_uint64(body.substr(at + 1), UINT64_MAX));
    generation = ExternalGeneration(value);
    body = body.substr(0, at);
  }
  if (body.empty()) {
    return Error(ErrorCode::MissingField, "external reference has an empty identity");
  }
  if (body.size() > limits::kMaxExternalIdentityBytes) {
    return Error(ErrorCode::TextTooLong, "external identity exceeds the documented bound");
  }
  return ExternalRef::create(kind, std::string(body), generation);
}

Result<MilliCelsius> parse_temperature(std::string_view text) {
  if (!text.empty() && (text.back() == 'C' || text.back() == 'c')) {
    text.remove_suffix(1);
  }
  return MilliCelsius::parse_text(text);
}

Result<Endpoint> parse_endpoint(std::string_view text) {
  const std::size_t dot = text.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) {
    return Error(ErrorCode::MalformedRecord, "endpoint must be written as node.port")
        .with_subject(std::string(text.substr(0, 64)));
  }
  CT_TRY(node, NodeId::parse(text.substr(0, dot)));
  CT_TRY(port, parse_port_role(text.substr(dot + 1)));
  Endpoint endpoint;
  endpoint.node = std::move(node);
  endpoint.port = port;
  return endpoint;
}

Result<std::optional<CoolingMedium>> optional_medium(const KeyValues& values) {
  if (const std::string* medium = values.find("medium")) {
    CT_TRY(parsed, parse_cooling_medium(*medium));
    return std::optional<CoolingMedium>(parsed);
  }
  return std::optional<CoolingMedium>();
}

Result<std::optional<MilliCelsius>> optional_temperature(const KeyValues& values, std::string_view key) {
  if (const std::string* value = values.find(key)) {
    CT_TRY(parsed, parse_temperature(*value));
    return std::optional<MilliCelsius>(parsed);
  }
  return std::optional<MilliCelsius>();
}

struct CommonKeys {
  std::string display_name;
  std::vector<ExternalRef> references;
};

Result<CommonKeys> read_common(const KeyValues& values) {
  CommonKeys common;
  if (const std::string* name = values.find("name")) {
    common.display_name = *name;
  }
  for (const auto& pair : values.pairs) {
    if (pair.first != "ref") {
      continue;
    }
    if (common.references.size() >= limits::kMaxNodeReferences) {
      return Error(ErrorCode::LimitExceeded, "node declares more references than the documented bound");
    }
    CT_TRY(reference, parse_extref(pair.second));
    common.references.push_back(std::move(reference));
  }
  return common;
}

struct HydronicKeys {
  std::optional<CoolingMedium> medium;
  std::optional<MilliCelsius> design_supply;
  std::optional<MilliCelsius> max_supply;
};

Result<HydronicKeys> read_hydronic(const KeyValues& values) {
  HydronicKeys keys;
  CT_TRY(medium, optional_medium(values));
  keys.medium = medium;
  CT_TRY(design, optional_temperature(values, "design-supply"));
  keys.design_supply = design;
  CT_TRY(maximum, optional_temperature(values, "max-supply"));
  keys.max_supply = maximum;
  return keys;
}

// ---------------------------------------------------------------------------
// Record parsing
// ---------------------------------------------------------------------------

Result<Node> parse_node(const std::vector<std::string>& tokens, std::size_t line) {
  if (tokens.size() < 3) {
    return line_error(Error(ErrorCode::MissingField, "node command needs an identity and a node kind"), line);
  }
  CT_TRY(id, NodeId::parse(tokens[1]));
  CT_TRY(kind, parse_node_kind(tokens[2]));
  CT_TRY(values, split_key_values(tokens, 3, false));
  CT_TRY(common, read_common(values));
  CT_TRYV(reject_unknown_keys(values, [kind]() -> std::vector<std::string_view> {
    switch (kind) {
      case NodeKind::Pump:
        return {"kind", "role"};
      case NodeKind::Crah:
      case NodeKind::Crac:
        return {"placement", "medium", "max-supply"};
      case NodeKind::ThermalZone:
        return {"class"};
      case NodeKind::CoolingSink:
        return {"kind", "consumer", "consumer-kind", "max-supply"};
      case NodeKind::CoolingSource:
        return {"kind", "medium", "design-supply"};
      default:
        return {"kind", "medium", "design-supply", "max-supply"};
    }
  }()));

  NodeAttributes attributes;
  switch (kind) {
    case NodeKind::CoolingPlant: {
      CoolingPlantAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_plant_kind(required));
      value.kind = parsed;
      CT_TRY(hydronic, read_hydronic(values));
      value.medium = hydronic.medium;
      value.design_supply_temperature = hydronic.design_supply;
      value.max_acceptable_supply_temperature = hydronic.max_supply;
      attributes = value;
      break;
    }
    case NodeKind::Chiller: {
      ChillerAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_chiller_kind(required));
      value.kind = parsed;
      CT_TRY(hydronic, read_hydronic(values));
      value.medium = hydronic.medium;
      value.design_supply_temperature = hydronic.design_supply;
      value.max_acceptable_supply_temperature = hydronic.max_supply;
      attributes = value;
      break;
    }
    case NodeKind::Pump: {
      PumpAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_pump_kind(required));
      value.kind = parsed;
      CT_TRY(required_role, require_key(values, "role"));
      CT_TRY(role, parse_pump_role(required_role));
      value.role = role;
      attributes = value;
      break;
    }
    case NodeKind::CoolingLoop: {
      CoolingLoopAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_loop_kind(required));
      value.kind = parsed;
      CT_TRY(hydronic, read_hydronic(values));
      value.medium = hydronic.medium;
      value.design_supply_temperature = hydronic.design_supply;
      value.max_acceptable_supply_temperature = hydronic.max_supply;
      attributes = value;
      break;
    }
    case NodeKind::Cdu: {
      CduAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_cdu_kind(required));
      value.kind = parsed;
      CT_TRY(hydronic, read_hydronic(values));
      value.medium = hydronic.medium;
      value.design_supply_temperature = hydronic.design_supply;
      value.max_acceptable_supply_temperature = hydronic.max_supply;
      attributes = value;
      break;
    }
    case NodeKind::Crah: {
      CrahAttributes value;
      CT_TRY(required, require_key(values, "placement"));
      CT_TRY(parsed, parse_air_handler_placement(required));
      value.placement = parsed;
      CT_TRY(medium, optional_medium(values));
      value.medium = medium;
      CT_TRY(maximum, optional_temperature(values, "max-supply"));
      value.max_acceptable_supply_temperature = maximum;
      attributes = value;
      break;
    }
    case NodeKind::Crac: {
      CracAttributes value;
      CT_TRY(required, require_key(values, "placement"));
      CT_TRY(parsed, parse_air_handler_placement(required));
      value.placement = parsed;
      CT_TRY(medium, optional_medium(values));
      value.medium = medium;
      CT_TRY(maximum, optional_temperature(values, "max-supply"));
      value.max_acceptable_supply_temperature = maximum;
      attributes = value;
      break;
    }
    case NodeKind::Manifold: {
      ManifoldAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_manifold_kind(required));
      value.kind = parsed;
      CT_TRY(hydronic, read_hydronic(values));
      value.medium = hydronic.medium;
      value.design_supply_temperature = hydronic.design_supply;
      value.max_acceptable_supply_temperature = hydronic.max_supply;
      attributes = value;
      break;
    }
    case NodeKind::Branch: {
      BranchAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_branch_kind(required));
      value.kind = parsed;
      CT_TRY(hydronic, read_hydronic(values));
      value.medium = hydronic.medium;
      value.design_supply_temperature = hydronic.design_supply;
      value.max_acceptable_supply_temperature = hydronic.max_supply;
      attributes = value;
      break;
    }
    case NodeKind::ThermalZone: {
      ThermalZoneAttributes value;
      CT_TRY(required, require_key(values, "class"));
      CT_TRY(parsed, parse_zone_class(required));
      value.zone_class = parsed;
      attributes = value;
      break;
    }
    case NodeKind::CoolingSink: {
      CoolingSinkAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_sink_kind(required));
      value.kind = parsed;
      ExternalRefKind consumer_kind = ExternalRefKind::Consumer;
      if (const std::string* text = values.find("consumer-kind")) {
        CT_TRY(kind_parsed, parse_external_ref_kind(*text));
        consumer_kind = kind_parsed;
      }
      CT_TRY(identity, require_key(values, "consumer"));
      CT_TRY(consumer, parse_extref(identity));
      CT_TRY(rebuilt, ExternalRef::create(consumer_kind, identity, consumer.generation));
      value.consumer = std::move(rebuilt);
      CT_TRY(maximum, optional_temperature(values, "max-supply"));
      value.max_acceptable_supply_temperature = maximum;
      attributes = value;
      break;
    }
    case NodeKind::CoolingSource: {
      CoolingSourceAttributes value;
      CT_TRY(required, require_key(values, "kind"));
      CT_TRY(parsed, parse_source_kind(required));
      value.kind = parsed;
      CT_TRY(medium, optional_medium(values));
      value.medium = medium;
      CT_TRY(design, optional_temperature(values, "design-supply"));
      value.design_supply_temperature = design;
      attributes = value;
      break;
    }
  }
  CT_TRY(node, Node::create(std::move(id), std::move(attributes), std::move(common.display_name),
                            std::move(common.references)));
  (void)line;
  return node;
}

Result<Edge> parse_edge(const std::vector<std::string>& tokens, std::size_t line) {
  if (tokens.size() != 6 || tokens[4] != "->") {
    return line_error(Error(ErrorCode::MalformedRecord,
                            "edge command is: edge <id> <kind> <node>.<port> -> <node>.<port>"),
                      line);
  }
  CT_TRY(id, EdgeId::parse(tokens[1]));
  CT_TRY(kind, parse_edge_kind(tokens[2]));
  CT_TRY(from, parse_endpoint(tokens[3]));
  CT_TRY(to, parse_endpoint(tokens[5]));
  CT_TRY(edge, Edge::create(std::move(id), kind, std::move(from), std::move(to)));
  return edge;
}

Result<Alias> parse_alias(const std::vector<std::string>& tokens, std::size_t line) {
  if (tokens.size() != 3) {
    return line_error(Error(ErrorCode::MalformedRecord, "alias command is: alias <id> <node-id>"), line);
  }
  CT_TRY(id, AliasId::parse(tokens[1]));
  CT_TRY(target, NodeId::parse(tokens[2]));
  Alias alias;
  alias.id = std::move(id);
  alias.target = std::move(target);
  return alias;
}

Result<RedundancyGroup> parse_group(const std::vector<std::string>& tokens, std::size_t line) {
  if (tokens.size() < 3) {
    return line_error(Error(ErrorCode::MissingField,
                            "group command is: group <id> scope=<scope> [keys] <member>[:<domain>] ..."),
                      line);
  }
  CT_TRY(id, RedundancyGroupId::parse(tokens[1]));
  CT_TRY(values, split_key_values(tokens, 2, true));
  CT_TRYV(reject_unknown_keys(values, {"scheme", "scope", "basis"}));
  CT_TRY(scope_token, require_key(values, "scope"));
  CT_TRY(scope, parse_redundancy_scope(scope_token));
  RedundancyScheme scheme = RedundancyScheme::N;
  if (const std::string* text = values.find("scheme")) {
    CT_TRY(parsed, parse_redundancy_scheme(*text));
    scheme = parsed;
  }
  std::string display_name;
  if (const std::string* name = values.find("name")) {
    display_name = *name;
  }
  std::string basis;
  if (const std::string* text = values.find("basis")) {
    basis = *text;
  }
  std::vector<RedundancyMember> members;
  for (const std::string& flag : values.flags) {
    if (flag == "require-distinct-failure-domains" || flag == "require-independent-sources") {
      continue;
    }
    RedundancyMember member;
    const std::size_t at = flag.find('@');
    const std::string_view spelling =
        at == std::string::npos ? std::string_view(flag) : std::string_view(flag).substr(0, at);
    CT_TRY(node, NodeId::parse(spelling));
    member.node = std::move(node);
    member.declared = std::string(spelling);
    if (at != std::string::npos) {
      CT_TRY(domain, parse_extref(std::string_view(flag).substr(at + 1)));
      member.failure_domain = std::move(domain);
    }
    if (members.size() >= limits::kMaxGroupMemberCount) {
      return line_error(Error(ErrorCode::LimitExceeded, "group declares more members than the bound"), line);
    }
    members.push_back(std::move(member));
  }
  CT_TRY(group, RedundancyGroup::create(std::move(id), scheme, scope, std::move(display_name),
                                        std::move(basis), std::move(members),
                                        values.has_flag("require-distinct-failure-domains"),
                                        values.has_flag("require-independent-sources")));
  return group;
}

Result<ChangeoverGroup> parse_changeover(const std::vector<std::string>& tokens, std::size_t line) {
  if (tokens.size() < 4) {
    return line_error(Error(ErrorCode::MissingField,
                            "changeover command is: changeover <id> max=<n> [name=<text>] <node>.<port> ..."),
                      line);
  }
  CT_TRY(id, ChangeoverGroupId::parse(tokens[1]));
  CT_TRY(values, split_key_values(tokens, 2, true));
  CT_TRYV(reject_unknown_keys(values, {"max"}));
  CT_TRY(max_token, require_key(values, "max"));
  CT_TRY(max_concurrent, parse_uint64(max_token, UINT32_MAX));
  std::string display_name;
  if (const std::string* name = values.find("name")) {
    display_name = *name;
  }
  std::vector<Endpoint> members;
  for (const std::string& flag : values.flags) {
    CT_TRY(endpoint, parse_endpoint(flag));
    if (members.size() >= limits::kMaxChangeoverMemberCount) {
      return line_error(Error(ErrorCode::LimitExceeded, "changeover declares more members than the bound"),
                        line);
    }
    members.push_back(std::move(endpoint));
  }
  CT_TRY(group, ChangeoverGroup::create(std::move(id), std::move(display_name), std::move(members),
                                        static_cast<std::uint32_t>(max_concurrent)));
  return group;
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

bool needs_quoting(std::string_view text) {
  if (text.empty()) {
    return true;
  }
  for (const char byte : text) {
    const auto value = static_cast<unsigned char>(byte);
    if (value <= 0x20u || value > 0x7Eu) {
      return true;
    }
    if (byte == '#' || byte == '=' || byte == ':' || byte == '@' || byte == '"') {
      return true;
    }
  }
  return false;
}

std::string render_text_value(std::string_view text) {
  return needs_quoting(text) ? escape_text(text) : std::string(text);
}

std::string render_temperature(const std::optional<MilliCelsius>& value) {
  if (!value.has_value()) {
    return {};
  }
  std::string text = value->to_string();
  if (!text.empty() && text.back() == 'C') {
    text.pop_back();
  }
  return text;
}

std::string render_medium(const std::optional<CoolingMedium>& value) {
  return value.has_value() ? std::string(to_token(*value)) : std::string();
}

std::string extref_text(const ExternalRef& reference) {
  std::string out = std::string(to_token(reference.kind));
  out += ':';
  out += reference.identity;
  if (reference.generation.bound()) {
    out += '@';
    out += std::to_string(reference.generation.value());
  }
  return out;
}

bool extref_needs_quoting(const ExternalRef& reference) {
  return needs_quoting(reference.identity) || needs_quoting(std::string(to_token(reference.kind)));
}

}  // namespace

// ---------------------------------------------------------------------------
// parse_import
// ---------------------------------------------------------------------------

Result<TopologyDraft> parse_import(std::string_view text, ImportStats* stats) {
  if (text.size() > limits::kMaxImportBytes) {
    return Error(ErrorCode::LimitExceeded, "import document exceeds the documented byte bound");
  }
  TopologyDraft draft;
  ImportStats counted;
  bool facility_seen = false;
  bool provenance_seen = false;

  std::size_t line_number = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    if (line_number >= limits::kMaxImportLines) {
      return Error(ErrorCode::LimitExceeded, "import document exceeds the documented line bound");
    }
    std::size_t end = text.find('\n', begin);
    const bool last = end == std::string_view::npos;
    if (last) {
      end = text.size();
    }
    std::string_view line = text.substr(begin, end - begin);
    begin = end + 1;
    ++line_number;
    if (line.size() > limits::kMaxImportLineBytes) {
      return line_error(Error(ErrorCode::LimitExceeded, "import line exceeds the documented byte bound"),
                        line_number);
    }
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const std::size_t comment = comment_start(line);
    if (comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    CT_TRY(tokens, with_line(tokenize(line), line_number));
    if (tokens.empty()) {
      if (last) {
        break;
      }
      continue;
    }
    ++counted.lines;
    const std::string& command = tokens[0];
    // The whole command dispatch runs inside one lambda so that a single wrap
    // point attaches the offending line to every parse, key, value, enum and
    // range failure the command can produce.
    const Result<void> dispatched = [&]() -> Result<void> {
      if (command == "facility") {
        if (tokens.size() != 2) {
          return line_error(Error(ErrorCode::MalformedRecord, "facility command takes exactly one reference"),
                            line_number);
        }
        if (facility_seen) {
          return line_error(Error(ErrorCode::DuplicateField, "facility is declared more than once"), line_number);
        }
        CT_TRY(facility, parse_extref(tokens[1]));
        draft.facility = std::move(facility);
        facility_seen = true;
      } else if (command == "provenance") {
        if (provenance_seen) {
          return line_error(Error(ErrorCode::DuplicateField, "provenance is declared more than once"),
                            line_number);
        }
        CT_TRY(values, split_key_values(tokens, 1, false));
        CT_TRYV(reject_unknown_keys(values, {"producer", "origin", "witness", "source", "authority-epoch"}));
        CT_TRY(producer, require_key(values, "producer"));
        CT_TRY(origin_token, require_key(values, "origin"));
        CT_TRY(origin, parse_provenance_origin(origin_token));
        std::string witness;
        if (const std::string* text_value = values.find("witness")) {
          witness = *text_value;
        }
        std::optional<ExternalRef> source;
        if (const std::string* text_value = values.find("source")) {
          CT_TRY(reference, parse_extref(*text_value));
          source = std::move(reference);
        }
        AuthorityEpoch epoch;
        if (const std::string* text_value = values.find("authority-epoch")) {
          CT_TRY(parsed, parse_uint64(*text_value, UINT64_MAX));
          epoch = AuthorityEpoch(parsed);
        }
        CT_TRY(provenance, Provenance::create(std::move(producer), origin, std::move(witness), std::move(source),
                                              epoch, std::move(draft.provenance.evidence)));
        draft.provenance = std::move(provenance);
        provenance_seen = true;
      } else if (command == "evidence") {
        if (tokens.size() < 2) {
          return line_error(Error(ErrorCode::MissingField, "evidence command needs an evidence kind"),
                            line_number);
        }
        CT_TRY(kind, parse_evidence_kind(tokens[1]));
        CT_TRY(values, split_key_values(tokens, 2, false));
        CT_TRYV(reject_unknown_keys(values, {"producer", "subject", "generation", "observation"}));
        CT_TRY(producer, require_key(values, "producer"));
        CT_TRY(subject_token, require_key(values, "subject"));
        CT_TRY(subject, parse_extref(subject_token));
        EvidenceGeneration generation;
        if (const std::string* text_value = values.find("generation")) {
          CT_TRY(parsed, parse_uint64(*text_value, UINT64_MAX));
          generation = EvidenceGeneration(parsed);
        }
        ObservationSequence observation;
        if (const std::string* text_value = values.find("observation")) {
          CT_TRY(parsed, parse_uint64(*text_value, UINT64_MAX));
          observation = ObservationSequence(parsed);
        }
        CT_TRY(binding, EvidenceBinding::create(kind, std::move(producer), subject, generation, observation));
        for (const EvidenceBinding& existing : draft.provenance.evidence) {
          if (existing.same_binding_as(binding)) {
            return line_error(Error(ErrorCode::DuplicateField, "the same evidence binding is already recorded"),
                              line_number);
          }
        }
        draft.provenance.evidence.push_back(std::move(binding));
        ++counted.evidence;
      } else if (command == "node") {
        CT_TRY(node, parse_node(tokens, line_number));
        for (const Node& existing : draft.nodes) {
          if (existing.id == node.id) {
            return line_error(Error(ErrorCode::DuplicateIdentifier, "node identity is declared twice"),
                              line_number);
          }
        }
        draft.nodes.push_back(std::move(node));
        ++counted.nodes;
      } else if (command == "edge") {
        CT_TRY(edge, parse_edge(tokens, line_number));
        draft.edges.push_back(std::move(edge));
        ++counted.edges;
      } else if (command == "alias") {
        CT_TRY(alias, parse_alias(tokens, line_number));
        draft.aliases.push_back(std::move(alias));
        ++counted.aliases;
      } else if (command == "group") {
        CT_TRY(group, parse_group(tokens, line_number));
        draft.groups.push_back(std::move(group));
        ++counted.groups;
      } else if (command == "changeover") {
        CT_TRY(changeover, parse_changeover(tokens, line_number));
        draft.changeovers.push_back(std::move(changeover));
        ++counted.changeovers;
      } else {
          return Error(ErrorCode::UnknownEnumToken, "unknown import command").with_subject(command);
        }
      return ok();
    }();
    if (!dispatched.has_value()) {
      return line_error(dispatched.error(), line_number);
    }
    if (last) {
      break;
    }
  }

  if (!facility_seen) {
    return Error(ErrorCode::MissingField, "import document does not declare a facility");
  }
  if (!provenance_seen) {
    return Error(ErrorCode::MissingField, "import document does not declare provenance");
  }
  if (stats != nullptr) {
    *stats = counted;
  }
  return draft;
}

// ---------------------------------------------------------------------------
// export_import
// ---------------------------------------------------------------------------

std::string export_import(const Topology& topology) {
  std::string out;
  out.reserve(4096);
  out += "# cooling topology grammar 1 (ctg1); generated by dccp-cooling-topology\n";
  out += "facility ";
  out += extref_needs_quoting(topology.facility()) ? escape_text(extref_text(topology.facility()))
                                                   : extref_text(topology.facility());
  out += '\n';

  const Provenance& provenance = topology.provenance();
  out += "provenance producer=";
  out += escape_text(provenance.producer);
  out += " origin=";
  out += to_token(provenance.origin);
  out += " witness=";
  out += escape_text(provenance.witness);
  if (provenance.source_reference.has_value()) {
    out += " source=";
    out += escape_text(extref_text(*provenance.source_reference));
  }
  if (provenance.authority_epoch.bound()) {
    out += " authority-epoch=";
    out += std::to_string(provenance.authority_epoch.value());
  }
  out += '\n';
  for (const EvidenceBinding& binding : provenance.evidence) {
    out += "evidence ";
    out += to_token(binding.kind);
    out += " producer=";
    out += escape_text(binding.producer);
    out += " subject=";
    out += escape_text(extref_text(binding.subject));
    if (binding.generation.bound()) {
      out += " generation=";
      out += std::to_string(binding.generation.value());
    }
    if (binding.observation.present()) {
      out += " observation=";
      out += std::to_string(binding.observation.value());
    }
    out += '\n';
  }

  for (const Node& node : topology.nodes()) {
    out += "node ";
    out += node.id.str();
    out += ' ';
    out += to_token(node.kind());
    switch (node.kind()) {
      case NodeKind::CoolingPlant: {
        const auto& value = *node.as_cooling_plant();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Chiller: {
        const auto& value = *node.as_chiller();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Pump: {
        const auto& value = *node.as_pump();
        out += " kind=";
        out += to_token(value.kind);
        out += " role=";
        out += to_token(value.role);
        break;
      }
      case NodeKind::CoolingLoop: {
        const auto& value = *node.as_cooling_loop();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Cdu: {
        const auto& value = *node.as_cdu();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Crah: {
        const auto& value = *node.as_crah();
        out += " placement=";
        out += to_token(value.placement);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Crac: {
        const auto& value = *node.as_crac();
        out += " placement=";
        out += to_token(value.placement);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Manifold: {
        const auto& value = *node.as_manifold();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::Branch: {
        const auto& value = *node.as_branch();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::ThermalZone: {
        out += " class=";
        out += to_token(node.as_thermal_zone()->zone_class);
        break;
      }
      case NodeKind::CoolingSink: {
        const auto& value = *node.as_cooling_sink();
        out += " kind=";
        out += to_token(value.kind);
        out += " consumer-kind=";
        out += to_token(value.consumer.kind);
        out += " consumer=";
        out += render_text_value(value.consumer.identity +
                                 (value.consumer.generation.bound()
                                      ? "@" + std::to_string(value.consumer.generation.value())
                                      : std::string()));
        if (value.max_acceptable_supply_temperature.has_value()) {
          out += " max-supply=";
          out += render_temperature(value.max_acceptable_supply_temperature);
        }
        break;
      }
      case NodeKind::CoolingSource: {
        const auto& value = *node.as_cooling_source();
        out += " kind=";
        out += to_token(value.kind);
        if (value.medium.has_value()) {
          out += " medium=";
          out += to_token(*value.medium);
        }
        if (value.design_supply_temperature.has_value()) {
          out += " design-supply=";
          out += render_temperature(value.design_supply_temperature);
        }
        break;
      }
    }
    if (!node.display_name.empty()) {
      out += " name=";
      out += escape_text(node.display_name);
    }
    for (const ExternalRef& reference : node.references) {
      out += " ref=";
      out += escape_text(extref_text(reference));
    }
    out += '\n';
  }

  for (const Edge& edge : topology.edges()) {
    out += "edge ";
    out += edge.id.str();
    out += ' ';
    out += to_token(edge.kind);
    out += ' ';
    out += edge.from.node.str();
    out += '.';
    out += to_token(edge.from.port);
    out += " -> ";
    out += edge.to.node.str();
    out += '.';
    out += to_token(edge.to.port);
    out += '\n';
  }

  for (const Alias& alias : topology.aliases()) {
    out += "alias ";
    out += alias.id.str();
    out += ' ';
    out += alias.target.str();
    out += '\n';
  }

  for (const RedundancyGroup& group : topology.groups()) {
    out += "group ";
    out += group.id.str();
    out += " scope=";
    out += to_token(group.scope);
    out += " scheme=";
    out += to_token(group.scheme);
    if (!group.display_name.empty()) {
      out += " name=";
      out += escape_text(group.display_name);
    }
    if (!group.basis.empty()) {
      out += " basis=";
      out += escape_text(group.basis);
    }
    if (group.require_distinct_failure_domains) {
      out += " require-distinct-failure-domains";
    }
    if (group.require_independent_sources) {
      out += " require-independent-sources";
    }
    for (const RedundancyMember& member : group.members) {
      out += ' ';
      out += render_text_value(member.declared +
                               (member.failure_domain.has_value()
                                    ? "@" + extref_text(*member.failure_domain)
                                    : std::string()));
    }
    out += '\n';
  }

  for (const ChangeoverGroup& group : topology.changeovers()) {
    out += "changeover ";
    out += group.id.str();
    out += " max=";
    out += std::to_string(group.max_concurrent);
    if (!group.display_name.empty()) {
      out += " name=";
      out += escape_text(group.display_name);
    }
    for (const Endpoint& member : group.members) {
      out += ' ';
      out += member.node.str();
      out += '.';
      out += to_token(member.port);
    }
    out += '\n';
  }

  out += "# claim boundary: ";
  out += posture_statement();
  out += '\n';
  return out;
}

}  // namespace dccp::cooling_topology
