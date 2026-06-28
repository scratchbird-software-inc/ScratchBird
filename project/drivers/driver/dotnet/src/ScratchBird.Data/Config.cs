// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Globalization;

namespace ScratchBird.Data;

public sealed class ScratchBirdConfig
{
    public string Host { get; set; } = "localhost";
    public int Port { get; set; } = 3092;
    public string FrontDoorMode { get; set; } = "direct";
    public string TransportMode { get; set; } = "inet_listener";
    public string IpcMethod { get; set; } = "unix";
    public string IpcPath { get; set; } = "";
    public string Protocol { get; set; } = "native";
    public string Database { get; set; } = "";
    public string Username { get; set; } = "";
    public string Password { get; set; } = "";
    public string Schema { get; set; } = "";
    public bool MetadataExpandSchemaParents { get; set; } = false;
    public string Role { get; set; } = "";
    public string SslMode { get; set; } = "require";
    public bool AllowInsecureDisable { get; set; } = false;
    public string? SslRootCert { get; set; }
    public string? SslCert { get; set; }
    public string? SslKey { get; set; }
    public string? SslPassword { get; set; }
    public int ConnectTimeoutMs { get; set; } = 30000;
    public int SocketTimeoutMs { get; set; } = 0;
    public string ApplicationName { get; set; } = "scratchbird_dotnet";
    public bool BinaryTransfer { get; set; } = true;
    public string Compression { get; set; } = "off";
    public int ConnectClientFlags { get; set; } = 0x0100;
    public string AuthMethodId { get; set; } = "";
    public string AuthMethodPayload { get; set; } = "";
    public string AuthToken { get; set; } = "";
    public string AuthPayloadJson { get; set; } = "";
    public string AuthPayloadB64 { get; set; } = "";
    public string AuthProviderProfile { get; set; } = "";
    public string AuthRequiredMethods { get; set; } = "";
    public string AuthForbiddenMethods { get; set; } = "";
    public bool AuthRequireChannelBinding { get; set; } = false;
    public string WorkloadIdentityToken { get; set; } = "";
    public string ProxyPrincipalAssertion { get; set; } = "";
    public string DormantId { get; set; } = "";
    public string DormantReattachToken { get; set; } = "";
    public int DefaultFetchSize { get; set; } = 0;
    public bool Pooling { get; set; } = false;
    public int MinPoolSize { get; set; } = 0;
    public int MaxPoolSize { get; set; } = 100;
    public int ConnectionLifetime { get; set; } = 0;
    public int PoolAcquireTimeoutMs { get; set; } = 250;
    public int CircuitBreakerFailureThreshold { get; set; } = 0;
    public int CircuitBreakerRecoveryTimeoutMs { get; set; } = 30000;
    public int CircuitBreakerSuccessThreshold { get; set; } = 2;
    public int CircuitBreakerHalfOpenMaxRequests { get; set; } = 1;
    public int KeepaliveIntervalMs { get; set; } = 120000;
    public int KeepaliveMaxIdleBeforeCheckMs { get; set; } = 600000;
    public int KeepaliveValidationTimeoutMs { get; set; } = 5000;
    public int PipelineMaxInFlight { get; set; } = 100;
    public bool PipelineAutoFlush { get; set; } = true;
    public int PipelineAutoFlushThreshold { get; set; } = 10;
    public int PipelineFlushTimeoutMs { get; set; } = 5000;
    public int LeakThresholdMs { get; set; } = 30000;
    public bool LeakCaptureStackTrace { get; set; } = false;
    public string ManagerAuthToken { get; set; } = string.Empty;
    public string ManagerUsername { get; set; } = string.Empty;
    public string ManagerDatabase { get; set; } = string.Empty;
    public string ManagerConnectionProfile { get; set; } = "SBsql";
    public string ManagerClientIntent { get; set; } = "SBsql";
    public ushort ManagerClientFlags { get; set; } = 0;
    public bool ManagerAuthFastPath { get; set; } = true;
    public bool TelemetryEnableTracing { get; set; } = true;
    public bool TelemetryEnableMetrics { get; set; } = true;
    public bool TelemetryEnableSlowOperationLog { get; set; } = true;
    public int TelemetrySlowOperationThresholdMs { get; set; } = 1000;
    public int TelemetrySlowOperationMaxEntries { get; set; } = 100;
    public double TelemetrySampleRate { get; set; } = 1d;
    public bool TelemetrySanitizeStatements { get; set; } = true;

    public static ScratchBirdConfig FromConnectionString(string connectionString)
    {
        var parsed = DsnParser.Parse(connectionString);
        return parsed;
    }

    internal static string NormalizeNativeProtocol(string? value)
    {
        var normalized = (value ?? string.Empty).Trim().ToLowerInvariant();
        return normalized switch
        {
            "" or "native" or "scratchbird" or "scratchbird-native" or "scratchbird_native" => "native",
            _ => throw new ArgumentException("Only protocol=native is supported; connect to the native parser listener/port.")
        };
    }

    internal static string NormalizeFrontDoorMode(string? value)
    {
        var normalized = (value ?? string.Empty).Trim().ToLowerInvariant();
        return normalized switch
        {
            "" or "direct" => "direct",
            "manager_proxy" or "manager-proxy" or "managed" => "manager_proxy",
            _ => throw new ArgumentException("front_door_mode must be direct or manager_proxy.")
        };
    }
}

internal static class DsnParser
{
    public static ScratchBirdConfig Parse(string dsn)
    {
        if (string.IsNullOrWhiteSpace(dsn))
        {
            return new ScratchBirdConfig();
        }

        if (dsn.Contains("://", StringComparison.Ordinal))
        {
            return ParseUri(dsn);
        }

        return ParseKeyValue(dsn);
    }

    private static ScratchBirdConfig ParseUri(string dsn)
    {
        var uri = new Uri(dsn);
        if (!string.Equals(uri.Scheme, "scratchbird", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException($"Unsupported DSN scheme: {uri.Scheme}");
        }

        var cfg = new ScratchBirdConfig();
        if (!string.IsNullOrEmpty(uri.Host)) cfg.Host = uri.Host;
        if (uri.Port > 0) cfg.Port = uri.Port;
        if (!string.IsNullOrEmpty(uri.UserInfo))
        {
            var parts = uri.UserInfo.Split(':', 2);
            cfg.Username = Uri.UnescapeDataString(parts[0]);
            if (parts.Length > 1) cfg.Password = Uri.UnescapeDataString(parts[1]);
        }
        if (!string.IsNullOrEmpty(uri.AbsolutePath) && uri.AbsolutePath != "/")
        {
            cfg.Database = uri.AbsolutePath.TrimStart('/');
        }

        var query = uri.Query.TrimStart('?');
        if (!string.IsNullOrEmpty(query))
        {
            foreach (var pair in query.Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                var idx = pair.IndexOf('=');
                if (idx <= 0) continue;
                var key = pair[..idx];
                var value = Uri.UnescapeDataString(pair[(idx + 1)..]);
                ApplyParam(cfg, key, value);
            }
        }

        return cfg;
    }

    private static ScratchBirdConfig ParseKeyValue(string dsn)
    {
        var cfg = new ScratchBirdConfig();
        var tokens = SplitConnectionString(dsn);
        foreach (var token in tokens)
        {
            var idx = token.IndexOf('=');
            if (idx <= 0) continue;
            var key = token[..idx].Trim();
            var value = token[(idx + 1)..].Trim().Trim('"').Trim('\'');
            ApplyParam(cfg, key, value);
        }
        return cfg;
    }

    private static IEnumerable<string> SplitConnectionString(string dsn)
    {
        var separator = dsn.Contains(';') ? ';' : ' ';
        return dsn.Split(separator, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    }

    private static void ApplyParam(ScratchBirdConfig cfg, string key, string value)
    {
        switch (key.ToLowerInvariant())
        {
            case "host":
            case "server":
            case "data source":
            case "datasource":
                cfg.Host = value;
                break;
            case "port":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var port))
                    cfg.Port = port;
                break;
            case "front_door_mode":
            case "frontdoormode":
            case "connection_mode":
            case "ingress_mode":
                cfg.FrontDoorMode = ScratchBirdConfig.NormalizeFrontDoorMode(value);
                break;
            case "transport_mode":
            case "transportmode":
            case "transport":
                cfg.TransportMode = value;
                break;
            case "ipc_method":
            case "ipcmethod":
                cfg.IpcMethod = value;
                break;
            case "ipc_path":
            case "ipcpath":
            case "socket_path":
            case "pipe_name":
                cfg.IpcPath = value;
                break;
            case "database":
            case "dbname":
            case "initial catalog":
                cfg.Database = value;
                break;
            case "protocol":
            case "parser":
            case "dialect":
                cfg.Protocol = ScratchBirdConfig.NormalizeNativeProtocol(value);
                break;
            case "user":
            case "username":
            case "user id":
            case "uid":
                cfg.Username = value;
                break;
            case "password":
            case "pwd":
                cfg.Password = value;
                break;
            case "schema":
            case "search_path":
            case "searchpath":
            case "currentschema":
                cfg.Schema = value;
                break;
            case "metadataexpandschemaparents":
            case "metadata_expand_schema_parents":
            case "expandschemaparents":
            case "expand_schema_parents":
            case "dbeaverexpandschemaparents":
            case "dbeaver_expand_schema_parents":
                cfg.MetadataExpandSchemaParents = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "role":
                cfg.Role = value;
                break;
            case "sslmode":
            case "ssl mode":
                cfg.SslMode = value;
                break;
            case "allow_insecure":
            case "allowinsecure":
            case "allow_insecure_disable":
            case "allowinsecuredisable":
                cfg.AllowInsecureDisable = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "sslrootcert":
                cfg.SslRootCert = value;
                break;
            case "sslcert":
                cfg.SslCert = value;
                break;
            case "sslkey":
                cfg.SslKey = value;
                break;
            case "sslpassword":
                cfg.SslPassword = value;
                break;
            case "connect_timeout":
            case "connecttimeout":
            case "timeout":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var timeout))
                    cfg.ConnectTimeoutMs = timeout * 1000;
                break;
            case "socket_timeout":
            case "sockettimeout":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var sockTimeout))
                    cfg.SocketTimeoutMs = sockTimeout * 1000;
                break;
            case "application_name":
            case "applicationname":
                cfg.ApplicationName = value;
                break;
            case "binary_transfer":
            case "binarytransfer":
                cfg.BinaryTransfer = value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1";
                break;
            case "compression":
                cfg.Compression = value.Equals("zstd", StringComparison.OrdinalIgnoreCase) ? "zstd" : "off";
                break;
            case "client_flags":
            case "connect_client_flags":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var clientFlags))
                    cfg.ConnectClientFlags = clientFlags;
                break;
            case "auth_method_id":
            case "authmethodid":
                cfg.AuthMethodId = value.Trim();
                break;
            case "auth_method_payload":
            case "authmethodpayload":
                cfg.AuthMethodPayload = value;
                break;
            case "auth_token":
            case "authtoken":
            case "bearer_token":
            case "bearertoken":
            case "token":
                cfg.AuthToken = value;
                break;
            case "auth_payload_json":
            case "authpayloadjson":
                cfg.AuthPayloadJson = value;
                break;
            case "auth_payload_b64":
            case "authpayloadb64":
                cfg.AuthPayloadB64 = value;
                break;
            case "auth_provider_profile":
            case "authproviderprofile":
                cfg.AuthProviderProfile = value.Trim();
                break;
            case "auth_required_methods":
            case "authrequiredmethods":
                cfg.AuthRequiredMethods = value.Trim();
                break;
            case "auth_forbidden_methods":
            case "authforbiddenmethods":
                cfg.AuthForbiddenMethods = value.Trim();
                break;
            case "auth_require_channel_binding":
            case "authrequirechannelbinding":
                cfg.AuthRequireChannelBinding = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "workload_identity_token":
            case "workloadidentitytoken":
                cfg.WorkloadIdentityToken = value;
                break;
            case "proxy_principal_assertion":
            case "proxyprincipalassertion":
            case "proxy_assertion":
                cfg.ProxyPrincipalAssertion = value;
                break;
            case "dormant_id":
            case "dormantid":
                cfg.DormantId = value;
                break;
            case "dormant_reattach_token":
            case "dormantreattachtoken":
                cfg.DormantReattachToken = value;
                break;
            case "fetch_size":
            case "fetchsize":
            case "default_fetch_size":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var fetch))
                    cfg.DefaultFetchSize = Math.Max(0, fetch);
                break;
            case "pooling":
                cfg.Pooling = value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1";
                break;
            case "minpoolsize":
            case "minimumpoolsize":
            case "min_pool_size":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var minPool))
                    cfg.MinPoolSize = Math.Max(0, minPool);
                break;
            case "maxpoolsize":
            case "maximumpoolsize":
            case "max_pool_size":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var maxPool))
                    cfg.MaxPoolSize = Math.Max(1, maxPool);
                break;
            case "connectionlifetime":
            case "connection_lifetime":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var lifetime))
                    cfg.ConnectionLifetime = Math.Max(0, lifetime);
                break;
            case "poolacquiretimeout":
            case "pool_acquire_timeout":
            case "poolingacquiretimeout":
            case "pooling_acquire_timeout":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var acquireTimeoutSeconds))
                {
                    var nonNegativeSeconds = Math.Max(0, acquireTimeoutSeconds);
                    cfg.PoolAcquireTimeoutMs = (int)Math.Min(int.MaxValue, (long)nonNegativeSeconds * 1000L);
                }
                break;
            case "poolacquiretimeoutms":
            case "pool_acquire_timeout_ms":
            case "poolingacquiretimeoutms":
            case "pooling_acquire_timeout_ms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var acquireTimeoutMs))
                    cfg.PoolAcquireTimeoutMs = Math.Max(0, acquireTimeoutMs);
                break;
            case "cb_failure_threshold":
            case "circuitbreakerfailurethreshold":
            case "circuit_breaker_failure_threshold":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var cbFailureThreshold))
                    cfg.CircuitBreakerFailureThreshold = Math.Max(0, cbFailureThreshold);
                break;
            case "cb_recovery_timeout_ms":
            case "circuitbreakerrecoverytimeoutms":
            case "circuit_breaker_recovery_timeout_ms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var cbRecoveryTimeoutMs))
                    cfg.CircuitBreakerRecoveryTimeoutMs = Math.Max(1, cbRecoveryTimeoutMs);
                break;
            case "cb_success_threshold":
            case "circuitbreakersuccessthreshold":
            case "circuit_breaker_success_threshold":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var cbSuccessThreshold))
                    cfg.CircuitBreakerSuccessThreshold = Math.Max(1, cbSuccessThreshold);
                break;
            case "cb_half_open_max_requests":
            case "circuitbreakerhalfopenmaxrequests":
            case "circuit_breaker_half_open_max_requests":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var cbHalfOpenMax))
                    cfg.CircuitBreakerHalfOpenMaxRequests = Math.Max(1, cbHalfOpenMax);
                break;
            case "keepalive":
            case "keepalive_enabled":
                var keepaliveEnabled = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                if (!keepaliveEnabled)
                {
                    cfg.KeepaliveIntervalMs = 0;
                }
                break;
            case "keepalive_interval_ms":
            case "keepaliveintervalms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var keepaliveIntervalMs))
                    cfg.KeepaliveIntervalMs = Math.Max(0, keepaliveIntervalMs);
                break;
            case "keepalive_max_idle_before_check_ms":
            case "keepalive_max_idle_ms":
            case "keepalivemaxidlebeforecheckms":
            case "keepalivemaxidlems":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var keepaliveIdleMs))
                    cfg.KeepaliveMaxIdleBeforeCheckMs = Math.Max(0, keepaliveIdleMs);
                break;
            case "keepalive_validation_timeout_ms":
            case "keepalivevalidationtimeoutms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var keepaliveValidationTimeoutMs))
                    cfg.KeepaliveValidationTimeoutMs = Math.Max(0, keepaliveValidationTimeoutMs);
                break;
            case "pipeline_max_in_flight":
            case "pipelinemaxinflight":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var pipelineMaxInFlight))
                    cfg.PipelineMaxInFlight = Math.Max(0, pipelineMaxInFlight);
                break;
            case "pipeline_auto_flush":
            case "pipelineautoflush":
                cfg.PipelineAutoFlush = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "pipeline_auto_flush_threshold":
            case "pipelineautoflushthreshold":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var pipelineAutoFlushThreshold))
                    cfg.PipelineAutoFlushThreshold = Math.Max(1, pipelineAutoFlushThreshold);
                break;
            case "pipeline_flush_timeout_ms":
            case "pipelineflushtimeoutms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var pipelineFlushTimeoutMs))
                    cfg.PipelineFlushTimeoutMs = Math.Max(1, pipelineFlushTimeoutMs);
                break;
            case "leak_threshold_ms":
            case "leakthresholdms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var leakThresholdMs))
                    cfg.LeakThresholdMs = Math.Max(0, leakThresholdMs);
                break;
            case "leak_capture_stack":
            case "leak_capture_stack_trace":
            case "leakcapturestack":
            case "leakcapturestacktrace":
                cfg.LeakCaptureStackTrace = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "manager_auth_token":
            case "mcp_auth_token":
                cfg.ManagerAuthToken = value;
                break;
            case "manager_username":
            case "mcp_username":
                cfg.ManagerUsername = value;
                break;
            case "manager_database":
            case "mcp_database":
                cfg.ManagerDatabase = value;
                break;
            case "manager_connection_profile":
            case "mcp_connection_profile":
                cfg.ManagerConnectionProfile = value;
                break;
            case "manager_client_intent":
            case "mcp_client_intent":
                cfg.ManagerClientIntent = value;
                break;
            case "manager_client_flags":
            case "mcp_client_flags":
                if (ushort.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var managerFlags))
                    cfg.ManagerClientFlags = managerFlags;
                break;
            case "manager_auth_fast_path":
            case "mcp_auth_fast_path":
                cfg.ManagerAuthFastPath = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "telemetry":
            case "telemetry_enabled":
            case "telemetryenabletracing":
            case "telemetry_enable_tracing":
                cfg.TelemetryEnableTracing = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "telemetryenablemetrics":
            case "telemetry_enable_metrics":
            case "telemetry_metrics":
                cfg.TelemetryEnableMetrics = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "telemetryenableslowoperationlog":
            case "telemetry_enable_slow_operation_log":
            case "telemetry_enable_slow_query_log":
            case "telemetry_slow_query_log":
                cfg.TelemetryEnableSlowOperationLog = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
            case "telemetryslowoperationthresholdms":
            case "telemetry_slow_operation_threshold_ms":
            case "telemetry_slow_query_threshold_ms":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var slowThresholdMs))
                    cfg.TelemetrySlowOperationThresholdMs = Math.Max(0, slowThresholdMs);
                break;
            case "telemetryslowoperationmaxentries":
            case "telemetry_slow_operation_max_entries":
            case "telemetry_slow_query_max_entries":
                if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var slowMaxEntries))
                    cfg.TelemetrySlowOperationMaxEntries = Math.Max(0, slowMaxEntries);
                break;
            case "telemetrysamplerate":
            case "telemetry_sample_rate":
                if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var sampleRate))
                    cfg.TelemetrySampleRate = Math.Clamp(sampleRate, 0d, 1d);
                break;
            case "telemetrysanitizestatements":
            case "telemetry_sanitize_statements":
            case "telemetry_sanitize_queries":
                cfg.TelemetrySanitizeStatements = value.Equals("true", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("1", StringComparison.Ordinal)
                    || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
                    || value.Equals("on", StringComparison.OrdinalIgnoreCase);
                break;
        }
    }
}
