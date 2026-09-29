// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Proof obligations for the exact temperature unit: construction, whole-degree
// scaling, canonical text parsing and rendering, checked arithmetic at the
// representable boundary and exact ordering. No floating point appears here.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ct = dccp::cooling_topology;

void expect_code(const ct::Error& error, ct::ErrorCode expected, const std::string& what) {
  CT_CHECK_MSG(error.code() == expected,
               what + ": expected " + std::string(ct::error_code_name(expected)) + ", observed " +
                   std::string(ct::error_code_name(error.code())) + " [" + error.to_string() + "]");
}

/// Rejection case for a parser.
struct ParseReject {
  const char* text;
  ct::ErrorCode code;
};

void check_temperature_rejections(const std::vector<ParseReject>& cases) {
  for (const ParseReject& item : cases) {
    auto parsed = ct::MilliCelsius::parse_text(item.text);
    CT_CHECK_MSG(!parsed.has_value(), std::string("expected rejection of \"") + item.text + "\"");
    if (!parsed.has_value()) {
      expect_code(parsed.error(), item.code, std::string("MilliCelsius::parse_text(\"") + item.text + "\")");
    }
  }
}

}  // namespace

CT_TEST(units_create_accepts_the_whole_representable_range) {
  auto zero = ct::MilliCelsius::create(0);
  CT_REQUIRE(zero.has_value());
  CT_CHECK_EQ(zero->milli_celsius(), static_cast<std::int64_t>(0));
  CT_CHECK_EQ(zero->to_string(), std::string("0.000C"));

  auto minimum = ct::MilliCelsius::create(ct::MilliCelsius::kMinValue);
  CT_REQUIRE(minimum.has_value());
  CT_CHECK_EQ(minimum->milli_celsius(), ct::MilliCelsius::kMinValue);
  CT_CHECK_EQ(minimum->to_string(), std::string("-10000.000C"));

  auto maximum = ct::MilliCelsius::create(ct::MilliCelsius::kMaxValue);
  CT_REQUIRE(maximum.has_value());
  CT_CHECK_EQ(maximum->milli_celsius(), ct::MilliCelsius::kMaxValue);
  CT_CHECK_EQ(maximum->to_string(), std::string("10000.000C"));

  auto below = ct::MilliCelsius::create(ct::MilliCelsius::kMinValue - 1);
  CT_REQUIRE(!below.has_value());
  expect_code(below.error(), ct::ErrorCode::QuantityOutOfRange, "one below the minimum");

  auto above = ct::MilliCelsius::create(ct::MilliCelsius::kMaxValue + 1);
  CT_REQUIRE(!above.has_value());
  expect_code(above.error(), ct::ErrorCode::QuantityOutOfRange, "one above the maximum");

  auto int64_min = ct::MilliCelsius::create(INT64_MIN);
  CT_REQUIRE(!int64_min.has_value());
  expect_code(int64_min.error(), ct::ErrorCode::QuantityOutOfRange, "INT64_MIN temperature");

  auto int64_max = ct::MilliCelsius::create(INT64_MAX);
  CT_REQUIRE(!int64_max.has_value());
  expect_code(int64_max.error(), ct::ErrorCode::QuantityOutOfRange, "INT64_MAX temperature");

  CT_CHECK(ct::temperature_in_range(ct::MilliCelsius::kMinValue));
  CT_CHECK(ct::temperature_in_range(ct::MilliCelsius::kMaxValue));
  CT_CHECK(ct::temperature_in_range(0));
  CT_CHECK(!ct::temperature_in_range(ct::MilliCelsius::kMinValue - 1));
  CT_CHECK(!ct::temperature_in_range(ct::MilliCelsius::kMaxValue + 1));
}

CT_TEST(units_from_whole_celsius_scales_exactly) {
  auto seven = ct::MilliCelsius::from_whole_celsius(7);
  CT_REQUIRE(seven.has_value());
  CT_CHECK_EQ(seven->milli_celsius(), static_cast<std::int64_t>(7000));
  CT_CHECK_EQ(seven->to_string(), std::string("7.000C"));

  auto negative = ct::MilliCelsius::from_whole_celsius(-12);
  CT_REQUIRE(negative.has_value());
  CT_CHECK_EQ(negative->milli_celsius(), static_cast<std::int64_t>(-12000));
  CT_CHECK_EQ(negative->to_string(), std::string("-12.000C"));

  auto zero = ct::MilliCelsius::from_whole_celsius(0);
  CT_REQUIRE(zero.has_value());
  CT_CHECK_EQ(zero->milli_celsius(), static_cast<std::int64_t>(0));

  auto top = ct::MilliCelsius::from_whole_celsius(10000);
  CT_REQUIRE(top.has_value());
  CT_CHECK_EQ(top->milli_celsius(), ct::MilliCelsius::kMaxValue);

  auto bottom = ct::MilliCelsius::from_whole_celsius(-10000);
  CT_REQUIRE(bottom.has_value());
  CT_CHECK_EQ(bottom->milli_celsius(), ct::MilliCelsius::kMinValue);

  auto too_high = ct::MilliCelsius::from_whole_celsius(10001);
  CT_REQUIRE(!too_high.has_value());
  expect_code(too_high.error(), ct::ErrorCode::QuantityOutOfRange, "10001 whole degrees");

  auto too_low = ct::MilliCelsius::from_whole_celsius(-10001);
  CT_REQUIRE(!too_low.has_value());
  expect_code(too_low.error(), ct::ErrorCode::QuantityOutOfRange, "-10001 whole degrees");

  auto int64_max = ct::MilliCelsius::from_whole_celsius(INT64_MAX);
  CT_REQUIRE(!int64_max.has_value());
  expect_code(int64_max.error(), ct::ErrorCode::QuantityOutOfRange, "INT64_MAX whole degrees");

  auto int64_min = ct::MilliCelsius::from_whole_celsius(INT64_MIN);
  CT_REQUIRE(!int64_min.has_value());
  expect_code(int64_min.error(), ct::ErrorCode::QuantityOutOfRange, "INT64_MIN whole degrees");
}

CT_TEST(units_parse_text_accepts_only_the_canonical_shape) {
  struct Accept {
    const char* text;
    std::int64_t value;
  };
  const std::vector<Accept> accepted = {
      {"0.000", 0},
      {"7.000", 7000},
      {"-12.500", -12500},
      {"7.500", 7500},
      {"10000.000", ct::MilliCelsius::kMaxValue},
      {"-10000.000", ct::MilliCelsius::kMinValue},
      {"0.001", 1},
      {"-0.001", -1},
      {"000.001", 1},
  };
  for (const Accept& item : accepted) {
    auto parsed = ct::MilliCelsius::parse_text(item.text);
    CT_CHECK_MSG(parsed.has_value(), std::string("expected acceptance of \"") + item.text + "\"");
    if (parsed.has_value()) {
      CT_CHECK_EQ(parsed->milli_celsius(), item.value);
    }
  }
}

CT_TEST(units_parse_text_rejects_malformed_shape) {
  check_temperature_rejections({
      {"+7.000", ct::ErrorCode::MalformedNumber},
      {"7", ct::ErrorCode::MalformedNumber},
      {"7.", ct::ErrorCode::MalformedNumber},
      {"7.00", ct::ErrorCode::MalformedNumber},
      {"7.0000", ct::ErrorCode::MalformedNumber},
      {" 7.000", ct::ErrorCode::MalformedNumber},
      {"7.000 ", ct::ErrorCode::MalformedNumber},
      {"", ct::ErrorCode::MalformedNumber},
      {"-", ct::ErrorCode::MalformedNumber},
      {".000", ct::ErrorCode::MalformedNumber},
      {"7.0001", ct::ErrorCode::MalformedNumber},
      {"17.0001", ct::ErrorCode::MalformedNumber},
      {"7.-000", ct::ErrorCode::MalformedNumber},
      {"1.000.000", ct::ErrorCode::MalformedNumber},
      {"--7.000", ct::ErrorCode::MalformedNumber},
      {"7,000", ct::ErrorCode::MalformedNumber},
      {"7.000e1", ct::ErrorCode::MalformedNumber},
      {"1000000.000", ct::ErrorCode::MalformedNumber},
      {"1234567.890", ct::ErrorCode::MalformedNumber},
      {"0x7.000", ct::ErrorCode::MalformedNumber},
  });
}

CT_TEST(units_parse_text_rejects_values_outside_the_range) {
  check_temperature_rejections({
      {"10000.001", ct::ErrorCode::QuantityOutOfRange},
      {"-10000.001", ct::ErrorCode::QuantityOutOfRange},
      {"99999.999", ct::ErrorCode::QuantityOutOfRange},
      {"-99999.999", ct::ErrorCode::QuantityOutOfRange},
      {"123456.789", ct::ErrorCode::QuantityOutOfRange},
      {"-123456.789", ct::ErrorCode::QuantityOutOfRange},
  });
}

CT_TEST(units_to_string_round_trips_through_parse_text) {
  const std::vector<std::int64_t> values = {
      0, 1, -1, 7, 7000, -12500, 7500, 999999, -999999,
      ct::MilliCelsius::kMaxValue, ct::MilliCelsius::kMinValue, ct::MilliCelsius::kMaxValue - 1,
      ct::MilliCelsius::kMinValue + 1,
  };
  for (const std::int64_t value : values) {
    auto created = ct::MilliCelsius::create(value);
    CT_REQUIRE(created.has_value());
    const std::string text = created->to_string();
    CT_CHECK_MSG(!text.empty() && text.back() == 'C', "canonical form must end in C: " + text);
    // The canonical text carries the 'C' unit suffix; the numeric grammar is the
    // text without it, which is exactly what the import layer feeds back in.
    auto reparsed = ct::MilliCelsius::parse_text(std::string_view(text).substr(0, text.size() - 1u));
    CT_CHECK_MSG(reparsed.has_value(), "round trip failed for " + text);
    if (reparsed.has_value()) {
      CT_CHECK_EQ(reparsed->milli_celsius(), value);
      CT_CHECK_EQ(reparsed->to_string(), text);
    }
  }
  CT_CHECK_EQ(ct::MilliCelsius(7).to_string(), std::string("0.007C"));
  CT_CHECK_EQ(ct::MilliCelsius(-7).to_string(), std::string("-0.007C"));
  CT_CHECK_EQ(ct::MilliCelsius(70000).to_string(), std::string("70.000C"));
  CT_CHECK_EQ(ct::MilliCelsius(-12500).to_string(), std::string("-12.500C"));
}

CT_TEST(units_checked_arithmetic_never_wraps) {
  auto minimum = ct::MilliCelsius::create(ct::MilliCelsius::kMinValue).value();
  auto maximum = ct::MilliCelsius::create(ct::MilliCelsius::kMaxValue).value();
  auto zero = ct::MilliCelsius::create(0).value();
  auto one_milli = ct::MilliCelsius::create(1).value();
  auto minus_one_milli = ct::MilliCelsius::create(-1).value();

  // Exactly at the boundary: the sum of the extremes is representable.
  auto cancellation = maximum.add(minimum);
  CT_REQUIRE(cancellation.has_value());
  CT_CHECK_EQ(cancellation->milli_celsius(), static_cast<std::int64_t>(0));

  auto top_step = maximum.subtract(one_milli);
  CT_REQUIRE(top_step.has_value());
  CT_CHECK_EQ(top_step->milli_celsius(), ct::MilliCelsius::kMaxValue - 1);

  auto bottom_step = minimum.add(one_milli);
  CT_REQUIRE(bottom_step.has_value());
  CT_CHECK_EQ(bottom_step->milli_celsius(), ct::MilliCelsius::kMinValue + 1);

  auto zero_to_minimum = zero.subtract(maximum);
  CT_REQUIRE(zero_to_minimum.has_value());
  CT_CHECK_EQ(zero_to_minimum->milli_celsius(), ct::MilliCelsius::kMinValue);

  auto zero_to_maximum = zero.subtract(minimum);
  CT_REQUIRE(zero_to_maximum.has_value());
  CT_CHECK_EQ(zero_to_maximum->milli_celsius(), ct::MilliCelsius::kMaxValue);

  // One past the boundary in each direction: reported, never wrapped.
  auto high = maximum.add(one_milli);
  CT_REQUIRE(!high.has_value());
  expect_code(high.error(), ct::ErrorCode::LimitExceeded, "kMax + 1 milli");

  auto low = minimum.subtract(one_milli);
  CT_REQUIRE(!low.has_value());
  expect_code(low.error(), ct::ErrorCode::LimitExceeded, "kMin - 1 milli");

  auto doubled_high = maximum.add(maximum);
  CT_REQUIRE(!doubled_high.has_value());
  expect_code(doubled_high.error(), ct::ErrorCode::LimitExceeded, "kMax + kMax");

  auto doubled_low = minimum.add(minimum);
  CT_REQUIRE(!doubled_low.has_value());
  expect_code(doubled_low.error(), ct::ErrorCode::LimitExceeded, "kMin + kMin");

  auto span = maximum.subtract(minimum);
  CT_REQUIRE(!span.has_value());
  expect_code(span.error(), ct::ErrorCode::LimitExceeded, "kMax - kMin spans twice the range");

  auto backwards = minimum.subtract(maximum);
  CT_REQUIRE(!backwards.has_value());
  expect_code(backwards.error(), ct::ErrorCode::LimitExceeded, "kMin - kMax");

  auto high_negative = maximum.add(minus_one_milli);
  CT_REQUIRE(high_negative.has_value());
  CT_CHECK_EQ(high_negative->milli_celsius(), ct::MilliCelsius::kMaxValue - 1);

  auto low_positive = minimum.subtract(minus_one_milli);
  CT_REQUIRE(low_positive.has_value());
  CT_CHECK_EQ(low_positive->milli_celsius(), ct::MilliCelsius::kMinValue + 1);

  // Failure is a value-free Result: nothing to read back as a wrapped sum.
  CT_CHECK(!high.has_value());
  CT_CHECK(!low.has_value());
  CT_CHECK(!high.error().ok());
}

CT_TEST(units_ordering_and_equality_are_exact) {
  auto minus_one = ct::MilliCelsius::create(-1).value();
  auto zero = ct::MilliCelsius::create(0).value();
  auto plus_one = ct::MilliCelsius::create(1).value();
  auto minimum = ct::MilliCelsius::create(ct::MilliCelsius::kMinValue).value();
  auto maximum = ct::MilliCelsius::create(ct::MilliCelsius::kMaxValue).value();

  CT_CHECK(minus_one < zero);
  CT_CHECK(zero < plus_one);
  CT_CHECK(minimum < maximum);
  CT_CHECK(minus_one != zero);
  CT_CHECK(minus_one != plus_one);
  CT_CHECK(!(zero < zero));
  CT_CHECK(zero == ct::MilliCelsius::create(0).value());
  CT_CHECK(ct::MilliCelsius(7500) == ct::MilliCelsius(7500));
  CT_CHECK(ct::MilliCelsius(7500) != ct::MilliCelsius(7501));
  CT_CHECK(ct::MilliCelsius(7500) < ct::MilliCelsius(7501));
  CT_CHECK(ct::MilliCelsius(-7501) < ct::MilliCelsius(-7500));
  CT_CHECK(ct::MilliCelsius(ct::MilliCelsius::kMaxValue) > ct::MilliCelsius(ct::MilliCelsius::kMinValue));
  CT_CHECK_EQ(ct::MilliCelsius().milli_celsius(), static_cast<std::int64_t>(0));
}
