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
    for required in ("connect_timeout", "socket_timeout", "binary_transfer", "protocol", "compression"):
        if required not in default_names:
            return fail(f"driver default property missing {required}")
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

    performance_dashboard = root.find(".//dashboard[@id='scratchbird.performance']/query")
    if performance_dashboard is None or (performance_dashboard.text or "").strip() != "SHOW METRICS":
        return fail("scratchbird.performance dashboard must use SHOW METRICS")

    plugin_text = PLUGIN_XML.read_text(encoding="utf-8")
    if "sys.performance" in plugin_text:
        return fail("plugin.xml still references removed sys.performance surface")
    return 0


def main() -> int:
    for check in (require_no_stale_paths, require_plugin_surface):
        result = check()
        if result != 0:
            return result
    print("DBeaver adapter current-project design gate ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
