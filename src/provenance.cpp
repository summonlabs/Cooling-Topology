// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "dccp/cooling_topology/text.hpp"
#include "dccp/cooling_topology/topology.hpp"

namespace dccp::cooling_topology {
namespace {

constexpr std::string_view kProvenanceOriginTokens[] = {"authored", "imported", "reconciled", "recovered"};
constexpr std::size_t kProvenanceOriginCount =
    sizeof(kProvenanceOriginTokens) / sizeof(kProvenanceOriginTokens[0]);

}  // namespace

std::string_view to_token(ProvenanceOrigin value) noexcept {
  const auto raw = static_cast<std::size_t>(value);
  return raw < kProvenanceOriginCount ? kProvenanceOriginTokens[raw] : std::string_view("unknown");
}

Result<ProvenanceOrigin> parse_provenance_origin(std::string_view token) {
  for (std::size_t index = 0; index < kProvenanceOriginCount; ++index) {
    if (token == kProvenanceOriginTokens[index]) {
      return static_cast<ProvenanceOrigin>(index);
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown provenance-origin token")
      .with_subject(std::string(token.substr(0, 64)));
}

Result<Provenance> Provenance::create(std::string producer, ProvenanceOrigin origin, std::string witness,
                                      std::optional<ExternalRef> source_reference, AuthorityEpoch authority_epoch,
                                      std::vector<EvidenceBinding> evidence) {
  if (producer.empty()) {
    return Error(ErrorCode::MissingField, "provenance producer must not be empty");
  }
  if (!is_valid_display_text(producer, limits::kMaxProducerBytes)) {
    return Error(ErrorCode::InvalidUtf8,
                 "provenance producer must be valid UTF-8 display text without control characters")
        .with_subject(producer.substr(0, 64));
  }
  if (!witness.empty() && !is_valid_display_text(witness, limits::kMaxWitnessBytes)) {
    return Error(ErrorCode::InvalidUtf8, "provenance witness must be valid UTF-8 display text");
  }
  if (source_reference.has_value() && source_reference->identity.empty()) {
    return Error(ErrorCode::MissingField, "provenance source reference must not be empty");
  }
  if (evidence.size() > limits::kMaxEvidenceBindings) {
    return Error(ErrorCode::LimitExceeded, "provenance carries more evidence bindings than the documented bound");
  }
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].producer.empty() || evidence[index].subject.identity.empty()) {
      return Error(ErrorCode::MissingField, "evidence binding must name its producer and its subject");
    }
    for (std::size_t other = index + 1; other < evidence.size(); ++other) {
      if (evidence[index].same_binding_as(evidence[other])) {
        return Error(ErrorCode::DuplicateField, "the same evidence binding is recorded twice");
      }
    }
  }
  Provenance provenance;
  provenance.producer = std::move(producer);
  provenance.origin = origin;
  provenance.witness = std::move(witness);
  provenance.source_reference = std::move(source_reference);
  provenance.authority_epoch = authority_epoch;
  provenance.evidence = std::move(evidence);
  return provenance;
}

}  // namespace dccp::cooling_topology
