// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_contracts.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

MetricValidationResult RequireNonEmpty(const std::string& value, const std::string& diagnostic_detail) {
  if (value.empty()) {
    return MetricError("SB-METRICS-CONTRACT-LABEL-REQUIRED", diagnostic_detail);
  }
  return MetricOk();
}

MetricValidationResult RequireRange(const std::string& family, double value, double minimum, double maximum) {
  if (value < minimum || value > maximum) {
    return MetricError("SB-METRICS-CONTRACT-VALUE-RANGE", family);
  }
  return MetricOk();
}

MetricLabelSet MergeLabels(MetricLabelSet first, MetricLabelSet second) {
  first.insert(first.end(), std::make_move_iterator(second.begin()), std::make_move_iterator(second.end()));
  return first;
}

MetricLabelDescriptor ContractLabel(std::string key, bool required = false, bool sensitive = false) {
  return {std::move(key), required, sensitive};
}

MetricDescriptor PageCacheContextDescriptor(std::string family,
                                            MetricType type,
                                            MetricUnit unit,
                                            std::string help) {
  MetricDescriptor descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.storage.pages.cache";
  descriptor.help = std::move(help);
  descriptor.producer_owner = "storage_page";
  descriptor.security_family = "OBS_METRICS_READ_FAMILY";
  descriptor.visibility = MetricVisibilityScope::family;
  descriptor.readiness = MetricReadiness::implemented;
  descriptor.labels = {ContractLabel("component", true),
                       ContractLabel("database_uuid", true),
                       ContractLabel("filespace_uuid", true),
                       ContractLabel("page_family", true),
                       ContractLabel("context", true),
                       ContractLabel("result", true),
                       ContractLabel("reason", true)};
  return descriptor;
}

void EnsurePageCacheContextMetricDescriptors() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = DefaultMetricRegistry();
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_resident_pages",
        MetricType::gauge,
        MetricUnit::count,
        "Resident page-cache pages by IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_resident_bytes",
        MetricType::gauge,
        MetricUnit::bytes,
        "Resident page-cache bytes by IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_pinned_pages",
        MetricType::gauge,
        MetricUnit::count,
        "Pinned page-cache pages by IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_dirty_pages",
        MetricType::gauge,
        MetricUnit::count,
        "Dirty page-cache pages by IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_admissions_total",
        MetricType::counter,
        MetricUnit::count,
        "Page-cache admissions by IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_reuses_total",
        MetricType::counter,
        MetricUnit::count,
        "Page-cache ring or slot reuses by IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_evictions_total",
        MetricType::counter,
        MetricUnit::count,
        "Page-cache evictions by owning IO context."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_protected_normal_hot_skips_total",
        MetricType::counter,
        MetricUnit::count,
        "Normal hot page eviction skips caused by scan-resistant IO contexts."));
    (void)registry.RegisterDescriptor(PageCacheContextDescriptor(
        "sb_page_cache_context_refusals_total",
        MetricType::counter,
        MetricUnit::count,
        "Page-cache scan-lane admissions refused by bounded ring or budget pressure."));
  });
}

}  // namespace

std::vector<MetricProducerContractStatus> MetricProducerContractsForOwner(const std::string& producer_owner) {
  EnsurePageCacheContextMetricDescriptors();
  std::vector<MetricProducerContractStatus> contracts;
  for (const auto& descriptor : DefaultMetricRegistry().Descriptors(true)) {
    if (descriptor.producer_owner != producer_owner) {
      continue;
    }
    contracts.push_back({descriptor.family,
                         descriptor.producer_owner,
                         descriptor.readiness,
                         descriptor.cluster_only,
                         descriptor.readiness != MetricReadiness::contract_ready_unwired});
  }
  return contracts;
}

MetricValidationResult PublishIdentitySessionsActive(double active_sessions,
                                                     std::string provider_family,
                                                     std::string visibility_scope,
                                                     MetricLabelSet labels) {
  auto status = RequireNonEmpty(provider_family, "provider_family");
  if (!status.ok) { return status; }
  return SetGauge("sb_identity_sessions_active",
                  MergeLabels(Labels({{"component", "security.session"},
                                      {"provider_family", std::move(provider_family)},
                                      {"visibility_scope", visibility_scope.empty() ? "self" : std::move(visibility_scope)}}),
                              std::move(labels)),
                  active_sessions,
                  "security_session");
}

MetricValidationResult PublishIdentityUsersOnline(double active_users,
                                                  std::string provider_family,
                                                  MetricLabelSet labels) {
  auto status = RequireNonEmpty(provider_family, "provider_family");
  if (!status.ok) { return status; }
  return SetGauge("sb_identity_users_online",
                  MergeLabels(Labels({{"component", "security.session"}, {"provider_family", std::move(provider_family)}}),
                              std::move(labels)),
                  active_users,
                  "security_session");
}

MetricValidationResult PublishParserSessionsActive(double active_sessions,
                                                   std::string parser_family,
                                                   std::string interface_family) {
  return SetGauge("sb_parser_sessions_active",
                  Labels({{"component", "parser.supervisor"}, {"parser_family", std::move(parser_family)},
                          {"interface", std::move(interface_family)}}),
                  active_sessions,
                  "parser_listener");
}

MetricValidationResult RecordParserFailure(std::string parser_family, std::string reason) {
  return IncrementCounter("sb_parser_failures_total",
                          Labels({{"component", "parser.supervisor"}, {"parser_family", std::move(parser_family)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "parser_listener");
}

MetricValidationResult RecordParserCrash(std::string parser_family, std::string reason) {
  return IncrementCounter("sb_parser_crashes_total",
                          Labels({{"component", "parser.supervisor"}, {"parser_family", std::move(parser_family)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "parser_listener");
}

MetricValidationResult ObserveParserPolicyAttachLatency(double latency_microseconds,
                                                        std::string parser_family,
                                                        std::string result) {
  return ObserveHistogram("sb_parser_policy_attach_latency_microseconds",
                          Labels({{"component", "parser.supervisor"}, {"parser_family", std::move(parser_family)},
                                  {"result", std::move(result)}}),
                          latency_microseconds,
                          "parser_listener");
}

MetricValidationResult PublishListenerSessionsActive(double active_sessions,
                                                     std::string listener_family,
                                                     std::string interface_family) {
  return SetGauge("sb_listener_sessions_active",
                  Labels({{"component", "listener.runtime"}, {"listener_family", std::move(listener_family)},
                          {"interface_family", std::move(interface_family)}}),
                  active_sessions,
                  "listener");
}

MetricValidationResult RecordListenerReject(std::string listener_family, std::string reason) {
  return IncrementCounter("sb_listener_rejects_total",
                          Labels({{"component", "listener.runtime"}, {"listener_family", std::move(listener_family)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "listener");
}

MetricValidationResult ObserveListenerQueueDelay(double delay_microseconds,
                                                 std::string listener_family,
                                                 std::string result) {
  return ObserveHistogram("sb_listener_queue_delay_microseconds",
                          Labels({{"component", "listener.runtime"}, {"listener_family", std::move(listener_family)},
                                  {"result", std::move(result)}}),
                          delay_microseconds,
                          "listener");
}

MetricValidationResult RecordManagementFrontendRequest(std::string request_class, std::string result) {
  return IncrementCounter("sb_management_frontend_requests_total",
                          Labels({{"component", "management.frontend"}, {"request_class", std::move(request_class)},
                                  {"result", std::move(result)}}),
                          1.0,
                          "management_frontend");
}

MetricValidationResult ObserveManagementFrontendLatency(double latency_microseconds,
                                                       std::string request_class,
                                                       std::string result) {
  return ObserveHistogram("sb_management_frontend_latency_microseconds",
                          Labels({{"component", "management.frontend"}, {"request_class", std::move(request_class)},
                                  {"result", std::move(result)}}),
                          latency_microseconds,
                          "management_frontend");
}

MetricValidationResult ObserveIndexLookupLatency(double latency_microseconds,
                                                 std::string index_kind,
                                                 std::string operation,
                                                 std::string result) {
  return ObserveHistogram("sb_index_lookup_latency_microseconds",
                          Labels({{"component", "index.runtime"}, {"index_kind", std::move(index_kind)},
                                  {"operation", std::move(operation)}, {"result", std::move(result)}}),
                          latency_microseconds,
                          "index_runtime");
}

MetricValidationResult RecordIndexSplit(std::string index_kind, std::string reason) {
  return IncrementCounter("sb_index_splits_total",
                          Labels({{"component", "index.runtime"}, {"index_kind", std::move(index_kind)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "index_runtime");
}

MetricValidationResult PublishIndexReadAmplificationRatio(double ratio, std::string index_kind) {
  return SetGauge("sb_index_read_amplification_ratio",
                  Labels({{"component", "index.runtime"}, {"index_kind", std::move(index_kind)}}),
                  ratio,
                  "index_runtime");
}

MetricValidationResult RecordDatatypeOperation(std::string canonical_type,
                                               std::string operation,
                                               std::string result,
                                               std::string reason) {
  auto status = RequireNonEmpty(canonical_type, "canonical_type");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_datatype_operation_total",
                          Labels({{"component", "datatype.runtime"},
                                  {"canonical_type", std::move(canonical_type)},
                                  {"operation", std::move(operation)},
                                  {"result", std::move(result)},
                                  {"reason", reason.empty() ? "none" : std::move(reason)}}),
                          1.0,
                          "datatype_runtime");
}

MetricValidationResult RecordDatatypeCast(std::string source_type,
                                           std::string target_type,
                                           std::string result,
                                           std::string reason) {
  auto status = RequireNonEmpty(source_type, "source_type");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(target_type, "target_type");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_datatype_cast_total",
                          Labels({{"component", "datatype.runtime"},
                                  {"source_type", std::move(source_type)},
                                  {"target_type", std::move(target_type)},
                                  {"result", std::move(result)},
                                  {"reason", reason.empty() ? "none" : std::move(reason)}}),
                          1.0,
                          "datatype_runtime");
}

MetricValidationResult RecordDatatypeNumericBackend(std::string numeric_backend,
                                                    std::string canonical_type,
                                                    std::string operation,
                                                    std::string result,
                                                    std::string reason) {
  auto status = RequireNonEmpty(numeric_backend, "numeric_backend");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(canonical_type, "canonical_type");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_datatype_numeric_backend_total",
                          Labels({{"component", "datatype.runtime"},
                                  {"numeric_backend", std::move(numeric_backend)},
                                  {"canonical_type", std::move(canonical_type)},
                                  {"operation", std::move(operation)},
                                  {"result", std::move(result)},
                                  {"reason", reason.empty() ? "none" : std::move(reason)}}),
                          1.0,
                          "datatype_runtime");
}

MetricValidationResult PublishDatatypeCatalogDescriptorCount(double descriptor_count,
                                                             std::string result) {
  return SetGauge("sb_datatype_catalog_descriptors",
                  Labels({{"component", "datatype.catalog"}, {"result", std::move(result)}}),
                  descriptor_count,
                  "datatype_runtime");
}

MetricValidationResult RecordDomainMethodInvocation(std::string domain_uuid,
                                                    std::string method,
                                                    std::string result,
                                                    std::string reason) {
  auto status = RequireNonEmpty(domain_uuid, "domain_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(method, "method");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_domain_method_invocation_total",
                          Labels({{"component", "datatype.domain"},
                                  {"domain_uuid", std::move(domain_uuid)},
                                  {"method", std::move(method)},
                                  {"result", std::move(result)},
                                  {"reason", reason.empty() ? "none" : std::move(reason)}}),
                          1.0,
                          "datatype_runtime");
}

MetricValidationResult RecordInsertBatchStarted(std::string object_uuid,
                                                std::string insert_mode,
                                                std::string result) {
  return IncrementCounter("sb_dml_insert_batch_started_total",
                          Labels({{"component", "engine.insert"},
                                  {"object_uuid", std::move(object_uuid)},
                                  {"operation", std::move(insert_mode)},
                                  {"result", std::move(result)}}),
                          1.0,
                          "engine_insert");
}

MetricValidationResult RecordInsertBatchFallback(std::string object_uuid,
                                                 std::string insert_mode,
                                                 std::string reason) {
  auto status = IncrementCounter("sb_dml_insert_batch_fallback_total",
                                 Labels({{"component", "engine.insert"},
                                         {"object_uuid", object_uuid},
                                         {"operation", insert_mode},
                                         {"result", "fallback"},
                                         {"reason", reason}}),
                                 1.0,
                                 "engine_insert");
  if (!status.ok) {
    return status;
  }
  return IncrementCounter("sb_dml_insert_batch_fallback_reason_total",
                          Labels({{"component", "engine.insert"},
                                  {"object_uuid", std::move(object_uuid)},
                                  {"operation", std::move(insert_mode)},
                                  {"result", "fallback"},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "engine_insert");
}

MetricValidationResult RecordInsertRowsInserted(double rows,
                                                std::string object_uuid,
                                                std::string insert_mode) {
  return IncrementCounter("sb_dml_insert_rows_inserted_total",
                          Labels({{"component", "engine.insert"},
                                  {"object_uuid", std::move(object_uuid)},
                                  {"operation", std::move(insert_mode)},
                                  {"result", "ok"}}),
                          rows,
                          "engine_insert");
}

MetricValidationResult ObserveInsertRowsPerBatch(double rows,
                                                 std::string object_uuid,
                                                 std::string insert_mode) {
  return ObserveHistogram("sb_dml_insert_rows_per_batch",
                          Labels({{"component", "engine.insert"},
                                  {"object_uuid", std::move(object_uuid)},
                                  {"operation", std::move(insert_mode)},
                                  {"result", "ok"}}),
                          rows,
                          "engine_insert");
}

MetricValidationResult RecordInsertTraceEvent(std::string object_uuid,
                                              std::string insert_mode,
                                              std::string phase) {
  return IncrementCounter("sb_dml_insert_trace_event_total",
                          Labels({{"component", "engine.insert"},
                                  {"object_uuid", std::move(object_uuid)},
                                  {"operation", std::move(insert_mode)},
                                  {"result", "trace"},
                                  {"reason", std::move(phase)}}),
                          1.0,
                          "engine_insert");
}

MetricValidationResult RecordInsertCancel(std::string object_uuid,
                                          std::string insert_mode,
                                          std::string reason) {
  return IncrementCounter("sb_dml_insert_cancel_total",
                          Labels({{"component", "engine.insert"},
                                  {"object_uuid", std::move(object_uuid)},
                                  {"operation", std::move(insert_mode)},
                                  {"result", "cancelled"},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "engine_insert");
}

namespace {

MetricLabelSet FilespaceLabels(std::string database_uuid,
                               std::string filespace_uuid,
                               std::string node_uuid,
                               std::string filespace_role,
                               std::string device_class) {
  return Labels({{"component", "storage.filespace"},
                 {"database_uuid", std::move(database_uuid)},
                 {"filespace_uuid", std::move(filespace_uuid)},
                 {"node_uuid", std::move(node_uuid)},
                 {"filespace_role", std::move(filespace_role)},
                 {"device_class", std::move(device_class)}});
}

MetricLabelSet PageLabels(std::string database_uuid,
                          std::string filespace_uuid,
                          std::string node_uuid,
                          std::string page_family,
                          std::string page_type) {
  return Labels({{"component", "storage.page"},
                 {"database_uuid", std::move(database_uuid)},
                 {"filespace_uuid", std::move(filespace_uuid)},
                 {"node_uuid", std::move(node_uuid)},
                 {"page_family", std::move(page_family)},
                 {"page_type", std::move(page_type)}});
}

MetricValidationResult RequireFilespaceIdentity(const std::string& database_uuid,
                                                const std::string& filespace_uuid,
                                                const std::string& filespace_role) {
  auto status = RequireNonEmpty(database_uuid, "database_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return RequireNonEmpty(filespace_role, "filespace_role");
}

MetricValidationResult RequirePageIdentity(const std::string& database_uuid,
                                           const std::string& filespace_uuid,
                                           const std::string& page_family,
                                           const std::string& page_type) {
  auto status = RequireNonEmpty(database_uuid, "database_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(page_family, "page_family");
  if (!status.ok) { return status; }
  return RequireNonEmpty(page_type, "page_type");
}

}  // namespace

MetricValidationResult PublishFilespaceCapacitySnapshot(double total_bytes,
                                                        double used_bytes,
                                                        double free_bytes,
                                                        std::string database_uuid,
                                                        std::string filespace_uuid,
                                                        std::string node_uuid,
                                                        std::string filespace_role,
                                                        std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  auto labels = FilespaceLabels(database_uuid, filespace_uuid, std::move(node_uuid), filespace_role, std::move(device_class));
  status = SetGauge("sb_filespace_total_bytes", labels, total_bytes, "storage_filespace");
  if (!status.ok) { return status; }
  status = SetGauge("sb_filespace_used_bytes", labels, used_bytes, "storage_filespace");
  if (!status.ok) { return status; }
  return SetGauge("sb_filespace_free_bytes", std::move(labels), free_bytes, "storage_filespace");
}

MetricValidationResult PublishFilespaceReservedBytes(double reserved_bytes,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string node_uuid,
                                                     std::string filespace_role,
                                                     std::string device_class,
                                                     std::string reason_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  return SetGauge("sb_filespace_reserved_bytes",
                  MergeLabels(FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                              std::move(filespace_role), std::move(device_class)),
                              Labels({{"reason_class", std::move(reason_class)}})),
                  reserved_bytes,
                  "storage_filespace");
}

MetricValidationResult PublishFilespaceHealthState(double state_value,
                                                   std::string state_text,
                                                   std::string database_uuid,
                                                   std::string filespace_uuid,
                                                   std::string node_uuid,
                                                   std::string filespace_role,
                                                   std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  const std::string label_state = state_text.empty() ? "unknown" : state_text;
  return SetState("sb_filespace_health_state",
                  MergeLabels(FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                              std::move(filespace_role), std::move(device_class)),
                              Labels({{"state", label_state}})),
                  state_value,
                  label_state,
                  "storage_filespace");
}

MetricValidationResult PublishFilespaceRoleState(double state_value,
                                                 std::string state_text,
                                                 std::string database_uuid,
                                                 std::string filespace_uuid,
                                                 std::string node_uuid,
                                                 std::string filespace_role,
                                                 std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  const std::string label_state = state_text.empty() ? "unknown" : state_text;
  return SetState("sb_filespace_role_state",
                  MergeLabels(FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                              std::move(filespace_role), std::move(device_class)),
                              Labels({{"state", label_state}})),
                  state_value,
                  label_state,
                  "storage_filespace");
}

MetricValidationResult ObserveFilespaceDeviceReadLatency(double latency_microseconds,
                                                         std::string database_uuid,
                                                         std::string filespace_uuid,
                                                         std::string node_uuid,
                                                         std::string filespace_role,
                                                         std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_filespace_device_read_latency_microseconds",
                          FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                          std::move(filespace_role), std::move(device_class)),
                          latency_microseconds,
                          "storage_disk");
}

MetricValidationResult ObserveFilespaceDeviceWriteLatency(double latency_microseconds,
                                                          std::string database_uuid,
                                                          std::string filespace_uuid,
                                                          std::string node_uuid,
                                                          std::string filespace_role,
                                                          std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_filespace_device_write_latency_microseconds",
                          FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                          std::move(filespace_role), std::move(device_class)),
                          latency_microseconds,
                          "storage_disk");
}

MetricValidationResult ObserveFilespaceFsyncLatency(double latency_microseconds,
                                                    std::string database_uuid,
                                                    std::string filespace_uuid,
                                                    std::string node_uuid,
                                                    std::string filespace_role,
                                                    std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_filespace_fsync_latency_microseconds",
                          FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                          std::move(filespace_role), std::move(device_class)),
                          latency_microseconds,
                          "storage_disk");
}

MetricValidationResult RecordFilespaceDeviceError(std::string error_class,
                                                  std::string database_uuid,
                                                  std::string filespace_uuid,
                                                  std::string node_uuid,
                                                  std::string filespace_role,
                                                  std::string device_class) {
  auto status = RequireFilespaceIdentity(database_uuid, filespace_uuid, filespace_role);
  if (!status.ok) { return status; }
  return IncrementCounter("sb_filespace_device_error_total",
                          MergeLabels(FilespaceLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                                      std::move(filespace_role), std::move(device_class)),
                                      Labels({{"error_class", std::move(error_class)}})),
                          1.0,
                          "storage_disk");
}

MetricValidationResult PublishPageAllocationSnapshot(double free_count,
                                                     double allocated_count,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string node_uuid,
                                                     std::string page_family,
                                                     std::string page_type) {
  auto status = RequirePageIdentity(database_uuid, filespace_uuid, page_family, page_type);
  if (!status.ok) { return status; }
  auto labels = PageLabels(database_uuid, filespace_uuid, std::move(node_uuid), page_family, page_type);
  status = SetGauge("sb_page_free_count", labels, free_count, "storage_page");
  if (!status.ok) { return status; }
  return SetGauge("sb_page_allocated_count", std::move(labels), allocated_count, "storage_page");
}

MetricValidationResult PublishPageReleasedFreeCount(double released_free_count,
                                                    std::string database_uuid,
                                                    std::string filespace_uuid,
                                                    std::string node_uuid,
                                                    std::string page_family,
                                                    std::string page_type) {
  auto status = RequirePageIdentity(database_uuid, filespace_uuid, page_family, page_type);
  if (!status.ok) { return status; }
  return SetGauge("sb_page_released_free_count",
                  PageLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                             std::move(page_family), std::move(page_type)),
                  released_free_count,
                  "storage_page");
}

MetricValidationResult PublishPageReservedCount(double reserved_count,
                                                std::string database_uuid,
                                                std::string filespace_uuid,
                                                std::string node_uuid,
                                                std::string page_family,
                                                std::string page_type,
                                                std::string reason_class) {
  auto status = RequirePageIdentity(database_uuid, filespace_uuid, page_family, page_type);
  if (!status.ok) { return status; }
  return SetGauge("sb_page_reserved_count",
                  MergeLabels(PageLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                         std::move(page_family), std::move(page_type)),
                              Labels({{"reason_class", std::move(reason_class)}})),
                  reserved_count,
                  "storage_page");
}

MetricValidationResult ObservePageAllocationLatency(double latency_microseconds,
                                                    std::string database_uuid,
                                                    std::string filespace_uuid,
                                                    std::string node_uuid,
                                                    std::string page_family,
                                                    std::string page_type) {
  auto status = RequirePageIdentity(database_uuid, filespace_uuid, page_family, page_type);
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_page_allocation_latency_microseconds",
                          PageLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                     std::move(page_family), std::move(page_type)),
                          latency_microseconds,
                          "storage_page");
}

MetricValidationResult RecordPageAllocationFailure(std::string error_class,
                                                   std::string database_uuid,
                                                   std::string filespace_uuid,
                                                   std::string node_uuid,
                                                   std::string page_family,
                                                   std::string page_type) {
  auto status = RequirePageIdentity(database_uuid, filespace_uuid, page_family, page_type);
  if (!status.ok) { return status; }
  return IncrementCounter("sb_page_allocation_failures_total",
                          MergeLabels(PageLabels(std::move(database_uuid), std::move(filespace_uuid), std::move(node_uuid),
                                                 std::move(page_family), std::move(page_type)),
                                      Labels({{"error_class", std::move(error_class)}})),
                          1.0,
                          "storage_page");
}

MetricValidationResult PublishPageCacheSnapshot(double resident_pages,
                                                double resident_bytes,
                                                double pinned_pages,
                                                double dirty_pages,
                                                std::string database_uuid,
                                                std::string filespace_uuid,
                                                std::string page_family) {
  auto status = RequireNonEmpty(database_uuid, "database_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(page_family, "page_family");
  if (!status.ok) { return status; }
  auto labels = Labels({{"component", "storage.page_cache"},
                        {"database_uuid", std::move(database_uuid)},
                        {"filespace_uuid", std::move(filespace_uuid)},
                        {"page_family", std::move(page_family)}});
  status = SetGauge("sb_page_cache_resident_pages", labels, resident_pages, "storage_page");
  if (!status.ok) { return status; }
  status = SetGauge("sb_page_cache_resident_bytes", labels, resident_bytes, "storage_page");
  if (!status.ok) { return status; }
  status = SetGauge("sb_page_cache_pinned_pages", labels, pinned_pages, "storage_page");
  if (!status.ok) { return status; }
  return SetGauge("sb_page_cache_dirty_pages", std::move(labels), dirty_pages, "storage_page");
}

MetricValidationResult RecordPageCacheEviction(std::string database_uuid,
                                               std::string filespace_uuid,
                                               std::string page_family,
                                               std::string result) {
  auto status = RequireNonEmpty(database_uuid, "database_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(page_family, "page_family");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_page_cache_evictions_total",
                          Labels({{"component", "storage.page_cache"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"filespace_uuid", std::move(filespace_uuid)},
                                  {"page_family", std::move(page_family)},
                                  {"result", std::move(result)}}),
                          1.0,
                          "storage_page");
}

namespace {

MetricValidationResult RequirePageCacheContextIdentity(const std::string& database_uuid,
                                                       const std::string& filespace_uuid,
                                                       const std::string& page_family,
                                                       const std::string& context,
                                                       const std::string& result,
                                                       const std::string& reason) {
  auto status = RequireNonEmpty(database_uuid, "database_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(page_family, "page_family");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(context, "context");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(result, "result");
  if (!status.ok) { return status; }
  return RequireNonEmpty(reason, "reason");
}

MetricLabelSet PageCacheContextLabels(std::string database_uuid,
                                      std::string filespace_uuid,
                                      std::string page_family,
                                      std::string context,
                                      std::string result,
                                      std::string reason) {
  return Labels({{"component", "storage.page_cache"},
                 {"database_uuid", std::move(database_uuid)},
                 {"filespace_uuid", std::move(filespace_uuid)},
                 {"page_family", std::move(page_family)},
                 {"context", std::move(context)},
                 {"result", std::move(result)},
                 {"reason", std::move(reason)}});
}

MetricValidationResult RecordPageCacheContextCounter(const std::string& family,
                                                     double count,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string page_family,
                                                     std::string context,
                                                     std::string result,
                                                     std::string reason) {
  EnsurePageCacheContextMetricDescriptors();
  if (count == 0.0) {
    return MetricOk();
  }
  auto status = RequireRange(family, count, 0.0, 1.0e18);
  if (!status.ok) { return status; }
  status = RequirePageCacheContextIdentity(database_uuid, filespace_uuid, page_family, context, result, reason);
  if (!status.ok) { return status; }
  return IncrementCounter(family,
                          PageCacheContextLabels(std::move(database_uuid),
                                                 std::move(filespace_uuid),
                                                 std::move(page_family),
                                                 std::move(context),
                                                 std::move(result),
                                                 std::move(reason)),
                          count,
                          "storage_page");
}

}  // namespace

MetricValidationResult PublishPageCacheContextSnapshot(double resident_pages,
                                                       double resident_bytes,
                                                       double pinned_pages,
                                                       double dirty_pages,
                                                       std::string database_uuid,
                                                       std::string filespace_uuid,
                                                       std::string page_family,
                                                       std::string context,
                                                       std::string result,
                                                       std::string reason) {
  EnsurePageCacheContextMetricDescriptors();
  auto status = RequirePageCacheContextIdentity(database_uuid, filespace_uuid, page_family, context, result, reason);
  if (!status.ok) { return status; }
  auto labels = PageCacheContextLabels(database_uuid,
                                      filespace_uuid,
                                      page_family,
                                      context,
                                      result,
                                      reason);
  status = SetGauge("sb_page_cache_context_resident_pages", labels, resident_pages, "storage_page");
  if (!status.ok) { return status; }
  status = SetGauge("sb_page_cache_context_resident_bytes", labels, resident_bytes, "storage_page");
  if (!status.ok) { return status; }
  status = SetGauge("sb_page_cache_context_pinned_pages", labels, pinned_pages, "storage_page");
  if (!status.ok) { return status; }
  return SetGauge("sb_page_cache_context_dirty_pages", std::move(labels), dirty_pages, "storage_page");
}

MetricValidationResult RecordPageCacheContextAdmission(double admissions,
                                                       std::string database_uuid,
                                                       std::string filespace_uuid,
                                                       std::string page_family,
                                                       std::string context,
                                                       std::string result,
                                                       std::string reason) {
  return RecordPageCacheContextCounter("sb_page_cache_context_admissions_total",
                                       admissions,
                                       std::move(database_uuid),
                                       std::move(filespace_uuid),
                                       std::move(page_family),
                                       std::move(context),
                                       std::move(result),
                                       std::move(reason));
}

MetricValidationResult RecordPageCacheContextReuse(double reuses,
                                                   std::string database_uuid,
                                                   std::string filespace_uuid,
                                                   std::string page_family,
                                                   std::string context,
                                                   std::string result,
                                                   std::string reason) {
  return RecordPageCacheContextCounter("sb_page_cache_context_reuses_total",
                                       reuses,
                                       std::move(database_uuid),
                                       std::move(filespace_uuid),
                                       std::move(page_family),
                                       std::move(context),
                                       std::move(result),
                                       std::move(reason));
}

MetricValidationResult RecordPageCacheContextEviction(double evictions,
                                                      std::string database_uuid,
                                                      std::string filespace_uuid,
                                                      std::string page_family,
                                                      std::string context,
                                                      std::string result,
                                                      std::string reason) {
  return RecordPageCacheContextCounter("sb_page_cache_context_evictions_total",
                                       evictions,
                                       std::move(database_uuid),
                                       std::move(filespace_uuid),
                                       std::move(page_family),
                                       std::move(context),
                                       std::move(result),
                                       std::move(reason));
}

MetricValidationResult RecordPageCacheContextProtectedNormalHotSkip(double skips,
                                                                    std::string database_uuid,
                                                                    std::string filespace_uuid,
                                                                    std::string page_family,
                                                                    std::string context,
                                                                    std::string result,
                                                                    std::string reason) {
  return RecordPageCacheContextCounter("sb_page_cache_context_protected_normal_hot_skips_total",
                                       skips,
                                       std::move(database_uuid),
                                       std::move(filespace_uuid),
                                       std::move(page_family),
                                       std::move(context),
                                       std::move(result),
                                       std::move(reason));
}

MetricValidationResult RecordPageCacheContextRefusal(double refusals,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string page_family,
                                                     std::string context,
                                                     std::string result,
                                                     std::string reason) {
  return RecordPageCacheContextCounter("sb_page_cache_context_refusals_total",
                                       refusals,
                                       std::move(database_uuid),
                                       std::move(filespace_uuid),
                                       std::move(page_family),
                                       std::move(context),
                                       std::move(result),
                                       std::move(reason));
}

MetricValidationResult PublishOptimizerPlanEstimateErrorRatio(double ratio,
                                                              std::string operator_family,
                                                              std::string plan_shape) {
  return SetGauge("sb_optimizer_plan_estimate_error_ratio",
                  Labels({{"component", "optimizer.feedback"}, {"operator_family", std::move(operator_family)},
                          {"plan_shape", std::move(plan_shape)}}),
                  ratio,
                  "optimizer_executor_feedback");
}

MetricValidationResult PublishOptimizerRuntimeFeedbackSample(
    const OptimizerRuntimeFeedbackMetricSample& sample,
    std::string operator_family,
    std::string plan_shape) {
  auto status = RequireNonEmpty(operator_family, "operator_family");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(plan_shape, "plan_shape");
  if (!status.ok) { return status; }

  const auto labels = Labels({{"component", "optimizer.feedback"},
                              {"operator_family", std::move(operator_family)},
                              {"plan_shape", std::move(plan_shape)}});
  const auto publish = [&](const std::string& family, double value) {
    return SetGauge(family, labels, value, "optimizer_executor_feedback");
  };

  status = publish("sb_optimizer_feedback_estimated_rows", sample.estimated_rows);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_actual_rows", sample.actual_rows);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_estimated_pages", sample.estimated_pages);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_actual_pages", sample.actual_pages);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_estimated_io_operations", sample.estimated_io_operations);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_actual_io_operations", sample.actual_io_operations);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_estimated_visibility_recheck_rows",
                   sample.estimated_visibility_recheck_rows);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_actual_visibility_recheck_rows",
                   sample.actual_visibility_recheck_rows);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_estimated_spill_bytes", sample.estimated_spill_bytes);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_actual_spill_bytes", sample.actual_spill_bytes);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_memory_grant_bytes", sample.memory_grant_bytes);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_peak_memory_bytes", sample.peak_memory_bytes);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_recommended_memory_grant_bytes",
                   sample.recommended_memory_grant_bytes);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_estimated_latency_microseconds",
                   sample.estimated_latency_microseconds);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_actual_latency_microseconds",
                   sample.actual_latency_microseconds);
  if (!status.ok) { return status; }
  status = publish("sb_optimizer_feedback_estimated_resource_units", sample.estimated_resource_units);
  if (!status.ok) { return status; }
  return publish("sb_optimizer_feedback_actual_resource_units", sample.actual_resource_units);
}

std::vector<std::string> LockLatchContentionRequiredWaitClasses() {
  return {"page_latch",
          "index_leaf_split",
          "index_rightmost_leaf",
          "buffer_pin",
          "descriptor_cache_lock",
          "delta_ledger_merge",
          "background_agent_interference",
          "ipc_queue"};
}

MetricValidationResult RecordLockLatchContentionWait(
    const LockLatchContentionSample& sample) {
  auto status = RequireNonEmpty(sample.subsystem, "subsystem");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(sample.wait_class, "wait_class");
  if (!status.ok) { return status; }
  status = RequireNonEmpty(sample.evidence_surface, "evidence_surface");
  if (!status.ok) { return status; }
  status = RequireRange("sb_lock_latch_contention_wait_total", sample.wait_count, 0.0, 1.0e18);
  if (!status.ok) { return status; }
  status = RequireRange("sb_lock_latch_contention_wait_microseconds",
                        sample.wait_microseconds,
                        0.0,
                        1.0e18);
  if (!status.ok) { return status; }

  const auto required = LockLatchContentionRequiredWaitClasses();
  if (std::find(required.begin(), required.end(), sample.wait_class) == required.end()) {
    return MetricError("SB-METRICS-CONTRACT-WAIT-CLASS-UNKNOWN", sample.wait_class);
  }

  const auto labels = Labels({{"component", "runtime.contention"},
                              {"subsystem", sample.subsystem},
                              {"wait_class", sample.wait_class},
                              {"evidence_surface", sample.evidence_surface}});
  status = IncrementCounter("sb_lock_latch_contention_wait_total",
                            labels,
                            sample.wait_count,
                            "runtime_contention");
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_lock_latch_contention_wait_microseconds",
                          labels,
                          sample.wait_microseconds,
                          "runtime_contention");
}

MetricValidationResult ObserveRemoteFragmentLatency(double latency_microseconds,
                                                    std::string fragment_kind,
                                                    std::string route_class) {
  return ObserveHistogram("sb_optimizer_remote_fragment_latency_microseconds",
                          Labels({{"component", "distributed.query"}, {"fragment_kind", std::move(fragment_kind)},
                                  {"route_class", std::move(route_class)}}),
                          latency_microseconds,
                          "distributed_query");
}

MetricValidationResult ObserveQueryFragmentQueueDelay(double delay_microseconds,
                                                      std::string fragment_kind,
                                                      std::string route_class) {
  return ObserveHistogram("sb_query_fragment_queue_delay_microseconds",
                          Labels({{"component", "distributed.query"}, {"fragment_kind", std::move(fragment_kind)},
                                  {"route_class", std::move(route_class)}}),
                          delay_microseconds,
                          "distributed_query");
}

MetricValidationResult ObserveQueryFragmentPropagationDelay(double delay_microseconds,
                                                            std::string fragment_kind,
                                                            std::string route_class) {
  return ObserveHistogram("sb_query_fragment_propagation_delay_microseconds",
                          Labels({{"component", "distributed.query"}, {"fragment_kind", std::move(fragment_kind)},
                                  {"route_class", std::move(route_class)}}),
                          delay_microseconds,
                          "distributed_query");
}

MetricValidationResult ObserveQueryFragmentLocalConnectionDelay(double delay_microseconds,
                                                               std::string fragment_kind,
                                                               std::string route_class) {
  return ObserveHistogram("sb_query_fragment_local_connection_delay_microseconds",
                          Labels({{"component", "distributed.query"}, {"fragment_kind", std::move(fragment_kind)},
                                  {"route_class", std::move(route_class)}}),
                          delay_microseconds,
                          "distributed_query");
}

MetricValidationResult PublishQueryFragmentSampleFreshness(double freshness_microseconds,
                                                           std::string fragment_kind,
                                                           std::string route_class) {
  return SetGauge("sb_query_fragment_sample_freshness_microseconds",
                  Labels({{"component", "distributed.query"}, {"fragment_kind", std::move(fragment_kind)},
                          {"route_class", std::move(route_class)}}),
                  freshness_microseconds,
                  "distributed_query");
}

MetricValidationResult PublishArchiveLagBytes(double lag_bytes, std::string archive_class, std::string reason_class) {
  return SetGauge("sb_archive_lag_bytes",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  lag_bytes,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveSliceCount(double slice_count, std::string archive_class, std::string reason_class) {
  return SetGauge("sb_archive_slice_count",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  slice_count,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveSliceBytes(double slice_bytes, std::string archive_class, std::string reason_class) {
  return SetGauge("sb_archive_slice_bytes",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  slice_bytes,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveSliceAge(double age_microseconds, std::string archive_class, std::string reason_class) {
  return SetGauge("sb_archive_slice_age_microseconds",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  age_microseconds,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveSliceMaxAge(double max_age_microseconds, std::string archive_class) {
  return SetGauge("sb_archive_slice_max_age_microseconds",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)}}),
                  max_age_microseconds,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveHealthState(double state_value, std::string state_text, std::string archive_class) {
  return SetState("sb_archive_health_state",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"state", state_text.empty() ? "unknown" : state_text}}),
                  state_value,
                  state_text.empty() ? "unknown" : std::move(state_text),
                  "archive_runtime");
}

MetricValidationResult PublishArchiveQueueDepth(double queue_depth, std::string archive_class, std::string reason_class) {
  return SetGauge("sb_archive_queue_depth",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  queue_depth,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveDeltaLagTransactions(double lag_transactions,
                                                          std::string archive_class,
                                                          std::string reason_class) {
  return SetGauge("sb_archive_delta_lag_transactions",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  lag_transactions,
                  "archive_runtime");
}

MetricValidationResult PublishArchiveDeltaApplyLagTransactions(double lag_transactions,
                                                               std::string archive_class,
                                                               std::string reason_class) {
  return SetGauge("sb_archive_delta_apply_lag_transactions",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                          {"reason_class", std::move(reason_class)}}),
                  lag_transactions,
                  "archive_runtime");
}

MetricValidationResult RecordArchiveChecksumFailure(std::string archive_class, std::string reason_class) {
  return IncrementCounter("sb_archive_checksum_failures_total",
                          Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                                  {"reason_class", std::move(reason_class)}}),
                          1.0,
                          "archive_runtime");
}

MetricValidationResult RecordArchiveRestoreRefusal(std::string archive_class, std::string reason_class) {
  return IncrementCounter("sb_archive_restore_refusals_total",
                          Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)},
                                  {"reason_class", std::move(reason_class)}}),
                          1.0,
                          "archive_runtime");
}

MetricValidationResult PublishBackupInProgress(double in_progress, std::string operation) {
  return SetGauge("sb_backup_in_progress",
                  Labels({{"component", "backup.runtime"}, {"operation", std::move(operation)}}),
                  in_progress,
                  "backup_runtime");
}

MetricValidationResult PublishBackupProgressPercent(double progress_percent, std::string operation) {
  auto status = RequireRange("sb_backup_progress_percent", progress_percent, 0.0, 100.0);
  if (!status.ok) { return status; }
  return SetGauge("sb_backup_progress_percent",
                  Labels({{"component", "backup.runtime"}, {"operation", std::move(operation)}}),
                  progress_percent,
                  "backup_runtime");
}

MetricValidationResult ObserveRestoreDrillDuration(double duration_microseconds, std::string result) {
  return ObserveHistogram("sb_restore_drill_duration_microseconds",
                          Labels({{"component", "backup.runtime"}, {"result", std::move(result)}}),
                          duration_microseconds,
                          "backup_runtime");
}

MetricValidationResult PublishPitrWindowAvailableSeconds(double available_seconds, std::string archive_class) {
  return SetGauge("sb_pitr_window_available_seconds",
                  Labels({{"component", "archive.runtime"}, {"archive_class", std::move(archive_class)}}),
                  available_seconds,
                  "archive_runtime");
}

MetricValidationResult PublishSchedulerQueueDepth(double queue_depth, std::string scheduler_class) {
  return SetGauge("sb_scheduler_queue_depth",
                  Labels({{"component", "scheduler.runtime"}, {"object_kind", std::move(scheduler_class)}}),
                  queue_depth,
                  "scheduler_runtime");
}

MetricValidationResult RecordJobControlAction(std::string action_class, std::string result) {
  return IncrementCounter("sb_job_control_actions_total",
                          Labels({{"component", "job.runtime"}, {"action_class", std::move(action_class)},
                                  {"result", std::move(result)}}),
                          1.0,
                          "job_runtime");
}

MetricValidationResult PublishClusterNodeCpuFeatureAvailable(std::string feature, bool available) {
  return SetState("sb_cluster_node_cpu_feature_available",
                  Labels({{"component", "cluster.node"}, {"object_kind", std::move(feature)}}),
                  available ? 1.0 : 0.0,
                  available ? "available" : "unavailable",
                  "cluster_node");
}

MetricValidationResult PublishClusterNodeRoleState(double state_value, std::string state_text, std::string node_class) {
  return SetState("sb_cluster_node_role_state",
                  Labels({{"component", "cluster.node"}, {"node_class", std::move(node_class)},
                          {"state", state_text.empty() ? "unknown" : state_text}}),
                  state_value,
                  state_text.empty() ? "unknown" : std::move(state_text),
                  "cluster_node");
}

MetricValidationResult PublishClusterNodeSaturationRatio(double ratio, std::string node_class, std::string resource_class) {
  return SetGauge("sb_cluster_node_saturation_ratio",
                  Labels({{"component", "cluster.node"}, {"node_class", std::move(node_class)},
                          {"resource_class", std::move(resource_class)}}),
                  ratio,
                  "cluster_node");
}

MetricValidationResult RecordClusterAdmissionDenied(std::string deny_reason, std::string workload_class) {
  return IncrementCounter("sb_cluster_admission_denied_total",
                          Labels({{"component", "cluster.admission"}, {"deny_reason", std::move(deny_reason)},
                                  {"workload_class", std::move(workload_class)}}),
                          1.0,
                          "cluster_admission");
}

MetricValidationResult PublishClusterLimboTransactions(double count) {
  return SetGauge("sb_cluster_limbo_transactions",
                  Labels({{"component", "cluster.transaction"}}),
                  count,
                  "cluster_transaction");
}

MetricValidationResult PublishClusterRollingUpgradeReadiness(double state_value, std::string state_text) {
  return SetState("sb_cluster_rolling_upgrade_readiness_state",
                  Labels({{"component", "cluster.lifecycle"}, {"state", state_text.empty() ? "unknown" : state_text}}),
                  state_value,
                  state_text.empty() ? "unknown" : std::move(state_text),
                  "cluster_lifecycle");
}

MetricValidationResult PublishClusterSchedulerQueueDepth(double queue_depth, std::string scheduler_class) {
  return SetGauge("sb_cluster_scheduler_queue_depth",
                  Labels({{"component", "cluster.scheduler"}, {"object_kind", std::move(scheduler_class)}}),
                  queue_depth,
                  "cluster_scheduler");
}

// SEARCH_KEY: SB_CLUSTER_INSERT_METRIC_CONTRACTS
MetricValidationResult RecordClusterInsertRouteCheck(std::string database_uuid,
                                                     std::string table_uuid,
                                                     std::string route_epoch,
                                                     std::string result,
                                                     std::string reason) {
  return IncrementCounter("sb_cluster_insert_route_checks_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"route_epoch", std::move(route_epoch)},
                                  {"result", std::move(result)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "cluster_insert");
}

MetricValidationResult RecordClusterInsertStaleRouteRejection(std::string database_uuid,
                                                              std::string table_uuid,
                                                              std::string route_epoch,
                                                              std::string owner_node_uuid) {
  return IncrementCounter("sb_cluster_insert_stale_route_rejections_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"route_epoch", std::move(route_epoch)},
                                  {"owner_node_uuid", std::move(owner_node_uuid)}}),
                          1.0,
                          "cluster_insert");
}

MetricValidationResult RecordClusterInsertParticipantAdmission(std::string database_uuid,
                                                               std::string table_uuid,
                                                               std::string participant_node_uuid,
                                                               std::string result,
                                                               std::string reason) {
  return IncrementCounter("sb_cluster_insert_participant_admissions_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"participant_node_uuid", std::move(participant_node_uuid)},
                                  {"result", std::move(result)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "cluster_insert");
}

MetricValidationResult RecordClusterInsertRemoteRequest(std::string database_uuid,
                                                        std::string table_uuid,
                                                        std::string participant_node_uuid,
                                                        std::string result,
                                                        std::string retry_class) {
  return IncrementCounter("sb_cluster_insert_remote_requests_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"participant_node_uuid", std::move(participant_node_uuid)},
                                  {"result", std::move(result)},
                                  {"retry_class", std::move(retry_class)}}),
                          1.0,
                          "cluster_insert");
}

MetricValidationResult ObserveClusterInsertFinalityWait(double latency_microseconds,
                                                        std::string database_uuid,
                                                        std::string table_uuid,
                                                        std::string participant_count,
                                                        std::string result) {
  return ObserveHistogram("sb_cluster_insert_finality_wait_microseconds",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"participant_count", std::move(participant_count)},
                                  {"result", std::move(result)}}),
                          latency_microseconds,
                          "cluster_insert");
}

MetricValidationResult RecordClusterInsertRowsMutated(double rows,
                                                      std::string database_uuid,
                                                      std::string table_uuid,
                                                      std::string participant_node_uuid,
                                                      std::string insert_mode) {
  return IncrementCounter("sb_cluster_insert_rows_mutated_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"participant_node_uuid", std::move(participant_node_uuid)},
                                  {"insert_mode", std::move(insert_mode)}}),
                          rows,
                          "cluster_insert");
}

MetricValidationResult RecordClusterInsertFailClosed(std::string database_uuid,
                                                     std::string table_uuid,
                                                     std::string authority_family,
                                                     std::string reason) {
  return IncrementCounter("sb_cluster_insert_fail_closed_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"authority_family", std::move(authority_family)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "cluster_insert");
}

MetricValidationResult RecordClusterInsertBadStatsSuppressed(std::string database_uuid,
                                                             std::string table_uuid,
                                                             std::string reason) {
  return IncrementCounter("sb_cluster_insert_bad_stats_suppressed_total",
                          Labels({{"component", "cluster.insert"},
                                  {"database_uuid", std::move(database_uuid)},
                                  {"table_uuid", std::move(table_uuid)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "cluster_insert");
}

MetricValidationResult RecordAgentAction(std::string agent_type, std::string action_class, std::string result) {
  auto status = RequireNonEmpty(agent_type, "agent_type");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_agent_actions_total",
                          Labels({{"component", "agent.runtime"}, {"agent_type", std::move(agent_type)},
                                  {"action_class", std::move(action_class)}, {"result", std::move(result)}}),
                          1.0,
                          "agent_runtime");
}

MetricValidationResult ObserveAgentDecisionLatency(double latency_microseconds,
                                                   std::string agent_type,
                                                   std::string decision_class) {
  auto status = RequireNonEmpty(agent_type, "agent_type");
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_agent_decision_latency_microseconds",
                          Labels({{"component", "agent.runtime"}, {"agent_type", std::move(agent_type)},
                                  {"decision_class", std::move(decision_class)}}),
                          latency_microseconds,
                          "agent_runtime");
}

MetricValidationResult RecordAgentRuntimeServiceEvent(std::string event_class,
                                                      std::string result,
                                                      std::string diagnostic_code) {
  auto status = RequireNonEmpty(event_class, "event_class");
  if (!status.ok) { return status; }
  if (diagnostic_code.empty()) { diagnostic_code = "none"; }
  return IncrementCounter(
      "sb_agent_runtime_service_events_total",
      Labels({{"component", "agent.runtime_service"},
              {"event_class", std::move(event_class)},
              {"result", std::move(result)},
              {"reason", std::move(diagnostic_code)}}),
      1.0,
      "agent_runtime");
}

MetricValidationResult PublishAgentRuntimeServiceLeaseCount(double lease_count,
                                                            std::string state) {
  auto status = RequireNonEmpty(state, "state");
  if (!status.ok) { return status; }
  return SetGauge("sb_agent_runtime_service_leases",
                  Labels({{"component", "agent.runtime_service"},
                          {"state", std::move(state)}}),
                  lease_count,
                  "agent_runtime");
}

MetricValidationResult PublishAgentRuntimeServiceActionCount(double action_count,
                                                             std::string state) {
  auto status = RequireNonEmpty(state, "state");
  if (!status.ok) { return status; }
  return SetGauge("sb_agent_runtime_service_actions",
                  Labels({{"component", "agent.runtime_service"},
                          {"state", std::move(state)}}),
                  action_count,
                  "agent_runtime");
}

MetricValidationResult PublishAgentRuntimeServiceHistoryCount(double history_count) {
  return SetGauge("sb_agent_runtime_service_history_records",
                  Labels({{"component", "agent.runtime_service"}}),
                  history_count,
                  "agent_runtime");
}

MetricValidationResult PublishAgentRuntimeServiceCatalogGeneration(double catalog_generation) {
  return SetGauge("sb_agent_runtime_service_catalog_generation",
                  Labels({{"component", "agent.runtime_service"}}),
                  catalog_generation,
                  "agent_runtime");
}

MetricValidationResult RecordFilespaceAgentCapacityRequest(std::string filespace_uuid,
                                                           std::string request_class,
                                                           std::string result) {
  auto status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_agent_filespace_capacity_requests_total",
                          Labels({{"component", "agent.filespace_capacity"},
                                  {"agent_type", "filespace_capacity_manager"},
                                  {"filespace_uuid", std::move(filespace_uuid)},
                                  {"request_class", std::move(request_class)},
                                  {"result", std::move(result)}}),
                          1.0,
                          "filespace_capacity_manager");
}

MetricValidationResult PublishFilespaceAgentFreeReservePages(double pages,
                                                             std::string filespace_uuid,
                                                             std::string reserve_class) {
  auto status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return SetGauge("sb_agent_filespace_free_reserve_pages",
                  Labels({{"component", "agent.filespace_capacity"},
                          {"agent_type", "filespace_capacity_manager"},
                          {"filespace_uuid", std::move(filespace_uuid)},
                          {"reserve_class", std::move(reserve_class)}}),
                  pages,
                  "filespace_capacity_manager");
}

MetricValidationResult ObserveFilespaceAgentDecisionLatency(double latency_microseconds,
                                                            std::string filespace_uuid,
                                                            std::string action_class) {
  auto status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return ObserveHistogram("sb_agent_filespace_decision_latency_microseconds",
                          Labels({{"component", "agent.filespace_capacity"},
                                  {"agent_type", "filespace_capacity_manager"},
                                  {"filespace_uuid", std::move(filespace_uuid)},
                                  {"action_class", std::move(action_class)}}),
                          latency_microseconds,
                          "filespace_capacity_manager");
}

MetricValidationResult RecordPageAllocationAgentRequest(std::string filespace_uuid,
                                                        std::string page_family,
                                                        std::string request_class,
                                                        std::string result) {
  auto status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_agent_page_allocation_requests_total",
                          Labels({{"component", "agent.page_allocation"},
                                  {"agent_type", "page_allocation_manager"},
                                  {"filespace_uuid", std::move(filespace_uuid)},
                                  {"page_family", std::move(page_family)},
                                  {"request_class", std::move(request_class)},
                                  {"result", std::move(result)}}),
                          1.0,
                          "page_allocation_manager");
}

MetricValidationResult PublishPageAllocationAgentPreallocatedPages(double pages,
                                                                   std::string filespace_uuid,
                                                                   std::string page_family) {
  auto status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return SetGauge("sb_agent_page_allocation_preallocated_pages",
                  Labels({{"component", "agent.page_allocation"},
                          {"agent_type", "page_allocation_manager"},
                          {"filespace_uuid", std::move(filespace_uuid)},
                          {"page_family", std::move(page_family)}}),
                  pages,
                  "page_allocation_manager");
}

MetricValidationResult RecordPageAllocationAgentRelocatedPages(double pages,
                                                               std::string filespace_uuid,
                                                               std::string page_family,
                                                               std::string result) {
  auto status = RequireNonEmpty(filespace_uuid, "filespace_uuid");
  if (!status.ok) { return status; }
  return IncrementCounter("sb_agent_page_allocation_relocated_pages_total",
                          Labels({{"component", "agent.page_allocation"},
                                  {"agent_type", "page_allocation_manager"},
                                  {"filespace_uuid", std::move(filespace_uuid)},
                                  {"page_family", std::move(page_family)},
                                  {"result", std::move(result)}}),
                          pages,
                          "page_allocation_manager");
}

MetricValidationResult RecordAlertFired(std::string severity, std::string health_state, std::string owner_group) {
  return IncrementCounter("sb_alerts_fired_total",
                          Labels({{"component", "alert.runtime"}, {"severity", std::move(severity)},
                                  {"health_state", std::move(health_state)}, {"owner_group", std::move(owner_group)}}),
                          1.0,
                          "alert_runtime");
}

MetricValidationResult RecordExportAdapterFailure(std::string adapter_family, std::string reason) {
  return IncrementCounter("sb_export_adapter_failures_total",
                          Labels({{"component", "core.metrics.export"}, {"adapter_family", std::move(adapter_family)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "metrics_exporter");
}

MetricValidationResult RecordExportAdapterRetry(std::string adapter_family, std::string reason) {
  return IncrementCounter("sb_export_adapter_retries_total",
                          Labels({{"component", "core.metrics.export"}, {"adapter_family", std::move(adapter_family)},
                                  {"reason", std::move(reason)}}),
                          1.0,
                          "metrics_exporter");
}

}  // namespace scratchbird::core::metrics
