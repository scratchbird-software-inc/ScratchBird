#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/diagnostic_value_fixture.hpp"

// CEIC-061 focused validation for LLVM dynamic/static memory accounting.
#include "llvm_memory_accounting.hpp"
#include "memory_support_bundle.hpp"
#include "native_compile.hpp"
#include "metric_builtin_definitions.hpp"
#include "metric_observation_queue.hpp"
#include <algorithm>
#include <map>

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

namespace metrics = scratchbird::core::metrics;
bool metric_fixture_ready = false;
std::shared_ptr<metrics::MetricObservationQueue> metric_queue;
metrics::MetricRetentionPolicy metric_policy;
std::map<std::string, metrics::MetricDescriptor> metric_descriptors;
std::map<metrics::MetricUuid, metrics::MetricSeriesIdentity> metric_series;
metrics::MetricUuid MetricId(unsigned n) {
  auto id = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-00000000ef00");
  id.bytes[14] = static_cast<unsigned char>(n >> 8);
  id.bytes[15] = static_cast<unsigned char>(n);
  return id;
}
unsigned next_metric_id = 10;
void RegisterMetricFixture() {
  auto queue = metrics::MetricObservationQueue::Create({MetricId(1), MetricId(2), {}}, {128, 1024*1024});
  Require(queue.ok(), "create LLVM observation queue");
  metric_queue = std::move(queue.queue);
  auto& registry = metrics::DefaultMetricRegistry();
  Require(registry.BindObservationQueue(metric_queue).ok, "bind LLVM node observation source");
  metric_policy.policy_uuid = MetricId(3); metric_policy.generation = 1;
  metric_policy.policy_name = "LLVM component retention fixture";
  const auto definitions = metrics::BuiltinMetricDescriptorDefinitions();
  for (const auto* family : {"sb_llvm_foreign_memory_reservations_total",
                           "sb_llvm_foreign_memory_reserved_bytes",
                           "sb_llvm_foreign_memory_refusals_total"}) {
    const auto found = std::find_if(definitions.begin(), definitions.end(),
        [&](const auto& d) { return d.family == family; });
    Require(found != definitions.end(), "LLVM compiled metric definition missing");
    metrics::MetricDescriptor descriptor;
    static_cast<metrics::MetricDescriptorDefinition&>(descriptor) = *found;
    descriptor.metric_uuid = MetricId(next_metric_id++); descriptor.descriptor_generation = 1;
    descriptor.label_schema_uuid = MetricId(next_metric_id++); descriptor.label_schema_generation = 1;
    descriptor.retention_policy_uuid = metric_policy.policy_uuid; descriptor.retention_policy_generation = 1;
    descriptor.visibility_policy_uuid = MetricId(next_metric_id++); descriptor.visibility_policy_generation = 1;
    descriptor.readiness = metrics::MetricReadiness::implemented;
    Require(registry.RegisterDescriptor(descriptor).ok, "register LLVM fixture descriptor");
    metric_descriptors.emplace(family, std::move(descriptor));
  }
  metric_fixture_ready = true;
}
void RegisterRequestMetrics(const memory::LlvmMemoryAccountingRequest& request) {
  if (!metric_fixture_ready) return;
  for (const auto& [family, descriptor] : metric_descriptors) {
    const bool refusal = family == "sb_llvm_foreign_memory_refusals_total";
    const char* result = refusal ? "refused" :
        family == "sb_llvm_foreign_memory_reserved_bytes" ? "current" : "reserved";
    const char* reason = refusal ? "SB_CEIC_061_LLVM_MEMORY_REQUEST_REFUSED" :
        memory::ForeignMemoryLinkageModeName(request.linkage_mode);
    metrics::MetricHistoryBinding binding;
    static_cast<metrics::MetricDescriptorBinding&>(binding) = descriptor;
    binding.database_uuid = MetricId(1); binding.node_uuid = MetricId(2);
    const auto series = metrics::MakeMetricSeriesIdentity(descriptor,
        {{"component", "llvm_memory"}, {"operation", request.operation_id},
         {"result", result}, {"reason", reason}}, metric_policy, binding,
        MetricId(next_metric_id++), 1);
    Require(series.ok() && metrics::DefaultMetricRegistry().RegisterSeries(*series.record, metric_policy).ok,
            "register LLVM fixture series");
    metric_series.emplace(series.record->series_uuid, *series.record);
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
    if (row.key == key && scratchbird::tests::DiagnosticTextEquals(row.value, value)) {
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
    if (label.key == key && std::holds_alternative<std::string>(label.value) &&
        std::get<std::string>(label.value) == value) {
      return true;
    }
  }
  return false;
}

bool HasGaugeValue(std::string_view family,
                   std::string_view operation,
                   std::string_view result,
                   std::string_view reason,
                   u64 value) {
  for (const auto& metric :
       scratchbird::core::metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    if (metric.family == family &&
        MetricHasLabel(metric, "operation", operation) &&
        MetricHasLabel(metric, "result", result) &&
        MetricHasLabel(metric, "reason", reason) &&
        std::holds_alternative<u64>(metric.value) &&
        std::get<u64>(metric.value) == value) {
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
  RegisterRequestMetrics(request);
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
                   "memory_support_bundle.benchmark_optimizer_index_agent_authority=false"),
          "support bundle authority=false evidence missing");
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
  Require(acquired.metric_publication.attempted == 2 && acquired.metric_publication.accepted == 2 &&
          !acquired.metric_publication.allocation_failed && acquired.metrics.size() == 2,
          "LLVM acquisition did not publish exactly its two typed series");
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
  Require(released.metric_publication.attempted == 1 && released.metric_publication.accepted == 1,
          "LLVM release zero observation was not accepted");
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
  request.target_object_uuid = scratchbird::tests::FixtureUuidLiteral("018f0000-0000-7000-8000-000000006161");
  request.principal_uuid = scratchbird::tests::FixtureUuidLiteral("018f0000-0000-7000-8000-000000006162");
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
  request.descriptors.push_back({scratchbird::tests::FixtureUuidLiteral("018f0000-0000-7000-8000-000000006163"),
                                 "table_descriptor",
                                 "sys.ceic_061",
                                 "columns:i32"});
  request.memory_accounting.reservation_ledger = budget_ledger;
  request.memory_accounting.foreign_ledger = foreign_ledger;
  request.memory_accounting.scope_chain = ScopeChain(required ? "native-required"
                                                              : "native-optional");
  request.memory_accounting.owner_id = required ? "ceic-061-native-required-owner"
                                                 : "ceic-061-native-optional-owner";
  request.memory_accounting.owning_scope = required ? "ceic-061-native-required-scope"
                                                     : "ceic-061-native-optional-scope";
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

void ValidateUnregisteredPublication() {
  memory::HierarchicalMemoryBudgetLedger lower;
  memory::ForeignMemoryReservationLedger foreign;
  auto acquired = memory::AcquireLlvmMemoryAccountingReservation(LlvmRequest(
      &lower, &foreign, memory::ForeignMemoryLinkageMode::dynamic_library, "unregistered"));
  Require(acquired.ok() && acquired.metrics.empty() &&
          acquired.metric_publication.attempted == 2 && acquired.metric_publication.accepted == 0 &&
          acquired.metric_publication.last_refusal.diagnostic_code == "SB-METRICS-FAMILY-UNKNOWN",
          "unregistered observation must not fabricate samples or alter reservation outcome");
  const auto released = acquired.reservation->Release();
  Require(released.ok() && released.metric_publication.accepted == 0 && lower.Snapshot().current_bytes == 0,
          "missing metrics registration must not hide completed release");
  Require(metrics::DefaultMetricRegistry().Descriptors().empty(),
          "LLVM producer must not manufacture catalog identities");
}

void ValidateExactValuesAndMissingSeries() {
  memory::HierarchicalMemoryBudgetLedger lower;
  memory::ForeignMemoryReservationLedger foreign;
  auto request = LlvmRequest(&lower, &foreign,
      memory::ForeignMemoryLinkageMode::dynamic_library, "exact-integer");
  request.reserve_loader_or_link_metadata = request.reserve_code = request.reserve_data = false;
  request.native_bytes = (u64{1} << 53) + 1;
  auto acquired = memory::AcquireLlvmMemoryAccountingReservation(request);
  Require(acquired.ok() && acquired.metric_publication.accepted == 2 && acquired.metrics.size() == 2,
          "exact integer reservation observation failed");
  Require(HasGaugeValue("sb_llvm_foreign_memory_reserved_bytes", request.operation_id,
                       "current", "dynamic_library", request.native_bytes),
          "bytes above double precision must remain exact uint64");
  for (const auto& metric : acquired.metrics)
    Require(MetricHasLabel(metric, "operation", request.operation_id),
            "LLVM result captured another operation's series");
  Require(acquired.reservation->Release().ok(), "exact integer accounting release");
  request.operation_id += "-not-registered";
  auto missing = memory::AcquireLlvmMemoryAccountingReservation(request);
  Require(missing.ok() && missing.metrics.empty() && missing.metric_publication.accepted == 0 &&
          missing.metric_publication.last_refusal.diagnostic_code == "METRIC.OBSERVATION_SOURCE_UNAVAILABLE",
          "missing series must not reuse another operation's prior samples");
  Require(missing.reservation->Release().ok() && lower.Snapshot().current_bytes == 0,
          "missing series must not prevent actual cleanup");
}

void ValidateObservationWireAndQueueFailure() {
  bool exact_seen = false;
  unsigned decoded_count = 0;
  for (;;) {
    auto held = metric_queue->TryAcquire();
    if (held.error == metrics::MetricQueueError::empty) break;
    Require(held.ok(), "acquire actual LLVM observation");
    const auto& observation = *held.lease.observation;
    const auto& series = metric_series.at(observation.series_uuid);
    const auto& descriptor = metric_descriptors.at(series.metric_family);
    const auto decoded = metrics::DecodeMetricRawSample(descriptor, series, observation.bytes);
    Require(decoded.ok() && observation.binding.database_uuid == MetricId(1) &&
            observation.binding.node_uuid == MetricId(2) && observation.binding.cluster_uuid.is_nil(),
            "LLVM observation wire or exact binary node ownership invalid");
    if (series.metric_family == "sb_llvm_foreign_memory_reserved_bytes" &&
        std::get<u64>(decoded.record->value.value) == (u64{1} << 53) + 1) exact_seen = true;
    // Component fixture consumes/discards validated observations; no durable
    // recorder, catalog activation or production retention proof is claimed.
    Require(metric_queue->TryRemove(held.lease) == metrics::MetricQueueError::none,
            "retire inspected fixture observation");
    ++decoded_count;
  }
  Require(exact_seen && decoded_count > 0, "missing exact large-byte wire observation");
  memory::HierarchicalMemoryBudgetLedger lower;
  memory::ForeignMemoryReservationLedger foreign;
  auto request = LlvmRequest(&lower, &foreign,
      memory::ForeignMemoryLinkageMode::dynamic_library, "queue-full");
  auto warm = memory::AcquireLlvmMemoryAccountingReservation(request);
  Require(warm.ok() && warm.metric_publication.accepted == 2 && warm.reservation->Release().ok(),
          "queue-full prior sample setup");
  auto& registry = metrics::DefaultMetricRegistry();
  for (unsigned i = 0; i != 256; ++i) {
    const auto published = registry.IncrementCounter("sb_llvm_foreign_memory_reservations_total",
        {{"component", "llvm_memory"}, {"operation", request.operation_id},
         {"result", "reserved"}, {"reason", "dynamic_library"}}, u64{1}, "llvm_memory_accounting");
    if (!published.ok) break;
  }
  Require(metric_queue->Stats().full != 0, "real observation queue did not fill");
  const auto admitted = metric_queue->Stats().admitted;
  auto blocked = memory::AcquireLlvmMemoryAccountingReservation(request);
  Require(blocked.ok() && blocked.metric_publication.attempted == 2 &&
          blocked.metric_publication.accepted == 0 && blocked.metrics.empty() &&
          metric_queue->Stats().admitted == admitted && lower.Snapshot().current_bytes == 2304,
          "rejected publication returned old samples or changed memory outcome");
  const auto released = blocked.reservation->Release();
  Require(released.ok() && released.metric_publication.accepted == 0 &&
          lower.Snapshot().current_bytes == 0 && foreign.Snapshot().current_estimated_bytes == 0,
          "full observation queue prevented actual release");
}

}  // namespace

int main() {
  ValidateUnregisteredPublication();
  RegisterMetricFixture();
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
  ValidateExactValuesAndMissingSeries();
  ValidateObservationWireAndQueueFailure();

  Require(foreign_ledger.Snapshot().active_reservation_count == 0,
          "foreign ledger leaked active LLVM reservations");
  Require(foreign_ledger.Snapshot().current_estimated_bytes == 0,
          "foreign ledger leaked LLVM bytes");
  Require(budget_ledger.Snapshot().current_bytes == 0,
          "budget ledger leaked LLVM bytes");

  std::cout << "CEIC-061 LLVM memory accounting gate passed\n";
  return 0;
}
