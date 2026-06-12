// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_temporal_wire.hpp"
#include "resource_seed_pack.hpp"

#include <filesystem>
#include <iostream>

using namespace scratchbird::core::datatypes;
using namespace scratchbird::core::resources;

namespace {

bool Expect(bool condition, const char* name) {
  std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
  return condition;
}

TimezoneSeedAuthority SeedAuthorityFrom(const ResourceSeedCatalogImage& image) {
  TimezoneSeedAuthority seed;
  seed.active = image.active;
  seed.seed_pack_name = image.seed_pack_name;
  seed.seed_pack_version = image.seed_pack_version;
  seed.content_hash = image.content_hash;
  seed.timezone_records = image.timezone_records;
  seed.timezone_transition_records = image.timezone_transition_records;
  seed.timezone_leap_second_records = image.timezone_leap_second_records;
  for (const auto& alias : image.aliases) {
    if (alias.family == ResourceSeedFamily::timezone_tables) {
      seed.timezone_names.push_back(alias.alias);
      if (alias.canonical_name != alias.alias) { seed.timezone_names.push_back(alias.canonical_name); }
    }
  }
  return seed;
}

ReferenceTemporalWireProfileRequest Request(const TimezoneSeedAuthority& seed,
                                        const char* reference,
                                        const char* reference_type,
                                        const char* profile,
                                        const char* value) {
  ReferenceTemporalWireProfileRequest request;
  request.reference_engine = reference;
  request.reference_type_or_family = reference_type;
  request.wire_profile = profile;
  request.encoded_value = value;
  request.timezone_seed = seed;
  request.fractional_second_precision = 12;
  request.require_timezone_seed = true;
  return request;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path seed_pack_root = "project/resources/seed-packs/initial-resource-pack";
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--seed-pack-root" && i + 1 < argc) {
      seed_pack_root = argv[++i];
    } else {
      std::cerr << "usage: sb_reference_timezone_wire_profile_probe [--seed-pack-root PATH]\n";
      return 2;
    }
  }

  ResourceSeedLoadConfig config;
  config.seed_pack_root = seed_pack_root.string();
  config.allow_minimal_bootstrap = false;
  config.require_timezone = true;
  const auto loaded = LoadResourceSeedPack(config);
  const auto seed = loaded.ok() ? SeedAuthorityFrom(loaded.image) : TimezoneSeedAuthority{};

  const auto common_timestamp = ValidateReferenceTemporalWireProfile(
      Request(seed, "common_sql", "TIMESTAMP", "timestamp_wire", "2026-05-01 12:34:56.123456789012"));
  const auto common_timestamp_tz = ValidateReferenceTemporalWireProfile(
      Request(seed, "common_sql", "TIMESTAMP WITH TIME ZONE", "timestamp_with_time_zone",
              "2026-05-01T12:34:56.123456789012Z"));
  const auto mysql_datetime = ValidateReferenceTemporalWireProfile(
      Request(seed, "mysql", "DATETIME", "datetime_wire_profile", "2026-05-01 12:34:56"));
  const auto mysql_timestamp = ValidateReferenceTemporalWireProfile(
      Request(seed, "mysql", "TIMESTAMP", "timestamp_timezone_profile", "2026-05-01T12:34:56-05:00"));
  const auto mssql_datetimeoffset = ValidateReferenceTemporalWireProfile(
      Request(seed, "mssql", "DATETIMEOFFSET", "tds_datetimeoffset_wire", "2026-05-01T12:34:56-05:00"));
  const auto named_zone = ValidateReferenceTemporalWireProfile(
      Request(seed, "postgresql", "TIMESTAMPTZ", "timestamptz_wire", "2026-05-01T12:34:56[America/Toronto]"));
  const auto bad_missing_zone = ValidateReferenceTemporalWireProfile(
      Request(seed, "postgresql", "TIMESTAMPTZ", "timestamptz_wire", "2026-05-01T12:34:56"));
  const auto bad_zone_on_datetime = ValidateReferenceTemporalWireProfile(
      Request(seed, "mysql", "DATETIME", "datetime_wire_profile", "2026-05-01T12:34:56Z"));
  const auto bad_unknown_zone = ValidateReferenceTemporalWireProfile(
      Request(seed, "postgresql", "TIMESTAMPTZ", "timestamptz_wire", "2026-05-01T12:34:56[Not/AZone]"));
  const auto bad_precision = ValidateReferenceTemporalWireProfile(
      Request(seed, "common_sql", "TIMESTAMP", "timestamp_wire", "2026-05-01 12:34:56.1234567890123"));
  const auto bad_seed = ValidateReferenceTemporalWireProfile(
      Request(TimezoneSeedAuthority{}, "postgresql", "TIMESTAMPTZ", "timestamptz_wire",
              "2026-05-01T12:34:56[America/Toronto]"));

  const bool ok = loaded.ok() && seed.active && seed.timezone_records > 0 &&
                  common_timestamp.ok() && common_timestamp.canonical_type_id == CanonicalTypeId::timestamp &&
                  common_timestamp_tz.ok() && common_timestamp_tz.timezone_identifier == "Z" &&
                  mysql_datetime.ok() && mysql_timestamp.ok() &&
                  mysql_timestamp.timezone_offset_minutes == -300 &&
                  mssql_datetimeoffset.ok() && named_zone.ok() && named_zone.used_timezone_seed &&
                  !bad_missing_zone.ok() && !bad_zone_on_datetime.ok() && !bad_unknown_zone.ok() &&
                  !bad_precision.ok() && !bad_seed.ok();

  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(loaded.ok(), "seed_pack_loaded");
  Expect(seed.active && seed.timezone_records > 0, "timezone_seed_authority_present");
  Expect(common_timestamp.ok(), "common_timestamp_wire");
  Expect(common_timestamp_tz.ok() && common_timestamp_tz.timezone_identifier == "Z", "common_timestamp_tz_wire");
  Expect(mysql_datetime.ok(), "mysql_datetime_wire");
  Expect(mysql_timestamp.ok() && mysql_timestamp.timezone_offset_minutes == -300, "mysql_timestamp_offset_wire");
  Expect(mssql_datetimeoffset.ok(), "mssql_datetimeoffset_wire");
  Expect(named_zone.ok() && named_zone.used_timezone_seed, "named_zone_seed_validated");
  Expect(!bad_missing_zone.ok(), "missing_zone_rejected");
  Expect(!bad_zone_on_datetime.ok(), "forbidden_zone_rejected");
  Expect(!bad_unknown_zone.ok(), "unknown_zone_rejected");
  Expect(!bad_precision.ok(), "excess_precision_rejected");
  std::cout << "  \"missing_seed_rejected\": " << (!bad_seed.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
