// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "behavior_support/api_behavior_store.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t size) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace api = scratchbird::engine::internal_api;
namespace {
unsigned checks = 0, faults = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
api::EngineUuid Id(unsigned n) {
  api::EngineUuid id{};
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  id.bytes[15] = static_cast<unsigned char>(n);
  return id;
}
api::EngineApiResult Base() {
  api::EngineApiResult result;
  result.ok = true;
  result.transaction_uuid = Id(3);
  result.local_transaction_id = 42;
  result.evidence.push_back({"owner_evidence", "retained"});
  api::EngineRowValue row;
  row.requested_row_uuid = Id(4);
  api::EngineTypedValue value;
  value.encoded_value = "owner_row_value";
  auto user_uuid = Id(5);
  user_uuid.bytes[6] = 0x40;  // Earlier UUID versions remain permissible user data.
  value.binary_value.assign(user_uuid.bytes.begin(), user_uuid.bytes.end());
  row.fields.push_back({"owner_field", value});
  result.result_shape.rows.push_back(std::move(row));
  result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
      "CATALOG.INVALID_INPUT", "owner.previous_diagnostic", "retained", false));
  return result;
}
void CheckOwner(const api::EngineApiResult& result, const api::EngineApiResult& base) {
  Check(result.transaction_uuid == base.transaction_uuid &&
        result.local_transaction_id == base.local_transaction_id, "transaction changed");
  Check(result.evidence.size() == 1 && result.evidence[0].evidence_kind == "owner_evidence" &&
        result.evidence[0].evidence_id == "retained", "fabricated or erased evidence");
  Check(result.result_shape.rows.size() == 1 &&
        result.result_shape.rows[0].requested_row_uuid == Id(4) &&
        result.result_shape.rows[0].fields.size() == 1 &&
        result.result_shape.rows[0].fields[0].first == "owner_field" &&
        result.result_shape.rows[0].fields[0].second.encoded_value == "owner_row_value" &&
        result.result_shape.rows[0].fields[0].second.binary_value ==
            base.result_shape.rows[0].fields[0].second.binary_value,
        "fabricated or changed result rows");
  Check(!result.diagnostics.empty() &&
        result.diagnostics[0].occurrence_uuid == base.diagnostics[0].occurrence_uuid &&
        result.diagnostics[0].detail == "retained", "owner diagnostic changed");
}
void Run() {
  static_assert(sizeof(api::EngineUuid) == 16);
  const auto base = Base();
  auto result = base;
  api::AddDdlPublicationResult(&result, "ddl.create_table", "table", Id(1), Id(2), "table");
  Check(result.ok && result.operation_id == "ddl.create_table" &&
        result.primary_object.uuid == Id(1) && result.primary_object.object_kind == "table" &&
        result.catalog_row_uuid == Id(2), "binary response identities not bound");
  CheckOwner(result, base);
  Check(result.diagnostics.size() == 1, "success invented a diagnostic");
  auto same = result;
  api::AddDdlPublicationResult(&same, {}, {}, Id(1), {}, "unexecuted_scope");
  Check(same.ok && same.operation_id == result.operation_id &&
        same.catalog_row_uuid == Id(2), "existing binding not preserved");
  CheckOwner(same, base);
  auto missing_row = base;
  api::AddDdlPublicationResult(&missing_row, "ddl.create_table", "table", Id(1));
  Check(missing_row.ok && missing_row.catalog_row_uuid.is_nil(),
        "decorator fabricated a persisted catalog row");
  CheckOwner(missing_row, base);
  auto failed = result; failed.ok = false;
  api::AddDdlPublicationResult(&failed, "different", "different", Id(9), Id(8));
  Check(!failed.ok && failed.operation_id == result.operation_id &&
        failed.primary_object.uuid == Id(1) && failed.diagnostics.size() == 1,
        "failed owner result overwritten");
  CheckOwner(failed, base);
  api::AddDdlPublicationResult(nullptr, {}, {}, {});

  for (unsigned variant = 0; variant != 11; ++variant) {
    auto candidate = result;
    auto object = Id(1), row = Id(2);
    std::string operation = "ddl.create_table", kind = "table";
    switch (variant) {
      case 0: object = {}; break;
      case 1: object.bytes[6] = 0x40; break;
      case 2: object.bytes[8] = 0xc0; break;
      case 3: row.bytes[6] = 0x10; break;
      case 4: object = Id(9); break;
      case 5: row = Id(9); break;
      case 6: operation = "ddl.drop_table"; break;
      case 7: kind = "index"; break;
      case 8: candidate.catalog_row_uuid.bytes[6] = 0x10; break;
      case 9: candidate.operation_id.clear(); operation.clear(); break;
      case 10: candidate.primary_object.object_kind.clear(); kind.clear(); break;
    }
    const auto before = candidate;
    api::AddDdlPublicationResult(&candidate, operation, kind, object, row);
    Check(!candidate.ok && candidate.primary_object.uuid == before.primary_object.uuid &&
          candidate.primary_object.object_kind == before.primary_object.object_kind &&
          candidate.catalog_row_uuid == before.catalog_row_uuid &&
          candidate.operation_id == before.operation_id, "mismatch rebound a result identity");
    Check(candidate.diagnostics.size() == 2 &&
          candidate.diagnostics.back().error &&
          candidate.diagnostics.back().code == "CATALOG.INVALID_INPUT" &&
          candidate.diagnostics.back().canonical_metadata.has_value(),
          "binding refusal lacks registered diagnostic");
    CheckOwner(candidate, base);
  }
  for (unsigned byte = 0; byte != 16; ++byte) {
    auto candidate = base;
    auto object = Id(1), row = Id(2);
    object.bytes[byte] ^= 4; row.bytes[byte] ^= 8;
    api::AddDdlPublicationResult(&candidate, "ddl.create_table", "table", object, row);
    Check(candidate.ok && candidate.primary_object.uuid == object &&
          candidate.catalog_row_uuid == row, "UUID byte lost during binding");
    CheckOwner(candidate, base);
    object.bytes[byte] ^= 1;
    api::AddDdlPublicationResult(&candidate, "ddl.create_table", "table", object, row);
    Check(!candidate.ok, "mismatched UUID byte aliased");
  }
  // Aliasing arguments into the result cannot invalidate the staged binding.
  api::AddDdlPublicationResult(&result, result.operation_id,
      result.primary_object.object_kind, result.primary_object.uuid, result.catalog_row_uuid);
  Check(result.ok && result.primary_object.uuid == Id(1), "aliased input corrupted binding");

  for (bool refuse : {false, true}) {
    bool finished = false;
    const std::string operation(80, 'o'), kind(80, 'k');
    for (long n = 0; n != 1024; ++n) {
      auto candidate = base;
      fault::remaining = n; fault::hit = false;
      bool threw = false;
      try {
        api::AddDdlPublicationResult(&candidate, operation, kind, refuse ? api::EngineUuid{} : Id(1), Id(2));
      } catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      fault::remaining = -1;
      CheckOwner(candidate, base);
      if (!hit) {
        Check(!threw && candidate.ok == !refuse, "fault sweep terminal state incorrect");
        finished = true; break;
      }
      ++faults;
      Check(threw && candidate.ok && candidate.primary_object.uuid.is_nil() &&
            candidate.catalog_row_uuid.is_nil() && candidate.operation_id.empty() &&
            candidate.primary_object.object_kind.empty() && candidate.diagnostics.size() == 1,
            "allocation failure partially changed result");
    }
    Check(finished, "fault sweep failed to reach successful allocation");
  }
}
}
int main() {
  try {
    Run();
    std::cout << "DDL identity checks=" << checks << " allocation_faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& error) {
    fault::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
