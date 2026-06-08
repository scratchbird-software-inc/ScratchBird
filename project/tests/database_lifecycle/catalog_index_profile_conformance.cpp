// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_index_profile.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace catalog = scratchbird::core::catalog;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

void RequireValidationOk(const catalog::CatalogIndexValidationResult& result,
                         std::string_view message) {
  if (!result.ok) {
    for (const auto& issue : result.issues) {
      std::cerr << issue.code << " " << issue.detail << '\n';
    }
  }
  Require(result.ok, message);
}

bool HasIndex(std::string_view index_name) {
  return catalog::FindCatalogIndexProfile(index_name) != nullptr;
}

bool HasTable(std::string_view table_path) {
  return catalog::FindCatalogTableProfile(table_path) != nullptr;
}

void TestBuiltinProfilesValidate() {
  RequireValidationOk(catalog::ValidateBuiltinCatalogIndexProfiles(),
                      "built-in catalog index profiles failed validation");
}

void TestUuidExactLookupUsesHashEquality() {
  bool saw_object_uuid_hash = false;
  bool saw_row_uuid_hash = false;

  for (const auto& profile : catalog::BuiltinCatalogIndexProfiles()) {
    if (!profile.supports_uuid_exact_lookup) { continue; }

    Require(profile.method == catalog::CatalogIndexMethod::hash_equality,
            "UUID exact lookup did not use hash equality profile");
    Require(profile.key_columns.size() == 1,
            "UUID exact lookup was not a single equality key");
    Require(profile.key_columns.front().equality_component,
            "UUID exact lookup key was not equality-capable");
    Require(!profile.key_columns.front().ordered_component,
            "UUID exact lookup key incorrectly requested ordering");
    Require(!profile.key_columns.front().prefix_component,
            "UUID exact lookup key incorrectly requested prefix access");
    Require(!profile.supports_ordered_scan && !profile.supports_group_scan &&
                !profile.supports_prefix_scan &&
                !profile.supports_catalog_generation_visibility &&
                !profile.supports_transaction_history,
            "UUID exact hash profile carried ordered traversal flags");

    if (profile.index_name == "sys_catalog_object_identity_uuid_hash") {
      saw_object_uuid_hash = true;
      Require(profile.unique, "object UUID hash profile must be unique");
      Require(profile.key_columns.front().role == catalog::CatalogColumnRole::object_uuid,
              "object UUID hash profile did not key object_uuid");
    }
    if (profile.index_name == "sys_catalog_object_identity_row_hash") {
      saw_row_uuid_hash = true;
      Require(profile.unique, "row UUID hash profile must be unique");
      Require(profile.key_columns.front().role == catalog::CatalogColumnRole::row_uuid,
              "row UUID hash profile did not key row_uuid");
    }
  }

  Require(saw_object_uuid_hash, "missing sys.catalog object UUID hash profile");
  Require(saw_row_uuid_hash, "missing sys.catalog row UUID hash profile");
}

void TestBtreeOnlyForOrderedGroupPrefixGenerationOrHistory() {
  for (const auto& profile : catalog::BuiltinCatalogIndexProfiles()) {
    if (profile.method != catalog::CatalogIndexMethod::btree_ordered) { continue; }

    Require(catalog::CatalogIndexProfileHasOrderedNeed(profile),
            "B-tree profile exists without ordered/group/prefix/generation/history access need");
    Require(!profile.supports_uuid_exact_lookup,
            "UUID exact lookup used B-tree instead of hash equality");

    bool saw_ordered_key = false;
    bool saw_prefix_key = false;
    for (const auto& key : profile.key_columns) {
      saw_ordered_key = saw_ordered_key || key.ordered_component;
      saw_prefix_key = saw_prefix_key || key.prefix_component;
    }
    Require(saw_ordered_key || saw_prefix_key || profile.supports_group_scan,
            "B-tree profile lacked ordered, prefix, or grouped key shape");
  }
}

void TestGenerationTransactionAndHistoryProfiles() {
  const auto* generation =
      catalog::FindCatalogIndexProfile("sys_catalog_object_identity_generation_btree");
  Require(generation != nullptr, "missing catalog generation index profile");
  Require(generation->supports_catalog_generation_visibility,
          "catalog generation index does not advertise generation visibility");
  Require(generation->supports_ordered_scan,
          "catalog generation index does not advertise ordered scan");

  const auto* object_history =
      catalog::FindCatalogIndexProfile("sys_catalog_object_versions_history_btree");
  Require(object_history != nullptr, "missing object history index profile");
  Require(object_history->supports_transaction_history,
          "object history index does not advertise history traversal");
  Require(object_history->supports_catalog_generation_visibility,
          "object history index does not advertise generation visibility");

  const auto* tx_history =
      catalog::FindCatalogIndexProfile("sys_catalog_object_versions_tx_history_btree");
  Require(tx_history != nullptr, "missing transaction history index profile");
  Require(tx_history->supports_transaction_history,
          "transaction history index does not advertise transaction traversal");
}

void TestIdentityResolverBoundary() {
  Require(HasTable("sys.catalog.object_identity"), "missing base object identity table profile");
  Require(HasTable("sys.catalog.object_name_entries"), "missing resolver entry table profile");

  bool saw_authoritative_resolver = false;
  bool saw_full_path_accelerator = false;
  for (const auto& profile : catalog::BuiltinCatalogIndexProfiles()) {
    if (!profile.supports_name_resolution) { continue; }

    const auto* table = catalog::FindCatalogTableProfile(profile.table_path);
    Require(table != nullptr, "name resolver index refers to unknown table");
    Require(table->surface == catalog::CatalogTableSurface::identity_resolver,
            "name resolver index escaped identity resolver table surface");
    Require(profile.authority_boundary == "identity_resolver" ||
                profile.authority_boundary == "identity_resolver_accelerator",
            "name resolver index lacked resolver authority boundary");
    if (profile.index_name == "sys_catalog_name_entries_authoritative_lookup_btree") {
      saw_authoritative_resolver = true;
      Require(profile.authoritative, "authoritative resolver lookup was not authoritative");
    }
    if (profile.index_name == "sys_catalog_name_entries_full_path_accelerator_btree") {
      saw_full_path_accelerator = true;
      Require(!profile.authoritative, "full-path resolver accelerator became authority");
    }
  }

  Require(saw_authoritative_resolver, "missing authoritative resolver lookup profile");
  Require(saw_full_path_accelerator, "missing resolver full-path accelerator profile");
}

void TestNoHumanNameDuplicationInBaseCatalog() {
  bool saw_human_name_resolver_columns = false;
  for (const auto& table : catalog::BuiltinCatalogTableProfiles()) {
    const bool has_human_name_text = catalog::CatalogTableProfileContainsHumanNameText(table);
    if (table.surface == catalog::CatalogTableSurface::base_catalog) {
      Require(!has_human_name_text,
              "base sys.catalog table duplicated human-facing SQL object names");
      Require(!table.parser_visible,
              "base sys.catalog table was exposed as parser-visible metadata");
    }
    if (table.surface == catalog::CatalogTableSurface::identity_resolver &&
        has_human_name_text) {
      saw_human_name_resolver_columns = true;
    }
  }
  Require(saw_human_name_resolver_columns,
          "identity resolver did not own human-facing name text columns");
}

void TestClusterPathsFailClosed() {
  const auto cluster_path = catalog::ValidateCatalogPathForLocalCatalog(
      "cluster.sys.catalog.object_identity");
  Require(!cluster_path.ok, "cluster catalog path did not fail closed");
  Require(!cluster_path.issues.empty(), "cluster catalog failure lacked diagnostic");
  Require(cluster_path.issues.front().code == "CATALOG.INDEX.CLUSTER_SCOPE_FORBIDDEN",
          "cluster catalog failure used wrong diagnostic");

  for (const auto& table : catalog::BuiltinCatalogTableProfiles()) {
    Require(table.cluster_path_fail_closed,
            "catalog table profile did not fail closed for cluster paths");
  }
  for (const auto& profile : catalog::BuiltinCatalogIndexProfiles()) {
    Require(profile.cluster_path_fail_closed,
            "catalog index profile did not fail closed for cluster paths");
  }
}

}  // namespace

int main() {
  TestBuiltinProfilesValidate();
  TestUuidExactLookupUsesHashEquality();
  TestBtreeOnlyForOrderedGroupPrefixGenerationOrHistory();
  TestGenerationTransactionAndHistoryProfiles();
  TestIdentityResolverBoundary();
  TestNoHumanNameDuplicationInBaseCatalog();
  TestClusterPathsFailClosed();
  return EXIT_SUCCESS;
}
