// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace memory = scratchbird::core::memory;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy Policy(const char* name) {
  memory::AllocationPolicy policy;
  policy.policy_name = name;
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

memory::MemoryTag Tag(const char* purpose) {
  memory::MemoryTag tag;
  tag.purpose = purpose;
  tag.category = memory::MemoryCategory::test_probe;
  tag.lifetime = memory::MemoryLifetime::temporary;
  tag.owner = "public_release_correctness";
  tag.context_id = "PCR-011";
  return tag;
}

bool UnconfiguredRefusesAllocation() {
  const auto before = memory::DefaultMemoryManagerState();
  bool ok = true;
  ok = Expect(!before.initialized, "default manager should start without production configuration") && ok;
  ok = Expect(!before.explicitly_configured, "default manager should not report explicit configuration") && ok;
  ok = Expect(before.active_policy.refuse_all_allocations,
              "unconfigured default manager policy must refuse all allocations") && ok;

  auto allocation = memory::DefaultMemoryManager().Allocate(32, alignof(std::max_align_t), Tag("unconfigured"));
  ok = Expect(!allocation.ok(), "unconfigured default manager allocation must fail closed") && ok;
  ok = Expect(allocation.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-DEFAULT-MANAGER-UNCONFIGURED",
              "unconfigured default manager should report explicit diagnostic") && ok;

  const auto after = memory::DefaultMemoryManagerState();
  ok = Expect(!after.initialized, "unconfigured default access must not publish a production manager") && ok;
  ok = Expect(after.active_policy.refuse_all_allocations,
              "unconfigured default manager state must remain refusal-only") && ok;
  return ok;
}

bool ProductionConfigurationAdmitsAllocation() {
  const auto missing_provenance =
      memory::ConfigureDefaultMemoryManager(Policy("missing_provenance_memory_policy"), "");
  bool ok = true;
  ok = Expect(!missing_provenance.ok(), "production memory policy must require startup provenance") && ok;
  const auto after_missing_provenance = memory::DefaultMemoryManagerState();
  ok = Expect(!after_missing_provenance.initialized,
              "failed production configuration must not initialize default manager") && ok;
  ok = Expect(after_missing_provenance.active_policy.refuse_all_allocations,
              "failed production configuration must leave refusal-only default policy active") && ok;

  auto refusal_policy = Policy("refusal_memory_policy");
  refusal_policy.refuse_all_allocations = true;
  const auto refusal_configuration =
      memory::ConfigureDefaultMemoryManager(refusal_policy, "public_release_startup_policy");
  ok = Expect(!refusal_configuration.ok(),
              "production memory policy must reject refusal-only policy as configured default") && ok;
  const auto after_refusal_policy = memory::DefaultMemoryManagerState();
  ok = Expect(!after_refusal_policy.initialized,
              "rejected refusal-only production policy must not initialize default manager") && ok;
  ok = Expect(after_refusal_policy.active_policy.refuse_all_allocations,
              "rejected production policy must leave refusal-only default policy active") && ok;

  const auto configured =
      memory::ConfigureDefaultMemoryManager(Policy("public_production_memory_policy"),
                                            "public_release_startup_policy");
  ok = Expect(configured.ok(), "production memory policy configuration should succeed") && ok;
  ok = Expect(configured.applied, "production memory policy should be applied") && ok;
  ok = Expect(!configured.fixture_mode, "production memory policy must not be fixture mode") && ok;

  const auto state = memory::DefaultMemoryManagerState();
  ok = Expect(state.initialized, "configured default manager should be initialized") && ok;
  ok = Expect(state.explicitly_configured, "configured default manager should record explicit configuration") && ok;
  ok = Expect(!state.fixture_mode, "configured production manager should not be fixture mode") && ok;
  ok = Expect(state.provenance == "public_release_startup_policy",
              "configured production manager should retain startup provenance") && ok;
  ok = Expect(!state.active_policy.refuse_all_allocations,
              "configured production manager must not use refusal-only policy") && ok;

  auto allocation = memory::DefaultMemoryManager().Allocate(64, alignof(std::max_align_t), Tag("production"));
  ok = Expect(allocation.ok(), "configured production default manager allocation should succeed") && ok;
  if (allocation.ok()) {
    auto release = memory::DefaultMemoryManager().Deallocate(allocation.pointer, Tag("production_cleanup"));
    ok = Expect(release.ok(), "configured production allocation cleanup should succeed") && ok;
  }

  const auto idempotent =
      memory::ConfigureDefaultMemoryManager(Policy("public_production_memory_policy"),
                                            "public_release_startup_policy");
  ok = Expect(idempotent.ok(), "idempotent production memory policy configuration should succeed") && ok;
  ok = Expect(idempotent.already_initialized,
              "idempotent production configuration should report existing manager") && ok;
  return ok;
}

bool FixtureConfigurationIsExplicitAndSeparated() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(Policy("public_fixture_memory_policy"),
                                                      "public_default_memory_manager_gate");
  bool ok = true;
  ok = Expect(configured.ok(), "fixture memory policy configuration should succeed") && ok;
  ok = Expect(configured.applied, "fixture memory policy should be applied") && ok;
  ok = Expect(configured.fixture_mode, "fixture memory policy should report fixture mode") && ok;

  const auto state = memory::DefaultMemoryManagerState();
  ok = Expect(state.initialized, "fixture default manager should be initialized") && ok;
  ok = Expect(state.explicitly_configured, "fixture default manager should be explicit") && ok;
  ok = Expect(state.fixture_mode, "fixture default manager should retain fixture mode") && ok;
  ok = Expect(state.provenance == "fixture:public_default_memory_manager_gate",
              "fixture default manager should retain fixture provenance") && ok;

  auto allocation = memory::DefaultMemoryManager().Allocate(64, alignof(std::max_align_t), Tag("fixture"));
  ok = Expect(allocation.ok(), "configured fixture default manager allocation should succeed") && ok;
  if (allocation.ok()) {
    auto release = memory::DefaultMemoryManager().Deallocate(allocation.pointer, Tag("fixture_cleanup"));
    ok = Expect(release.ok(), "configured fixture allocation cleanup should succeed") && ok;
  }

  const auto production_reconfigure =
      memory::ConfigureDefaultMemoryManager(Policy("public_production_memory_policy"),
                                            "public_release_startup_policy");
  ok = Expect(!production_reconfigure.ok(),
              "fixture default manager must not be silently reclassified as production") && ok;
  ok = Expect(production_reconfigure.already_initialized,
              "fixture-to-production reconfigure should report existing manager") && ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_default_memory_manager_gate <unconfigured|production|fixture>\n";
    return EXIT_FAILURE;
  }

  const std::string mode = argv[1];
  bool ok = false;
  if (mode == "unconfigured") {
    ok = UnconfiguredRefusesAllocation();
  } else if (mode == "production") {
    ok = ProductionConfigurationAdmitsAllocation();
  } else if (mode == "fixture") {
    ok = FixtureConfigurationIsExplicitAndSeparated();
  } else {
    std::cerr << "unknown mode: " << mode << '\n';
    return EXIT_FAILURE;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
