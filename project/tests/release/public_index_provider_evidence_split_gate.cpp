// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_access_method.hpp"
#include "index_family_registry.hpp"
#include "index_route_capability.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace index = scratchbird::core::index;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

index::IndexProviderAdmissionResult StaticOnlyAdmission(
    index::IndexFamily family,
    const index::IndexFamilyPhysicalCapabilityState& state) {
  index::IndexProviderAccessMethodContract contract;
  contract.family = family;
  contract.route = index::IndexRouteKind::sql_select;
  contract.route_boundary.static_registry_complete_capability_seen =
      state.physically_complete();
  contract.route_boundary.route_capability_present =
      index::FindBuiltinIndexRouteCapabilityState(contract.route, family) !=
      nullptr;
  contract.route_boundary.route_specific_boundary_declared = true;
  return index::AdmitIndexProviderAccessMethod(contract);
}

}  // namespace

int main() {
  std::size_t families = 0;
  std::size_t physically_complete = 0;
  std::size_t static_contracts = 0;
  std::size_t static_only_refusals = 0;

  for (const auto& descriptor : index::BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        index::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr, "PCR-051 missing family capability state");
    ++families;

    Require(state->declared_capability,
            "PCR-051 built-in descriptor lacks declared capability state");
    Require(state->static_contract == state->declared_capability,
            "PCR-051 static_contract must mirror the static declaration");
    Require(state->evidence_required == state->static_contract,
            "PCR-051 evidence_required must be explicit for each static "
            "contract");
    Require(!state->provider_present,
            "PCR-051 static declarations must not imply provider presence");
    Require(!state->provider_admitted,
            "PCR-051 static declarations must not imply provider admission");
    Require(!state->durable_closure_admitted,
            "PCR-051 static declarations must not imply durable closure");

    if (state->static_contract) {
      ++static_contracts;
    }
    if (state->physically_complete()) {
      ++physically_complete;
      Require(state->evidence_required,
              "PCR-051 physically complete static rows still require provider "
              "evidence for production admission");
    }

    const auto admission = StaticOnlyAdmission(descriptor.family, *state);
    Require(!admission.ok(),
            "PCR-051 static-only contract was admitted as a provider");
    Require(!admission.admitted,
            "PCR-051 static-only contract reported admitted");
    Require(admission.fail_closed,
            "PCR-051 static-only contract did not fail closed");
    Require(!admission.durable_family_closure_claimed,
            "PCR-051 static-only contract claimed durable family closure");
    Require(!admission.enterprise_ready_claimed,
            "PCR-051 static-only contract claimed enterprise readiness");
    Require(admission.admitted == state->provider_admitted,
            "PCR-051 provider admission state diverged from static-only "
            "admission");
    Require(admission.durable_family_closure_claimed ==
                state->durable_closure_admitted,
            "PCR-051 durable closure state diverged from static-only "
            "admission");

    if (descriptor.persistence == index::IndexPersistenceClass::persistent &&
        state->physically_complete()) {
      Require(admission.admission_status ==
                  index::IndexProviderAdmissionStatus::static_capability_only,
              "PCR-051 persistent complete static row must be refused as "
              "static capability only");
      ++static_only_refusals;
    }
  }

  Require(families == index::BuiltinIndexFamilyDescriptors().size(),
          "PCR-051 descriptor/state count mismatch");
  Require(static_contracts == families,
          "PCR-051 every built-in descriptor must expose its static contract");
  Require(physically_complete != 0,
          "PCR-051 no physically complete static capabilities were tested");
  Require(static_only_refusals != 0,
          "PCR-051 no persistent static-only provider refusals were tested");

  std::cout << "public_index_provider_evidence_split_gate=passed families="
            << families << " physically_complete=" << physically_complete
            << " static_only_refusals=" << static_only_refusals << '\n';
  return EXIT_SUCCESS;
}
