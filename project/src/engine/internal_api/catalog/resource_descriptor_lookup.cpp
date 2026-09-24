// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/name_resolution_api.hpp"
#include "catalog/resource_catalog_admission.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include "../../../core/datatypes/canonical_utf8.hpp"
#include <algorithm>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {
std::string LowerResourceClass(std::string value) {
  for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return value;
}
bool IsEngineResourceClass(const std::string& family) {
  return family == "charset" || family == "collation";
}
}
EngineResourceCatalogAdmission OpenEngineResourceCatalog(const EngineRequestContext& context) {
  EngineResourceCatalogAdmission result;
  const auto fail = [&](const char* code, const char* key, const char* detail) {
    result.diagnostic = MakeEngineApiDiagnostic(code, key, detail);
    return std::move(result);
  };
  if (context.database_path.empty() || context.database_uuid.is_nil())
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.database_required",
                "database_path_and_binary_identity_required");
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(context.database_uuid))
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.database_identity_mismatch",
                "database_identity_not_system_uuidv7");
  if (context.local_transaction_id == 0 || context.transaction_uuid.is_nil())
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.transaction_required",
                "exact_active_transaction_identity_required");
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(context.transaction_uuid))
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.transaction_invalid",
                "transaction_uuid_malformed");
  if (context.resource_epoch == 0)
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.epoch_required",
                "exact_nonzero_resource_epoch_required");

  scratchbird::storage::database::DatabaseOpenConfig config;
  config.path = context.database_path;
  config.read_only = true;
  config.suppress_background_agents = true;
  auto opened = scratchbird::storage::database::OpenDatabaseFile(config);
  if (!opened.ok()) {
    result.diagnostic = MakeEngineApiDiagnosticFromNative(opened.diagnostic,
        "CATALOG.INVALID_INPUT", "catalog.resource.catalog_unavailable", {});
    return result;
  }
  const auto& state = opened.state;
  using scratchbird::core::platform::UuidKind;
  if (state.database_uuid.kind != UuidKind::database ||
      state.database_uuid.value != context.database_uuid)
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.database_identity_mismatch",
                "database_uuid_does_not_match_catalog_authority");
  if (!state.resource_seed_catalog_present || !state.resource_seed_catalog.active ||
      state.resource_seed_catalog.minimal_bootstrap)
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.catalog_required",
                "durable_resource_seed_catalog_required");
  const scratchbird::transaction::mga::TransactionInventoryEntry* matched = nullptr;
  if (state.local_transaction_inventory_present) {
    for (const auto& entry : state.local_transaction_inventory.entries) {
      if (entry.identity.local_id.value != context.local_transaction_id) continue;
      if (matched)
        return fail("CATALOG.INVALID_INPUT", "catalog.resource.transaction_not_active",
                    "ambiguous_local_transaction_identity");
      matched = &entry;
    }
  }
  using scratchbird::transaction::mga::TransactionState;
  if (!matched || matched->identity.transaction_uuid.kind != UuidKind::transaction ||
      matched->identity.transaction_uuid.value != context.transaction_uuid ||
      (matched->state != TransactionState::active && matched->state != TransactionState::read_only_active))
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.transaction_not_active",
                "exact_active_transaction_identity_required");
  if (state.resource_seed_catalog.resource_epoch != context.resource_epoch)
    return fail("CATALOG.INVALID_INPUT", "catalog.resource.epoch_stale",
                "requested_resource_epoch_does_not_match_admitted_cohort");

  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.state = std::move(opened.state);
  return result;
}

namespace {
// Both public catalog entry points project the same already-admitted image.
// Do not reopen between alias resolution and descriptor publication: identity,
// epochs and compiled comparison tables must belong to one resource cohort.
EngineResourceDescriptorLookupResult ProjectResourceDescriptor(
    const EngineRequestContext& context,
    const scratchbird::core::resources::ResourceSeedCatalogImage& image,
    const EngineUuid& resource_uuid,
    const std::string& resource_family) {
  EngineResourceDescriptorLookupResult result;
  auto fail = [&](std::string code,
                  std::string message_key,
                  std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(std::move(code),
                                                std::move(message_key),
                                                std::move(detail));
    return result;
  };

  EngineResolvedResourceDescriptor descriptor;
  descriptor.present = true;
  descriptor.database_uuid = context.database_uuid;
  descriptor.resource_family = resource_family;
  descriptor.seed_pack_name = image.seed_pack_name;
  descriptor.seed_pack_version = image.seed_pack_version;
  descriptor.resource_epoch = image.resource_epoch;
  if (resource_family == "charset") {
    const scratchbird::core::resources::ResourceSeedCharsetDescriptor*
        matched = nullptr;
    for (const auto& charset : image.charsets) {
      if (charset.resource_uuid == resource_uuid) {
        matched = &charset;
        break;
      }
    }
    if (matched == nullptr) {
      for (const auto& collation : image.collations) {
        if (collation.resource_uuid == resource_uuid) {
          return fail("CATALOG.INVALID_INPUT",
                      "catalog.resource.family_mismatch",
                      "expected=charset;actual=collation");
        }
      }
      return fail("CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE",
                  "catalog.resource.uuid_not_found",
                  "requested resource identity");
    }
    descriptor.canonical_name = matched->canonical_name;
    descriptor.resource_uuid = matched->resource_uuid;
    descriptor.default_collation_uuid =
        matched->default_collation_uuid;
    descriptor.default_collation_name = matched->default_collation_name;
    descriptor.family_epoch = matched->family_epoch;
    descriptor.family_version = matched->family_version;
    descriptor.min_bytes = matched->min_bytes;
    descriptor.max_bytes = matched->max_bytes;
    descriptor.variable_width = matched->variable_width;
  } else {
    const scratchbird::core::resources::ResourceSeedCollationDescriptor*
        matched = nullptr;
    for (const auto& collation : image.collations) {
      if (collation.resource_uuid == resource_uuid) {
        matched = &collation;
        break;
      }
    }
    if (matched == nullptr) {
      for (const auto& charset : image.charsets) {
        if (charset.resource_uuid == resource_uuid) {
          return fail("CATALOG.INVALID_INPUT",
                      "catalog.resource.family_mismatch",
                      "expected=collation;actual=charset");
        }
      }
      return fail("CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE",
                  "catalog.resource.uuid_not_found",
                  "requested resource identity");
    }
    descriptor.canonical_name = matched->canonical_name;
    descriptor.resource_uuid = matched->resource_uuid;
    descriptor.parent_resource_uuid = matched->charset_uuid;
    descriptor.parent_canonical_name = matched->charset_name;
    descriptor.family_epoch = matched->family_epoch;
    descriptor.family_version = matched->family_version;
    descriptor.default_for_parent = matched->default_for_charset;
    descriptor.case_insensitive = matched->case_insensitive;
    descriptor.accent_insensitive = matched->accent_insensitive;
    descriptor.comparison_profile = matched->comparison_profile;
    if (scratchbird::core::resources::UsesUnicodeRoot(matched->comparison_profile))
      descriptor.unicode_collation = image.unicode_collation;
  }

  if (descriptor.resource_uuid.is_nil() ||
      descriptor.resource_epoch == 0 || descriptor.family_epoch == 0 ||
      descriptor.family_version.empty() ||
      (resource_family == "charset" &&
       (descriptor.min_bytes == 0 ||
        descriptor.max_bytes < descriptor.min_bytes)) ||
      (resource_family == "collation" &&
       descriptor.parent_resource_uuid.is_nil())) {
    return fail("CATALOG.INVALID_INPUT",
                "catalog.resource.descriptor_invalid",
                descriptor.canonical_name);
  }

  result.ok = true;
  result.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.resource_descriptor = std::move(descriptor);
  return result;
}
} // namespace

EngineResourceDescriptorLookupResult LookupEngineResourceDescriptorByUuid(
    const EngineRequestContext& context,
    const EngineUuid& resource_uuid,
    const std::string& expected_resource_family) {
  EngineResourceDescriptorLookupResult result;
  const auto family = LowerResourceClass(expected_resource_family);
  if (!IsEngineResourceClass(family)) {
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.INVALID_INPUT",
        "catalog.resource.family_invalid", expected_resource_family);
    return result;
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(resource_uuid)) {
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.INVALID_INPUT",
        resource_uuid.is_nil() ? "catalog.resource.uuid_required" : "catalog.resource.uuid_invalid",
        family + (resource_uuid.is_nil() ? "_uuid_required" : "_uuid_malformed"));
    return result;
  }
  auto admission = OpenEngineResourceCatalog(context);
  if (!admission.ok()) { result.diagnostic = std::move(admission.diagnostic); return result; }
  return ProjectResourceDescriptor(context, admission.state->resource_seed_catalog,
                                   resource_uuid, family);
}

EngineResourceDescriptorLookupResult LookupEngineResourceDescriptorByName(
    const EngineRequestContext& context,
    const std::string& name,
    const std::string& expected_resource_family) {
  EngineResourceDescriptorLookupResult result;
  const auto family = LowerResourceClass(expected_resource_family);
  if (!IsEngineResourceClass(family)) {
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.INVALID_INPUT",
        "catalog.resource.family_invalid", expected_resource_family);
    return result;
  }
  if (name.empty() || name.find('\0') != std::string::npos ||
      !scratchbird::core::datatypes::ValidateCanonicalUtf8(
          reinterpret_cast<const std::uint8_t*>(name.data()), name.size())) {
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.INVALID_INPUT",
        "catalog.resource.name_invalid", "resource_name_must_be_nonempty_canonical_utf8");
    return result;
  }
  auto admission = OpenEngineResourceCatalog(context);
  if (!admission.ok()) { result.diagnostic = std::move(admission.diagnostic); return result; }
  const auto& image = admission.state->resource_seed_catalog;
  EngineUuid identity;
  if (family == "charset") {
    if (const auto* row = scratchbird::core::resources::FindResourceSeedCharset(image, name))
      identity = row->resource_uuid;
  } else {
    if (const auto* row = scratchbird::core::resources::FindResourceSeedCollation(image, name))
      identity = row->resource_uuid;
  }
  if (identity.is_nil()) {
    const auto alias = scratchbird::core::resources::ResolveResourceSeedAlias(image,
        family == "charset" ? scratchbird::core::resources::ResourceSeedFamily::charset
                            : scratchbird::core::resources::ResourceSeedFamily::collation, name);
    if (!alias.ok() && alias.diagnostic.diagnostic_code == "SB_RESOURCE_ALIAS_AMBIGUOUS") {
      result.diagnostic = MakeEngineApiDiagnostic(alias.diagnostic.diagnostic_code,
                                                  alias.diagnostic.message_key, {});
      for (const auto& field : alias.diagnostic.arguments)
        result.diagnostic.fields.push_back({field.key, field.value});
    } else {
      result.diagnostic = MakeEngineApiDiagnostic("CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE",
          "message_vector.item_not_found_or_does_not_exist", family + "_not_found_or_not_visible");
    }
    return result;
  }
  return ProjectResourceDescriptor(context, image, identity, family);
}

EngineTimezoneSeedAuthorityLookupResult LookupEngineTimezoneSeedAuthority(
    const EngineRequestContext& context) {
  EngineTimezoneSeedAuthorityLookupResult result;
  auto fail = [&](std::string code,
                  std::string message_key,
                  std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(std::move(code),
                                                std::move(message_key),
                                                std::move(detail));
    return result;
  };
  auto admission = OpenEngineResourceCatalog(context);
  if (!admission.ok()) { result.diagnostic = std::move(admission.diagnostic); return result; }
  const auto& image = admission.state->resource_seed_catalog;

  EngineTimezoneSeedAuthorityDescriptor authority;
  authority.active = image.active;
  authority.seed_pack_name = image.seed_pack_name;
  authority.seed_pack_version = image.seed_pack_version;
  authority.content_hash = image.timezone_content_hash;
  authority.resource_epoch = image.resource_epoch;
  authority.timezone_epoch = image.timezone_epoch;
  authority.timezone_records = image.timezone_records;
  authority.timezone_transition_records =
      image.timezone_transition_records;
  authority.timezone_leap_second_records =
      image.timezone_leap_second_records;
  for (const auto& alias : image.aliases) {
    if (alias.family !=
        scratchbird::core::resources::ResourceSeedFamily::timezone_tables) {
      continue;
    }
    authority.timezone_names.push_back(alias.alias);
    authority.timezone_names.push_back(alias.canonical_name);
  }
  std::sort(authority.timezone_names.begin(), authority.timezone_names.end());
  authority.timezone_names.erase(
      std::unique(authority.timezone_names.begin(),
                  authority.timezone_names.end()),
      authority.timezone_names.end());
  if (!authority.active || authority.seed_pack_name.empty() ||
      authority.seed_pack_version.empty() || authority.content_hash.empty() ||
      authority.resource_epoch == 0 || authority.timezone_epoch == 0 ||
      authority.timezone_records == 0 || authority.timezone_names.empty()) {
    return fail("CATALOG.INVALID_INPUT",
                "catalog.resource.timezone_authority_invalid",
                "durable_timezone_seed_authority_incomplete");
  }
  result.ok = true;
  result.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.authority = std::move(authority);
  return result;
}


}  // namespace scratchbird::engine::internal_api
