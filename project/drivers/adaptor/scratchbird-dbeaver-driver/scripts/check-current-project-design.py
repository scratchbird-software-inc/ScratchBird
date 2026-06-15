#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static conformance checks for the current-repo DBeaver adapter design."""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLUGIN_XML = ROOT / "plugins" / "org.jkiss.dbeaver.ext.scratchbird" / "plugin.xml"
README = ROOT / "README.md"
BUILD_P2 = ROOT / "scripts" / "build-p2-update-site.sh"
BUILD_BUNDLE = ROOT / "scripts" / "build-stock-test-bundle.sh"
INSTALL_SOURCE = ROOT / "scripts" / "install-into-dbeaver.sh"
SCHEMA_NODE_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "ScratchBirdSchemaNode.java"
)
DATA_SOURCE_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "ScratchBirdDataSource.java"
)
CATALOG_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "ScratchBirdCatalog.java"
)
SCHEMA_NODE_MANAGER_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "edit"
    / "ScratchBirdSchemaNodeManager.java"
)
TABLE_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "ScratchBirdTable.java"
)
VIEW_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "ScratchBirdView.java"
)
QUALIFIED_NAMES_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "ScratchBirdQualifiedNames.java"
)
MANAGER_SUPPORT_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "edit"
    / "ScratchBirdManagerSupport.java"
)
TABLE_MANAGER_SOURCE = (
    ROOT
    / "plugins"
    / "org.jkiss.dbeaver.ext.scratchbird"
    / "src"
    / "org"
    / "jkiss"
    / "dbeaver"
    / "ext"
    / "scratchbird"
    / "model"
    / "edit"
    / "ScratchBirdTableManager.java"
)

REQUIRED_DRIVER_PROPERTIES = {
    "connect_timeout",
    "socket_timeout",
    "binary_transfer",
    "application_name",
    "schema",
    "currentSchema",
    "search_path",
    "role",
    "sslmode",
    "sslrootcert",
    "sslcert",
    "sslkey",
    "sslpassword",
    "fetch_size",
    "connect_client_flags",
    "protocol",
    "compression",
    "pooling",
    "min_pool_size",
    "max_pool_size",
    "connection_lifetime",
    "acquire_timeout",
    "rewrite_batched_inserts",
    "loggerLevel",
    "loggerFile",
    "front_door_mode",
    "manager_auth_token",
    "manager_username",
    "manager_database",
    "manager_connection_profile",
    "manager_client_intent",
    "manager_client_flags",
    "manager_auth_fast_path",
    "auth_token",
    "auth_method_id",
    "auth_method_payload",
    "auth_payload_json",
    "auth_payload_b64",
    "auth_provider_profile",
    "auth_required_methods",
    "auth_forbidden_methods",
    "auth_require_channel_binding",
    "workload_identity_token",
    "proxy_principal_assertion",
    "dormant_id",
    "dormant_reattach_token",
    "metadataExpandSchemaParents",
    "metadata_fixture_catalog",
}

REQUIRED_PROVIDER_PROPERTIES = REQUIRED_DRIVER_PROPERTIES - {
    "application_name",
    "sslmode",
    "sslrootcert",
    "sslcert",
    "sslkey",
    "sslpassword",
}


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def require_no_stale_paths() -> int:
    stale_checks = {
        README: ("lanes/active/", "ScratchBird-driver/lanes/active/"),
        BUILD_P2: ("../../drivers/jdbc",),
        BUILD_BUNDLE: ("${INTEGRATION_DIR}/dist",),
        INSTALL_SOURCE: ("../../drivers/jdbc",),
    }
    hits: list[str] = []
    for path, needles in stale_checks.items():
        text = path.read_text(encoding="utf-8")
        for needle in needles:
            if needle in text:
                hits.append(f"{path.relative_to(ROOT)} still contains {needle!r}")
    if hits:
        return fail("\n".join(hits))

    for path in (BUILD_P2, BUILD_BUNDLE, INSTALL_SOURCE):
        text = path.read_text(encoding="utf-8")
        if "SCRATCHBIRD_DRIVER_BUILD_ROOT" not in text:
            return fail(f"{path.relative_to(ROOT)} does not route generated output through build/drivers")
        if "PROJECT_DRIVERS_DIR}/driver/jdbc" in text or path == BUILD_BUNDLE:
            continue
        return fail(f"{path.relative_to(ROOT)} does not resolve the current project/drivers/driver/jdbc path")
    return 0


def require_plugin_surface() -> int:
    tree = ET.parse(PLUGIN_XML)
    root = tree.getroot()

    driver = root.find(".//driver[@id='scratchbird_jdbc']")
    if driver is None:
        return fail("scratchbird_jdbc driver declaration missing")

    sample_url = driver.get("sampleURL", "")
    if "{host}" not in sample_url or "{database}" not in sample_url:
        return fail("sample JDBC URL must expose host and database placeholders")
    if "binary_transfer=true" not in sample_url:
        return fail("sample JDBC URL must use canonical binary_transfer key")
    if "metadata_fixture_catalog" in sample_url:
        return fail("sample JDBC URL must use live metadata by default")

    driver_properties = set()
    for param in driver.findall("parameter"):
        if param.get("name") == "driver-properties":
            driver_properties = {
                item.strip()
                for item in param.get("value", "").split(",")
                if item.strip()
            }
            break
    missing = sorted(REQUIRED_DRIVER_PROPERTIES - driver_properties)
    if missing:
        return fail("driver-properties missing canonical keys: " + ", ".join(missing))

    default_names = {prop.get("name") for prop in driver.findall("property")}
    for required in ("connect_timeout", "socket_timeout", "binary_transfer", "protocol", "compression", "metadata_fixture_catalog"):
        if required not in default_names:
            return fail(f"driver default property missing {required}")
    if driver.find("property[@name='metadata_fixture_catalog']").get("value") != "":
        return fail("driver metadata_fixture_catalog default must be empty for live metadata")
    for stale in ("connectTimeout", "socketTimeout", "binaryTransfer"):
        if stale in default_names:
            return fail(f"driver default property still uses stale alias {stale}")

    provider_properties = {
        prop.get("id")
        for prop in root.findall(".//provider-properties//property")
        if prop.get("id")
    }
    missing_provider = sorted(REQUIRED_PROVIDER_PROPERTIES - provider_properties)
    if missing_provider:
        return fail("provider-properties missing adapter fields: " + ", ".join(missing_provider))

    if root.find(".//extension[@point='org.jkiss.dbeaver.dashboard']") is not None:
        return fail("live dashboard extension must not be registered until metrics/catalog dashboard queries are implemented")
    blocked_dashboard_tokens = (
        "scratchbird.sessions",
        "scratchbird.transactions",
        "scratchbird.locks",
        "scratchbird.performance",
        'SELECT COUNT(*) AS "Sessions" FROM sys.sessions',
        'SELECT COUNT(*) AS "Transactions" FROM sys.transactions',
        'SELECT COUNT(*) AS "Locks" FROM sys.locks',
        "<query>SHOW METRICS</query>",
    )
    plugin_text = PLUGIN_XML.read_text(encoding="utf-8")
    for token in blocked_dashboard_tokens:
        if token in plugin_text:
            return fail(f"plugin.xml still registers unsupported dashboard query {token!r}")

    if "sys.performance" in plugin_text:
        return fail("plugin.xml still references removed sys.performance surface")

    required_tree_tokens = (
        '<treeInjection path="generic/catalog">\n                <items label="#schema" path="schema" property="schemaTree"',
        '<items label="#schema" path="schema" property="childSchemas"',
        'recursive=".."',
        '<items label="%tree.table.node.name" path="table" property="physicalTables"',
        '<items label="%tree.tview.node.name" path="view" property="views"',
    )
    for token in required_tree_tokens:
        if token not in plugin_text:
            return fail(f"plugin.xml missing DBeaver navigator container token {token!r}")
    blocked_tree_tokens = (
        'label="%tree.schemas.node.name"',
        'visibleIf="object.schemaBranchesFolderVisible"',
        'recursive="../.."',
        '<folder type="org.jkiss.dbeaver.ext.generic.model.GenericTable"',
        '<folder type="org.jkiss.dbeaver.ext.generic.model.GenericView"',
    )
    for token in blocked_tree_tokens:
        if token in plugin_text:
            return fail(f"plugin.xml still groups physical navigator children with {token!r}")
    return 0


def require_schema_node_metadata_fallback() -> int:
    source = SCHEMA_NODE_SOURCE.read_text(encoding="utf-8")
    required_tokens = (
        "return super.getTables(monitor);",
        "return super.getTable(monitor, name);",
        "getTableCache().setCache(tables);",
        "navigatorPhysicalTables",
        "navigatorViews",
        "addNavigatorPhysicalTable",
        "addNavigatorView",
        "hasNavigatorRelations()",
        "getTableCache().setCache(navigatorTables);",
        "if (hasNavigatorRelations()) {\n            getTables(monitor);",
        "session.getMetaData().getTables(null, getAuthorityPath(), \"%\", PHYSICAL_TABLE_TYPES)",
        "session.getMetaData().getTables(null, getAuthorityPath(), \"%\", VIEW_TYPES)",
        "new ScratchBirdTable(this, tableName",
        "new ScratchBirdView(this, viewName",
        "new ScratchBirdTable(this, tableName, tableType, null, objectPath)",
        "new ScratchBirdView(this, viewName, viewType, null, objectPath)",
        "loadMetadataPhysicalTables(monitor)",
        "loadMetadataViews(monitor)",
        "public boolean isSchemaBranchesFolderVisible()",
        "ScratchBird schema constraints are not available for navigator",
        "ScratchBird schema indexes are not available for navigator",
    )
    for token in required_tokens:
        if token not in source:
            return fail(f"schema node metadata fallback missing {token!r}")
    return 0


def require_collapsed_catalog_schema_tree() -> int:
    source = DATA_SOURCE_SOURCE.read_text(encoding="utf-8")
    required_tokens = (
        "public synchronized Collection<ScratchBirdSchemaNode> getSchemaTree",
        "getOrCreateSyntheticRootCatalog()",
        "syntheticRootCatalog = null;",
    )
    for token in required_tokens:
        if token not in source:
            return fail(f"data-source schema tree fallback missing {token!r}")
    return 0


def require_readable_catalog_tree_query() -> int:
    source = CATALOG_SOURCE.read_text(encoding="utf-8")
    required_tokens = (
        "FROM sys.catalog_readable.navigator_tree",
        "executeNavigatorTreeQuery",
        "databaseDisplayName",
        "navigatorAuthorityPath",
        "navigatorPathFromNodePath",
        "nodeRole != null && nodeRole.startsWith(\"database.\")",
        "return nodePath;",
        "ScratchBirdCatalogObjectReference.catalogObject",
        "isNavigatorPhysicalTable",
        "isNavigatorView",
        "node.addNavigatorPhysicalTable",
        "node.addNavigatorView",
        "child.getObjectPath()",
        "ScratchBird readable navigator tree is required for first-release navigation",
        "schemaTree = Collections.emptyList();",
    )
    for token in required_tokens:
        if token not in source:
            return fail(f"catalog readable schema tree query missing {token!r}")
    blocked_tokens = (
        "ORDER BY depth, sort_group, sort_ordinal",
        "FROM sys.catalog_readable.object_tree",
        "FROM sys.catalog.object_resolver",
    )
    for token in blocked_tokens:
        if token in source:
            return fail(f"catalog readable schema tree query still has physical-tree fallback token {token!r}")
    return 0


def require_schema_node_manager_nested_create_policy() -> int:
    source = SCHEMA_NODE_MANAGER_SOURCE.read_text(encoding="utf-8")
    required_tokens = (
        "container instanceof ScratchBirdSchemaNode schemaNode",
        "return false;",
        "container instanceof ScratchBirdCatalog scratchBirdCatalog",
    )
    for token in required_tokens:
        if token not in source:
            return fail(f"schema-node manager nested create policy missing {token!r}")
    return 0


def require_system_metadata_visible_in_navigator() -> int:
    for path in (TABLE_SOURCE, VIEW_SOURCE):
        source = path.read_text(encoding="utf-8")
        if "public boolean isSystem()" not in source or "return false;" not in source:
            return fail(f"{path.relative_to(ROOT)} must keep ScratchBird sys metadata visible in DBeaver")
    return 0


def require_authority_path_action_context() -> int:
    qualified_names_source = QUALIFIED_NAMES_SOURCE.read_text(encoding="utf-8")
    for token in (
        "static String qualifyAuthorityPath",
        "static String joinPath",
        "static String parentPath",
    ):
        if token not in qualified_names_source:
            return fail(f"qualified-name authority helper missing {token!r}")

    for path in (TABLE_SOURCE, VIEW_SOURCE):
        source = path.read_text(encoding="utf-8")
        for token in (
            "ScratchBirdObjectPath objectPath",
            "qualifyAuthorityPath(objectPath.authorityPath())",
            "public String getAuthorityPath()",
            "public String getAuthoritySchemaPath()",
            "objectPath.identityStatus()",
        ):
            if token not in source:
                return fail(f"{path.relative_to(ROOT)} missing authority context token {token!r}")

    support_source = MANAGER_SUPPORT_SOURCE.read_text(encoding="utf-8")
    for token in (
        "container instanceof ScratchBirdSchemaNode schemaNode",
        "return schemaNode.getAuthorityPath();",
        "object instanceof ScratchBirdTable scratchBirdTable",
        "object instanceof ScratchBirdView scratchBirdView",
    ):
        if token not in support_source:
            return fail(f"manager support missing authority context token {token!r}")

    table_manager_source = TABLE_MANAGER_SOURCE.read_text(encoding="utf-8")
    for token in (
        "return ScratchBirdManagerSupport.canCreateRegularSqlObject(container);",
        "GenericConstants.TABLE_TYPE_TABLE",
        "createTableOrViewImpl",
    ):
        if token not in table_manager_source:
            return fail(f"table manager missing ScratchBird create-context token {token!r}")
    return 0


def main() -> int:
    for check in (
        require_no_stale_paths,
        require_plugin_surface,
        require_schema_node_metadata_fallback,
        require_collapsed_catalog_schema_tree,
        require_readable_catalog_tree_query,
        require_schema_node_manager_nested_create_policy,
        require_system_metadata_visible_in_navigator,
        require_authority_path_action_context,
    ):
        result = check()
        if result != 0:
            return result
    print("DBeaver adapter current-project design gate ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
