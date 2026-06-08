// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "database_format.hpp"
#include "datatype_wire_metadata.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SB_PUBLIC_RELEASE_PROJECT_VERSION
#define SB_PUBLIC_RELEASE_PROJECT_VERSION "0.0.0"
#endif

namespace {

namespace catalog = scratchbird::core::catalog;
namespace datatype = scratchbird::core::datatypes;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::Uuid;
using platform::UuidKind;
using platform::u64;

struct Args {
  std::filesystem::path output_path;
  bool self_test = false;
};

struct RefusalRecord {
  std::string surface;
  std::string diagnostic_code;
};

struct GateState {
  std::vector<RefusalRecord> refusals;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  if (args == nullptr) {
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string key = argv[index];
    if (key == "--self-test") {
      args->self_test = true;
      continue;
    }
    if (key == "--out" && index + 1 < argc) {
      args->output_path = argv[++index];
      continue;
    }
    return false;
  }
  return args->self_test && !args->output_path.empty();
}

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "public_release_version_metadata=fail:" << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeIdentity(UuidKind kind, u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1834000000000ull + salt);
  if (!generated.ok()) {
    std::cerr << generated.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

std::string JsonEscape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped.push_back(ch); break;
    }
  }
  return escaped;
}

std::string Quoted(std::string_view value) {
  return "\"" + JsonEscape(value) + "\"";
}

bool HasRefusal(const GateState& state, std::string_view surface, std::string_view diagnostic_code) {
  for (const RefusalRecord& record : state.refusals) {
    if (record.surface == surface && record.diagnostic_code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

bool CheckEngineAbi(GateState*) {
  if (!Require(sb_engine_abi_version_packed() == SB_ENGINE_ABI_VERSION_PACKED,
               "engine ABI function must match public header macro")) {
    return false;
  }
  const char* build_id = nullptr;
  std::uint64_t build_id_size = 0;
  if (!Require(sb_engine_abi_build_id(&build_id, &build_id_size) == SB_ENGINE_STATUS_OK,
               "engine ABI build id call must succeed") ||
      !Require(build_id != nullptr && build_id_size > 0, "engine ABI build id must be non-empty")) {
    return false;
  }
  return true;
}

bool CheckDatabaseFormat(GateState* state) {
  const Uuid database_uuid = MakeIdentity(UuidKind::database, 1).value;
  const auto made = disk::MakeDatabaseHeader(
      database_uuid,
      static_cast<platform::u32>(disk::PageSizeProfile::profile_16k),
      1834000000000ull,
      0,
      disk::DatabaseCompatibilityFlag::public_node_safe_header_open);
  if (!Require(made.ok(), "current database format header must validate")) {
    return false;
  }
  const auto serialized = disk::SerializeDatabaseHeader(made.header);
  if (!Require(serialized.ok(), "current database format header must serialize")) {
    return false;
  }
  const auto parsed = disk::ParseDatabaseHeader(serialized.serialized);
  if (!Require(parsed.ok(), "current database format header must parse") ||
      !Require(parsed.header.format_major == disk::kScratchBirdDatabaseFormatMajor,
               "parsed database major format must match metadata") ||
      !Require(parsed.header.format_minor == disk::kScratchBirdDatabaseFormatMinor,
               "parsed database minor format must match metadata")) {
    return false;
  }

  auto future_minor = made.header;
  future_minor.format_minor = disk::kScratchBirdDatabaseFormatMinor + 1;
  const auto future_minor_result = disk::ValidateDatabaseHeader(future_minor);
  if (!Require(!future_minor_result.ok(), "future database minor format must fail closed")) {
    return false;
  }
  state->refusals.push_back({"database_format.future_minor", future_minor_result.diagnostic.diagnostic_code});

  auto old_major = made.header;
  old_major.format_major = 0;
  const auto old_major_result = disk::ValidateDatabaseHeader(old_major);
  if (!Require(!old_major_result.ok(), "old database major format must fail closed")) {
    return false;
  }
  state->refusals.push_back({"database_format.old_major", old_major_result.diagnostic.diagnostic_code});

  auto unknown_required = made.header;
  unknown_required.compatibility_flags = 1ull << 63;
  const auto unknown_required_result = disk::ValidateDatabaseHeader(unknown_required);
  if (!Require(!unknown_required_result.ok(), "unknown required database compatibility flag must fail closed")) {
    return false;
  }
  state->refusals.push_back({"database_format.unknown_required_flag",
                             unknown_required_result.diagnostic.diagnostic_code});
  return true;
}

catalog::CatalogTypedRecord MakeCatalogRecord() {
  catalog::CatalogTypedRecord record;
  record.header.kind = catalog::CatalogRecordKind::database;
  record.header.record_version = catalog::kCatalogRecordSchemaVersionCurrent;
  record.header.row_uuid = MakeIdentity(UuidKind::row, 10);
  record.header.object_uuid = MakeIdentity(UuidKind::object, 11);
  record.payload = "release_metadata=public";
  return record;
}

bool CheckCatalogRecordCodec(GateState* state, catalog::CatalogRecordCodecResult* encoded_out) {
  const auto descriptors = catalog::BuiltinCatalogRecordDescriptors();
  if (!Require(!descriptors.empty(), "catalog descriptor inventory must be non-empty")) {
    return false;
  }
  for (const catalog::CatalogRecordDescriptor& descriptor : descriptors) {
    const auto validation = catalog::ValidateCatalogRecordDescriptor(descriptor);
    if (!Require(validation.ok(), "builtin catalog descriptor must validate")) {
      return false;
    }
  }

  const catalog::CatalogTypedRecord record = MakeCatalogRecord();
  const auto encoded = catalog::EncodeCatalogTypedRecord(record, 1);
  if (!Require(encoded.ok(), "current catalog record codec must encode")) {
    return false;
  }
  const auto decoded = catalog::DecodeCatalogTypedRecord(encoded.row);
  if (!Require(decoded.ok(), "current catalog record codec must decode") ||
      !Require(decoded.record.header.record_version == catalog::kCatalogRecordSchemaVersionCurrent,
               "decoded catalog record version must match current metadata")) {
    return false;
  }

  catalog::CatalogTypedRecord future_record = record;
  future_record.header.record_version = catalog::kCatalogRecordSchemaVersionMaxSupported + 1;
  const auto future_result = catalog::EncodeCatalogTypedRecord(future_record, 2);
  if (!Require(!future_result.ok(), "future catalog record version must fail closed")) {
    return false;
  }
  state->refusals.push_back({"catalog_record_codec.future_record_version",
                             future_result.diagnostic.diagnostic_code});
  if (encoded_out != nullptr) {
    *encoded_out = encoded;
  }
  return true;
}

bool CheckCatalogPageBody(GateState* state, const catalog::CatalogRecordCodecResult& encoded) {
  const page::CatalogPageRow row{page::CatalogPageRowKind::typed_catalog_record, 1, encoded.row.payload};
  const auto built = page::BuildCatalogPageSet({row}, 8192, 1, 2);
  if (!Require(built.ok(), "current catalog page body must build") ||
      !Require(!built.pages.empty(), "catalog page set must contain a page")) {
    return false;
  }
  const auto parsed = page::ParseCatalogPageBody(built.pages.front().body, built.pages.front().page_number);
  if (!Require(parsed.ok(), "current catalog page body must parse") ||
      !Require(parsed.body.rows.size() == 1, "catalog page body must preserve one row")) {
    return false;
  }

  std::vector<platform::byte> future_minor = built.pages.front().body;
  future_minor[10] = static_cast<platform::byte>(page::kCatalogPageBodyFormatMinorMaxSupported + 1);
  future_minor[11] = 0;
  const auto future_result = page::ParseCatalogPageBody(future_minor, built.pages.front().page_number);
  if (!Require(!future_result.ok(), "future catalog page body minor version must fail closed")) {
    return false;
  }
  state->refusals.push_back({"catalog_page_body.future_minor", future_result.diagnostic.diagnostic_code});
  return true;
}

bool CheckDatatypeWireMetadata(GateState*) {
  return Require(datatype::kNativeWireMetadataLayoutVersion == 1,
                 "datatype wire metadata layout version must be current") &&
         Require(datatype::kCanonicalWireTypeIdBytes == 8,
                 "canonical wire type id byte count must match metadata") &&
         Require(datatype::kParameterDescriptionHeaderBytes > 0,
                 "parameter description header metadata must be non-zero") &&
         Require(datatype::kResultColumnDescriptorFixedBytes > 0,
                 "result column descriptor metadata must be non-zero");
}

std::string BuildMetadataJson(const GateState& state) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"project_version\": " << Quoted(SB_PUBLIC_RELEASE_PROJECT_VERSION) << ",\n";
  out << "  \"authority_policy\": {\n";
  out << "    \"engine_execution_envelope\": \"sblr_or_internal_only\",\n";
  out << "    \"durable_identity\": \"uuid_authority\",\n";
  out << "    \"transaction_authority\": \"mga_inventory_and_row_versions\",\n";
  out << "    \"release_metadata_authority\": \"evidence_only\"\n";
  out << "  },\n";
  out << "  \"engine_abi\": {\n";
  out << "    \"major\": " << SB_ENGINE_ABI_VERSION_MAJOR << ",\n";
  out << "    \"minor\": " << SB_ENGINE_ABI_VERSION_MINOR << ",\n";
  out << "    \"patch\": " << SB_ENGINE_ABI_VERSION_PATCH << ",\n";
  out << "    \"packed\": " << SB_ENGINE_ABI_VERSION_PACKED << "\n";
  out << "  },\n";
  out << "  \"database_format\": {\n";
  out << "    \"major\": " << disk::kScratchBirdDatabaseFormatMajor << ",\n";
  out << "    \"minor\": " << disk::kScratchBirdDatabaseFormatMinor << ",\n";
  out << "    \"header_bytes\": " << disk::kDatabaseHeaderSerializedBytes << ",\n";
  out << "    \"magic\": \"SBDBV001\"\n";
  out << "  },\n";
  out << "  \"catalog_page_body\": {\n";
  out << "    \"format_major\": " << page::kCatalogPageBodyFormatMajor << ",\n";
  out << "    \"format_minor\": " << page::kCatalogPageBodyFormatMinor << ",\n";
  out << "    \"format_major_min_supported\": " << page::kCatalogPageBodyFormatMajorMinSupported << ",\n";
  out << "    \"format_major_max_supported\": " << page::kCatalogPageBodyFormatMajorMaxSupported << ",\n";
  out << "    \"format_minor_max_supported\": " << page::kCatalogPageBodyFormatMinorMaxSupported << "\n";
  out << "  },\n";
  out << "  \"catalog_record_codec\": {\n";
  out << "    \"schema_version_current\": " << catalog::kCatalogRecordSchemaVersionCurrent << ",\n";
  out << "    \"schema_version_min_supported\": " << catalog::kCatalogRecordSchemaVersionMinSupported << ",\n";
  out << "    \"schema_version_max_supported\": " << catalog::kCatalogRecordSchemaVersionMaxSupported << ",\n";
  out << "    \"builtin_descriptor_count\": " << catalog::BuiltinCatalogRecordDescriptors().size() << "\n";
  out << "  },\n";
  out << "  \"datatype_wire_metadata\": {\n";
  out << "    \"layout_version\": " << datatype::kNativeWireMetadataLayoutVersion << ",\n";
  out << "    \"canonical_wire_type_id_bytes\": " << datatype::kCanonicalWireTypeIdBytes << ",\n";
  out << "    \"canonical_type_ref_bytes\": " << datatype::kCanonicalTypeRefBytes << ",\n";
  out << "    \"parameter_description_header_bytes\": " << datatype::kParameterDescriptionHeaderBytes << ",\n";
  out << "    \"result_column_descriptor_fixed_bytes\": " << datatype::kResultColumnDescriptorFixedBytes << "\n";
  out << "  },\n";
  out << "  \"compatibility_refusals\": [\n";
  for (std::size_t index = 0; index < state.refusals.size(); ++index) {
    const RefusalRecord& record = state.refusals[index];
    out << "    {\"surface\": " << Quoted(record.surface)
        << ", \"diagnostic_code\": " << Quoted(record.diagnostic_code) << "}";
    out << (index + 1 == state.refusals.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

bool WriteFile(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << text;
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: public_release_version_metadata --self-test --out PATH\n";
    return EXIT_FAILURE;
  }

  GateState state;
  catalog::CatalogRecordCodecResult encoded_catalog_record;
  if (!CheckEngineAbi(&state) ||
      !CheckDatabaseFormat(&state) ||
      !CheckCatalogRecordCodec(&state, &encoded_catalog_record) ||
      !CheckCatalogPageBody(&state, encoded_catalog_record) ||
      !CheckDatatypeWireMetadata(&state)) {
    return EXIT_FAILURE;
  }
  if (!Require(HasRefusal(state, "database_format.future_minor", "FORMAT.VERSION_UNSUPPORTED"),
               "database future minor refusal diagnostic must be present") ||
      !Require(HasRefusal(state, "database_format.old_major", "FORMAT.VERSION_TOO_OLD"),
               "database old major refusal diagnostic must be present") ||
      !Require(HasRefusal(state,
                          "database_format.unknown_required_flag",
                          "FORMAT.UNKNOWN_REQUIRED_FLAG"),
               "database required flag refusal diagnostic must be present") ||
      !Require(HasRefusal(state,
                          "catalog_record_codec.future_record_version",
                          "SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED"),
               "catalog record future version refusal diagnostic must be present") ||
      !Require(HasRefusal(state,
                          "catalog_page_body.future_minor",
                          "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED"),
               "catalog page future minor refusal diagnostic must be present")) {
    return EXIT_FAILURE;
  }

  const std::string metadata = BuildMetadataJson(state);
  if (!Require(WriteFile(args.output_path, metadata), "metadata output write must succeed")) {
    return EXIT_FAILURE;
  }
  std::cout << "public_release_version_metadata=passed\n";
  return EXIT_SUCCESS;
}
