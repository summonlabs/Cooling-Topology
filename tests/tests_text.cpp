// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Proof obligations for the text layer: strict UTF-8, display-text rules,
// verbatim external identities, byte-exact escaping, the canonical decimal
// grammars and the identifier grammar. Every rejection is asserted by
// ErrorCode; no assertion depends on message text.

#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/cooling_topology/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;

using ct_test::Rng;

/// Asserts the rejection code of a failed operation, with the full error in the
/// failure message so a failing run is self-explaining.
void expect_code(const ct::Error& error, ct::ErrorCode expected, const std::string& what) {
  CT_CHECK_MSG(error.code() == expected,
               what + ": expected " + std::string(ct::error_code_name(expected)) + ", observed " +
                   std::string(ct::error_code_name(error.code())) + " [" + error.to_string() + "]");
}

/// Builds a byte string from explicit byte values, so no test depends on source
/// encoding or on escape-sequence interpretation.
std::string raw_bytes(std::initializer_list<unsigned int> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned int value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFFu)));
  }
  return out;
}

bool ascii_only(std::string_view text) {
  for (const char character : text) {
    if (static_cast<unsigned char>(character) > 0x7Eu) {
      return false;
    }
  }
  return true;
}

bool is_ascii_alnum(char character) {
  return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z');
}

/// Independent statement of the documented identifier grammar, used as the
/// reference the implementation is compared against.
bool reference_identifier(std::string_view raw) {
  if (raw.empty() || raw.size() > ct::limits::kMaxIdentifierBytes) {
    return false;
  }
  if (!is_ascii_alnum(raw.front()) || !is_ascii_alnum(raw.back())) {
    return false;
  }
  for (const char character : raw) {
    if (is_ascii_alnum(character) || character == '.' || character == '_' || character == ':' ||
        character == '-') {
      continue;
    }
    return false;
  }
  return true;
}

struct ReferenceU64 {
  bool accepted = false;
  std::uint64_t value = 0;
};

/// Independent statement of the canonical unsigned decimal grammar.
ReferenceU64 reference_uint64(std::string_view text, std::uint64_t max_value) {
  ReferenceU64 result;
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    return result;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return result;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      return result;
    }
    value = value * 10u + digit;
  }
  if (value > max_value) {
    return result;
  }
  result.accepted = true;
  result.value = value;
  return result;
}

}  // namespace

CT_TEST(text_utf8_accepts_every_valid_shape) {
  const std::vector<std::string> valid = {
      std::string(),
      std::string("plain ASCII text 1234 !@#$%^&*()_+-=[]{};,.?/"),
      raw_bytes({0xC2, 0x80}),                          // U+0080, lowest two-byte value
      raw_bytes({0xDF, 0xBF}),                          // U+07FF, highest two-byte value
      raw_bytes({0xE0, 0xA0, 0x80}),                    // U+0800, lowest three-byte value
      raw_bytes({0xED, 0x9F, 0xBF}),                    // U+D7FF, just below the surrogate range
      raw_bytes({0xEE, 0x80, 0x80}),                    // U+E000, just above the surrogate range
      raw_bytes({0xEF, 0xBF, 0xBD}),                    // U+FFFD
      raw_bytes({0xF0, 0x90, 0x80, 0x80}),              // U+10000, lowest four-byte value
      raw_bytes({0xF4, 0x8F, 0xBF, 0xBF}),              // U+10FFFF, highest code point
      raw_bytes({0x63, 0x61, 0x66, 0xC3, 0xA9}),        // "caf" + U+00E9
      raw_bytes({0xE2, 0x82, 0xAC, 0x20, 0xF0, 0x9F, 0x98, 0x80}),
  };
  for (const std::string& candidate : valid) {
    CT_CHECK_MSG(ct::is_valid_utf8(candidate), "expected valid UTF-8: " + ct::to_hex(candidate));
  }
  for (unsigned int value = 1; value <= 0x7Fu; ++value) {
    const std::string single = raw_bytes({value});
    CT_CHECK_MSG(ct::is_valid_utf8(single),
                 "ASCII byte " + std::to_string(value) + " must be valid UTF-8");
  }
}

CT_TEST(text_utf8_rejects_every_malformed_shape) {
  struct Case {
    const char* label;
    std::string bytes;
  };
  const std::vector<Case> cases = {
      {"embedded NUL", raw_bytes({0x61, 0x00, 0x62})},
      {"lone NUL", raw_bytes({0x00})},
      {"trailing NUL after valid text", raw_bytes({0x61, 0x62, 0x00})},
      {"stray continuation 0x80", raw_bytes({0x80})},
      {"stray continuation 0xBF", raw_bytes({0xBF})},
      {"overlong two-byte C0 80", raw_bytes({0xC0, 0x80})},
      {"overlong two-byte C0 AF", raw_bytes({0xC0, 0xAF})},
      {"overlong two-byte C1 81", raw_bytes({0xC1, 0x81})},
      {"overlong three-byte E0 80 80", raw_bytes({0xE0, 0x80, 0x80})},
      {"overlong three-byte E0 9F BF", raw_bytes({0xE0, 0x9F, 0xBF})},
      {"overlong four-byte F0 80 80 80", raw_bytes({0xF0, 0x80, 0x80, 0x80})},
      {"overlong four-byte F0 8F BF BF", raw_bytes({0xF0, 0x8F, 0xBF, 0xBF})},
      {"surrogate U+D800", raw_bytes({0xED, 0xA0, 0x80})},
      {"surrogate U+DFFF", raw_bytes({0xED, 0xBF, 0xBF})},
      {"above U+10FFFF (F4 90)", raw_bytes({0xF4, 0x90, 0x80, 0x80})},
      {"above U+10FFFF (F5)", raw_bytes({0xF5, 0x80, 0x80, 0x80})},
      {"invalid lead 0xFE", raw_bytes({0xFE})},
      {"invalid lead 0xFF", raw_bytes({0xFF})},
      {"truncated two-byte", raw_bytes({0xC3})},
      {"two-byte lead followed by ASCII", raw_bytes({0xC2, 0x20})},
      {"truncated three-byte (one of three)", raw_bytes({0xE2})},
      {"truncated three-byte (two of three)", raw_bytes({0xE2, 0x82})},
      {"three-byte with ASCII second byte", raw_bytes({0xE2, 0x28, 0xA1})},
      {"truncated four-byte (one of four)", raw_bytes({0xF0})},
      {"truncated four-byte (two of four)", raw_bytes({0xF0, 0x9F})},
      {"truncated four-byte (three of four)", raw_bytes({0xF0, 0x9F, 0x98})},
      {"valid prefix then invalid", raw_bytes({0x6F, 0x6B, 0xC1, 0x81})},
      {"invalid then valid suffix", raw_bytes({0x80, 0x6F, 0x6B})},
  };
  for (const Case& item : cases) {
    CT_CHECK_MSG(!ct::is_valid_utf8(item.bytes), std::string("expected invalid UTF-8: ") + item.label);
  }
}

CT_TEST(text_display_text_rejects_control_characters) {
  const std::vector<std::string> valid = {
      std::string(),
      std::string("Rack A-1 / row 3"),
      raw_bytes({0x63, 0x61, 0x66, 0xC3, 0xA9}),
      raw_bytes({0xC2, 0xA0}),              // U+00A0: not a C1 control
      raw_bytes({0xE2, 0x82, 0xAC}),
      std::string(ct::limits::kMaxDisplayNameBytes, 'x'),
  };
  for (const std::string& candidate : valid) {
    CT_CHECK_MSG(ct::is_valid_display_text(candidate, ct::limits::kMaxDisplayNameBytes),
                 "expected valid display text: " + ct::to_hex(candidate));
  }

  const std::vector<std::string> invalid = {
      raw_bytes({0x01}), raw_bytes({0x09}), raw_bytes({0x0A}), raw_bytes({0x0D}), raw_bytes({0x1F}),
      raw_bytes({0x7F}), raw_bytes({0x00}),
      raw_bytes({0x61, 0x01, 0x62}),
      raw_bytes({0xC2, 0x80}),              // U+0080, C1 control
      raw_bytes({0xC2, 0x9F}),              // U+009F, C1 control
      raw_bytes({0xC0, 0xAF}),              // overlong, also invalid UTF-8
      std::string(static_cast<std::size_t>(ct::limits::kMaxDisplayNameBytes) + 1u, 'x'),
  };
  for (const std::string& candidate : invalid) {
    CT_CHECK_MSG(!ct::is_valid_display_text(candidate, ct::limits::kMaxDisplayNameBytes),
                 "expected invalid display text: " + ct::to_hex(candidate));
  }
  // The bound is a byte bound: exactly max_bytes is accepted, one more is not.
  CT_CHECK(ct::is_valid_display_text("abcd", 4));
  CT_CHECK(!ct::is_valid_display_text("abcde", 4));
  CT_CHECK(ct::is_valid_display_text("", 0));
}

CT_TEST(text_external_identity_is_verbatim) {
  CT_CHECK(ct::is_valid_external_identity("Rack-A", ct::limits::kMaxExternalIdentityBytes));
  CT_CHECK(!ct::is_valid_external_identity("", ct::limits::kMaxExternalIdentityBytes));
  CT_CHECK(ct::is_valid_external_identity(std::string(ct::limits::kMaxExternalIdentityBytes, 'a'),
                                          ct::limits::kMaxExternalIdentityBytes));
  CT_CHECK(!ct::is_valid_external_identity(std::string(ct::limits::kMaxExternalIdentityBytes + 1u, 'a'),
                                           ct::limits::kMaxExternalIdentityBytes));
  CT_CHECK(!ct::is_valid_external_identity(raw_bytes({0x61, 0x00, 0x62}), ct::limits::kMaxExternalIdentityBytes));
  CT_CHECK(!ct::is_valid_external_identity(raw_bytes({0xC0, 0xAF}), ct::limits::kMaxExternalIdentityBytes));
  // No trimming: leading and trailing spaces are part of the identity.
  CT_CHECK(ct::is_valid_external_identity("  Rack  A  ", ct::limits::kMaxExternalIdentityBytes));

  auto upper = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "Rack-A", ct::ExternalGeneration(0));
  auto lower = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "rack-a", ct::ExternalGeneration(0));
  auto padded = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "  Rack-A  ", ct::ExternalGeneration(0));
  CT_REQUIRE(upper.has_value());
  CT_REQUIRE(lower.has_value());
  CT_REQUIRE(padded.has_value());
  CT_CHECK_EQ(upper->identity, std::string("Rack-A"));
  CT_CHECK_EQ(lower->identity, std::string("rack-a"));
  CT_CHECK_EQ(padded->identity, std::string("  Rack-A  "));
  CT_CHECK(!upper->same_binding_as(*lower));
  CT_CHECK(!upper->same_binding_as(*padded));
  CT_CHECK(upper->same_binding_as(*upper));
  CT_CHECK(ct::external_identity_equal("Rack-A", "Rack-A"));
  CT_CHECK(!ct::external_identity_equal("Rack-A", "rack-a"));
  CT_CHECK(!ct::external_identity_equal("Rack-A", "Rack-A "));

  const std::string with_nul = raw_bytes({0x61, 0x00, 0x62});
  CT_CHECK(!ct::external_identity_equal(with_nul, raw_bytes({0x61, 0x00, 0x63})));
  CT_CHECK(ct::external_identity_equal(with_nul, with_nul));

  auto empty = ct::ExternalRef::create(ct::ExternalRefKind::Rack, "", ct::ExternalGeneration(0));
  CT_REQUIRE(!empty.has_value());
  expect_code(empty.error(), ct::ErrorCode::MissingField, "empty external identity");
  auto too_long = ct::ExternalRef::create(ct::ExternalRefKind::Rack,
                                          std::string(ct::limits::kMaxExternalIdentityBytes + 1u, 'a'),
                                          ct::ExternalGeneration(0));
  CT_REQUIRE(!too_long.has_value());
  expect_code(too_long.error(), ct::ErrorCode::TextTooLong, "over-long external identity");
  auto nul = ct::ExternalRef::create(ct::ExternalRefKind::Rack, with_nul, ct::ExternalGeneration(0));
  CT_REQUIRE(!nul.has_value());
  expect_code(nul.error(), ct::ErrorCode::InvalidUtf8, "NUL-bearing external identity");

  // Ordering is (kind, identity bytes, generation) and nothing else.
  std::vector<ct::ExternalRef> refs = {
      *upper,
      ct::ExternalRef::create(ct::ExternalRefKind::Facility, "a", ct::ExternalGeneration(0)).value(),
      ct::ExternalRef::create(ct::ExternalRefKind::Rack, "Rack-A", ct::ExternalGeneration(5)).value(),
      ct::ExternalRef::create(ct::ExternalRefKind::Asset, "a", ct::ExternalGeneration(0)).value(),
  };
  std::vector<ct::ExternalRef> expected = refs;
  std::sort(expected.begin(), expected.end(), [](const ct::ExternalRef& lhs, const ct::ExternalRef& rhs) {
    if (lhs.kind != rhs.kind) {
      return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
    }
    if (lhs.identity != rhs.identity) {
      return lhs.identity < rhs.identity;
    }
    return lhs.generation.value() < rhs.generation.value();
  });
  std::sort(refs.begin(), refs.end());
  CT_REQUIRE(refs.size() == expected.size());
  for (std::size_t index = 0; index < refs.size(); ++index) {
    CT_CHECK_MSG(refs[index].same_binding_as(expected[index]),
                 "external reference ordering at index " + std::to_string(index));
  }
  // Kind order is Facility(0), Rack(1), Asset(2); within a kind the identity
  // bytes decide, and only then the generation binding.
  CT_CHECK_EQ(expected[0].kind, ct::ExternalRefKind::Facility);
  CT_CHECK_EQ(expected[0].identity, std::string("a"));
  CT_CHECK_EQ(expected[1].kind, ct::ExternalRefKind::Rack);
  CT_CHECK_EQ(expected[1].identity, std::string("Rack-A"));
  CT_CHECK_EQ(expected[1].generation.value(), static_cast<std::uint64_t>(0));
  CT_CHECK_EQ(expected[2].kind, ct::ExternalRefKind::Rack);
  CT_CHECK_EQ(expected[2].generation.value(), static_cast<std::uint64_t>(5));
  CT_CHECK_EQ(expected[3].kind, ct::ExternalRefKind::Asset);
  CT_CHECK_EQ(expected[3].identity, std::string("a"));
}

CT_TEST(text_escape_round_trip_every_byte) {
  std::string all;
  all.reserve(256);
  for (unsigned int value = 0; value <= 0xFFu; ++value) {
    all.push_back(static_cast<char>(value));
  }
  const std::string escaped = ct::escape_text(all);
  CT_CHECK(!escaped.empty() && escaped.front() == '"' && escaped.back() == '"');

  auto restored = ct::unescape_text(escaped, all.size());
  CT_REQUIRE(restored.has_value());
  CT_CHECK_MSG(*restored == all, "escape/unescape round trip over all 256 byte values");

  auto too_tight = ct::unescape_text(escaped, all.size() - 1u);
  CT_REQUIRE(!too_tight.has_value());
  expect_code(too_tight.error(), ct::ErrorCode::TextTooLong, "unescape bound one byte short");

  // Escaping is deterministic for the same bytes.
  CT_CHECK_EQ(ct::escape_text(all), escaped);
}

CT_TEST(text_escape_round_trip_random_mixed) {
  const std::string test_name = "text_escape_round_trip_random_mixed";
  Rng rng(ct_test::case_seed(test_name));
  std::string payload;
  payload.reserve(4096);
  for (int index = 0; index < 4096; ++index) {
    payload.push_back(static_cast<char>(rng.below(256)));
  }
  // Every escape marker explicitly, so the marker paths are always exercised.
  payload += raw_bytes({0x22, 0x5C, 0x0A, 0x0D, 0x09, 0x23, 0x3D, 0x40, 0x3A, 0x00, 0x7F});

  const std::string escaped = ct::escape_text(payload);
  auto restored = ct::unescape_text(escaped, payload.size());
  if (!restored.has_value()) {
    ct_test::report_note("seed=" + std::to_string(ct_test::run_seed()) + " case=" + test_name +
                         " escaped=" + escaped.substr(0, 128));
  }
  CT_REQUIRE(restored.has_value());
  CT_CHECK_MSG(*restored == payload, "random mixed escape round trip");

  auto too_tight = ct::unescape_text(escaped, payload.size() - 1u);
  CT_REQUIRE(!too_tight.has_value());
  expect_code(too_tight.error(), ct::ErrorCode::TextTooLong, "random unescape bound one byte short");
}

CT_TEST(text_escape_renders_ascii_only) {
  // text.hpp states that escape_text() renders arbitrary bytes into a quoted,
  // ASCII-only form, which is what makes the escaped form safe for a text
  // grammar whose unquoted alphabet is ASCII. Every byte above 0x7E therefore
  // has to be escaped rather than passed through.
  std::string all;
  all.reserve(256);
  for (unsigned int value = 0; value <= 0xFFu; ++value) {
    all.push_back(static_cast<char>(value));
  }
  const std::string escaped = ct::escape_text(all);
  std::string raw_high;
  for (const char character : escaped) {
    if (static_cast<unsigned char>(character) > 0x7Eu) {
      raw_high.push_back(character);
    }
  }
  CT_CHECK_MSG(ascii_only(escaped),
               "escape_text passed high bytes through unescaped: " + ct::to_hex(raw_high.substr(0, 32)));
  std::string raw_control;
  for (const char character : escaped) {
    if (static_cast<unsigned char>(character) < 0x20u) {
      raw_control.push_back(character);
    }
  }
  CT_CHECK_MSG(raw_control.empty(), "escape_text passed control bytes through unescaped");
  auto restored = ct::unescape_text(escaped, 256);
  CT_REQUIRE(restored.has_value());
  CT_CHECK(*restored == all);
}

CT_TEST(text_unescape_rejections) {
  struct Case {
    const char* label;
    std::string escaped;
    ct::ErrorCode code;
  };
  const std::vector<Case> cases = {
      {"unquoted text", "abc", ct::ErrorCode::MalformedRecord},
      {"missing closing quote", "\"abc", ct::ErrorCode::MalformedRecord},
      {"single quote byte", "\"", ct::ErrorCode::MalformedRecord},
      {"incomplete escape", "\"ab\\\"", ct::ErrorCode::MalformedRecord},
      {"truncated hex escape", "\"ab\\x1\"", ct::ErrorCode::MalformedRecord},
      {"non-hex escape", "\"ab\\xzz\"", ct::ErrorCode::MalformedRecord},
      {"unknown escape", "\"ab\\q\"", ct::ErrorCode::MalformedRecord},
  };
  for (const Case& item : cases) {
    auto parsed = ct::unescape_text(item.escaped, 64);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection: ") + item.label);
    if (!parsed.has_value()) {
      expect_code(parsed.error(), item.code, item.label);
    }
  }

  auto empty = ct::unescape_text("\"\"", 0);
  CT_REQUIRE(empty.has_value());
  CT_CHECK(empty->empty());
  auto literal = ct::unescape_text("\"a b#c=d\"", 7);
  CT_REQUIRE(literal.has_value());
  CT_CHECK_EQ(*literal, std::string("a b#c=d"));
  auto hash = ct::unescape_text("\"a#b\"", 3);
  CT_REQUIRE(hash.has_value());
  CT_CHECK_EQ(*hash, std::string("a#b"));
}

CT_TEST(text_parse_uint64_accepts_only_canonical_forms) {
  struct Accept {
    const char* text;
    std::uint64_t max_value;
    std::uint64_t value;
  };
  const std::vector<Accept> accepted = {
      {"0", UINT64_MAX, 0},
      {"1", UINT64_MAX, 1},
      {"10", UINT64_MAX, 10},
      {"10", 10, 10},
      {"42", 42, 42},
      {"18446744073709551615", UINT64_MAX, UINT64_MAX},
  };
  for (const Accept& item : accepted) {
    auto parsed = ct::parse_uint64(item.text, item.max_value);
    CT_REQUIRE(parsed.has_value());
    CT_CHECK_EQ(*parsed, item.value);
    CT_CHECK_EQ(std::string(ct::error_code_name(parsed.error().code())),
                std::string(ct::error_code_name(ct::ErrorCode::Ok)));
  }

  struct Reject {
    const char* text;
    std::uint64_t max_value;
    ct::ErrorCode code;
  };
  const std::vector<Reject> rejected = {
      {"", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"+1", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {" 1", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"1 ", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"007", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"-0x1", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"1_000", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"-1", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"0x10", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"1.0", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"1\t", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"00", UINT64_MAX, ct::ErrorCode::MalformedNumber},
      {"18446744073709551616", UINT64_MAX, ct::ErrorCode::LimitExceeded},
      {"99999999999999999999999999", UINT64_MAX, ct::ErrorCode::LimitExceeded},
      {"11", 10, ct::ErrorCode::LimitExceeded},
      {"1", 0, ct::ErrorCode::LimitExceeded},
  };
  for (const Reject& item : rejected) {
    auto parsed = ct::parse_uint64(item.text, item.max_value);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection of \"") + item.text + "\"");
    if (!parsed.has_value()) {
      expect_code(parsed.error(), item.code, std::string("parse_uint64(\"") + item.text + "\")");
      // The offending text is the subject; only the empty input has none.
      CT_CHECK_MSG(item.text[0] == '\0' || !parsed.error().subject().empty(),
                   std::string("parse_uint64 subject for \"") + item.text + "\"");
    }
  }
}

CT_TEST(text_parse_uint64_matches_reference) {
  const std::string test_name = "text_parse_uint64_matches_reference";
  Rng rng(ct_test::case_seed(test_name));
  const std::string pool = "0123456789+- _x.";
  const std::uint64_t max_value = 1000000;
  for (int iteration = 0; iteration < 5000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.below(24));
    std::string candidate;
    candidate.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      candidate.push_back(pool[rng.below(static_cast<std::uint32_t>(pool.size()))]);
    }
    const ReferenceU64 expected = reference_uint64(candidate, max_value);
    auto parsed = ct::parse_uint64(candidate, max_value);
    if (expected.accepted != parsed.has_value()) {
      ct_test::report_note("seed=" + std::to_string(ct_test::run_seed()) + " case=" + test_name +
                           " candidate=" + ct::to_hex(candidate));
    }
    CT_CHECK_MSG(expected.accepted == parsed.has_value(),
                 "parse_uint64 agreement for \"" + candidate + "\"");
    if (expected.accepted && parsed.has_value()) {
      CT_CHECK_EQ(*parsed, expected.value);
    }
  }
  // Canonical extremes are always accepted.
  auto huge = ct::parse_uint64("18446744073709551615", UINT64_MAX);
  CT_REQUIRE(huge.has_value());
  CT_CHECK_EQ(*huge, UINT64_MAX);
}

CT_TEST(text_parse_int64_reaches_int64_min_exactly) {
  auto zero = ct::parse_int64("0", INT64_MIN, INT64_MAX);
  CT_REQUIRE(zero.has_value());
  CT_CHECK_EQ(*zero, static_cast<std::int64_t>(0));

  auto minimum = ct::parse_int64("-9223372036854775808", INT64_MIN, INT64_MAX);
  CT_REQUIRE(minimum.has_value());
  CT_CHECK_EQ(*minimum, INT64_MIN);

  auto maximum = ct::parse_int64("9223372036854775807", INT64_MIN, INT64_MAX);
  CT_REQUIRE(maximum.has_value());
  CT_CHECK_EQ(*maximum, INT64_MAX);

  auto negative = ct::parse_int64("-1", INT64_MIN, INT64_MAX);
  CT_REQUIRE(negative.has_value());
  CT_CHECK_EQ(*negative, static_cast<std::int64_t>(-1));

  auto exact = ct::parse_int64("42", 42, 42);
  CT_REQUIRE(exact.has_value());
  CT_CHECK_EQ(*exact, static_cast<std::int64_t>(42));

  struct Reject {
    const char* text;
    std::int64_t min_value;
    std::int64_t max_value;
    ct::ErrorCode code;
  };
  const std::vector<Reject> rejected = {
      {"", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"-", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"+1", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {" 1", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"1 ", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"007", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"-0x1", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"1_000", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"--1", INT64_MIN, INT64_MAX, ct::ErrorCode::MalformedNumber},
      {"9223372036854775808", INT64_MIN, INT64_MAX, ct::ErrorCode::LimitExceeded},
      {"-9223372036854775809", INT64_MIN, INT64_MAX, ct::ErrorCode::LimitExceeded},
      {"-9223372036854775808", 0, INT64_MAX, ct::ErrorCode::LimitExceeded},
      {"5", 0, 4, ct::ErrorCode::LimitExceeded},
      {"-5", 0, 4, ct::ErrorCode::LimitExceeded},
  };
  for (const Reject& item : rejected) {
    auto parsed = ct::parse_int64(item.text, item.min_value, item.max_value);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection of \"") + item.text + "\"");
    if (!parsed.has_value()) {
      expect_code(parsed.error(), item.code, std::string("parse_int64(\"") + item.text + "\")");
    }
  }
}

CT_TEST(text_ascii_lower_and_ascii_token) {
  CT_CHECK_EQ(ct::ascii_lower("AbC-1._:z"), std::string("abc-1._:z"));
  CT_CHECK_EQ(ct::ascii_lower(""), std::string());
  CT_CHECK_EQ(ct::ascii_lower("already-lower"), std::string("already-lower"));
  // Non-ASCII bytes are left untouched: the helper is ASCII-only by contract.
  CT_CHECK_EQ(ct::ascii_lower(raw_bytes({0xC3, 0x89, 0x41})), raw_bytes({0xC3, 0x89, 0x61}));

  CT_CHECK(ct::is_ascii_token("a"));
  CT_CHECK(ct::is_ascii_token("AZaz09"));
  CT_CHECK(ct::is_ascii_token("a.b:c_d-e"));
  CT_CHECK(ct::is_ascii_token("0"));
  CT_CHECK(!ct::is_ascii_token(""));
  CT_CHECK(!ct::is_ascii_token("a b"));
  CT_CHECK(!ct::is_ascii_token("a\tb"));
  CT_CHECK(!ct::is_ascii_token("a/b"));
  CT_CHECK(!ct::is_ascii_token("a#b"));
  CT_CHECK(!ct::is_ascii_token("a=b"));
  CT_CHECK(!ct::is_ascii_token("a@b"));
  CT_CHECK(!ct::is_ascii_token(raw_bytes({0x41, 0xC3, 0xA9})));
}

CT_TEST(text_identifier_grammar_boundaries) {
  CT_CHECK(ct::is_valid_identifier_syntax("a"));
  CT_CHECK(ct::is_valid_identifier_syntax("0"));
  CT_CHECK(ct::is_valid_identifier_syntax(std::string(ct::limits::kMaxIdentifierBytes, 'a')));
  CT_CHECK(!ct::is_valid_identifier_syntax(std::string(ct::limits::kMaxIdentifierBytes + 1u, 'a')));
  CT_CHECK(!ct::is_valid_identifier_syntax(""));
  CT_CHECK(ct::is_valid_identifier_syntax("a.b:c-d"));
  CT_CHECK(ct::is_valid_identifier_syntax("plant:chp-a"));
  CT_CHECK(ct::is_valid_identifier_syntax("a..b"));
  CT_CHECK(ct::is_valid_identifier_syntax("a." + std::string("b")));
  // The documented interior set is [A-Za-z0-9._:-], which is exactly what
  // is_ascii_token() accepts and what identifier_syntax_help() states.
  CT_CHECK(ct::is_valid_identifier_syntax("pump_1"));
  CT_CHECK(ct::is_valid_identifier_syntax("a_b.c-d:e"));

  const std::vector<std::string> invalid = {
      ".a", ":a", "-a", "_a", "a.", "a:", "a-", "a_", "a b", "a/b", "a%b", "a#b", "a=b", "a@b",
      "a+b", "a\"b", raw_bytes({0x61, 0xC3, 0xA9}), raw_bytes({0xE2, 0x82, 0xAC}),
  };
  for (const std::string& candidate : invalid) {
    CT_CHECK_MSG(!ct::is_valid_identifier_syntax(candidate),
                 "expected invalid identifier: " + ct::to_hex(candidate));
  }

  auto good = ct::NodeId::parse("plant:chp-a");
  CT_REQUIRE(good.has_value());
  CT_CHECK_EQ(good->str(), std::string("plant:chp-a"));
  CT_CHECK(!good->empty());

  auto bad = ct::NodeId::parse("plant chp-a");
  CT_REQUIRE(!bad.has_value());
  expect_code(bad.error(), ct::ErrorCode::MalformedIdentifier, "NodeId::parse rejection");
  CT_CHECK(!bad.error().subject().empty());

  auto too_long = ct::NodeId::parse(std::string(ct::limits::kMaxIdentifierBytes + 1u, 'a'));
  CT_REQUIRE(!too_long.has_value());
  expect_code(too_long.error(), ct::ErrorCode::MalformedIdentifier, "over-long NodeId::parse rejection");

  CT_CHECK(ct::NodeId{}.empty());
  CT_CHECK(!std::string(ct::identifier_syntax_help()).empty());
}

CT_TEST(text_identifier_grammar_matches_reference) {
  const std::string test_name = "text_identifier_grammar_matches_reference";
  Rng rng(ct_test::case_seed(test_name));
  const std::string pool = raw_bytes({'a', 'b', 'Z', '0', '9', '.', ':', '-', '/', '_', ' ', '#', '=', '@',
                                      0x7F, 0xC3, 0xA9, 0x00});
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.below(141));
    std::string candidate;
    candidate.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      candidate.push_back(pool[rng.below(static_cast<std::uint32_t>(pool.size()))]);
    }
    const bool expected = reference_identifier(candidate);
    const bool observed = ct::is_valid_identifier_syntax(candidate);
    if (expected != observed) {
      ct_test::report_note("seed=" + std::to_string(ct_test::run_seed()) + " case=" + test_name +
                           " candidate=" + ct::to_hex(candidate));
    }
    CT_CHECK_MSG(expected == observed, "identifier grammar agreement for " + ct::to_hex(candidate));

    auto parsed = ct::NodeId::parse(candidate);
    CT_CHECK_MSG(parsed.has_value() == expected, "NodeId::parse agreement for " + ct::to_hex(candidate));
    if (parsed.has_value()) {
      CT_CHECK(parsed->str() == candidate);
    }
  }
}
