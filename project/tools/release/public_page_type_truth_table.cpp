// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_header.hpp"
#include "page_layout.hpp"
#include "page_registry.hpp"
#include "page_skeleton.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using scratchbird::core::platform::Uuid;
using scratchbird::storage::disk::ClassifyPageHeader;
using scratchbird::storage::disk::IsClusterOnlyPageType;
using scratchbird::storage::disk::PageClassificationKindName;
using scratchbird::storage::disk::PageHeader;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::disk::SerializePageHeader;
using scratchbird::storage::disk::kDeclaredPageTypes;
using scratchbird::storage::page::ClassifyPageBodyProductionAdmission;
using scratchbird::storage::page::ClassifyForPageManager;
using scratchbird::storage::page::ClassifyPageSkeleton;
using scratchbird::storage::page::LookupPageFamily;
using scratchbird::storage::page::LookupPageLayout;
using scratchbird::storage::page::LookupPageSkeleton;
using scratchbird::storage::page::PageBodyProductionAdmissionKindName;
using scratchbird::storage::page::PageBodyGrowthDirectionName;
using scratchbird::storage::page::PageFamilyName;
using scratchbird::storage::page::PageSkeletonKindName;
using scratchbird::storage::page::PageSkeletonStateName;

struct TruthRecord {
  PageType page_type = PageType::unknown;
  std::string page_type_value;
  std::string page_type_name;
  bool declared_enum = false;
  bool registry_mapped = false;
  bool layout_mapped = false;
  bool skeleton_mapped = false;
  std::string registry_family;
  std::string layout_family;
  std::string skeleton_family;
  std::string layout_growth;
  std::string skeleton_kind;
  std::string skeleton_state;
  bool body_parser_available = false;
  bool body_mutation_available = false;
  bool manager_may_read_body = false;
  bool manager_may_write_body = false;
  bool production_may_interpret_body = false;
  bool production_may_mutate_body = false;
  std::string production_admission;
  std::string provider_boundary;
  bool cluster_only = false;
  bool encrypted_or_opaque = false;
  bool test_only = false;
  bool reserved = false;
  std::string header_classification;
  std::string diagnostic_code;
};

Uuid MakeV7Uuid(std::uint8_t seed) {
  Uuid uuid;
  for (std::size_t index = 0; index < uuid.bytes.size(); ++index) {
    uuid.bytes[index] = static_cast<scratchbird::core::platform::byte>(seed + index + 1);
  }
  uuid.bytes[6] = static_cast<scratchbird::core::platform::byte>((uuid.bytes[6] & 0x0fu) | 0x70u);
  uuid.bytes[8] = static_cast<scratchbird::core::platform::byte>((uuid.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

std::string CsvEscape(const std::string& value) {
  if (value.find_first_of(",\"\n\r") == std::string::npos) {
    return value;
  }
  std::string escaped = "\"";
  for (char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped += ch;
    }
  }
  escaped += '"';
  return escaped;
}

std::string CsvLine(const std::vector<std::string>& fields) {
  std::ostringstream out;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) {
      out << ',';
    }
    out << CsvEscape(fields[index]);
  }
  out << '\n';
  return out.str();
}

PageHeader HeaderFor(PageType page_type, std::uint8_t seed) {
  PageHeader header;
  header.page_size = 16384;
  header.page_type = page_type;
  header.database_uuid = MakeV7Uuid(static_cast<std::uint8_t>(seed + 1));
  header.filespace_uuid = MakeV7Uuid(static_cast<std::uint8_t>(seed + 32));
  header.page_uuid = MakeV7Uuid(static_cast<std::uint8_t>(seed + 64));
  header.page_number = static_cast<std::uint64_t>(seed) + 1;
  header.page_generation = 1;
  return header;
}

std::string ProviderBoundary(bool cluster_only, bool encrypted_or_opaque, bool reserved) {
  if (cluster_only) {
    return "external_cluster_provider";
  }
  if (encrypted_or_opaque) {
    return "decryption_provider";
  }
  if (reserved) {
    return "reserved_no_provider";
  }
  return "local_engine";
}

TruthRecord BuildRecord(PageType page_type, std::size_t index) {
  TruthRecord record;
  record.page_type = page_type;
  record.page_type_value = std::to_string(static_cast<std::uint32_t>(page_type));
  record.page_type_name = PageTypeName(page_type);
  record.declared_enum = true;
  record.cluster_only = IsClusterOnlyPageType(page_type);

  const auto family = LookupPageFamily(page_type);
  record.registry_mapped = family.ok();
  record.registry_family = PageFamilyName(family.descriptor.family);
  record.cluster_only = record.cluster_only || family.descriptor.cluster_only;
  record.encrypted_or_opaque = family.descriptor.encrypted_or_opaque;
  record.reserved = family.descriptor.reserved;

  const auto layout = LookupPageLayout(page_type);
  record.layout_mapped = layout.ok();
  if (layout.ok()) {
    record.layout_family = PageFamilyName(layout.descriptor.family);
    record.layout_growth = PageBodyGrowthDirectionName(layout.descriptor.growth);
  }

  const auto skeleton = LookupPageSkeleton(page_type);
  record.skeleton_mapped = skeleton.ok();
  record.skeleton_family = PageFamilyName(skeleton.descriptor.family);
  record.skeleton_kind = PageSkeletonKindName(skeleton.descriptor.skeleton_kind);
  record.skeleton_state = PageSkeletonStateName(skeleton.descriptor.state);
  record.body_parser_available = skeleton.descriptor.body_parser_available;
  record.body_mutation_available = skeleton.descriptor.body_mutation_available;

  const auto serialized = SerializePageHeader(HeaderFor(page_type, static_cast<std::uint8_t>(index + 1)));
  if (!serialized.ok()) {
    record.header_classification = "serialization_failed";
    record.diagnostic_code = serialized.diagnostic.diagnostic_code;
    record.provider_boundary = ProviderBoundary(record.cluster_only, record.encrypted_or_opaque, record.reserved);
    record.production_admission = "body_refused";
    return record;
  }

  const auto header = ClassifyPageHeader(serialized.serialized);
  record.header_classification = PageClassificationKindName(header.kind);
  const auto manager = ClassifyForPageManager(header);
  record.manager_may_read_body = manager.may_read_body;
  record.manager_may_write_body = manager.may_write_body;

  const auto skeleton_classification = ClassifyPageSkeleton(header);
  const auto production_admission = ClassifyPageBodyProductionAdmission(header);
  record.production_may_interpret_body = production_admission.may_interpret_body;
  record.production_may_mutate_body = production_admission.may_mutate_body;
  record.production_admission = PageBodyProductionAdmissionKindName(production_admission.kind);
  if (!production_admission.diagnostic.diagnostic_code.empty()) {
    record.diagnostic_code = production_admission.diagnostic.diagnostic_code;
  } else if (!skeleton_classification.diagnostic.diagnostic_code.empty()) {
    record.diagnostic_code = skeleton_classification.diagnostic.diagnostic_code;
  } else if (!manager.diagnostic.diagnostic_code.empty()) {
    record.diagnostic_code = manager.diagnostic.diagnostic_code;
  } else if (!header.diagnostic.diagnostic_code.empty()) {
    record.diagnostic_code = header.diagnostic.diagnostic_code;
  }

  record.provider_boundary = ProviderBoundary(record.cluster_only, record.encrypted_or_opaque, record.reserved);
  return record;
}

std::vector<TruthRecord> BuildRecords() {
  std::vector<TruthRecord> records;
  records.reserve(kDeclaredPageTypes.size());
  for (std::size_t index = 0; index < kDeclaredPageTypes.size(); ++index) {
    records.push_back(BuildRecord(kDeclaredPageTypes[index], index));
  }
  return records;
}

std::string ToCsv(const std::vector<TruthRecord>& records) {
  std::ostringstream out;
  out << CsvLine({"page_type_value",
                  "page_type_name",
                  "declared_enum",
                  "registry_mapped",
                  "layout_mapped",
                  "skeleton_mapped",
                  "registry_family",
                  "layout_family",
                  "skeleton_family",
                  "layout_growth",
                  "skeleton_kind",
                  "skeleton_state",
                  "body_parser_available",
                  "body_mutation_available",
                  "manager_may_read_body",
                  "manager_may_write_body",
                  "production_may_interpret_body",
                  "production_may_mutate_body",
                  "production_admission",
                  "provider_boundary",
                  "cluster_only",
                  "encrypted_or_opaque",
                  "test_only",
                  "reserved",
                  "header_classification",
                  "diagnostic_code"});
  for (const TruthRecord& record : records) {
    out << CsvLine({record.page_type_value,
                    record.page_type_name,
                    BoolText(record.declared_enum),
                    BoolText(record.registry_mapped),
                    BoolText(record.layout_mapped),
                    BoolText(record.skeleton_mapped),
                    record.registry_family,
                    record.layout_family,
                    record.skeleton_family,
                    record.layout_growth,
                    record.skeleton_kind,
                    record.skeleton_state,
                    BoolText(record.body_parser_available),
                    BoolText(record.body_mutation_available),
                    BoolText(record.manager_may_read_body),
                    BoolText(record.manager_may_write_body),
                    BoolText(record.production_may_interpret_body),
                    BoolText(record.production_may_mutate_body),
                    record.production_admission,
                    record.provider_boundary,
                    BoolText(record.cluster_only),
                    BoolText(record.encrypted_or_opaque),
                    BoolText(record.test_only),
                    BoolText(record.reserved),
                    record.header_classification,
                    record.diagnostic_code});
  }
  return out.str();
}

const TruthRecord* FindRecord(const std::vector<TruthRecord>& records, PageType page_type) {
  const auto found = std::find_if(records.begin(), records.end(), [page_type](const TruthRecord& record) {
    return record.page_type == page_type;
  });
  return found == records.end() ? nullptr : &*found;
}

void RequireRecord(const std::vector<TruthRecord>& records,
                   PageType page_type,
                   const std::string& admission,
                   bool may_mutate,
                   std::vector<std::string>* errors) {
  const TruthRecord* record = FindRecord(records, page_type);
  if (record == nullptr) {
    errors->push_back(std::string("missing required page type ") + PageTypeName(page_type));
    return;
  }
  if (record->production_admission != admission) {
    errors->push_back(record->page_type_name + " admission was " + record->production_admission +
                      " expected " + admission);
  }
  if (record->production_may_mutate_body != may_mutate) {
    errors->push_back(record->page_type_name + " mutation state did not match expected production truth");
  }
}

std::vector<std::string> VerifyRecords(const std::vector<TruthRecord>& records) {
  std::vector<std::string> errors;
  std::set<std::uint32_t> values;
  std::set<std::string> names;

  if (records.size() != kDeclaredPageTypes.size()) {
    errors.push_back("generated record count does not match declared page type count");
  }

  for (const TruthRecord& record : records) {
    if (!values.insert(static_cast<std::uint32_t>(record.page_type)).second) {
      errors.push_back("duplicate page type value " + record.page_type_value);
    }
    if (!names.insert(record.page_type_name).second) {
      errors.push_back("duplicate page type name " + record.page_type_name);
    }
    if (record.page_type == PageType::unknown || record.page_type_name == "unknown") {
      errors.push_back("declared page type list contains unknown");
    }
    if (!record.registry_mapped) {
      errors.push_back(record.page_type_name + " has no page family registry mapping");
    }
    if (!record.layout_mapped) {
      errors.push_back(record.page_type_name + " has no page layout mapping");
    }
    if (record.cluster_only && record.provider_boundary != "external_cluster_provider") {
      errors.push_back(record.page_type_name + " is cluster-only without external provider boundary");
    }
    if (record.cluster_only && record.production_may_mutate_body) {
      errors.push_back(record.page_type_name + " is cluster-only but locally mutating");
    }
    if (record.encrypted_or_opaque && record.production_may_mutate_body) {
      errors.push_back(record.page_type_name + " is encrypted/opaque but locally mutating");
    }
    if (record.reserved && record.production_may_mutate_body) {
      errors.push_back(record.page_type_name + " is reserved but locally mutating");
    }
    if (record.production_may_mutate_body &&
        (!record.body_parser_available || !record.body_mutation_available)) {
      errors.push_back(record.page_type_name + " mutates without parser and mutator availability");
    }
    if (record.body_mutation_available && !record.body_parser_available) {
      errors.push_back(record.page_type_name + " mutator is available without parser availability");
    }
    if (record.test_only) {
      errors.push_back(record.page_type_name + " is marked test-only in first-release page truth");
    }
  }

  const auto unknown_family = LookupPageFamily(PageType::unknown);
  if (unknown_family.ok()) {
    errors.push_back("unknown page type unexpectedly has a family registry mapping");
  }
  const auto unknown_layout = LookupPageLayout(PageType::unknown);
  if (unknown_layout.ok()) {
    errors.push_back("unknown page type unexpectedly has a layout mapping");
  }
  const auto unknown_skeleton = LookupPageSkeleton(PageType::unknown);
  if (unknown_skeleton.ok()) {
    errors.push_back("unknown page type unexpectedly has a skeleton mapping");
  }

  RequireRecord(records, PageType::catalog, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::transaction_inventory, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::row_data, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::index_btree, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::allocation_map, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::index_hash, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::metrics, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::archive, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::columnar, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::vector, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::graph, "local_engine_mutating", true, &errors);
  RequireRecord(records, PageType::reserved_local, "reserved_nonmutating", false, &errors);
  RequireRecord(records, PageType::cluster_transaction, "external_cluster_provider_required", false, &errors);
  RequireRecord(records, PageType::encrypted_opaque, "decryption_required", false, &errors);

  return errors;
}

struct Args {
  bool verify = false;
  bool print = false;
  std::filesystem::path out_path;
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--verify") {
      args.verify = true;
    } else if (arg == "--print") {
      args.print = true;
    } else if (arg == "--out" && index + 1 < argc) {
      args.out_path = argv[++index];
    } else {
      throw std::runtime_error("usage: public_page_type_truth_table [--verify] [--print] [--out PATH]");
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  try {
    args = ParseArgs(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }

  const std::vector<TruthRecord> records = BuildRecords();
  const std::string csv = ToCsv(records);

  if (!args.out_path.empty()) {
    if (!args.out_path.parent_path().empty()) {
      std::filesystem::create_directories(args.out_path.parent_path());
    }
    std::ofstream out(args.out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::cerr << "failed to open output path: " << args.out_path << '\n';
      return 2;
    }
    out << csv;
  }

  if (args.print || args.out_path.empty()) {
    std::cout << csv;
  }

  if (args.verify) {
    const std::vector<std::string> errors = VerifyRecords(records);
    if (!errors.empty()) {
      std::cerr << "page-type truth verification failed:\n";
      for (const std::string& error : errors) {
        std::cerr << " - " << error << '\n';
      }
      return 1;
    }
  }

  return 0;
}
