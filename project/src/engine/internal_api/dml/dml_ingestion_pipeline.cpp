// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_ingestion_pipeline.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace scratchbird::engine::internal_api {
namespace {

using PipelineClock = std::chrono::steady_clock;

EngineApiU64 ElapsedMicros(PipelineClock::time_point start,
                           PipelineClock::time_point finish) {
  return static_cast<EngineApiU64>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic PipelineDiagnostic(std::string detail) {
  return MakeInvalidRequestDiagnostic("dml.ingestion_pipeline", std::move(detail));
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool IsTruthyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "1" || lowered == "true" || lowered == "enabled" ||
         lowered == "on" || lowered == "required";
}

bool IsFalsyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "0" || lowered == "false" || lowered == "disabled" ||
         lowered == "off";
}

EngineApiU64 ParseU64(const std::string& value, EngineApiU64 fallback) {
  if (value.empty()) {
    return fallback;
  }
  EngineApiU64 parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return fallback;
    }
    const EngineApiU64 digit = static_cast<EngineApiU64>(ch - '0');
    if (parsed > (std::numeric_limits<EngineApiU64>::max() - digit) / 10) {
      return fallback;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

EngineApiU64 CeilDiv(EngineApiU64 numerator, EngineApiU64 denominator) {
  if (denominator == 0) {
    return 0;
  }
  return numerator / denominator + (numerator % denominator == 0 ? 0 : 1);
}

void AddAllocationSummary(const DmlPageAllocationRuntimeResult& allocation,
                          EngineDmlSummaryCounters* summary) {
  if (summary == nullptr || !allocation.active) {
    return;
  }
  ++summary->preallocation_requests;
  summary->preallocation_granted_pages += allocation.granted_preallocation_pages;
  if (allocation.preallocation_capped) {
    ++summary->preallocation_capped;
  }
  if (allocation.preallocation_refused) {
    ++summary->preallocation_refused;
  }
  if (allocation.active) {
    ++summary->page_reservations;
  }
}

}  // namespace

std::string DmlIngestionOptionValue(const std::vector<std::string>& options,
                                    const std::string& key) {
  const std::string equals_prefix = key + "=";
  const std::string colon_prefix = key + ":";
  for (const auto& option : options) {
    if (option.rfind(equals_prefix, 0) == 0) {
      return option.substr(equals_prefix.size());
    }
    if (option.rfind(colon_prefix, 0) == 0) {
      return option.substr(colon_prefix.size());
    }
  }
  return {};
}

EngineApiU64 DmlIngestionOptionU64(const std::vector<std::string>& options,
                                   const std::string& key,
                                   EngineApiU64 fallback) {
  return ParseU64(DmlIngestionOptionValue(options, key), fallback);
}

bool DmlIngestionOptionTruthy(const std::vector<std::string>& options,
                              const std::string& key) {
  return IsTruthyValue(DmlIngestionOptionValue(options, key));
}

bool DmlIngestionOptionFalsy(const std::vector<std::string>& options,
                             const std::string& key) {
  return IsFalsyValue(DmlIngestionOptionValue(options, key));
}

DmlIngestionPipelineConfig ApplyDmlIngestionOptions(
    DmlIngestionPipelineConfig config) {
  const auto& options = config.option_envelopes;
  config.enable_preallocator =
      !DmlIngestionOptionFalsy(options, "dml.ingest.preallocator") &&
      !DmlIngestionOptionFalsy(options, "dml.ingestion.preallocator");
  config.enable_writer =
      !DmlIngestionOptionFalsy(options, "dml.ingest.writer") &&
      !DmlIngestionOptionFalsy(options, "dml.ingestion.writer");
  if (DmlIngestionOptionTruthy(options, "dml.ingest.preallocator") ||
      DmlIngestionOptionTruthy(options, "dml.ingestion.preallocator")) {
    config.enable_preallocator = true;
  }
  if (DmlIngestionOptionTruthy(options, "dml.ingest.writer") ||
      DmlIngestionOptionTruthy(options, "dml.ingestion.writer")) {
    config.enable_writer = true;
  }
  config.min_rows_for_preallocator =
      std::max<EngineApiU64>(1,
          DmlIngestionOptionU64(options,
                                "dml.ingest.preallocator.min_rows",
                                config.min_rows_for_preallocator));
  config.max_preallocator_queue_rows =
      std::max<EngineApiU64>(1,
          DmlIngestionOptionU64(options,
                                "dml.ingest.preallocator.max_rows",
                                config.max_preallocator_queue_rows));
  config.max_preallocator_queue_bytes =
      std::max<EngineApiU64>(1,
          DmlIngestionOptionU64(options,
                                "dml.ingest.preallocator.max_bytes",
                                config.max_preallocator_queue_bytes));
  config.writer_worker_count =
      std::max<EngineApiU64>(1,
          DmlIngestionOptionU64(options,
                                "dml.ingest.writer_workers",
                                config.writer_worker_count));
  config.source_size_bytes =
      DmlIngestionOptionU64(options,
                            "dml.ingest.source_size_bytes",
                            config.source_size_bytes);
  config.source_size_bytes =
      DmlIngestionOptionU64(options,
                            "copy.source_size_bytes",
                            config.source_size_bytes);
  config.source_size_bytes =
      DmlIngestionOptionU64(options,
                            "copy_source_size_bytes",
                            config.source_size_bytes);
  config.source_preallocation_bytes =
      DmlIngestionOptionU64(options,
                            "dml.ingest.preallocation_bytes",
                            config.source_preallocation_bytes);
  config.source_preallocation_bytes =
      DmlIngestionOptionU64(options,
                            "copy.preallocation_bytes",
                            config.source_preallocation_bytes);
  config.source_preallocation_bytes =
      DmlIngestionOptionU64(options,
                            "copy_preallocation_bytes",
                            config.source_preallocation_bytes);
  config.source_preallocation_factor_percent =
      std::max<EngineApiU64>(1,
          DmlIngestionOptionU64(options,
                                "dml.ingest.preallocation_factor_percent",
                                config.source_preallocation_factor_percent));
  config.source_preallocation_factor_percent =
      std::max<EngineApiU64>(1,
          DmlIngestionOptionU64(options,
                                "copy.preallocation_factor_percent",
                                config.source_preallocation_factor_percent));
  config.preallocation_page_size_bytes =
      std::max<EngineApiU64>(1024,
          DmlIngestionOptionU64(options,
                                "dml.ingest.preallocation_page_size_bytes",
                                config.preallocation_page_size_bytes));
  config.preallocation_page_size_bytes =
      std::max<EngineApiU64>(1024,
          DmlIngestionOptionU64(options,
                                "physical_mga_cow.page_size_bytes",
                                config.preallocation_page_size_bytes));
  return config;
}

DmlIngestionPipeline::DmlIngestionPipeline(DmlIngestionPipelineConfig config)
    : config_(ApplyDmlIngestionOptions(std::move(config))) {
  stats_.diagnostic = OkDiagnostic();
  stats_.source_size_bytes = config_.source_size_bytes;
  stats_.source_preallocation_bytes = SourcePreallocationBytes();
  stats_.source_preallocation_pages = SourcePreallocationPages();
}

DmlIngestionPipeline::~DmlIngestionPipeline() {
  (void)Fence();
}

bool DmlIngestionPipeline::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return !stats_.failed;
  }
  started_ = true;
  stats_.preallocator_enabled = PreallocatorShouldRun();
  stats_.writer_enabled = WriterShouldRun();
  stats_.writer_worker_count = stats_.writer_enabled ? config_.writer_worker_count : 0;
  EnqueueSourceSizeHintLocked();
  StartPreallocatorIfNeeded();
  StartWritersIfNeeded();
  return !stats_.failed;
}

bool DmlIngestionPipeline::PreallocatorShouldRun() const {
  if (!config_.enable_preallocator || config_.target_table_uuid.empty()) {
    return false;
  }
  if (config_.source_size_bytes != 0 || config_.source_preallocation_bytes != 0) {
    return true;
  }
  return config_.input_row_count >= config_.min_rows_for_preallocator;
}

bool DmlIngestionPipeline::WriterShouldRun() const {
  if (!config_.enable_writer) {
    return false;
  }
  return config_.input_row_count != 0 || config_.source_size_bytes != 0;
}

EngineApiU64 DmlIngestionPipeline::SourcePreallocationBytes() const {
  if (config_.source_preallocation_bytes != 0) {
    return config_.source_preallocation_bytes;
  }
  if (config_.source_size_bytes == 0) {
    return 0;
  }
  if (config_.source_size_bytes >
      std::numeric_limits<EngineApiU64>::max() /
          std::max<EngineApiU64>(1, config_.source_preallocation_factor_percent)) {
    return config_.source_size_bytes;
  }
  return (config_.source_size_bytes * config_.source_preallocation_factor_percent + 99) / 100;
}

EngineApiU64 DmlIngestionPipeline::SourcePreallocationPages() const {
  return CeilDiv(SourcePreallocationBytes(), config_.preallocation_page_size_bytes);
}

bool DmlIngestionPipeline::PreworkFitsLocked(
    const PreworkQueueItem& item) const {
  const bool row_space =
      prework_queue_.size() < config_.max_preallocator_queue_rows;
  const bool byte_space =
      prework_queued_bytes_ + item.encoded_bytes <=
          config_.max_preallocator_queue_bytes ||
      prework_queue_.empty();
  return row_space && byte_space;
}

void DmlIngestionPipeline::EnqueueSourceSizeHintLocked() {
  const EngineApiU64 pages = SourcePreallocationPages();
  if (pages == 0 || !PreallocatorShouldRun()) {
    return;
  }
  PreworkQueueItem item;
  item.source_hint_pages = pages;
  item.source_hint_bytes = SourcePreallocationBytes();
  prework_queue_.push_back(std::move(item));
  stats_.source_size_hint_consumed = true;
  stats_.preallocation_max_depth =
      std::max<EngineApiU64>(stats_.preallocation_max_depth,
                             static_cast<EngineApiU64>(prework_queue_.size()));
}

void DmlIngestionPipeline::StartPreallocatorIfNeeded() {
  if (!stats_.preallocator_enabled || stats_.preallocator_thread_started) {
    return;
  }
  stats_.preallocator_thread_started = true;
  preallocator_worker_ = std::thread([this]() { PreallocatorLoop(); });
  prework_available_.notify_all();
}

void DmlIngestionPipeline::StartWritersIfNeeded() {
  if (!stats_.writer_enabled || !writer_workers_.empty()) {
    return;
  }
  for (EngineApiU64 index = 0; index < config_.writer_worker_count; ++index) {
    writer_workers_.emplace_back([this]() { WriterLoop(); });
  }
}

bool DmlIngestionPipeline::EnqueuePreallocation(
    DmlIngestionPreallocationItem item) {
  std::vector<DmlIngestionPreallocationItem> items;
  items.push_back(std::move(item));
  return EnqueuePreallocationBatch(std::move(items));
}

bool DmlIngestionPipeline::EnqueuePreallocationBatch(
    std::vector<DmlIngestionPreallocationItem> items) {
  if (!Start()) {
    return false;
  }
  if (items.empty()) {
    return true;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (!stats_.preallocator_enabled) {
    return true;
  }
  for (auto& item : items) {
    PreworkQueueItem queued;
    queued.logical_values = std::move(item.logical_values);
    queued.borrowed_logical_values = item.borrowed_logical_values;
    queued.encoded_bytes = item.encoded_bytes;
    while (!prework_stop_requested_ && !stats_.failed &&
           !PreworkFitsLocked(queued)) {
      ++stats_.preallocation_wait_count;
      prework_space_available_.wait(lock);
    }
    if (prework_stop_requested_ || stats_.failed) {
      return false;
    }
    prework_queued_bytes_ += queued.encoded_bytes;
    prework_queue_.push_back(std::move(queued));
    ++stats_.preallocation_rows_enqueued;
    stats_.preallocation_bytes_enqueued += item.encoded_bytes;
    stats_.preallocation_max_depth =
        std::max<EngineApiU64>(stats_.preallocation_max_depth,
                               static_cast<EngineApiU64>(prework_queue_.size()));
    prework_available_.notify_one();
  }
  prework_available_.notify_one();
  return true;
}

bool DmlIngestionPipeline::EnqueueWrite(DmlIngestionWriteTask task) {
  if (!Start()) {
    return false;
  }
  if (!task.execute) {
    SetFailure(PipelineDiagnostic("write_task_callback_required"));
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stats_.writer_enabled) {
    const auto diagnostic = task.execute();
    if (diagnostic.error) {
      stats_.failed = true;
      stats_.diagnostic = diagnostic;
      return false;
    }
    ++stats_.write_tasks_enqueued;
    ++stats_.write_tasks_completed;
    stats_.write_rows_enqueued += task.row_count;
    stats_.write_rows_completed += task.row_count;
    write_drained_.notify_all();
    return true;
  }
  write_queue_.push_back(std::move(task));
  ++stats_.write_tasks_enqueued;
  stats_.write_rows_enqueued += write_queue_.back().row_count;
  stats_.write_queue_max_depth =
      std::max<EngineApiU64>(stats_.write_queue_max_depth,
                             static_cast<EngineApiU64>(write_queue_.size()));
  write_available_.notify_one();
  return true;
}

DmlIngestionPipelineStats DmlIngestionPipeline::FencePreallocator() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    prework_stop_requested_ = true;
    prework_available_.notify_all();
    prework_space_available_.notify_all();
  }
  if (preallocator_worker_.joinable()) {
    preallocator_worker_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

DmlIngestionPipelineStats DmlIngestionPipeline::DrainWriters() {
  std::unique_lock<std::mutex> lock(mutex_);
  write_drained_.wait(lock, [&]() {
    return stats_.failed ||
           stats_.write_tasks_completed >= stats_.write_tasks_enqueued;
  });
  return stats_;
}

void DmlIngestionPipeline::PreallocatorLoop() {
  for (;;) {
    std::deque<PreworkQueueItem> work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      prework_available_.wait(lock, [&]() {
        return prework_stop_requested_ || !prework_queue_.empty() ||
               stats_.failed;
      });
      if ((prework_queue_.empty() && prework_stop_requested_) || stats_.failed) {
        return;
      }
      while (!prework_queue_.empty()) {
        prework_queued_bytes_ -= prework_queue_.front().encoded_bytes;
        work.push_back(std::move(prework_queue_.front()));
        prework_queue_.pop_front();
      }
      prework_space_available_.notify_all();
    }
    ProcessPreallocationBatch(std::move(work));
  }
}

void DmlIngestionPipeline::ProcessPreallocationBatch(
    std::deque<PreworkQueueItem> work) {
  if (work.empty()) {
    return;
  }
  EngineApiU64 row_count = 0;
  EngineApiU64 source_hint_pages = 0;
  EngineApiU64 source_hint_bytes = 0;
  std::vector<const std::vector<std::pair<std::string, std::string>>*> index_value_refs;
  index_value_refs.reserve(work.size());
  for (auto& item : work) {
    if (item.source_hint_pages != 0) {
      source_hint_pages += item.source_hint_pages;
      source_hint_bytes += item.source_hint_bytes;
      continue;
    }
    ++row_count;
    index_value_refs.push_back(item.borrowed_logical_values != nullptr
                                   ? item.borrowed_logical_values
                                   : &item.logical_values);
  }

  if (source_hint_pages != 0) {
    const auto start = PipelineClock::now();
    auto allocation = ReserveDmlPageAllocationRuntime(
        config_.context,
        config_.option_envelopes,
        config_.target_table_uuid,
        DmlPageAllocationRuntimeFamily::row_data,
        source_hint_pages,
        config_.lane_operation + ".source_size_preallocation");
    const EngineApiU64 elapsed = ElapsedMicros(start, PipelineClock::now());
    if (!allocation.ok()) {
      SetFailure(allocation.diagnostic);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.allocations.push_back({std::move(allocation),
                                  "row_source_size_hint",
                                  0,
                                  source_hint_pages,
                                  elapsed});
    stats_.source_preallocation_bytes =
        std::max(stats_.source_preallocation_bytes, source_hint_bytes);
    stats_.source_preallocation_pages =
        std::max(stats_.source_preallocation_pages, source_hint_pages);
  }

  if (row_count == 0) {
    return;
  }

  const auto row_start = PipelineClock::now();
  auto row_allocation = ReserveDmlPageAllocationRuntime(
      config_.context,
      config_.option_envelopes,
      config_.target_table_uuid,
      DmlPageAllocationRuntimeFamily::row_data,
      row_count,
      config_.lane_operation + ".ingestion_preallocator.row_data");
  const EngineApiU64 row_elapsed = ElapsedMicros(row_start, PipelineClock::now());
  if (!row_allocation.ok()) {
    SetFailure(row_allocation.diagnostic);
    return;
  }

  DmlPageAllocationRuntimeResult index_allocation;
  EngineApiU64 index_elapsed = 0;
  if (config_.state != nullptr && !index_value_refs.empty()) {
    const auto index_start = PipelineClock::now();
    index_allocation = ReserveDmlIndexPageAllocationRuntimeForRowRefs(
        config_.context,
        config_.option_envelopes,
        *config_.state,
        config_.target_table_uuid,
        index_value_refs,
        config_.lane_operation + ".ingestion_preallocator.index");
    index_elapsed = ElapsedMicros(index_start, PipelineClock::now());
    if (!index_allocation.ok()) {
      SetFailure(index_allocation.diagnostic);
      return;
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  stats_.allocations.push_back(
      {std::move(row_allocation), "row", row_count, row_count, row_elapsed});
  if (stats_.allocations.back().allocation.active) {
    stats_.row_prework_rows += row_count;
  }
  stats_.allocations.push_back(
      {std::move(index_allocation), "index", row_count, 0, index_elapsed});
  if (stats_.allocations.back().allocation.active) {
    stats_.index_prework_rows += row_count;
  }
}

void DmlIngestionPipeline::WriterLoop() {
  for (;;) {
    DmlIngestionWriteTask task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      write_available_.wait(lock, [&]() {
        return write_stop_requested_ || !write_queue_.empty() ||
               stats_.failed;
      });
      if ((write_queue_.empty() && write_stop_requested_) || stats_.failed) {
        return;
      }
      task = std::move(write_queue_.front());
      write_queue_.pop_front();
    }
    EngineApiDiagnostic diagnostic = OkDiagnostic();
    try {
      diagnostic = task.execute();
    } catch (...) {
      diagnostic = PipelineDiagnostic("write_task_exception");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (diagnostic.error) {
      stats_.failed = true;
      stats_.diagnostic = diagnostic;
      write_stop_requested_ = true;
      prework_stop_requested_ = true;
      write_available_.notify_all();
      write_drained_.notify_all();
      prework_available_.notify_all();
      prework_space_available_.notify_all();
      return;
    }
    ++stats_.write_tasks_completed;
    stats_.write_rows_completed += task.row_count;
    write_drained_.notify_all();
  }
}

void DmlIngestionPipeline::SetFailure(EngineApiDiagnostic diagnostic) {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.failed = true;
  stats_.diagnostic = std::move(diagnostic);
  prework_stop_requested_ = true;
  write_stop_requested_ = true;
  prework_available_.notify_all();
  prework_space_available_.notify_all();
  write_available_.notify_all();
  write_drained_.notify_all();
}

DmlIngestionPipelineStats DmlIngestionPipeline::Fence() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fenced_) {
      return stats_;
    }
    fenced_ = true;
    prework_stop_requested_ = true;
    write_stop_requested_ = true;
    prework_available_.notify_all();
    prework_space_available_.notify_all();
    write_available_.notify_all();
    write_drained_.notify_all();
  }
  if (preallocator_worker_.joinable()) {
    preallocator_worker_.join();
  }
  for (auto& worker : writer_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

DmlIngestionPipelineStats DmlIngestionPipeline::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void AddDmlIngestionPipelineEvidence(const DmlIngestionPipelineStats& stats,
                                     EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"dml_ingestion_pipeline",
                              stats.writer_enabled || stats.preallocator_enabled
                                  ? "enabled"
                                  : "not_enabled"});
  result->evidence.push_back({"dml_ingestion_preallocator_enabled",
                              stats.preallocator_enabled ? "true" : "false"});
  result->evidence.push_back({"dml_ingestion_preallocator_thread_started",
                              stats.preallocator_thread_started ? "true" : "false"});
  result->evidence.push_back({"dml_ingestion_preallocation_rows_enqueued",
                              std::to_string(stats.preallocation_rows_enqueued)});
  result->evidence.push_back({"dml_ingestion_preallocation_bytes_enqueued",
                              std::to_string(stats.preallocation_bytes_enqueued)});
  result->evidence.push_back({"dml_ingestion_row_prework_rows",
                              std::to_string(stats.row_prework_rows)});
  result->evidence.push_back({"dml_ingestion_index_prework_rows",
                              std::to_string(stats.index_prework_rows)});
  result->evidence.push_back({"dml_ingestion_preallocation_queue_max_depth",
                              std::to_string(stats.preallocation_max_depth)});
  result->evidence.push_back({"dml_ingestion_preallocation_wait_count",
                              std::to_string(stats.preallocation_wait_count)});
  result->evidence.push_back({"dml_ingestion_source_size_bytes",
                              std::to_string(stats.source_size_bytes)});
  result->evidence.push_back({"dml_ingestion_source_size_hint_consumed",
                              stats.source_size_hint_consumed ? "true" : "false"});
  result->evidence.push_back({"dml_ingestion_source_preallocation_bytes",
                              std::to_string(stats.source_preallocation_bytes)});
  result->evidence.push_back({"dml_ingestion_source_preallocation_pages",
                              std::to_string(stats.source_preallocation_pages)});
  result->evidence.push_back({"dml_ingestion_writer_enabled",
                              stats.writer_enabled ? "true" : "false"});
  result->evidence.push_back({"dml_ingestion_writer_worker_count",
                              std::to_string(stats.writer_worker_count)});
  result->evidence.push_back({"dml_ingestion_write_tasks_enqueued",
                              std::to_string(stats.write_tasks_enqueued)});
  result->evidence.push_back({"dml_ingestion_write_tasks_completed",
                              std::to_string(stats.write_tasks_completed)});
  result->evidence.push_back({"dml_ingestion_write_rows_enqueued",
                              std::to_string(stats.write_rows_enqueued)});
  result->evidence.push_back({"dml_ingestion_write_rows_completed",
                              std::to_string(stats.write_rows_completed)});
  result->evidence.push_back({"dml_ingestion_write_queue_max_depth",
                              std::to_string(stats.write_queue_max_depth)});
  result->evidence.push_back({"dml_ingestion_write_wait_count",
                              std::to_string(stats.write_wait_count)});
  result->evidence.push_back({"dml_ingestion_return_before_flush", "false"});
  result->evidence.push_back({"dml_ingestion_commit_fence",
                              "statement_queue_drained_before_result"});
  result->evidence.push_back({"mga_finality_authority",
                              "engine_transaction_inventory"});
  result->evidence.push_back({"parser_finality", "false"});
  if (stats.failed) {
    result->evidence.push_back({"dml_ingestion_pipeline_failed", "true"});
    result->evidence.push_back({"dml_ingestion_pipeline_diagnostic",
                                stats.diagnostic.code});
  }
  for (const auto& record : stats.allocations) {
    AddDmlPageAllocationRuntimeEvidence(record.allocation, result);
    AddAllocationSummary(record.allocation, &result->dml_summary);
    if (record.allocation.active) {
      result->evidence.push_back({"dml_ingestion_allocation_family",
                                  record.family});
      result->evidence.push_back({"dml_ingestion_allocation_rows",
                                  std::to_string(record.row_count)});
      result->evidence.push_back({"dml_ingestion_allocation_requested_pages",
                                  std::to_string(record.requested_pages)});
      result->evidence.push_back({"dml_ingestion_allocation_elapsed_us",
                                  std::to_string(record.elapsed_microseconds)});
    }
  }
}

}  // namespace scratchbird::engine::internal_api
