// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/text.hpp"

#include <cstdint>
#include <limits>

#include "dccp/cooling_topology/limits.hpp"

namespace dccp::cooling_topology {
namespace {

// Decodes one UTF-8 sequence starting at index. Returns false for every
// malformed shape: overlong encodings, surrogates, values above U+10FFFF,
// truncated sequences and stray continuation bytes.
bool decode_utf8(std::string_view raw, std::size_t index, std::uint32_t& code_point, std::size_t& width) noexcept {
  const auto byte = [&](std::size_t offset) { return static_cast<std::uint8_t>(raw[index + offset]); };
  const std::size_t remaining = raw.size() - index;
  const std::uint8_t first = byte(0);

  if (first < 0x80) {
    code_point = first;
    width = 1;
    return true;
  }
  if (first < 0xC2) {
    return false;  // continuation byte or overlong two-byte lead (C0/C1)
  }
  if (first < 0xE0) {
    if (remaining < 2) {
      return false;
    }
    const std::uint8_t second = byte(1);
    if ((second & 0xC0) != 0x80) {
      return false;
    }
    code_point = (static_cast<std::uint32_t>(first & 0x1F) << 6) | (second & 0x3F);
    width = 2;
    return true;
  }
  if (first < 0xF0) {
    if (remaining < 3) {
      return false;
    }
    const std::uint8_t second = byte(1);
    const std::uint8_t third = byte(2);
    if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80) {
      return false;
    }
    if (first == 0xE0 && second < 0xA0) {
      return false;  // overlong
    }
    if (first == 0xED && second > 0x9F) {
      return false;  // surrogate range
    }
    code_point = (static_cast<std::uint32_t>(first & 0x0F) << 12) | (static_cast<std::uint32_t>(second & 0x3F) << 6) |
                 (third & 0x3F);
    width = 3;
    return true;
  }
  if (first < 0xF5) {
    if (remaining < 4) {
      return false;
    }
    const std::uint8_t second = byte(1);
    const std::uint8_t third = byte(2);
    const std::uint8_t fourth = byte(3);
    if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80) {
      return false;
    }
    if (first == 0xF0 && second < 0x90) {
      return false;  // overlong
    }
    if (first == 0xF4 && second > 0x8F) {
      return false;  // above U+10FFFF
    }
    code_point = (static_cast<std::uint32_t>(first & 0x07) << 18) | (static_cast<std::uint32_t>(second & 0x3F) << 12) |
                 (static_cast<std::uint32_t>(third & 0x3F) << 6) | (fourth & 0x3F);
    width = 4;
    return true;
  }
  return false;
}

void append_hex_byte(std::string& out, std::uint8_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  out.push_back(kHex[(value >> 4) & 0x0F]);
  out.push_back(kHex[value & 0x0F]);
}

int hex_value(char digit) noexcept {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return -1;
}

// A canonical decimal magnitude is 1..20 ASCII digits with no leading zero
// unless the whole magnitude is exactly "0". Signs, whitespace, underscores and
// every other byte are shape violations.
bool is_canonical_magnitude(std::string_view digits) noexcept {
  if (digits.empty()) {
    return false;
  }
  if (digits.size() > 1 && digits.front() == '0') {
    return false;
  }
  for (const char character : digits) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

// Accumulates a canonical magnitude. Returns false when the value does not fit
// in 64 bits; the caller reports a range violation, never a shape violation.
bool accumulate_magnitude(std::string_view digits, std::uint64_t& value) noexcept {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t out = 0;
  for (const char character : digits) {
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (out > (kMax - digit) / 10u) {
      return false;
    }
    out = out * 10u + digit;
  }
  value = out;
  return true;
}

// The subject of a rejected number is the offending text, truncated so that an
// untrusted record cannot inflate the error value without bound.
std::string number_subject(std::string_view text) { return std::string(text.substr(0, 64)); }

}  // namespace

bool is_valid_utf8(std::string_view raw) noexcept {
  std::size_t index = 0;
  while (index < raw.size()) {
    std::uint32_t code_point = 0;
    std::size_t width = 0;
    if (!decode_utf8(raw, index, code_point, width)) {
      return false;
    }
    if (code_point == 0) {
      return false;  // embedded NUL is never valid text
    }
    index += width;
  }
  return true;
}

bool is_valid_display_text(std::string_view raw, std::size_t max_bytes) noexcept {
  if (raw.size() > max_bytes) {
    return false;
  }
  std::size_t index = 0;
  while (index < raw.size()) {
    std::uint32_t code_point = 0;
    std::size_t width = 0;
    if (!decode_utf8(raw, index, code_point, width)) {
      return false;
    }
    if (code_point < 0x20 || code_point == 0x7F) {
      return false;  // C0 controls and DEL
    }
    if (code_point >= 0x80 && code_point <= 0x9F) {
      return false;  // C1 controls
    }
    index += width;
  }
  return true;
}

bool is_valid_external_identity(std::string_view raw, std::size_t max_bytes) noexcept {
  if (raw.empty() || raw.size() > max_bytes) {
    return false;
  }
  return is_valid_utf8(raw);
}

std::string escape_text(std::string_view raw) {
  std::string out;
  out.reserve(raw.size() + 2);
  out.push_back('"');
  for (const char raw_byte : raw) {
    const auto byte = static_cast<std::uint8_t>(raw_byte);
    switch (byte) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        // The rendering is ASCII-only: every byte outside printable ASCII is
        // escaped, so the result can be embedded in an ASCII document, a log
        // line or a terminal without depending on the reader's encoding.
        if (byte < 0x20 || byte >= 0x7F) {
          out.append("\\x");
          append_hex_byte(out, byte);
        } else {
          out.push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

Result<std::string> unescape_text(std::string_view escaped, std::size_t max_bytes) {
  if (escaped.size() < 2 || escaped.front() != '"' || escaped.back() != '"') {
    return Error(ErrorCode::MalformedRecord, "escaped text must be enclosed in double quotes");
  }
  std::string out;
  out.reserve(escaped.size());
  std::size_t index = 1;
  const std::size_t end = escaped.size() - 1;
  while (index < end) {
    // The bound is checked on every iteration, so it holds for literal bytes as
    // well as for escape sequences: a caller may rely on max_bytes to bound the
    // decoded text whatever the input mixes.
    if (out.size() > max_bytes) {
      return Error(ErrorCode::TextTooLong, "unescaped text exceeds the configured bound");
    }
    const char current = escaped[index];
    if (current != '\\') {
      out.push_back(current);
      ++index;
      continue;
    }
    if (index + 1 >= end) {
      return Error(ErrorCode::MalformedRecord, "escaped text ends with an incomplete escape");
    }
    const char marker = escaped[index + 1];
    index += 2;
    switch (marker) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'x': {
        if (index + 2 > end) {
          return Error(ErrorCode::MalformedRecord, "escaped text has a truncated \\x escape");
        }
        const int high = hex_value(escaped[index]);
        const int low = hex_value(escaped[index + 1]);
        if (high < 0 || low < 0) {
          return Error(ErrorCode::MalformedRecord, "escaped text has a non-hex \\x escape");
        }
        out.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        break;
      }
      default:
        return Error(ErrorCode::MalformedRecord, "escaped text has an unknown escape").with_subject(
            std::string(1, marker));
    }
  }
  if (out.size() > max_bytes) {
    return Error(ErrorCode::TextTooLong, "unescaped text exceeds the configured bound");
  }
  return out;
}

std::string ascii_lower(std::string_view raw) {
  std::string out(raw);
  for (char& character : out) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return out;
}

bool is_ascii_token(std::string_view raw) noexcept {
  if (raw.empty()) {
    return false;
  }
  for (const char character : raw) {
    const bool ok = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') || character == '.' || character == ':' ||
                    character == '_' || character == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool external_identity_equal(std::string_view lhs, std::string_view rhs) noexcept { return lhs == rhs; }

Result<std::uint64_t> parse_uint64(std::string_view text, std::uint64_t max_value) {
  if (!is_canonical_magnitude(text)) {
    return Error(ErrorCode::MalformedNumber,
                 "unsigned decimal text must be 1..20 digits with no sign, no leading zero, no whitespace and no "
                 "separator")
        .with_subject(number_subject(text));
  }
  std::uint64_t value = 0;
  if (!accumulate_magnitude(text, value)) {
    return Error(ErrorCode::LimitExceeded, "unsigned decimal text exceeds the 64-bit magnitude")
        .with_subject(number_subject(text));
  }
  if (value > max_value) {
    return Error(ErrorCode::LimitExceeded, "unsigned decimal text exceeds the permitted maximum")
        .with_subject(number_subject(text));
  }
  return value;
}

Result<std::int64_t> parse_int64(std::string_view text, std::int64_t min_value, std::int64_t max_value) {
  bool negative = false;
  std::string_view magnitude = text;
  if (!magnitude.empty() && magnitude.front() == '-') {
    negative = true;
    magnitude.remove_prefix(1);
  }
  if (!is_canonical_magnitude(magnitude)) {
    return Error(ErrorCode::MalformedNumber,
                 "signed decimal text must be an optional '-' followed by 1..20 digits with no leading zero, no "
                 "whitespace and no separator")
        .with_subject(number_subject(text));
  }
  std::uint64_t value = 0;
  if (!accumulate_magnitude(magnitude, value)) {
    return Error(ErrorCode::LimitExceeded, "signed decimal text exceeds the 64-bit magnitude")
        .with_subject(number_subject(text));
  }
  // The magnitude bound is applied before negation. INT64_MIN is reachable only
  // through the explicit comparison below, so no unchecked negation can produce
  // it.
  const std::uint64_t positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const std::uint64_t negative_limit = positive_limit + 1u;
  const std::uint64_t magnitude_limit = negative ? negative_limit : positive_limit;
  if (value > magnitude_limit) {
    return Error(ErrorCode::LimitExceeded, "signed decimal text exceeds the 64-bit magnitude")
        .with_subject(number_subject(text));
  }
  const std::int64_t signed_value =
      negative ? (value == negative_limit ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(value))
               : static_cast<std::int64_t>(value);
  if (signed_value < min_value || signed_value > max_value) {
    return Error(ErrorCode::LimitExceeded, "signed decimal text is outside the permitted range")
        .with_subject(number_subject(text));
  }
  return signed_value;
}

}  // namespace dccp::cooling_topology
