<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

require_once __DIR__ . '/bootstrap.php';

use PHPUnit\Framework\TestCase;
use ScratchBird\PDO\Connection;
use ScratchBird\PDO\ScratchBirdAuthException;
use ScratchBird\PDO\ScratchBirdPDO;
use ScratchBird\PDO\ScratchBirdException;
use ScratchBird\PDO\ScratchBirdIntegrityException;
use ScratchBird\PDO\ScratchBirdNotSupportedException;
use ScratchBird\PDO\ScratchBirdTransactionException;

final class IntegrationTest extends TestCase
{
    public function testSelect(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->query('SELECT 1');
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertSame(1, (int)$row[0]);
    }

    public function testPrepareBind(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->prepare('SELECT ?::INTEGER');
        $stmt->execute([42]);
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertSame(42, (int)$row[0]);
    }

    public function testTypesFixture(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->query('SELECT * FROM type_coverage');
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertNotFalse($row);
    }

    public function testConnectWithCompatibilityConnOptions(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $separator = str_contains($dsn, '?') ? '&' : '?';
        $pdo = new ScratchBirdPDO($dsn . $separator . 'binary_transfer=false&compression=zstd');
        $stmt = $pdo->query('SELECT 1');
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertSame(1, (int)$row[0]);
    }

    public function testCancel(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $cancelSql = getenv('SCRATCHBIRD_PHP_CANCEL_SQL') ?: getenv('SCRATCHBIRD_TEST_CANCEL_SQL');
        if (!$cancelSql) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_CANCEL_SQL/SCRATCHBIRD_TEST_CANCEL_SQL not set');
        }
        $conn = new \ScratchBird\PDO\Connection($dsn);
        $stream = $conn->executeQuery($cancelSql);
        $conn->cancel();
        $this->expectException(\Throwable::class);
        $stream->readRow();
    }

    public function testQueryMultiReturnsIndependentResultSets(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        try {
            $results = $pdo->queryMulti('SELECT 1 AS first_value; SELECT 2 AS second_value');
        } catch (\Throwable $ex) {
            $this->skipIfFeatureUnsupported($ex, 'queryMulti');
            throw $ex;
        }
        $this->assertCount(2, $results);
        $this->assertSame(1, (int)($results[0]['rows'][0]['first_value'] ?? 0));
        $this->assertSame(2, (int)($results[1]['rows'][0]['second_value'] ?? 0));
    }

    public function testExecuteMultiAliasReturnsIndependentResultSets(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        try {
            $results = $pdo->executeMulti('SELECT 3 AS third_value; SELECT 4 AS fourth_value');
        } catch (\Throwable $ex) {
            $this->skipIfFeatureUnsupported($ex, 'executeMulti');
            throw $ex;
        }
        $this->assertCount(2, $results);
        $this->assertSame(3, (int)($results[0]['rows'][0]['third_value'] ?? 0));
        $this->assertSame(4, (int)($results[1]['rows'][0]['fourth_value'] ?? 0));
    }

    public function testExecuteBatchReturnsPerItemSummary(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $batch = $pdo->executeBatch('SELECT ?::INTEGER AS value', [[11], [22], [33]]);
        $this->assertCount(3, $batch['items']);
        $this->assertSame(0, $batch['items'][0]['index']);
        $this->assertSame(1, $batch['items'][0]['rowCount']);
        $this->assertSame(3, $batch['totalRowCount']);
    }

    public function testCallExecutesJdbcCallableEscapeSyntax(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        try {
            $stmt = $pdo->call('{ ? = call abs(?) }', [-3]);
        } catch (\Throwable $ex) {
            $this->skipIfFeatureUnsupported($ex, 'call');
            throw $ex;
        }
        $row = $stmt->fetch(\PDO::FETCH_ASSOC);
        $this->assertNotFalse($row);
        $firstValue = $row['return_value'] ?? array_values($row)[0] ?? null;
        $this->assertSame(3, (int)$firstValue);
    }

    public function testStatementNextRowsetTraversesMultipleResults(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        try {
            $stmt = $pdo->query('SELECT 10 AS first_value; SELECT 20 AS second_value');
        } catch (\Throwable $ex) {
            $this->skipIfFeatureUnsupported($ex, 'nextRowset');
            throw $ex;
        }
        $first = $stmt->fetch(\PDO::FETCH_ASSOC);
        $this->assertSame(10, (int)$first['first_value']);
        $this->assertTrue($stmt->nextRowset());
        $second = $stmt->fetch(\PDO::FETCH_ASSOC);
        $this->assertSame(20, (int)$second['second_value']);
        $this->assertFalse($stmt->nextRowset());
    }

    public function testManagerProxySelect(): void
    {
        $dsn = getenv('SCRATCHBIRD_PHP_MANAGER_PROXY_DSN');
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_MANAGER_PROXY_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->query('SELECT 1');
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertSame(1, (int)$row[0]);
    }

    public function testTlsVerifyCaSelect(): void
    {
        $dsn = getenv('SCRATCHBIRD_PHP_TLS_VERIFY_CA_DSN');
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_TLS_VERIFY_CA_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->query('SELECT 1');
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertSame(1, (int)$row[0]);
    }

    public function testTlsVerifyFullSelect(): void
    {
        $dsn = getenv('SCRATCHBIRD_PHP_TLS_VERIFY_FULL_DSN');
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_TLS_VERIFY_FULL_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->query('SELECT 1');
        $row = $stmt->fetch(\PDO::FETCH_NUM);
        $this->assertSame(1, (int)$row[0]);
    }

    public function testTransactionLifecycleWithSavepointAndRollbackToSavepoint(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $this->assertTrue($pdo->beginTransaction());
        $this->assertTrue($pdo->inTransaction());
        $pdo->savepoint('php_sp1');
        $stmt = $pdo->query('SELECT 1');
        $this->assertNotFalse($stmt->fetch(\PDO::FETCH_NUM));
        $pdo->rollbackToSavepoint('php_sp1');
        $pdo->releaseSavepoint('php_sp1');
        $this->assertTrue($pdo->commit());
        $this->assertTrue($pdo->inTransaction());
    }

    public function testNativeConnectionAdoptsFreshBoundaryAndRejectsNestedBegin(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $conn = new Connection($dsn);
        try {
            $this->assertTrue($conn->beginTransaction());
            $this->assertTrue($conn->inTransaction());

            try {
                $conn->beginTransaction();
                $this->fail('Expected nested begin rejection');
            } catch (ScratchBirdTransactionException $ex) {
                $this->assertSame('25001', $ex->sqlState);
            }

            $this->assertTrue($conn->rollBack());
            $this->assertTrue($conn->inTransaction());
        } finally {
            $conn->close();
        }
    }

    public function testNativeConnectionPostRollbackQueryReturnsActualResult(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $conn = new Connection($dsn);
        try {
            $this->assertTrue($conn->beginTransaction());
            $this->assertTrue($conn->rollBack());
            $this->assertTrue($conn->inTransaction());

            $stream = $conn->executeQuery('SELECT 2');
            $row = $stream->readRow();
            $this->assertNotNull($row);
            $this->assertSame(2, (int)$row[0]);
        } finally {
            $conn->close();
        }
    }

    public function testMetadataCollectionsAndRestrictionsLiveShape(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        try {
            $tables = $pdo->getSchema('tables');
            $types = $pdo->getSchema('type_info');
            $catalogs = $pdo->getSchema('catalogs');
            $restricted = $pdo->getSchema('tables', ['schema' => 'sys']);
            $procedures = $pdo->procedures(null, 'users');
            $functions = $pdo->functions(null, 'users');
            $routines = $pdo->routines(null, 'users');
            $tree = $pdo->getSchemaTree(true);
        } catch (\Throwable $ex) {
            $this->skipIfFeatureUnsupported($ex, 'metadata');
            throw $ex;
        }

        $this->assertIsArray($tables);
        $this->assertIsArray($types);
        $this->assertIsArray($catalogs);
        $this->assertIsArray($restricted);
        $this->assertIsArray($procedures);
        $this->assertIsArray($functions);
        $this->assertIsArray($routines);
        $this->assertIsArray($tree);
        $this->assertArrayHasKey('schemas', $tree);
        $this->assertIsArray($tree['schemas']);
    }

    public function testConstraintViolationMapsToIntegrityException(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $pdo = new ScratchBirdPDO($dsn);
        $table = 'php_err_' . (string) time();
        try {
            $pdo->exec("CREATE TABLE {$table} (id INTEGER PRIMARY KEY)");
            $pdo->exec("INSERT INTO {$table} (id) VALUES (1)");
        } catch (\Throwable $ex) {
            $this->markTestSkipped('runtime does not support temporary integrity fixture: ' . $ex->getMessage());
        }

        try {
            $pdo->exec("INSERT INTO {$table} (id) VALUES (1)");
            $this->fail('Expected duplicate-key violation');
        } catch (ScratchBirdIntegrityException $ex) {
            $this->assertSame('23', substr($ex->sqlState, 0, 2));
        } finally {
            try {
                $pdo->exec("DROP TABLE {$table}");
            } catch (\Throwable) {
                // best effort cleanup
            }
        }
    }

    public function testBadAuthDsnMapsToAuthException(): void
    {
        $badDsn = getenv('SCRATCHBIRD_PHP_BAD_AUTH_DSN');
        if (!$badDsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_BAD_AUTH_DSN not set');
        }
        try {
            new ScratchBirdPDO($badDsn);
            $this->fail('Expected authentication failure');
        } catch (ScratchBirdAuthException $ex) {
            $this->assertSame('28', substr($ex->sqlState, 0, 2));
        }
    }

    public function testTypeMatrixRoundTripQuery(): void
    {
        $dsn = $this->integrationDsn();
        if (!$dsn) {
            $this->markTestSkipped('SCRATCHBIRD_PHP_URL/SCRATCHBIRD_TEST_DSN not set');
        }
        $sql = getenv('SCRATCHBIRD_PHP_TYPE_MATRIX_SQL');
        if (!$sql) {
            $sql = 'SELECT * FROM type_coverage LIMIT 1';
        }

        $pdo = new ScratchBirdPDO($dsn);
        $stmt = $pdo->query($sql);
        $row = $stmt->fetch(\PDO::FETCH_ASSOC);
        $this->assertNotFalse($row);
        $this->assertIsArray($row);
        $this->assertNotSame([], $row);
    }

    private function integrationDsn(): string|false
    {
        return getenv('SCRATCHBIRD_PHP_URL') ?: getenv('SCRATCHBIRD_TEST_DSN');
    }

    private function skipIfFeatureUnsupported(\Throwable $ex, string $feature): void
    {
        if ($ex instanceof ScratchBirdNotSupportedException) {
            $this->markTestSkipped($feature . ' not supported by runtime: ' . $ex->getMessage());
        }
        if ($ex instanceof ScratchBirdException && $ex->sqlState === '0A000') {
            $this->markTestSkipped($feature . ' not supported by runtime: ' . $ex->getMessage());
        }
    }
}
