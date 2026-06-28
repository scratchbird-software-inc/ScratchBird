<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

use ScratchBird\CircuitBreaker;
use ScratchBird\TelemetryCollector;
use ScratchBird\KeepaliveManager;
use ScratchBird\KeepaliveTracker;
use ScratchBird\LeakDetector;
use ScratchBird\LeakDetectionGuard;

final class Connection
{
    private const QUERY_FLAG_BINARY_RESULT = 0x04;
    private const DEFAULT_SESSION_SCHEMA = 'users.public';
    private const MANAGER_PROTOCOL_MAGIC = 0x42444253; // SBDB
    private const MANAGER_PROTOCOL_VERSION = 0x0101;
    private const MANAGER_HEADER_SIZE = 12;
    private const MANAGER_MAX_PAYLOAD_SIZE = 16777216;
    private const MCP_PROTOCOL_VERSION = 0x0100;
    private const MCP_MSG_CONNECT_RESPONSE = 0x02;
    private const MCP_MSG_AUTH_CHALLENGE = 0x12;
    private const MCP_MSG_AUTH_RESPONSE = 0x11;
    private const MCP_MSG_STATUS_RESPONSE = 0x64;
    private const MCP_MSG_HELLO = 0x65;
    private const MCP_MSG_AUTH_START = 0x66;
    private const MCP_MSG_AUTH_CONTINUE = 0x67;
    private const MCP_MSG_DB_CONNECT = 0x69;
    private const MCP_AUTH_METHOD_TOKEN = 4;
    private static int $connectionCounter = 0;

    private Config $config;
    /** @var resource|null */
    private $socket = null;
    private string $attachmentId = '';
    private int $txnId = 0;
    private int $sequence = 0;
    private int $lastQuerySequence = 0;
    private int $lastMaxRows = 0;
    private bool $inTransaction = false;
    private bool $runtimeTxnActive = false;
    private bool $explicitTransaction = false;
    private bool $portalResumePending = false;
    private bool $connected = false;
    private array $attributes = [];
    private array $parameters = [];
    private array $lastError = ['00000', 0, null];
    private array $notificationHandlers = [];
    private ?array $lastPlan = null;
    private ?array $lastSblr = null;
    private bool $hasLastInsertId = false;
    private int $lastInsertIdValue = 0;
    private string $connectionId;
    private CircuitBreaker $circuitBreaker;
    private TelemetryCollector $telemetry;
    private KeepaliveManager $keepaliveManager;
    private ?KeepaliveTracker $keepaliveTracker = null;
    private LeakDetector $leakDetector;
    private ?LeakDetectionGuard $leakGuard = null;
    private array $resolvedAuthContext = [
        'front_door_mode' => 'direct',
        'resolved_auth_method' => '',
        'resolved_auth_plugin_id' => '',
        'manager_authenticated' => false,
        'attached' => false,
    ];

    public function __construct(string $dsn, ?string $username = null, ?string $password = null, array $options = [])
    {
        $this->config = Config::fromDsn($dsn);
        if ($username !== null) {
            $this->config->user = $username;
        }
        if ($password !== null) {
            $this->config->password = $password;
        }
        $this->attributes = $options;
        self::$connectionCounter++;
        $this->connectionId = 'conn-' . self::$connectionCounter;
        $this->circuitBreaker = new CircuitBreaker();
        $this->telemetry = new TelemetryCollector();
        $this->keepaliveManager = new KeepaliveManager();
        $this->leakDetector = new LeakDetector();
        $this->keepaliveManager->start();
        $this->leakDetector->start();
        try {
            $this->connect();
            $this->keepaliveTracker = $this->keepaliveManager->register(
                $this->connectionId,
                $this,
                function (): bool {
                    $this->ping();
                    return true;
                }
            );
            $this->leakGuard = $this->leakDetector->checkout($this->connectionId, ['driver' => 'php']);
        } catch (\Throwable $ex) {
            $this->close();
            throw $ex;
        }
    }

    public static function probeAuthSurface(string $dsn): array
    {
        $cfg = Config::fromDsn($dsn);
        $conn = self::newBootstrapConnection($cfg);
        try {
            return $conn->probeAuthSurfaceInternal();
        } finally {
            $conn->close();
        }
    }

    public function getResolvedAuthContext(): array
    {
        return $this->resolvedAuthContext;
    }

    private static function newBootstrapConnection(Config $cfg): self
    {
        $class = new \ReflectionClass(self::class);
        /** @var self $conn */
        $conn = $class->newInstanceWithoutConstructor();
        $conn->config = $cfg;
        $conn->socket = null;
        $conn->attachmentId = str_repeat("\0", 16);
        $conn->txnId = 0;
        $conn->sequence = 0;
        $conn->lastQuerySequence = 0;
        $conn->lastMaxRows = 0;
        $conn->inTransaction = false;
        $conn->runtimeTxnActive = false;
        $conn->explicitTransaction = false;
        $conn->portalResumePending = false;
        $conn->connected = false;
        $conn->attributes = [];
        $conn->parameters = [];
        $conn->lastError = ['00000', 0, null];
        $conn->notificationHandlers = [];
        $conn->lastPlan = null;
        $conn->lastSblr = null;
        $conn->hasLastInsertId = false;
        $conn->lastInsertIdValue = 0;
        self::$connectionCounter++;
        $conn->connectionId = 'probe-' . self::$connectionCounter;
        $conn->circuitBreaker = new CircuitBreaker();
        $conn->telemetry = new TelemetryCollector();
        $conn->keepaliveManager = new KeepaliveManager();
        $conn->keepaliveTracker = null;
        $conn->leakDetector = new LeakDetector();
        $conn->leakGuard = null;
        $conn->resolvedAuthContext = self::defaultResolvedAuthContext(
            $cfg->frontDoorMode !== '' ? $cfg->frontDoorMode : 'direct'
        );
        return $conn;
    }

    private static function defaultResolvedAuthContext(string $frontDoorMode): array
    {
        return [
            'front_door_mode' => $frontDoorMode !== '' ? $frontDoorMode : 'direct',
            'resolved_auth_method' => '',
            'resolved_auth_plugin_id' => '',
            'manager_authenticated' => false,
            'attached' => false,
        ];
    }

    public function prepare(string $statement, array $options = []): Statement
    {
        return new Statement($this, $statement, $options);
    }

    public function query(string $statement, mixed ...$fetchModeArgs): Statement
    {
        $stmt = $this->prepare($statement);
        $stmt->execute();
        return $stmt;
    }

    public function nativeSql(string $sql, array $params = []): string
    {
        try {
            $normalized = Sql::normalize($sql, $params);
        } catch (\InvalidArgumentException $ex) {
            throw new ScratchBirdException($ex->getMessage(), '07001');
        }
        return $normalized['sql'];
    }

    public function nativeCallableSql(string $sql, array $params = []): string
    {
        try {
            $normalized = Sql::normalizeCallable($sql, $params);
        } catch (\InvalidArgumentException $ex) {
            throw new ScratchBirdException($ex->getMessage(), '07001');
        }
        return $normalized['sql'];
    }

    public function call(string $sql, array $params = []): Statement
    {
        try {
            $normalized = Sql::normalizeCallable($sql, $params);
        } catch (\InvalidArgumentException $ex) {
            throw new ScratchBirdException($ex->getMessage(), '07001');
        }
        $stmt = $this->prepare($normalized['sql']);
        $stmt->execute($normalized['params']);
        return $stmt;
    }

    /**
     * @return array{items: array<int, array{index: int, rowCount: int, fields: array, command: string, lastId: int|false}>, totalRowCount: int}
     */
    public function executeBatch(string $sql, iterable $batchParams): array
    {
        $items = [];
        $totalRowCount = 0;
        $index = 0;
        foreach ($batchParams as $params) {
            if ($params instanceof \Traversable) {
                $params = iterator_to_array($params);
            }
            if (!is_array($params)) {
                throw new ScratchBirdException('batch parameter entry must be an array', '07001');
            }
            $stmt = $this->prepare($sql);
            $stmt->execute($params);
            $stmt->fetchAll(\PDO::FETCH_ASSOC);
            $rowCount = $stmt->rowCount();
            if ($rowCount > 0) {
                $totalRowCount += $rowCount;
            }
            $items[] = [
                'index' => $index,
                'rowCount' => $rowCount,
                'fields' => $stmt->fields(),
                'command' => $stmt->statusMessage(),
                'lastId' => $stmt->lastInsertId(),
            ];
            $index++;
        }
        return [
            'items' => $items,
            'totalRowCount' => $totalRowCount,
        ];
    }

    /**
     * @return array{items: array<int, array{index: int, rowCount: int, fields: array, command: string, lastId: int|false}>, totalRowCount: int}
     */
    public function queryBatch(string $sql, iterable $batchParams): array
    {
        return $this->executeBatch($sql, $batchParams);
    }

    /**
     * @return array<int, array{rows: array, rowCount: int, fields: array, command: string, lastId: int|false}>
     */
    public function queryMulti(string $sql, array $params = []): array
    {
        $stmt = $this->prepare($sql);
        $stmt->execute($params);
        $results = [];
        while (true) {
            $rows = $stmt->fetchAll(\PDO::FETCH_ASSOC);
            $results[] = [
                'rows' => $rows,
                'rowCount' => $stmt->rowCount(),
                'fields' => $stmt->fields(),
                'command' => $stmt->statusMessage(),
                'lastId' => $stmt->lastInsertId(),
            ];
            if (!$stmt->nextRowset()) {
                break;
            }
        }
        return $results;
    }

    /**
     * @return array<int, array{rows: array, rowCount: int, fields: array, command: string, lastId: int|false}>
     */
    public function executeMulti(string $sql, array $params = []): array
    {
        return $this->queryMulti($sql, $params);
    }

    /**
     * @return array<int, array{0: int}>
     */
    public function executeWithGeneratedKeys(string $sql, array $params = []): array
    {
        $stmt = $this->prepare($sql);
        $stmt->execute($params);
        do {
            $stmt->fetchAll(\PDO::FETCH_ASSOC);
        } while ($stmt->nextRowset());
        return $stmt->getGeneratedKeys();
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function queryMetadata(string $collectionName = 'tables'): Statement
    {
        $normalizedCollection = $this->normalizeMetadataCollection($collectionName);
        return $this->query(Metadata::resolveCollectionQuery($normalizedCollection));
    }

    /**
     * @param array<string, mixed> $restrictions
     * @return array<int, array<string, mixed>>
     */
    public function getSchema(string $collectionName = 'tables', array $restrictions = []): array
    {
        $normalizedCollection = $this->normalizeMetadataCollection($collectionName);
        $statement = $this->queryMetadata($normalizedCollection);
        $rows = $statement->fetchAll(\PDO::FETCH_ASSOC);
        /** @var array<int, array<string, mixed>> $rows */
        $rows = Metadata::filterRowsForCollectionFamily($rows, $normalizedCollection);
        if ($restrictions !== []) {
            /** @var array<int, array<string, mixed>> $rows */
            $rows = Metadata::filterRowsByRestrictions($rows, $restrictions, $normalizedCollection);
        }
        if ($normalizedCollection === 'schemas' && $this->config->metadataExpandSchemaParents) {
            /** @var array<int, array<string, mixed>> $rows */
            $rows = Metadata::expandSchemaMetadataRows($rows);
        }
        return $rows;
    }

    /**
     * @param array<string, mixed> $restrictions
     * @return array{database: ?string, schemas: array<int, array{name: string, path: string, terminal: bool, children: array}>}
     */
    public function getSchemaTree(?bool $expandParents = null, ?string $database = null, array $restrictions = []): array
    {
        $rows = $this->getSchema('schemas', $restrictions);
        return Metadata::buildMetadataSchemaTree(
            $rows,
            $expandParents ?? ($this->config->metadataExpandSchemaParents === true),
            $database ?? $this->config->database
        );
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function schemas(?string $catalog = null): array
    {
        return $this->getSchema('schemas', $this->metadataRestrictions(['catalog' => $catalog]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function tables(?string $schema = null, ?string $table = null, ?string $type = null): array
    {
        return $this->getSchema('tables', $this->metadataRestrictions(['schema' => $schema, 'table' => $table, 'type' => $type]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function columns(?string $schema = null, ?string $table = null, ?string $column = null, ?string $type = null): array
    {
        return $this->getSchema('columns', $this->metadataRestrictions(['schema' => $schema, 'table' => $table, 'column' => $column, 'type' => $type]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function indexes(?string $schema = null, ?string $table = null, ?string $index = null): array
    {
        return $this->getSchema('indexes', $this->metadataRestrictions(['schema' => $schema, 'table' => $table, 'index' => $index]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function indexColumns(?string $schema = null, ?string $table = null, ?string $index = null, ?string $column = null): array
    {
        return $this->getSchema('index_columns', $this->metadataRestrictions(['schema' => $schema, 'table' => $table, 'index' => $index, 'column' => $column]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function constraints(?string $schema = null, ?string $table = null, ?string $constraint = null): array
    {
        return $this->getSchema('constraints', $this->metadataRestrictions(['schema' => $schema, 'table' => $table, 'constraint' => $constraint]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function catalogs(?string $catalog = null): array
    {
        return $this->getSchema('catalogs', $this->metadataRestrictions(['catalog' => $catalog]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function primaryKeys(?string $catalog = null, ?string $schema = null, ?string $table = null, ?string $constraint = null): array
    {
        return $this->getSchema('primary_keys', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'table' => $table, 'constraint' => $constraint]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function foreignKeys(?string $catalog = null, ?string $schema = null, ?string $table = null, ?string $constraint = null): array
    {
        return $this->getSchema('foreign_keys', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'table' => $table, 'constraint' => $constraint]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function procedures(?string $catalog = null, ?string $schema = null, ?string $procedure = null): array
    {
        return $this->getSchema('procedures', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'procedure' => $procedure]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function functions(?string $catalog = null, ?string $schema = null, ?string $function = null): array
    {
        return $this->getSchema('functions', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'function' => $function]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function routines(?string $catalog = null, ?string $schema = null, ?string $routine = null): array
    {
        return $this->getSchema('routines', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'routine' => $routine]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function tablePrivileges(?string $catalog = null, ?string $schema = null, ?string $table = null): array
    {
        return $this->getSchema('table_privileges', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'table' => $table]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function columnPrivileges(?string $catalog = null, ?string $schema = null, ?string $table = null, ?string $column = null): array
    {
        return $this->getSchema('column_privileges', $this->metadataRestrictions(['catalog' => $catalog, 'schema' => $schema, 'table' => $table, 'column' => $column]));
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function typeInfo(?string $type = null): array
    {
        return $this->getSchema('type_info', $this->metadataRestrictions(['type' => $type]));
    }

    public function getSessionSchema(): ?string
    {
        $normalized = $this->normalizeSessionSchema($this->config->schema);
        return $normalized === '' ? null : $normalized;
    }

    public function setSessionSchema(?string $schema): void
    {
        $normalized = $this->normalizeSessionSchema($schema);
        $current = $this->normalizeSessionSchema($this->config->schema);
        if ($normalized === $current) {
            return;
        }
        $this->config->schema = $normalized ?? '';
        if (!$this->connected) {
            return;
        }
        $statement = $this->buildSchemaStatement($normalized ?? self::DEFAULT_SESSION_SCHEMA);
        if ($statement === '') {
            return;
        }
        $this->executeSimple($statement);
    }

    public function exec(string $statement): int|false
    {
        try {
            $this->sendSimpleQuery($statement, 0);
            $stream = new ResultStream($this);
            while ($stream->readRow() !== null) {
                // Drain all rows so command-complete rowsAffected is finalized.
            }
            $this->noteLastInsertId($stream->lastInsertId());
            return max(0, $stream->rowsAffected());
        } catch (\Throwable $ex) {
            $this->recordError($ex);
            return false;
        }
    }

    public function beginTransaction(): bool
    {
        return $this->beginTransactionEx();
    }

    public function beginTransactionEx(array $options = []): bool
    {
        $readCommittedModeProvided = array_key_exists('read_committed_mode', $options);
        $isolationLevel = $options['isolation_level'] ?? Protocol::ISOLATION_READ_COMMITTED;
        $flags = 0;
        if (array_key_exists('isolation_level', $options)) {
            $flags |= Protocol::TXN_FLAG_HAS_ISOLATION;
        }
        if ($readCommittedModeProvided) {
            if (
                array_key_exists('isolation_level', $options) &&
                $isolationLevel !== Protocol::ISOLATION_READ_UNCOMMITTED &&
                $isolationLevel !== Protocol::ISOLATION_READ_COMMITTED
            ) {
                throw new ScratchBirdNotSupportedException(
                    'read_committed_mode requires a READ COMMITTED isolation alias',
                    '0A000'
                );
            }
            $flags |= Protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE;
            if (!array_key_exists('isolation_level', $options)) {
                $isolationLevel = Protocol::ISOLATION_READ_COMMITTED;
                $flags |= Protocol::TXN_FLAG_HAS_ISOLATION;
            }
        }
        if (array_key_exists('access_mode', $options)) {
            $flags |= Protocol::TXN_FLAG_HAS_ACCESS;
        }
        if (array_key_exists('deferrable', $options)) {
            $flags |= Protocol::TXN_FLAG_HAS_DEFERRABLE;
        }
        if (array_key_exists('wait', $options)) {
            $flags |= Protocol::TXN_FLAG_HAS_WAIT;
        }
        if (array_key_exists('timeout_ms', $options)) {
            $flags |= Protocol::TXN_FLAG_HAS_TIMEOUT;
        }
        if (array_key_exists('autocommit_mode', $options)) {
            $flags |= Protocol::TXN_FLAG_HAS_AUTOCOMMIT;
        }
        if ($this->explicitTransaction) {
            throw new ScratchBirdTransactionException('Transaction already active', '25001');
        }
        $payload = Protocol::buildTxnBeginPayload(
            $flags,
            $options['conflict_action'] ?? 0,
            $options['autocommit_mode'] ?? 0,
            $isolationLevel,
            $options['access_mode'] ?? 0,
            !empty($options['deferrable']) ? 1 : 0,
            !empty($options['wait']) ? 1 : 0,
            $options['timeout_ms'] ?? 0,
            $readCommittedModeProvided
                ? (int) $options['read_committed_mode']
                : Protocol::READ_COMMITTED_MODE_DEFAULT
        );
        $this->sendMessage(Protocol::MSG_TXN_BEGIN, $payload, 0, false);
        $this->drainUntilReady();
        $this->explicitTransaction = true;
        return true;
    }

    public function inTransaction(): bool
    {
        return $this->inTransaction;
    }

    public function commit(): bool
    {
        $this->requireActiveTransaction('commit');
        $payload = Protocol::buildTxnCommitPayload(0);
        $this->sendMessage(Protocol::MSG_TXN_COMMIT, $payload, 0, false);
        $this->drainUntilReady();
        $this->explicitTransaction = false;
        $this->drainImmediateReopenBoundary();
        return true;
    }

    public function rollBack(): bool
    {
        $this->requireActiveTransaction('rollback');
        $payload = Protocol::buildTxnRollbackPayload(0);
        $this->sendMessage(Protocol::MSG_TXN_ROLLBACK, $payload, 0, false);
        $this->drainUntilReady();
        $this->explicitTransaction = false;
        $this->drainImmediateReopenBoundary();
        return true;
    }

    public function supportsPreparedTransactions(): bool
    {
        return true;
    }

    public function supportsDormantReattach(): bool
    {
        return false;
    }

    public function prepareTransaction(string $gid): bool
    {
        return $this->executePreparedTransactionControl(
            'prepare_transaction',
            $this->buildPreparedTransactionSql('PREPARE TRANSACTION', $gid)
        );
    }

    public function commitPrepared(string $gid): bool
    {
        return $this->executePreparedTransactionControl(
            'commit_prepared',
            $this->buildPreparedTransactionSql('COMMIT PREPARED', $gid)
        );
    }

    public function rollbackPrepared(string $gid): bool
    {
        return $this->executePreparedTransactionControl(
            'rollback_prepared',
            $this->buildPreparedTransactionSql('ROLLBACK PREPARED', $gid)
        );
    }

    public function detachToDormant(): never
    {
        throw new ScratchBirdNotSupportedException(
            'dormant detach/reattach is not yet exposed by the public PHP driver surface',
            '0A000'
        );
    }

    public function reattachDormant(string $dormantId, ?string $authToken = null): never
    {
        unset($dormantId, $authToken);
        throw new ScratchBirdNotSupportedException(
            'dormant detach/reattach is not yet exposed by the public PHP driver surface',
            '0A000'
        );
    }

    public function savepoint(string $name): void
    {
        $this->requireActiveTransaction('savepoint');
        $name = $this->normalizeSavepointName($name);
        $payload = Protocol::buildTxnSavepointPayload($name);
        $this->sendMessage(Protocol::MSG_TXN_SAVEPOINT, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function releaseSavepoint(string $name): void
    {
        $this->requireActiveTransaction('release savepoint');
        $name = $this->normalizeSavepointName($name);
        $payload = Protocol::buildTxnReleasePayload($name);
        $this->sendMessage(Protocol::MSG_TXN_RELEASE, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function rollbackToSavepoint(string $name): void
    {
        $this->requireActiveTransaction('rollback to savepoint');
        $name = $this->normalizeSavepointName($name);
        $payload = Protocol::buildTxnRollbackToPayload($name);
        $this->sendMessage(Protocol::MSG_TXN_ROLLBACK_TO, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function setOption(string $name, string $value): void
    {
        $payload = Protocol::buildSetOptionPayload($name, $value);
        $this->sendMessage(Protocol::MSG_SET_OPTION, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function ping(): void
    {
        $this->sendMessage(Protocol::MSG_PING, '', 0, false);
        while (true) {
            [$type, , $payload] = $this->receive();
            if ($this->handleAsyncMessage($type, $payload)) {
                continue;
            }
            if ($type === Protocol::MSG_PONG || $type === Protocol::MSG_READY) {
                if ($type === Protocol::MSG_READY) {
                    [$status, $txnId] = Protocol::parseReady($payload);
                    $this->applyRuntimeReadyState($status, $txnId);
                }
                return;
            }
            if ($type === Protocol::MSG_ERROR) {
                throw $this->buildQueryException($payload);
            }
        }
    }

    public function subscribe(string $channel, int $subType = Protocol::SUB_TYPE_CHANNEL, string $filterExpr = ''): void
    {
        $payload = Protocol::buildSubscribePayload($subType, $channel, $filterExpr);
        $this->sendMessage(Protocol::MSG_SUBSCRIBE, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function unsubscribe(string $channel): void
    {
        $payload = Protocol::buildUnsubscribePayload($channel);
        $this->sendMessage(Protocol::MSG_UNSUBSCRIBE, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function executeSblr(int $hash, ?string $bytecode = null, array $params = []): ResultStream
    {
        return $this->withResilience('sblr_execute', null, function () use ($hash, $bytecode, $params): ResultStream {
            $paramValues = [];
            foreach ($params as $param) {
                $encoded = TypeCodec::encodeParam($param);
                $paramValues[] = $encoded['param'];
            }
            $payload = Protocol::buildSblrExecutePayload($hash, $bytecode, $paramValues);
            $this->sendMessage(Protocol::MSG_SBLR_EXECUTE, $payload, 0, false);
            $this->sendMessage(Protocol::MSG_SYNC, '', 0, false);
            return new ResultStream($this);
        });
    }

    public function streamControl(int $controlType, int $windowSize, int $timeoutMs): void
    {
        $payload = Protocol::buildStreamControlPayload($controlType, $windowSize, $timeoutMs);
        $this->sendMessage(Protocol::MSG_STREAM_CONTROL, $payload, 0, false);
    }

    public function attachCreate(string $emulationMode, string $dbName): void
    {
        $payload = Protocol::buildAttachCreatePayload($emulationMode, $dbName);
        $this->sendMessage(Protocol::MSG_ATTACH_CREATE, $payload, 0, false);
        $this->drainUntilReady();
    }

    public function attachDetach(): void
    {
        $this->sendMessage(Protocol::MSG_ATTACH_DETACH, '', 0, false);
        $this->drainUntilReady();
    }

    public function attachList(): ResultStream
    {
        $this->sendMessage(Protocol::MSG_ATTACH_LIST, '', 0, false);
        $this->sendMessage(Protocol::MSG_SYNC, '', 0, false);
        return new ResultStream($this);
    }

    public function onNotification(callable $handler): void
    {
        $this->notificationHandlers[] = $handler;
    }

    public function lastPlan(): ?array
    {
        return $this->lastPlan;
    }

    public function lastSblr(): ?array
    {
        return $this->lastSblr;
    }

    public function lastInsertId(?string $name = null): string|false
    {
        if (!$this->hasLastInsertId) {
            return false;
        }
        return (string)$this->lastInsertIdValue;
    }

    public function noteLastInsertId(int $lastInsertId): void
    {
        $this->hasLastInsertId = true;
        $this->lastInsertIdValue = $lastInsertId;
    }

    public function setAttribute(int $attribute, mixed $value): bool
    {
        $this->attributes[$attribute] = $value;
        return true;
    }

    public function getAttribute(int $attribute): mixed
    {
        return $this->attributes[$attribute] ?? null;
    }

    public function errorInfo(): array
    {
        return $this->lastError;
    }

    public function errorCode(): ?string
    {
        return $this->lastError[0] ?? null;
    }

    public function close(): void
    {
        if ($this->socket !== null) {
            fclose($this->socket);
            $this->socket = null;
        }
        $this->clearAbandonedSessionState();
        $this->connected = false;
        $this->hasLastInsertId = false;
        $this->lastInsertIdValue = 0;
        if ($this->keepaliveTracker !== null) {
            $this->keepaliveManager->unregister($this->connectionId);
            $this->keepaliveTracker = null;
        }
        if ($this->leakGuard !== null) {
            $this->leakGuard->release();
            $this->leakGuard = null;
        }
        $this->keepaliveManager->stop();
        $this->leakDetector->stop();
    }

    public function updateTxnId(int $txnId): void
    {
        $this->applyRuntimeTxnId($txnId);
    }

    public function updateReadyState(int $status, int $txnId): void
    {
        $this->applyRuntimeReadyState($status, $txnId);
    }

    private function clearAbandonedSessionState(): void
    {
        $this->attachmentId = str_repeat("\0", 16);
        $this->clearTransactionState();
        $this->sequence = 0;
        $this->lastQuerySequence = 0;
        $this->lastMaxRows = 0;
        $this->portalResumePending = false;
        $this->parameters = [];
        $this->lastPlan = null;
        $this->lastSblr = null;
        $this->markResolvedAuthContextDetached();
    }

    private function withResilience(string $operation, ?string $sql, callable $fn): mixed
    {
        if (isset($this->circuitBreaker) && !$this->circuitBreaker->allowRequest()) {
            throw new ScratchBirdConnectionException('Circuit breaker is OPEN', '08006');
        }
        if (isset($this->keepaliveTracker) && $this->keepaliveTracker !== null && $this->keepaliveTracker->needsValidation()) {
            $this->ping();
            $this->keepaliveTracker->markActive();
        }

        $span = isset($this->telemetry) ? $this->telemetry->startSpan($operation) : null;
        if ($span && $sql && isset($this->telemetry)) {
            $span->withAttribute('db.statement', TelemetryCollector::sanitizeQuery($sql));
        }

        $success = false;
        try {
            $result = $fn();
            $success = true;
            if (isset($this->circuitBreaker)) {
                $this->circuitBreaker->recordSuccess();
            }
            if (isset($this->keepaliveTracker) && $this->keepaliveTracker !== null) {
                $this->keepaliveTracker->markActive();
            }
            return $result;
        } catch (\Throwable $ex) {
            if (isset($this->circuitBreaker)) {
                $this->circuitBreaker->recordFailure();
            }
            throw $ex;
        } finally {
            if (isset($this->telemetry)) {
                $this->telemetry->endSpan($span, $success);
            }
        }
    }

    public function executeQuery(string $sql, array $params = [], ?int $maxRows = null): ResultStream
    {
        return $this->withResilience('query', $sql, function () use ($sql, $params, $maxRows): ResultStream {
            $pageSize = $maxRows ?? $this->config->fetchSize;
            if (empty($params)) {
                $this->sendSimpleQuery($sql, $pageSize);
            } else {
                $this->sendExtendedQuery($sql, $params, $pageSize);
            }
            return new ResultStream($this);
        });
    }

    public function allowPortalResume(): void
    {
        $this->portalResumePending = true;
    }

    public function resumePortal(): void
    {
        if (!$this->portalResumePending) {
            throw new ScratchBirdException('portal resume requires explicit suspended state', '55000');
        }
        $this->portalResumePending = false;
        $execPayload = Protocol::buildExecutePayload('', $this->lastMaxRows);
        $this->lastQuerySequence = $this->sendMessage(Protocol::MSG_EXECUTE, $execPayload, 0, false);
    }

    public function cancel(): void
    {
        $payload = Protocol::buildCancelPayload(0, $this->lastQuerySequence);
        $this->sendMessage(Protocol::MSG_CANCEL, $payload, Protocol::MSG_FLAG_URGENT, false);
    }

    public function sendMessage(int $type, string $payload, int $flags = 0, bool $forceZero = false): int
    {
        if ($this->socket === null) {
            throw new ScratchBirdConnectionException('Connection not open', '08006');
        }
        if ($this->config->socketTimeoutMs > 0) {
            stream_set_timeout($this->socket, 0, $this->config->socketTimeoutMs * 1000);
        }
        $sequence = $this->sequence++;
        $attachmentId = $forceZero ? str_repeat("\0", 16) : $this->attachmentId;
        $txnId = $forceZero ? 0 : $this->txnId;
        $payload = Protocol::encodeMessage($type, $payload, $flags, $sequence, $attachmentId, $txnId);
        $total = 0;
        $length = strlen($payload);
        while ($total < $length) {
            $written = fwrite($this->socket, substr($payload, $total));
            if ($written === false || $written === 0) {
                throw new ScratchBirdConnectionException('Connection closed', '08006');
            }
            $total += $written;
        }
        return $sequence;
    }

    public function receive(): array
    {
        if ($this->socket === null) {
            throw new ScratchBirdConnectionException('Connection not open', '08006');
        }
        if ($this->config->socketTimeoutMs > 0) {
            stream_set_timeout($this->socket, 0, $this->config->socketTimeoutMs * 1000);
        }
        $header = $this->readExact(Protocol::HEADER_SIZE);
        [$type, $flags, $length, $sequence, $attachmentId, $txnId] = Protocol::decodeHeader($header);
        $payload = $length > 0 ? $this->readExact($length) : '';
        return [$type, $flags, $payload, $sequence, $attachmentId, $txnId];
    }

    public function handleAsyncMessage(int $type, string $payload): bool
    {
        if ($type === Protocol::MSG_PARAMETER_STATUS) {
            [$name, $value] = Protocol::parseParameterStatus($payload);
            $this->parameters[$name] = $value;
            if ($name === 'attachment_id') {
                $parsed = $this->parseUuidBytes($value);
                if ($parsed !== null) {
                    $this->attachmentId = $parsed;
                }
            }
            if ($name === 'current_txn_id') {
                $parsed = $this->parseUint64($value);
                if ($parsed !== null) {
                    $this->applyRuntimeTxnId($parsed);
                }
            }
            return true;
        }
        if ($type === Protocol::MSG_TXN_STATUS) {
            [$status, $txnId] = Protocol::parseTxnStatus($payload);
            if ($status === ord('T')) {
                $this->applyRuntimeTxnId($txnId);
            } else {
                $this->clearTransactionState();
            }
            return true;
        }
        if ($type === Protocol::MSG_NOTIFICATION) {
            $notice = Protocol::parseNotification($payload);
            foreach ($this->notificationHandlers as $handler) {
                $handler($notice);
            }
            return true;
        }
        if ($type === Protocol::MSG_QUERY_PLAN) {
            $this->lastPlan = Protocol::parseQueryPlan($payload);
            return true;
        }
        if ($type === Protocol::MSG_SBLR_COMPILED) {
            $this->lastSblr = Protocol::parseSblrCompiled($payload);
            return true;
        }
        return false;
    }

    public function drainUntilReady(): void
    {
        $pendingError = null;
        while (true) {
            [$type, , $payload] = $this->receive();
            if ($this->handleAsyncMessage($type, $payload)) {
                continue;
            }
            if ($type === Protocol::MSG_ERROR) {
                if ($pendingError === null) {
                    $pendingError = $this->buildQueryException($payload);
                }
                continue;
            }
            if ($type === Protocol::MSG_READY) {
                [$status, $txnId] = Protocol::parseReady($payload);
                $this->applyRuntimeReadyState($status, $txnId);
                if ($pendingError !== null) {
                    throw $pendingError;
                }
                return;
            }
        }
    }

    private function connect(): void
    {
        $this->resetResolvedAuthContext();
        $this->connectTransport(true, true);
        if ($this->config->frontDoorMode === 'manager_proxy') {
            $this->performManagerConnect();
        }
        $this->handshake();
        $this->applySchema();
        $this->connected = true;
    }

    private function probeAuthSurfaceInternal(): array
    {
        $this->resetResolvedAuthContext();
        $this->connectTransport(false, false);
        if ($this->config->frontDoorMode === 'manager_proxy') {
            return $this->probeManagerAuthSurface();
        }
        return $this->probeDirectAuthSurface();
    }

    private function connectTransport(bool $requireIdentity, bool $requireManagerToken): void
    {
        $this->config->protocol = $this->normalizeNativeProtocol($this->config->protocol ?? 'native');
        $this->config->frontDoorMode = $this->normalizeFrontDoorMode($this->config->frontDoorMode ?? 'direct');
        $this->resolvedAuthContext['front_door_mode'] = $this->config->frontDoorMode;
        if ($requireIdentity && ($this->config->user === '' || $this->config->database === '')) {
            throw new ScratchBirdConnectionException('user and database are required', '08001');
        }
        if ($requireManagerToken && $this->config->frontDoorMode === 'manager_proxy' && $this->config->managerAuthToken === '') {
            throw new ScratchBirdConnectionException('manager_proxy mode requires manager_auth_token', '08001');
        }
        $transport = $this->normalizeTransport($this->config->transport ?? 'inet');
        if ($transport === 'embedded') {
            throw new ScratchBirdNotSupportedException(
                'embedded transport is not supported by the PHP driver; no ScratchBird C++ library boundary is exposed.',
                '0A000'
            );
        }
        if ($transport === 'ipc') {
            $this->connectIpcSocket();
            return;
        }
        $timeout = $this->config->connectTimeoutMs / 1000;
        $address = sprintf('tcp://%s:%d', $this->config->host, $this->config->port);
        $socket = @stream_socket_client($address, $errno, $errstr, $timeout);
        if ($socket === false) {
            throw new ScratchBirdConnectionException($errstr ?: 'Connection failed', '08001');
        }
        stream_set_blocking($socket, true);
        $this->socket = $socket;
        $this->applyTls();
    }

    private function connectIpcSocket(): void
    {
        if ($this->config->ipcPath === '') {
            throw new ScratchBirdConnectionException('ipc_path is required for local IPC transport', '08001');
        }
        if (!in_array('unix', stream_get_transports(), true)) {
            throw new ScratchBirdNotSupportedException(
                'Unix-domain socket IPC transport is not supported by this PHP runtime',
                '0A000'
            );
        }

        $timeout = $this->config->connectTimeoutMs / 1000;
        $address = 'unix://' . $this->config->ipcPath;
        $socket = @stream_socket_client($address, $errno, $errstr, $timeout);
        if ($socket === false) {
            throw new ScratchBirdConnectionException($errstr ?: 'IPC connection failed', '08001');
        }
        stream_set_blocking($socket, true);
        $this->socket = $socket;
    }

    private function resetResolvedAuthContext(): void
    {
        $this->resolvedAuthContext = self::defaultResolvedAuthContext(
            $this->config->frontDoorMode !== '' ? $this->config->frontDoorMode : 'direct'
        );
    }

    private function markResolvedAuthContextDetached(): void
    {
        $this->resolvedAuthContext['attached'] = false;
        $this->resolvedAuthContext['manager_authenticated'] = false;
    }

    private function normalizeNativeProtocol(string $value): string
    {
        $normalized = strtolower(trim($value));
        if ($normalized === '' ||
            $normalized === 'native' ||
            $normalized === 'scratchbird' ||
            $normalized === 'scratchbird-native' ||
            $normalized === 'scratchbird_native') {
            return 'native';
        }
        throw new ScratchBirdNotSupportedException(
            'Only protocol=native is supported; connect to the native parser listener/port.',
            '0A000'
        );
    }

    private function normalizeFrontDoorMode(string $value): string
    {
        $normalized = strtolower(trim($value));
        if ($normalized === '' || $normalized === 'direct') {
            return 'direct';
        }
        if ($normalized === 'manager_proxy' || $normalized === 'manager-proxy' || $normalized === 'managed') {
            return 'manager_proxy';
        }
        throw new ScratchBirdNotSupportedException('front_door_mode must be direct or manager_proxy.', '0A000');
    }

    private function normalizeTransport(string $value): string
    {
        $normalized = strtolower(trim($value));
        if (in_array($normalized, ['', 'inet', 'tcp', 'network'], true)) {
            return 'inet';
        }
        if (in_array($normalized, ['ipc', 'ipc_local', 'local_ipc', 'unix', 'unix_socket', 'uds'], true)) {
            return 'ipc';
        }
        if ($normalized === 'embedded') {
            return 'embedded';
        }
        throw new ScratchBirdNotSupportedException('transport must be inet, ipc, or embedded.', '0A000');
    }

    private function authMethodName(int $method): string
    {
        return match ($method) {
            Protocol::AUTH_OK => 'OK',
            Protocol::AUTH_PASSWORD => 'PASSWORD',
            Protocol::AUTH_MD5 => 'MD5',
            Protocol::AUTH_SCRAM_SHA256 => 'SCRAM_SHA_256',
            Protocol::AUTH_SCRAM_SHA512 => 'SCRAM_SHA_512',
            Protocol::AUTH_TOKEN => 'TOKEN',
            Protocol::AUTH_PEER => 'PEER',
            Protocol::AUTH_REATTACH => 'REATTACH',
            default => 'UNKNOWN',
        };
    }

    private function authPluginIdForMethod(int $method): string
    {
        return match ($method) {
            Protocol::AUTH_OK => 'scratchbird.auth.none',
            Protocol::AUTH_PASSWORD => 'scratchbird.auth.password_compat',
            Protocol::AUTH_MD5 => 'scratchbird.auth.md5_legacy',
            Protocol::AUTH_SCRAM_SHA256 => 'scratchbird.auth.scram_sha_256',
            Protocol::AUTH_SCRAM_SHA512 => 'scratchbird.auth.scram_sha_512',
            Protocol::AUTH_TOKEN => 'scratchbird.auth.authkey_token',
            Protocol::AUTH_PEER => 'scratchbird.auth.peer_uid',
            Protocol::AUTH_REATTACH => 'scratchbird.auth.reattach',
            default => '',
        };
    }

    private function authMethodExecutableLocally(int $method): bool
    {
        return in_array(
            $method,
            [
                Protocol::AUTH_PASSWORD,
                Protocol::AUTH_SCRAM_SHA256,
                Protocol::AUTH_SCRAM_SHA512,
                Protocol::AUTH_TOKEN,
            ],
            true
        );
    }

    private function authMethodBrokerRequired(int $method): bool
    {
        return in_array($method, [Protocol::AUTH_PEER], true);
    }

    private function additionalContinuationPossible(int $method): bool
    {
        return in_array(
            $method,
            [Protocol::AUTH_SCRAM_SHA256, Protocol::AUTH_SCRAM_SHA512, Protocol::AUTH_TOKEN],
            true
        );
    }

    private function describeAuthMethod(int $method): array
    {
        return [
            'method_code' => $method,
            'method_name' => $this->authMethodName($method),
            'plugin_id' => $this->authPluginIdForMethod($method),
            'executable_locally' => $this->authMethodExecutableLocally($method),
            'broker_required' => $this->authMethodBrokerRequired($method),
        ];
    }

    private function applyTls(): void
    {
        $mode = strtolower($this->config->sslMode ?: 'require');
        if ($mode === 'disable') {
            return;
        }
        $options = [
            'ssl' => [
                'crypto_method' => STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT,
                'verify_peer' => in_array($mode, ['verify-full', 'verify-ca', 'require'], true),
                'verify_peer_name' => $mode === 'verify-full',
            ],
        ];
        if ($this->config->sslRootCert) {
            $options['ssl']['cafile'] = $this->config->sslRootCert;
        }
        if ($this->config->sslCert && $this->config->sslKey) {
            $options['ssl']['local_cert'] = $this->config->sslCert;
            $options['ssl']['local_pk'] = $this->config->sslKey;
            if ($this->config->sslPassword) {
                $options['ssl']['passphrase'] = $this->config->sslPassword;
            }
        }
        stream_context_set_option($this->socket, $options);
        $result = @stream_socket_enable_crypto($this->socket, true, STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT);
        if ($result !== true) {
            throw new ScratchBirdConnectionException('TLS handshake failed', '08001');
        }
    }

    private function managerLpref(string $value): string
    {
        return pack('V', strlen($value)) . $value;
    }

    private function sendManagerFrame(int $type, string $payload): void
    {
        $frame = pack('V', self::MANAGER_PROTOCOL_MAGIC)
            . pack('v', self::MANAGER_PROTOCOL_VERSION)
            . chr($type)
            . chr(0)
            . pack('V', strlen($payload))
            . $payload;
        $total = 0;
        $length = strlen($frame);
        while ($total < $length) {
            $written = fwrite($this->socket, substr($frame, $total));
            if ($written === false || $written === 0) {
                throw new ScratchBirdConnectionException('Manager frame write failed', '08006');
            }
            $total += $written;
        }
    }

    private function recvManagerFrame(): array
    {
        $header = $this->readExact(self::MANAGER_HEADER_SIZE);
        $magic = unpack('V', substr($header, 0, 4))[1];
        if ($magic !== self::MANAGER_PROTOCOL_MAGIC) {
            throw new ScratchBirdConnectionException('Manager frame magic mismatch', '08P01');
        }
        $version = unpack('v', substr($header, 4, 2))[1];
        if ($version !== self::MANAGER_PROTOCOL_VERSION) {
            throw new ScratchBirdConnectionException('Manager frame version mismatch', '08P01');
        }
        $type = ord($header[6]);
        $length = unpack('V', substr($header, 8, 4))[1];
        if ($length > self::MANAGER_MAX_PAYLOAD_SIZE) {
            throw new ScratchBirdConnectionException('Manager payload too large', '08P01');
        }
        $payload = $length > 0 ? $this->readExact($length) : '';
        return [$type, $payload];
    }

    private function performManagerConnect(): void
    {
        if ($this->config->managerAuthToken === '') {
            throw new ScratchBirdConnectionException('manager_proxy mode requires manager_auth_token', '08001');
        }
        $managerUser = $this->config->managerUsername !== ''
            ? $this->config->managerUsername
            : ($this->config->user !== '' ? $this->config->user : 'admin');
        $managerDatabase = $this->config->managerDatabase !== '' ? $this->config->managerDatabase : $this->config->database;
        $managerProfile = $this->config->managerConnectionProfile !== '' ? $this->config->managerConnectionProfile : 'SBsql';
        $managerIntent = $this->config->managerClientIntent !== '' ? $this->config->managerClientIntent : 'SBsql';
        $managerFlags = $this->config->managerClientFlags & 0xFFFF;
        $authFastPath = $this->config->managerAuthFastPath !== false;

        $helloPayload = pack('vv', self::MCP_PROTOCOL_VERSION, $managerFlags);
        $this->sendManagerFrame(self::MCP_MSG_HELLO, $helloPayload);
        [$type] = $this->recvManagerFrame();
        if ($type !== self::MCP_MSG_STATUS_RESPONSE) {
            throw new ScratchBirdConnectionException('Expected MCP hello status response', '08P01');
        }

        $authStart = $this->managerLpref($managerUser)
            . chr(self::MCP_AUTH_METHOD_TOKEN);
        if ($authFastPath) {
            $token = $this->config->managerAuthToken;
            $authStart .= pack('V', strlen($token)) . $token;
        } else {
            $authStart .= pack('V', 0);
        }
        $this->sendManagerFrame(self::MCP_MSG_AUTH_START, $authStart);
        [$type, $payload] = $this->recvManagerFrame();
        if ($type === self::MCP_MSG_AUTH_CHALLENGE) {
            $token = $this->config->managerAuthToken;
            $this->sendManagerFrame(self::MCP_MSG_AUTH_CONTINUE, pack('V', strlen($token)) . $token);
            [$type, $payload] = $this->recvManagerFrame();
        }
        if ($type !== self::MCP_MSG_AUTH_RESPONSE) {
            throw new ScratchBirdConnectionException('Expected MCP auth response', '08P01');
        }
        if (strlen($payload) < (1 + 4 + 256)) {
            throw new ScratchBirdConnectionException('Truncated MCP auth response', '08P01');
        }
        if (ord($payload[0]) !== 0) {
            $err = rtrim(substr($payload, 5, 256), "\0");
            throw new ScratchBirdAuthException($err !== '' ? $err : 'MCP authentication failed', '28000');
        }

        $nonce = random_bytes(16);
        $dbConnect = 'MCP1'
            . $this->managerLpref($managerDatabase)
            . $this->managerLpref($managerProfile)
            . $this->managerLpref($managerIntent)
            . pack('v', strlen($nonce))
            . $nonce;
        $this->sendManagerFrame(self::MCP_MSG_DB_CONNECT, $dbConnect);
        [$type, $payload] = $this->recvManagerFrame();
        if ($type !== self::MCP_MSG_CONNECT_RESPONSE) {
            throw new ScratchBirdConnectionException('Expected MCP connect response', '08P01');
        }
        if (strlen($payload) < (1 + 2 + 2 + 16 + 64 + 32)) {
            throw new ScratchBirdConnectionException('Truncated MCP connect response', '08P01');
        }
        if (ord($payload[0]) !== 0) {
            $err = 'MCP database connect failed';
            $errOffset = 1 + 2 + 2 + 16 + 64 + 32;
            if (strlen($payload) >= $errOffset + 4) {
                $errLen = unpack('V', substr($payload, $errOffset, 4))[1];
                if (strlen($payload) >= $errOffset + 4 + $errLen) {
                    $err = substr($payload, $errOffset + 4, $errLen);
                }
            }
            throw new ScratchBirdAuthException($err, '28000');
        }
        $this->resolvedAuthContext['manager_authenticated'] = true;
    }

    private function probeManagerAuthSurface(): array
    {
        $managerUser = $this->config->managerUsername !== ''
            ? $this->config->managerUsername
            : ($this->config->user !== '' ? $this->config->user : 'admin');
        $managerFlags = $this->config->managerClientFlags & 0xFFFF;

        $helloPayload = pack('vv', self::MCP_PROTOCOL_VERSION, $managerFlags);
        $this->sendManagerFrame(self::MCP_MSG_HELLO, $helloPayload);
        [$type] = $this->recvManagerFrame();
        if ($type !== self::MCP_MSG_STATUS_RESPONSE) {
            throw new ScratchBirdConnectionException('Expected MCP hello status response', '08P01');
        }

        $authStart = $this->managerLpref($managerUser)
            . chr(self::MCP_AUTH_METHOD_TOKEN)
            . pack('V', 0);
        $this->sendManagerFrame(self::MCP_MSG_AUTH_START, $authStart);
        [$type] = $this->recvManagerFrame();
        if ($type !== self::MCP_MSG_AUTH_CHALLENGE
            && $type !== self::MCP_MSG_AUTH_RESPONSE
            && $type !== self::MCP_MSG_STATUS_RESPONSE) {
            throw new ScratchBirdConnectionException('Expected MCP auth challenge or auth response', '08P01');
        }

        return [
            'reachable' => true,
            'front_door_mode' => 'manager_proxy',
            'resolved_host' => $this->config->host,
            'resolved_port' => $this->config->port,
            'admitted_methods' => [$this->describeAuthMethod(Protocol::AUTH_TOKEN)],
            'required_method_code' => Protocol::AUTH_TOKEN,
            'required_method' => $this->authMethodName(Protocol::AUTH_TOKEN),
            'required_plugin_method_id' => $this->authPluginIdForMethod(Protocol::AUTH_TOKEN),
            'required_method_broker_required' => $this->authMethodBrokerRequired(Protocol::AUTH_TOKEN),
            'additional_continuation_possible' => true,
        ];
    }

    private function probeDirectAuthSurface(): array
    {
        $features = $this->buildStartupFeatures();
        $params = $this->buildStartupParams(false);
        $startup = Protocol::buildStartupPayload($features, $params);
        $this->sendMessage(Protocol::MSG_STARTUP, $startup, 0, true);

        while (true) {
            [$type, , $payload] = $this->receive();
            if ($type === Protocol::MSG_NEGOTIATE_VERSION || $type === Protocol::MSG_PARAMETER_STATUS) {
                continue;
            }
            if ($type === Protocol::MSG_AUTH_REQUEST) {
                [$method] = Protocol::parseAuthRequest($payload);
                return [
                    'reachable' => true,
                    'front_door_mode' => 'direct',
                    'resolved_host' => $this->config->host,
                    'resolved_port' => $this->config->port,
                    'admitted_methods' => [$this->describeAuthMethod($method)],
                    'required_method_code' => $method,
                    'required_method' => $this->authMethodName($method),
                    'required_plugin_method_id' => $this->authPluginIdForMethod($method),
                    'required_method_broker_required' => $this->authMethodBrokerRequired($method),
                    'additional_continuation_possible' => $this->additionalContinuationPossible($method),
                ];
            }
            if ($type === Protocol::MSG_AUTH_OK || $type === Protocol::MSG_READY) {
                return [
                    'reachable' => true,
                    'front_door_mode' => 'direct',
                    'resolved_host' => $this->config->host,
                    'resolved_port' => $this->config->port,
                    'admitted_methods' => [],
                    'required_method_code' => Protocol::AUTH_OK,
                    'required_method' => $this->authMethodName(Protocol::AUTH_OK),
                    'required_plugin_method_id' => $this->authPluginIdForMethod(Protocol::AUTH_OK),
                    'required_method_broker_required' => false,
                    'additional_continuation_possible' => false,
                ];
            }
            if ($type === Protocol::MSG_ERROR) {
                throw $this->buildQueryException($payload);
            }
        }
    }

    private function applySchema(): void
    {
        $schema = $this->normalizeSessionSchema($this->config->schema);
        if ($schema === null) {
            return;
        }
        $statement = $this->buildSchemaStatement($schema);
        if ($statement === '') {
            return;
        }
        $this->executeSimple($statement);
    }

    private function buildSchemaStatement(string $schema): string
    {
        $schema = trim($schema);
        if ($schema === '') {
            return '';
        }
        if (str_contains($schema, ',')) {
            $parts = array_filter(array_map('trim', explode(',', $schema)));
            if (!$parts) {
                return '';
            }
            $quoted = array_map([$this, 'quoteIdentifier'], $parts);
            return 'SET SEARCH_PATH TO ' . implode(', ', $quoted);
        }
        return 'SET SCHEMA ' . $this->quoteIdentifier($schema);
    }

    private function quoteIdentifier(string $name): string
    {
        return '"' . str_replace('"', '""', $name) . '"';
    }

    private function buildStartupFeatures(): int
    {
        $features = 0;
        if (strtolower($this->config->compression) === 'zstd') {
            $features |= Protocol::FEATURE_COMPRESSION;
        }
        if ($this->config->binaryTransfer) {
            $features |= Protocol::FEATURE_STREAMING;
        }
        return $features;
    }

    private function buildStartupParams(bool $requireIdentity = true): array
    {
        $params = [
            'client_flags' => (string)$this->config->connectClientFlags,
        ];
        if ($requireIdentity || $this->config->database !== '') {
            $params['database'] = $this->config->database;
        }
        if ($requireIdentity || $this->config->user !== '') {
            $params['user'] = $this->config->user;
        }
        if ($this->config->role !== '') {
            $params['role'] = $this->config->role;
        }
        if ($this->config->applicationName !== '') {
            $params['application_name'] = $this->config->applicationName;
        }
        if ($this->config->authMethodId !== '') {
            if (!str_starts_with($this->config->authMethodId, 'scratchbird.auth.')) {
                throw new ScratchBirdAuthException('invalid auth_method_id namespace', '28000');
            }
            $params['auth_method_id'] = $this->config->authMethodId;
        }
        if ($this->config->authMethodPayload !== '') {
            $params['auth_method_payload'] = $this->config->authMethodPayload;
        }
        if ($this->config->authPayloadJson !== '') {
            $params['auth_payload_json'] = $this->config->authPayloadJson;
        }
        if ($this->config->authPayloadB64 !== '') {
            $params['auth_payload_b64'] = $this->config->authPayloadB64;
        }
        if ($this->config->authProviderProfile !== '') {
            $params['auth_provider_profile'] = $this->config->authProviderProfile;
        }
        if ($this->config->authRequiredMethods !== '') {
            $params['auth_required_methods'] = $this->config->authRequiredMethods;
        }
        if ($this->config->authForbiddenMethods !== '') {
            $params['auth_forbidden_methods'] = $this->config->authForbiddenMethods;
        }
        if ($this->config->authRequireChannelBinding) {
            $params['auth_require_channel_binding'] = '1';
        }
        if ($this->config->workloadIdentityToken !== '') {
            $params['workload_identity_token'] = $this->config->workloadIdentityToken;
        }
        if ($this->config->proxyPrincipalAssertion !== '') {
            $params['proxy_principal_assertion'] = $this->config->proxyPrincipalAssertion;
        }
        return $params;
    }

    private function handshake(): void
    {
        $features = $this->buildStartupFeatures();
        $params = $this->buildStartupParams(true);
        $startup = Protocol::buildStartupPayload($features, $params);
        $this->sendMessage(Protocol::MSG_STARTUP, $startup, 0, true);

        $scram = null;

        while (true) {
            [$type, , $payload, , $attachmentId, $txnId] = $this->receive();
            switch ($type) {
                case Protocol::MSG_NEGOTIATE_VERSION:
                    continue 2;
                case Protocol::MSG_AUTH_REQUEST:
                    [$method, $data] = Protocol::parseAuthRequest($payload);
                    $this->resolvedAuthContext['resolved_auth_method'] = $this->authMethodName($method);
                    $this->resolvedAuthContext['resolved_auth_plugin_id'] = $this->authPluginIdForMethod($method);
                    if ($method === Protocol::AUTH_OK) {
                        continue 2;
                    }
                    if ($method === Protocol::AUTH_PASSWORD) {
                        $this->sendMessage(Protocol::MSG_AUTH_RESPONSE, $this->config->password ?? '', 0, true);
                        continue 2;
                    }
                    if ($method === Protocol::AUTH_SCRAM_SHA256 || $method === Protocol::AUTH_SCRAM_SHA512) {
                        if ($scram === null) {
                            $scram = new Scram(
                                $this->config->user,
                                $method === Protocol::AUTH_SCRAM_SHA512 ? Scram::ALGORITHM_SHA512 : Scram::ALGORITHM_SHA256
                            );
                        }
                        $clientFirst = $scram->clientFirstMessage();
                        $this->sendMessage(Protocol::MSG_AUTH_RESPONSE, $clientFirst, 0, true);
                        continue 2;
                    }
                    if ($method === Protocol::AUTH_TOKEN) {
                        $this->sendMessage(Protocol::MSG_AUTH_RESPONSE, $this->resolveTokenAuthPayload(), 0, true);
                        continue 2;
                    }
                    if ($method === Protocol::AUTH_MD5 || $method === Protocol::AUTH_PEER || $method === Protocol::AUTH_REATTACH) {
                        throw new ScratchBirdNotSupportedException(
                            $this->authMethodName($method) . ' authentication is not supported by the PHP driver',
                            '0A000'
                        );
                    }
                    throw new ScratchBirdAuthException('Unsupported auth method', '28000');
                case Protocol::MSG_AUTH_CONTINUE:
                    [$method, , $data] = Protocol::parseAuthContinue($payload);
                    if (($method === Protocol::AUTH_SCRAM_SHA256 || $method === Protocol::AUTH_SCRAM_SHA512) && $scram !== null) {
                        $clientFinal = $scram->handleServerFirst($this->config->password, $data);
                        $this->sendMessage(Protocol::MSG_AUTH_RESPONSE, $clientFinal, 0, true);
                        continue 2;
                    }
                    if ($method === Protocol::AUTH_TOKEN) {
                        $this->sendMessage(Protocol::MSG_AUTH_RESPONSE, $this->resolveTokenAuthPayload(), 0, true);
                        continue 2;
                    }
                    throw new ScratchBirdAuthException('Unsupported auth continue', '28000');
                case Protocol::MSG_AUTH_OK:
                    [, $serverInfo] = Protocol::parseAuthOk($payload);
                    $this->attachmentId = $attachmentId;
                    $this->applyRuntimeTxnId($txnId);
                    if ($scram !== null && $serverInfo !== '' && str_starts_with($serverInfo, 'v=')) {
                        $scram->verifyServerFinal($serverInfo);
                    }
                    continue 2;
                case Protocol::MSG_PARAMETER_STATUS:
                    [$name, $value] = Protocol::parseParameterStatus($payload);
                    $this->parameters[$name] = $value;
                    continue 2;
                case Protocol::MSG_READY:
                    [$status, $txnId] = Protocol::parseReady($payload);
                    $this->applyRuntimeReadyState($status, $txnId);
                    $this->resolvedAuthContext['attached'] = true;
                    return;
                case Protocol::MSG_ERROR:
                    throw $this->buildQueryException($payload);
                default:
                    continue 2;
            }
        }
    }

    private function sendSimpleQuery(string $sql, int $maxRows): void
    {
        $flags = $this->config->binaryTransfer ? self::QUERY_FLAG_BINARY_RESULT : 0;
        $payload = Protocol::buildQueryPayload($sql, $flags, $maxRows, 0);
        $this->lastMaxRows = max(0, $maxRows);
        $this->lastQuerySequence = $this->sendMessage(Protocol::MSG_QUERY, $payload, 0, false);
    }

    private function sendExtendedQuery(string $sql, array $params, int $maxRows): void
    {
        $paramValues = [];
        $paramTypes = [];
        foreach ($params as $param) {
            $encoded = TypeDecoder::encodeParam($param);
            $paramValues[] = $encoded['param'];
            $paramTypes[] = $encoded['oid'];
        }
        $parsePayload = Protocol::buildParsePayload('', $sql, $paramTypes);
        $this->sendMessage(Protocol::MSG_PARSE, $parsePayload, 0, false);
        $paramCount = $this->describeStatement('');
        if ($paramCount >= 0 && $paramCount !== count($paramTypes)) {
            throw new ScratchBirdException('parameter count mismatch', '07001');
        }

        $resultFormats = $this->config->binaryTransfer ? [TypeDecoder::FORMAT_BINARY] : [];
        $bindPayload = Protocol::buildBindPayload('', '', $paramValues, $resultFormats);
        $this->sendMessage(Protocol::MSG_BIND, $bindPayload, 0, false);

        $execPayload = Protocol::buildExecutePayload('', $maxRows);
        $this->lastMaxRows = max(0, $maxRows);
        $this->lastQuerySequence = $this->sendMessage(Protocol::MSG_EXECUTE, $execPayload, 0, false);
        if ($maxRows === 0) {
            $this->sendMessage(Protocol::MSG_SYNC, '', 0, false);
        }
    }

    private function describeStatement(string $name): int
    {
        $payload = Protocol::buildDescribePayload(ord('S'), $name);
        $this->sendMessage(Protocol::MSG_DESCRIBE, $payload, 0, false);
        $this->sendMessage(Protocol::MSG_SYNC, '', 0, false);
        $paramCount = -1;
        while (true) {
            [$type, , $payload] = $this->receive();
            if ($type === Protocol::MSG_ERROR) {
                throw $this->buildQueryException($payload);
            }
            if ($type === Protocol::MSG_PARAMETER_DESCRIPTION) {
                $paramCount = count(Protocol::parseParameterDescription($payload));
                continue;
            }
            if ($type === Protocol::MSG_PARAMETER_STATUS) {
                [$name, $value] = Protocol::parseParameterStatus($payload);
                $this->parameters[$name] = $value;
                continue;
            }
            if ($type === Protocol::MSG_READY) {
                [$status, $txnId] = Protocol::parseReady($payload);
                $this->applyRuntimeReadyState($status, $txnId);
                return $paramCount;
            }
        }
    }

    private function executeSimple(string $sql): bool
    {
        try {
            $this->sendSimpleQuery($sql, 0);
            $this->drainUntilReady();
            return true;
        } catch (\Throwable $ex) {
            $this->recordError($ex);
            return false;
        }
    }

    private function parseUuidBytes(string $value): ?string
    {
        $hex = strtolower(str_replace('-', '', trim($value)));
        if (!preg_match('/^[0-9a-f]{32}$/', $hex)) {
            return null;
        }
        $bin = @hex2bin($hex);
        if ($bin === false || strlen($bin) !== 16) {
            return null;
        }
        return $bin;
    }

    private function parseUint64(string $value): ?int
    {
        $trimmed = trim($value);
        if ($trimmed === '') {
            return null;
        }
        if (!ctype_digit($trimmed)) {
            return null;
        }
        return (int) $trimmed;
    }

    private function resolveTokenAuthPayload(): string
    {
        if ($this->config->authToken !== '') {
            return $this->config->authToken;
        }
        if ($this->config->authMethodPayload !== '') {
            return $this->config->authMethodPayload;
        }
        if ($this->config->authPayloadB64 !== '') {
            $decoded = base64_decode(trim($this->config->authPayloadB64), true);
            if ($decoded === false) {
                throw new ScratchBirdDataException('invalid auth_payload_b64 encoding', '22023');
            }
            return $decoded;
        }
        if ($this->config->authPayloadJson !== '') {
            return $this->config->authPayloadJson;
        }
        if ($this->config->workloadIdentityToken !== '') {
            return $this->config->workloadIdentityToken;
        }
        if ($this->config->proxyPrincipalAssertion !== '') {
            return $this->config->proxyPrincipalAssertion;
        }
        throw new ScratchBirdAuthException(
            'TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, auth_payload_b64, workload_identity_token, or proxy_principal_assertion',
            '28000'
        );
    }

    private function applyRuntimeTxnId(int $txnId): void
    {
        $this->txnId = $txnId;
        if ($txnId !== 0) {
            $this->runtimeTxnActive = true;
            $this->inTransaction = true;
        }
    }

    private function applyRuntimeReadyState(int $status, int $txnId): void
    {
        $this->txnId = $txnId;
        if ($status !== 0) {
            // READY is authoritative for native MGA activity. Live listeners
            // also publish current_txn_id, so ScratchBird remains always in a
            // transaction even as COMMIT / ROLLBACK reopen the next
            // boundary.
            $this->runtimeTxnActive = true;
            $this->inTransaction = true;
            return;
        }
        $this->clearTransactionState();
    }

    private function clearTransactionState(): void
    {
        $this->txnId = 0;
        $this->runtimeTxnActive = false;
        $this->explicitTransaction = false;
        $this->inTransaction = false;
    }

    private function hasActiveTransaction(): bool
    {
        return $this->runtimeTxnActive || $this->txnId !== 0;
    }

    private function canAdoptFreshNativeBoundary(array $options): bool
    {
        $isolationLevel = $options['isolation_level'] ?? Protocol::ISOLATION_READ_COMMITTED;
        return $isolationLevel === Protocol::ISOLATION_READ_COMMITTED
            && (!array_key_exists('read_committed_mode', $options)
                || (int) $options['read_committed_mode'] === Protocol::READ_COMMITTED_MODE_DEFAULT)
            && !array_key_exists('access_mode', $options)
            && !array_key_exists('deferrable', $options)
            && !array_key_exists('wait', $options)
            && !array_key_exists('timeout_ms', $options)
            && !array_key_exists('autocommit_mode', $options)
            && (($options['conflict_action'] ?? 0) === 0);
    }

    private function requireActiveTransaction(string $operation): void
    {
        if (!$this->hasActiveTransaction()) {
            throw new ScratchBirdTransactionException("No active transaction for {$operation}", '25000');
        }
    }

    private function drainImmediateReopenBoundary(): void
    {
        if ($this->socket === null) {
            return;
        }
        while (true) {
            $read = [$this->socket];
            $write = [];
            $except = [];
            $ready = @stream_select($read, $write, $except, 0, 0);
            if ($ready === false || $ready === 0) {
                return;
            }
            [$type, , $payload] = $this->receive();
            if ($this->handleAsyncMessage($type, $payload)) {
                continue;
            }
            if ($type === Protocol::MSG_ERROR) {
                throw $this->buildQueryException($payload);
            }
            if ($type === Protocol::MSG_READY) {
                [$status, $txnId] = Protocol::parseReady($payload);
                $this->applyRuntimeReadyState($status, $txnId);
                $this->portalResumePending = false;
                continue;
            }
            throw new ScratchBirdException('unexpected message while draining immediate reopen boundary', '08P01');
        }
    }

    private function executePreparedTransactionControl(string $operation, string $sql): bool
    {
        $this->withResilience($operation, $sql, function () use ($sql): void {
            $this->sendSimpleQuery($sql, 0);
            $this->drainUntilReady();
        });
        return true;
    }

    private function buildPreparedTransactionSql(string $verb, string $gid): string
    {
        $normalized = trim($gid);
        if ($normalized === '') {
            throw new ScratchBirdSyntaxException('Global transaction id must not be empty', '42601');
        }
        return $verb . " '" . str_replace("'", "''", $normalized) . "'";
    }

    private function normalizeSavepointName(string $name): string
    {
        $normalized = trim($name);
        if ($normalized === '') {
            throw new ScratchBirdTransactionException('Savepoint name must not be empty', '3B001');
        }
        return $normalized;
    }

    private function normalizeMetadataCollection(string $collectionName): string
    {
        try {
            return Metadata::normalizeCollectionName($collectionName);
        } catch (\InvalidArgumentException $ex) {
            throw new ScratchBirdNotSupportedException($ex->getMessage(), '0A000');
        }
    }

    /**
     * @param array<string, mixed> $restrictions
     * @return array<string, mixed>
     */
    private function metadataRestrictions(array $restrictions): array
    {
        return array_filter(
            $restrictions,
            static fn (mixed $value): bool => $value !== null
        );
    }

    private function normalizeSessionSchema(?string $schema): ?string
    {
        if ($schema === null) {
            return null;
        }
        $normalized = trim($schema);
        if ($normalized === '') {
            return null;
        }
        if (strtolower($normalized) === 'public') {
            return self::DEFAULT_SESSION_SCHEMA;
        }
        return $normalized;
    }

    private function readExact(int $length): string
    {
        $data = '';
        while (strlen($data) < $length) {
            $chunk = fread($this->socket, $length - strlen($data));
            if ($chunk === false || $chunk === '') {
                throw new ScratchBirdConnectionException('Connection closed', '08006');
            }
            $data .= $chunk;
        }
        return $data;
    }

    public function buildQueryException(string $payload): ScratchBirdException
    {
        [, $sqlState, $message, $detail, $hint] = Protocol::parseErrorMessage($payload);
        return ErrorMapper::map($sqlState, $message, $detail, $hint);
    }

    private function recordError(\Throwable $ex): void
    {
        if ($ex instanceof ScratchBirdException) {
            $this->lastError = [$ex->sqlState, $ex->getCode(), $ex->getMessage()];
        } else {
            $this->lastError = ['HY000', 0, $ex->getMessage()];
        }
    }
}
