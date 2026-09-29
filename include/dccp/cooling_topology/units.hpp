// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_TOPOLOGY_UNITS_HPP
#define DCCP_COOLING_TOPOLOGY_UNITS_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/result.hpp"

namespace dccp::cooling_topology {

/// Exact integer temperature: unit 10^-3 degrees Celsius (millidegrees).
///
/// Physical quantities in this library are exact integers with an explicit unit
/// and checked arithmetic. No floating-point value appears in any authoritative
/// comparison, canonical encoding or durable record, so equality and ordering
/// are exact and platform independent.
///
/// A temperature here is a *declared design* attribute of a structural element
/// (for example the design supply temperature of a loop or the highest design
/// supply temperature a consumer declares it accepts). It is not a measurement,
/// not an observation and not a claim that any medium is at that temperature.
class MilliCelsius {
 public:
  static constexpr std::int64_t kScale = limits::kMilliCelsiusPerCelsius;
  static constexpr std::int64_t kMinValue = limits::kMinTemperatureMilliCelsius;
  static constexpr std::int64_t kMaxValue = limits::kMaxTemperatureMilliCelsius;

  constexpr MilliCelsius() noexcept = default;
  explicit constexpr MilliCelsius(std::int64_t milli_celsius) noexcept : value_(milli_celsius) {}

  /// Rejects a value outside the representable range.
  static Result<MilliCelsius> create(std::int64_t milli_celsius);

  /// Parses a whole number of degrees Celsius; the multiplication is checked.
  static Result<MilliCelsius> from_whole_celsius(std::int64_t celsius);

  /// Parses canonical decimal text: optional sign, 1..6 integer digits, optional
  /// '.' followed by exactly three fraction digits. Anything else is rejected.
  static Result<MilliCelsius> parse_text(std::string_view text);

  /// Canonical text form, e.g. "7.500C", "-12.250C", "0.000C".
  std::string to_string() const;

  constexpr std::int64_t milli_celsius() const noexcept { return value_; }

  /// Checked addition inside the representable range; overflow is reported.
  Result<MilliCelsius> add(const MilliCelsius& other) const;
  /// Checked subtraction inside the representable range; overflow is reported.
  Result<MilliCelsius> subtract(const MilliCelsius& other) const;

  friend constexpr bool operator==(const MilliCelsius&, const MilliCelsius&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const MilliCelsius& lhs,
                                                    const MilliCelsius& rhs) noexcept = default;

 private:
  std::int64_t value_ = 0;
};

/// True when the temperature lies inside the bounded representable range.
bool temperature_in_range(std::int64_t milli_celsius) noexcept;

}  // namespace dccp::cooling_topology

#endif  // DCCP_COOLING_TOPOLOGY_UNITS_HPP
