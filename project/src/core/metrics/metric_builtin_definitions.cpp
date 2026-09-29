// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_builtin_definitions.hpp"
#include "metric_page_cache_definitions.hpp"
#include <string_view>
#include <initializer_list>
#include <utility>

namespace scratchbird::core::metrics {
namespace {
MetricLabelDescriptor Label(std::string key, bool required = false, bool sensitive = false) {
  return {std::move(key), required, sensitive};
}

MetricLabelDescriptor UuidLabel(std::string key, bool required = false, bool sensitive = false) {
  return {std::move(key), required, sensitive, MetricLabelType::system_uuid};
}

std::vector<MetricLabelDescriptor> CommonLabels() {
  return {
      UuidLabel("database_uuid"), UuidLabel("node_uuid"), UuidLabel("cluster_uuid"), UuidLabel("filespace_uuid"),
      UuidLabel("index_uuid"), UuidLabel("object_uuid"), Label("component"), Label("producer"), Label("operation"),
      Label("result"), Label("reason"), Label("page_size"), Label("classification"), Label("device_class"),
      Label("filespace_role"), Label("page_type"), UuidLabel("policy_uuid"), Label("error_class"),
      Label("access_mode"), Label("unknown_page_policy"), Label("role"),
      Label("provider_family"), Label("auth_provider"), Label("policy_family"), Label("scope_class"),
      Label("parser_family"), Label("interface"), Label("interface_family"), Label("session_class"),
      Label("isolation_class"), Label("owner_class"), Label("allocator_class"), Label("reserve_class"),
      Label("agent_family"), Label("agent_type"), Label("action_class"), Label("decision_class"),
      Label("index_kind"), Label("index_family"), Label("object_kind"), Label("metric_family"), Label("failure_class"),
      Label("subsystem"), Label("wait_class"), Label("evidence_surface"),
      Label("visibility_scope"), Label("cluster_role"), Label("route_generation"), Label("state"),
      Label("page_family"), Label("action"), Label("plan_shape"), Label("operator_family"), Label("route_class"),
      Label("fragment_kind"), Label("remote_node_class"), Label("source_node_class"), Label("target_node_class"),
      Label("reason_class"), Label("blocker_class"), Label("archive_class"), Label("node_class"), Label("resource_class"),
      Label("deny_reason"), Label("workload_class"), Label("adapter_family"), UuidLabel("export_profile_uuid"),
      Label("severity"), Label("health_state"), Label("owner_group"), Label("listener_family"),
      Label("request_class"), Label("format"), Label("feature"), Label("machine_id", false, true),
      Label("window_class"), Label("hold_class"), Label("owner_subsystem"), Label("approval_class"),
      Label("denial_class"), Label("range_class"),
      Label("canonical_type"), Label("source_type"), Label("target_type"), Label("numeric_backend"),
      UuidLabel("domain_uuid"), Label("method"), Label("semantic_profile"),
      Label("authority"), Label("policy"), Label("local_transaction_id"), Label("event_class"),
      Label("restore_classification"), Label("forensic_class"),
      Label("source_host", false, true), Label("network_interface"),
      UuidLabel("user_uuid", false, true), UuidLabel("session_uuid", false, true), UuidLabel("principal_uuid", false, true),
      Label("source_address", false, true)};
}

std::vector<MetricScalar> LatencyBuckets() {
  return {1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0, 50000.0, 100000.0, 500000.0, 1000000.0};
}

MetricDescriptorDefinition Descriptor(std::string family,
                            MetricType type,
                            MetricUnit unit,
                            std::string namespace_path,
                            std::string help,
                            std::string producer_owner,
                            bool cluster_only = false,
                            std::initializer_list<u64> state_codes = {}) {
  MetricDescriptorDefinition descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = std::move(namespace_path);
  descriptor.help = std::move(help);
  descriptor.producer_owner = std::move(producer_owner);
  descriptor.security_family = cluster_only ? "OBS_METRICS_READ_FAMILY:cluster" : "OBS_METRICS_READ_FAMILY";
  descriptor.visibility = cluster_only ? MetricVisibilityScope::cluster : MetricVisibilityScope::family;
  descriptor.cluster_only = cluster_only;
  descriptor.labels = CommonLabels();
  descriptor.value_type = type == MetricType::state
      ? MetricScalarType::enumeration
      : (unit == MetricUnit::ratio ||
         unit == MetricUnit::percent || unit == MetricUnit::seconds ||
         unit == MetricUnit::microseconds)
          ? MetricScalarType::float64 : MetricScalarType::uint64;
  if (descriptor.family == "sb_filespace_growth_rate_bytes_per_second" ||
      descriptor.family == "sb_page_family_growth_rate_pages_per_second")
    descriptor.value_type = MetricScalarType::float64;
  descriptor.enum_values.assign(state_codes);
  if (type == MetricType::counter) descriptor.value_type = MetricScalarType::uint64;
  if (type == MetricType::histogram) {
    descriptor.histogram_buckets = LatencyBuckets();
    if (descriptor.value_type == MetricScalarType::uint64) {
      // Counts are exact integers even when aggregated as a histogram.
      descriptor.histogram_buckets = {u64{1}, u64{5}, u64{10}, u64{50}, u64{100},
          u64{500}, u64{1000}, u64{5000}, u64{10000}, u64{50000}, u64{100000},
          u64{500000}, u64{1000000}};
    }
  }
  return descriptor;
}

void LoadInsertOptimizationDescriptors(std::vector<MetricDescriptorDefinition>* definitions) {
  if (definitions == nullptr) {
    return;
  }
  definitions->push_back(Descriptor("sb_dml_insert_batch_started_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Insert batches admitted by the engine insert batch context.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_batch_fallback_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Insert batches downgraded or refused from the optimized path.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_batch_fallback_reason_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Insert batch fallback/refusal counts by reason.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_rows_inserted_total",
                                                MetricType::counter,
                                                MetricUnit::records,
                                                "sys.metrics.dml.insert",
                                                "Rows inserted through the engine insert path.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_rows_per_batch",
                                                MetricType::histogram,
                                                MetricUnit::records,
                                                "sys.metrics.dml.insert",
                                                "Rows per insert batch.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_row_template_cache_hit_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Bound insert row template cache hits.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_prepared_descriptor_cache_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Prepared insert descriptor cache hits and misses by epoch-keyed statement shape.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_relation_state_full_load_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Full MGA relation-state loads admitted by insert or COPY slow paths.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_relation_state_scoped_load_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Target-scoped MGA relation-state loads admitted by insert or COPY hot paths.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_adaptive_batch_requested_rows",
                                                MetricType::gauge,
                                                MetricUnit::records,
                                                "sys.metrics.dml.insert",
                                                "Requested rows seen by the insert adaptive batch controller.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_adaptive_batch_admitted_rows",
                                                MetricType::gauge,
                                                MetricUnit::records,
                                                "sys.metrics.dml.insert",
                                                "Rows admitted by the insert adaptive batch controller.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_adaptive_batch_admitted_bytes",
                                                MetricType::gauge,
                                                MetricUnit::bytes,
                                                "sys.metrics.dml.insert",
                                                "Estimated bytes admitted by the insert adaptive batch controller.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_adaptive_batch_resize_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Insert adaptive batch reductions caused by policy, memory, large-value, or index pressure.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_allocation_stall_microseconds",
                                                MetricType::histogram,
                                                MetricUnit::microseconds,
                                                "sys.metrics.dml.insert",
                                                "Time spent waiting for insert allocation, page, extent, or agent resources.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_slow_path_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Insert or COPY degraded-path decisions by driver-visible reason.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_identity_range_reserved_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "SQL identity/sequence values reserved in insert batches.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_insert_page_reuse_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.insert",
                                                "Writable page reuse events from insert batches.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_insert_page_full_retry_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.insert",
                                                "Insert retries caused by full page or local page insert refusal.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_insert_page_split_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.insert",
                                                "Page split events caused by insert.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_insert_preallocated_pages_total",
                                                MetricType::counter,
                                                MetricUnit::pages,
                                                "sys.metrics.page.insert",
                                                "Pages preallocated or reserved for insert and COPY by page family.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_allocation_reserve_low_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Page allocation reserve-low notifications.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_reservation_admitted_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Durable insert page reservations admitted by the page allocation manager.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_reservation_refused_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Insert page reservations refused by the page allocation manager.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_reservation_expired_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Insert page reservations expired before full consumption.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_reservation_consumed_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Insert page reservation consumption events.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_reservation_released_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Insert page reservation release events.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_page_reservation_recovered_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.page.allocation",
                                                "Insert page reservations recovered or classified during startup.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_index_insert_synchronous_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.index.insert",
                                                "Synchronous index maintenance events from insert.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_index_insert_delta_ledger_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.index.insert",
                                                "Deferred non-unique secondary-index delta ledger writes.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_index_insert_delta_merge_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.index.insert",
                                                "Deferred secondary-index delta merge events.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_index_insert_unique_preflight_hit_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.index.insert",
                                                "Unique preflight cache/check events.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_index_insert_unique_physical_probe_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.index.insert",
                                                "Physical unique index probes performed by insert conflict checking.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_index_insert_sorted_run_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.index.insert",
                                                "Sorted-run insert/index maintenance events.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_filespace_insert_growth_request_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.filespace.insert",
                                                "Filespace growth requests caused by insert admission.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_filespace_insert_growth_wait_microseconds",
                                                MetricType::histogram,
                                                MetricUnit::microseconds,
                                                "sys.metrics.filespace.insert",
                                                "Time spent waiting for filespace growth during insert.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_cancel_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Cancelled insert batches or bulk load phases.",
                                                "engine_insert"));
  definitions->push_back(Descriptor("sb_dml_insert_trace_event_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.insert",
                                                "Insert trace/debug phase events emitted by the engine.",
                                                "engine_insert"));
}

void LoadUpdateDeleteOptimizationDescriptors(std::vector<MetricDescriptorDefinition>* definitions) {
  if (definitions == nullptr) {
    return;
  }
  definitions->push_back(Descriptor("sb_dml_update_batch_started_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.update",
                                                "UPDATE batches admitted by the engine update batch context.",
                                                "engine_update"));
  definitions->push_back(Descriptor("sb_dml_update_batch_fallback_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.update",
                                                "UPDATE batches downgraded or refused from the optimized path.",
                                                "engine_update"));
  definitions->push_back(Descriptor("sb_dml_update_rows_updated_total",
                                                MetricType::counter,
                                                MetricUnit::records,
                                                "sys.metrics.dml.update",
                                                "Rows updated through the engine update path.",
                                                "engine_update"));
  definitions->push_back(Descriptor("sb_dml_update_cancel_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.update",
                                                "Cancelled UPDATE batches or phases.",
                                                "engine_update"));
  definitions->push_back(Descriptor("sb_dml_update_trace_event_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.update",
                                                "UPDATE trace/debug phase events emitted by the engine.",
                                                "engine_update"));
  definitions->push_back(Descriptor("sb_dml_delete_batch_started_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.delete",
                                                "DELETE batches admitted by the engine delete batch context.",
                                                "engine_delete"));
  definitions->push_back(Descriptor("sb_dml_delete_batch_fallback_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.delete",
                                                "DELETE batches downgraded or refused from the optimized path.",
                                                "engine_delete"));
  definitions->push_back(Descriptor("sb_dml_delete_rows_deleted_total",
                                                MetricType::counter,
                                                MetricUnit::records,
                                                "sys.metrics.dml.delete",
                                                "Rows tombstoned through the engine delete path.",
                                                "engine_delete"));
  definitions->push_back(Descriptor("sb_dml_delete_cancel_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.delete",
                                                "Cancelled DELETE batches or phases.",
                                                "engine_delete"));
  definitions->push_back(Descriptor("sb_dml_delete_trace_event_total",
                                                MetricType::counter,
                                                MetricUnit::events,
                                                "sys.metrics.dml.delete",
                                                "DELETE trace/debug phase events emitted by the engine.",
                                                "engine_delete"));
  definitions->push_back(Descriptor("sb_dml_merge_rows_total",
                                                MetricType::counter,
                                                MetricUnit::records,
                                                "sys.metrics.dml.merge",
                                                "Rows processed by the normalized MERGE/UPSERT engine path by action.",
                                                "engine_merge"));
}

void LoadRelationStateMaterializationDescriptors(std::vector<MetricDescriptorDefinition>* definitions) {
  if (definitions == nullptr) { return; }
  constexpr std::string_view kNamespace = "sys.metrics.mga.relation_load";
  constexpr std::string_view kOwner = "mga_relation_store";
  definitions->push_back(Descriptor(
      "sb_mga_relation_state_load_total", MetricType::counter,
      MetricUnit::events, std::string(kNamespace),
      "MGA relation-state loads by scope, operation, target, and reason.",
      std::string(kOwner)));
  definitions->push_back(Descriptor(
      "sb_mga_relation_state_rows_materialized_total", MetricType::counter,
      MetricUnit::records, std::string(kNamespace),
      "Row versions retained by MGA relation-state loads.",
      std::string(kOwner)));
  definitions->push_back(Descriptor(
      "sb_mga_relation_state_bytes_materialized_total", MetricType::counter,
      MetricUnit::bytes, std::string(kNamespace),
      "Retained object and payload bytes materialized by MGA relation-state loads.",
      std::string(kOwner)));
  definitions->push_back(Descriptor(
      "sb_mga_relation_state_allocation_units_materialized_total",
      MetricType::counter, MetricUnit::events, std::string(kNamespace),
      "Retained allocation-bearing objects materialized by MGA relation-state loads.",
      std::string(kOwner)));
}

}  // namespace

std::vector<MetricDescriptorDefinition> BuiltinMetricDescriptorDefinitions() {
  std::vector<MetricDescriptorDefinition> builtins = {
      Descriptor("sb_memory_allocated_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.memory", "Current allocated memory bytes.", "core_memory"),
      Descriptor("sb_memory_allocation_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.memory", "Memory allocation latency.", "core_memory"),
      Descriptor("sb_memory_allocation_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.memory", "Memory allocation failures.", "core_memory"),
      Descriptor("sb_memory_emergency_reserve_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.memory", "Emergency memory reserve bytes.", "core_memory"),
      Descriptor("sb_memory_background_reclamation_runs_total", MetricType::counter, MetricUnit::events, "sys.metrics.memory", "Bounded background memory reclamation runs.", "core_memory"),
      Descriptor("sb_memory_background_reclaimed_bytes_total", MetricType::counter, MetricUnit::bytes, "sys.metrics.memory", "Bytes reclaimed by bounded background memory reclamation.", "core_memory"),
      Descriptor("sb_memory_background_reclamation_retained_items", MetricType::gauge, MetricUnit::none, "sys.metrics.memory", "Items retained by the latest bounded background memory reclamation run.", "core_memory"),
      Descriptor("sb_llvm_foreign_memory_reservations_total", MetricType::counter, MetricUnit::events, "sys.metrics.memory.foreign", "LLVM foreign-memory reservations by linkage mode and result.", "llvm_memory_accounting"),
      Descriptor("sb_llvm_foreign_memory_reserved_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.memory.foreign", "Current LLVM foreign-memory bytes reserved before loader, code, data, JIT, AOT, or native calls.", "llvm_memory_accounting"),
      Descriptor("sb_llvm_foreign_memory_refusals_total", MetricType::counter, MetricUnit::events, "sys.metrics.memory.foreign", "LLVM foreign-memory accounting refusals by reason.", "llvm_memory_accounting"),
      Descriptor("sb_storage_device_read_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.disk", "Storage device read latency.", "storage_disk"),
      Descriptor("sb_storage_device_write_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.disk", "Storage device write latency.", "storage_disk"),
      Descriptor("sb_storage_fsync_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.disk", "Storage sync latency.", "storage_disk"),
      Descriptor("sb_storage_checksum_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.page", "Page checksum failures.", "storage_page"),
      Descriptor("sb_storage_unknown_pages_total", MetricType::counter, MetricUnit::pages, "sys.metrics.storage.page", "Unknown page classifications.", "storage_page"),
      Descriptor("sb_storage_device_errors_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.disk", "Storage device errors.", "storage_disk"),
      Descriptor("sb_filespace_total_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace total bytes.", "storage_filespace"),
      Descriptor("sb_filespace_used_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace used bytes.", "storage_filespace"),
      Descriptor("sb_filespace_free_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace free bytes.", "storage_filespace"),
      Descriptor("sb_filespace_reserved_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace reserved bytes.", "storage_filespace"),
      Descriptor("sb_filespace_expandable_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace expandable bytes.", "storage_filespace"),
      Descriptor("sb_filespace_growth_rate_bytes_per_second", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace growth rate in bytes per second.", "metrics_runtime"),
      Descriptor("sb_filespace_depletion_eta_seconds", MetricType::gauge, MetricUnit::seconds, "sys.metrics.storage.filespaces", "Filespace depletion ETA.", "metrics_runtime"),
      Descriptor("sb_filespace_shrink_candidate_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace shrink candidate bytes.", "storage_filespace"),
      Descriptor("sb_filespace_truncate_ready_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace truncate-ready bytes.", "page_runtime"),
      Descriptor("sb_filespace_pending_page_relocation_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.filespaces", "Filespace bytes waiting on page relocation.", "page_runtime"),
      Descriptor("sb_filespace_shrink_blocker_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.filespaces", "Filespace shrink blocker count.", "page_runtime"),
      Descriptor("sb_filespace_device_read_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.filespaces", "Filespace-scoped device read latency.", "storage_disk"),
      Descriptor("sb_filespace_device_write_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.filespaces", "Filespace-scoped device write latency.", "storage_disk"),
      Descriptor("sb_filespace_fsync_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.filespaces", "Filespace-scoped fsync latency.", "storage_disk"),
      Descriptor("sb_filespace_device_error_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Filespace-scoped device errors.", "storage_disk"),
      Descriptor("sb_filespace_health_state", MetricType::state, MetricUnit::none, "sys.metrics.storage.filespaces", "Filespace health state.", "storage_filespace", false, {0, 1, 2, 3, 4}),
      Descriptor("sb_filespace_role_state", MetricType::state, MetricUnit::none, "sys.metrics.storage.filespaces", "Filespace role state.", "storage_filespace", false, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}),
      Descriptor("sb_storage_filespace_lifecycle_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Filespace lifecycle operation outcomes.", "storage_filespace"),
      Descriptor("sb_storage_filespace_active_pins", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.filespaces", "Active pins by filespace role/state.", "storage_filespace"),
      Descriptor("sb_cloud_filespace_operation_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Cloud filespace provider operation outcomes.", "storage_filespace"),
      Descriptor("sb_foreign_filespace_quarantine_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Foreign filespace quarantine inspection and release outcomes.", "storage_filespace"),
      Descriptor("sb_filespace_move_request_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Filespace move requests.", "agent_runtime"),
      Descriptor("sb_filespace_expand_request_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Filespace expand requests.", "agent_runtime"),
      Descriptor("sb_filespace_shrink_request_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.filespaces", "Filespace shrink requests.", "agent_runtime"),
      Descriptor("sb_storage_pressure_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.pressure", "Storage pressure events by reason.", "metrics_runtime"),
      Descriptor("sb_temp_workspace_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.temp", "Temporary workspace bytes by operation.", "metrics_runtime"),
      Descriptor("sb_index_build_workspace_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.index_build", "Index-build workspace bytes.", "metrics_runtime"),
      Descriptor("sb_storage_support_redaction_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.redaction", "Storage support-bundle redaction events.", "metrics_runtime"),
      Descriptor("sb_page_free_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Free page count by filespace/page family/page type.", "storage_page"),
      Descriptor("sb_page_allocated_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Allocated page count by filespace/page family/page type.", "storage_page"),
      Descriptor("sb_page_released_free_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Released/free page count used for filespace capacity notification.", "storage_page"),
      Descriptor("sb_page_reserved_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Reserved page count by filespace/page family/page type.", "storage_page"),
      Descriptor("sb_page_preallocated_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Preallocated page count by filespace/page family/page type.", "storage_page"),
      Descriptor("sb_page_preallocation_target_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Target preallocated page count from active policy.", "page_runtime"),
      Descriptor("sb_page_preallocation_deficit_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Preallocation deficit count from active policy.", "page_runtime"),
      Descriptor("sb_page_allocation_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.pages", "Page allocation latency.", "storage_page"),
      Descriptor("sb_page_allocation_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.pages", "Page allocation failures.", "storage_page"),
      Descriptor("sb_page_cache_resident_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.storage.pages.cache", "Resident pages in the page cache.", "storage_page"),
      Descriptor("sb_page_cache_resident_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.pages.cache", "Resident bytes in the page cache.", "storage_page"),
      Descriptor("sb_page_cache_pinned_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.storage.pages.cache", "Pinned pages in the page cache.", "storage_page"),
      Descriptor("sb_page_cache_dirty_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.storage.pages.cache", "Dirty pages in the page cache.", "storage_page"),
      Descriptor("sb_page_cache_evictions_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.pages.cache", "Page cache evictions.", "storage_page"),
      Descriptor("sb_page_fragmentation_ratio", MetricType::gauge, MetricUnit::ratio, "sys.metrics.storage.pages", "Page fragmentation ratio.", "page_runtime"),
      Descriptor("sb_page_relocation_backlog_count", MetricType::gauge, MetricUnit::none, "sys.metrics.storage.pages", "Page relocation backlog count.", "page_runtime"),
      Descriptor("sb_page_relocation_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.storage.pages", "Page relocation latency.", "page_runtime"),
      Descriptor("sb_page_relocation_blocked_total", MetricType::counter, MetricUnit::events, "sys.metrics.storage.pages", "Page relocation blocked events.", "page_runtime"),
      Descriptor("sb_page_relocatable_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.pages", "Relocatable bytes by page family/type.", "page_runtime"),
      Descriptor("sb_page_unmovable_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.storage.pages", "Unmovable bytes by page family/type.", "page_runtime"),
      Descriptor("sb_page_family_growth_rate_pages_per_second", MetricType::gauge, MetricUnit::pages, "sys.metrics.storage.pages", "Page family growth rate in pages per second.", "metrics_runtime"),
      Descriptor("sb_page_family_depletion_eta_seconds", MetricType::gauge, MetricUnit::seconds, "sys.metrics.storage.pages", "Page family depletion ETA.", "metrics_runtime"),
      Descriptor("sb_page_relocation_ready_for_filespace_shrink", MetricType::state, MetricUnit::none, "sys.metrics.storage.pages", "Page relocation readiness for filespace shrink.", "page_runtime", false, {0, 1}),
      Descriptor("sb_index_lookup_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.indexes", "Index lookup latency.", "index_runtime"),
      Descriptor("sb_index_splits_total", MetricType::counter, MetricUnit::events, "sys.metrics.indexes", "Index page splits.", "index_runtime"),
      Descriptor("sb_index_read_amplification_ratio", MetricType::gauge, MetricUnit::ratio, "sys.metrics.indexes", "Index read amplification ratio.", "index_runtime"),
      Descriptor("sb_datatype_operation_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes", "Datatype operation outcomes by canonical type.", "datatype_runtime"),
      Descriptor("sb_datatype_cast_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes", "Datatype cast outcomes by source and target type.", "datatype_runtime"),
      Descriptor("sb_datatype_numeric_backend_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes", "Numeric backend operation outcomes.", "datatype_runtime"),
      Descriptor("sb_datatype_catalog_descriptors", MetricType::gauge, MetricUnit::none, "sys.metrics.datatypes", "Datatype descriptors visible through catalog and information surfaces.", "datatype_runtime"),
      Descriptor("sb_datatype_physical_encoding_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes.physical", "Datatype physical payload encoding outcomes.", "datatype_runtime"),
      Descriptor("sb_datatype_chunk_event_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes.physical", "Datatype chunk, overflow, relocation, and salvage events.", "datatype_runtime"),
      Descriptor("sb_datatype_comparison_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes.operations", "Datatype comparison and collation outcomes.", "datatype_runtime"),
      Descriptor("sb_datatype_locator_event_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes.locators", "Datatype locator and opaque handle outcomes.", "datatype_runtime"),
      Descriptor("sb_datatype_redaction_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes.redaction", "Datatype support-bundle redaction outcomes.", "datatype_runtime"),
      Descriptor("sb_domain_method_invocation_total", MetricType::counter, MetricUnit::events, "sys.metrics.datatypes.domains", "Domain method invocations by binding and result.", "datatype_runtime"),
      Descriptor("sb_optimizer_plan_estimate_error_ratio", MetricType::gauge, MetricUnit::ratio, "sys.metrics.optimizer", "Optimizer estimate error ratio.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_rows", MetricType::gauge, MetricUnit::records, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated rows.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_rows", MetricType::gauge, MetricUnit::records, "sys.metrics.optimizer.feedback", "Optimizer feedback actual rows.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated pages.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.optimizer.feedback", "Optimizer feedback actual pages.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_io_operations", MetricType::gauge, MetricUnit::none, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated IO operations.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_io_operations", MetricType::gauge, MetricUnit::none, "sys.metrics.optimizer.feedback", "Optimizer feedback actual IO operations.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_visibility_recheck_rows", MetricType::gauge, MetricUnit::records, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated MGA visibility recheck rows.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_visibility_recheck_rows", MetricType::gauge, MetricUnit::records, "sys.metrics.optimizer.feedback", "Optimizer feedback actual MGA visibility recheck rows.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_spill_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated spill bytes.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_spill_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.optimizer.feedback", "Optimizer feedback actual spill bytes.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_memory_grant_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.optimizer.feedback", "Optimizer feedback memory grant bytes.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_peak_memory_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.optimizer.feedback", "Optimizer feedback observed peak memory bytes.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_recommended_memory_grant_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.optimizer.feedback", "Optimizer feedback recommended memory grant bytes.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_latency_microseconds", MetricType::gauge, MetricUnit::microseconds, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated latency in microseconds.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_latency_microseconds", MetricType::gauge, MetricUnit::microseconds, "sys.metrics.optimizer.feedback", "Optimizer feedback actual latency in microseconds.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_estimated_resource_units", MetricType::gauge, MetricUnit::none, "sys.metrics.optimizer.feedback", "Optimizer feedback estimated resource units.", "optimizer_executor_feedback"),
      Descriptor("sb_optimizer_feedback_actual_resource_units", MetricType::gauge, MetricUnit::none, "sys.metrics.optimizer.feedback", "Optimizer feedback actual resource units.", "optimizer_executor_feedback"),
      Descriptor("sb_lock_latch_contention_wait_total", MetricType::counter, MetricUnit::events, "sys.metrics.runtime.contention", "Lock and latch wait count by subsystem and support surface.", "runtime_contention"),
      Descriptor("sb_lock_latch_contention_wait_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.runtime.contention", "Lock and latch wait time by subsystem and support surface.", "runtime_contention"),
      Descriptor("sb_native_compile_jit_compile_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "JIT compile attempts by result unit kind and profile.", "native_compile"),
      Descriptor("sb_native_compile_jit_compile_seconds", MetricType::histogram, MetricUnit::seconds, "sys.metrics.native_compile", "JIT/AOT compile latency in seconds.", "native_compile"),
      Descriptor("sb_native_compile_jit_cache_hit_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "JIT cache hits by unit kind and profile.", "native_compile"),
      Descriptor("sb_native_compile_fallback_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "Interpreter fallbacks or refusals by reason.", "native_compile"),
      Descriptor("sb_native_compile_aot_load_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "AOT compile/load attempts by result.", "native_compile"),
      Descriptor("sb_native_compile_aot_invalid_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "AOT invalid or refused artifacts by reason.", "native_compile"),
      Descriptor("sb_native_compile_execution_fault_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "Native execution faults by unit kind and reason.", "native_compile"),
      Descriptor("sb_native_compile_invalidation_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "Native code cache invalidations by dependency family.", "native_compile"),
      Descriptor("sb_native_compile_equivalence_failure_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "Native/interpreter equivalence failures.", "native_compile"),
      Descriptor("sb_native_compile_quarantine_total", MetricType::counter, MetricUnit::events, "sys.metrics.native_compile", "Quarantined native entries or artifacts.", "native_compile"),
      Descriptor("sb_gpu_device_available", MetricType::gauge, MetricUnit::none, "sys.metrics.gpu", "GPU device/provider availability by provider workload and result.", "gpu_acceleration"),
      Descriptor("sb_gpu_kernel_compile_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU kernel compile attempts by backend workload and result.", "gpu_acceleration"),
      Descriptor("sb_gpu_kernel_compile_seconds", MetricType::histogram, MetricUnit::seconds, "sys.metrics.gpu", "GPU kernel compile latency in seconds.", "gpu_acceleration"),
      Descriptor("sb_gpu_artifact_load_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU artifact load attempts by result.", "gpu_acceleration"),
      Descriptor("sb_gpu_artifact_invalid_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU invalid artifact refusals by reason.", "gpu_acceleration"),
      Descriptor("sb_gpu_execution_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU execution admission attempts by workload result and reason.", "gpu_acceleration"),
      Descriptor("sb_gpu_execution_seconds", MetricType::histogram, MetricUnit::seconds, "sys.metrics.gpu", "GPU execution latency in seconds.", "gpu_acceleration"),
      Descriptor("sb_gpu_fallback_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU fallback to CPU/interpreter by workload and reason.", "gpu_acceleration"),
      Descriptor("sb_gpu_runtime_fault_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU runtime faults by device workload and reason.", "gpu_acceleration"),
      Descriptor("sb_gpu_quarantine_total", MetricType::counter, MetricUnit::events, "sys.metrics.gpu", "GPU device kernel or artifact quarantines.", "gpu_acceleration"),
      Descriptor("sb_gpu_memory_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.gpu", "GPU device or pinned host memory reserved for admitted GPU work.", "gpu_acceleration"),
      Descriptor("sb_gpu_transfer_bytes_total", MetricType::counter, MetricUnit::bytes, "sys.metrics.gpu", "GPU host/device transfer bytes by workload.", "gpu_acceleration"),
      Descriptor("sb_gpu_cluster_placement_refused_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.gpu", "Cluster GPU placement refusals by reason.", "gpu_acceleration", true),
      Descriptor("sb_udr_registration_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Trusted UDR registration attempts by result and reason.", "udr_runtime"),
      Descriptor("sb_udr_load_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Trusted UDR load attempts by result and reason.", "udr_runtime"),
      Descriptor("sb_udr_unload_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Trusted UDR unload attempts by result and reason.", "udr_runtime"),
      Descriptor("sb_udr_inspect_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Trusted UDR inspection attempts by result and reason.", "udr_runtime"),
      Descriptor("sb_udr_invocation_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Trusted UDR invocation admissions by result and reason.", "udr_runtime"),
      Descriptor("sb_udr_resource_refused_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Trusted UDR resource-budget refusals by reason.", "udr_runtime"),
      Descriptor("sb_udr_non_cpp_refusal_total", MetricType::counter, MetricUnit::events, "sys.metrics.udr", "Non-C++ UDR runtime registration attempts refused by the trusted runtime boundary.", "udr_runtime"),
      Descriptor("sb_security_deep_enforcement_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.enforcement", "Unified deep security enforcement decisions by phase right result and reason.", "security_deep_enforcement"),
      Descriptor("sb_security_deep_refusal_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.enforcement", "Unified deep security enforcement refusals by phase and reason.", "security_deep_enforcement"),
      Descriptor("sb_security_audit_before_success_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.enforcement", "Mutating operations that wrote required security audit evidence before success.", "security_deep_enforcement"),
      Descriptor("sb_security_masking_applied_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.enforcement", "Masking decisions applied by deep security enforcement.", "security_deep_enforcement"),
      Descriptor("sb_security_rls_filter_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.enforcement", "Row-level security filter decisions applied by deep security enforcement.", "security_deep_enforcement"),
      Descriptor("sb_optimizer_remote_fragment_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "cluster.sys.metrics.query.fragments", "Remote fragment latency.", "distributed_query", true),
      Descriptor("sb_query_fragment_queue_delay_microseconds", MetricType::histogram, MetricUnit::microseconds, "cluster.sys.metrics.query.fragments", "Query fragment queue delay.", "distributed_query", true),
      Descriptor("sb_query_fragment_propagation_delay_microseconds", MetricType::histogram, MetricUnit::microseconds, "cluster.sys.metrics.query.fragments", "Cross-node query fragment propagation delay.", "distributed_query", true),
      Descriptor("sb_query_fragment_local_connection_delay_microseconds", MetricType::histogram, MetricUnit::microseconds, "cluster.sys.metrics.query.fragments", "Local connection delay before remote fragment dispatch.", "distributed_query", true),
      Descriptor("sb_query_fragment_sample_freshness_microseconds", MetricType::gauge, MetricUnit::microseconds, "cluster.sys.metrics.query.fragments", "Freshness age of cross-node metric samples used by distributed planning.", "distributed_query", true),
      Descriptor("sb_parser_sessions_active", MetricType::gauge, MetricUnit::none, "sys.metrics.parsers", "Active parser sessions.", "parser_listener"),
      Descriptor("sb_parser_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.parsers", "Parser failures.", "parser_listener"),
      Descriptor("sb_parser_crashes_total", MetricType::counter, MetricUnit::events, "sys.metrics.parsers", "Parser process crashes.", "parser_listener"),
      Descriptor("sb_parser_policy_attach_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.parsers", "Parser policy attachment latency.", "parser_listener"),
      Descriptor("sb_listener_sessions_active", MetricType::gauge, MetricUnit::none, "sys.metrics.listener", "Active listener sessions.", "listener"),
      Descriptor("sb_listener_rejects_total", MetricType::counter, MetricUnit::events, "sys.metrics.listener", "Listener rejected connection attempts.", "listener"),
      Descriptor("sb_listener_queue_depth", MetricType::gauge, MetricUnit::none, "sys.metrics.listener", "Listener admission queue depth.", "listener"),
      Descriptor("sb_listener_queue_delay_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.listener", "Listener admission queue delay.", "listener"),
      Descriptor("sb_management_frontend_requests_total", MetricType::counter, MetricUnit::events, "sys.metrics.management", "Management frontend requests.", "management_frontend"),
      Descriptor("sb_management_frontend_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.management", "Management frontend request latency.", "management_frontend"),
      Descriptor("sb_tx_begin_total", MetricType::counter, MetricUnit::transactions, "sys.metrics.transactions", "Local transaction begin operations.", "transaction_mga"),
      Descriptor("sb_tx_commit_total", MetricType::counter, MetricUnit::transactions, "sys.metrics.transactions", "Local transaction commit operations.", "transaction_mga"),
      Descriptor("sb_tx_rollback_total", MetricType::counter, MetricUnit::transactions, "sys.metrics.transactions", "Local transaction rollback operations.", "transaction_mga"),
      Descriptor("sb_tx_abort_total", MetricType::counter, MetricUnit::transactions, "sys.metrics.transactions", "Local transaction abort operations.", "transaction_mga"),
      Descriptor("sb_tx_failure_total", MetricType::counter, MetricUnit::events, "sys.metrics.transactions", "Local transaction failures.", "transaction_mga"),
      Descriptor("sb_tx_cluster_fail_closed_total", MetricType::counter, MetricUnit::events, "sys.metrics.transactions", "Cluster transaction fail-closed operations.", "transaction_mga"),
      Descriptor("sb_tx_active_transactions", MetricType::gauge, MetricUnit::transactions, "sys.metrics.transactions", "Active local transactions.", "transaction_mga"),
      Descriptor("sb_tx_oldest_snapshot_age_microseconds", MetricType::gauge, MetricUnit::microseconds, "sys.metrics.transactions", "Oldest snapshot age.", "transaction_mga"),
      Descriptor("sb_tx_runtime_policy_violation_total", MetricType::counter, MetricUnit::events, "sys.metrics.transactions.policy", "Fail-closed local transaction runtime policy violations.", "transaction_mga"),
      Descriptor("sb_tx_lock_decisions_total", MetricType::counter, MetricUnit::events, "sys.metrics.transactions.locks", "Local transaction lock decisions by decision class.", "transaction_mga_lock"),
      Descriptor("sb_tx_locks_held", MetricType::gauge, MetricUnit::none, "sys.metrics.transactions.locks", "Currently held local transaction lock resources.", "transaction_mga_lock"),
      Descriptor("sb_tx_lock_waiters", MetricType::gauge, MetricUnit::none, "sys.metrics.transactions.locks", "Current local transaction lock waiters.", "transaction_mga_lock"),
      Descriptor("sb_tx_savepoint_operations_total", MetricType::counter, MetricUnit::events, "sys.metrics.transactions.savepoints", "Local savepoint, subtransaction, and rollback-journal operations by action.", "transaction_mga_savepoint"),
      Descriptor("sb_mga_row_versions_reclaimed_total", MetricType::counter, MetricUnit::events, "sys.metrics.mga.cleanup", "Row versions reclaimed by authoritative MGA cleanup worksets.", "transaction_mga_cleanup"),
      Descriptor("sb_mga_cleanup_horizon_local_transaction_id", MetricType::gauge, MetricUnit::revisions, "sys.metrics.mga.cleanup", "Authoritative local cleanup horizon transaction id.", "transaction_mga_cleanup"),
      Descriptor("sb_mga_cleanup_blocked_total", MetricType::counter, MetricUnit::events, "sys.metrics.mga.cleanup", "MGA cleanup blocked decisions.", "transaction_mga_cleanup"),
      Descriptor("sb_mga_cleanup_retained_row_versions", MetricType::gauge, MetricUnit::none, "sys.metrics.mga.cleanup", "Row versions retained by the latest authoritative MGA cleanup workset.", "transaction_mga_cleanup"),
      Descriptor("sb_mga_cleanup_retention_age_microseconds", MetricType::gauge, MetricUnit::microseconds, "sys.metrics.mga.cleanup", "Approximate retained version age behind the authoritative cleanup horizon.", "transaction_mga_cleanup"),
      Descriptor("sb_mga_temporary_on_commit_delete_rows_total", MetricType::counter, MetricUnit::records, "sys.metrics.mga.temporary", "Rows deleted from session-scoped temporary objects by ON COMMIT DELETE ROWS.", "engine_crud_temporary"),
      Descriptor("sb_archive_lag_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.archive", "Archive lag bytes.", "archive_runtime"),
      Descriptor("sb_archive_slice_count", MetricType::gauge, MetricUnit::none, "sys.metrics.archive", "Archive slice count by archive class.", "archive_runtime"),
      Descriptor("sb_archive_slice_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.archive", "Archive slice bytes by archive class.", "archive_runtime"),
      Descriptor("sb_archive_slice_age_microseconds", MetricType::gauge, MetricUnit::microseconds, "sys.metrics.archive", "Archive slice age.", "archive_runtime"),
      Descriptor("sb_archive_slice_max_age_microseconds", MetricType::gauge, MetricUnit::microseconds, "sys.metrics.archive", "Archive slice maximum allowed age from active policy.", "archive_runtime"),
      Descriptor("sb_archive_health_state", MetricType::state, MetricUnit::none, "sys.metrics.archive", "Archive health state.", "archive_runtime", false, {0, 1, 2, 3, 4}),
      Descriptor("sb_archive_queue_depth", MetricType::gauge, MetricUnit::none, "sys.metrics.archive", "Archive queue depth.", "archive_runtime"),
      Descriptor("sb_archive_delta_lag_transactions", MetricType::gauge, MetricUnit::transactions, "sys.metrics.archive.pitr", "PIT delta source finality lag in transactions.", "archive_runtime"),
      Descriptor("sb_archive_delta_apply_lag_transactions", MetricType::gauge, MetricUnit::transactions, "sys.metrics.archive.pitr", "PIT delta apply lag in transactions.", "archive_runtime"),
      Descriptor("sb_archive_checksum_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.archive.health", "Archive manifest or payload checksum failures.", "archive_runtime"),
      Descriptor("sb_archive_restore_refusals_total", MetricType::counter, MetricUnit::events, "sys.metrics.archive.health", "Archive restore or apply refusals by reason.", "archive_runtime"),
      Descriptor("sb_backup_in_progress", MetricType::gauge, MetricUnit::none, "sys.metrics.backup", "Backup operations currently in progress.", "backup_runtime"),
      Descriptor("sb_backup_progress_percent", MetricType::gauge, MetricUnit::percent, "sys.metrics.backup", "Backup progress percent.", "backup_runtime"),
      Descriptor("sb_restore_drill_duration_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.restore", "Restore drill duration.", "restore_drill_manager"),
      Descriptor("sb_pitr_window_available_seconds", MetricType::gauge, MetricUnit::seconds, "sys.metrics.pitr", "PITR window available seconds.", "pitr_manager"),
      Descriptor("sb_auth_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.auth", "Authentication failures.", "security_auth"),
      Descriptor("sb_identity_auth_attempts_total", MetricType::counter, MetricUnit::events, "sys.metrics.identity", "Authentication attempts.", "identity_manager"),
      Descriptor("sb_identity_sessions_active", MetricType::gauge, MetricUnit::none, "sys.metrics.identity", "Active identity sessions.", "session_manager"),
      Descriptor("sb_identity_users_online", MetricType::gauge, MetricUnit::none, "sys.metrics.identity", "Online authenticated users.", "session_manager"),
      Descriptor("sb_policy_evaluations_total", MetricType::counter, MetricUnit::events, "sys.metrics.security.policy", "Policy evaluations.", "policy_runtime"),
      Descriptor("sb_agent_actions_total", MetricType::counter, MetricUnit::events, "sys.metrics.agents", "Agent actions.", "agent_runtime"),
      Descriptor("sb_agent_decision_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.agents", "Agent decision latency.", "agent_runtime"),
      Descriptor("sb_agent_runtime_service_events_total", MetricType::counter, MetricUnit::events, "sys.metrics.agents.runtime_service", "Agent runtime service lifecycle events by result.", "agent_runtime"),
      Descriptor("sb_agent_runtime_service_leases", MetricType::gauge, MetricUnit::none, "sys.metrics.agents.runtime_service", "Durable agent runtime service leases by state.", "agent_runtime"),
      Descriptor("sb_agent_runtime_service_actions", MetricType::gauge, MetricUnit::none, "sys.metrics.agents.runtime_service", "Durable agent runtime service action queue records by state.", "agent_runtime"),
      Descriptor("sb_agent_runtime_service_history_records", MetricType::gauge, MetricUnit::records, "sys.metrics.agents.runtime_service", "Durable agent runtime service retained history records.", "agent_runtime"),
      Descriptor("sb_agent_runtime_service_catalog_generation", MetricType::gauge, MetricUnit::revisions, "sys.metrics.agents.runtime_service", "Durable agent runtime service catalog generation.", "agent_runtime"),
      Descriptor("sb_agent_filespace_capacity_requests_total", MetricType::counter, MetricUnit::events, "sys.metrics.agents.filespace", "Filespace capacity manager requests by class and result.", "filespace_capacity_manager"),
      Descriptor("sb_agent_filespace_free_reserve_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.agents.filespace", "Filespace capacity manager free-page reserve tracked against policy.", "filespace_capacity_manager"),
      Descriptor("sb_agent_filespace_decision_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.agents.filespace", "Filespace capacity manager decision latency.", "filespace_capacity_manager"),
      Descriptor("sb_agent_page_allocation_requests_total", MetricType::counter, MetricUnit::events, "sys.metrics.agents.page_allocation", "Page allocation manager requests by page family, class, and result.", "page_allocation_manager"),
      Descriptor("sb_agent_page_allocation_preallocated_pages", MetricType::gauge, MetricUnit::pages, "sys.metrics.agents.page_allocation", "Page allocation manager preallocated pages available for fast use.", "page_allocation_manager"),
      Descriptor("sb_agent_page_allocation_relocated_pages_total", MetricType::counter, MetricUnit::pages, "sys.metrics.agents.page_allocation", "Page allocation manager pages relocated for compaction, shrink, or balancing.", "page_allocation_manager"),
      Descriptor("sb_alerts_fired_total", MetricType::counter, MetricUnit::events, "sys.metrics.alerts", "Alerts fired.", "alert_runtime"),
      Descriptor("sb_cluster_node_cpu_feature_available", MetricType::state, MetricUnit::none, "sys.metrics.physical", "Node CPU feature availability.", "cluster_node", false, {0, 1}),
      Descriptor("sb_cluster_node_role_state", MetricType::state, MetricUnit::none, "cluster.sys.metrics.node", "Cluster node role lifecycle state.", "cluster_node", true, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}),
      Descriptor("sb_cluster_node_saturation_ratio", MetricType::gauge, MetricUnit::ratio, "cluster.sys.metrics.node", "Cluster node saturation ratio.", "cluster_node", true),
      Descriptor("sb_cluster_admission_denied_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.admission", "Cluster admission denials.", "cluster_admission", true),
      Descriptor("sb_cluster_limbo_transactions", MetricType::gauge, MetricUnit::transactions, "cluster.sys.metrics.transactions", "Cluster limbo transactions.", "cluster_transaction", true),
      Descriptor("sb_cluster_rolling_upgrade_readiness_state", MetricType::state, MetricUnit::none, "cluster.sys.metrics.lifecycle", "Cluster rolling upgrade readiness.", "cluster_lifecycle", true, {0, 1, 2}),
      Descriptor("sb_cluster_scheduler_queue_depth", MetricType::gauge, MetricUnit::none, "cluster.sys.metrics.scheduler", "Cluster scheduler queue depth.", "cluster_scheduler", true),
      Descriptor("sb_cluster_insert_route_checks_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.insert", "Cluster insert route fence checks by result and reason.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_stale_route_rejections_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.insert", "Cluster insert stale-owner route rejections.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_participant_admissions_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.insert", "Cluster insert participant admission attempts by result and reason.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_remote_requests_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.insert", "Cluster insert remote participant request attempts by result and retry class.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_finality_wait_microseconds", MetricType::histogram, MetricUnit::microseconds, "cluster.sys.metrics.insert", "Cluster insert finality wait duration.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_rows_mutated_total", MetricType::counter, MetricUnit::records, "cluster.sys.metrics.insert", "Cluster insert rows made durable after finality.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_fail_closed_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.insert", "Cluster insert fail-closed decisions by authority family and reason.", "cluster_insert", true),
      Descriptor("sb_cluster_insert_bad_stats_suppressed_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.insert", "Cluster insert bad remote-stat candidates suppressed instead of costed.", "cluster_insert", true),
      Descriptor("sb_event_channels", MetricType::gauge, MetricUnit::none, "sys.metrics.events", "Event channels visible in the local database.", "event_notification"),
      Descriptor("sb_event_subscriptions_active", MetricType::gauge, MetricUnit::none, "sys.metrics.events", "Active event subscriptions.", "event_notification"),
      Descriptor("sb_event_subscribe_attempts_total", MetricType::counter, MetricUnit::events, "sys.metrics.events", "Event subscribe attempts by result and parser family.", "event_notification"),
      Descriptor("sb_event_publish_attempts_total", MetricType::counter, MetricUnit::events, "sys.metrics.events", "Event publish attempts by result and event class.", "event_notification"),
      Descriptor("sb_event_publications_committed_total", MetricType::counter, MetricUnit::events, "sys.metrics.events", "Event publications classified as committed-visible.", "event_notification"),
      Descriptor("sb_event_publications_rolled_back_total", MetricType::counter, MetricUnit::events, "sys.metrics.events", "Event publications discarded by rollback classification.", "event_notification"),
      Descriptor("sb_event_queued", MetricType::gauge, MetricUnit::none, "sys.metrics.events.queue", "Queued event notifications by parser channel.", "event_notification"),
      Descriptor("sb_event_queue_bytes", MetricType::gauge, MetricUnit::bytes, "sys.metrics.events.queue", "Queued event notification bytes by parser channel.", "event_notification"),
      Descriptor("sb_event_delivered_total", MetricType::counter, MetricUnit::events, "sys.metrics.events.delivery", "Event notifications delivered to parsers.", "event_notification"),
      Descriptor("sb_event_delivery_latency_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.events.delivery", "Event notification delivery latency.", "event_notification"),
      Descriptor("sb_event_backpressure_total", MetricType::counter, MetricUnit::events, "sys.metrics.events.queue", "Event notification backpressure decisions.", "event_notification"),
      Descriptor("sb_event_redacted_total", MetricType::counter, MetricUnit::events, "sys.metrics.events.delivery", "Event notification redaction decisions.", "event_notification"),
      Descriptor("sb_cluster_event_route_attempts_total", MetricType::counter, MetricUnit::events, "cluster.sys.metrics.events", "Cluster event route attempts reserved for private cluster authority.", "cluster_event_notification", true),
      Descriptor("sb_query_fragment_route_generation", MetricType::gauge, MetricUnit::revisions, "cluster.sys.metrics.query.fragments", "Route generation observed by query-fragment metrics.", "distributed_query", true),
      Descriptor("sb_scheduler_queue_depth", MetricType::gauge, MetricUnit::none, "sys.metrics.scheduler", "Scheduler queue depth.", "scheduler_runtime"),
      Descriptor("sb_job_control_actions_total", MetricType::counter, MetricUnit::events, "sys.metrics.jobs", "Job control actions.", "job_runtime"),
      Descriptor("sb_support_bundle_completeness_ratio", MetricType::gauge, MetricUnit::ratio, "sys.metrics.supportability", "Support bundle completeness ratio.", "support_bundle_triage_agent"),
      Descriptor("sb_export_adapter_queue_depth", MetricType::gauge, MetricUnit::none, "sys.metrics.export", "Export adapter queue depth.", "metrics_exporter"),
      Descriptor("sb_export_adapter_failures_total", MetricType::counter, MetricUnit::events, "sys.metrics.export", "Export adapter failures.", "metrics_exporter"),
      Descriptor("sb_export_adapter_retries_total", MetricType::counter, MetricUnit::events, "sys.metrics.export", "Export adapter retries.", "metrics_exporter"),
      Descriptor("sb_metric_samples_rejected_total", MetricType::counter, MetricUnit::events, "sys.metrics.registry", "Metric samples rejected by policy or validation.", "metrics_registry_manager"),
      Descriptor("sb_metric_export_shed_total", MetricType::counter, MetricUnit::events, "sys.metrics.registry", "Metric export samples shed by local registry pressure control.", "metrics_registry_manager"),
      Descriptor("sb_idle_capacity_ratio", MetricType::gauge, MetricUnit::ratio, "sys.metrics.capacity", "Idle capacity ratio.", "capacity_runtime"),
      Descriptor("sb_workload_slo_burn_rate", MetricType::gauge, MetricUnit::ratio, "sys.metrics.workloads", "Workload SLO burn rate.", "slo_manager"),
  };
  LoadInsertOptimizationDescriptors(&builtins);
  LoadUpdateDeleteOptimizationDescriptors(&builtins);
  LoadRelationStateMaterializationDescriptors(&builtins);
  for (auto definition : PageCacheContextMetricDefinitions())
    builtins.push_back(std::move(definition));
  return builtins;
}

}  // namespace scratchbird::core::metrics
