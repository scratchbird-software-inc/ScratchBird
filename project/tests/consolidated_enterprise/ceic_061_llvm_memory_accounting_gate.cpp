// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-061 focused validation for LLVM dynamic/static memory accounting.
#include "llvm_memory_accounting.hpp"
#include "memory_support_bundle.hpp"
#include "native_compile.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
namespace native = scratchbird::engine::native_compile;

using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ceic_061_llvm_memory_accounting_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& evidence, std::string_view needle) {
  for (const auto& entry : evidence) {
    if (entry.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasRow(const memory::MemorySupportBundleResult& bundle,
            std::string_view key,
            std::string_view value) {
  for (const auto& row : bundle.rows) {
    if (row.key == key && row.value == value) {
      return true;
    }
  }
  return false;
}

bool HasMetricFamily(const std::vector<scratchbird::core::metrics::MetricValue>& metrics,
                     std::string_view family) {
  for (const auto& metric : metrics) {
    if (metric.family == family) {
      return true;
    }
  }
  return false;
}

bool MetricHasLabel(const scratchbird::core::metrics::MetricValue& metric,
                    std::string_view key,
                    std::string_view value) {
  for (const auto& label : metric.labels) {
    if (label.key == key && label.value == value) {
      return true;
    }
  }
  return false;
}

bool HasGaugeValue(std::string_view family,
                   std::string_view operation,
                   std::string_view result,
                   std::string_view reason,
                   double value) {
  for (const auto& metric :
       scratchbird::core::metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    if (metric.family == family &&
        MetricHasLabel(metric, "operation", operation) &&
        MetricHasLabel(metric, "result", result) &&
        MetricHasLabel(metric, "reason", reason) &&
        metric.value == value) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "ceic_061_llvm_memory_accounting_gate";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    const std::string& suffix) {
  return {{memory::HierarchicalMemoryScopeKind::process, "ceic-061-process"},
          {memory::HierarchicalMemoryScopeKind::database, "ceic-061-database"},
          {memory::HierarchicalMemoryScopeKind::session,
           "ceic-061-session-" + suffix},
          {memory::HierarchicalMemoryScopeKind::statement,
           "ceic-061-statement-" + suffix}};
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "budget setup failed");
}

memory::LlvmMemoryAccountingRequest LlvmRequest(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger,
    memory::ForeignMemoryLinkageMode mode,
    const std::string& suffix) {
  memory::LlvmMemoryAccountingRequest request;
  request.reservation_ledger = budget_ledger;
  request.foreign_ledger = foreign_ledger;
  request.scope_chain = ScopeChain(suffix);
  request.owner_id = "ceic-061-owner-" + suffix;
  request.owning_scope = "ceic-061-scope-" + suffix;
  request.operation_id = "ceic-061-operation-" + suffix;
  request.native_callsite = "ceic_061.llvm";
  request.provider_label = "ceic-061-configured-provider";
  request.linkage_mode = mode;
  request.production_like = true;
  request.provider_available = true;
  request.loader_bytes = 512;
  request.static_link_metadata_bytes = 256;
  request.code_bytes = 768;
  request.data_bytes = 384;
  request.native_bytes = 640;
  request.provenance = Provenance();
  request.authority.evidence_label = "ceic_061_llvm_memory_accounting";
  request.authority.authority_generation = "ceic-061-runtime";
  request.evidence = {"ceic_061=true",
                      "reserve_before_llvm_or_native_call=true",
                      "memory_evidence_only=true",
                      "cluster_optimization_external_provider_only=true"};
  return request;
}

void ValidateSupportBundle(
    const memory::ForeignMemoryReservationSnapshot& snapshot,
    u64 expected_active,
    u64 expected_bytes) {
  memory::MemoryManager manager(memory::DefaultLocalEngineMemoryPolicy());
  memory::MemorySupportBundleRequest bundle_request;
  bundle_request.snapshot = manager.Snapshot();
  bundle_request.foreign_memory_snapshot = snapshot;
  bundle_request.include_foreign_memory = true;
  auto bundle =
      memory::BuildMemorySupportBundleEvidence(std::move(bundle_request));
  Require(bundle.ok(), "support bundle failed");
  Require(bundle.foreign_source_count >= 1,
          "support bundle omitted LLVM foreign source rows");
  Require(HasRow(bundle,
                 "foreign_memory.snapshot.active_reservation_count",
                 std::to_string(expected_active)),
          "support bundle active reservation row missing");
  Require(HasRow(bundle,
                 "foreign_memory.snapshot.current_estimated_bytes",
                 std::to_string(expected_bytes)),
          "support bundle current estimated bytes row missing");
  Require(Contains(bundle.evidence,
                   "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE"),
          "support bundle foreign-memory evidence anchor missing");
  Require(Contains(bundle.evidence,
                   "no_authority.benchmark_optimizer_index_agent=true"),
          "support bundle no-authority evidence missing");
}

void ValidateDynamicDefaultAndCleanup(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto request = LlvmRequest(budget_ledger,
                             foreign_ledger,
                             memory::ForeignMemoryLinkageMode::dynamic_library,
                             "dynamic");
  auto acquired =
      memory::AcquireLlvmMemoryAccountingReservation(std::move(request));
  Require(acquired.ok(), "dynamic LLVM memory reservation failed");
  Require(Contains(acquired.evidence,
                   "CEIC-061_LLVM_DYNAMIC_STATIC_MEMORY_ACCOUNTING"),
          "CEIC-061 dynamic evidence anchor missing");
  Require(Contains(acquired.evidence, "llvm_memory.linkage_mode=dynamic_library"),
          "dynamic linkage evidence missing");
  Require(Contains(acquired.evidence, "llvm_memory.reserved_phase=dynamic_loader"),
          "dynamic loader reservation evidence missing");
  Require(Contains(acquired.evidence, "llvm_memory.reserved_phase=jit_native"),
          "JIT/native reservation evidence missing");
  Require(acquired.reservation->reservation_count() == 4,
          "dynamic reservation phase count mismatch");
  Require(acquired.reservation->reserved_bytes() == 2304,
          "dynamic reserved bytes mismatch");
  Require(HasMetricFamily(acquired.metrics,
                          "sb_llvm_foreign_memory_reservations_total"),
          "LLVM memory reservation metric missing");
  Require(HasMetricFamily(acquired.metrics,
                          "sb_llvm_foreign_memory_reserved_bytes"),
          "LLVM memory reserved-bytes metric missing");

  const auto snapshot = foreign_ledger->Snapshot();
  Require(snapshot.active_reservation_count == 4,
          "foreign ledger dynamic active reservation count mismatch");
  Require(snapshot.current_estimated_bytes == 2304,
          "foreign ledger dynamic bytes mismatch");
  ValidateSupportBundle(snapshot, 4, 2304);

  const auto released = acquired.reservation->Release(
      memory::ForeignMemoryReleaseEvent::adapter_shutdown);
  Require(released.ok(), "dynamic LLVM release failed");
  Require(released.released_reservation_count == 4,
          "dynamic release count mismatch");
  Require(Contains(released.evidence, "llvm_memory.release.phase=dynamic_loader"),
          "dynamic release evidence missing");
  Require(foreign_ledger->Snapshot().current_estimated_bytes == 0,
          "dynamic foreign memory leaked");
  Require(HasGaugeValue("sb_llvm_foreign_memory_reserved_bytes",
                        "ceic-061-operation-dynamic",
                        "current",
                        "dynamic_library",
                        0.0),
          "LLVM current reserved-bytes gauge was not cleared on release");
}

void ValidateStaticOptionMetadata(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto request = LlvmRequest(budget_ledger,
                             foreign_ledger,
                             memory::ForeignMemoryLinkageMode::static_library,
                             "static");
  request.aot = true;
  auto acquired =
      memory::AcquireLlvmMemoryAccountingReservation(std::move(request));
  Require(acquired.ok(), "static LLVM memory reservation failed");
  Require(Contains(acquired.evidence, "llvm_memory.linkage_mode=static_library"),
          "static linkage evidence missing");
  Require(Contains(acquired.evidence,
                   "llvm_memory.reserved_phase=static_linkage_metadata"),
          "static metadata reservation evidence missing");
  Require(Contains(acquired.evidence, "llvm_memory.reserved_phase=aot_native"),
          "AOT/native reservation evidence missing");
  Require(acquired.reservation->reservation_count() == 4,
          "static reservation phase count mismatch");
  Require(acquired.reservation->reserved_bytes() == 2048,
          "static reserved bytes mismatch");
  Require(acquired.reservation->Release().ok(),
          "static LLVM release failed");
}

void ValidateProviderRefusalAndFixtureSeparation(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto unavailable = LlvmRequest(
      budget_ledger,
      foreign_ledger,
      memory::ForeignMemoryLinkageMode::dynamic_library,
      "unavailable");
  unavailable.provider_available = false;
  auto refused =
      memory::AcquireLlvmMemoryAccountingReservation(std::move(unavailable));
  Require(!refused.ok() && refused.fail_closed,
          "production unavailable LLVM provider was accepted");
  Require(refused.status.code == StatusCode::memory_invalid_request,
          "production unavailable LLVM status mismatch");
  Require(Contains(refused.evidence, "llvm_memory.reservation_created=false"),
          "production unavailable refusal evidence missing");
  Require(HasMetricFamily(refused.metrics,
                          "sb_llvm_foreign_memory_refusals_total"),
          "LLVM refusal metric missing");

  auto fixture = LlvmRequest(budget_ledger,
                             foreign_ledger,
                             memory::ForeignMemoryLinkageMode::dynamic_library,
                             "fixture");
  fixture.provider_available = false;
  fixture.production_like = false;
  fixture.explicit_test_fixture = true;
  auto acquired_fixture =
      memory::AcquireLlvmMemoryAccountingReservation(std::move(fixture));
  Require(acquired_fixture.ok(),
          "explicit LLVM fixture reservation was refused");
  Require(Contains(acquired_fixture.evidence,
                   "llvm_memory.test_fixture_explicit=true"),
          "explicit fixture evidence missing");
  Require(acquired_fixture.reservation->Release().ok(),
          "explicit fixture release failed");
}

native::NativeCompileRequest NativeRequest(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger,
    bool required) {
  native::NativeCompileRequest request;
  request.module_payload = "sblr:predicate:ceic_061_col_i32_gt_const";
  request.target_object_uuid = "018f0000-0000-7000-8000-000000006161";
  request.principal_uuid = "018f0000-0000-7000-8000-000000006162";
  request.database_path = "/tmp/sb_ceic_061_llvm_memory";
  request.catalog_generation_id = 6101;
  request.security_epoch = 6102;
  request.policy_epoch = 6103;
  request.resource_epoch = 6104;
  request.security_context_present = true;
  request.allow_interpreter_fallback = !required;
  request.policy_profiles.push_back(
      required ? "native_compile.jit_required_for_declared_units"
               : "native_compile.jit_optional");
  request.descriptors.push_back({"018f0000-0000-7000-8000-000000006163",
                                 "table_descriptor",
                                 "sys.ceic_061",
                                 "columns:i32"});
  request.memory_accounting.reservation_ledger = budget_ledger;
  request.memory_accounting.foreign_ledger = foreign_ledger;
  request.memory_accounting.scope_chain = ScopeChain(required ? "native-required"
                                                              : "native-optional");
  request.memory_accounting.operation_id = required
                                               ? "ceic-061-native-required"
                                               : "ceic-061-native-optional";
  request.memory_accounting.native_callsite = "ceic_061.native_compile";
  request.memory_accounting.evidence.push_back(
      "ceic_061_native_compile_memory_accounting=true");
  return request;
}

void ValidateNativeCompileMemoryPath(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto required = native::CompileNativeUnit(
      NativeRequest(budget_ledger, foreign_ledger, true));
  if (required.backend_available) {
    Require(required.ok && required.compiled,
            "available LLVM native compile did not compile");
    Require(required.llvm_memory_accounting_required,
            "available LLVM compile did not require memory accounting");
    Require(required.llvm_memory_reserved,
            "available LLVM compile did not reserve memory");
    Require(required.llvm_memory_released,
            "available LLVM compile did not release memory");
    Require(required.llvm_memory_reserved_bytes != 0,
            "available LLVM compile reserved zero bytes");
    Require(Contains(required.llvm_memory_evidence,
                     "reserve_before_llvm_or_native_call=true"),
            "native compile reserve-before-call evidence missing");
  } else {
    Require(!required.ok, "unavailable required LLVM compile succeeded");
    Require(required.diagnostic_code == "NATIVE.LLVM_BACKEND_UNAVAILABLE" ||
                required.diagnostic_code == "NATIVE.LLVM_MEMORY_ACCOUNTING_REFUSED",
            "unavailable LLVM compile did not fail closed diagnostically");
    if (!required.llvm_library_path.empty()) {
      Require(required.llvm_memory_released,
              "failed LLVM load with configured library did not release reservations");
    }
  }

  auto fixture = NativeRequest(budget_ledger, foreign_ledger, false);
  fixture.simulate_backend_unavailable = true;
  fixture.allow_interpreter_fallback = true;
  fixture.memory_accounting.explicit_test_fixture = true;
  fixture.memory_accounting.production_like = false;
  auto fallback = native::CompileNativeUnit(fixture);
  Require(fallback.ok && fallback.fallback_used,
          "explicit LLVM fixture fallback failed");
  Require(fallback.llvm_memory_test_fixture,
          "explicit LLVM fixture fallback did not mark fixture mode");

  auto non_fixture = NativeRequest(budget_ledger, foreign_ledger, false);
  non_fixture.simulate_backend_unavailable = true;
  non_fixture.allow_interpreter_fallback = true;
  auto refused = native::CompileNativeUnit(non_fixture);
  Require(!refused.ok &&
              refused.diagnostic_code == "NATIVE.LLVM_TEST_FIXTURE_REQUIRED",
          "non-fixture unavailable simulation was accepted");
}

void ValidateAuthorityRefusals(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto unsafe = LlvmRequest(budget_ledger,
                            foreign_ledger,
                            memory::ForeignMemoryLinkageMode::dynamic_library,
                            "unsafe");
  unsafe.authority.optimizer_plan_authority = true;
  auto refused =
      memory::AcquireLlvmMemoryAccountingReservation(std::move(unsafe));
  Require(!refused.ok() && refused.fail_closed,
          "LLVM optimizer-plan authority drift was accepted");
  Require(Contains(refused.evidence, "no_authority.transaction_finality=true"),
          "LLVM authority refusal omitted no-authority evidence");
}

}  // namespace

int main() {
  memory::HierarchicalMemoryBudgetLedger budget_ledger;
  memory::ForeignMemoryReservationLedger foreign_ledger;
  SetBudget(&budget_ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-061-process",
            16ull * 1024ull * 1024ull);

  ValidateDynamicDefaultAndCleanup(&budget_ledger, &foreign_ledger);
  ValidateStaticOptionMetadata(&budget_ledger, &foreign_ledger);
  ValidateProviderRefusalAndFixtureSeparation(&budget_ledger, &foreign_ledger);
  ValidateNativeCompileMemoryPath(&budget_ledger, &foreign_ledger);
  ValidateAuthorityRefusals(&budget_ledger, &foreign_ledger);

  Require(foreign_ledger.Snapshot().active_reservation_count == 0,
          "foreign ledger leaked active LLVM reservations");
  Require(foreign_ledger.Snapshot().current_estimated_bytes == 0,
          "foreign ledger leaked LLVM bytes");
  Require(budget_ledger.Snapshot().current_bytes == 0,
          "budget ledger leaked LLVM bytes");

  std::cout << "CEIC-061 LLVM memory accounting gate passed\n";
  return 0;
}
