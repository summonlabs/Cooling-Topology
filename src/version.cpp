// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_topology/version.hpp"

namespace dccp::cooling_topology {

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view systems_boundary() noexcept {
  return "generation-bound structural model of data-centre cooling connectivity; no operating state, no flow, no "
         "capacity, no thermal safety, no actuation";
}

std::string_view component_id() noexcept { return "dccp-cooling-topology/1.0.0"; }

}  // namespace dccp::cooling_topology
