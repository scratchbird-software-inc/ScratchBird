// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_NAME_STREAM_TEST (streaming reader, owner 2026-11-25).
//
// A names relation grown far past the old 4096-row / 4 MiB scan-tier cap by the PRODUCTION catalog writers on a file
// made by the real create path, read by the streaming reader. Harness: native_name_resolve_test.cpp (create/forge
// children, the offline ground truth). Driver:
//   bigcreate <path> <page_size> <total_pages>          create child (the real create path, a larger file)
//   bigforge  <path> <bootstrap> <rows> [per_commit]    forge child: <rows> aliases on the bootstrap principal
//   rate <page_size> <rows>                             create + forge, timed (measurement only)

#define SB_NAME_STREAM_TEST 1
#define main name_resolve_driver_main
#include "native_name_resolve_test.cpp"
#undef main

#include "native_catalog_visibility_stream_reader.hpp"
#include <fstream>

namespace {
double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

int BigCreateMode(const fs::path& path, std::uint32_t page_size, std::uint64_t total_pages) {
  std::string verifier;
  if (!scratchbird::core::crypto::DeriveLocalPasswordVerifier(kBootstrapPassword, &verifier)) return 1;
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.total_pages = total_pages;
  request.database_name = "native_name_stream";
  request.bootstrap_principal_name = std::string(kBootstrapName);
  request.service_principal_name = std::string(kServiceName);
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  request.bootstrap_credential_fingerprint = verifier;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  p::Uuid binding;
  if (!scratchbird::tests::NativeFixtureReceiptUuid(made.api, "service_runtime_binding_uuid", &binding)) return 1;
  std::cout << "CREATED database=" << Hex(made.database_uuid) << " bootstrap=" << Hex(made.bootstrap_principal_uuid)
            << " service=" << Hex(made.service_principal_uuid) << " binding=" << Hex(binding) << std::endl;
  return 0;
}

// <rows> aliases on the bootstrap principal, written by WriteCatalogInsertVersions, <per_commit> rows per transaction
// (0: all in one). The writer's per-write read bounds are raised to the file's page count (they are bounds on what one
// write reads, caller-set; see the inventory W1).
void BigInsert(Writing& w, std::vector<db::CatalogInsertRow> rows, std::size_t pages) {
  db::CatalogInsertRequest r;
  r.transaction_order = w.order;
  r.transaction_uuid = w.header.transaction_uuid;
  r.rows = std::move(rows);
  for (std::size_t i = 0; i < pages; ++i) r.new_pages.push_back({NewId(), NewId()});
  const auto written = db::WriteCatalogInsertVersions(*w.sink, w.binding, r);
  if (!written.ok()) std::cerr << "insert refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok(), "the name rows are inserted in place");
}

int BigForgeMode(const fs::path& path, const p::Uuid& bootstrap, int rows, int per_commit, int per_insert = 10, int start = 0) {
  ProcessMemory();
  const auto started = std::chrono::steady_clock::now();
  const auto before = ReadOffline(path);
  const auto* base_boot = EntryOf(before, bootstrap, std::string(kBootstrapName));
  Check(base_boot != nullptr, "the create wrote the bootstrap entry");
  if (per_commit <= 0) per_commit = rows;
  int done = 0;
  while (done < rows) {
    auto w = BeginWriting(path, before);
    w->binding.maximum_chain_pages = w->binding.maximum_index_pages = w->binding.maximum_base_pages = 1u << 20;
    Touch(*w, before.security.at(bootstrap));
    const auto batch_end = std::min(rows, done + per_commit);
    for (int i = done; i < batch_end; i += per_insert) {
      std::vector<db::CatalogInsertRow> batch;
      for (int j = i; j < std::min(i + per_insert, batch_end); ++j) {
        char name[32];
        std::snprintf(name, sizeof name, "bulk_%06d", start + j);
        batch.push_back(EntryRow(*w, *base_boot, {name, false, std::nullopt, cat::CatalogNameClass::alias, false}));
      }
      const auto count = batch.size();
      BigInsert(*w, std::move(batch), 16 + 2 * count);
      if ((i / per_insert) % 20 == 0) std::fprintf(stderr, "  forged %d rows, %.1f s\n", i, Seconds(started));
    }
    Commit(*w);
    Check(w->device.Close().ok(), "close");
    done = batch_end;
  }
  std::cout << "FORGED rows=" << rows << " seconds=" << Seconds(started) << std::endl;
  return 0;
}

// One visible row reduced to a comparable line: identity, flags and a hash of its payload and (for a name) its text.
std::string Fingerprint(const db::NativeCatalogVisibleRow& row) {
  const auto& h = row.metadata.record.header;
  const std::string_view payload(reinterpret_cast<const char*>(row.metadata.record.payload.data()), row.metadata.record.payload.size());
  std::string text;
  if (row.name_payload)
    if (const auto* e = std::get_if<cat::CatalogNameEntryView>(&*row.name_payload)) text = std::string(e->raw_name_text);
  char buffer[256];
  std::snprintf(buffer, sizeof buffer, "%s|%s|del=%d|prov=%d|lc=%d|st=%d|def=%llu|pay=%zx|", Hex(row.version_uuid).c_str(), Hex(h.row_uuid.value).c_str(),
                h.deleted ? 1 : 0, row.provisional ? 1 : 0, static_cast<int>(row.effective_lifecycle), static_cast<int>(row.effective_status),
                static_cast<unsigned long long>(row.metadata.definition_version), std::hash<std::string_view>{}(payload));
  return std::string(buffer) + text;
}

// Every relation of the file read by BOTH readers; the sets of visible rows must be identical. The collect-everything reader
// is the ground truth (it is what the file's open and the offline reads use today); the files here are within its tier.
int StreamCompareMode(const fs::path& path, int window, int history) {
  ProcessMemory();
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(), "open read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  const auto database = zero.record->bootstrap.database_uuid;
  const auto filespace = zero.record->bootstrap.filespace_uuid;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = 8ull * zero.record->total_pages * size;
  const db::NativeCatalogRelationMemoryLimits old_limits{profile, 1, db::NativeCatalogScanBasePages(size), db::kNativeCatalogScanRelationMaximumRows,
      db::kNativeCatalogScanRelationMaximumRows, db::NativeCatalogScanNavigationLimits(size),
      db::NativeNameTreeNavigationLimits(size, db::kNativeCatalogScanRelationMaximumRows)};
  db::NativeCatalogStreamLimits limits;
  limits.maximum_profile = profile;
  limits.window_pages = static_cast<std::size_t>(window);
  limits.history_versions = static_cast<std::size_t>(history);
  BoundMemory old_memory(database, db::NativeCatalogVisibilityWorkspaceBytes(old_limits)), stream_memory(database, db::NativeCatalogStreamWorkspaceBytes(limits)),
      source_memory(database, 32ull * 1024 * 1024);
  auto old_prepared = db::PrepareNativeCatalogVisibilityRead(old_limits, old_memory.memory, old_memory.binding);
  auto prepared = db::PrepareNativeCatalogStreamRead(limits, stream_memory.memory, stream_memory.binding);
  Check(old_prepared.ok() && prepared.ok(), "both workspaces prepare");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(database, devices, filespace, {budget, 32ull * 1024 * 1024}, source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  std::uint64_t total = 0, histories = 0;
  for (int selector : {2}) {
    for (std::uint16_t role = 1; role <= scratchbird::storage::page::kNativeCatalogRelationRoles; ++role) {
      const auto old_read = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(*source.lease, static_cast<std::uint16_t>(selector), role, budget, old_prepared.workspace);
      std::set<std::string> expected, got;
      if (old_read.ok()) for (const auto& row : old_read.rows) expected.insert(Fingerprint(row));
      std::uint64_t visited = 0;
      const auto streamed = db::NativeCatalogVisibilityStreamReader::StreamCommittedSelected(
          *source.lease, static_cast<std::uint16_t>(selector), role, budget, prepared.workspace, [&](const db::NativeCatalogVisibleRow& row) {
            ++visited;
            got.insert(Fingerprint(row));
            return true;
          });
      if (!old_read.ok()) {
        // A relation the collect-everything reader refuses (a missing role of this selector) the stream must refuse too.
        Check(!streamed.ok(), "role " + std::to_string(role) + ": both readers refuse");
        continue;
      }
      if (!streamed.ok()) std::cerr << "role " << role << " stream error " << db::NativeCatalogStreamErrorName(streamed.error) << '\n';
      Check(streamed.ok(), "role " + std::to_string(role) + ": the stream reads what the collect-everything reader reads");
      Check(visited == got.size(), "role " + std::to_string(role) + ": the stream visits each row once");
      if (got != expected) {
        std::size_t shown = 0;
        for (const auto& line : expected) if (!got.contains(line) && shown++ < 3) std::cerr << "  only collect-everything: " << line << '\n';
        for (const auto& line : got) if (!expected.contains(line) && shown++ < 6) std::cerr << "  only stream: " << line << '\n';
      }
      Check(got == expected, "role " + std::to_string(role) + ": the same visible rows (" + std::to_string(expected.size()) + ")");
      total += expected.size();
      histories += streamed.history_rows;
    }
  }
  std::printf("STREAMCOMPARE rows=%llu history_rows=%llu\n", (unsigned long long)total, (unsigned long long)histories);
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return 0;
}

// Drops <count> of the bulk aliases starting at bulk_<first>: each drop is a successor version, so each dropped name's
// row has history.
int BigDropMode(const fs::path& path, const p::Uuid& bootstrap, int first, int count) {
  ProcessMemory();
  const auto before = ReadOffline(path);
  auto w = BeginWriting(path, before);
  w->binding.maximum_chain_pages = w->binding.maximum_index_pages = w->binding.maximum_base_pages = 1u << 20;
  Touch(*w, before.security.at(bootstrap));
  std::map<std::string, const NameFact*> by_text;
  for (const auto& n : before.names) by_text.emplace(n.text, &n);
  for (int i = first; i < first + count; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "bulk_%06d", i);
    const auto found = by_text.find(name);
    Check(found != by_text.end(), "the alias to drop is visible and committed");
    DropEntry(*w, *found->second);
  }
  Commit(*w);
  Check(w->device.Close().ok(), "close");
  std::cout << "DROPPED count=" << count << std::endl;
  return 0;
}

// The collect-everything reader's verdict on the names relation of the file (the cap the stream lifts).
int OldReadMode(const fs::path& path) {
  ProcessMemory();
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(), "open read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  const auto database = zero.record->bootstrap.database_uuid;
  const auto filespace = zero.record->bootstrap.filespace_uuid;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = 8ull * zero.record->total_pages * size;
  const db::NativeCatalogRelationMemoryLimits limits{profile, 1, db::NativeCatalogScanBasePages(size), db::kNativeCatalogScanRelationMaximumRows,
      db::kNativeCatalogScanRelationMaximumRows, db::NativeCatalogScanNavigationLimits(size),
      db::NativeNameTreeNavigationLimits(size, db::kNativeCatalogScanRelationMaximumRows)};
  BoundMemory workspace_memory(database, db::NativeCatalogVisibilityWorkspaceBytes(limits)), source_memory(database, 32ull * 1024 * 1024);
  auto prepared = db::PrepareNativeCatalogVisibilityRead(limits, workspace_memory.memory, workspace_memory.binding);
  Check(prepared.ok(), "the collect-everything workspace prepares");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(database, devices, filespace, {budget, 32ull * 1024 * 1024}, source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  const auto names = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(*source.lease, 2, 2, budget, prepared.workspace);
  std::printf("OLDREAD ok=%d error=%d relation_error=%d\n", names.ok() ? 1 : 0, static_cast<int>(names.error), static_cast<int>(names.source.error));
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return 0;
}

std::uint64_t HighWaterKb() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmHWM:", 0) == 0) return std::stoull(line.substr(6));
  return 0;
}
std::uint64_t CurrentKb() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmRSS:", 0) == 0) return std::stoull(line.substr(6));
  return 0;
}

// Streams one relation of the file through the streaming reader and prints what it saw. role 2 names, 4 security, 5 config.
int StreamReadMode(const fs::path& path, int role, int window, int history, bool retain_all) {
  ProcessMemory();
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(), "open read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  const auto database = zero.record->bootstrap.database_uuid;
  const auto filespace = zero.record->bootstrap.filespace_uuid;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = 8ull * zero.record->total_pages * size;
  db::NativeCatalogStreamLimits limits;
  limits.maximum_profile = profile;
  limits.window_pages = static_cast<std::size_t>(window);
  limits.history_versions = static_cast<std::size_t>(history);
  BoundMemory workspace_memory(database, db::NativeCatalogStreamWorkspaceBytes(limits)), source_memory(database, 32ull * 1024 * 1024);
  const auto before_kb = CurrentKb();
  auto prepared = db::PrepareNativeCatalogStreamRead(limits, workspace_memory.memory, workspace_memory.binding);
  Check(prepared.ok(), "the stream workspace prepares");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(database, devices, filespace, {budget, 32ull * 1024 * 1024}, source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  std::uint64_t rows = 0, live = 0;
  std::size_t digest = 0;
  std::vector<std::string> kept;
  const auto started = std::chrono::steady_clock::now();
  const auto result = db::NativeCatalogVisibilityStreamReader::StreamCommittedSelected(
      *source.lease, 2, static_cast<std::uint16_t>(role), budget, prepared.workspace, [&](const db::NativeCatalogVisibleRow& row) {
        ++rows;
        if (!row.metadata.record.header.deleted && !row.provisional) ++live;
        const auto& id = row.metadata.record.header.row_uuid.value;
        digest ^= std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(id.bytes.data()), 16));
        if (retain_all && row.name_payload) {
          if (const auto* e = std::get_if<cat::CatalogNameEntryView>(&*row.name_payload)) kept.emplace_back(e->raw_name_text);
          kept.emplace_back(row.metadata.record.payload.begin(), row.metadata.record.payload.end());
        }
        return true;
      });
  const auto seconds = Seconds(started);
  if (!result.ok()) {
    std::printf("STREAMREAD error=%s history_versions=%llu\n", db::NativeCatalogStreamErrorName(result.error), (unsigned long long)result.history_versions);
    source.lease.Reset();
    Check(device.Close().ok(), "close");
    return 0;
  }
  std::printf("STREAMREAD role=%d rows=%llu live=%llu digest=%zx cells=%llu leaf_reads=%llu tree_reads=%llu versions=%llu history_versions=%llu history_rows=%llu bytes=%llu workspace=%zu seconds=%.2f rss_before_kb=%llu hwm_kb=%llu kept=%zu\n",
              role, (unsigned long long)rows, (unsigned long long)live, digest, (unsigned long long)result.cells, (unsigned long long)result.leaf_reads,
              (unsigned long long)result.tree_page_reads, (unsigned long long)result.versions_examined, (unsigned long long)result.history_versions,
              (unsigned long long)result.history_rows, (unsigned long long)result.physical_bytes_read, result.workspace_bytes, seconds,
              (unsigned long long)before_kb, (unsigned long long)HighWaterKb(), kept.size());
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return 0;
}
}  // namespace

namespace {
std::string Run(const std::string& command, int* status = nullptr) {
  int local = 0;
  const auto out = Capture(command, status ? status : &local);
  return out;
}
std::string Field2(const std::string& text, const std::string& key) {
  const auto at = text.find(key + "=");
  if (at == std::string::npos) return {};
  const auto from = at + key.size() + 1;
  return text.substr(from, text.find_first_of(" \n", from) - from);
}
std::uint64_t Num(const std::string& text, const std::string& key) {
  const auto v = Field2(text, key);
  return v.empty() ? 0 : std::stoull(v);
}

// Each state of one page size. Every database directory is removed as soon as its state finishes (the /tmp footprint).
void StreamOneSize(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::vector<std::string>& states) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const std::uint64_t total_pages = std::max<std::uint64_t>(512, (64ull << 20) / page_size);
  for (const auto& state : states) {
    const auto dir = root / (state + std::to_string(page_size));
    fs::create_directory(dir);
    const auto path = dir / "names.sdb";
    int status = 0;
    const auto created = Capture("'" + self + "' bigcreate '" + path.string() + "' " + std::to_string(page_size) + " " + std::to_string(total_pages), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
    const auto boot = Field2(created, "bootstrap");
    const auto service = Field2(created, "service");
    const auto read = [&](int role, int window, int history, const char* retain) {
      int st = 0;
      const auto out = Capture("'" + self + "' streamread '" + path.string() + "' " + std::to_string(role) + " " + std::to_string(window) + " " + std::to_string(history) + " " + retain, &st);
      Check(st == 0 && out.find("STREAMREAD") != std::string::npos, tag + state + ": streamread: " + out.substr(0, 1500));
      return out;
    };
    if (state == "equal") {
      // The stream against the collect-everything reader on the same files, every relation, at the smallest window and a
      // roomy one; files with a committed drop (history) and with an uncommitted writer's version.
      const auto base = Capture("'" + self + "' streamcompare '" + path.string() + "' 3 64", &status);
      Check(status == 0 && base.find("STREAMCOMPARE") != std::string::npos, tag + "equal: created file: " + base.substr(0, 1500));
      Check(Capture("'" + self + "' forge '" + path.string() + "' dead " + boot + " " + service, &status).find("FORGED kind=dead") != std::string::npos && status == 0,
            tag + "equal: the production writers wrote names and a committed drop");
      for (const char* window : {"3", "16"}) {
        const auto out = Capture("'" + self + "' streamcompare '" + path.string() + "' " + window + " 64", &status);
        Check(status == 0 && out.find("STREAMCOMPARE") != std::string::npos, tag + "equal: window " + window + ": " + out.substr(0, 1500));
        Check(Num(out, "history_rows") >= 3, tag + "equal: rows with history were compared (" + std::to_string(Num(out, "history_rows")) + ")");
      }
      // A second file: names written by a transaction left ACTIVE (never committed). The collect-everything reader hides the
      // version (an in-progress creator is not visible to a committed read); so must the stream. Skipping visibility in the
      // stream is caught here.
      {
        const auto path2 = dir / "active.sdb";
        const auto created2 = Capture("'" + self + "' bigcreate '" + path2.string() + "' " + std::to_string(page_size) + " " + std::to_string(total_pages), &status);
        Check(status == 0 && created2.starts_with("CREATED "), tag + "equal: second create: " + created2);
        Check(Capture("'" + self + "' forge '" + path2.string() + "' uncommitted " + Field2(created2, "bootstrap") + " " + Field2(created2, "service"), &status).find("FORGED kind=uncommitted") != std::string::npos && status == 0,
              tag + "equal: the production writers left an active transaction's names");
        for (const char* window : {"3", "16"}) {
          const auto out = Capture("'" + self + "' streamcompare '" + path2.string() + "' " + window + " 64", &status);
          Check(status == 0 && out.find("STREAMCOMPARE") != std::string::npos, tag + "equal: active transaction, window " + window + ": " + out.substr(0, 1500));
        }
      }
      std::printf("%sequal: stream == collect-everything on every relation, rows %llu\n", tag.c_str(), (unsigned long long)Num(Capture("'" + self + "' streamcompare '" + path.string() + "' 4 64", &status), "rows"));
    } else if (state == "big") {
      // Grow the names relation in steps by the production writers; read it by the stream after every step.
      constexpr int kStep = 1200, kSteps = 3;
      const auto at0 = read(2, 4, 1024, "noretain");
      const auto base_rows = Num(at0, "rows");
      std::vector<std::string> reads{at0};
      std::printf("%sbig: step 0: %llu name rows, %llu leaf images read\n", tag.c_str(), (unsigned long long)base_rows, (unsigned long long)Num(at0, "leaf_reads"));
      for (int step = 1; step <= kSteps; ++step) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto forged = Capture("'" + self + "' bigforge '" + path.string() + "' " + boot + " " + std::to_string(kStep) + " 0 250 " + std::to_string((step - 1) * kStep), &status);
        Check(status == 0 && forged.find("FORGED rows=" + std::to_string(kStep)) != std::string::npos, tag + "big: forge step " + std::to_string(step) + ": " + forged.substr(0, 1500));
        const auto out = read(2, 4, 1024, "noretain");
        reads.push_back(out);
        Check(Num(out, "rows") == base_rows + static_cast<std::uint64_t>(step) * kStep, tag + "big: the stream sees exactly the names written (" + std::to_string(Num(out, "rows")) + ")");
        Check(Num(out, "live") == Num(out, "rows"), tag + "big: every streamed name is live");
        std::printf("%sbig: step %d: +%.1f s forge; %llu name rows, cells %llu, leaf reads %llu, tree reads %llu, window workspace %llu, hwm %llu KB (rss before %llu KB), stream %.2f s\n",
                    tag.c_str(), step, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), (unsigned long long)Num(out, "rows"),
                    (unsigned long long)Num(out, "cells"), (unsigned long long)Num(out, "leaf_reads"), (unsigned long long)Num(out, "tree_reads"),
                    (unsigned long long)Num(out, "workspace"), (unsigned long long)Num(out, "hwm_kb"), (unsigned long long)Num(out, "rss_before_kb"),
                    std::stod(Field2(out, "seconds")));
      }
      const auto& last = reads.back();
      Check(Num(last, "rows") > db::kNativeCatalogScanRelationMaximumRows, tag + "big: the relation is past the old 4096-row cap");
      Check(Num(last, "leaf_reads") * page_size > db::kNativeCatalogScanRelationMaximumBytes, tag + "big: and its base leaves are more than the old 4 MiB leaf-image cap (" + std::to_string(Num(last, "leaf_reads") * page_size) + " bytes of leaf images)");
      // The collect-everything reader is where the cap was.
      const auto old_read = Capture("'" + self + "' oldread '" + path.string() + "'", &status);
      Check(status == 0 && old_read.find("OLDREAD ok=0") != std::string::npos, tag + "big: the collect-everything reader still refuses the relation: " + old_read.substr(0, 500));
      // Memory: the governed workspace is the same at every size; the process high water does not follow the rows.
      for (const auto& r : reads) Check(Num(r, "workspace") == Num(reads.front(), "workspace"), tag + "big: the governed stream workspace is the same at every size");
      const auto hwm0 = Num(reads.front(), "hwm_kb"), hwmN = Num(last, "hwm_kb");
      Check(hwmN <= hwm0 + 3 * 1024, tag + "big: the process high water at " + std::to_string(Num(last, "rows")) + " rows (" + std::to_string(hwmN) + " KB) is within 3 MB of the high water at " + std::to_string(base_rows) + " rows (" + std::to_string(hwm0) + " KB)");
      Check(Num(last, "leaf_reads") < 2 * Num(last, "cells"), tag + "big: leaf reads stay below two per cell (the window works)");
      // The control that makes the flatness check mean something: a visitor that keeps every row grows with the rows.
      const auto retained = read(2, 4, 1024, "retain");
      Check(Num(retained, "hwm_kb") > hwmN + 2 * 1024, tag + "big: control: a visitor that retains every row raises the high water well past the streaming one (" + std::to_string(Num(retained, "hwm_kb")) + " KB vs " + std::to_string(hwmN) + " KB)");
    } else if (state == "budget") {
      // Rows with history: committed drops. The registry that holds their versions is governed and refuses with its own code.
      Check(Capture("'" + self + "' bigforge '" + path.string() + "' " + boot + " 120 0 60 0", &status).find("FORGED rows=120") != std::string::npos && status == 0, tag + "budget: forged");
      const auto before = read(2, 4, 1024, "noretain");
      Check(Capture("'" + self + "' bigdrop '" + path.string() + "' " + boot + " 0 40", &status).find("DROPPED count=40") != std::string::npos && status == 0, tag + "budget: dropped 40");
      const auto roomy = read(2, 4, 1024, "noretain");
      Check(Num(roomy, "history_rows") >= 40, tag + "budget: the dropped names are rows with history (" + std::to_string(Num(roomy, "history_rows")) + ")");
      Check(Num(roomy, "live") + 40 == Num(before, "live"), tag + "budget: forty fewer live names after the drops");
      const auto tight = read(2, 4, 16, "noretain");
      Check(tight.find("STREAMREAD error=history_exhausted") != std::string::npos, tag + "budget: a registry of 16 versions refuses with history_exhausted: " + tight.substr(0, 300));
      const auto window = read(2, 3, 1024, "noretain");
      Check(window.find("STREAMREAD") != std::string::npos, tag + "budget: the smallest window, 3");
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    std::vector<std::string> states{"equal", "big", "budget"};
    if (argc > 3) states.assign(argv + 3, argv + argc);
    StreamOneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), states);
    artifacts.Cleanup();
    std::printf("native_name_stream: %d driver checks passed\n", checks);
    return 0;
  }
  if (argc == 5 && std::string(argv[1]) == "bigcreate")
    return BigCreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])), std::stoull(argv[4]));
  if (argc >= 5 && std::string(argv[1]) == "bigforge")
    return BigForgeMode(argv[2], FromHex(argv[3]), std::stoi(argv[4]), argc > 5 ? std::stoi(argv[5]) : 0, argc > 6 ? std::stoi(argv[6]) : 10, argc > 7 ? std::stoi(argv[7]) : 0);
  if (argc == 6 && std::string(argv[1]) == "bigdrop") return BigDropMode(argv[2], FromHex(argv[3]), std::stoi(argv[4]), std::stoi(argv[5]));
  if (argc == 3 && std::string(argv[1]) == "oldread") return OldReadMode(argv[2]);
  if (argc == 5 && std::string(argv[1]) == "streamcompare") return StreamCompareMode(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
  if (argc >= 7 && std::string(argv[1]) == "streamread")
    return StreamReadMode(argv[2], std::stoi(argv[3]), std::stoi(argv[4]), std::stoi(argv[5]), std::string(argv[6]) == "retain");
  return name_resolve_driver_main(argc, argv);
}
