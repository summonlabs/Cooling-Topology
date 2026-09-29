// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/strong_id.hpp"

namespace dccp::cooling_topology {
namespace {

bool is_alphanumeric(char character) noexcept {
  return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z');
}

/// Interior bytes are ASCII alphanumeric or one of '.', '_', ':' and '-'. The
/// set is exactly the one is_ascii_token() accepts and the one identifier_syntax_help()
/// and the parse error message document, so the grammar has a single definition.
bool is_interior(char character) noexcept {
  return is_alphanumeric(character) || character == '.' || character == '_' || character == ':' ||
         character == '-';
}

}  // namespace

bool is_valid_identifier_syntax(std::string_view raw) noexcept {
  if (raw.empty() || raw.size() > limits::kMaxIdentifierBytes) {
    return false;
  }
  if (!is_alphanumeric(raw.front()) || !is_alphanumeric(raw.back())) {
    return false;
  }
  for (const char character : raw) {
    if (!is_interior(character)) {
      return false;
    }
  }
  return true;
}

std::string_view identifier_syntax_help() noexcept {
  return "1..128 bytes; first and last byte ASCII alphanumeric; interior bytes ASCII alphanumeric or one "
         "of . _ : -";
}

}  // namespace dccp::cooling_topology
