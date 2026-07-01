// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;
using System.Data.Common;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ScratchBird.Data;

var code = await SBIsqlDotNet.RunAsync(args);
return code;

internal static class SBIsqlDotNet
{
    private static readonly HashSet<string> PageSizes = ["4k", "8k", "16k", "32k", "64k", "128k"];
    private static readonly HashSet<string> Routes = ["embedded", "ipc_local", "listener-parser", "manager-listener-parser"];
    private static readonly HashSet<string> ParserModes = ["server-parser", "standalone-parser", "driver-sblr-uuid"];
    private static readonly HashSet<string> SupportedArgs =
    [
        "--database",
        "--host",
        "--port",
        "--user",
        "--password",
        "--role",
        "--sslmode",
        "--sslrootcert",
        "--sslcert",
        "--sslkey",
        "--ipc-path",
        "--route",
        "--parser-mode",
        "--page-size",
        "--namespace",
        "--input",
        "--output",
        "--error",
        "--diagnostics",
        "--metrics",
        "--transcript",
        "--summary",
        "--stop-on-error",
        "--expected-refusals",
        "--statement-timeout-ms",
        "--fetch-size",
        "--concurrency-worker",
        "--create-database",
        "--create-emulation-mode",
        "--run-id",
        "--language-resource-pack",
        "--language-resource-identity",
        "--language-resource-hash",
        "--language-profile",
        "--syntax-profile",
        "--topology-profile",
        "--standard-english-fallback",
    ];

    public static async Task<int> RunAsync(string[] raw)
    {
        var args = ParseArgs(raw);
        Validate(args);
        var summaryPath = Required(args, "--summary");
        var runRoot = Path.GetDirectoryName(summaryPath) ?? ".";
        Directory.CreateDirectory(runRoot);
        var paths = new Dictionary<string, string>
        {
            ["events"] = Path.Combine(runRoot, "command-events.jsonl"),
            ["wire"] = Path.Combine(runRoot, "wire-transcript.jsonl"),
            ["timing"] = Path.Combine(runRoot, "timing-groups.json"),
            ["digests"] = Path.Combine(runRoot, "result-digests.json"),
            ["metadata"] = Path.Combine(runRoot, "metadata-snapshots.json"),
            ["routeEnv"] = Path.Combine(runRoot, "route-environment.json"),
            ["refusals"] = Path.Combine(runRoot, "security-refusals.json"),
            ["api"] = Path.Combine(runRoot, "native-api-coverage.json"),
            ["review"] = Path.Combine(runRoot, "code-example-review.json"),
            ["junit"] = Path.Combine(runRoot, "junit.xml"),
            ["stdout"] = Path.Combine(runRoot, "stdout.log"),
            ["stderr"] = Path.Combine(runRoot, "stderr.log")
        };
        foreach (var path in new[]
        {
            Required(args, "--output"), Required(args, "--error"), Required(args, "--diagnostics"),
            Required(args, "--metrics"), Required(args, "--transcript"), Required(args, "--summary")
        }.Concat(paths.Values))
        {
            await WriteTextAsync(path, "");
        }

        var timings = new Dictionary<string, long>();
        var apiHits = new Dictionary<string, int>
        {
            ["DbConnection"] = 0,
            ["DbCommand"] = 0,
            ["DbParameter"] = 0,
            ["DbDataReader"] = 0,
            ["DbTransaction"] = 0,
            ["GetSchema"] = 0,
            ["SBWP_COPY"] = 0
        };
        var testcases = new List<Dictionary<string, object?>>();
        var failures = new List<Dictionary<string, object?>>();
        var digests = new List<Dictionary<string, object?>>();
        var securityRefusals = new List<Dictionary<string, object?>>();
        var started = NowNs();
        var expectedRefusals = LoadExpectedRefusals(ValueOrDefault(args, "--expected-refusals", ""));
        await WriteTextAsync(paths["routeEnv"], JsonSerializer.Serialize(
            RouteEnvironment(args, null, "fail", "not_probed")) + "\n");

        DbConnection? connection = null;
        ScratchBirdTransaction? activeTransaction = null;
        try
        {
            var builder = new ScratchBirdConnectionStringBuilder
            {
                Host = Required(args, "--host"),
                Port = int.Parse(Required(args, "--port")),
                Database = Required(args, "--database"),
                Username = Required(args, "--user"),
                Password = Required(args, "--password"),
                SSLMode = Required(args, "--route") == "ipc_local"
                    ? "disable"
                    : ValueOrDefault(args, "--sslmode", "require"),
                FrontDoorMode = Required(args, "--route") == "manager-listener-parser" ? "manager_proxy" : "direct",
                FetchSize = int.Parse(ValueOrDefault(args, "--fetch-size", "1000")),
                CommandTimeout = Math.Max(1, int.Parse(ValueOrDefault(args, "--statement-timeout-ms", "30000")) / 1000)
            };
            if (Required(args, "--route") == "ipc_local")
            {
                builder["Transport_Mode"] = "local_ipc";
                builder["IPC_Method"] = "unix";
                builder["IPC_Path"] = ValueOrDefault(args, "--ipc-path", "");
                builder["AllowInsecureDisable"] = "true";
            }
            builder["SslRootCert"] = ValueOrDefault(args, "--sslrootcert", "");
            builder["SslCert"] = ValueOrDefault(args, "--sslcert", "");
            builder["SslKey"] = ValueOrDefault(args, "--sslkey", "");
            if (!string.IsNullOrWhiteSpace(ValueOrDefault(args, "--role", "")))
            {
                builder["Role"] = ValueOrDefault(args, "--role", "");
            }
            builder["ApplicationName"] = "SBIsqlDotNet";
            builder["Metadata_Expand_Schema_Parents"] = "true";

            var connectStarted = NowNs();
            connection = new ScratchBirdConnection(builder.ConnectionString);
            apiHits["DbConnection"]++;
            await connection.OpenAsync();
            AddTiming(timings, "connection", connectStarted);
            await AppendJsonlAsync(Required(args, "--transcript"), new
            {
                @event = "connect",
                driver = "dotnet",
                route = Required(args, "--route"),
                parser_mode = Required(args, "--parser-mode"),
                page_size = Required(args, "--page-size"),
                language_profile = ValueOrDefault(args, "--language-profile", "en-US"),
                language_resource_identity = ValueOrDefault(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
                language_resource_hash = ValueOrDefault(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
                syntax_profile = ValueOrDefault(args, "--syntax-profile", "sbsql.v3"),
                topology_profile = ValueOrDefault(args, "--topology-profile", "topology.sbsql.canonical.v1")
            });
            await AppendJsonlAsync(paths["wire"], new { @event = "server_admission_required", driver_or_parser_finality = "forbidden" });

            if (args.ContainsKey("--create-database"))
            {
                var createStarted = NowNs();
                ((ScratchBirdConnection)connection).AttachCreate(
                    ValueOrDefault(args, "--create-emulation-mode", "sbsql"),
                    Required(args, "--database"));
                apiHits["AttachCreate"] = apiHits.GetValueOrDefault("AttachCreate") + 1;
                AddTiming(timings, "database_create", createStarted);
            }
            var routeEnvironment = await ProbeRouteEnvironmentAsync(connection, args);
            await WriteTextAsync(paths["routeEnv"], JsonSerializer.Serialize(routeEnvironment) + "\n");
            if (Required(args, "--route") != "embedded"
                && routeEnvironment["page_size_verification_status"]?.ToString() != "pass")
            {
                failures.Add(new Dictionary<string, object?>
                {
                    ["statement_id"] = "route_page_size",
                    ["message"] = "route page-size verification failed",
                    ["expected_page_size_bytes"] = routeEnvironment["expected_page_size_bytes"],
                    ["actual_page_size_bytes"] = routeEnvironment["actual_page_size_bytes"]
                });
                throw new InvalidOperationException("route page-size verification failed");
            }
            var inputPath = Required(args, "--input");
            var statements = SplitStatements(inputPath, await ReadInputAsync(inputPath));
            foreach (var statement in statements)
            {
                var sql = statement.Sql;
                var statementId = $"{statement.ScriptName}:{statement.StatementIndex}";
                var expectedRefusal = expectedRefusals.Contains(statementId);
                var expectedOutcome = expectedRefusal ? "refusal" : "success";
                var group = Classify(sql);
                var statementStarted = NowNs();
                var outcome = "success";
                var rowCount = -1;
                string? resultDigest = null;
                string? sqlState = null;
                string? diagnostic = null;
                try
                {
                    var executableSql = ExecutableSqlWithoutCopyMarkers(sql);
                    var copyPayload = CopyPayloadForStatement(sql);
                    if (IsCopyStdinStatement(sql))
                    {
                        if (copyPayload.Length == 0)
                        {
                            throw new InvalidOperationException("COPY FROM STDIN requires SB_COPY_INPUT rows in the script");
                        }
                        var rowsCopied = ((ScratchBirdConnection)connection).ExecuteCopyIn(executableSql, copyPayload,
                            int.Parse(ValueOrDefault(args, "--statement-timeout-ms", "30000")));
                        apiHits["SBWP_COPY"]++;
                        var rows = new List<List<object?>> { new() { "copy_in", rowsCopied } };
                        rowCount = rowsCopied > int.MaxValue ? int.MaxValue : (int)rowsCopied;
                        resultDigest = Sha256Text(JsonSerializer.Serialize(rows));
                        await AppendTextAsync(Required(args, "--output"), JsonSerializer.Serialize(new { statement_id = statementId, rows }) + "\n");
                        await AppendJsonlAsync(paths["wire"], new
                        {
                            @event = "copy_in",
                            statement_id = statementId,
                            parser_mode = Required(args, "--parser-mode"),
                            payload_bytes = copyPayload.Length,
                            rows_copied = rowsCopied,
                            engine_sql_text_execution = false,
                            mga_authority = "engine"
                        });
                    }
                    else if (group == "transaction")
                    {
                        activeTransaction = await RunTransactionAsync(connection, activeTransaction, sql, apiHits);
                        rowCount = 0;
                        resultDigest = Sha256Text("transaction");
                    }
                    else if (Required(args, "--parser-mode") != "server-parser")
                    {
                        var sbConnection = (ScratchBirdConnection)connection;
                        var timeoutMs = int.Parse(ValueOrDefault(args, "--statement-timeout-ms", "30000"));
                        var fetchSize = int.Parse(ValueOrDefault(args, "--fetch-size", "1000"));
                        var compiled = sbConnection.CompileSblr(sql, timeoutMs);
                        apiHits["ReturnSblr"] = apiHits.GetValueOrDefault("ReturnSblr") + 1;
                        await AppendJsonlAsync(paths["wire"], new
                        {
                            @event = "driver_sblr_compile",
                            driver = "dotnet",
                            parser_mode = Required(args, "--parser-mode"),
                            statement_id = statementId,
                            sblr_hash = compiled.Hash.ToString(),
                            sblr_version = compiled.Version,
                            sblr_bytes = compiled.Bytecode.Length
                        });

                        var resultSets = sbConnection.ExecuteSblr(compiled, timeoutMs, fetchSize);
                        apiHits["ExecuteSblr"] = apiHits.GetValueOrDefault("ExecuteSblr") + 1;
                        await AppendJsonlAsync(paths["wire"], new
                        {
                            @event = "driver_sblr_execute",
                            driver = "dotnet",
                            parser_mode = Required(args, "--parser-mode"),
                            statement_id = statementId,
                            sblr_hash = compiled.Hash.ToString(),
                            sblr_version = compiled.Version,
                            sblr_bytes = compiled.Bytecode.Length,
                            engine_sql_text_execution = false,
                            mga_authority = "engine"
                        });

                        var rows = resultSets
                            .SelectMany(result => result.Rows.Select(row => row.ToList()))
                            .ToList();
                        if (group is "query" or "metadata" || rows.Count > 0)
                        {
                            rowCount = rows.Count;
                            resultDigest = Sha256Text(JsonSerializer.Serialize(rows));
                            await AppendTextAsync(Required(args, "--output"), JsonSerializer.Serialize(new { statement_id = statementId, rows }) + "\n");
                        }
                        else
                        {
                            var affected = resultSets.Sum(result => result.RowCount > 0 ? result.RowCount : 0);
                            rowCount = affected > int.MaxValue ? int.MaxValue : (int)affected;
                            resultDigest = Sha256Text(rowCount.ToString());
                        }
                    }
                    else
                    {
                        await using var command = connection.CreateCommand();
                        apiHits["DbCommand"]++;
                        command.CommandText = sql;
                        command.CommandTimeout = int.Parse(ValueOrDefault(args, "--statement-timeout-ms", "30000")) / 1000;
                        if (activeTransaction != null)
                        {
                            command.Transaction = activeTransaction;
                        }
                        DbParameter? parameter = null;
                        if (parameter != null)
                        {
                            command.Parameters.Add(parameter);
                            apiHits["DbParameter"]++;
                            command.Prepare();
                        }
                        await using var reader = await command.ExecuteReaderAsync();
                        apiHits["DbDataReader"]++;
                        var rows = new List<List<object?>>();
                        while (await reader.ReadAsync())
                        {
                            var row = new List<object?>();
                            for (var column = 0; column < reader.FieldCount; column++)
                            {
                                row.Add(await reader.IsDBNullAsync(column) ? null : reader.GetValue(column));
                            }
                            rows.Add(row);
                        }
                        rowCount = rows.Count;
                        resultDigest = Sha256Text(JsonSerializer.Serialize(rows));
                        await AppendTextAsync(Required(args, "--output"), JsonSerializer.Serialize(new { statement_id = statementId, rows }) + "\n");
                    }
                    digests.Add(new Dictionary<string, object?> { ["statement_id"] = statementId, ["row_count"] = rowCount, ["result_digest"] = resultDigest });
                    if (expectedRefusal)
                    {
                        outcome = "unexpected_success";
                        diagnostic = "statement succeeded but was expected to refuse";
                        failures.Add(new Dictionary<string, object?> { ["statement_id"] = statementId, ["message"] = diagnostic });
                    }
                }
                catch (Exception ex)
                {
                    outcome = "refusal";
                    sqlState = ex is ScratchBirdException sbEx ? sbEx.SqlState : "HY000";
                    diagnostic = ex.Message;
                    await AppendJsonlAsync(Required(args, "--diagnostics"), new { statement_id = statementId, sqlstate = sqlState, message = diagnostic });
                    await AppendTextAsync(Required(args, "--error"), $"{statementId}: {diagnostic}\n");
                    if (expectedRefusal)
                    {
                        securityRefusals.Add(new Dictionary<string, object?> { ["statement_id"] = statementId, ["sqlstate"] = sqlState, ["diagnostic_code"] = diagnostic });
                    }
                    else
                    {
                        failures.Add(new Dictionary<string, object?> { ["statement_id"] = statementId, ["message"] = diagnostic });
                    }
                    if (!expectedRefusal && BooleanArg(args, "--stop-on-error", true))
                    {
                        AddTiming(timings, group, NowNs() - statementStarted);
                        break;
                    }
                }
                var elapsed = NowNs() - statementStarted;
                AddTiming(timings, group, elapsed);
                var ev = new Dictionary<string, object?>
                {
                    ["run_id"] = ValueOrDefault(args, "--run-id", "manual"),
                    ["driver_name"] = "dotnet",
                    ["driver_version"] = "unknown",
                    ["route"] = Required(args, "--route"),
                    ["parser_mode"] = Required(args, "--parser-mode"),
                    ["page_size"] = Required(args, "--page-size"),
                    ["namespace"] = Required(args, "--namespace"),
                    ["script"] = Required(args, "--input"),
                    ["statement_index"] = statement.StatementIndex,
                    ["statement_id"] = statementId,
                    ["command_group"] = group,
                    ["sql_hash"] = Sha256Text(sql),
                    ["expected_outcome"] = expectedOutcome,
                    ["actual_outcome"] = outcome,
                    ["sqlstate"] = sqlState,
                    ["diagnostic_code"] = diagnostic,
                    ["canonical_message_vector"] = Array.Empty<string>(),
                    ["row_count"] = rowCount,
                    ["result_digest"] = resultDigest,
                    ["elapsed_ns"] = elapsed,
                    ["server_revalidation_state"] = "required",
                    ["language_profile"] = ValueOrDefault(args, "--language-profile", "en-US"),
                    ["language_resource_pack"] = ValueOrDefault(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
                    ["language_resource_identity"] = ValueOrDefault(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
                    ["language_resource_hash"] = ValueOrDefault(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
                    ["syntax_profile"] = ValueOrDefault(args, "--syntax-profile", "sbsql.v3"),
                    ["topology_profile"] = ValueOrDefault(args, "--topology-profile", "topology.sbsql.canonical.v1"),
                    ["standard_english_fallback"] = BooleanArg(args, "--standard-english-fallback", true),
                    ["transaction_id_observed"] = null,
                    ["mga_authority"] = "engine",
                    ["native_api_surface"] = "ado_net",
                    ["code_example_section"] = IsCopyStdinStatement(sql) ? "copy_in" : "dbcommand_reader"
                };
                await AppendJsonlAsync(paths["events"], ev);
                testcases.Add(ev);
            }

            var metadataStarted = NowNs();
            var schema = connection.GetSchema("Schemas");
            apiHits["GetSchema"]++;
            await WriteTextAsync(paths["metadata"], JsonSerializer.Serialize(new
            {
                tables_digest = Sha256Text(JsonSerializer.Serialize(schema.Rows.Count)),
                row_count = schema.Rows.Count
            }) + "\n");
            AddTiming(timings, "metadata", metadataStarted);
        }
        catch (Exception ex)
        {
            failures.Add(new Dictionary<string, object?> { ["statement_id"] = "run", ["message"] = ex.Message });
            await AppendTextAsync(paths["stderr"], ex + "\n");
        }
        finally
        {
            activeTransaction?.Dispose();
            if (connection != null)
            {
                await connection.DisposeAsync();
            }
        }

        var elapsedTotal = NowNs() - started;
        timings["overall"] = elapsedTotal;
        var sslmode = ValueOrDefault(args, "--sslmode", "require");
        var transportMode = TransportModeForRoute(Required(args, "--route"), sslmode);
        var processMetrics = CurrentProcessMetrics();
        var summary = new
        {
            run_id = ValueOrDefault(args, "--run-id", "manual"),
            driver_name = "dotnet",
            route = Required(args, "--route"),
            parser_mode = Required(args, "--parser-mode"),
            page_size = Required(args, "--page-size"),
            @namespace = Required(args, "--namespace"),
            sslmode,
            transport_mode = transportMode,
            transport_endpoint_kind = EndpointKindForRoute(Required(args, "--route")),
            driver_transport_implementation = TransportImplementationForRoute(Required(args, "--route")),
            cpp_library_boundary = "none",
            language_resource_pack = ValueOrDefault(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
            language_resource_identity = ValueOrDefault(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
            language_resource_hash = ValueOrDefault(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
            language_resource_authority = "shared_server_parser_resource_pack",
            language_profile = ValueOrDefault(args, "--language-profile", "en-US"),
            syntax_profile = ValueOrDefault(args, "--syntax-profile", "sbsql.v3"),
            topology_profile = ValueOrDefault(args, "--topology-profile", "topology.sbsql.canonical.v1"),
            standard_english_fallback = BooleanArg(args, "--standard-english-fallback", true),
            status = failures.Count == 0 ? "pass" : "fail",
            failure_count = failures.Count,
            elapsed_ns = elapsedTotal,
            process_metrics = processMetrics,
            server_revalidation_required = true,
            driver_or_parser_finality = "forbidden",
            mga_authority = "engine"
        };
        await WriteTextAsync(Required(args, "--summary"), JsonSerializer.Serialize(summary) + "\n");
        var clientMetrics = processMetrics["client"];
        await WriteTextAsync(Required(args, "--metrics"), JsonSerializer.Serialize(new
        {
            role = "client",
            rss_kb = clientMetrics["last_rss_kb"],
            vsize_kb = clientMetrics["last_vsize_kb"]
        }) + "\n");
        await WriteTextAsync(paths["timing"], JsonSerializer.Serialize(timings) + "\n");
        await WriteTextAsync(paths["digests"], JsonSerializer.Serialize(digests) + "\n");
        await WriteTextAsync(paths["refusals"], JsonSerializer.Serialize(securityRefusals) + "\n");
        await WriteTextAsync(paths["api"], JsonSerializer.Serialize(apiHits) + "\n");
        await WriteTextAsync(paths["review"], JsonSerializer.Serialize(new
        {
            driver = "dotnet",
            public_api_only = true,
            shells_out_to_other_driver = false,
            source_is_canonical_example = true,
            sections = new[] { "connection", "command", "reader", "metadata", "diagnostics", "transaction" }
        }) + "\n");
        await WriteTextAsync(paths["junit"], Junit("SBIsqlDotNet", "scratchbird.dotnet", testcases, failures));
        await AppendTextAsync(paths["stdout"], $"SBIsqlDotNet status={summary.status}\n");
        return failures.Count == 0 ? 0 : 1;
    }

    private static async Task<ScratchBirdTransaction?> RunTransactionAsync(
        DbConnection connection,
        ScratchBirdTransaction? activeTransaction,
        string sql,
        Dictionary<string, int> apiHits)
    {
        var tokens = SqlWithoutLineComments(sql)
            .Trim()
            .Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        var first = tokens.FirstOrDefault()?.ToLowerInvariant() ?? "";
        if (first is "begin" or "start")
        {
            if (activeTransaction != null)
            {
                throw new InvalidOperationException("transaction already active");
            }
            var transaction = (ScratchBirdTransaction)await connection.BeginTransactionAsync(IsolationLevel.ReadCommitted);
            apiHits["DbTransaction"]++;
            return transaction;
        }
        if (first == "commit")
        {
            if (activeTransaction == null)
            {
                throw new InvalidOperationException("COMMIT requires an active transaction");
            }
            activeTransaction.Commit();
            activeTransaction.Dispose();
            apiHits["DbTransactionCommit"] = apiHits.GetValueOrDefault("DbTransactionCommit") + 1;
            return null;
        }
        if (first == "rollback")
        {
            if (activeTransaction == null)
            {
                throw new InvalidOperationException("ROLLBACK requires an active transaction");
            }
            if (tokens.Length >= 4 &&
                string.Equals(tokens[1], "to", StringComparison.OrdinalIgnoreCase) &&
                string.Equals(tokens[2], "savepoint", StringComparison.OrdinalIgnoreCase))
            {
                activeTransaction.Rollback(NormalizeTerminator(tokens[3]));
                apiHits["DbTransactionRollbackToSavepoint"] = apiHits.GetValueOrDefault("DbTransactionRollbackToSavepoint") + 1;
                return activeTransaction;
            }
            activeTransaction.Rollback();
            activeTransaction.Dispose();
            apiHits["DbTransactionRollback"] = apiHits.GetValueOrDefault("DbTransactionRollback") + 1;
            return null;
        }
        if (first == "savepoint")
        {
            if (activeTransaction == null)
            {
                throw new InvalidOperationException("SAVEPOINT requires an active transaction");
            }
            if (tokens.Length < 2)
            {
                throw new InvalidOperationException("SAVEPOINT requires a name");
            }
            activeTransaction.Save(NormalizeTerminator(tokens[1]));
            apiHits["DbTransactionSavepoint"] = apiHits.GetValueOrDefault("DbTransactionSavepoint") + 1;
            return activeTransaction;
        }
        if (first == "release")
        {
            if (activeTransaction == null)
            {
                throw new InvalidOperationException("RELEASE SAVEPOINT requires an active transaction");
            }
            if (tokens.Length < 3 || !string.Equals(tokens[1], "savepoint", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException("only RELEASE SAVEPOINT is supported in this runner");
            }
            activeTransaction.Release(NormalizeTerminator(tokens[2]));
            apiHits["DbTransactionReleaseSavepoint"] = apiHits.GetValueOrDefault("DbTransactionReleaseSavepoint") + 1;
            return activeTransaction;
        }
        throw new InvalidOperationException($"unsupported transaction statement: {sql}");
    }

    private static string NormalizeTerminator(string token) => token.TrimEnd(';');

    private static string SqlWithoutLineComments(string sql) =>
        string.Join("\n", sql.Split('\n')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0 && !line.StartsWith("--", StringComparison.Ordinal)));

    private static string ExecutableSqlWithoutCopyMarkers(string sql) =>
        string.Join("\n", sql.Split('\n')
            .Where(line => !line.TrimStart().StartsWith("-- SB_COPY_INPUT ", StringComparison.Ordinal)))
            .Trim();

    private static byte[] CopyPayloadForStatement(string sql)
    {
        var rows = sql.Split('\n')
            .Select(line => line.TrimStart())
            .Where(line => line.StartsWith("-- SB_COPY_INPUT ", StringComparison.Ordinal))
            .Select(line => line["-- SB_COPY_INPUT ".Length..].TrimEnd('\r'))
            .ToList();
        return Encoding.UTF8.GetBytes(string.Join("\n", rows) + (rows.Count == 0 ? "" : "\n"));
    }

    private static bool IsCopyStdinStatement(string sql)
    {
        var executable = string.Join(" ", sql.Split('\n')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0 && !line.StartsWith("--", StringComparison.Ordinal)))
            .ToLowerInvariant();
        return executable.StartsWith("copy ", StringComparison.Ordinal) &&
            executable.Contains(" from stdin", StringComparison.Ordinal);
    }

    private static Dictionary<string, string> ParseArgs(string[] raw)
    {
        var args = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < raw.Length; i++)
        {
            var key = raw[i];
            if (!key.StartsWith("--", StringComparison.Ordinal))
            {
                throw new ArgumentException($"unexpected positional argument: {key}");
            }
            if (!SupportedArgs.Contains(key))
            {
                throw new ArgumentException($"unsupported argument: {key}");
            }
            if (key is "--stop-on-error" or "--create-database" or "--standard-english-fallback")
            {
                if (i + 1 < raw.Length && !raw[i + 1].StartsWith("--", StringComparison.Ordinal))
                {
                    args[key] = raw[++i];
                }
                else
                {
                    args[key] = "true";
                }
                continue;
            }
            if (i + 1 >= raw.Length || raw[i + 1].StartsWith("--", StringComparison.Ordinal))
            {
                throw new ArgumentException($"missing value for {key}");
            }
            args[key] = raw[++i];
        }
        return args;
    }

    private static void Validate(Dictionary<string, string> args)
    {
        if (!PageSizes.Contains(Required(args, "--page-size"))) throw new ArgumentException($"unsupported page size: {Required(args, "--page-size")}");
        if (!Routes.Contains(Required(args, "--route"))) throw new ArgumentException($"unsupported route: {Required(args, "--route")}");
        if (Required(args, "--route") == "embedded") throw new ArgumentException("embedded transport is unsupported by the .NET driver; no ScratchBird C++ library boundary is exposed");
        if (!ParserModes.Contains(Required(args, "--parser-mode"))) throw new ArgumentException($"unsupported parser mode: {Required(args, "--parser-mode")}");
    }

    private static List<StatementChunk> SplitStatements(string inputPath, string script)
    {
        var chain = SqlStatementSplitter.SplitChain(script);
        if (chain.Count > 0)
        {
            return chain
                .Select(statement =>
                    new StatementChunk(statement.ScriptName, statement.StatementIndex, statement.Sql))
                .ToList();
        }
        return SqlStatementSplitter.Split(script)
            .Select((sql, index) => new StatementChunk(Path.GetFileName(inputPath), index + 1, sql))
            .ToList();
    }

    private static string Classify(string sql)
    {
        var trimmed = string.Join(" ", sql.Split('\n')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0 && !line.StartsWith("--", StringComparison.Ordinal)))
            .ToLowerInvariant();
        var first = trimmed.Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
        if (first == "copy") return "copy";
        if (new[] { "create", "alter", "drop" }.Contains(first)) return "ddl";
        if (new[] { "insert", "update", "delete", "merge", "upsert" }.Contains(first)) return "dml";
        if (new[] { "commit", "rollback", "savepoint", "begin", "start", "release" }.Contains(first)) return "transaction";
        if (new[] { "grant", "revoke" }.Contains(first)) return "security_refusal";
        return trimmed.Contains("sys.", StringComparison.Ordinal) ? "metadata" : "query";
    }

    private static string Required(Dictionary<string, string> args, string key) =>
        args.TryGetValue(key, out var value) && !string.IsNullOrEmpty(value)
            ? value
            : throw new ArgumentException($"missing required argument {key}");

    private static string ValueOrDefault(Dictionary<string, string> args, string key, string fallback) =>
        args.TryGetValue(key, out var value) ? value : fallback;

    private static bool BooleanArg(Dictionary<string, string> args, string key, bool fallback)
    {
        if (!args.TryGetValue(key, out var value))
        {
            return fallback;
        }
        return value.ToLowerInvariant() switch
        {
            "true" or "1" or "yes" or "on" => true,
            "false" or "0" or "no" or "off" => false,
            _ => throw new ArgumentException($"invalid boolean value for {key}: {value}")
        };
    }

    private static string TransportModeForRoute(string route, string sslmode) =>
        route switch
        {
            "embedded" => "embedded_no_network_transport",
            "ipc_local" => "local_ipc_no_tls",
            _ => string.Equals(sslmode, "disable", StringComparison.OrdinalIgnoreCase) ? "tls_disabled" : "tls_required"
        };

    private static string EndpointKindForRoute(string route) =>
        route switch
        {
            "ipc_local" => "unix_domain_socket",
            "embedded" => "embedded_bridge",
            _ => "tcp"
        };

    private static string TransportImplementationForRoute(string route) =>
        route switch
        {
            "embedded" => "unsupported_no_cpp_library_boundary",
            "ipc_local" => "native_dotnet_unix_domain_socket",
            _ => "native_dotnet_tcp"
        };

    private static int PageSizeBytes(string pageSize) =>
        pageSize switch
        {
            "4k" => 4096,
            "8k" => 8192,
            "16k" => 16384,
            "32k" => 32768,
            "64k" => 65536,
            "128k" => 131072,
            _ => 0
        };

    private static Dictionary<string, object?> RouteEnvironment(
        Dictionary<string, string> args,
        int? actualPageSize,
        string status,
        string? reason = null)
    {
        var record = new Dictionary<string, object?>
        {
            ["run_id"] = ValueOrDefault(args, "--run-id", "manual"),
            ["driver"] = "dotnet",
            ["route"] = Required(args, "--route"),
            ["sslmode"] = ValueOrDefault(args, "--sslmode", "require"),
            ["parser_mode"] = Required(args, "--parser-mode"),
            ["concurrency_mode"] = "single",
            ["namespace"] = Required(args, "--namespace"),
            ["page_size"] = Required(args, "--page-size"),
            ["expected_page_size_bytes"] = PageSizeBytes(Required(args, "--page-size")),
            ["actual_page_size_bytes"] = actualPageSize,
            ["page_size_verification_source"] = "SHOW DATABASE",
            ["page_size_verification_status"] = status,
            ["transport_mode"] = TransportModeForRoute(Required(args, "--route"), ValueOrDefault(args, "--sslmode", "require")),
            ["transport_endpoint_kind"] = EndpointKindForRoute(Required(args, "--route")),
            ["driver_transport_implementation"] = TransportImplementationForRoute(Required(args, "--route"))
        };
        if (!string.IsNullOrWhiteSpace(reason))
        {
            record["failure_reason"] = reason;
        }
        return record;
    }

    private static async Task<Dictionary<string, object?>> ProbeRouteEnvironmentAsync(
        DbConnection connection,
        Dictionary<string, string> args)
    {
        try
        {
            await using var command = connection.CreateCommand();
            command.CommandText = "SHOW DATABASE";
            await using var reader = await command.ExecuteReaderAsync();
            var pageIndex = -1;
            for (var index = 0; index < reader.FieldCount; index++)
            {
                if (string.Equals(reader.GetName(index), "page_size_bytes", StringComparison.OrdinalIgnoreCase))
                {
                    pageIndex = index;
                    break;
                }
            }
            if (pageIndex < 0 && reader.FieldCount >= 3)
            {
                pageIndex = 2;
            }
            if (pageIndex < 0)
            {
                return RouteEnvironment(args, null, "fail", "show_database_missing_page_size_bytes");
            }
            if (!await reader.ReadAsync())
            {
                return RouteEnvironment(args, null, "fail", "show_database_returned_no_rows");
            }
            var actual = Convert.ToInt32(reader.GetValue(pageIndex));
            var expected = PageSizeBytes(Required(args, "--page-size"));
            return RouteEnvironment(
                args,
                actual,
                actual == expected ? "pass" : "fail",
                actual == expected ? null : "actual_page_size_mismatch");
        }
        catch (Exception ex)
        {
            return RouteEnvironment(args, null, "fail", ex.Message);
        }
    }

    private static HashSet<string> LoadExpectedRefusals(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return [];
        }
        if (!File.Exists(path))
        {
            throw new FileNotFoundException("expected refusal file not found", path);
        }
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        var ids = new HashSet<string>(StringComparer.Ordinal);
        if (doc.RootElement.ValueKind == JsonValueKind.Array)
        {
            AddExpectedRefusals(doc.RootElement, ids);
            return ids;
        }
        if (doc.RootElement.ValueKind != JsonValueKind.Object)
        {
            throw new ArgumentException("expected refusals must be a JSON object or array");
        }
        if (doc.RootElement.TryGetProperty("statement_ids", out var statementIds))
        {
            AddExpectedRefusals(statementIds, ids);
        }
        if (doc.RootElement.TryGetProperty("expected_refusals", out var expected))
        {
            AddExpectedRefusals(expected, ids);
        }
        if (doc.RootElement.TryGetProperty("expected_diagnostics", out var expectedDiagnostics) &&
            expectedDiagnostics.ValueKind == JsonValueKind.Object)
        {
            foreach (var item in expectedDiagnostics.EnumerateObject())
            {
                ids.Add(item.Name);
            }
        }
        return ids;
    }

    private static void AddExpectedRefusals(JsonElement value, HashSet<string> ids)
    {
        if (value.ValueKind != JsonValueKind.Array)
        {
            return;
        }
        foreach (var item in value.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String)
            {
                ids.Add(item.GetString() ?? "");
            }
            else if (item.ValueKind == JsonValueKind.Object &&
                     item.TryGetProperty("statement_id", out var statementId) &&
                     statementId.ValueKind == JsonValueKind.String)
            {
                ids.Add(statementId.GetString() ?? "");
            }
        }
        ids.Remove("");
    }

    private static async Task<string> ReadInputAsync(string path) =>
        path == "-" ? await Console.In.ReadToEndAsync() : await File.ReadAllTextAsync(path);

    private static long NowNs() => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() * 1_000_000L;

    private static void AddTiming(IDictionary<string, long> timings, string group, long started) =>
        timings[group] = (timings.TryGetValue(group, out var current) ? current : 0L) + (NowNs() - started);

    private static Dictionary<string, Dictionary<string, long>> CurrentProcessMetrics()
    {
        var rssKb = Math.Max(1L, Environment.WorkingSet / 1024L);
        var vsizeKb = Math.Max(rssKb, GC.GetTotalMemory(false) / 1024L);
        return new Dictionary<string, Dictionary<string, long>>
        {
            ["client"] = new()
            {
                ["max_rss_kb"] = rssKb,
                ["max_vsize_kb"] = vsizeKb,
                ["last_rss_kb"] = rssKb,
                ["last_vsize_kb"] = vsizeKb
            }
        };
    }

    private static string Sha256Text(string text) =>
        "sha256:" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text))).ToLowerInvariant();

    private static async Task WriteTextAsync(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? ".");
        await File.WriteAllTextAsync(path, text);
    }

    private static async Task AppendTextAsync(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? ".");
        await File.AppendAllTextAsync(path, text);
    }

    private static Task AppendJsonlAsync(string path, object record) =>
        AppendTextAsync(path, JsonSerializer.Serialize(record) + "\n");

    private static string Junit(string suite, string klass, IReadOnlyCollection<Dictionary<string, object?>> testcases, IReadOnlyCollection<Dictionary<string, object?>> failures)
    {
        var builder = new StringBuilder();
        builder.AppendLine("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
        builder.AppendLine($"<testsuite name=\"{EscapeXml(suite)}\" tests=\"{Math.Max(testcases.Count, 1)}\" failures=\"{failures.Count}\">");
        if (testcases.Count == 0) builder.AppendLine($"  <testcase classname=\"{EscapeXml(klass)}\" name=\"run\"></testcase>");
        foreach (var testcase in testcases) builder.AppendLine($"  <testcase classname=\"{EscapeXml(klass)}\" name=\"{EscapeXml(Convert.ToString(testcase["statement_id"]) ?? "statement")}\"></testcase>");
        foreach (var failure in failures) builder.AppendLine($"  <testcase classname=\"{EscapeXml(klass)}\" name=\"{EscapeXml(Convert.ToString(failure["statement_id"]) ?? "run")}\"><failure message=\"{EscapeXml(Convert.ToString(failure["message"]) ?? "failure")}\" /></testcase>");
        builder.AppendLine("</testsuite>");
        return builder.ToString();
    }

    private static string EscapeXml(string text) =>
        text.Replace("&", "&amp;").Replace("\"", "&quot;").Replace("<", "&lt;").Replace(">", "&gt;");
}

internal sealed record StatementChunk(string ScriptName, int StatementIndex, string Sql);
