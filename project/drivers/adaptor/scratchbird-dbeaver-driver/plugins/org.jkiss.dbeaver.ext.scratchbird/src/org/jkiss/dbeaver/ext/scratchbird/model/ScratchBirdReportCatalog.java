// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;

import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class ScratchBirdReportCatalog {

    public static final String DASHBOARD_SESSIONS_ID = "scratchbird.sessions";
    public static final String DASHBOARD_SESSIONS_QUERY =
        "SELECT COUNT(*) AS \"Sessions\" FROM sys.sessions";
    public static final String DASHBOARD_TRANSACTIONS_ID = "scratchbird.transactions";
    public static final String DASHBOARD_TRANSACTIONS_QUERY =
        "SELECT COUNT(*) AS \"Transactions\" FROM sys.transactions";
    public static final String DASHBOARD_LOCKS_ID = "scratchbird.locks";
    public static final String DASHBOARD_LOCKS_QUERY =
        "SELECT COUNT(*) AS \"Locks\" FROM sys.locks";
    public static final String DASHBOARD_PERFORMANCE_ID = "scratchbird.performance";
    public static final String DASHBOARD_PERFORMANCE_QUERY = "SHOW METRICS";

    public static final List<String> METRICS_BRANCHES = List.of(
        "health-scorecards",
        "workload-and-sql",
        "sessions-and-transactions",
        "locks-and-contention",
        "storage-buffer-cache",
        "mga-and-gc",
        "scheduler-and-jobs",
        "security-and-auth",
        "listener-and-parser",
        "cluster-and-replication",
        "admin-and-management",
        "alerts",
        "future-gated"
    );

    private static final Map<String, ScratchBirdReportDefinition> REPORTS_BY_ID = new LinkedHashMap<>();

    static {
        report("SBDV-RPT-CORE-001", "Executive Health Scorecard", "health-scorecards", "SBDV-FRM-900",
            "KPI tiles plus sparklines", "database, current plus trend", "7 day sparklines",
            "connection saturation, query latency, scheduler failure, deadlock pressure",
            "global summary may require admin for full values",
            false, "SHOW METRICS", "sb_engine_connections_active", "sb_engine_sessions_active", "sb_tx_active");
        report("SBDV-RPT-CORE-002", "Query Workload Mix", "workload-and-sql", "SBDV-FRM-903",
            "stacked bar plus daily summary table", "database, query type, result, day", "30 days",
            "query error-rate spike", "label-aware raw metrics required", false,
            "scratchbird_queries_total", "sb_engine_queries_total", "scratchbird_query_errors_total");
        report("SBDV-RPT-CORE-003", "Query Latency And SLO", "workload-and-sql", "SBDV-FRM-903",
            "histogram plus p95 or p99 trend plus breach table", "database, query type, result, histogram bucket",
            "30 days", "query latency breach", "percentile mode requires raw histogram metrics", false,
            "scratchbird_query_duration_seconds_*", "sb_engine_query_duration_seconds_*", "SHOW METRICS");
        report("SBDV-RPT-CORE-004", "Sessions And Connections", "sessions-and-transactions", "SBDV-FRM-903",
            "live table plus utilization trend", "session, database, connection state", "live plus 24 hours",
            "connection saturation", "self versus superuser filtering applies", false,
            "sys.sessions", "scratchbird_connections_active", "sb_engine_sessions_active");
        report("SBDV-RPT-CORE-005", "Active Statements And Progress", "workload-and-sql", "SBDV-FRM-903",
            "top-N table plus progress bars", "statement, session, wait state", "live plus 1 hour",
            "stalled long-running query", "self versus superuser filtering applies", false,
            "sys.statements", "sys.sessions.current_query", "scratchbird_query_progress_last_update_micros");
        report("SBDV-RPT-CORE-006", "Transaction Health", "sessions-and-transactions", "SBDV-FRM-903",
            "trend plus live transaction table", "transaction, state, database, histogram bucket", "7 days",
            "transaction backlog or limbo growth", "MGA views may require elevated privileges", false,
            "sys.transactions", "sys.sb_mga_active_transactions", "sb_tx_active", "sb_tx_commit_fence_flush_seconds_*");
        report("SBDV-RPT-CORE-007", "Lock Contention And Deadlocks", "locks-and-contention", "SBDV-FRM-903",
            "heatmap plus blocker and wait table", "lock object, waiter, blocker, wait mode", "7 days",
            "lock wait or deadlock pressure", "self versus superuser filtering applies", false,
            "sys.locks", "sys.sb_mga_wait_history", "scratchbird_lock_wait_seconds_*", "sb_lock_deadlocks_total");
        report("SBDV-RPT-CORE-008", "COPY And Bulk Transfer", "workload-and-sql", "SBDV-FRM-903",
            "throughput line plus error table", "database, operation, result, histogram bucket", "30 days",
            "copy error spike", "raw metrics required for duration percentiles", false,
            "scratchbird_copy_rows_total", "scratchbird_copy_bytes_total", "scratchbird_copy_duration_seconds_*");
        report("SBDV-RPT-CORE-009", "Scheduler And Job Operations", "scheduler-and-jobs", "SBDV-FRM-900",
            "backlog line plus job run table", "job, queue, result, run", "30 days",
            "scheduler backlog or job failure", "job mutation routes through SBDV-FRM-102", false,
            "scratchbird_scheduler_queue_depth", "sys.jobs", "sys.job_runs", "sys.job_dependencies");
        report("SBDV-RPT-CORE-010", "Security Auth And Subsystem Events", "security-and-auth", "SBDV-FRM-900",
            "stacked bar plus audit table", "subsystem, event type, result", "90 days",
            "security/auth anomaly", "security details may require admin", false,
            "scratchbird_auth_legacy_method_total", "scratchbird_vnext_security_events_total");

        report("SBDV-RPT-STOR-001", "Buffer Pool Efficiency", "storage-buffer-cache", "SBDV-FRM-903",
            "line trend plus summary tiles", "database, buffer pool, interval", "30 days",
            "buffer hit ratio drop or dirty-page growth", "global view likely admin-oriented", false,
            "sys.buffer_pool_stats", "sys.sb_buffer_pool_stats", "scratchbird_buffer_pool_hits_total");
        report("SBDV-RPT-STOR-002", "Buffer Policy And Domain Health", "storage-buffer-cache", "SBDV-FRM-903",
            "domain table plus stacked area", "domain, page class, breach type", "30 days",
            "reserve breach or dirty-domain growth", "requires domain label support", false,
            "sys.sb_buffer_domain_stats", "sys.sb_buffer_policy_health", "sb_buf_domain_dirty_pages");
        report("SBDV-RPT-STOR-003", "Prefetch Effectiveness And Thrash Control", "storage-buffer-cache", "SBDV-FRM-903",
            "usefulness trend plus state table", "prefetch class, relation, state", "30 days",
            "prefetch thrash", "raw label detail preferred", false,
            "sys.sb_buffer_prefetch_health", "sb_buf_prefetch_usefulness_pct", "sb_buf_prefetch_thrash_state");
        report("SBDV-RPT-STOR-004", "Checkpoint And Writeback Pressure", "storage-buffer-cache", "SBDV-FRM-903",
            "status board plus queue trend", "checkpoint generation, queue, incident", "90 days",
            "checkpoint or writeback pressure", "current surfaces plus future history views", false,
            "sys.sb_checkpoint_writeback_pressure", "sb_checkpoint_state", "sb_buf_writeback_queue_depth");
        report("SBDV-RPT-STOR-005", "Disk I/O And Latency", "storage-buffer-cache", "SBDV-FRM-903",
            "throughput line plus latency histogram", "device, operation, histogram bucket", "30 days",
            "disk latency spike", "histogram samples required for percentile mode", false,
            "sys.io_stats", "scratchbird_disk_read_latency_seconds_*", "scratchbird_disk_write_latency_seconds_*");
        report("SBDV-RPT-STOR-006", "Cache Effectiveness", "storage-buffer-cache", "SBDV-FRM-903",
            "cache-family tiles plus detail table", "cache family, session, database", "30 days",
            "cache miss or eviction spike", "statement cache may be connection-local", false,
            "sys.cache_stats", "sys.statement_cache", "scratchbird_statement_cache_hits_total");
        report("SBDV-RPT-STOR-007", "Access Path And Index Health", "workload-and-sql", "SBDV-FRM-901",
            "mix chart plus top-N tables", "relation, index, scan type", "30 days",
            "hot leaf or index latency pressure", "object drilldown routes to index editor", false,
            "sys.table_stats", "scratchbird_index_scans_total", "SHOW INDEX ... HEALTH");
        report("SBDV-RPT-STOR-008", "MGA Cleanup Debt And Snapshot Blockers", "mga-and-gc", "SBDV-FRM-903",
            "relation table plus blocker list plus trend", "relation, transaction, blocker, bucket", "90 days",
            "MGA cleanup debt or long snapshot blocker", "preserve MGA terminology and avoid WAL framing", false,
            "sys.sb_mga_cleanup_debt", "sys.sb_mga_snapshot_blockers", "sb_mga_long_snapshot_count");
        report("SBDV-RPT-STOR-009", "GC And Sweep Operations", "mga-and-gc", "SBDV-FRM-903",
            "run history plus reclaim trend", "sweep generation, reclaim class", "90 days",
            "sweep stalled or reclaim debt growth", "future sb_sweep_resume_status improves this report", false,
            "MON_SWEEP", "MON_GARBAGE_COLLECTION", "sb_gc_background_reclaim_bytes_total");
        report("SBDV-RPT-STOR-010", "Recovery And Startup State", "mga-and-gc", "SBDV-FRM-903",
            "status card plus incident trend", "recovery class, generation, incident", "90 days",
            "recovery issue", "future recovery incident views improve RCA", false,
            "sb_recovery_classification_total", "sb_recovery_generation_current", "sb_recovery_repair_required_pages");

        report("SBDV-RPT-ADMIN-001", "Listener And Front-Door Health", "listener-and-parser", "SBDV-FRM-904",
            "accept, reject, open trend plus reason table", "listener, protocol, reason, histogram bucket", "7 days",
            "listener reject spike or queue saturation", "listener metrics are process-scoped", false,
            "scratchbird_listener_connections_total", "scratchbird_listener_reject_total", "SHOW MANAGEMENT LISTENERS");
        report("SBDV-RPT-ADMIN-002", "Parser Pool Capacity And Health", "listener-and-parser", "SBDV-FRM-904",
            "utilization table plus latency trend", "listener, protocol, parser pool, bucket", "7 days",
            "parser saturation or errors", "management views currently superuser-only", false,
            "scratchbird_parser_pool_busy", "scratchbird_parser_errors_total", "SHOW MANAGEMENT PARSER POOL");
        report("SBDV-RPT-ADMIN-003", "Replication And Shard Health", "cluster-and-replication", "SBDV-FRM-903",
            "lag trend plus conflict and cursor tables", "channel, shard, cursor, conflict", "30 days",
            "replication lag or conflict growth", "cluster permission model applies", false,
            "sys.replication_channel_status", "sys.shard_status", "sb_cluster_replication_lag_seconds");
        report("SBDV-RPT-ADMIN-004", "Cluster Routing And Admission", "cluster-and-replication", "SBDV-FRM-903",
            "policy and status tables", "cluster, route, admission result", "30 days",
            "fencing rejections or admission failures", "cluster views currently superuser-only", false,
            "SHOW CLUSTER ROUTING PLAN", "SHOW CLUSTER ADMISSION STATUS", "sb_cluster_routing_requests_total");
        report("SBDV-RPT-ADMIN-005", "SLO Error Budget And Autoscale", "cluster-and-replication", "SBDV-FRM-903",
            "SLO board plus burn-rate trend", "service, SLO, action, alert", "30 days",
            "SLO burn-rate or readiness breach", "cluster views currently superuser-only", false,
            "SHOW CLUSTER STATE SLO_STATUS", "SHOW CLUSTER STATE ERROR_BUDGET_STATUS");
        report("SBDV-RPT-ADMIN-006", "Management Drift And Control-Plane Compliance", "admin-and-management", "SBDV-FRM-900",
            "status tables", "manager, server, instruction, drift class", "30 days",
            "management drift", "management views currently superuser-only", false,
            "SHOW MANAGEMENT MANAGER", "SHOW MANAGEMENT SERVERS", "SHOW MANAGEMENT DRIFT");
        report("SBDV-RPT-ADMIN-007", "Migration And Connector Readiness", "admin-and-management", "SBDV-FRM-900",
            "readiness plus audit tables", "connector, migration, audit status", "90 days",
            "migration connector unhealthy", "may require admin for global connector state", false,
            "sys.migration_status", "sys.migration_audit_summary", "sys.prepared_statement");
        report("SBDV-RPT-ADMIN-008", "Plugin Module And Capability Inventory", "admin-and-management", "SBDV-FRM-900",
            "inventory tables", "plugin, module, capability", "current plus 7 days",
            "capability drift", "read-only inventory by default", false,
            "sys.plugin", "sys.server_capabilities");
        report("SBDV-RPT-ADMIN-009", "Support Bundle Safety And Readiness", "admin-and-management", "SBDV-FRM-900",
            "summary dashboard", "cluster, readiness component, safety check", "30 days",
            "support bundle unsafe or readiness breach", "cluster views currently superuser-only", false,
            "SHOW CLUSTER STATE SUPPORT_BUNDLE_SAFETY", "SHOW CLUSTER STATE READINESS_HEALTH");

        alert("SBDV-ALERT-001", "Query Latency Breach",
            "p95 or p99 latency threshold against query-duration histograms",
            "scratchbird_query_duration_seconds_*", "sb_engine_query_duration_seconds_*");
        alert("SBDV-ALERT-002", "Query Error-Rate Spike",
            "error-rate threshold comparing query errors to total query volume",
            "scratchbird_query_errors_total", "scratchbird_queries_total", "sb_engine_queries_total");
        alert("SBDV-ALERT-003", "Stalled Long-Running Query",
            "running-query age and progress-staleness threshold",
            "scratchbird_query_currently_running", "scratchbird_query_progress_last_update_micros",
            "sys.statements.elapsed_ms", "sys.sessions.wait_event");
        alert("SBDV-ALERT-004", "Lock Wait And Deadlock Pressure",
            "lock-wait duration, blocker count, and deadlock threshold",
            "scratchbird_lock_wait_seconds_*", "scratchbird_lock_deadlocks_total",
            "sb_lock_blockers", "sb_lock_wait_seconds_total", "sb_lock_deadlocks_total");
        alert("SBDV-ALERT-005", "Connection Or Front-Door Saturation",
            "connection, open-listener, and queue-depth saturation threshold",
            "scratchbird_connections_active", "scratchbird_connections_total",
            "scratchbird_listener_open_connections", "scratchbird_listener_queue_depth");
        alert("SBDV-ALERT-006", "Transaction Backlog Or Limbo Growth",
            "active, limbo, and restart-growth transaction threshold",
            "scratchbird_transactions_active", "sb_tx_active", "sb_tx_limbo", "sb_tx_restart_normalized_total");
        alert("SBDV-ALERT-007", "Buffer Hit Ratio Drop Or Dirty-Page Growth",
            "buffer hit-ratio and dirty-page pressure threshold",
            "scratchbird_buffer_pool_hits_total", "scratchbird_buffer_pool_misses_total",
            "scratchbird_buffer_pool_pages_dirty");
        alert("SBDV-ALERT-008", "Checkpoint Or Writeback Pressure",
            "checkpoint flush-debt, writeback queue, and incident threshold",
            "sb_checkpoint_flush_debt_pages", "sb_buf_writeback_queue_depth",
            "sb_buf_foreground_help_backlog_pages", "sb_writeback_incidents_open");
        alert("SBDV-ALERT-009", "Prefetch Thrash",
            "prefetch usefulness and cancelled/unused page threshold",
            "sb_buf_prefetch_usefulness_pct", "sb_buf_prefetch_pages_unused_evicted_total",
            "sb_buf_prefetch_cancelled_pages_total", "sb_buf_prefetch_thrash_state");
        alert("SBDV-ALERT-010", "MGA Cleanup Debt Or Long Snapshot Blocker",
            "MGA cleanup debt, retained-dead space, and snapshot-blocker threshold",
            "sb_gc_cleanup_debt_bytes", "sb_mga_retained_dead_bytes",
            "sb_mga_long_snapshot_count", "sys.sb_mga_snapshot_blockers");
        alert("SBDV-ALERT-011", "Replication Lag Or Conflict Growth",
            "replication lag and conflict-queue growth threshold",
            "sb_cluster_replication_lag_seconds", "sb_cluster_replication_lag_txn",
            "sys.replication_conflict_queue", "sys.replication_cursor_status");
        alert("SBDV-ALERT-012", "Scheduler Backlog Or Job Failure",
            "scheduler queue, job failure, and run-latency threshold",
            "scratchbird_scheduler_queue_depth", "scratchbird_scheduler_jobs_failed_total",
            "scratchbird_scheduler_job_run_latency_seconds_*", "sys.job_runs");
        alert("SBDV-ALERT-013", "Listener Reject Spike Or Parser Saturation",
            "listener reject, queue-wait, parser-busy, and parser-error threshold",
            "scratchbird_listener_reject_total", "scratchbird_listener_queue_wait_seconds_*",
            "scratchbird_parser_pool_busy", "scratchbird_parser_errors_total");
        alert("SBDV-ALERT-014", "Recovery Issue",
            "recovery repair, startup duration, and classification threshold",
            "sb_recovery_repair_required_pages", "sb_recovery_startup_seconds",
            "sb_recovery_classification_total");
        alert("SBDV-ALERT-015", "Migration Connector Unhealthy",
            "migration status and audit-summary health threshold",
            "sys.migration_status", "sys.migration_audit_summary");
        alert("SBDV-ALERT-016", "Security Or Auth Anomaly",
            "auth legacy-method and security event anomaly threshold",
            "scratchbird_auth_legacy_method_total", "scratchbird_vnext_security_events_total");

        report("SBDV-RPT-FUTURE-001", "Writeback Debt And Reserve Exhaustion", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_buffer_writeback_debt");
        report("SBDV-RPT-FUTURE-002", "Checkpoint Run History", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_checkpoint_history");
        report("SBDV-RPT-FUTURE-003", "Live Checkpoint Control", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_checkpoint_status");
        report("SBDV-RPT-FUTURE-004", "Recovery Incident RCA", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_recovery_incidents");
        report("SBDV-RPT-FUTURE-005", "Recovery Status", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_recovery_status");
        report("SBDV-RPT-FUTURE-006", "Sweep Resume Health", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_sweep_resume_status");
        report("SBDV-RPT-FUTURE-007", "Writeback Incident History", "future-gated", "SBDV-FRM-903",
            "future dashboard", "pending sys view", "future", "future alert", "blocked until sys view exists", true,
            "sb_writeback_incidents");
    }

    private ScratchBirdReportCatalog() {
    }

    @NotNull
    public static Collection<ScratchBirdReportDefinition> allReports() {
        return REPORTS_BY_ID.values();
    }

    @Nullable
    public static ScratchBirdReportDefinition findById(@NotNull String id) {
        return REPORTS_BY_ID.get(id);
    }

    @Nullable
    public static ScratchBirdReportDefinition findByNavigatorPath(@NotNull String path) {
        int separator = path.lastIndexOf('.');
        return separator < 0 ? null : findById(path.substring(separator + 1));
    }

    @NotNull
    public static List<ScratchBirdReportDefinition> reportsForNavigatorPath(@NotNull String path) {
        ScratchBirdReportDefinition report = findByNavigatorPath(path);
        if (report != null) {
            return List.of(report);
        }
        String branch = branchForNavigatorPath(path);
        return REPORTS_BY_ID.values().stream()
            .filter(candidate -> branch == null || candidate.branch().equals(branch))
            .toList();
    }

    @NotNull
    public static List<String> metricTreePaths() {
        List<String> paths = new ArrayList<>();
        paths.add(ScratchBirdNamespaceSemantics.METRICS_ROOT);
        for (String branch : METRICS_BRANCHES) {
            paths.add(ScratchBirdNamespaceSemantics.METRICS_ROOT + "." + branch);
        }
        for (ScratchBirdReportDefinition report : REPORTS_BY_ID.values()) {
            paths.add(report.navigatorPath());
        }
        return paths;
    }

    private static void report(
        @NotNull String id,
        @NotNull String title,
        @NotNull String branch,
        @NotNull String parentForm,
        @NotNull String bestOutput,
        @NotNull String aggregationGrain,
        @NotNull String defaultRetention,
        @NotNull String alertStarter,
        @NotNull String accessNotes,
        boolean futureGated,
        @NotNull String... sources
    ) {
        REPORTS_BY_ID.put(id, new ScratchBirdReportDefinition(
            id,
            title,
            branch,
            parentForm,
            bestOutput,
            List.of(sources),
            aggregationGrain,
            defaultRetention,
            alertStarter,
            accessNotes,
            futureGated));
    }

    private static void alert(
        @NotNull String id,
        @NotNull String title,
        @NotNull String alertStarter,
        @NotNull String... sources
    ) {
        report(id, title, "alerts", "SBDV-FRM-900",
            "alert badge plus drilldown report panel",
            "policy window, severity, source surface, and affected object where available",
            "local policy",
            alertStarter,
            "thresholds remain configurable; server-side permissions govern source visibility",
            false,
            sources);
    }

    @Nullable
    private static String branchForNavigatorPath(@NotNull String path) {
        String prefix = ScratchBirdNamespaceSemantics.METRICS_ROOT + ".";
        if (!path.startsWith(prefix)) {
            return null;
        }
        String rest = path.substring(prefix.length());
        int separator = rest.indexOf('.');
        String candidate = separator < 0 ? rest : rest.substring(0, separator);
        return METRICS_BRANCHES.contains(candidate) ? candidate : null;
    }
}
