// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "temp_workspace_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace memory = scratchbird::core::memory;
using Uuid = memory::TempWorkspaceUuid;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct OwnedDirectory {
  std::filesystem::path path;
  OwnedDirectory() {
    std::string pattern = "/tmp/sb-temp-owner-recovery-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = ::mkdtemp(buffer.data());
    Require(created != nullptr, "mkdtemp");
    path = created;
  }
  ~OwnedDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

Uuid Identity(unsigned char suffix) {
  // Test oracle only; production issuance is not replaced by this fixture.
  Uuid value{{0x01,0x99,0x20,0,0,0,0x70,0,0x80,0,0,0,0,0,0,suffix}};
  return value;
}

memory::TempWorkspaceAllocationRequest Request() {
  memory::TempWorkspaceAllocationRequest request;
  request.bytes = 64;
  request.owner.temp_object_uuid = Identity(1);
  request.owner.database_id = Identity(2);
  request.owner.engine_id = Identity(3);
  request.owner.session_id = Identity(4);
  request.owner.transaction_id = Identity(5);
  request.owner.statement_id = Identity(6);
  request.owner.cursor_id = Identity(7);
  request.owner.result_set_id = Identity(8);
  request.owner.operation_id = Identity(9);
  request.owner.scheduler_task_id = Identity(10);
  request.owner.snapshot_boundary = Identity(11);
  request.owner.metadata_boundary = Identity(12);
  request.owner.resource_budget_reference = Identity(13);
  request.owner.policy_generation = 37;
  request.owner.security_generation = 59;
  request.purpose = "query\tspill\nwith embedded delimiters";
  return request;
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "read manifest");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path, const std::string& value) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  out.close();
  Require(static_cast<bool>(out), "write test manifest");
}
std::uint64_t Read64(const std::string& bytes, std::size_t at) {
  Require(at + 8 <= bytes.size(), "oracle integer bounds");
  std::uint64_t out = 0;
  for (unsigned i = 0; i != 8; ++i)
    out |= std::uint64_t(static_cast<unsigned char>(bytes[at+i])) << (8*i);
  return out;
}
void Write64(std::string* bytes, std::size_t at, std::uint64_t value) {
  Require(at + 8 <= bytes->size(), "oracle write bounds");
  for (unsigned i = 0; i != 8; ++i) (*bytes)[at+i] = char(value >> (8*i));
}
std::string Seal(const std::string& header, const std::string& body) {
  // Independent corruption-check oracle, not a production encoder call.
  std::uint64_t checksum = 14695981039346656037ull;
  for (unsigned char byte : body) { checksum ^= byte; checksum *= 1099511628211ull; }
  std::ostringstream out;
  out << header.substr(0, header.rfind('\t') + 1)
      << std::hex << std::setw(16) << std::setfill('0') << checksum << '\n';
  return out.str() + body;
}

int main() {
  try {
    OwnedDirectory owned;
    memory::TempWorkspacePolicy policy;
    policy.database_uuid = Identity(2);
    policy.engine_uuid = Identity(3);
    policy.root_path = owned.path;
    policy.filespace_quota_bytes = 4096;
    policy.session_quota_bytes = 64;
    const auto request = Request();
    std::string allocation_id;
    {
      memory::TempWorkspaceLifecycleManager manager(policy);
      const auto result = manager.AllocateSpillFile(request);
      Require(result.ok() && result.record.has_value(), "actual spill allocation");
      allocation_id = result.record->allocation_id;
      Require(result.record->owner == request.owner, "live owner equality");
    }
    const auto manifest = owned.path / ".scratchbird_temp_workspace_manifest.v3";
    const auto original = Read(manifest);
    const auto header_end = original.find('\n', original.find('\n') + 1);
    Require(header_end != std::string::npos, "manifest framing");
    const auto header = original.substr(0, header_end);
    const auto body = original.substr(header_end + 1);
    Require(Read64(body, 0) == 1, "record count");
    Require(Read64(body, 8) == body.size() - 16, "exact framed record size");
    Require(Read64(body, 16) == 3, "binary record version");
    for (unsigned slot = 0; slot != 13; ++slot) {
      const auto expected = Identity(static_cast<unsigned char>(slot + 1));
      for (unsigned byte = 0; byte != 16; ++byte)
        Require(static_cast<unsigned char>(body[24 + slot*16 + byte]) == expected.bytes[byte],
                "independent raw sixteen-byte owner slot");
    }
    Require(Read64(body, 232) == 37 && Read64(body, 240) == 59,
            "independent generation offsets");
    {
      memory::TempWorkspaceLifecycleManager reopened(policy);
      const auto records = reopened.ActiveRecords();
      Require(records.size() == 1 && records[0].owner == request.owner &&
              records[0].purpose == request.purpose &&
              records[0].reserved_bytes == request.bytes, "complete real-file owner recovery");
      const auto accounting = reopened.Snapshot();
      Require(accounting.session_bytes.at(request.owner.session_id) == 64 &&
              accounting.transaction_bytes.at(request.owner.transaction_id) == 64 &&
              accounting.statement_bytes.at(request.owner.statement_id) == 64 &&
              accounting.operation_bytes.at(request.owner.operation_id) == 64,
              "recovered binary quota keys");
      auto extra = request;
      extra.owner.temp_object_uuid = Identity(20);
      Require(!reopened.AllocateSpillFile(extra).ok(), "recovered session quota enforced");
      extra.owner.session_id = Identity(21);
      const auto distinct = reopened.AllocateSpillFile(extra);
      Require(distinct.ok(), "distinct binary owner quota independence");
      Require(reopened.CleanupOnDisconnect(Identity(22)).cleaned_count == 0,
              "different owner cannot clean spill");
      const auto cleanup = reopened.CleanupOnDisconnect(extra.owner.session_id);
      Require(cleanup.ok() && cleanup.cleaned_count == 1 &&
              reopened.Find(allocation_id).has_value(), "exact owner cleanup");
    }
    // Restore the single-record baseline; files now match this manifest again.
    Write(manifest, original);
    std::size_t negatives = 0;
    auto refused = [&](const std::string& malformed) {
      Write(manifest, malformed);
      memory::TempWorkspaceLifecycleManager manager(policy);
      Require(manager.ActiveRecords().empty() && manager.Snapshot().active_bytes == 0,
              "malformed manifest must publish no prefix");
      Require(!manager.AllocateSpillFile(request).ok(), "invalid manifest must block allocations");
      Require(!manager.CleanupOnShutdown().ok(), "invalid manifest must block cleanup");
      Require(Read(manifest) == malformed, "failed recovery must preserve original evidence");
      ++negatives;
    };
    for (std::size_t cut = 0; cut != body.size(); ++cut)
      refused(Seal(header, body.substr(0, cut)));
    for (unsigned version : {0u,1u,2u,4u,255u}) {
      auto changed = body;
      Write64(&changed, 16, version);
      refused(Seal(header, changed));
    }
    for (unsigned slot = 0; slot != 13; ++slot) {
      for (unsigned version = 0; version != 16; ++version) {
        if (version == 7) continue;
        auto changed = body;
        changed[24 + slot*16 + 6] = char(version << 4);
        refused(Seal(header, changed));
      }
      for (unsigned variant : {0u,0x40u,0xc0u}) {
        auto changed = body;
        changed[24 + slot*16 + 8] = char(variant);
        refused(Seal(header, changed));
      }
    }
    for (std::size_t enum_offset : {280u,288u,296u,304u,312u}) {
      auto invalid_enum = body;
      Write64(&invalid_enum, enum_offset, ~std::uint64_t{0});
      refused(Seal(header, invalid_enum));
    }
    for (std::size_t bool_offset = 320; bool_offset != 333; ++bool_offset) {
      auto invalid_bool = body;
      invalid_bool[bool_offset] = 2;
      refused(Seal(header, invalid_bool));
    }
    for (unsigned slot : {0u,1u,2u,5u,10u,11u,12u}) {
      auto missing_owner = body;
      std::fill_n(missing_owner.begin() + 24 + slot*16, 16, '\0');
      refused(Seal(header, missing_owner));
    }
    for (unsigned length_offset : {0u,8u}) {
      auto invalid_length = body;
      Write64(&invalid_length, length_offset, ~std::uint64_t{0});
      refused(Seal(header, invalid_length));
    }
    auto changed = body + "trailing";
    refused(Seal(header, changed));
    changed = body + body.substr(8);  // Duplicated allocation must not double-charge accounting.
    Write64(&changed, 0, 2);
    refused(Seal(header, changed));
    changed = body;
    Write64(&changed, 8, Read64(body,8) + 1);
    changed.push_back('\0');
    refused(Seal(header, changed));
    changed = body;
    changed[24] ^= 1;
    refused(original.substr(0, header_end+1) + changed); // checksum mismatch
    for (unsigned slot : {1u,2u}) {
      changed = body;
      changed[24 + slot*16 + 15] ^= 1;
      refused(Seal(header, changed)); // Valid UUID, wrong database or engine owner.
    }
    Write(manifest, original);
    // Old text versions lack fences: preserve them and refuse, never auto-upgrade.
    for (unsigned version : {1u,2u}) {
      const auto legacy = owned.path /
          (".scratchbird_temp_workspace_manifest.v" + std::to_string(version));
      Write(legacy, "legacy text metadata");
      memory::TempWorkspaceLifecycleManager manager(policy);
      Require(!manager.AllocateSpillFile(request).ok() &&
              manager.ActiveRecords().empty(), "legacy metadata refused");
      std::filesystem::remove(legacy);
    }
    Write(manifest, original);
    {
      memory::TempWorkspaceLifecycleManager manager(policy);
      auto invalid = request;
      invalid.owner.temp_object_uuid = Identity(23);
      invalid.owner.snapshot_boundary = {};
      Require(!manager.AllocateSpillFile(invalid).ok(), "missing visibility fence refused");
      Require(!manager.CleanupOnDisconnect({}).ok(), "nil cleanup selector refused");
      auto wrong = Identity(4);
      wrong.bytes[6] = 0x40;
      Require(!manager.CleanupOnDisconnect(wrong).ok(), "non-v7 cleanup selector refused");
      const auto cleanup = manager.CleanupOnDisconnect(request.owner.session_id);
      Require(cleanup.ok() && cleanup.cleaned_count == 1 &&
              manager.Snapshot().active_bytes == 0, "final exact cleanup");
    }
    {
      OwnedDirectory ledger_directory;
      memory::HierarchicalMemoryBudgetLedger ledger(3, 5);
      auto ledger_policy = policy;
      ledger_policy.root_path = ledger_directory.path;
      ledger_policy.reservation_ledger = &ledger;
      ledger_policy.require_ceic_011_reservation = true;
      using Kind = memory::HierarchicalMemoryScopeKind;
      const std::vector<memory::HierarchicalMemoryScopeRef> expected{
        {Kind::process, {}, Identity(3).bytes}, {Kind::database, {}, Identity(2).bytes},
        {Kind::session, {}, Identity(4).bytes}, {Kind::transaction, {}, Identity(5).bytes},
        {Kind::statement, {}, Identity(6).bytes}, {Kind::query, {}, Identity(7).bytes},
        {Kind::operator_scope, {}, Identity(1).bytes}, {Kind::background, {}, Identity(10).bytes}};
      memory::HierarchicalMemoryBudgetProvenance provenance;
      provenance.source = memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
      provenance.source_label = "temp_owner_recovery_test_runtime_policy";
      for (const auto& scope : expected)
        Require(ledger.SetBudget({scope, 1024, 0, provenance}).ok(), "binary scope budget");
      memory::TempWorkspaceLifecycleManager manager(ledger_policy);
      const auto allocation = manager.AllocateSpillFile(request);
      Require(allocation.ok() && allocation.record &&
              allocation.record->budget_reservation_evidence.token.valid(),
              "actual hierarchical reservation for spill");
      const auto snapshot = ledger.Snapshot();
      for (const auto& wanted : expected) {
        const auto found = std::find_if(snapshot.scopes.begin(), snapshot.scopes.end(),
            [&](const auto& scope) {
              return scope.kind == wanted.kind && scope.binary_scope_uuid == wanted.binary_scope_uuid;
            });
        Require(found != snapshot.scopes.end() && found->scope_id.empty() &&
                found->active_bytes == 64, "actual binary ledger charge");
      }
      // A process-local token must not survive as authority in the on-disk
      // record. A fresh process ledger must receive a real new reservation.
      memory::HierarchicalMemoryBudgetLedger recovery_ledger(3, 5);
      for (const auto& scope : expected)
        Require(recovery_ledger.SetBudget({scope, 1024, 0, provenance}).ok(),
                "recovery binary scope budget");
      auto recovery_policy = ledger_policy;
      recovery_policy.reservation_ledger = &recovery_ledger;
      {
        memory::TempWorkspaceLifecycleManager recovered(recovery_policy);
        const auto records = recovered.ActiveRecords();
        Require(records.size() == 1 && records[0].budget_reservation_evidence.token.valid() &&
                recovery_ledger.Snapshot().current_bytes == 64,
                "actual reservation reconstructed in fresh ledger");
        // Release this recovery inspection's actual token without deleting the
        // fixture still owned by the original manager below.
        Require(recovery_ledger.Release(records[0].budget_reservation_evidence.token).ok(),
                "release fresh ledger inspection reservation");
      }
      recovery_policy.reservation_ledger = nullptr;
      recovery_policy.require_ceic_011_reservation = false;
      {
        memory::TempWorkspaceLifecycleManager missing_ledger(recovery_policy);
        Require(missing_ledger.ActiveRecords().empty() &&
                !missing_ledger.AllocateSpillFile(request).ok(),
                "persisted required reservation cannot be downgraded");
      }
      Require(manager.CleanupOnDisconnect(request.owner.session_id).ok() &&
              ledger.Snapshot().current_bytes == 0, "actual ledger reservation released");
    }
    std::cout << "actual binary temp owner/recovery PASS negatives=" << negatives << '\n';
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
