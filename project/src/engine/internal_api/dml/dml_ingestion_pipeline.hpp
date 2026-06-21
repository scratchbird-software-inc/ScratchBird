// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

struct DmlIngestionPipelineConfig {
  EngineRequestContext context;
  std::vector<std::string> option_envelopes;
  const CrudState* state = nullptr;
  std::string operation_id = "dml.ingestion";
  std::string lane_operation = "insert_rows";
  std::string target_table_uuid;
  EngineApiU64 input_row_count = 0;
  EngineApiU64 source_size_bytes = 0;
  EngineApiU64 source_preallocation_bytes = 0;
  EngineApiU64 source_preallocation_factor_percent = 82;
  EngineApiU64 preallocation_page_size_bytes = 16 * 1024;
  EngineApiU64 min_rows_for_preallocator = 8;
  EngineApiU64 max_preallocator_queue_rows = 4096;
  EngineApiU64 max_preallocator_queue_bytes = 8 * 1024 * 1024;
  EngineApiU64 writer_worker_count = 1;
  bool enable_preallocator = true;
  bool enable_writer = true;
};

struct DmlIngestionPreallocationItem {
  std::vector<std::pair<std::string, std::string>> logical_values;
  const std::vector<std::pair<std::string, std::string>>* borrowed_logical_values = nullptr;
  EngineApiU64 encoded_bytes = 0;
};

struct DmlIngestionWriteTask {
  std::string phase;
  EngineApiU64 row_count = 0;
  std::function<EngineApiDiagnostic()> execute;
};

struct DmlIngestionAllocationRecord {
  DmlPageAllocationRuntimeResult allocation;
  std::string family;
  EngineApiU64 row_count = 0;
  EngineApiU64 requested_pages = 0;
  EngineApiU64 elapsed_microseconds = 0;
};

struct DmlIngestionPipelineStats {
  bool preallocator_enabled = false;
  bool preallocator_thread_started = false;
  bool writer_enabled = false;
  EngineApiU64 writer_worker_count = 0;
  EngineApiU64 preallocation_rows_enqueued = 0;
  EngineApiU64 preallocation_bytes_enqueued = 0;
  EngineApiU64 preallocation_max_depth = 0;
  EngineApiU64 preallocation_wait_count = 0;
  EngineApiU64 row_prework_rows = 0;
  EngineApiU64 index_prework_rows = 0;
  EngineApiU64 source_size_bytes = 0;
  EngineApiU64 source_preallocation_bytes = 0;
  EngineApiU64 source_preallocation_pages = 0;
  bool source_size_hint_consumed = false;
  EngineApiU64 write_tasks_enqueued = 0;
  EngineApiU64 write_tasks_completed = 0;
  EngineApiU64 write_rows_enqueued = 0;
  EngineApiU64 write_rows_completed = 0;
  EngineApiU64 write_queue_max_depth = 0;
  EngineApiU64 write_wait_count = 0;
  bool failed = false;
  EngineApiDiagnostic diagnostic;
  std::vector<DmlIngestionAllocationRecord> allocations;
};

class DmlIngestionPipeline {
 public:
  explicit DmlIngestionPipeline(DmlIngestionPipelineConfig config);
  ~DmlIngestionPipeline();

  DmlIngestionPipeline(const DmlIngestionPipeline&) = delete;
  DmlIngestionPipeline& operator=(const DmlIngestionPipeline&) = delete;

  bool Start();
  bool EnqueuePreallocation(DmlIngestionPreallocationItem item);
  bool EnqueuePreallocationBatch(std::vector<DmlIngestionPreallocationItem> items);
  bool EnqueueWrite(DmlIngestionWriteTask task);
  DmlIngestionPipelineStats FencePreallocator();
  DmlIngestionPipelineStats DrainWriters();
  DmlIngestionPipelineStats Fence();
  DmlIngestionPipelineStats Snapshot() const;

 private:
  struct PreworkQueueItem {
    std::vector<std::pair<std::string, std::string>> logical_values;
    const std::vector<std::pair<std::string, std::string>>* borrowed_logical_values = nullptr;
    EngineApiU64 encoded_bytes = 0;
    EngineApiU64 source_hint_pages = 0;
    EngineApiU64 source_hint_bytes = 0;
  };

  void StartPreallocatorIfNeeded();
  void StartWritersIfNeeded();
  void EnqueueSourceSizeHintLocked();
  void PreallocatorLoop();
  void WriterLoop();
  void ProcessPreallocationBatch(std::deque<PreworkQueueItem> work);
  void SetFailure(EngineApiDiagnostic diagnostic);
  bool PreallocatorShouldRun() const;
  bool WriterShouldRun() const;
  EngineApiU64 SourcePreallocationBytes() const;
  EngineApiU64 SourcePreallocationPages() const;
  bool PreworkFitsLocked(const PreworkQueueItem& item) const;

  DmlIngestionPipelineConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable prework_available_;
  std::condition_variable prework_space_available_;
  std::condition_variable write_available_;
  std::condition_variable write_drained_;
  std::deque<PreworkQueueItem> prework_queue_;
  std::deque<DmlIngestionWriteTask> write_queue_;
  EngineApiU64 prework_queued_bytes_ = 0;
  bool started_ = false;
  bool prework_stop_requested_ = false;
  bool write_stop_requested_ = false;
  bool fenced_ = false;
  std::thread preallocator_worker_;
  std::vector<std::thread> writer_workers_;
  DmlIngestionPipelineStats stats_;
};

EngineApiU64 DmlIngestionOptionU64(const std::vector<std::string>& options,
                                   const std::string& key,
                                   EngineApiU64 fallback);
std::string DmlIngestionOptionValue(const std::vector<std::string>& options,
                                    const std::string& key);
bool DmlIngestionOptionTruthy(const std::vector<std::string>& options,
                              const std::string& key);
bool DmlIngestionOptionFalsy(const std::vector<std::string>& options,
                             const std::string& key);
DmlIngestionPipelineConfig ApplyDmlIngestionOptions(
    DmlIngestionPipelineConfig config);
void AddDmlIngestionPipelineEvidence(const DmlIngestionPipelineStats& stats,
                                     EngineApiResult* result);

}  // namespace scratchbird::engine::internal_api
