#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Unit tests for the DBeaver management release gate."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[3]
GATE = REPO_ROOT / "project" / "tools" / "release" / "dbeaver_management_platform_gate.py"


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")


class DBeaverManagementPlatformGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.adapter = self.repo / "project/drivers/adaptor/scratchbird-dbeaver-driver"
        self.contract = self.repo / "contract.json"
        self.fixture = self.repo / "fixture.json"
        self.output = self.root / "report.json"
        self._write_minimal_adapter()
        self._write_contract()
        self._write_fixture()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_gate(self, *extra_args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(GATE),
                "--repo-root",
                str(self.repo),
                "--contract",
                str(self.contract),
                "--fixture",
                str(self.fixture),
                "--output",
                str(self.output),
                *extra_args,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def report(self) -> dict[str, object]:
        return json.loads(self.output.read_text(encoding="utf-8"))

    def _write_minimal_adapter(self) -> None:
        write(
            self.adapter / "pom.xml",
            """
            <project xmlns="http://maven.apache.org/POM/4.0.0"
                     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
                     xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd">
              <modelVersion>4.0.0</modelVersion>
              <version>1.0.1-SNAPSHOT</version>
              <properties>
                <tycho.version>4.0.8</tycho.version>
                <eclipse.version>2025-12</eclipse.version>
                <repo.p2.eclipse.url>https://download.eclipse.org/releases/${eclipse.version}/</repo.p2.eclipse.url>
                <repo.p2.dbeaver-update.url>https://dbeaver.io/update/latest/</repo.p2.dbeaver-update.url>
              </properties>
              <repositories>
                <repository><url>${repo.p2.eclipse.url}</url></repository>
                <repository><url>${repo.p2.dbeaver-update.url}</url></repository>
              </repositories>
            </project>
            """,
        )
        write(
            self.adapter / "features/org.jkiss.dbeaver.ext.scratchbird.feature/feature.xml",
            """
            <feature id="org.jkiss.dbeaver.ext.scratchbird.feature" version="1.0.1.qualifier">
              <description url="https://scratchbird.com/">ScratchBird</description>
              <license>Licensed under the Apache License, Version 2.0.</license>
              <plugin id="org.jkiss.dbeaver.ext.scratchbird" version="0.0.0"/>
              <plugin id="org.jkiss.dbeaver.ext.scratchbird.ui" version="0.0.0"/>
            </feature>
            """,
        )
        for rel in (
            "features/org.jkiss.dbeaver.ext.scratchbird.feature/pom.xml",
            "plugins/org.jkiss.dbeaver.ext.scratchbird/pom.xml",
            "plugins/org.jkiss.dbeaver.ext.scratchbird.ui/pom.xml",
            "repository/pom.xml",
        ):
            write(
                self.adapter / rel,
                """
                <project xmlns="http://maven.apache.org/POM/4.0.0"
                         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
                         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd">
                </project>
                """,
            )
        write(
            self.adapter / "repository/category.xml",
            """
            <site>
              <feature id="org.jkiss.dbeaver.ext.scratchbird.feature" version="0.0.0"/>
            </site>
            """,
        )
        write(
            self.adapter / "plugins/org.jkiss.dbeaver.ext.scratchbird/META-INF/MANIFEST.MF",
            """
            Manifest-Version: 1.0
            Bundle-SymbolicName: org.jkiss.dbeaver.ext.scratchbird;singleton:=true
            Bundle-Version: 1.0.1.qualifier
            Require-Bundle: org.jkiss.dbeaver.model,
             org.jkiss.dbeaver.model.jdbc,
             org.jkiss.dbeaver.model.sql,
             org.jkiss.dbeaver.ext.generic
            Bundle-RequiredExecutionEnvironment: JavaSE-21
            Export-Package: org.jkiss.dbeaver.ext.scratchbird.model,
             org.jkiss.dbeaver.ext.scratchbird.parser.v3
            """,
        )
        write(
            self.adapter / "plugins/org.jkiss.dbeaver.ext.scratchbird.ui/META-INF/MANIFEST.MF",
            """
            Manifest-Version: 1.0
            Bundle-SymbolicName: org.jkiss.dbeaver.ext.scratchbird.ui;singleton:=true
            Require-Bundle: org.eclipse.core.expressions,
             org.eclipse.jface,
             org.eclipse.jface.text,
             org.eclipse.swt,
             org.eclipse.ui,
             org.jkiss.dbeaver.ext.generic,
             org.jkiss.dbeaver.ext.scratchbird,
             org.jkiss.dbeaver.model,
             org.jkiss.dbeaver.ui,
             org.jkiss.dbeaver.ui.editors.sql,
             org.jkiss.dbeaver.ui.navigator
            Bundle-RequiredExecutionEnvironment: JavaSE-21
            """,
        )
        write(
            self.adapter / "plugins/org.jkiss.dbeaver.ext.scratchbird/plugin.xml",
            """
            <plugin>
              <extension point="org.jkiss.dbeaver.objectManager"/>
              <extension point="org.jkiss.dbeaver.dataTypeProvider"/>
              <extension point="org.jkiss.dbeaver.generic.meta"/>
              <extension point="org.jkiss.dbeaver.dataSourceProvider">
                <datasource id="scratchbird" parent="generic">
                  <drivers>
                    <driver id="scratchbird_jdbc" class="com.scratchbird.jdbc.SBDriver" defaultPort="3092" webURL="https://scratchbird.com/">
                      <parameter name="driver-properties" value="manager_auth_token,auth_token,auth_method_payload,auth_payload_json,auth_payload_b64,workload_identity_token,proxy_principal_assertion,dormant_reattach_token"/>
                    </driver>
                    <provider-properties drivers="scratchbird_jdbc">
                      <propertyGroup label="ScratchBird Connection">
                        <property id="manager_auth_token" type="string" features="secured,password"/>
                        <property id="auth_token" type="string" features="secured,password"/>
                        <property id="auth_method_payload" type="string" features="secured,password"/>
                        <property id="auth_payload_json" type="string" features="secured,password"/>
                        <property id="auth_payload_b64" type="string" features="secured,password"/>
                        <property id="workload_identity_token" type="string" features="secured,password"/>
                        <property id="proxy_principal_assertion" type="string" features="secured,password"/>
                        <property id="dormant_reattach_token" type="string" features="secured,password"/>
                      </propertyGroup>
                    </provider-properties>
                  </drivers>
                </datasource>
              </extension>
              <extension point="org.jkiss.dbeaver.dashboard"/>
            </plugin>
            """,
        )
        write(
            self.adapter / "plugins/org.jkiss.dbeaver.ext.scratchbird.ui/plugin.xml",
            """
            <plugin>
              <extension point="org.eclipse.core.expressions.propertyTesters"/>
              <extension point="org.eclipse.ui.editors.annotationTypes"/>
              <extension point="org.eclipse.ui.editors.markerAnnotationContract"/>
              <extension point="org.jkiss.dbeaver.sql.editorAddIns"/>
              <extension point="org.jkiss.dbeaver.sql.quickFixProcessors"/>
              <extension point="org.eclipse.ui.commands">
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.open"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.new"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.alter"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.delete"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.tasks"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.reports"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.sourceStatus"/>
                <command id="org.jkiss.dbeaver.ext.scratchbird.ui.validateSql"/>
              </extension>
              <extension point="org.eclipse.ui.handlers"/>
              <extension point="org.eclipse.ui.menus"/>
            </plugin>
            """,
        )
        write(
            self.adapter
            / "plugins/org.jkiss.dbeaver.ext.scratchbird/src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdGateFixture.java",
            """
            // Copyright (c) 2026 ScratchBird Software Inc.
            //
            // SPDX-License-Identifier: MPL-2.0
            /*
             * DBeaver - Universal Database Manager
             * Copyright (C) 2010-2026 DBeaver Corp and others
             * Licensed under the Apache License, Version 2.0
             */
            package org.jkiss.dbeaver.ext.scratchbird.model;
            public final class ScratchBirdGateFixture {}
            """,
        )
        write(
            self.adapter
            / "plugins/org.jkiss.dbeaver.ext.scratchbird/src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdNetworkPolicy.java",
            """
            // Copyright (c) 2026 ScratchBird Software Inc.
            //
            // SPDX-License-Identifier: MPL-2.0
            /*
             * DBeaver - Universal Database Manager
             * Copyright (C) 2010-2026 DBeaver Corp and others
             * Licensed under the Apache License, Version 2.0
             */
            package org.jkiss.dbeaver.ext.scratchbird.model;
            public record ScratchBirdNetworkPolicy(
                boolean telemetryEnabledByDefault,
                boolean updateChecksEnabledByDefault,
                boolean diagnosticsUploadEnabledByDefault
            ) {
                public static final String POLICY = "plugin-telemetry diagnostic-upload marketplace-call implicit-update-check ScratchBirdSecurityRedactor.redactPropertyValue";
            }
            """,
        )
        for script, text in {
            "build-p2-update-site.sh": "scratchbird-jdbc.jar repository/target/repository scratchbird-dbeaver-update-site-",
            "build-stock-test-bundle.sh": "install-into-stock-dbeaver.sh uninstall-from-stock-dbeaver.sh install-into-stock-dbeaver.bat uninstall-from-stock-dbeaver.bat SHA256SUMS.txt THIRD-PARTY-NOTICES.txt scratchbird-jdbc.jar",
            "install-into-dbeaver.sh": "plugins/org.jkiss.dbeaver.ext.scratchbird plugins/org.jkiss.dbeaver.ext.scratchbird.ui scratchbird-jdbc.jar",
            "install-into-stock-dbeaver.sh": "org.eclipse.equinox.p2.director -repository \"jar:file:${ZIP_PATH}!/\" -installIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group -uninstallIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group -purgeHistory -listInstalledRoots ScratchBird install completed successfully Installed root confirmed",
            "uninstall-from-stock-dbeaver.sh": "org.eclipse.equinox.p2.director -uninstallIU \"${FEATURE_IU}\" -purgeHistory -listInstalledRoots",
        }.items():
            write(
                self.adapter / "scripts" / script,
                "# SPDX-License-Identifier: MPL-2.0\n" + text + "\n",
            )

    def _write_contract(self) -> None:
        self.contract.write_text(
            json.dumps(
                {
                    "schema_id": "scratchbird.dbeaver_management.release_contract.v1",
                    "expected_build": {
                        "adapter_version_prefix": "1.0.1",
                        "java_execution_environment": "JavaSE-21",
                        "eclipse_release": "2025-12",
                        "tycho_version": "4.0.8",
                    },
                    "supported_version_matrix": [
                        {
                            "target_id": "linux-static-supported",
                            "dbeaver_ce_version": "26.0.2",
                            "java_execution_environment": "JavaSE-21",
                            "eclipse_release": "2025-12",
                            "tycho_version": "4.0.8",
                            "os": "linux",
                            "arch": "x86_64",
                            "support_status": "supported_static_contract",
                            "expected_outcome": "static supported contract",
                            "proof_anchor": "project/drivers/adaptor/scratchbird-dbeaver-driver/README.md",
                        },
                        {
                            "target_id": "old-dbeaver-refused",
                            "dbeaver_ce_version": "<26.0.2",
                            "java_execution_environment": "JavaSE-21",
                            "eclipse_release": "2025-12",
                            "tycho_version": "4.0.8",
                            "os": "all",
                            "arch": "all",
                            "support_status": "unsupported_refusal",
                            "expected_outcome": "refusal",
                            "refusal_reason": "unsupported_dbeaver_version",
                            "proof_anchor": "fixture#old",
                        },
                        {
                            "target_id": "java17-refused",
                            "dbeaver_ce_version": "26.0.2",
                            "java_execution_environment": "JavaSE-21",
                            "eclipse_release": "2025-12",
                            "tycho_version": "4.0.8",
                            "os": "all",
                            "arch": "all",
                            "support_status": "unsupported_refusal",
                            "expected_outcome": "refusal",
                            "refusal_reason": "unsupported_java_version",
                            "proof_anchor": "fixture#java",
                        },
                    ],
                    "api_drift": {
                        "feature_id": "org.jkiss.dbeaver.ext.scratchbird.feature",
                        "plugin_ids": [
                            "org.jkiss.dbeaver.ext.scratchbird",
                            "org.jkiss.dbeaver.ext.scratchbird.ui",
                        ],
                        "driver_class": "com.scratchbird.jdbc.SBDriver",
                        "default_port": "3092",
                        "core_extension_points": [
                            "org.jkiss.dbeaver.objectManager",
                            "org.jkiss.dbeaver.dataTypeProvider",
                            "org.jkiss.dbeaver.generic.meta",
                            "org.jkiss.dbeaver.dataSourceProvider",
                            "org.jkiss.dbeaver.dashboard",
                        ],
                        "ui_extension_points": [
                            "org.eclipse.core.expressions.propertyTesters",
                            "org.eclipse.ui.editors.annotationTypes",
                            "org.eclipse.ui.editors.markerAnnotationContract",
                            "org.jkiss.dbeaver.sql.editorAddIns",
                            "org.jkiss.dbeaver.sql.quickFixProcessors",
                            "org.eclipse.ui.commands",
                            "org.eclipse.ui.handlers",
                            "org.eclipse.ui.menus",
                        ],
                        "core_require_bundles": [
                            "org.jkiss.dbeaver.model",
                            "org.jkiss.dbeaver.model.jdbc",
                            "org.jkiss.dbeaver.model.sql",
                            "org.jkiss.dbeaver.ext.generic",
                        ],
                        "ui_require_bundles": [
                            "org.eclipse.core.expressions",
                            "org.eclipse.jface",
                            "org.eclipse.jface.text",
                            "org.eclipse.swt",
                            "org.eclipse.ui",
                            "org.jkiss.dbeaver.ext.generic",
                            "org.jkiss.dbeaver.ext.scratchbird",
                            "org.jkiss.dbeaver.model",
                            "org.jkiss.dbeaver.ui",
                            "org.jkiss.dbeaver.ui.editors.sql",
                            "org.jkiss.dbeaver.ui.navigator",
                        ],
                        "core_export_packages": [
                            "org.jkiss.dbeaver.ext.scratchbird.model",
                            "org.jkiss.dbeaver.ext.scratchbird.parser.v3",
                        ],
                        "ui_command_ids": [
                            "org.jkiss.dbeaver.ext.scratchbird.ui.open",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.new",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.alter",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.delete",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.tasks",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.reports",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.sourceStatus",
                            "org.jkiss.dbeaver.ext.scratchbird.ui.validateSql",
                        ],
                    },
                    "license_ip": {
                        "third_party_notice_components": [
                            {"component": "DBeaver", "license": "Apache-2.0", "boundary": "extension API"},
                            {"component": "Eclipse Platform", "license": "EPL-2.0", "boundary": "target platform"},
                            {"component": "Tycho", "license": "EPL-2.0", "boundary": "build-time packaging"},
                            {"component": "SWT", "license": "EPL-2.0", "boundary": "UI dependency"},
                            {"component": "JFace", "license": "EPL-2.0", "boundary": "UI dependency"},
                            {"component": "ScratchBird JDBC", "license": "MPL-2.0", "boundary": "bundled driver"},
                        ]
                    },
                    "secure_properties": {
                        "sensitive_provider_properties": [
                            "manager_auth_token",
                            "auth_token",
                            "auth_method_payload",
                            "auth_payload_json",
                            "auth_payload_b64",
                            "workload_identity_token",
                            "proxy_principal_assertion",
                            "dormant_reattach_token",
                        ],
                    },
                    "network_egress_policy": {
                        "allowed_runtime_urls": ["https://scratchbird.com/"],
                        "allowed_build_time_urls": [
                            "http://maven.apache.org/POM/4.0.0",
                            "http://maven.apache.org/xsd/maven-4.0.0.xsd",
                            "http://www.w3.org/2001/XMLSchema-instance",
                            "https://download.eclipse.org/releases/${eclipse.version}/",
                            "https://dbeaver.io/update/latest/",
                        ],
                        "runtime_egress_policy": (
                            "No telemetry, marketplace, update, or diagnostic upload endpoints are declared."
                        ),
                    },
                },
                indent=2,
            ),
            encoding="utf-8",
        )

    def _write_fixture(self) -> None:
        lifecycle = []
        for phase, script in [
            ("p2_update_site_build", "build-p2-update-site.sh"),
            ("stock_bundle_build", "build-stock-test-bundle.sh"),
            ("source_checkout_install", "install-into-dbeaver.sh"),
            ("stock_install", "install-into-stock-dbeaver.sh"),
            ("upgrade_replace_existing", "install-into-stock-dbeaver.sh"),
            ("stock_uninstall", "uninstall-from-stock-dbeaver.sh"),
            ("stock_reinstall", "install-into-stock-dbeaver.sh"),
        ]:
            lifecycle.append(
                {
                    "case_id": phase,
                    "phase": phase,
                    "script_path": f"project/drivers/adaptor/scratchbird-dbeaver-driver/scripts/{script}",
                    "required_script_tokens": self._tokens_for_phase(phase),
                    "expected_outcome": "checked",
                    "proof_policy": "script_contract",
                }
            )
        for phase in [
            "downgrade_refusal",
            "workspace_profile_cleanup",
            "driver_cache_cleanup",
            "secret_cleanup",
        ]:
            lifecycle.append(
                {
                    "case_id": phase,
                    "phase": phase,
                    "requires_external_proof": True,
                    "expected_outcome": "checked",
                    "proof_policy": "external_log",
                }
            )
        self.fixture.write_text(
            json.dumps(
                {
                    "schema_id": "scratchbird.dbeaver_management.install_lifecycle_fixture.v1",
                    "lifecycle_cases": lifecycle,
                    "migration_fixtures": [
                        {
                            "fixture_id": case,
                            "case_type": case,
                            "input_shape": "shape",
                            "expected_outcome": "outcome",
                            "redaction_policy": "no_plaintext_secret_output",
                        }
                        for case in [
                            "old_plugin_id",
                            "old_preference_key",
                            "backup_restore",
                            "corrupted_preferences",
                            "reinstall_recovery",
                            "redaction_safe_cleanup",
                        ]
                    ],
                },
                indent=2,
            ),
            encoding="utf-8",
        )

    @staticmethod
    def _tokens_for_phase(phase: str) -> list[str]:
        if phase in {"stock_install", "stock_reinstall", "upgrade_replace_existing"}:
            return ["org.jkiss.dbeaver.ext.scratchbird.feature.feature.group"]
        if phase == "stock_uninstall":
            return ["org.eclipse.equinox.p2.director"]
        return ["scratchbird-jdbc.jar"]

    def _mark_fixture_release_proof_passed(self) -> None:
        proof_rel = "proof/dbeaver-lifecycle-proof.json"
        proof_path = self.repo / proof_rel
        proof_path.parent.mkdir(parents=True, exist_ok=True)
        proof_path.write_text(
            json.dumps({"status": "passed", "proof_kind": "lifecycle_fixture"}, indent=2),
            encoding="utf-8",
        )
        payload = json.loads(self.fixture.read_text(encoding="utf-8"))
        for row in payload["lifecycle_cases"]:
            row["proof_status"] = "passed"
            row["proof_artifact"] = proof_rel
        self.fixture.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    def _write_release_artifacts(self) -> Path:
        artifact_root = self.repo / "artifacts"
        artifact_root.mkdir(parents=True, exist_ok=True)

        core_plugin = self.root / "core-plugin.jar"
        with zipfile.ZipFile(core_plugin, "w") as archive:
            archive.writestr("drivers/scratchbird/scratchbird-jdbc.jar", b"jdbc")
            archive.writestr("META-INF/MANIFEST.MF", "Bundle-SymbolicName: org.jkiss.dbeaver.ext.scratchbird\n")
        ui_plugin = self.root / "ui-plugin.jar"
        with zipfile.ZipFile(ui_plugin, "w") as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Bundle-SymbolicName: org.jkiss.dbeaver.ext.scratchbird.ui\n")
        feature = self.root / "feature.jar"
        with zipfile.ZipFile(feature, "w") as archive:
            archive.writestr("feature.xml", "<feature id='org.jkiss.dbeaver.ext.scratchbird.feature'/>")

        update_site = artifact_root / "scratchbird-dbeaver-update-site-20260613T000000.zip"
        with zipfile.ZipFile(update_site, "w") as archive:
            archive.writestr("content.jar", b"content")
            archive.writestr("artifacts.jar", b"artifacts")
            archive.write(feature, "features/org.jkiss.dbeaver.ext.scratchbird.feature_1.0.1.jar")
            archive.write(core_plugin, "plugins/org.jkiss.dbeaver.ext.scratchbird_1.0.1.jar")
            archive.write(ui_plugin, "plugins/org.jkiss.dbeaver.ext.scratchbird.ui_1.0.1.jar")

        notices = "\n".join(
            [
                "DBeaver Apache-2.0",
                "Eclipse Platform EPL-2.0",
                "Tycho EPL-2.0",
                "SWT EPL-2.0",
                "JFace EPL-2.0",
                "ScratchBird JDBC MPL-2.0",
            ]
        )
        stock_bundle = artifact_root / "scratchbird-dbeaver-stock-test-bundle-20260613T000000.zip"
        base = "scratchbird-dbeaver-stock-test-bundle-20260613T000000"
        with zipfile.ZipFile(stock_bundle, "w") as archive:
            archive.writestr(f"{base}/README.txt", "ScratchBird DBeaver Stock Test Bundle")
            archive.writestr(f"{base}/THIRD-PARTY-NOTICES.txt", notices)
            archive.writestr(f"{base}/install-into-stock-dbeaver.sh", "#!/usr/bin/env bash\n")
            archive.writestr(f"{base}/install-into-stock-dbeaver.bat", "@echo off\n")
            archive.writestr(f"{base}/uninstall-from-stock-dbeaver.sh", "#!/usr/bin/env bash\n")
            archive.writestr(f"{base}/uninstall-from-stock-dbeaver.bat", "@echo off\n")
            archive.writestr(f"{base}/SHA256SUMS.txt", "checksums\n")
            archive.write(update_site, f"{base}/{update_site.name}")

        (artifact_root / "SHA256SUMS.txt").write_text("checksums\n", encoding="utf-8")
        (artifact_root / "scratchbird-dbeaver-source-checkout-proof-20260613.json").write_text(
            json.dumps({"status": "passed", "artifact_type": "source_checkout_verify"}, indent=2),
            encoding="utf-8",
        )
        (artifact_root / "scratchbird-dbeaver-stock-install-proof-20260613.json").write_text(
            json.dumps({"status": "passed", "artifact_type": "stock_install_lifecycle"}, indent=2),
            encoding="utf-8",
        )
        return artifact_root

    def test_gate_accepts_minimal_contract(self) -> None:
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.report()["status"], "pass")
        self.assertTrue(self.report()["closure_support"]["can_support_static_preflight"])
        self.assertFalse(self.report()["closure_support"]["can_support_final_closure"])

    def test_release_mode_accepts_verified_package_artifacts_and_proof(self) -> None:
        self._mark_fixture_release_proof_passed()
        artifact_root = self._write_release_artifacts()
        result = self.run_gate("--mode", "release", "--artifact-root", str(artifact_root))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.report()["status"], "pass")

    def test_gate_rejects_runtime_http_client_import(self) -> None:
        write(
            self.adapter
            / "plugins/org.jkiss.dbeaver.ext.scratchbird/src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdNetwork.java",
            """
            // SPDX-License-Identifier: MPL-2.0
            package org.jkiss.dbeaver.ext.scratchbird.model;
            import java.net.http.HttpClient;
            public final class ScratchBirdNetwork {}
            """,
        )
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            any("runtime_network_api_present" in issue for issue in self.report()["issues"]),
            self.report()["issues"],
        )

    def test_gate_rejects_skipped_required_packaging_proof(self) -> None:
        script = self.adapter / "scripts" / "build-p2-update-site.sh"
        script.write_text(
            script.read_text(encoding="utf-8") + "\nmvn clean verify -DskipTests\n",
            encoding="utf-8",
        )
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            any("script_skips_required_proof" in issue for issue in self.report()["issues"]),
            self.report()["issues"],
        )

    def test_gate_rejects_insecure_sensitive_provider_property(self) -> None:
        plugin_xml = self.adapter / "plugins/org.jkiss.dbeaver.ext.scratchbird/plugin.xml"
        plugin_xml.write_text(
            plugin_xml.read_text(encoding="utf-8").replace(
                '<property id="auth_token" type="string" features="secured,password"/>',
                '<property id="auth_token" type="string"/>',
            ),
            encoding="utf-8",
        )
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            any("provider_property_not_secured_password:auth_token" in issue for issue in self.report()["issues"]),
            self.report()["issues"],
        )

    def test_release_mode_rejects_missing_required_proof_status(self) -> None:
        artifact_root = self._write_release_artifacts()
        result = self.run_gate("--mode", "release", "--artifact-root", str(artifact_root))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            any("required_proof_status_missing_or_not_passed" in issue for issue in self.report()["issues"]),
            self.report()["issues"],
        )

    def test_release_mode_rejects_update_site_without_nested_jdbc_jar(self) -> None:
        self._mark_fixture_release_proof_passed()
        artifact_root = self._write_release_artifacts()
        update_site = artifact_root / "scratchbird-dbeaver-update-site-20260613T000000.zip"
        core_plugin = self.root / "core-plugin-without-driver.jar"
        with zipfile.ZipFile(core_plugin, "w") as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Bundle-SymbolicName: org.jkiss.dbeaver.ext.scratchbird\n")
        with zipfile.ZipFile(update_site, "w") as archive:
            archive.writestr("content.jar", b"content")
            archive.writestr("artifacts.jar", b"artifacts")
            archive.writestr("features/org.jkiss.dbeaver.ext.scratchbird.feature_1.0.1.jar", b"feature")
            archive.write(core_plugin, "plugins/org.jkiss.dbeaver.ext.scratchbird_1.0.1.jar")
            archive.writestr("plugins/org.jkiss.dbeaver.ext.scratchbird.ui_1.0.1.jar", b"ui")
        result = self.run_gate("--mode", "release", "--artifact-root", str(artifact_root))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            any("update_site_core_plugin_missing_scratchbird_jdbc_jar" in issue for issue in self.report()["issues"]),
            self.report()["issues"],
        )

    def test_gate_rejects_missing_unsupported_java_refusal(self) -> None:
        payload = json.loads(self.contract.read_text(encoding="utf-8"))
        payload["supported_version_matrix"] = [
            row
            for row in payload["supported_version_matrix"]
            if row.get("refusal_reason") != "unsupported_java_version"
        ]
        self.contract.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            any("missing_refusal_case:unsupported_java_version" in issue for issue in self.report()["issues"]),
            self.report()["issues"],
        )


if __name__ == "__main__":
    unittest.main()
