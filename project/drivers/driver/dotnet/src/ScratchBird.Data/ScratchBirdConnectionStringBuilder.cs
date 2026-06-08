// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data.Common;
using System.Globalization;

namespace ScratchBird.Data;

public sealed class ScratchBirdConnectionStringBuilder : DbConnectionStringBuilder
{
    public string Host
    {
        get => GetString("Host", "localhost");
        set => this["Host"] = value;
    }

    public int Port
    {
        get => GetInt("Port", 3092);
        set => this["Port"] = value;
    }

    public string Database
    {
        get => GetString("Database", string.Empty);
        set => this["Database"] = value;
    }

    public string Protocol
    {
        get => GetString("Protocol", "native");
        set => this["Protocol"] = value;
    }

    public string FrontDoorMode
    {
        get => GetString("Front_Door_Mode", "direct");
        set => this["Front_Door_Mode"] = value;
    }

    public string Username
    {
        get => GetString("Username", string.Empty);
        set => this["Username"] = value;
    }

    public string Password
    {
        get => GetString("Password", string.Empty);
        set => this["Password"] = value;
    }

    public string AuthMethodId
    {
        get => GetString("Auth_Method_Id", string.Empty);
        set => this["Auth_Method_Id"] = value;
    }

    public string AuthMethodPayload
    {
        get => GetString("Auth_Method_Payload", string.Empty);
        set => this["Auth_Method_Payload"] = value;
    }

    public string AuthToken
    {
        get => GetString("Auth_Token", string.Empty);
        set => this["Auth_Token"] = value;
    }

    public string AuthPayloadJson
    {
        get => GetString("Auth_Payload_Json", string.Empty);
        set => this["Auth_Payload_Json"] = value;
    }

    public string AuthPayloadB64
    {
        get => GetString("Auth_Payload_B64", string.Empty);
        set => this["Auth_Payload_B64"] = value;
    }

    public string AuthProviderProfile
    {
        get => GetString("Auth_Provider_Profile", string.Empty);
        set => this["Auth_Provider_Profile"] = value;
    }

    public string AuthRequiredMethods
    {
        get => GetString("Auth_Required_Methods", string.Empty);
        set => this["Auth_Required_Methods"] = value;
    }

    public string AuthForbiddenMethods
    {
        get => GetString("Auth_Forbidden_Methods", string.Empty);
        set => this["Auth_Forbidden_Methods"] = value;
    }

    public bool AuthRequireChannelBinding
    {
        get => GetBool("Auth_Require_Channel_Binding", false);
        set => this["Auth_Require_Channel_Binding"] = value;
    }

    public string WorkloadIdentityToken
    {
        get => GetString("Workload_Identity_Token", string.Empty);
        set => this["Workload_Identity_Token"] = value;
    }

    public string ProxyPrincipalAssertion
    {
        get => GetString("Proxy_Principal_Assertion", string.Empty);
        set => this["Proxy_Principal_Assertion"] = value;
    }

    public string ManagerAuthToken
    {
        get => GetString("Manager_Auth_Token", string.Empty);
        set => this["Manager_Auth_Token"] = value;
    }

    public string Schema
    {
        get => GetString("Schema", string.Empty);
        set => this["Schema"] = value;
    }

    public string SSLMode
    {
        get => GetString("SSLMode", "require");
        set => this["SSLMode"] = value;
    }

    public bool AllowInsecure
    {
        get => GetBool("AllowInsecure", false);
        set => this["AllowInsecure"] = value;
    }

    public int Timeout
    {
        get => GetInt("Timeout", 30);
        set => this["Timeout"] = value;
    }

    public int CommandTimeout
    {
        get => GetInt("CommandTimeout", 30);
        set => this["CommandTimeout"] = value;
    }

    public int FetchSize
    {
        get => GetInt("FetchSize", 0);
        set => this["FetchSize"] = value;
    }

    public bool Pooling
    {
        get => GetBool("Pooling", true);
        set => this["Pooling"] = value;
    }

    public int MinPoolSize
    {
        get => GetInt("MinPoolSize", 0);
        set => this["MinPoolSize"] = value;
    }

    public int MaxPoolSize
    {
        get => GetInt("MaxPoolSize", 100);
        set => this["MaxPoolSize"] = value;
    }

    public int ConnectionLifetime
    {
        get => GetInt("ConnectionLifetime", 0);
        set => this["ConnectionLifetime"] = value;
    }

    public int PoolAcquireTimeoutMs
    {
        get => GetInt("PoolAcquireTimeoutMs", 250);
        set => this["PoolAcquireTimeoutMs"] = value;
    }

    public int CircuitBreakerFailureThreshold
    {
        get => GetInt("cb_failure_threshold", 0);
        set => this["cb_failure_threshold"] = value;
    }

    public int CircuitBreakerRecoveryTimeoutMs
    {
        get => GetInt("cb_recovery_timeout_ms", 30000);
        set => this["cb_recovery_timeout_ms"] = value;
    }

    public int CircuitBreakerSuccessThreshold
    {
        get => GetInt("cb_success_threshold", 2);
        set => this["cb_success_threshold"] = value;
    }

    public int CircuitBreakerHalfOpenMaxRequests
    {
        get => GetInt("cb_half_open_max_requests", 1);
        set => this["cb_half_open_max_requests"] = value;
    }

    public int KeepaliveIntervalMs
    {
        get => GetInt("keepalive_interval_ms", 120000);
        set => this["keepalive_interval_ms"] = value;
    }

    public int KeepaliveMaxIdleBeforeCheckMs
    {
        get => GetInt("keepalive_max_idle_before_check_ms", 600000);
        set => this["keepalive_max_idle_before_check_ms"] = value;
    }

    public int KeepaliveValidationTimeoutMs
    {
        get => GetInt("keepalive_validation_timeout_ms", 5000);
        set => this["keepalive_validation_timeout_ms"] = value;
    }

    public int LeakThresholdMs
    {
        get => GetInt("leak_threshold_ms", 30000);
        set => this["leak_threshold_ms"] = value;
    }

    public int PipelineMaxInFlight
    {
        get => GetInt("pipeline_max_in_flight", 100);
        set => this["pipeline_max_in_flight"] = value;
    }

    public bool PipelineAutoFlush
    {
        get => GetBool("pipeline_auto_flush", true);
        set => this["pipeline_auto_flush"] = value;
    }

    public int PipelineAutoFlushThreshold
    {
        get => GetInt("pipeline_auto_flush_threshold", 10);
        set => this["pipeline_auto_flush_threshold"] = value;
    }

    public int PipelineFlushTimeoutMs
    {
        get => GetInt("pipeline_flush_timeout_ms", 5000);
        set => this["pipeline_flush_timeout_ms"] = value;
    }

    public bool LeakCaptureStackTrace
    {
        get => GetBool("leak_capture_stack", false);
        set => this["leak_capture_stack"] = value;
    }

    public bool Enlist
    {
        get => GetBool("Enlist", true);
        set => this["Enlist"] = value;
    }

    public bool TelemetryEnableTracing
    {
        get => GetBool("TelemetryEnableTracing", true);
        set => this["TelemetryEnableTracing"] = value;
    }

    public bool TelemetryEnableMetrics
    {
        get => GetBool("TelemetryEnableMetrics", true);
        set => this["TelemetryEnableMetrics"] = value;
    }

    public bool TelemetryEnableSlowOperationLog
    {
        get => GetBool("TelemetryEnableSlowOperationLog", true);
        set => this["TelemetryEnableSlowOperationLog"] = value;
    }

    public int TelemetrySlowOperationThresholdMs
    {
        get => GetInt("TelemetrySlowOperationThresholdMs", 1000);
        set => this["TelemetrySlowOperationThresholdMs"] = value;
    }

    public int TelemetrySlowOperationMaxEntries
    {
        get => GetInt("TelemetrySlowOperationMaxEntries", 100);
        set => this["TelemetrySlowOperationMaxEntries"] = value;
    }

    public double TelemetrySampleRate
    {
        get => GetDouble("TelemetrySampleRate", 1d);
        set => this["TelemetrySampleRate"] = value;
    }

    public bool TelemetrySanitizeStatements
    {
        get => GetBool("TelemetrySanitizeStatements", true);
        set => this["TelemetrySanitizeStatements"] = value;
    }

    public override string ToString()
    {
        return ConnectionString;
    }

    private string GetString(string key, string fallback)
    {
        return TryGetValue(key, out var value) ? Convert.ToString(value) ?? fallback : fallback;
    }

    private int GetInt(string key, int fallback)
    {
        if (TryGetValue(key, out var value))
        {
            if (value is int i)
            {
                return i;
            }
            if (int.TryParse(Convert.ToString(value), out var parsed))
            {
                return parsed;
            }
        }
        return fallback;
    }

    private bool GetBool(string key, bool fallback)
    {
        if (TryGetValue(key, out var value))
        {
            if (value is bool b)
            {
                return b;
            }
            if (bool.TryParse(Convert.ToString(value), out var parsed))
            {
                return parsed;
            }
        }
        return fallback;
    }

    private double GetDouble(string key, double fallback)
    {
        if (TryGetValue(key, out var value))
        {
            if (value is double d)
            {
                return d;
            }
            if (double.TryParse(
                Convert.ToString(value),
                NumberStyles.Float | NumberStyles.AllowThousands,
                CultureInfo.InvariantCulture,
                out var parsed))
            {
                return parsed;
            }
        }
        return fallback;
    }
}
