// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/canonical.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "dccp/cooling_topology/limits.hpp"
#include "dccp/cooling_topology/text.hpp"

namespace dccp::cooling_topology {
namespace {

constexpr std::size_t kFrameHeaderBytes = 8 + 2 + 2 + 8;

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

class Writer {
 public:
  void u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }
  void u16(std::uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void bytes(std::string_view value) { out_.append(value.data(), value.size()); }
  void string(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(value);
  }
  void digest(const Digest& value) {
    bytes(std::string_view(reinterpret_cast<const char*>(value.bytes().data()), Digest::kBytes));
  }
  template <class T, class Fn>
  void optional(const std::optional<T>& value, Fn write) {
    if (value.has_value()) {
      u8(1);
      write(*value);
    } else {
      u8(0);
    }
  }
  void ref(const ExternalRef& value) {
    u8(static_cast<std::uint8_t>(value.kind));
    string(value.identity);
    u64(value.generation.value());
  }
  std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

void encode_temperature(Writer& writer, const std::optional<MilliCelsius>& value) {
  writer.optional(value, [&writer](const MilliCelsius& temperature) {
    writer.u64(static_cast<std::uint64_t>(temperature.milli_celsius()));
  });
}

void encode_medium(Writer& writer, const std::optional<CoolingMedium>& value) {
  writer.optional(value, [&writer](CoolingMedium medium) { writer.u8(static_cast<std::uint8_t>(medium)); });
}

void encode_hydronic(Writer& writer, std::uint8_t kind, const std::optional<CoolingMedium>& medium,
                     const std::optional<MilliCelsius>& design, const std::optional<MilliCelsius>& maximum) {
  writer.u8(kind);
  encode_medium(writer, medium);
  encode_temperature(writer, design);
  encode_temperature(writer, maximum);
}

void encode_attributes(Writer& writer, const NodeAttributes& attributes) {
  writer.u8(static_cast<std::uint8_t>(attributes.index()));
  switch (attributes.index()) {
    case 0: {
      const auto& value = std::get<CoolingPlantAttributes>(attributes);
      encode_hydronic(writer, static_cast<std::uint8_t>(value.kind), value.medium,
                      value.design_supply_temperature, value.max_acceptable_supply_temperature);
      break;
    }
    case 1: {
      const auto& value = std::get<ChillerAttributes>(attributes);
      encode_hydronic(writer, static_cast<std::uint8_t>(value.kind), value.medium,
                      value.design_supply_temperature, value.max_acceptable_supply_temperature);
      break;
    }
    case 2: {
      const auto& value = std::get<PumpAttributes>(attributes);
      writer.u8(static_cast<std::uint8_t>(value.kind));
      writer.u8(static_cast<std::uint8_t>(value.role));
      break;
    }
    case 3: {
      const auto& value = std::get<CoolingLoopAttributes>(attributes);
      encode_hydronic(writer, static_cast<std::uint8_t>(value.kind), value.medium,
                      value.design_supply_temperature, value.max_acceptable_supply_temperature);
      break;
    }
    case 4: {
      const auto& value = std::get<CduAttributes>(attributes);
      encode_hydronic(writer, static_cast<std::uint8_t>(value.kind), value.medium,
                      value.design_supply_temperature, value.max_acceptable_supply_temperature);
      break;
    }
    case 5: {
      const auto& value = std::get<CrahAttributes>(attributes);
      writer.u8(static_cast<std::uint8_t>(value.placement));
      encode_medium(writer, value.medium);
      encode_temperature(writer, value.max_acceptable_supply_temperature);
      break;
    }
    case 6: {
      const auto& value = std::get<CracAttributes>(attributes);
      writer.u8(static_cast<std::uint8_t>(value.placement));
      encode_medium(writer, value.medium);
      encode_temperature(writer, value.max_acceptable_supply_temperature);
      break;
    }
    case 7: {
      const auto& value = std::get<ManifoldAttributes>(attributes);
      encode_hydronic(writer, static_cast<std::uint8_t>(value.kind), value.medium,
                      value.design_supply_temperature, value.max_acceptable_supply_temperature);
      break;
    }
    case 8: {
      const auto& value = std::get<BranchAttributes>(attributes);
      encode_hydronic(writer, static_cast<std::uint8_t>(value.kind), value.medium,
                      value.design_supply_temperature, value.max_acceptable_supply_temperature);
      break;
    }
    case 9: {
      const auto& value = std::get<ThermalZoneAttributes>(attributes);
      writer.u8(static_cast<std::uint8_t>(value.zone_class));
      break;
    }
    case 10: {
      const auto& value = std::get<CoolingSinkAttributes>(attributes);
      writer.u8(static_cast<std::uint8_t>(value.kind));
      writer.ref(value.consumer);
      encode_temperature(writer, value.max_acceptable_supply_temperature);
      break;
    }
    case 11: {
      const auto& value = std::get<CoolingSourceAttributes>(attributes);
      writer.u8(static_cast<std::uint8_t>(value.kind));
      encode_medium(writer, value.medium);
      encode_temperature(writer, value.design_supply_temperature);
      break;
    }
    default:
      break;
  }
}

void encode_endpoint(Writer& writer, const Endpoint& endpoint) {
  writer.string(endpoint.node.value());
  writer.u8(static_cast<std::uint8_t>(endpoint.port));
}

void encode_binding(Writer& writer, const EvidenceBinding& binding) {
  writer.u8(static_cast<std::uint8_t>(binding.kind));
  writer.string(binding.producer);
  writer.ref(binding.subject);
  writer.u64(binding.generation.value());
  writer.u64(binding.observation.value());
}

void encode_provenance(Writer& writer, const Provenance& provenance) {
  writer.string(provenance.producer);
  writer.u8(static_cast<std::uint8_t>(provenance.origin));
  writer.string(provenance.witness);
  writer.optional(provenance.source_reference, [&writer](const ExternalRef& value) { writer.ref(value); });
  writer.u64(provenance.authority_epoch.value());
  writer.u32(static_cast<std::uint32_t>(provenance.evidence.size()));
  for (const EvidenceBinding& binding : provenance.evidence) {
    encode_binding(writer, binding);
  }
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

class Reader {
 public:
  Reader(std::string_view bytes, std::string_view what) : bytes_(bytes), what_(what) {}

  std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  std::size_t position() const noexcept { return position_; }

  Result<std::uint8_t> u8() {
    if (remaining() < 1) {
      return truncated("u8");
    }
    return static_cast<std::uint8_t>(bytes_[position_++]);
  }
  Result<std::uint16_t> u16() {
    if (remaining() < 2) {
      return truncated("u16");
    }
    const std::uint16_t value =
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(static_cast<unsigned char>(bytes_[position_])) |
                                   static_cast<std::uint16_t>(
                                       static_cast<unsigned char>(bytes_[position_ + 1]) << 8));
    position_ += 2;
    return value;
  }
  Result<std::uint32_t> u32() {
    if (remaining() < 4) {
      return truncated("u32");
    }
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index) {
      value = (value << 8) | static_cast<unsigned char>(bytes_[position_ + static_cast<std::size_t>(index)]);
    }
    position_ += 4;
    return value;
  }
  Result<std::uint64_t> u64() {
    if (remaining() < 8) {
      return truncated("u64");
    }
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
      value = (value << 8) | static_cast<unsigned char>(bytes_[position_ + static_cast<std::size_t>(index)]);
    }
    position_ += 8;
    return value;
  }
  Result<std::string> string() {
    CT_TRY(length, u32());
    if (length > limits::kMaxCanonicalStringBytes) {
      return Error(ErrorCode::LimitExceeded, std::string(what_) + ": string length exceeds the documented bound");
    }
    if (length > remaining()) {
      return truncated("string payload");
    }
    std::string value(bytes_.substr(position_, length));
    position_ += length;
    if (value.find('\0') != std::string::npos) {
      return Error(ErrorCode::MalformedRecord, std::string(what_) + ": string contains an embedded NUL");
    }
    return value;
  }
  Result<Digest> digest() {
    if (remaining() < Digest::kBytes) {
      return truncated("digest");
    }
    std::array<std::uint8_t, Digest::kBytes> bytes{};
    std::memcpy(bytes.data(), bytes_.data() + position_, Digest::kBytes);
    position_ += Digest::kBytes;
    return Digest(bytes);
  }
  /// Reads a collection count and checks it against both the documented bound
  /// and the number of bytes that remain, so a declared count can never drive an
  /// allocation larger than the input could possibly describe.
  Result<std::uint32_t> count(std::size_t limit, std::string_view what) {
    CT_TRY(value, u32());
    if (value > limit) {
      return Error(ErrorCode::LimitExceeded,
                   std::string(what_) + ": " + std::string(what) + " count exceeds the documented bound");
    }
    if (value > remaining()) {
      return Error(ErrorCode::CountMismatch,
                   std::string(what_) + ": " + std::string(what) +
                       " count exceeds the remaining input length");
    }
    return value;
  }
  Result<bool> presence(std::string_view what) {
    CT_TRY(value, u8());
    if (value > 1) {
      return Error(ErrorCode::MalformedRecord,
                   std::string(what_) + ": presence byte for " + std::string(what) + " is not 0 or 1");
    }
    return value == 1;
  }
  /// Advances past a run of bytes that has already been length-checked by the
  /// caller.
  void skip(std::size_t count) noexcept { position_ += count; }
  bool at_end() const noexcept { return position_ == bytes_.size(); }

  Error truncated(std::string_view what) const {
    return Error(ErrorCode::TruncatedInput, std::string(what_) + ": input ends before " + std::string(what));
  }
  Error malformed(std::string_view message) const {
    return Error(ErrorCode::MalformedRecord, std::string(what_) + ": " + std::string(message));
  }

 private:
  std::string_view bytes_;
  std::string_view what_;
  std::size_t position_ = 0;
};

template <class Enum>
Result<Enum> read_enum(std::uint8_t raw, std::uint8_t maximum, std::string_view what) {
  if (raw > maximum) {
    return Error(ErrorCode::UnknownEnumToken,
                 std::string(what) + " is not a declared enumerator value")
        .with_subject(std::to_string(raw));
  }
  return static_cast<Enum>(raw);
}

Result<std::string> read_identifier(Reader& reader, std::string_view what) {
  CT_TRY(value, reader.string());
  if (!is_valid_identifier_syntax(value)) {
    return Error(ErrorCode::MalformedIdentifier, std::string(what) + " does not match the identifier grammar");
  }
  return value;
}

Result<std::string> read_display_text(Reader& reader, std::string_view what, std::size_t max_bytes) {
  CT_TRY(value, reader.string());
  if (!value.empty() && !is_valid_display_text(value, max_bytes)) {
    return Error(ErrorCode::InvalidUtf8, std::string(what) + " must be valid UTF-8 display text");
  }
  return value;
}

Result<ExternalRef> read_ref(Reader& reader, std::string_view what) {
  CT_TRY(kind_byte, reader.u8());
  CT_TRY(kind, read_enum<ExternalRefKind>(kind_byte, static_cast<std::uint8_t>(ExternalRefKind::Evidence), what));
  CT_TRY(identity, reader.string());
  if (identity.empty() || !is_valid_external_identity(identity, limits::kMaxExternalIdentityBytes)) {
    return Error(ErrorCode::InvalidUtf8, std::string(what) + " must be a valid non-empty external identity");
  }
  CT_TRY(generation, reader.u64());
  return ExternalRef::create(kind, std::move(identity), ExternalGeneration(generation));
}

Result<EvidenceBinding> read_binding(Reader& reader) {
  CT_TRY(kind_byte, reader.u8());
  CT_TRY(kind, read_enum<EvidenceKind>(kind_byte, static_cast<std::uint8_t>(EvidenceKind::PowerStateObservation),
                                       "evidence kind"));
  CT_TRY(producer, read_display_text(reader, "evidence producer", limits::kMaxProducerBytes));
  CT_TRY(subject, read_ref(reader, "evidence subject"));
  CT_TRY(generation, reader.u64());
  CT_TRY(observation, reader.u64());
  return EvidenceBinding::create(kind, std::move(producer), subject, EvidenceGeneration(generation),
                                 ObservationSequence(observation));
}

Result<Provenance> read_provenance(Reader& reader) {
  CT_TRY(producer, read_display_text(reader, "provenance producer", limits::kMaxProducerBytes));
  CT_TRY(origin_byte, reader.u8());
  CT_TRY(origin, read_enum<ProvenanceOrigin>(origin_byte, static_cast<std::uint8_t>(ProvenanceOrigin::Recovered),
                                             "provenance origin"));
  CT_TRY(witness, read_display_text(reader, "provenance witness", limits::kMaxWitnessBytes));
  CT_TRY(has_source, reader.presence("provenance source reference"));
  std::optional<ExternalRef> source;
  if (has_source) {
    CT_TRY(value, read_ref(reader, "provenance source reference"));
    source = std::move(value);
  }
  CT_TRY(epoch, reader.u64());
  CT_TRY(count, reader.count(limits::kMaxEvidenceBindings, "evidence binding"));
  std::vector<EvidenceBinding> evidence;
  evidence.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    CT_TRY(binding, read_binding(reader));
    evidence.push_back(std::move(binding));
  }
  return Provenance::create(std::move(producer), origin, std::move(witness), std::move(source),
                            AuthorityEpoch(epoch), std::move(evidence));
}

Result<std::optional<MilliCelsius>> read_temperature(Reader& reader, std::string_view what) {
  CT_TRY(present, reader.presence(what));
  if (!present) {
    return std::optional<MilliCelsius>();
  }
  CT_TRY(raw, reader.u64());
  if (raw > static_cast<std::uint64_t>(INT64_MAX)) {
    return Error(ErrorCode::QuantityOutOfRange,
                 std::string(what) + " is outside the representable design-temperature range");
  }
  const auto signed_value = static_cast<std::int64_t>(raw);
  if (!temperature_in_range(signed_value)) {
    return Error(ErrorCode::QuantityOutOfRange,
                 std::string(what) + " is outside the representable design-temperature range");
  }
  return std::optional<MilliCelsius>(MilliCelsius(signed_value));
}

Result<std::optional<CoolingMedium>> read_medium(Reader& reader, std::string_view what) {
  CT_TRY(present, reader.presence(what));
  if (!present) {
    return std::optional<CoolingMedium>();
  }
  CT_TRY(raw, reader.u8());
  CT_TRY(value, read_enum<CoolingMedium>(raw, static_cast<std::uint8_t>(CoolingMedium::Air), what));
  return std::optional<CoolingMedium>(value);
}

Result<NodeAttributes> read_attributes(Reader& reader) {
  CT_TRY(tag, reader.u8());
  if (tag > 11) {
    return Error(ErrorCode::UnknownEnumToken, "node attribute variant tag is not a declared node kind")
        .with_subject(std::to_string(tag));
  }
  switch (tag) {
    case 0:
    case 1:
    case 3:
    case 4:
    case 7:
    case 8: {
      CT_TRY(kind_raw, reader.u8());
      CT_TRY(medium, read_medium(reader, "declared medium"));
      CT_TRY(design, read_temperature(reader, "design supply temperature"));
      CT_TRY(maximum, read_temperature(reader, "maximum acceptable supply temperature"));
      switch (tag) {
        case 0: {
          CT_TRY(kind, read_enum<PlantKind>(kind_raw,
                                            static_cast<std::uint8_t>(PlantKind::DistrictSupply), "plant kind"));
          CoolingPlantAttributes value;
          value.kind = kind;
          value.medium = medium;
          value.design_supply_temperature = design;
          value.max_acceptable_supply_temperature = maximum;
          return NodeAttributes(value);
        }
        case 1: {
          CT_TRY(kind, read_enum<ChillerKind>(kind_raw,
                                              static_cast<std::uint8_t>(ChillerKind::MagneticBearing),
                                              "chiller kind"));
          ChillerAttributes value;
          value.kind = kind;
          value.medium = medium;
          value.design_supply_temperature = design;
          value.max_acceptable_supply_temperature = maximum;
          return NodeAttributes(value);
        }
        case 3: {
          CT_TRY(kind, read_enum<LoopKind>(kind_raw,
                                           static_cast<std::uint8_t>(LoopKind::HeatRejection), "loop kind"));
          CoolingLoopAttributes value;
          value.kind = kind;
          value.medium = medium;
          value.design_supply_temperature = design;
          value.max_acceptable_supply_temperature = maximum;
          return NodeAttributes(value);
        }
        case 4: {
          CT_TRY(kind, read_enum<CduKind>(kind_raw, static_cast<std::uint8_t>(CduKind::InRackCdu),
                                          "cdu kind"));
          CduAttributes value;
          value.kind = kind;
          value.medium = medium;
          value.design_supply_temperature = design;
          value.max_acceptable_supply_temperature = maximum;
          return NodeAttributes(value);
        }
        case 7: {
          CT_TRY(kind, read_enum<ManifoldKind>(kind_raw,
                                               static_cast<std::uint8_t>(ManifoldKind::Combined),
                                               "manifold kind"));
          ManifoldAttributes value;
          value.kind = kind;
          value.medium = medium;
          value.design_supply_temperature = design;
          value.max_acceptable_supply_temperature = maximum;
          return NodeAttributes(value);
        }
        default: {
          CT_TRY(kind, read_enum<BranchKind>(kind_raw,
                                             static_cast<std::uint8_t>(BranchKind::EquipmentBranch),
                                             "branch kind"));
          BranchAttributes value;
          value.kind = kind;
          value.medium = medium;
          value.design_supply_temperature = design;
          value.max_acceptable_supply_temperature = maximum;
          return NodeAttributes(value);
        }
      }
    }
    case 2: {
      CT_TRY(kind_raw, reader.u8());
      CT_TRY(kind, read_enum<PumpKind>(kind_raw,
                                       static_cast<std::uint8_t>(PumpKind::PositiveDisplacement), "pump kind"));
      CT_TRY(role_raw, reader.u8());
      CT_TRY(role, read_enum<PumpRole>(role_raw, static_cast<std::uint8_t>(PumpRole::Jockey),
                                       "pump role"));
      PumpAttributes value;
      value.kind = kind;
      value.role = role;
      return NodeAttributes(value);
    }
    case 5:
    case 6: {
      CT_TRY(placement_raw, reader.u8());
      CT_TRY(placement, read_enum<AirHandlerPlacement>(placement_raw,
                                                       static_cast<std::uint8_t>(AirHandlerPlacement::RearDoor),
                                                       "air handler placement"));
      CT_TRY(medium, read_medium(reader, "declared medium"));
      CT_TRY(maximum, read_temperature(reader, "maximum acceptable supply temperature"));
      if (tag == 5) {
        CrahAttributes value;
        value.placement = placement;
        value.medium = medium;
        value.max_acceptable_supply_temperature = maximum;
        return NodeAttributes(value);
      }
      CracAttributes value;
      value.placement = placement;
      value.medium = medium;
      value.max_acceptable_supply_temperature = maximum;
      return NodeAttributes(value);
    }
    case 9: {
      CT_TRY(class_raw, reader.u8());
      CT_TRY(zone_class, read_enum<ZoneClass>(class_raw, static_cast<std::uint8_t>(ZoneClass::Plenum),
                                              "zone class"));
      ThermalZoneAttributes value;
      value.zone_class = zone_class;
      return NodeAttributes(value);
    }
    case 10: {
      CT_TRY(kind_raw, reader.u8());
      CT_TRY(kind, read_enum<SinkKind>(kind_raw, static_cast<std::uint8_t>(SinkKind::FacilityLoad),
                                       "sink kind"));
      CT_TRY(consumer, read_ref(reader, "sink consumer reference"));
      CT_TRY(maximum, read_temperature(reader, "maximum acceptable supply temperature"));
      CoolingSinkAttributes value;
      value.kind = kind;
      value.consumer = std::move(consumer);
      value.max_acceptable_supply_temperature = maximum;
      return NodeAttributes(value);
    }
    default: {
      CT_TRY(kind_raw, reader.u8());
      CT_TRY(kind, read_enum<SourceKind>(kind_raw, static_cast<std::uint8_t>(SourceKind::ThermalStore),
                                         "source kind"));
      CT_TRY(medium, read_medium(reader, "declared medium"));
      CT_TRY(design, read_temperature(reader, "design supply temperature"));
      CoolingSourceAttributes value;
      value.kind = kind;
      value.medium = medium;
      value.design_supply_temperature = design;
      return NodeAttributes(value);
    }
  }
}

Result<Node> read_node(Reader& reader) {
  CT_TRY(id, read_identifier(reader, "node identity"));
  CT_TRY(attributes, read_attributes(reader));
  CT_TRY(display_name, read_display_text(reader, "node display name", limits::kMaxDisplayNameBytes));
  CT_TRY(reference_count, reader.count(limits::kMaxNodeReferences, "node reference"));
  std::vector<ExternalRef> references;
  references.reserve(reference_count);
  for (std::uint32_t index = 0; index < reference_count; ++index) {
    CT_TRY(reference, read_ref(reader, "node reference"));
    references.push_back(std::move(reference));
  }
  CT_TRY(node_id, NodeId::parse(id));
  return Node::create(std::move(node_id), std::move(attributes), std::move(display_name),
                      std::move(references));
}

Result<Endpoint> read_endpoint(Reader& reader) {
  CT_TRY(node, read_identifier(reader, "endpoint node identity"));
  CT_TRY(port_raw, reader.u8());
  CT_TRY(port, read_enum<PortRole>(port_raw, static_cast<std::uint8_t>(PortRole::Served), "port role"));
  CT_TRY(id, NodeId::parse(node));
  Endpoint endpoint;
  endpoint.node = std::move(id);
  endpoint.port = port;
  return endpoint;
}

Result<Edge> read_edge(Reader& reader) {
  CT_TRY(id_text, read_identifier(reader, "edge identity"));
  CT_TRY(kind_raw, reader.u8());
  CT_TRY(kind, read_enum<EdgeKind>(kind_raw, static_cast<std::uint8_t>(EdgeKind::DependsOn), "edge kind"));
  CT_TRY(from, read_endpoint(reader));
  CT_TRY(to, read_endpoint(reader));
  CT_TRY(id, EdgeId::parse(id_text));
  return Edge::create(std::move(id), kind, std::move(from), std::move(to));
}

Result<RedundancyGroup> read_group(Reader& reader) {
  CT_TRY(id_text, read_identifier(reader, "redundancy group identity"));
  CT_TRY(scheme_raw, reader.u8());
  CT_TRY(scheme, read_enum<RedundancyScheme>(scheme_raw,
                                             static_cast<std::uint8_t>(RedundancyScheme::ConcurrentlyMaintainable),
                                             "redundancy scheme"));
  CT_TRY(scope_raw, reader.u8());
  CT_TRY(scope, read_enum<RedundancyScope>(scope_raw,
                                           static_cast<std::uint8_t>(RedundancyScope::AirHandling),
                                           "redundancy scope"));
  CT_TRY(display_name, read_display_text(reader, "redundancy group display name", limits::kMaxDisplayNameBytes));
  CT_TRY(basis, read_display_text(reader, "redundancy group basis", limits::kMaxBasisBytes));
  CT_TRY(member_count, reader.count(limits::kMaxGroupMemberCount, "redundancy member"));
  std::vector<RedundancyMember> members;
  members.reserve(member_count);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    CT_TRY(node_text, read_identifier(reader, "redundancy member node identity"));
    CT_TRY(declared, read_display_text(reader, "redundancy member spelling", limits::kMaxIdentifierBytes));
    CT_TRY(has_domain, reader.presence("redundancy member failure domain"));
    RedundancyMember member;
    CT_TRY(node, NodeId::parse(node_text));
    member.node = std::move(node);
    member.declared = std::move(declared);
    if (has_domain) {
      CT_TRY(domain, read_ref(reader, "redundancy member failure domain"));
      member.failure_domain = std::move(domain);
    }
    members.push_back(std::move(member));
  }
  CT_TRY(flags, reader.u8());
  if ((flags & 0xFCu) != 0) {
    return reader.malformed("redundancy group reserved flag bits are not zero");
  }
  CT_TRY(id, RedundancyGroupId::parse(id_text));
  return RedundancyGroup::create(std::move(id), scheme, scope, std::move(display_name), std::move(basis),
                                 std::move(members), (flags & 0x01u) != 0, (flags & 0x02u) != 0);
}

Result<Alias> read_alias(Reader& reader) {
  CT_TRY(id_text, read_identifier(reader, "alias identity"));
  CT_TRY(target_text, read_identifier(reader, "alias target"));
  CT_TRY(id, AliasId::parse(id_text));
  CT_TRY(target, NodeId::parse(target_text));
  Alias alias;
  alias.id = std::move(id);
  alias.target = std::move(target);
  return alias;
}

Result<ChangeoverGroup> read_changeover(Reader& reader) {
  CT_TRY(id_text, read_identifier(reader, "changeover group identity"));
  CT_TRY(display_name, read_display_text(reader, "changeover group display name", limits::kMaxDisplayNameBytes));
  CT_TRY(member_count, reader.count(limits::kMaxChangeoverMemberCount, "changeover member"));
  std::vector<Endpoint> members;
  members.reserve(member_count);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    CT_TRY(member, read_endpoint(reader));
    members.push_back(std::move(member));
  }
  CT_TRY(max_concurrent, reader.u32());
  CT_TRY(id, ChangeoverGroupId::parse(id_text));
  return ChangeoverGroup::create(std::move(id), std::move(display_name), std::move(members), max_concurrent);
}

}  // namespace

// ---------------------------------------------------------------------------
// canonical_order
// ---------------------------------------------------------------------------

Result<CanonicalTables> canonical_order(const TopologyDraft& draft) {
  if (draft.nodes.size() > limits::kMaxNodeCount || draft.edges.size() > limits::kMaxEdgeCount ||
      draft.groups.size() > limits::kMaxRedundancyGroupCount || draft.aliases.size() > limits::kMaxAliasCount ||
      draft.changeovers.size() > limits::kMaxChangeoverGroupCount) {
    return Error(ErrorCode::LimitExceeded, "draft declares more records than the documented bound");
  }

  CanonicalTables tables;
  tables.nodes = draft.nodes;
  tables.edges = draft.edges;
  tables.groups = draft.groups;
  tables.aliases = draft.aliases;
  tables.changeovers = draft.changeovers;

  const auto sort_and_check = [](auto& records, auto key, std::string_view what) -> Result<void> {
    for (const auto& record : records) {
      if (key(record).empty()) {
        return Error(ErrorCode::MalformedIdentifier, std::string(what) + " identity must not be empty");
      }
    }
    std::sort(records.begin(), records.end(),
              [&key](const auto& lhs, const auto& rhs) { return key(lhs) < key(rhs); });
    for (std::size_t index = 1; index < records.size(); ++index) {
      if (key(records[index]) == key(records[index - 1])) {
        return Error(ErrorCode::DuplicateIdentifier, std::string("two ") + std::string(what) +
                                                         " records declare the same identity")
            .with_subject(key(records[index]).str());
      }
    }
    return ok();
  };

  CT_TRYV(sort_and_check(tables.nodes, [](const Node& node) -> const NodeId& { return node.id; }, "node"));
  CT_TRYV(sort_and_check(tables.edges, [](const Edge& edge) -> const EdgeId& { return edge.id; }, "edge"));
  CT_TRYV(sort_and_check(tables.groups, [](const RedundancyGroup& group) -> const RedundancyGroupId& { return group.id; },
                         "redundancy group"));
  CT_TRYV(sort_and_check(tables.aliases, [](const Alias& alias) -> const AliasId& { return alias.id; }, "alias"));
  CT_TRYV(sort_and_check(tables.changeovers,
                         [](const ChangeoverGroup& group) -> const ChangeoverGroupId& { return group.id; },
                         "changeover group"));

  // Member order inside a declaration carries no meaning, so the canonical form
  // must not depend on it.
  for (RedundancyGroup& group : tables.groups) {
    std::sort(group.members.begin(), group.members.end(),
              [](const RedundancyMember& lhs, const RedundancyMember& rhs) {
                if (lhs.node != rhs.node) {
                  return lhs.node < rhs.node;
                }
                if (lhs.declared != rhs.declared) {
                  return lhs.declared < rhs.declared;
                }
                const bool lhs_domain = lhs.failure_domain.has_value();
                const bool rhs_domain = rhs.failure_domain.has_value();
                if (lhs_domain != rhs_domain) {
                  return !lhs_domain;
                }
                return lhs_domain && (*lhs.failure_domain < *rhs.failure_domain);
              });
  }
  for (ChangeoverGroup& group : tables.changeovers) {
    std::sort(group.members.begin(), group.members.end());
  }
  for (Node& node : tables.nodes) {
    std::sort(node.references.begin(), node.references.end());
  }
  return tables;
}

// ---------------------------------------------------------------------------
// encode_topology
// ---------------------------------------------------------------------------

Result<std::string> encode_topology(const TopologyHeader& header, const std::vector<Node>& nodes,
                                    const std::vector<Edge>& edges, const std::vector<RedundancyGroup>& groups,
                                    const std::vector<Alias>& aliases,
                                    const std::vector<ChangeoverGroup>& changeovers) {
  if (nodes.size() > limits::kMaxNodeCount || edges.size() > limits::kMaxEdgeCount ||
      groups.size() > limits::kMaxRedundancyGroupCount || aliases.size() > limits::kMaxAliasCount ||
      changeovers.size() > limits::kMaxChangeoverGroupCount) {
    return Error(ErrorCode::LimitExceeded, "generation declares more records than the documented bound");
  }
  Writer writer;
  writer.u16(header.schema_version);
  writer.u64(header.generation.value());
  writer.u64(header.parent_generation.value());
  writer.digest(header.parent_digest);
  writer.ref(header.facility);
  encode_provenance(writer, header.provenance);

  writer.u32(static_cast<std::uint32_t>(nodes.size()));
  for (const Node& node : nodes) {
    writer.string(node.id.value());
    encode_attributes(writer, node.attributes);
    writer.string(node.display_name);
    writer.u32(static_cast<std::uint32_t>(node.references.size()));
    for (const ExternalRef& reference : node.references) {
      writer.ref(reference);
    }
  }
  writer.u32(static_cast<std::uint32_t>(edges.size()));
  for (const Edge& edge : edges) {
    writer.string(edge.id.value());
    writer.u8(static_cast<std::uint8_t>(edge.kind));
    encode_endpoint(writer, edge.from);
    encode_endpoint(writer, edge.to);
  }
  writer.u32(static_cast<std::uint32_t>(groups.size()));
  for (const RedundancyGroup& group : groups) {
    writer.string(group.id.value());
    writer.u8(static_cast<std::uint8_t>(group.scheme));
    writer.u8(static_cast<std::uint8_t>(group.scope));
    writer.string(group.display_name);
    writer.string(group.basis);
    writer.u32(static_cast<std::uint32_t>(group.members.size()));
    for (const RedundancyMember& member : group.members) {
      writer.string(member.node.value());
      writer.string(member.declared);
      writer.optional(member.failure_domain, [&writer](const ExternalRef& value) { writer.ref(value); });
    }
    std::uint8_t flags = 0;
    if (group.require_distinct_failure_domains) {
      flags |= 0x01u;
    }
    if (group.require_independent_sources) {
      flags |= 0x02u;
    }
    writer.u8(flags);
  }
  writer.u32(static_cast<std::uint32_t>(aliases.size()));
  for (const Alias& alias : aliases) {
    writer.string(alias.id.value());
    writer.string(alias.target.value());
  }
  writer.u32(static_cast<std::uint32_t>(changeovers.size()));
  for (const ChangeoverGroup& group : changeovers) {
    writer.string(group.id.value());
    writer.string(group.display_name);
    writer.u32(static_cast<std::uint32_t>(group.members.size()));
    for (const Endpoint& member : group.members) {
      encode_endpoint(writer, member);
    }
    writer.u32(group.max_concurrent);
  }

  std::string payload = writer.take();
  if (payload.size() > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "canonical generation image exceeds the documented byte bound");
  }
  return payload;
}

// ---------------------------------------------------------------------------
// decode_topology
// ---------------------------------------------------------------------------

Result<DecodedGeneration> decode_topology(std::string_view payload) {
  if (payload.size() > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "canonical generation image exceeds the documented byte bound");
  }
  Reader reader(payload, "canonical generation");
  CT_TRY(schema, reader.u16());
  if (schema != kCanonicalSchemaVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "canonical generation schema version is not supported")
        .with_subject(std::to_string(schema));
  }
  DecodedGeneration decoded;
  CT_TRY(generation, reader.u64());
  CT_TRY(parent, reader.u64());
  CT_TRY(parent_digest, reader.digest());
  CT_TRY(facility, read_ref(reader, "facility reference"));
  CT_TRY(provenance, read_provenance(reader));
  decoded.header.schema_version = schema;
  decoded.header.generation = TopologyGeneration(generation);
  decoded.header.parent_generation = TopologyGeneration(parent);
  decoded.header.parent_digest = parent_digest;
  decoded.header.facility = std::move(facility);
  decoded.header.provenance = std::move(provenance);

  CT_TRY(node_count, reader.count(limits::kMaxNodeCount, "node"));
  decoded.tables.nodes.reserve(node_count);
  for (std::uint32_t index = 0; index < node_count; ++index) {
    CT_TRY(node, read_node(reader));
    decoded.tables.nodes.push_back(std::move(node));
  }
  CT_TRY(edge_count, reader.count(limits::kMaxEdgeCount, "edge"));
  decoded.tables.edges.reserve(edge_count);
  for (std::uint32_t index = 0; index < edge_count; ++index) {
    CT_TRY(edge, read_edge(reader));
    decoded.tables.edges.push_back(std::move(edge));
  }
  CT_TRY(group_count, reader.count(limits::kMaxRedundancyGroupCount, "redundancy group"));
  decoded.tables.groups.reserve(group_count);
  for (std::uint32_t index = 0; index < group_count; ++index) {
    CT_TRY(group, read_group(reader));
    decoded.tables.groups.push_back(std::move(group));
  }
  CT_TRY(alias_count, reader.count(limits::kMaxAliasCount, "alias"));
  decoded.tables.aliases.reserve(alias_count);
  for (std::uint32_t index = 0; index < alias_count; ++index) {
    CT_TRY(alias, read_alias(reader));
    decoded.tables.aliases.push_back(std::move(alias));
  }
  CT_TRY(changeover_count, reader.count(limits::kMaxChangeoverGroupCount, "changeover group"));
  decoded.tables.changeovers.reserve(changeover_count);
  for (std::uint32_t index = 0; index < changeover_count; ++index) {
    CT_TRY(group, read_changeover(reader));
    decoded.tables.changeovers.push_back(std::move(group));
  }
  if (!reader.at_end()) {
    return Error(ErrorCode::CountMismatch,
                 "canonical generation image has trailing bytes after the last table");
  }
  return decoded;
}

// ---------------------------------------------------------------------------
// Generation file framing
// ---------------------------------------------------------------------------

Result<std::string> encode_generation_file(std::string_view payload) {
  if (payload.empty()) {
    return Error(ErrorCode::EmptyInput, "canonical payload is empty");
  }
  if (payload.size() > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "canonical payload exceeds the documented byte bound");
  }
  Writer writer;
  writer.bytes(kGenerationFileMagic);
  writer.u16(kCanonicalSchemaVersion);
  writer.u16(0);
  writer.u64(static_cast<std::uint64_t>(payload.size()));
  writer.bytes(payload);
  writer.digest(digest_bytes(payload));
  return writer.take();
}

Result<GenerationFile> decode_generation_file(std::string_view bytes) {
  if (bytes.size() > limits::kMaxGenerationFileBytes) {
    return Error(ErrorCode::LimitExceeded, "generation file exceeds the documented byte bound");
  }
  if (bytes.size() < kFrameHeaderBytes + Digest::kBytes) {
    return Error(ErrorCode::TruncatedInput, "generation file is shorter than its fixed frame");
  }
  if (bytes.substr(0, kGenerationFileMagic.size()) != kGenerationFileMagic) {
    return Error(ErrorCode::MalformedRecord, "generation file magic does not match this format");
  }
  Reader reader(bytes, "generation file");
  reader.skip(kGenerationFileMagic.size());
  CT_TRY(schema, reader.u16());
  if (schema != kCanonicalSchemaVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "generation file schema version is not supported")
        .with_subject(std::to_string(schema));
  }
  CT_TRY(reserved, reader.u16());
  if (reserved != 0) {
    return Error(ErrorCode::MalformedRecord, "generation file reserved field is not zero");
  }
  CT_TRY(payload_length, reader.u64());
  if (payload_length > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "generation file declares a payload above the documented bound");
  }
  if (payload_length > reader.remaining() || reader.remaining() - payload_length != Digest::kBytes) {
    return Error(ErrorCode::CountMismatch,
                 "generation file payload length does not agree with the frame byte count");
  }
  const std::size_t payload_begin = reader.position();
  const std::string_view payload = bytes.substr(payload_begin, static_cast<std::size_t>(payload_length));
  reader.skip(static_cast<std::size_t>(payload_length));
  CT_TRY(frame_digest, reader.digest());
  if (!reader.at_end()) {
    return Error(ErrorCode::CountMismatch, "generation file has trailing bytes after its frame");
  }
  GenerationFile file;
  file.schema_version = schema;
  file.payload = std::string(payload);
  file.payload_digest = frame_digest;
  if (!(digest_bytes(file.payload) == frame_digest)) {
    return Error(ErrorCode::DigestMismatch, "generation payload does not match its recorded digest");
  }
  return file;
}

}  // namespace dccp::cooling_topology
