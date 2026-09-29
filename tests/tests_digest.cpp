// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Proof obligations for SHA-256, the digest value and the generation frame
// helpers: FIPS 180-4 vectors, streaming equivalence at every chunk size and
// every padding boundary, endianness of the integer helpers and the strict
// lowercase-hex rendering.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;

using ct_test::Rng;

void expect_code(const ct::Error& error, ct::ErrorCode expected, const std::string& what) {
  CT_CHECK_MSG(error.code() == expected,
               what + ": expected " + std::string(ct::error_code_name(expected)) + ", observed " +
                   std::string(ct::error_code_name(error.code())) + " [" + error.to_string() + "]");
}

std::string bytes_from(std::initializer_list<unsigned int> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned int value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFFu)));
  }
  return out;
}

/// Independent hex rendering used to compare against Digest::to_hex().
std::string hex_of(const std::array<std::uint8_t, ct::Sha256::kDigestBytes>& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2u);
  for (const std::uint8_t byte : bytes) {
    out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
    out.push_back(kDigits[byte & 0x0Fu]);
  }
  return out;
}

struct Vector {
  const char* label;
  std::string message;
  const char* expected_hex;
};

}  // namespace

CT_TEST(digest_matches_published_fips_vectors) {
  const std::string million_a(1000000, 'a');
  const std::vector<Vector> vectors = {
      {"empty", std::string(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", std::string("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"two-block 56-byte message",
       std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"four-block 112-byte message",
       std::string("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmn"
                   "opqrstnopqrstu"),
       "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
      {"one million a", million_a, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
  };
  for (const Vector& vector : vectors) {
    const ct::Digest digest = ct::digest_bytes(vector.message);
    CT_CHECK_MSG(digest.to_hex() == vector.expected_hex,
                 std::string("published vector ") + vector.label + ": " + digest.to_hex());
    CT_CHECK_EQ(hex_of(digest.bytes()), std::string(vector.expected_hex));
    CT_CHECK_MSG(!digest.is_zero(), std::string("vector ") + vector.label + " must not be the zero digest");
  }
  CT_CHECK(ct::Digest().is_zero());
  CT_CHECK(ct::Digest{}.to_hex() == std::string(64, '0'));
}

CT_TEST(digest_streaming_in_every_chunk_size_matches_one_shot) {
  const std::string test_name = "digest_streaming_in_every_chunk_size_matches_one_shot";
  Rng rng(ct_test::case_seed(test_name));
  std::string payload;
  payload.reserve(1000);
  for (int index = 0; index < 1000; ++index) {
    payload.push_back(static_cast<char>(rng.below(256)));
  }
  const ct::Digest expected = ct::digest_bytes(payload);
  for (std::size_t chunk = 1; chunk <= 80; ++chunk) {
    ct::Sha256 hasher;
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const std::size_t remaining = payload.size() - offset;
      const std::size_t take = remaining < chunk ? remaining : chunk;
      hasher.update(payload.data() + offset, take);
      offset += take;
    }
    const ct::Digest streamed(hasher.finish());
    if (!(streamed == expected)) {
      ct_test::report_note("seed=" + std::to_string(ct_test::run_seed()) + " case=" + test_name +
                           " chunk=" + std::to_string(chunk));
    }
    CT_CHECK_MSG(streamed == expected, "chunk size " + std::to_string(chunk));
  }
}

CT_TEST(digest_streaming_across_every_padding_boundary) {
  std::string payload;
  payload.reserve(200);
  for (unsigned int index = 0; index < 200; ++index) {
    payload.push_back(static_cast<char>(static_cast<unsigned char>((index * 7u + 3u) & 0xFFu)));
  }
  for (std::size_t length = 0; length <= 200; ++length) {
    const std::string_view view(payload.data(), length);
    const ct::Digest expected = ct::digest_bytes(view);
    for (std::size_t split = 0; split <= length; ++split) {
      ct::Sha256 hasher;
      hasher.update(view.substr(0, split));
      hasher.update(view.substr(split));
      const ct::Digest streamed(hasher.finish());
      CT_CHECK_MSG(streamed == expected,
                   "length " + std::to_string(length) + " split " + std::to_string(split));
    }
  }
}

CT_TEST(digest_integer_helpers_match_explicit_bytes) {
  // Little endian 0x01020304 is the byte string 04 03 02 01.
  ct::Sha256 little32;
  little32.update_u32(true, 0x01020304u);
  CT_CHECK(little32.finish() == ct::digest_bytes(bytes_from({0x04, 0x03, 0x02, 0x01})).bytes());

  ct::Sha256 big32;
  big32.update_u32(false, 0x01020304u);
  CT_CHECK(big32.finish() == ct::digest_bytes(bytes_from({0x01, 0x02, 0x03, 0x04})).bytes());

  ct::Sha256 little64;
  little64.update_u64(true, 0x0102030405060708ull);
  CT_CHECK(little64.finish() ==
           ct::digest_bytes(bytes_from({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01})).bytes());

  ct::Sha256 big64;
  big64.update_u64(false, 0x0102030405060708ull);
  CT_CHECK(big64.finish() ==
           ct::digest_bytes(bytes_from({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08})).bytes());

  // The two endiannesses disagree, so the comparison is not vacuous.
  CT_CHECK(!(little32.finish() == big32.finish()));
  CT_CHECK(!(little64.finish() == big64.finish()));

  // A mixed stream equals the same bytes fed in one call.
  ct::Sha256 mixed;
  mixed.update_u32(true, 0x01020304u);
  mixed.update_u64(false, 0x0102030405060708ull);
  ct::Sha256 explicit_bytes;
  explicit_bytes.update(bytes_from({0x04, 0x03, 0x02, 0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
  CT_CHECK(mixed.finish() == explicit_bytes.finish());

  // Byte-at-a-time feeding is the same stream.
  ct::Sha256 bytes_one_by_one;
  for (const unsigned int value : {0x04u, 0x03u, 0x02u, 0x01u}) {
    bytes_one_by_one.update_byte(static_cast<std::uint8_t>(value));
  }
  CT_CHECK(bytes_one_by_one.finish() == ct::digest_bytes(bytes_from({0x04, 0x03, 0x02, 0x01})).bytes());
}

CT_TEST(digest_reset_returns_the_hasher_to_its_initial_state) {
  ct::Sha256 hasher;
  hasher.update(std::string_view("abc"));
  const ct::Digest first(hasher.finish());
  hasher.reset();
  hasher.update(std::string_view("abc"));
  const ct::Digest second(hasher.finish());
  CT_CHECK(first == second);
  CT_CHECK(first == ct::digest_bytes("abc"));

  hasher.reset();
  hasher.update(std::string_view("abd"));
  CT_CHECK(!(ct::Digest(hasher.finish()) == first));
}

CT_TEST(digest_parse_hex_accepts_exactly_64_lowercase_characters) {
  const std::string valid = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  auto parsed = ct::Digest::parse_hex(valid);
  CT_REQUIRE(parsed.has_value());
  CT_CHECK_EQ(parsed->to_hex(), valid);
  CT_CHECK(*parsed == ct::digest_bytes(""));

  const std::vector<std::string> rejected = {
      std::string(),
      std::string(63, 'a'),
      std::string(65, 'a'),
      std::string(64, '0').replace(0, 1, "A"),          // uppercase hex digit
      valid.substr(0, 63) + "G",
      valid.substr(0, 63) + " ",
      std::string(64, 'z'),
  };
  for (const std::string& candidate : rejected) {
    auto value = ct::Digest::parse_hex(candidate);
    CT_CHECK_MSG(!value.has_value(), "expected rejection of \"" + candidate + "\"");
    if (!value.has_value()) {
      expect_code(value.error(), ct::ErrorCode::MalformedRecord, "Digest::parse_hex rejection");
      // The offending hex text is the subject; only the empty candidate has none.
      CT_CHECK_MSG(candidate.empty() || !value.error().subject().empty(),
                   "Digest::parse_hex subject for \"" + candidate + "\"");
    }
  }
  // Uppercase must be rejected everywhere, not only in the first nibble.
  std::string upper_tail = valid;
  upper_tail[62] = 'A';
  upper_tail[63] = 'B';
  auto tail = ct::Digest::parse_hex(upper_tail);
  CT_CHECK(!tail.has_value());
  if (!tail.has_value()) {
    expect_code(tail.error(), ct::ErrorCode::MalformedRecord, "uppercase tail nibble");
  }
}

CT_TEST(digest_to_hex_round_trips_through_parse_hex) {
  const std::string test_name = "digest_to_hex_round_trips_through_parse_hex";
  Rng rng(ct_test::case_seed(test_name));
  std::string payload;
  payload.reserve(513);
  for (int index = 0; index < 513; ++index) {
    payload.push_back(static_cast<char>(rng.below(256)));
  }
  const ct::Digest digest = ct::digest_bytes(payload);
  const std::string hex = digest.to_hex();
  CT_CHECK_EQ(hex.size(), static_cast<std::size_t>(64));
  for (const char character : hex) {
    const bool lowercase_hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    CT_CHECK_MSG(lowercase_hex, std::string("digest hex must be lowercase: ") + hex);
  }
  auto parsed = ct::Digest::parse_hex(hex);
  CT_REQUIRE(parsed.has_value());
  CT_CHECK(*parsed == digest);
  CT_CHECK(!digest.is_zero());
}

CT_TEST(digest_bytes_of_a_large_buffer_is_streaming_equivalent) {
  const std::string test_name = "digest_bytes_of_a_large_buffer_is_streaming_equivalent";
  Rng rng(ct_test::case_seed(test_name));
  std::string payload;
  payload.reserve(1024u * 1024u);
  while (payload.size() < 1024u * 1024u) {
    payload.push_back(static_cast<char>(rng.below(256)));
  }
  const ct::Digest one_shot = ct::digest_bytes(payload);
  ct::Sha256 hasher;
  const std::size_t chunk = 4093;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const std::size_t remaining = payload.size() - offset;
    const std::size_t take = remaining < chunk ? remaining : chunk;
    hasher.update(payload.data() + offset, take);
    offset += take;
  }
  const ct::Digest streamed(hasher.finish());
  CT_CHECK(one_shot == streamed);

  const std::string million_a(1000000, 'a');
  CT_CHECK_EQ(ct::digest_bytes(million_a).to_hex(),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  CT_CHECK(!(ct::digest_bytes(million_a) == one_shot));

  // to_hex() over raw bytes is a byte-for-byte hex rendering.
  CT_CHECK_EQ(ct::to_hex(bytes_from({0x00, 0x0F, 0x10, 0xFF})), std::string("000f10ff"));
  CT_CHECK_EQ(ct::to_hex(std::string()), std::string());
  CT_CHECK_EQ(ct::to_hex("A"), std::string("41"));
  // The zero-length message is the published empty-string vector.
  CT_CHECK_EQ(ct::digest_bytes(std::string()).to_hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}
