// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_ingestion_pipeline.hpp"
#include "api_diagnostics.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace allocation_fault {
thread_local bool next = false;
}
void* operator new(std::size_t bytes) {
  if (allocation_fault::next) {
    allocation_fault::next=false;
    throw std::bad_alloc();
  }
  if (auto* result=std::malloc(bytes ? bytes : 1)) return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer,std::size_t) noexcept { std::free(pointer); }

namespace api = scratchbird::engine::internal_api;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}

auto Ok() {
  return api::MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

api::DmlIngestionPipelineConfig Config(bool asynchronous = true) {
  api::DmlIngestionPipelineConfig config;
  config.enable_preallocator = false;
  config.enable_writer = asynchronous;
  config.option_envelopes = {"dml.ingest.preallocator=false"};
  if (!asynchronous) config.option_envelopes.push_back("dml.ingest.writer=false");
  config.input_row_count = 3;
  config.writer_worker_count = 2;
  return config;
}

template<class Predicate> void Await(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  Require(predicate(), message);
}

void FailedDrainWaitsForOtherWriter(bool throws) {
  api::DmlIngestionPipeline pipeline(Config());
  std::promise<void> entered, release, fail;
  auto released = release.get_future().share();
  auto fail_now = fail.get_future().share();
  std::atomic<unsigned> effects = 0, cancelled_effects = 0;
  api::DmlIngestionWriteScope scope(pipeline);
  Require(pipeline.EnqueueWrite({"retained_writer", 7, [&] {
    entered.set_value();
    released.wait();
    ++effects;
    return Ok();
  }}), "retained writer admission failed");
  Require(entered.get_future().wait_for(5s) == std::future_status::ready,
          "retained writer never started");
  Require(pipeline.EnqueueWrite({"failing_writer", 11, [&]() -> api::EngineApiDiagnostic {
    fail_now.wait();
    if (throws) throw std::runtime_error("callback failure");
    return api::MakeInvalidRequestDiagnostic("dml.ingestion_pipeline", "test_failure");
  }}), "failing writer admission failed");
  Require(pipeline.EnqueueWrite({"must_not_run_after_failure", 13, [&] {
    ++cancelled_effects;
    return Ok();
  }}), "queued writer admission failed");
  fail.set_value();
  Await([&] { return pipeline.Snapshot().failed; }, "failure not published");
  std::promise<void> draining;
  auto drain = std::async(std::launch::async, [&] {
    draining.set_value();
    return pipeline.DrainWriters();
  });
  draining.get_future().wait();
  Require(drain.wait_for(100ms) == std::future_status::timeout,
          "failed drain returned while another writer still owned captures");
  Require(effects == 0 && cancelled_effects == 0, "unexpected pre-release effect");
  release.set_value();
  const auto receipt = drain.get();
  Require(receipt.failed && receipt.diagnostic.error && effects == 1 && cancelled_effects == 0,
          "failed drain lost failure or retained effects");
  Require(receipt.write_tasks_enqueued == 3 && receipt.write_tasks_completed == 1 &&
              receipt.write_rows_enqueued == 31 && receipt.write_rows_completed == 7,
          "failed/cancelled tasks were reported as successful effects");
  const auto fenced = pipeline.Fence();
  Require(fenced.failed && fenced.write_tasks_completed == 1, "fence changed partial outcome");
  Require(!pipeline.EnqueueWrite({"late", 1, [] { return Ok(); }}),
          "failed/fenced pipeline accepted more work");
}

void ConcurrentFencesAndClosedAdmission() {
  api::DmlIngestionPipeline pipeline(Config());
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  Require(pipeline.EnqueueWrite({"held", 1, [&] {
    entered.set_value();
    released.wait();
    return Ok();
  }}), "held writer refused");
  entered.get_future().wait();
  auto first = std::async(std::launch::async, [&] { return pipeline.Fence(); });
  Await([&] { return !pipeline.Start(); }, "fence did not close admission");
  auto second = std::async(std::launch::async, [&] { return pipeline.Fence(); });
  Require(first.wait_for(100ms) == std::future_status::timeout &&
              second.wait_for(100ms) == std::future_status::timeout,
          "concurrent fence returned before joined completion");
  Require(!pipeline.EnqueueWrite({"late", 1, [] { return Ok(); }}) &&
              !pipeline.EnqueuePreallocation({}), "closed pipeline admitted work");
  release.set_value();
  for (auto* future : {&first, &second}) {
    const auto result = future->get();
    Require(!result.failed && result.write_tasks_completed == 1, "incomplete joined receipt");
  }
  pipeline.StopAndJoin();
  Require(!pipeline.Start(), "joined pipeline restarted");
  api::DmlIngestionPipeline never_started(Config());
  never_started.StopAndJoin();
  Require(!never_started.Start(), "closed unused pipeline started");
}

void ExceptionUnwindPreservesCaptureLifetime() {
  api::DmlIngestionPipeline pipeline(Config()); // Deliberately before captures.
  std::atomic<bool> alive = true, unwinding = false, accessed_alive = false;
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto releaser = std::async(std::launch::async, [&] {
    Await([&] { return unwinding.load(); }, "owner did not unwind");
    // Start returning false proves scope guard closed admission on unwind.
    Await([&] { return !pipeline.Start(); }, "unwind guard did not close admission");
    Require(alive, "callback capture destroyed before worker join");
    release.set_value();
  });
  try {
    struct Captured {
      std::atomic<bool>& alive;
      ~Captured() { alive = false; }
    } capture{alive};
    api::DmlIngestionWriteScope scope(pipeline);
    Require(pipeline.EnqueueWrite({"borrowed", 1, [&] {
      entered.set_value();
      released.wait();
      accessed_alive = capture.alive.load();
      return Ok();
    }}), "borrowed writer refused");
    entered.get_future().wait();
    unwinding = true;
    throw std::runtime_error("owner unwind");
  } catch (const std::runtime_error&) {}
  releaser.get();
  Require(accessed_alive && !alive && pipeline.Snapshot().write_tasks_completed == 1,
          "scope unwind did not retain captures until completion");
}

void InlineExceptionAndReentrantSnapshot() {
  api::DmlIngestionPipeline pipeline(Config(false));
  Require(pipeline.EnqueueWrite({"inline_success", 3, [&] {
    Require(pipeline.Snapshot().write_tasks_completed == 0, "inline snapshot mismatch");
    return Ok();
  }}), "inline success refused");
  Require(!pipeline.EnqueueWrite({"inline_exception", 5, []() -> api::EngineApiDiagnostic {
    throw std::runtime_error("inline failure");
  }}), "inline exception accepted");
  const auto receipt = pipeline.DrainWriters();
  Require(receipt.failed && receipt.diagnostic.error && receipt.write_tasks_enqueued == 2 &&
              receipt.write_tasks_completed == 1 && receipt.write_rows_completed == 3,
          "inline exception outcome lost");
}

void InlineDiagnosticAllocationFailureDrains(bool during_catch) {
  api::DmlIngestionPipeline pipeline(Config(false));
  Require(pipeline.Start(),"inline allocation-fault pipeline startup");
  unsigned calls=0;
  api::DmlIngestionWriteTask task{"inline_allocation_failure",1,[&]() -> api::EngineApiDiagnostic {
    ++calls;
    allocation_fault::next=true;
    throw std::bad_alloc(); // Catch-diagnostic construction must also unwind safely.
  }};
  if (!during_catch) allocation_fault::next=true;
  bool failed=false;
  try { (void)pipeline.EnqueueWrite(std::move(task)); }
  catch (const std::bad_alloc&) { failed=true; }
  Require(failed && !allocation_fault::next && calls==(during_catch?1u:0u),
      "actual inline pre-callback/catch diagnostic allocation failure was not exercised");
  allocation_fault::next=false;
  // Before the repair this blocks forever on an orphaned active_writers_.
  const auto drained=pipeline.DrainWriters();
  Require(drained.failed && drained.diagnostic.error &&
      drained.diagnostic.code=="SBLR.EXECUTION_FAILED" &&
      drained.diagnostic.message_key=="engine.dml.ingestion.exceptional_completion" &&
      drained.diagnostic.detail=="inline_writer_diagnostic_construction_failed",
      "diagnostic OOM must retain an owned failure and cannot strand an incomplete task drain");
  pipeline.StopAndJoin();
  const auto receipt=pipeline.Snapshot();
  Require(receipt.write_tasks_enqueued==1 && receipt.write_tasks_completed==0 &&
      receipt.write_rows_completed==0 && !pipeline.Start(),
      "diagnostic OOM must not report completed effects or reopen admission");
}

int main() {
  FailedDrainWaitsForOtherWriter(false);
  FailedDrainWaitsForOtherWriter(true);
  ConcurrentFencesAndClosedAdmission();
  ExceptionUnwindPreservesCaptureLifetime();
  InlineExceptionAndReentrantSnapshot();
  InlineDiagnosticAllocationFailureDrains(false);
  InlineDiagnosticAllocationFailureDrains(true);
}
