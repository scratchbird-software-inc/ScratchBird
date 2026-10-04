// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "observability/native_committed_metric_observation.hpp"
#include "observability/native_metric_recording.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "metric_observation_queue.hpp"
#include "metric_builtin_definitions.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <cerrno>
#include <pthread.h>
#include <thread>
#include <array>
#include <latch>

namespace {
off_t injected_offset = -1;
unsigned injected_mode = 0, injected_count = 0;
unsigned native_write_calls = 0;
pthread_mutex_t* queue_mutex = nullptr;
unsigned queue_lock_calls = 0, busy_on_queue_lock = 0, queue_busy_faults = 0;
}
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
  // Deterministic native try-lock failure, not a claim of contention scheduling.
  // The first call after arming is the actual queue lease preflight. Only that
  // mutex is targeted; native storage guards remain unchanged.
  if (busy_on_queue_lock) {
    if (!queue_mutex) queue_mutex = mutex;
    if (mutex == queue_mutex && ++queue_lock_calls == busy_on_queue_lock) {
      busy_on_queue_lock = 0; ++queue_busy_faults; return EBUSY;
    }
  }
  return __real_pthread_mutex_trylock(mutex);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t size, off_t offset) {
  ++native_write_calls;
  if (injected_mode && offset == injected_offset) {
    const auto mode = injected_mode; injected_mode = 0; ++injected_count;
    if (mode == 1) { errno = EIO; return -1; }
    const auto written = __real_pwrite(fd, bytes, size, offset);
    if (written == static_cast<ssize_t>(size)) throw std::bad_alloc();
    return written;
  }
  return __real_pwrite(fd, bytes, size, offset);
}

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace p = scratchbird::core::platform;
namespace m = scratchbird::core::metrics;
namespace mga = scratchbird::transaction::mga;
namespace types = scratchbird::core::datatypes;
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
p::TypedUuid Id(p::UuidKind kind = p::UuidKind::object) {
  const auto id = scratchbird::core::uuid::GenerateDurableEngineIdentityV7(kind, 1790000000000ull);
  Check(id.ok(), "native identity issuance");
  return id.value;
}
struct Fixture {
  std::filesystem::path root;
  std::string path;
  disk::FileDevice device;
  p::TypedUuid database = Id(p::UuidKind::database), relation = Id();
  p::Uuid node = Id().value;
  p::u32 page_size;
  p::u64 page;
  std::shared_ptr<m::MetricObservationQueue> queue;
  m::MetricRegistry registry;
  m::MetricDescriptor descriptor;
  m::MetricSeriesIdentity series;
  m::MetricObservationLease lease;
  Fixture(p::u32 size) : page_size(size) {
    std::string pattern = (std::filesystem::temp_directory_path() / "metric_commit.XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    const auto made = ::mkdtemp(name.data()); Check(made, "private fixture directory"); root = made;
    try {
      path = (root / "node.sbdb").string();
      db::DatabaseCreateConfig config;
      config.path = path; config.database_uuid = database; config.filespace_uuid = Id(p::UuidKind::filespace);
      config.page_size = size; config.creation_unix_epoch_millis = 1790000000100ull;
      config.allow_minimal_resource_bootstrap = true; config.require_resource_seed_pack = false;
      config.require_bootstrap_principal = false; config.allow_uncredentialed_bootstrap = true;
      Check(db::CreateDatabaseFile(config).ok(), "real database creation");
      Check(device.Open(path, disk::FileOpenMode::open_existing).ok(), "retain native device");
      const auto length = device.Size(); Check(length.ok(), "native size"); page = length.size_bytes / size;
      auto created = m::MetricObservationQueue::Create({database.value, node, {}}, {8, 65536});
      Check(created.ok(), "real queue"); queue = std::move(created.queue);
      Check(registry.BindObservationQueue(queue).ok, "bind owned queue");
      // Explicit component binding, not native catalog/source-policy activation.
      const auto definitions = m::BuiltinMetricDescriptorDefinitions();
      for (const auto& definition : definitions)
        if (definition.family == "sb_agent_runtime_service_history_records")
          static_cast<m::MetricDescriptorDefinition&>(descriptor) = definition;
      descriptor.metric_uuid = Id().value; descriptor.descriptor_generation = 1;
      descriptor.label_schema_uuid = Id().value; descriptor.label_schema_generation = 1;
      descriptor.retention_policy_uuid = Id().value; descriptor.retention_policy_generation = 1;
      descriptor.visibility_policy_uuid = Id().value; descriptor.visibility_policy_generation = 1;
      descriptor.readiness = m::MetricReadiness::implemented;
      m::MetricRetentionPolicy policy; policy.policy_uuid = descriptor.retention_policy_uuid;
      policy.generation = 1; policy.policy_name = "component retention";
      m::MetricHistoryBinding binding; static_cast<m::MetricDescriptorBinding&>(binding) = descriptor;
      binding.database_uuid = database.value; binding.node_uuid = node;
      auto selected = m::MakeMetricSeriesIdentity(descriptor, {{"component", "agent.runtime_service"}},
          policy, binding, Id().value, 1);
      Check(selected.ok(), "actual series construction"); series = std::move(*selected.record);
      Check(registry.RegisterDescriptor(descriptor).ok, "descriptor binding");
      Check(registry.RegisterSeries(series, policy).ok, "series binding");
      Check(registry.SetGauge(descriptor.family, series.labels, p::u64{9007199254740993ull},
          descriptor.producer_owner).ok, "actual exact producer publication");
      auto acquired = queue->TryAcquire(); Check(acquired.ok(), "retain actual queued sample");
      lease = std::move(acquired.lease);
    } catch (...) { device.Close(); std::filesystem::remove_all(root); throw; }
  }
  ~Fixture() { device.Close(); std::error_code ec; std::filesystem::remove_all(root, ec); }
  mga::TransactionIdentity Begin() {
    auto loaded = db::LoadLocalTransactionInventoryFromOpenDevice(&device, page_size);
    Check(loaded.ok(), "actual inventory load");
    auto begun = mga::BeginLocalTransaction(loaded.inventory, Id(p::UuidKind::transaction), 1790000000200ull);
    Check(begun.ok(), "native transaction begin");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&device, page_size, begun.inventory).ok(), "durable BEGIN");
    return begun.entry.identity;
  }
  void Finish(const mga::TransactionIdentity& tx, bool commit) {
    db::PhysicalMgaCowFinalization final;
    final.transaction = tx; final.final_unix_epoch_millis = 1790000000300ull;
    final.decision = commit ? db::PhysicalMgaCowFinalizeDecision::commit : db::PhysicalMgaCowFinalizeDecision::rollback;
    Check(db::FinalizePhysicalMgaCowTransactionToOpenDevice(device, final).ok(), "native finality");
  }
  void Write(const mga::TransactionIdentity& tx, p::TypedUuid row, p::u64 at, bool corrupt = false) {
    if (!corrupt) {
      const auto written = api::StageNativeMetricQueuedObservation(device, *queue, lease,
          descriptor, series, {relation, row, at, tx});
      Check(written.ok() && written.physical_result_available, "actual queued-sample native staging");
      Check(queue->Stats().queued == 1 && queue->Stats().removed == 0, "staging acknowledged before commit");
      return;
    }
    db::PhysicalMgaCowMutation mutation;
    mutation.relation_uuid = relation; mutation.row_uuid = row; mutation.page_number = at;
    mutation.transaction_uuid = tx.transaction_uuid; mutation.existing_local_transaction_id = tx.local_id;
    mutation.use_existing_transaction = true;
    types::DatatypeBinaryValue value; value.type_id = types::CanonicalTypeId::binary;
    value.payload = lease.observation->bytes;
    if (corrupt) value.payload.front() ^= 1;
    mutation.cells.push_back({1, std::move(value)});
    const auto written = db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(device, mutation);
    Check(written.ok(), "actual native binary sample write");
  }
  auto Read(p::TypedUuid row, p::u64 at) {
    return api::ReadNativeCommittedMetricObservation(device, node, relation, at, row,
        descriptor, series, lease.observation->sample_uuid);
  }
};
void Run(p::u32 size) {
  Fixture f(size); const auto row = Id(p::UuidKind::row); const auto tx = f.Begin();
  const api::NativeMetricRecordingTarget target{f.relation, row, f.page, tx};
  auto forged = f.lease; ++forged.token;
  auto refused = api::StageNativeMetricQueuedObservation(f.device, *f.queue, forged, f.descriptor, f.series, target);
  Check(!refused.ok() && !refused.physical_writer_entered &&
      refused.queue_error == m::MetricQueueError::stale_lease, "forged token staged observation");
  forged = f.lease; forged.observation = std::make_shared<m::MetricQueuedObservation>(*f.lease.observation);
  refused = api::StageNativeMetricQueuedObservation(f.device, *f.queue, forged, f.descriptor, f.series, target);
  Check(!refused.physical_writer_entered && refused.queue_error == m::MetricQueueError::stale_lease,
      "copied public observation forged queue provenance");
  f.Write(tx, row, f.page);
  auto before = f.Read(row, f.page);
  Check(!before.observation && before.error == api::NativeCommittedMetricReadError::not_visible, "pending row leaked");
  Check(!api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue, f.lease,
      f.descriptor, f.series, target).acknowledged, "pending observation acknowledged");
  f.Finish(tx, true);
  auto committed = f.Read(row, f.page); Check(committed.ok(), "committed sample absent");
  const auto& observation = *committed.observation;
  auto exact = m::EncodeMetricRawSample(f.descriptor, f.series, observation.sample());
  Check(exact.ok() && exact.bytes == f.lease.observation->bytes, "reader changed observation bytes or time");
  Check(observation.creator_uuid().value == tx.transaction_uuid.value &&
      observation.creator_local_id() == tx.local_id.value && observation.row_uuid() == row.value &&
      !observation.version_uuid().is_nil(), "lost actual native provenance");
  Check(!api::ReadNativeCommittedMetricObservation(f.device, Id().value, f.relation, f.page,
      row, f.descriptor, f.series, f.lease.observation->sample_uuid).ok(), "foreign node accepted");
  auto foreign = f.series; foreign.database_uuid = Id(p::UuidKind::database).value;
  Check(!api::ReadNativeCommittedMetricObservation(f.device, f.node, f.relation, f.page,
      row, f.descriptor, foreign, f.lease.observation->sample_uuid).ok(), "foreign database accepted");
  Check(!api::ReadNativeCommittedMetricObservation(f.device, f.node, f.relation, f.page,
      row, f.descriptor, f.series, Id().value).ok(), "wrong capture identity accepted");
  auto stale = f.descriptor; ++stale.descriptor_generation;
  Check(!api::ReadNativeCommittedMetricObservation(f.device, f.node, f.relation, f.page,
      row, stale, f.series, f.lease.observation->sample_uuid).ok(), "incompatible generation accepted");
  const auto rollback_row = Id(p::UuidKind::row); const auto rollback_tx = f.Begin();
  f.Write(rollback_tx, rollback_row, f.page + 1); f.Finish(rollback_tx, false);
  Check(!f.Read(rollback_row, f.page + 1).observation, "rolled-back sample leaked");
  Check(!api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue, f.lease,
      f.descriptor, f.series, {f.relation, rollback_row, f.page + 1, rollback_tx}).acknowledged,
      "rollback acknowledged queue");
  const auto corrupt_row = Id(p::UuidKind::row); const auto corrupt_tx = f.Begin();
  f.Write(corrupt_tx, corrupt_row, f.page + 2, true); f.Finish(corrupt_tx, true);
  Check(f.Read(corrupt_row, f.page + 2).error == api::NativeCommittedMetricReadError::invalid_sample,
      "committed corruption treated as observation");
  f.device.Close();
  Check(!f.Read(row, f.page).observation, "failed native read supplied observation");
  Check(f.device.Open(f.path, disk::FileOpenMode::open_existing).ok(), "reopen same database");
  Check(f.Read(row, f.page).ok(), "committed observation lost on reopen");
  Check(!f.Read(rollback_row, f.page + 1).observation, "rollback changed on reopen");
  Check(f.queue->Stats().queued == 1 && f.queue->Stats().removed == 0 && f.queue->Stats().admitted == 1,
      "reader changed producer queue");
  for (unsigned mode : {1u, 2u}) {
    const auto failed_tx = f.Begin();
    api::NativeMetricRecordingTarget failed_target{f.relation, Id(p::UuidKind::row), f.page + 2 + mode, failed_tx};
    injected_offset = static_cast<off_t>(failed_target.page_number * size);
    injected_mode = mode; const auto previous = injected_count;
    const auto failed = api::StageNativeMetricQueuedObservation(f.device, *f.queue,
        f.lease, f.descriptor, f.series, failed_target);
    injected_mode = 0;
    Check(injected_count == previous + 1, "native write fault did not execute");
    Check(!failed.ok() && failed.physical_writer_entered, "native failure hidden or marked no-effects");
    Check(f.queue->Stats().queued == 1 && f.queue->Stats().removed == 0,
        "failed or uncertain physical write acknowledged observation");
    f.Finish(failed_tx, false);
    Check(!api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue,
        f.lease, f.descriptor, f.series, failed_target).acknowledged,
        "failed write acknowledged after rollback");
  }
  auto wrong_writer = target; wrong_writer.writer = rollback_tx;
  const auto mismatch = api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue,
      f.lease, f.descriptor, f.series, wrong_writer);
  Check(!mismatch.acknowledged && mismatch.error == api::NativeMetricRecordingError::committed_payload_mismatch,
      "another transaction's row acknowledged original write");
  const auto retained_bytes = f.lease.observation->bytes;
  const auto retained_token = f.lease.token;
  const auto writes_before_ack = native_write_calls;
  queue_mutex = nullptr; queue_lock_calls = 0; busy_on_queue_lock = 2;
  const auto faults_before_ack = queue_busy_faults;
  const auto busy_ack = api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue,
      f.lease, f.descriptor, f.series, target);
  busy_on_queue_lock = 0;
  Check(queue_busy_faults == faults_before_ack + 1 && queue_lock_calls == 2,
      "acknowledgment removal fault not reached after lease validation");
  Check(busy_ack.committed.ok() && !busy_ack.acknowledged &&
      busy_ack.error == api::NativeMetricRecordingError::none &&
      busy_ack.queue_error == m::MetricQueueError::busy,
      "busy removal lost committed receipt or claimed acknowledgment");
  Check(f.queue->Stats().queued == 1 && f.queue->Stats().removed == 0 &&
      f.lease.token == retained_token && f.lease.observation->bytes == retained_bytes,
      "busy removal changed original queue attempt");
  Check(native_write_calls == writes_before_ack, "acknowledgment performed native writes");
  const auto ack = api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue,
      f.lease, f.descriptor, f.series, target);
  Check(ack.committed.ok() && ack.acknowledged && f.queue->Stats().removed == 1 && f.queue->Stats().queued == 0,
      "confirmed native commit did not acknowledge exactly once");
  Check(native_write_calls == writes_before_ack &&
      ack.committed.observation->version_uuid() == busy_ack.committed.observation->version_uuid() &&
      ack.committed.observation->creator_uuid().value == busy_ack.committed.observation->creator_uuid().value &&
      ack.committed.observation->creator_uuid().kind == busy_ack.committed.observation->creator_uuid().kind &&
      ack.committed.observation->creator_local_id() == busy_ack.committed.observation->creator_local_id(),
      "acknowledgment retry restaged or replaced the original committed attempt");
  const auto repeated = api::AcknowledgeNativeCommittedMetricObservation(f.device, *f.queue,
      f.lease, f.descriptor, f.series, target);
  Check(!repeated.acknowledged && repeated.queue_error == m::MetricQueueError::stale_lease &&
      f.queue->Stats().removed == 1, "removed lease replayed");
  std::cout << "profile=" << size << " PASS\n";
}
void TestBoundedDiagnosticCounters() {
  m::detail::MetricQueueSaturatingCounter ordinary;
  m::detail::MetricQueueSaturatingCounter boundary(std::numeric_limits<p::u64>::max() - 1);
  m::detail::MetricQueueSaturatingCounter concurrent(std::numeric_limits<p::u64>::max() - 31);
  boundary.Increment();
  Check(boundary.load() == std::numeric_limits<p::u64>::max(), "counter lost exact maximum");
  boundary.Increment(); boundary.Increment();
  Check(boundary.load() == std::numeric_limits<p::u64>::max(), "counter exposed quiescent wrap");
  std::latch start(1);
  std::array<std::jthread, 4> workers;
  try {
    for (auto& worker : workers) worker = std::jthread([&] {
      start.wait();
      for (unsigned n = 0; n != 4096; ++n) { ordinary.Increment(); concurrent.Increment(); }
    });
  } catch (...) { start.count_down(); throw; }
  start.count_down();
  for (auto& worker : workers) worker.join();
  Check(ordinary.load() == 4 * 4096, "bounded counters lost concurrent increments");
  Check(concurrent.load() == std::numeric_limits<p::u64>::max(), "concurrent counter failed saturation");
}
}
int main(int argc, char** argv) {
  unsigned failures = 0;
  try { TestBoundedDiagnosticCounters(); }
  catch (const std::exception& e) { ++failures; std::cerr << "counter FAILED: " << e.what() << '\n'; }
  if (argc == 2 && std::string(argv[1]) == "--counter-only") {
    std::cout << checks << " counter checks; failures=" << failures << '\n';
    return failures ? 1 : 0;
  }
  for (p::u32 profile : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    try { Run(profile); }
    catch (const std::exception& e) {
      injected_mode = 0; busy_on_queue_lock = 0; ++failures;
      std::cerr << "profile=" << profile << " FAILED: " << e.what() << '\n';
    }
  }
  std::cout << checks << " checks; profile_failures=" << failures << '\n';
  return failures ? 1 : 0;
}
