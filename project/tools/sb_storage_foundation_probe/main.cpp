// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-STORAGE-FOUNDATION-PROBE-ANCHOR
#include "catalog_record_codec.hpp"
#include "catalog_records.hpp"
#include "database_lifecycle.hpp"
#include "datatype_binary.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"
#include "page_header.hpp"
#include "page_layout.hpp"
#include "row_data_page.hpp"
#include "runtime_platform.hpp"
#include "toast_page.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::core::catalog::BuiltinCatalogRecordDescriptors;
using scratchbird::core::catalog::CatalogRecordDescriptor;
using scratchbird::core::catalog::CatalogTypedRecord;
using scratchbird::core::catalog::DecodeCatalogTypedRecord;
using scratchbird::core::catalog::EncodeCatalogTypedRecord;
using scratchbird::core::catalog::LookupCatalogRecordDescriptor;
using scratchbird::core::datatypes::BuiltinDatatypeDescriptors;
using scratchbird::core::datatypes::CanonicalTypeId;
using scratchbird::core::datatypes::DatatypeBinaryValue;
using scratchbird::core::datatypes::DecodeDatatypeBinaryValue;
using scratchbird::core::datatypes::EncodeDatatypeBinaryValue;
using scratchbird::core::datatypes::LookupDatatypeStorageLayout;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::uuid::UuidToString;
using scratchbird::storage::database::CreateDatabaseFile;
using scratchbird::storage::database::DatabaseCreateConfig;
using scratchbird::storage::database::DatabaseOpenConfig;
using scratchbird::storage::database::OpenDatabaseFile;
using scratchbird::storage::disk::PageSizeProfile;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::page::BuildRowDataPageBody;
using scratchbird::storage::page::ComputePageLayoutCapacity;
using scratchbird::storage::page::PageLayoutDescriptor;
using scratchbird::storage::page::ParseRowDataPageBody;
using scratchbird::storage::page::PlanToastValue;
using scratchbird::storage::page::RowDataCell;
using scratchbird::storage::page::RowDataPageBody;
using scratchbird::storage::page::RowDataRecord;

struct Args {
  std::string seed_pack_root;
  std::string database_path;
  u64 creation_millis = 0;
  u32 page_size = 16384;
  bool overwrite = false;
};

struct ProbeCounters {
  u32 datatype_layouts = 0;
  u32 datatype_round_trips = 0;
  u32 page_layouts = 0;
  u32 toast_plans = 0;
  u32 row_page_round_trips = 0;
  u32 catalog_record_round_trips = 0;
  u32 database_typed_catalog_records = 0;
};

void Usage() {
  std::cerr << "usage: sb_storage_foundation_probe --seed-pack-root PATH --database-path PATH --creation-ms MILLIS [--page-size BYTES] [--overwrite]\n";
}

bool ParseU64(const std::string& text, u64* value) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *value = static_cast<u64>(parsed);
  return true;
}

bool ParseU32(const std::string& text, u32* value) {
  u64 parsed = 0;
  if (!ParseU64(text, &parsed) || parsed > 0xffffffffull) {
    return false;
  }
  *value = static_cast<u32>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--seed-pack-root") {
      args->seed_pack_root = value;
    } else if (key == "--database-path") {
      args->database_path = value;
    } else if (key == "--creation-ms") {
      if (!ParseU64(value, &args->creation_millis)) {
        return false;
      }
    } else if (key == "--page-size") {
      if (!ParseU32(value, &args->page_size)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !args->seed_pack_root.empty() && !args->database_path.empty() && args->creation_millis != 0;
}

void PrintDiagnostic(const DiagnosticRecord& diagnostic) {
  std::cerr << diagnostic.diagnostic_code << ":" << diagnostic.message_key << "\n";
}

bool Fail(const std::string& code, const std::string& detail) {
  std::cerr << code << ":" << detail << "\n";
  return false;
}

TypedUuid GenerateTyped(UuidKind kind, u64 millis) {
  const auto generated = GenerateEngineIdentityV7(kind, millis);
  if (!generated.ok()) {
    return {};
  }
  return generated.value;
}

std::vector<byte> PayloadFor(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::null_type:
      return {};
    case CanonicalTypeId::boolean:
      return {1};
    case CanonicalTypeId::int8:
    case CanonicalTypeId::uint8:
      return {42};
    case CanonicalTypeId::int16:
    case CanonicalTypeId::bfloat16:
    case CanonicalTypeId::real16:
    case CanonicalTypeId::uint16: {
      std::vector<byte> value(2, 0);
      StoreLittle16(value.data(), 42);
      return value;
    }
    case CanonicalTypeId::int32:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::real32:
    case CanonicalTypeId::date: {
      std::vector<byte> value(4, 0);
      StoreLittle32(value.data(), 42);
      return value;
    }
    case CanonicalTypeId::int64:
    case CanonicalTypeId::uint64:
    case CanonicalTypeId::real64:
    case CanonicalTypeId::time: {
      std::vector<byte> value(8, 0);
      StoreLittle64(value.data(), 42);
      return value;
    }
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint128:
    case CanonicalTypeId::real128:
    case CanonicalTypeId::decimal:
    case CanonicalTypeId::timestamp:
    case CanonicalTypeId::interval:
    case CanonicalTypeId::uuid:
    case CanonicalTypeId::ip_address:
    case CanonicalTypeId::enum_value:
      return std::vector<byte>(16, 7);
    case CanonicalTypeId::network_prefix:
      return std::vector<byte>(18, 7);
    case CanonicalTypeId::mac_address:
      return std::vector<byte>(8, 7);
    case CanonicalTypeId::blob:
      return std::vector<byte>(24, 9);
    case CanonicalTypeId::character:
      return {'S', 'c', 'r', 'a', 't', 'c', 'h', 'B', 'i', 'r', 'd'};
    case CanonicalTypeId::binary:
      return {0, 1, 2, 3, 4, 5};
    case CanonicalTypeId::bit_string:
      return {'b', 'i', 't', ':', 0xf0};
    case CanonicalTypeId::decimal_float:
      return {'d', 'e', 'c', 'f', 'l', 'o', 'a', 't'};
    case CanonicalTypeId::document:
    case CanonicalTypeId::json_document:
    case CanonicalTypeId::binary_json_document:
    case CanonicalTypeId::bson_document:
    case CanonicalTypeId::xml_document:
    case CanonicalTypeId::hstore_document:
    case CanonicalTypeId::object_document:
    case CanonicalTypeId::flattened_object_document:
      return {'{', '}'};
    case CanonicalTypeId::set_value:
    case CanonicalTypeId::array:
    case CanonicalTypeId::list:
    case CanonicalTypeId::map:
    case CanonicalTypeId::row:
    case CanonicalTypeId::composite:
    case CanonicalTypeId::variant:
    case CanonicalTypeId::range:
    case CanonicalTypeId::multirange:
      return {'s', 't', 'r', 'u', 'c', 't'};
    case CanonicalTypeId::token_stream:
    case CanonicalTypeId::search_query:
    case CanonicalTypeId::search_rank_feature:
    case CanonicalTypeId::search_completion:
    case CanonicalTypeId::search_percolator:
      return {'s', 'e', 'a', 'r', 'c', 'h'};
    case CanonicalTypeId::geometry:
    case CanonicalTypeId::geography:
    case CanonicalTypeId::point:
    case CanonicalTypeId::shape:
    case CanonicalTypeId::raster:
      return {'s', 'p', 'a', 't', 'i', 'a', 'l'};
    case CanonicalTypeId::vector:
    case CanonicalTypeId::dense_vector:
    case CanonicalTypeId::sparse_vector:
    case CanonicalTypeId::binary_vector:
    case CanonicalTypeId::quantized_vector:
      return {'v', 'e', 'c'};
    case CanonicalTypeId::graph_node:
      return {'n', 'o', 'd', 'e'};
    case CanonicalTypeId::graph_edge:
      return {'e', 'd', 'g', 'e'};
    case CanonicalTypeId::graph_path:
      return {'p', 'a', 't', 'h'};
    case CanonicalTypeId::time_series_value:
      return {'s', 'e', 'r', 'i', 'e', 's'};
    case CanonicalTypeId::columnar_segment:
      return {'c', 'o', 'l', 'u', 'm', 'n'};
    case CanonicalTypeId::aggregate_state:
      return {'a', 'g', 'g', 's', 't', 'a', 't', 'e'};
    case CanonicalTypeId::hll_sketch:
    case CanonicalTypeId::bloom_filter:
    case CanonicalTypeId::quantile_sketch:
    case CanonicalTypeId::histogram_sketch:
    case CanonicalTypeId::ranking_summary:
    case CanonicalTypeId::vector_summary:
      return {'s', 'k', 'e', 't', 'c', 'h'};
    case CanonicalTypeId::lob_locator:
    case CanonicalTypeId::external_file_locator:
    case CanonicalTypeId::remote_object_locator:
    case CanonicalTypeId::bridge_handle:
    case CanonicalTypeId::cursor_handle:
    case CanonicalTypeId::system_reference:
      return {'l', 'o', 'c', 'a', 't', 'o', 'r'};
    case CanonicalTypeId::opaque_extension:
      return {'o', 'p', 'a', 'q', 'u', 'e'};
    case CanonicalTypeId::cursor:
    case CanonicalTypeId::result_set:
    case CanonicalTypeId::table_value:
      return {'r', 'e', 's', 'u', 'l', 't'};
    case CanonicalTypeId::unknown:
      return {};
  }
  return {};
}

bool CheckDatatypes(ProbeCounters* counters) {
  for (const auto& descriptor : BuiltinDatatypeDescriptors()) {
    const auto layout = LookupDatatypeStorageLayout(descriptor.type_id);
    if (!layout.ok()) {
      PrintDiagnostic(layout.diagnostic);
      return false;
    }
    ++counters->datatype_layouts;

    DatatypeBinaryValue value;
    value.type_id = descriptor.type_id;
    value.is_null = descriptor.type_id == CanonicalTypeId::null_type;
    value.payload = PayloadFor(descriptor.type_id);
    value.payload_is_toast_reference = descriptor.type_id == CanonicalTypeId::blob;
    const auto encoded = EncodeDatatypeBinaryValue(value);
    if (!encoded.ok()) {
      PrintDiagnostic(encoded.diagnostic);
      return false;
    }
    const auto decoded = DecodeDatatypeBinaryValue(encoded.encoded);
    if (!decoded.ok()) {
      PrintDiagnostic(decoded.diagnostic);
      return false;
    }
    if (decoded.value.type_id != value.type_id || decoded.value.payload.size() != value.payload.size()) {
      return Fail("SB-STORAGE-FOUNDATION-DATATYPE-ROUNDTRIP-MISMATCH", descriptor.stable_name);
    }
    ++counters->datatype_round_trips;
  }
  return true;
}

bool CheckPageLayouts(ProbeCounters* counters, u32 page_size) {
  const std::vector<PageType> page_types = {
      PageType::database_header,
      PageType::allocation_map,
      PageType::catalog,
      PageType::transaction_inventory,
      PageType::row_data,
      PageType::index_btree,
      PageType::blob,
      PageType::metrics,
      PageType::archive,
      PageType::columnar,
      PageType::vector,
      PageType::graph,
      PageType::reserved_local,
      PageType::cluster_decision,
      PageType::cluster_route,
      PageType::cluster_catalog,
      PageType::cluster_transaction,
      PageType::encrypted_opaque,
  };
  for (PageType page_type : page_types) {
    const auto capacity = ComputePageLayoutCapacity(page_type, page_size);
    if (!capacity.ok()) {
      PrintDiagnostic(capacity.diagnostic);
      return false;
    }
    ++counters->page_layouts;
  }
  const auto toast = PlanToastValue(100000, page_size);
  if (!toast.ok()) {
    PrintDiagnostic(toast.diagnostic);
    return false;
  }
  if (toast.plan.chunk_count == 0) {
    return Fail("SB-STORAGE-FOUNDATION-TOAST-PLAN-EMPTY", "chunk_count");
  }
  ++counters->toast_plans;
  return true;
}

bool CheckRowPage(ProbeCounters* counters, u32 page_size, u64 creation_millis) {
  RowDataRecord row;
  row.row_uuid = GenerateTyped(UuidKind::row, creation_millis + 1000);
  row.transaction_uuid = GenerateTyped(UuidKind::transaction, creation_millis + 1001);
  row.local_transaction_id = 1;
  row.row_version = 1;

  RowDataCell bool_cell;
  bool_cell.column_ordinal = 1;
  bool_cell.value.type_id = CanonicalTypeId::boolean;
  bool_cell.value.payload = {1};
  row.cells.push_back(bool_cell);

  RowDataCell text_cell;
  text_cell.column_ordinal = 2;
  text_cell.value.type_id = CanonicalTypeId::character;
  text_cell.value.payload = PayloadFor(CanonicalTypeId::character);
  row.cells.push_back(text_cell);

  RowDataPageBody body;
  body.page_number = 100;
  body.rows.push_back(row);

  const auto built = BuildRowDataPageBody(body, page_size);
  if (!built.ok()) {
    PrintDiagnostic(built.diagnostic);
    return false;
  }
  const auto parsed = ParseRowDataPageBody(built.serialized, body.page_number);
  if (!parsed.ok()) {
    PrintDiagnostic(parsed.diagnostic);
    return false;
  }
  if (parsed.body.rows.size() != 1 || parsed.body.rows[0].cells.size() != 2) {
    return Fail("SB-STORAGE-FOUNDATION-ROW-PAGE-ROUNDTRIP-MISMATCH", "row/cell count");
  }
  ++counters->row_page_round_trips;
  return true;
}

bool CheckCatalogRecords(ProbeCounters* counters, u64 creation_millis) {
  u64 seed = creation_millis + 2000;
  for (const CatalogRecordDescriptor& descriptor : BuiltinCatalogRecordDescriptors()) {
    CatalogTypedRecord record;
    record.header.kind = descriptor.kind;
    record.header.row_uuid = GenerateTyped(UuidKind::row, seed++);
    if (descriptor.requires_object_uuid) {
      record.header.object_uuid = GenerateTyped(UuidKind::object, seed++);
    }
    if (descriptor.requires_parent_uuid) {
      record.header.parent_uuid = GenerateTyped(UuidKind::object, seed++);
    }
    record.payload = "probe=1";
    const auto encoded = EncodeCatalogTypedRecord(record, counters->catalog_record_round_trips + 1);
    if (!encoded.ok()) {
      PrintDiagnostic(encoded.diagnostic);
      return false;
    }
    const auto decoded = DecodeCatalogTypedRecord(encoded.row);
    if (!decoded.ok()) {
      PrintDiagnostic(decoded.diagnostic);
      return false;
    }
    ++counters->catalog_record_round_trips;
  }
  return true;
}

bool CheckDatabasePersistence(const Args& args, ProbeCounters* counters) {
  const auto database_uuid = GenerateEngineIdentityV7(UuidKind::database, args.creation_millis + 3000);
  const auto filespace_uuid = GenerateEngineIdentityV7(UuidKind::filespace, args.creation_millis + 3001);
  if (!database_uuid.ok()) {
    PrintDiagnostic(database_uuid.diagnostic);
    return false;
  }
  if (!filespace_uuid.ok()) {
    PrintDiagnostic(filespace_uuid.diagnostic);
    return false;
  }

  DatabaseCreateConfig create;
  create.path = args.database_path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = args.page_size;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.resource_seed_pack_root = args.seed_pack_root;
  create.allow_overwrite = args.overwrite;

  const auto created = CreateDatabaseFile(create);
  if (!created.ok()) {
    PrintDiagnostic(created.diagnostic);
    return false;
  }

  DatabaseOpenConfig open;
  open.path = args.database_path;
  const auto opened = OpenDatabaseFile(open);
  if (!opened.ok()) {
    PrintDiagnostic(opened.diagnostic);
    return false;
  }
  if (!opened.state.resource_seed_catalog_present || !opened.state.typed_catalog_records_present) {
    return Fail("SB-STORAGE-FOUNDATION-DATABASE-CATALOG-MISSING", "resource/typed catalog state");
  }
  counters->database_typed_catalog_records = opened.state.typed_catalog_record_count;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    Usage();
    return 2;
  }

  ProbeCounters counters;
  if (!CheckDatatypes(&counters)) { return 1; }
  if (!CheckPageLayouts(&counters, args.page_size)) { return 1; }
  if (!CheckRowPage(&counters, args.page_size, args.creation_millis)) { return 1; }
  if (!CheckCatalogRecords(&counters, args.creation_millis)) { return 1; }
  if (!CheckDatabasePersistence(args, &counters)) { return 1; }

  std::cout << "{\n";
  std::cout << "  \"ok\": true,\n";
  std::cout << "  \"datatype_layouts\": " << counters.datatype_layouts << ",\n";
  std::cout << "  \"datatype_round_trips\": " << counters.datatype_round_trips << ",\n";
  std::cout << "  \"page_layouts\": " << counters.page_layouts << ",\n";
  std::cout << "  \"toast_plans\": " << counters.toast_plans << ",\n";
  std::cout << "  \"row_page_round_trips\": " << counters.row_page_round_trips << ",\n";
  std::cout << "  \"catalog_record_round_trips\": " << counters.catalog_record_round_trips << ",\n";
  std::cout << "  \"database_typed_catalog_records\": " << counters.database_typed_catalog_records << "\n";
  std::cout << "}\n";
  return 0;
}
