// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/units.hpp"

#include <limits>

namespace dccp::cooling_topology {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

// Reads exactly count ASCII decimal digits starting at index. The callers have
// already established that the range lies inside text.
bool read_digits(std::string_view text, std::size_t index, std::size_t count, std::int64_t& value) noexcept {
  std::int64_t out = 0;
  for (std::size_t offset = 0; offset < count; ++offset) {
    const char character = text[index + offset];
    if (character < '0' || character > '9') {
      return false;
    }
    out = out * 10 + static_cast<std::int64_t>(character - '0');
  }
  value = out;
  return true;
}

// Text that does not have the canonical shape is a shape error; the offending
// text is bounded so that an untrusted record cannot inflate the error value.
Error malformed_temperature(std::string_view text) {
  return Error(ErrorCode::MalformedNumber,
               "temperature text must be an optional '-' followed by 1..6 digits, '.', exactly three digits")
      .with_subject(std::string(text.substr(0, 64)));
}

// A well-formed value that cannot be a representable design temperature is a
// quantity error, never a shape error.
Error temperature_out_of_range(std::int64_t milli_celsius) {
  return Error(ErrorCode::QuantityOutOfRange,
               "declared temperature is outside the representable range [-10000.000C, 10000.000C]")
      .with_subject(std::to_string(milli_celsius));
}

Error temperature_limit(std::int64_t receiver, std::int64_t operand, const char* explanation) {
  return Error(ErrorCode::LimitExceeded, explanation)
      .with_subject(std::to_string(receiver))
      .with_detail(std::string("operand=") + std::to_string(operand));
}

}  // namespace

bool temperature_in_range(std::int64_t milli_celsius) noexcept {
  return milli_celsius >= MilliCelsius::kMinValue && milli_celsius <= MilliCelsius::kMaxValue;
}

Result<MilliCelsius> MilliCelsius::create(std::int64_t milli_celsius) {
  if (!temperature_in_range(milli_celsius)) {
    return temperature_out_of_range(milli_celsius);
  }
  return MilliCelsius(milli_celsius);
}

Result<MilliCelsius> MilliCelsius::from_whole_celsius(std::int64_t celsius) {
  // The whole-degree value is bounded before scaling, so the multiplication can
  // never overflow and an unrepresentable temperature is rejected rather than
  // wrapped.
  if (celsius < kMinValue / kScale || celsius > kMaxValue / kScale) {
    return Error(ErrorCode::QuantityOutOfRange,
                 "whole-degree temperature is outside the representable range [-10000C, 10000C]")
        .with_subject(std::to_string(celsius));
  }
  return MilliCelsius(celsius * kScale);
}

Result<MilliCelsius> MilliCelsius::parse_text(std::string_view text) {
  if (text.empty()) {
    return malformed_temperature(text);
  }
  std::size_t index = 0;
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    index = 1;
  }

  std::size_t integer_digits = 0;
  while (index + integer_digits < text.size() && text[index + integer_digits] >= '0' &&
         text[index + integer_digits] <= '9') {
    ++integer_digits;
  }
  // The canonical form is [-]IIIIII.FFF: 1..6 integer digits, one '.', exactly
  // three fraction digits and nothing else. Leading zeros satisfy the shape; a
  // magnitude they cannot disguise is rejected by the range check below.
  if (integer_digits < 1 || integer_digits > 6) {
    return malformed_temperature(text);
  }
  const std::size_t fraction_start = index + integer_digits;
  if (fraction_start + 4 != text.size() || text[fraction_start] != '.') {
    return malformed_temperature(text);
  }

  std::int64_t whole = 0;
  std::int64_t fraction = 0;
  if (!read_digits(text, index, integer_digits, whole) || !read_digits(text, fraction_start + 1, 3, fraction)) {
    return malformed_temperature(text);
  }
  // Six integer digits and three fraction digits keep the scaled magnitude far
  // below the 64-bit domain, so this multiplication cannot overflow.
  const std::int64_t magnitude = whole * kScale + fraction;
  const std::int64_t milli_celsius = negative ? -magnitude : magnitude;
  if (!temperature_in_range(milli_celsius)) {
    return temperature_out_of_range(milli_celsius);
  }
  return MilliCelsius(milli_celsius);
}

std::string MilliCelsius::to_string() const {
  const bool negative = value_ < 0;
  // The magnitude is formed in unsigned arithmetic so that INT64_MIN renders
  // without an overflowing negation.
  const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value_)
                                           : static_cast<std::uint64_t>(value_);
  const auto scale = static_cast<std::uint64_t>(kScale);
  const std::uint64_t whole = magnitude / scale;
  const std::uint64_t fraction = magnitude % scale;

  std::string out;
  out.reserve(24);
  if (negative) {
    out.push_back('-');
  }
  out.append(std::to_string(whole));
  out.push_back('.');
  const std::string digits = std::to_string(fraction);
  for (std::size_t pad = digits.size(); pad < 3; ++pad) {
    out.push_back('0');
  }
  out.append(digits);
  out.push_back('C');
  return out;
}

Result<MilliCelsius> MilliCelsius::add(const MilliCelsius& other) const {
  // The exact sum is computed only after the addition is known not to overflow
  // the 64-bit domain, and is then checked against the declared range: no
  // intermediate value wraps.
  if (other.value_ > 0 && value_ > kInt64Max - other.value_) {
    return temperature_limit(value_, other.value_, "temperature addition exceeds the 64-bit domain");
  }
  if (other.value_ < 0 && value_ < kInt64Min - other.value_) {
    return temperature_limit(value_, other.value_, "temperature addition underflows the 64-bit domain");
  }
  const std::int64_t sum = value_ + other.value_;
  if (!temperature_in_range(sum)) {
    return temperature_limit(value_, other.value_, "temperature sum is outside the representable range");
  }
  return MilliCelsius(sum);
}

Result<MilliCelsius> MilliCelsius::subtract(const MilliCelsius& other) const {
  // The exact difference is computed only after the subtraction is known not to
  // overflow the 64-bit domain, and is then checked against the declared range.
  if (other.value_ > 0 && value_ < kInt64Min + other.value_) {
    return temperature_limit(value_, other.value_, "temperature subtraction underflows the 64-bit domain");
  }
  if (other.value_ < 0 && value_ > kInt64Max + other.value_) {
    return temperature_limit(value_, other.value_, "temperature subtraction exceeds the 64-bit domain");
  }
  const std::int64_t difference = value_ - other.value_;
  if (!temperature_in_range(difference)) {
    return temperature_limit(value_, other.value_, "temperature difference is outside the representable range");
  }
  return MilliCelsius(difference);
}

}  // namespace dccp::cooling_topology
