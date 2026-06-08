<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use PHPUnit\Framework\TestCase;
use ScratchBird\PDO\Config;

final class ConfigTest extends TestCase
{
    public function testParseUri(): void
    {
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?sslmode=require&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd');
        $this->assertSame('localhost', $cfg->host);
        $this->assertSame(3092, $cfg->port);
        $this->assertSame('mydb', $cfg->database);
        $this->assertSame('user', $cfg->user);
        $this->assertSame('pass', $cfg->password);
        $this->assertSame('require', $cfg->sslMode);
        $this->assertSame(3000, $cfg->connectTimeoutMs);
        $this->assertSame('app', $cfg->applicationName);
        $this->assertFalse($cfg->binaryTransfer);
        $this->assertSame('zstd', $cfg->compression);
    }

    public function testParseKeyValue(): void
    {
        $cfg = Config::fromDsn('Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;Timeout=5;Socket_Timeout=7');
        $this->assertSame('server', $cfg->host);
        $this->assertSame(4000, $cfg->port);
        $this->assertSame('db', $cfg->database);
        $this->assertSame('me', $cfg->user);
        $this->assertSame('secret', $cfg->password);
        $this->assertSame(5000, $cfg->connectTimeoutMs);
        $this->assertSame(7000, $cfg->socketTimeoutMs);
    }

    public function testParseManagerProxyParams(): void
    {
        $cfg = Config::fromDsn('scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7');
        $this->assertSame('manager_proxy', $cfg->frontDoorMode);
        $this->assertSame('token', $cfg->managerAuthToken);
        $this->assertSame(7, $cfg->managerClientFlags);
    }

    public function testParseBooleanVariants(): void
    {
        $cfg = Config::fromDsn('binary_transfer=off manager_auth_fast_path=no');
        $this->assertFalse($cfg->binaryTransfer);
        $this->assertFalse($cfg->managerAuthFastPath);

        $cfg = Config::fromDsn('binary_transfer=on manager_auth_fast_path=yes');
        $this->assertTrue($cfg->binaryTransfer);
        $this->assertTrue($cfg->managerAuthFastPath);
    }

    public function testParseProtocolAndFrontDoorAliases(): void
    {
        $cfg = Config::fromDsn('protocol=scratchbird_native front_door_mode=managed');
        $this->assertSame('native', $cfg->protocol);
        $this->assertSame('manager_proxy', $cfg->frontDoorMode);
    }

    public function testParseMetadataExpandSchemaParentsAliases(): void
    {
        $cfg = Config::fromDsn('metadata_expand_schema_parents=yes');
        $this->assertTrue($cfg->metadataExpandSchemaParents);

        $cfg = Config::fromDsn('expand_schema_parents=1');
        $this->assertTrue($cfg->metadataExpandSchemaParents);

        $cfg = Config::fromDsn('dbeaver_expand_schema_parents=on');
        $this->assertTrue($cfg->metadataExpandSchemaParents);

        $cfg = Config::fromDsn('metadataexpandschemaparents=off');
        $this->assertFalse($cfg->metadataExpandSchemaParents);
    }

    public function testCompressionNormalizationAcceptsNoneAlias(): void
    {
        $cfg = Config::fromDsn('compression=none');
        $this->assertSame('off', $cfg->compression);
    }

    public function testCompressionNormalizationRejectsUnknownValue(): void
    {
        $this->expectException(\InvalidArgumentException::class);
        $this->expectExceptionMessage('compression must be off or zstd');
        Config::fromDsn('compression=gzip');
    }

    public function testParseAuthPluginAndPinningParams(): void
    {
        $cfg = Config::fromDsn(
            'scratchbird://user:pass@localhost:3092/mydb'
            . '?connect_client_flags=257'
            . '&auth_method_id=scratchbird.auth.proxy_assertion'
            . '&auth_method_payload=opaque'
            . '&auth_token=bearer-token'
            . '&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D'
            . '&auth_payload_b64=YWJj'
            . '&auth_provider_profile=corp_primary'
            . '&auth_required_methods=SCRAM_SHA_256%2CTOKEN'
            . '&auth_forbidden_methods=MD5'
            . '&auth_require_channel_binding=true'
            . '&workload_identity_token=jwt-token'
            . '&proxy_principal_assertion=signed-assertion'
        );

        $this->assertSame(257, $cfg->connectClientFlags);
        $this->assertSame('scratchbird.auth.proxy_assertion', $cfg->authMethodId);
        $this->assertSame('opaque', $cfg->authMethodPayload);
        $this->assertSame('bearer-token', $cfg->authToken);
        $this->assertSame('{"subject":"alice"}', $cfg->authPayloadJson);
        $this->assertSame('YWJj', $cfg->authPayloadB64);
        $this->assertSame('corp_primary', $cfg->authProviderProfile);
        $this->assertSame('SCRAM_SHA_256,TOKEN', $cfg->authRequiredMethods);
        $this->assertSame('MD5', $cfg->authForbiddenMethods);
        $this->assertTrue($cfg->authRequireChannelBinding);
        $this->assertSame('jwt-token', $cfg->workloadIdentityToken);
        $this->assertSame('signed-assertion', $cfg->proxyPrincipalAssertion);
    }
}
