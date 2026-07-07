// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import org.hibernate.dialect.Dialect;
import org.junit.jupiter.api.Test;

class ScratchBirdDialectContractTest {

  @Test
  void jdbcUrlPolicyRejectsUnsupportedFlags() {
    assertThrows(
        IllegalArgumentException.class,
        () ->
            ScratchBirdJdbcUrlPolicy.validateAndParse(
                "jdbc:scratchbird://localhost:3092/main?sslmode=disable"));

    assertThrows(
        IllegalArgumentException.class,
        () ->
            ScratchBirdJdbcUrlPolicy.validateAndParse(
                "jdbc:scratchbird://localhost:3092/main?binaryTransfer=false"));

    assertThrows(
        IllegalArgumentException.class,
        () ->
            ScratchBirdJdbcUrlPolicy.validateAndParse(
                "jdbc:scratchbird://localhost:3092/main?compression=zstd"));
  }

  @Test
  void jdbcUrlPolicyAcceptsSupportedFlags() {
    Map<String, String> params =
        ScratchBirdJdbcUrlPolicy.validateAndParse(
            "jdbc:scratchbird://localhost:3092/main?sslmode=require&binaryTransfer=true&connectTimeout=30"
                + "&currentSchema=tenant.analytics"
                + "&frontDoorMode=manager_proxy"
                + "&managerAuthToken=manager-token"
                + "&authToken=opaque-token"
                + "&authMethodId=scratchbird.auth.jwt_oidc"
                + "&authRequiredMethods=TOKEN,SCRAM_SHA_512"
                + "&authForbiddenMethods=MD5"
                + "&authRequireChannelBinding=true"
                + "&workloadIdentityToken=workload-token"
                + "&proxyPrincipalAssertion=proxy-assertion"
                + "&dormantId=dormant-1"
                + "&dormantReattachToken=reattach-token");

    assertEquals("require", params.get("sslmode"));
    assertEquals("true", params.get("binaryTransfer"));
    assertEquals("30", params.get("connectTimeout"));
    assertEquals("tenant.analytics", params.get("search_path"));
    assertEquals("manager_proxy", params.get("front_door_mode"));
    assertEquals("manager-token", params.get("manager_auth_token"));
    assertEquals("opaque-token", params.get("auth_token"));
    assertEquals("scratchbird.auth.jwt_oidc", params.get("auth_method_id"));
    assertEquals("TOKEN,SCRAM_SHA_512", params.get("auth_required_methods"));
    assertEquals("MD5", params.get("auth_forbidden_methods"));
    assertEquals("true", params.get("auth_require_channel_binding"));
    assertEquals("workload-token", params.get("workload_identity_token"));
    assertEquals("proxy-assertion", params.get("proxy_principal_assertion"));
    assertEquals("dormant-1", params.get("dormant_id"));
    assertEquals("reattach-token", params.get("dormant_reattach_token"));
  }

  @Test
  void jdbcUrlPolicyRejectsInvalidBootstrapNamespaceAndIngressMode() {
    assertThrows(
        IllegalArgumentException.class,
        () ->
            ScratchBirdJdbcUrlPolicy.validateAndParse(
                "jdbc:scratchbird://localhost:3092/main?frontDoorMode=sidecar"));

    assertThrows(
        IllegalArgumentException.class,
        () ->
            ScratchBirdJdbcUrlPolicy.validateAndParse(
                "jdbc:scratchbird://localhost:3092/main?authMethodId=jwt_oidc"));
  }

  @Test
  void dialectProvidesTypeContributionsAndQualifiedNames() {
    ScratchBirdDialect dialect = new ScratchBirdDialect();

    assertInstanceOf(Dialect.class, dialect);
    assertTrue(dialect.supportsRecursiveSchemaMetadata());
    assertEquals("\"sys\".\"users\"", dialect.formatQualifiedName("sys", "users"));
    assertEquals("\"users\"", dialect.formatQualifiedName(null, "users"));

    Map<String, String> typeMap = dialect.getScratchBirdTypeContributions();
    assertEquals("json", typeMap.get("JSONB"));
    assertEquals("uuid-char", typeMap.get("UUID"));
    assertEquals("string", dialect.mapScratchBirdTypeToHibernate("vector(1536)"));
    assertFalse(typeMap.isEmpty());
  }

  @Test
  void metadataMapperBuildsIdentityAndConstraintContracts() {
    ScratchBirdJdbcMetadataMapper mapper = new ScratchBirdJdbcMetadataMapper();
    ScratchBirdJdbcMetadataMapper.ColumnMetadata metadata =
        new ScratchBirdJdbcMetadataMapper.ColumnMetadata(
            "sys", "users", "id", "BIGINT", false, true, null);

    Map<String, Object> mapped = mapper.toHibernateColumnDefinition(metadata);
    assertEquals("long", mapped.get("hibernateType"));
    assertEquals(false, mapped.get("nullable"));
    assertEquals(true, mapped.get("identity"));

    String pk = mapper.generatePrimaryKeyConstraint("users", List.of("id"));
    assertEquals("alter table users add primary key (id)", pk);

    String fk =
        mapper.generateForeignKeyConstraint(
            "orders", "fk_orders_user", List.of("user_id"), "users", List.of("id"));
    assertEquals(
        "alter table orders add constraint fk_orders_user foreign key (user_id) references users (id)",
        fk);
  }

  @Test
  void transactionContractBuildsSavepointLifecycleSql() {
    assertEquals("BEGIN", ScratchBirdTransactionContract.begin());
    assertEquals("COMMIT", ScratchBirdTransactionContract.commit());
    assertEquals("ROLLBACK", ScratchBirdTransactionContract.rollback());
    assertEquals("SAVEPOINT sp1", ScratchBirdTransactionContract.savepoint("sp1"));
    assertEquals(
        "ROLLBACK TO SAVEPOINT sp1", ScratchBirdTransactionContract.rollbackToSavepoint("sp1"));
    assertEquals("RELEASE SAVEPOINT sp1", ScratchBirdTransactionContract.releaseSavepoint("sp1"));

    assertThrows(
        IllegalArgumentException.class, () -> ScratchBirdTransactionContract.savepoint("sp-1"));
  }
}
