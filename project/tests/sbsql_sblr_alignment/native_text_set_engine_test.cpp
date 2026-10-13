// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/engine_statement_fixture.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "query/expression_api.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "datatype_text_set.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "transaction/transaction_api.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace mm = scratchbird::core::memory;
namespace db = scratchbird::storage::database;
namespace tests = scratchbird::tests;
void Check(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
template<class T> void Ok(const T& value, const char* detail) {
  if (!value.ok) {
    for (const auto& d : value.diagnostics) std::cerr << d.code << ':' << d.detail << '\n';
    throw std::runtime_error(detail);
  }
}
struct Directory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("scratchbird_native_text_set_engine_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { std::filesystem::create_directories(path); }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct TextProfile {
  api::EngineDescriptor descriptor;
  dt::NativeTextSetBindingV1 binding;
};
TextProfile Profile(const api::EngineRequestContext& context, const char* collation) {
  const auto resource = api::LookupEngineResourceDescriptorByName(context, collation, "collation");
  Check(resource.ok && resource.resource_descriptor.present, "live node collation lookup");
  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV3();
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
    return row.legacy_fields.catalog_snapshot_uuid == context.datatype_catalog_snapshot_uuid &&
        row.legacy_fields.catalog_generation == context.datatype_catalog_generation &&
        row.legacy_fields.registry_generation == context.datatype_registry_generation &&
        dt::IsExactCanonicalTextTypeCodecIdentityV1(row.legacy_fields);
  });
  Check(found != rows.end(), "actual statement TEXT cohort");
  const auto& row = found->legacy_fields;
  TextProfile p;
  auto& d = p.descriptor;
  d.descriptor_uuid = api::GenerateCrudEngineUuid("object"); d.descriptor_kind = "scalar";
  d.canonical_type_name = "character"; d.type_uuid = row.type_uuid;
  d.datatype_descriptor_uuid = row.descriptor_uuid; d.datatype_descriptor_generation = row.descriptor_generation;
  d.datatype_cohort = {context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation, context.datatype_registry_generation};
  d.charset_uuid = resource.resource_descriptor.parent_resource_uuid;
  d.collation_uuid = resource.resource_descriptor.resource_uuid;
  api::CatalogColumnMetadata fields; fields.text["nullability"] = "nullable";
  Check(api::EncodeCatalogColumnMetadata(fields, &d.encoded_descriptor), "native descriptor metadata");
  auto& b = p.binding;
  b.policy_uuid = dt::kNativeTextSetPolicyV1; b.policy_generation = 1;
  b.catalog_snapshot_uuid = context.datatype_catalog_snapshot_uuid;
  b.catalog_generation = context.datatype_catalog_generation; b.registry_generation = context.datatype_registry_generation;
  b.element_type_uuid = row.type_uuid; b.element_type_generation = row.type_generation;
  b.text_seed = api::TextSeedFromResource(resource.resource_descriptor); b.allow_null_elements = true;
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = {scratchbird::core::platform::UuidKind::object, row.descriptor_uuid};
  metadata.descriptor_epoch = row.descriptor_generation;
  metadata.charset_uuid = {scratchbird::core::platform::UuidKind::object, d.charset_uuid};
  metadata.collation_uuid = {scratchbird::core::platform::UuidKind::object, d.collation_uuid};
  const auto bound = dt::LookupExecutionTypeDescriptorFromCatalog(dt::CanonicalTypeId::character, metadata);
  Check(bound.ok(), "native TEXT element descriptor"); b.element = bound.descriptor; b.element.nullable_allowed = true;
  return p;
}
api::EngineTypedValue Frame(const TextProfile& p, std::initializer_list<dt::NativeTextSetElementV1> values) {
  dt::NativeTextSetControlV1 control{1024, 65536, 65536, 4*1024*1024, 1024*1024};
  control.memory = std::pmr::new_delete_resource(); // Fixture encoding, NOT runtime admission.
  const auto encoded = dt::EncodeNativeTextSetV1(p.binding, {values.begin(), values.size()}, control);
  Check(encoded.ok(), "fixture native SET encoding");
  api::EngineTypedValue value; value.descriptor.canonical_type_name = "set_value";
  value.binary_value.assign(encoded.encoded_set.begin(), encoded.encoded_set.end()); return value;
}
bool Truth(const api::EngineSetOperationResult& r) {
  Ok(r, "native SET operation");
  Check(r.value.encoded_value.empty() && r.value.binary_value.size() == 1 &&
      !r.value.descriptor.type_uuid.is_nil() && !r.value.descriptor.datatype_descriptor_uuid.is_nil(),
      "native bound BOOL result");
  return r.value.binary_value[0] == 1;
}
api::EngineDescriptor Relational(const api::EngineDescriptor& descriptor, const api::EngineRequestContext& context) {
  const auto* row = dt::FindDatatypeTypeCodecIdentityV3(descriptor.datatype_cohort.catalog_snapshot_uuid,
      descriptor.datatype_cohort.catalog_generation, descriptor.datatype_cohort.registry_generation,
      descriptor.datatype_descriptor_uuid, descriptor.datatype_descriptor_generation);
  Check(row != nullptr, "relational fixture registry");
  api::RelationalTypeDescriptor record;
  record.descriptor_id = 1; record.descriptor_uuid = descriptor.descriptor_uuid;
  record.type_uuid = descriptor.type_uuid; record.nullability = api::RelationalNullability::kNullable;
  record.collation_uuid = descriptor.collation_uuid; record.datatype_identity_authoritative = true;
  record.descriptor_generation = row->legacy_fields.descriptor_generation;
  record.type_generation = row->legacy_fields.type_generation; record.codec_id = row->legacy_fields.codec_id;
  record.codec_version = row->legacy_fields.codec_version; record.codec_generation = row->legacy_fields.codec_generation;
  record.statement_receipt_uuid = context.statement_receipt_uuid;
  record.datatype_catalog_snapshot_uuid = descriptor.datatype_cohort.catalog_snapshot_uuid;
  record.datatype_catalog_generation = descriptor.datatype_cohort.catalog_generation;
  record.datatype_registry_generation = descriptor.datatype_cohort.registry_generation;
  std::vector<std::uint8_t> bytes;
  Check(scratchbird::engine::sblr::EncodeRelationalTypeDescriptorV1(record, &bytes), "relational fixture encoding");
  auto result = descriptor; result.encoded_descriptor.assign(bytes.begin(), bytes.end()); return result;
}
void Exercise(const api::EngineRequestContext& context) {
  const auto p = Profile(context, "SB_UCA_17_PRIMARY");
  // A relational occurrence UUID is not its native datatype descriptor UUID.
  // Both actual collation profiles must work under the live statement cohort.
  for (const auto name : {"SB_UCA_17_PRIMARY", "SB_UTF8_BINARY"}) {
    const auto profile = Profile(context, name);
    api::EngineCompareScalarValuesRequest compare;
    compare.context = context;
    compare.left_value.descriptor = Relational(profile.descriptor, context);
    compare.right_value.descriptor = compare.left_value.descriptor;
    compare.left_value.encoded_value = "A";
    compare.right_value.encoded_value = "a";
    const auto result = api::EngineCompareScalarValues(compare);
    Ok(result, "live native character comparison");
    Check(result.collation_uuid == profile.descriptor.collation_uuid && result.collation_epoch != 0 &&
        (std::string_view(name) == "SB_UCA_17_PRIMARY" ? result.comparison == 0 : result.comparison < 0),
        "native comparison lost exact collation semantics");
    for (const bool right : {false, true}) {
      for (unsigned mutation = 0; mutation != 19; ++mutation) {
        auto forged = compare;
        auto& value = right ? forged.right_value : forged.left_value;
        auto& descriptor = value.descriptor;
        if (mutation < 11 || mutation == 18) {
          switch (mutation) {
            case 0: descriptor.datatype_descriptor_uuid = api::GenerateCrudEngineUuid("object"); break;
            case 1: ++descriptor.datatype_descriptor_generation; break;
            case 2: descriptor.datatype_cohort.catalog_snapshot_uuid = api::GenerateCrudEngineUuid("object"); break;
            case 3: ++descriptor.datatype_cohort.catalog_generation; break;
            case 4: ++descriptor.datatype_cohort.registry_generation; break;
            case 5: descriptor.type_uuid = api::GenerateCrudEngineUuid("object"); break;
            case 6: descriptor.charset_uuid = {}; break;
            case 7: descriptor.collation_uuid = api::GenerateCrudEngineUuid("object"); break;
            case 8: value.binary_value = {0}; break;
            case 9: value.state = static_cast<api::EngineValueState>(255); break;
            case 10: value.setState(api::EngineValueState::sql_null); break; // Dirty NULL keeps its payload.
            case 18: descriptor.charset_uuid = api::GenerateCrudEngineUuid("object"); break;
          }
        } else {
          api::RelationalTypeDescriptor record;
          Check(scratchbird::engine::sblr::DecodeRelationalTypeDescriptorV1(
              reinterpret_cast<const std::uint8_t*>(descriptor.encoded_descriptor.data()),
              descriptor.encoded_descriptor.size(), &record), "comparison mutation decode");
          switch (mutation) {
            case 11: record.statement_receipt_uuid = api::GenerateCrudEngineUuid("object"); break;
            case 12: ++record.descriptor_generation; break;
            case 13: ++record.type_generation; break;
            case 14: ++record.codec_version; break;
            case 15: ++record.codec_generation; break;
            case 16: ++record.datatype_catalog_generation; break;
            case 17: record.codec_id = "foreign.text.codec"; break;
          }
          std::vector<std::uint8_t> bytes;
          Check(scratchbird::engine::sblr::EncodeRelationalTypeDescriptorV1(record, &bytes),
              "comparison mutation encode");
          descriptor.encoded_descriptor.assign(bytes.begin(), bytes.end());
        }
        const auto refused = api::EngineCompareScalarValues(forged);
        if (refused.ok || refused.diagnostics.empty() || !refused.evidence.empty())
          std::cerr << "comparison mutation " << name << ' ' << right << ' ' << mutation
                    << " ok=" << refused.ok << " evidence=" << refused.evidence.size() << '\n';
        Check(!refused.ok && !refused.diagnostics.empty() && refused.evidence.empty(),
            "forged comparison produced success or partial evidence");
        auto null_peer = forged;
        auto& peer = right ? null_peer.left_value : null_peer.right_value;
        peer.encoded_value.clear(); peer.setState(api::EngineValueState::sql_null);
        const auto null_refusal = api::EngineCompareScalarValues(null_peer);
        Check(!null_refusal.ok && !null_refusal.diagnostics.empty() && null_refusal.evidence.empty(),
            "NULL peer concealed forged comparison authority or malformed payload");
        Check(api::EngineCompareScalarValues(compare).ok, "valid comparison retry failed");
      }
      auto null = compare;
      auto& operand = right ? null.right_value : null.left_value;
      operand.encoded_value.clear(); operand.setState(api::EngineValueState::sql_null);
      const auto unknown = api::EngineCompareScalarValues(null);
      Ok(unknown, "bound NULL comparison");
      Check(std::any_of(unknown.evidence.begin(), unknown.evidence.end(), [](const auto& evidence) {
        const auto* value = std::get_if<std::string>(&evidence.evidence_id);
        return evidence.evidence_kind == "sql_truth_value" && value && *value == "unknown";
      }), "NULL comparison did not preserve SQL unknown");
      for (const std::string bytes : {std::string{"AA"}, std::string{"\xc3\xa9\xc3\xa9"},
                                      std::string{"\xc0\xaf",2}, std::string{"\xed\xa0\x80",3}}) {
        auto bounded = compare;
        auto& operand = right ? bounded.right_value : bounded.left_value;
        api::RelationalTypeDescriptor record;
        Check(scratchbird::engine::sblr::DecodeRelationalTypeDescriptorV1(
            reinterpret_cast<const std::uint8_t*>(operand.descriptor.encoded_descriptor.data()),
            operand.descriptor.encoded_descriptor.size(), &record), "bounded comparison decode");
        record.width = 1;
        std::vector<std::uint8_t> encoded;
        Check(scratchbird::engine::sblr::EncodeRelationalTypeDescriptorV1(record, &encoded),
              "bounded comparison encode");
        operand.descriptor.encoded_descriptor.assign(encoded.begin(), encoded.end());
        operand.encoded_value = bytes;
        Check(!api::EngineCompareScalarValues(bounded).ok, "TEXT extent or invalid UTF8 was ignored");
        auto& peer = right ? bounded.left_value : bounded.right_value;
        peer.encoded_value.clear(); peer.setState(api::EngineValueState::sql_null);
        Check(!api::EngineCompareScalarValues(bounded).ok, "NULL concealed TEXT extent or invalid UTF8");
        operand.encoded_value = "\xc3\xa9";
        Check(api::EngineCompareScalarValues(bounded).ok, "one multibyte scalar exceeded character width1");
        peer.setState(api::EngineValueState::value); peer.encoded_value = "\xc3\xa9";
        const auto multibyte_equal = api::EngineCompareScalarValues(bounded);
        Check(multibyte_equal.ok && multibyte_equal.comparison == 0,
              "one multibyte scalar did not execute an actual comparison");
        operand.encoded_value.clear();
        Check(api::EngineCompareScalarValues(bounded).ok, "empty present TEXT was confused with NULL");
        peer.encoded_value.clear();
        const auto empty_equal = api::EngineCompareScalarValues(bounded);
        Check(empty_equal.ok && empty_equal.comparison == 0,
              "empty present TEXT did not execute an actual comparison");
      }
    }
    auto borrowed = compare;
    borrowed.context = {}; borrowed.left_value = {}; borrowed.right_value = {};
    borrowed.borrowed_context = &compare.context;
    borrowed.borrowed_left_value = &compare.left_value;
    borrowed.borrowed_right_value = &compare.right_value;
    const auto borrowed_result = api::EngineCompareScalarValues(borrowed);
    Check(borrowed_result.ok && borrowed_result.comparison == result.comparison &&
        borrowed_result.collation_uuid == result.collation_uuid,
        "borrowed comparison changed native authority or semantics");
  }
  api::EngineSetOperationRequest request; request.context = context;
  request.descriptors.push_back(p.descriptor); request.allow_null_elements = true;
  request.set_operation = "membership";
  request.left_set = Frame(p, {{false,"A"},{false,"a"},{false,"e\xcc\x81"},{true,{}},{false,"<NULL>"},{false,{}}});
  request.right_set_or_value.descriptor = p.descriptor;
  for (const std::string bytes : {"a", "A", "\xc3\xa9", "<NULL>", ""}) {
    request.right_set_or_value.binary_value.assign(bytes.begin(), bytes.end());
    Check(Truth(api::EngineSetOperation(request)), "live collation membership");
  }
  request.right_set_or_value.binary_value.clear(); request.right_set_or_value.setState(api::EngineValueState::sql_null);
  Check(Truth(api::EngineSetOperation(request)), "SQL NULL element membership");
  auto dirty = request; dirty.right_set_or_value.binary_value = {0};
  const auto dirty_result = api::EngineSetOperation(dirty);
  Check(!dirty_result.ok && dirty_result.diagnostics.front().code == "DATATYPE.NULL_STATE.INVALID", "dirty NULL diagnostic");
  for (const auto state : {api::EngineValueState::sql_null, api::EngineValueState::value}) {
    for (const auto modifier : {"precision", "scale", "vector_dimensions", "container_rank", "unknown_modifier"}) {
      auto altered = request; altered.right_set_or_value.setState(state);
      api::CatalogColumnMetadata fields; fields.text = {{"nullability","nullable"},{modifier,"1"}};
      Check(api::EncodeCatalogColumnMetadata(fields, &altered.right_set_or_value.descriptor.encoded_descriptor), "modifier fixture");
      Check(!api::EngineSetOperation(altered).ok, "member modifier discarded");
    }
    for (const auto modifier : {"timezone_uuid", "security_policy_uuid", "element_descriptor_uuid", "domain_uuid"}) {
      auto altered = request; altered.right_set_or_value.setState(state);
      api::CatalogColumnMetadata fields; fields.text["nullability"] = "nullable";
      fields.identities[modifier] = api::GenerateCrudEngineUuid("object");
      Check(api::EncodeCatalogColumnMetadata(fields, &altered.right_set_or_value.descriptor.encoded_descriptor), "identity modifier fixture");
      Check(!api::EngineSetOperation(altered).ok, "member identity modifier discarded");
    }
  }
  auto forged = request; forged.context.resource_admission_uuid = api::GenerateCrudEngineUuid("object");
  Check(!api::EngineSetOperation(forged).ok, "forged admission accepted");
  forged = request; forged.context.dml_update_resource_receipt.reset();
  Check(!api::EngineSetOperation(forged).ok, "copied flags minted authority");
  forged = request; ++forged.descriptors[0].datatype_cohort.registry_generation;
  Check(!api::EngineSetOperation(forged).ok, "stale cohort accepted");
  forged = request; forged.descriptors.front().encoded_descriptor.assign(1048576, 'x');
  Check(!api::EngineSetOperation(forged).ok, "oversized element metadata accepted");
  auto legacy = request; legacy.left_set.encoded_value.assign(legacy.left_set.binary_value.begin(), legacy.left_set.binary_value.end());
  legacy.left_set.binary_value.clear(); Check(!api::EngineSetOperation(legacy).ok, "native SET text carrier accepted");
  request.set_operation = "cardinality"; request.right_set_or_value = {};
  const auto count = api::EngineSetOperation(request); Ok(count, "unary cardinality");
  Check(count.value.encoded_value.empty() && count.value.binary_value == std::vector<std::uint8_t>({5,0,0,0,0,0,0,0}), "native UINT64 cardinality");
  request.right_set_or_value = Frame(p, {{false,"a"}});
  for (const auto op : {"superset", "equals", "subset"}) {
    request.set_operation = op; const bool expected = std::string_view(op) == "superset";
    Check(Truth(api::EngineSetOperation(request)) == expected, "binary-frame comparison");
  }
  request.right_set_or_value = request.left_set; request.set_operation = "equals";
  Check(Truth(api::EngineSetOperation(request)), "SET equality");
  // The SBLR dispatcher copies the base DTO only: options and row operands
  // must reach the same receiving path without requiring derived DTO fields.
  auto base_route = request; base_route.set_operation.clear(); base_route.option_envelopes = {"set_operation:equals"};
  api::EngineRowValue row; row.fields = {{"left",request.left_set},{"right",request.right_set_or_value}};
  base_route.rows = {row}; base_route.left_set = {}; base_route.right_set_or_value = {};
  Check(Truth(api::EngineSetOperation(base_route)), "base SBLR request operand selection");
  const auto binary = Profile(context, "SB_UTF8_BINARY");
  request.descriptors[0] = binary.descriptor; request.left_set = Frame(binary, {{false,"A"}});
  request.set_operation = "membership"; request.right_set_or_value = {};
  request.right_set_or_value.descriptor = binary.descriptor; request.right_set_or_value.binary_value = {'a'};
  Check(!Truth(api::EngineSetOperation(request)), "binary profile silently collated");
  auto& manager = mm::DefaultMemoryManager();
  mm::MemoryFailureInjectionConfiguration fault;
  fault.test_guard = mm::MakeMemoryFailureInjectionTestGuard(); fault.fixture_enabled = true;
  fault.fixture_name = "native_text_set_engine"; fault.evidence_note = "actual native statement PMR fault";
  mm::MemoryFailureInjectionRule rule; rule.rule_id = "first_native_set_allocation";
  rule.scope_kind = mm::MemoryFailureInjectionScopeKind::context;
  rule.binary_scope_uuid = context.statement_receipt_uuid.bytes; fault.rules.push_back(rule);
  Check(manager.EnableAllocationFailureInjection(fault).ok(), "physical fault enable");
  const auto failed = api::EngineSetOperation(request);
  Check(manager.DisableAllocationFailureInjection().ok(), "physical fault disable");
  Check(!failed.ok, "physical allocation fault reported success");
  Check(!Truth(api::EngineSetOperation(request)), "post-failure retry");
}
int main() try {
  Check(mm::ConfigureDefaultMemoryManagerForFixture(mm::DefaultLocalEngineMemoryPolicy(),
      "native_text_set_engine").ok(), "explicit fixture physical memory policy");
  Directory directory;
  db::DatabaseCreateConfig create; create.path = (directory.path / "set.sbdb").string(); create.page_size = 8192;
  create.database_uuid = {scratchbird::core::platform::UuidKind::database, api::GenerateCrudEngineUuid("database")};
  create.filespace_uuid = {scratchbird::core::platform::UuidKind::filespace, api::GenerateCrudEngineUuid("filespace")};
  tests::ConfigureCredentialedFixtureBootstrap(create); Check(db::CreateDatabaseFile(create).ok(), "credentialed database");
  auto context = tests::BootstrapFixtureOwnerContext(create);
  tests::FixtureEngineSession session(context);
  api::EngineBeginTransactionRequest begin; begin.context = context; begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin); Ok(begun, "native BEGIN");
  context.transaction_uuid = begun.transaction_uuid; context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  const auto cancellation = std::make_shared<std::atomic<bool>>(false);
  context.query_cancellation_requested = [cancellation]{return cancellation->load();};
  {
    tests::FixtureEngineStatement statement(session, context);
    const auto memory_before = mm::DefaultMemoryManager().Snapshot();
    scratchbird::core::platform::DiagnosticRecord admission_diagnostic;
    const auto scratch_status = api::RunWithStatementScratchMemoryV1(statement.context, [&](auto& owner) {
      const auto allocated = owner.memory()->allocate(4096, alignof(std::max_align_t));
      Check(owner.Snapshot().allocated_bytes == 4096, "actual scratch allocation accounting");
      bool entered = false;
      Check(api::RunWithStatementScratchMemoryV1(statement.context, [&](auto&){entered=true;}) ==
          SB_ENGINE_STATUS_CONFLICT && !entered, "direct synchronous reentry did not refuse");
      owner.memory()->deallocate(allocated, 4096, alignof(std::max_align_t));
      Check(owner.Snapshot().allocated_bytes == 0, "scratch deallocation accounting");
    }, &admission_diagnostic);
    if (scratch_status != SB_ENGINE_STATUS_OK) std::cerr << "scratch admission: " << sb_engine_status_name(scratch_status) << '\n';
    if (scratch_status != SB_ENGINE_STATUS_OK) std::cerr << admission_diagnostic.diagnostic_code << ':' << admission_diagnostic.message_key << '\n';
    Check(scratch_status == SB_ENGINE_STATUS_OK, "issued physical scratch callback");
    const auto memory_after = mm::DefaultMemoryManager().Snapshot();
    Check(memory_after.current_bytes == memory_before.current_bytes &&
        memory_after.reserved_capacity_bytes == memory_before.reserved_capacity_bytes &&
        memory_after.active_capacity_reservation_count == memory_before.active_capacity_reservation_count,
        "physical scratch or reservation escaped callback");
    Exercise(statement.context);
    {
      tests::FixtureEngineStatement second(session, context);
      const auto profile = Profile(statement.context, "SB_UTF8_BINARY");
      api::EngineSetOperationRequest request; request.context = statement.context;
      request.allow_null_elements = true; request.set_operation = "membership";
      request.left_set = Frame(profile, {{false,"a"},{true,{}}});
      request.descriptors = {Relational(profile.descriptor, statement.context)};
      request.right_set_or_value.descriptor = request.descriptors.front(); request.right_set_or_value.binary_value = {'a'};
      Check(Truth(api::EngineSetOperation(request)), "live relational TEXT receipt");
      for (const auto state : {api::EngineValueState::value, api::EngineValueState::sql_null}) {
        auto altered = request; altered.right_set_or_value.setState(state);
        if (state == api::EngineValueState::sql_null) altered.right_set_or_value.binary_value.clear();
        Check(Truth(api::EngineSetOperation(altered)), "same-receipt relational member");
        altered.right_set_or_value.descriptor = Relational(profile.descriptor, second.context);
        Check(!api::EngineSetOperation(altered).ok, "foreign relational member receipt accepted");
        altered.right_set_or_value.descriptor = request.descriptors.front();
        altered.descriptors.front() = Relational(profile.descriptor, second.context);
        Check(!api::EngineSetOperation(altered).ok, "foreign relational element receipt accepted");
      }
      cancellation->store(true);
      const auto cancelled = api::EngineSetOperation(request);
      cancellation->store(false);
      Check(!cancelled.ok && cancelled.diagnostics.front().code == "PROCESS.CANCELLED", "actual issued cancellation diagnostic");
    }
    auto stale = statement.context; statement.Release();
    bool entered = false;
    Check(api::RunWithStatementScratchMemoryV1(stale, [&](auto&){entered = true;}) != SB_ENGINE_STATUS_OK && !entered,
        "retired receipt minted physical scratch");
  }
  {
    tests::FixtureEngineStatement statement(session, context);
    const auto live = statement.context;
    std::atomic<bool> entered=false, finish=false, retirement_started=false, retired=false, observed=false, operation_done=false;
    sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
    std::exception_ptr failure;
    std::thread operation([&] {
      status = api::RunWithStatementScratchMemoryV1(live, [&](auto& owner) {
        auto* pointer = owner.memory()->allocate(4096, alignof(std::max_align_t));
        entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!owner.teardown_requests() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Check(owner.teardown_requests() != 0, "receipt retirement never entered owning gate");
        observed.store(true);
        while (!finish.load()) std::this_thread::yield();
        owner.memory()->deallocate(pointer, 4096, alignof(std::max_align_t));
      });
      if (!entered.load()) entered.store(true); // Admission failure must not hang the controller.
      operation_done.store(true);
    });
    while (!entered.load()) std::this_thread::yield();
    std::thread retirement([&] { retirement_started.store(true); try { statement.Release(); retired.store(true); } catch (...) {failure=std::current_exception();} });
    while (!retirement_started.load()) std::this_thread::yield();
    while (!observed.load() && !operation_done.load()) std::this_thread::yield();
    const bool retired_early = retired.load();
    finish.store(true); operation.join(); retirement.join();
    if (failure) std::rethrow_exception(failure);
    Check(status == SB_ENGINE_STATUS_OK && observed.load() && retired.load() && !retired_early, "concurrent scratch completion/receipt retirement");
  }
  api::EngineRollbackTransactionRequest rollback; rollback.context = context; Ok(api::EngineRollbackTransaction(rollback), "native rollback");
  session.Close();
  {
    auto closing = tests::BootstrapFixtureOwnerContext(create);
    tests::FixtureEngineSession closing_session(closing);
    api::EngineBeginTransactionRequest begin_close; begin_close.context = closing;
    begin_close.isolation_level = "read_committed";
    const auto active = api::EngineBeginTransaction(begin_close); Ok(active, "close-control BEGIN");
    closing.transaction_uuid = active.transaction_uuid; closing.local_transaction_id = active.local_transaction_id;
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest acquire; acquire.engine_context = &closing;
    acquire.exact_transaction_uuid = closing.transaction_uuid;
    bridge::StatementContextReceiptHandle handle; bridge::StatementContextReceiptView view;
    tests::FixtureEngineSession::Check(bridge::AcquireStatementContextReceipt(closing_session.get(), &acquire,
        &handle, &view, nullptr), nullptr, "shutdown-control statement");
    api::EngineRequestContext issued;
    tests::FixtureEngineSession::Check(bridge::CopyStatementContextEngineContextV1(handle, &issued, nullptr),
        nullptr, "shutdown-control context");
    std::atomic<bool> entered=false, finish=false, shutdown_started=false, shutdown_completed=false, observed=false, operation_done=false;
    sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
    std::exception_ptr failure;
    std::thread operation([&] {
      status = api::RunWithStatementScratchMemoryV1(issued, [&](auto& owner) {
        auto* pointer = owner.memory()->allocate(4096, alignof(std::max_align_t));
        entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!owner.teardown_requests() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Check(owner.teardown_requests() != 0, "session shutdown never entered owning gate");
        observed.store(true);
        while (!finish.load()) std::this_thread::yield();
        owner.memory()->deallocate(pointer, 4096, alignof(std::max_align_t));
      });
      if (!entered.load()) entered.store(true);
      operation_done.store(true);
    });
    while (!entered.load()) std::this_thread::yield();
    std::thread shutdown([&] { shutdown_started.store(true); try {closing_session.Close(); shutdown_completed.store(true);} catch (...) {failure=std::current_exception();} });
    while (!shutdown_started.load()) std::this_thread::yield();
    while (!observed.load() && !operation_done.load()) std::this_thread::yield();
    const bool closed_early = shutdown_completed.load();
    finish.store(true); operation.join(); shutdown.join();
    if (failure) std::rethrow_exception(failure);
    Check(status == SB_ENGINE_STATUS_OK && observed.load() && shutdown_completed.load() && !closed_early, "shutdown reclaimed active scratch");
    bool entered_after_shutdown = false;
    Check(api::RunWithStatementScratchMemoryV1(issued, [&](auto&){entered_after_shutdown=true;}) != SB_ENGINE_STATUS_OK &&
        !entered_after_shutdown, "closed session reissued scratch");
  }
  std::cout << "native TEXT SET engine admission and physical scratch PASS\n";
  return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
