// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import org.junit.jupiter.api.Test;

class ScratchBirdPackageContractTest {

  @Test
  void packageContractRecordsHibernateReleasePosture() throws IOException {
    String contract = Files.readString(Path.of("package_contract.json"));

    assertTrue(contract.contains("\"component_id\": \"adaptor:scratchbird-hibernate-dialect\""));
    assertTrue(contract.contains("\"status\": \"beta_2\""));
    assertTrue(contract.contains("\"release_scope\": \"in_scope_required\""));
    assertTrue(contract.contains("\"server_revalidation_required\": true"));
    assertTrue(contract.contains("\"transaction_authority\": \"mga_engine\""));
    assertTrue(contract.contains("\"mode\": \"delegates_to_jdbc\""));
    assertTrue(contract.contains("\"target_component\": \"driver:jdbc\""));
    assertTrue(contract.contains("\"this_artifact_includes_dbeaver\": false"));
    assertTrue(contract.contains("\"package_type\": \"maven_jar\""));
  }

  @Test
  void packageContractFilesArePresentAndExcludeDbeaver() throws IOException {
    String contract = Files.readString(Path.of("package_contract.json"));
    for (String relative : List.of("package_contract.json", "pom.xml", "README.md")) {
      assertTrue(Files.isRegularFile(Path.of(relative)), relative + " should exist");
      assertTrue(contract.contains("\"" + relative + "\""), relative + " should be listed");
      assertTrue(!relative.contains("dbeaver"), relative + " must not include DBeaver");
    }
  }
}
