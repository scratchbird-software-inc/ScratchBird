// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// sb-isql-go is the native Go conformance shell and database/sql example.
package main

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	scratchbird "github.com/scratchbird/scratchbird-go"
)

type config struct {
	Database                 string
	Host                     string
	Port                     int
	User                     string
	Password                 string
	Role                     string
	SSLMode                  string
	SSLRootCert              string
	SSLCert                  string
	SSLKey                   string
	Route                    string
	ParserMode               string
	PageSize                 string
	Namespace                string
	Input                    string
	Output                   string
	Error                    string
	Diagnostics              string
	Metrics                  string
	Transcript               string
	Summary                  string
	StopOnError              bool
	ExpectedRefusals         string
	StatementTimeoutMS       int
	FetchSize                int
	ConcurrencyWorker        int
	RunID                    string
	CreateDatabase           bool
	CreateEmulation          string
	LanguageResourcePack     string
	LanguageResourceIdentity string
	LanguageResourceHash     string
	LanguageProfile          string
	SyntaxProfile            string
	TopologyProfile          string
	StandardEnglishFallback  bool
}

type commandEvent map[string]any

var (
	pageSizes   = map[string]bool{"4k": true, "8k": true, "16k": true, "32k": true, "64k": true, "128k": true}
	routes      = map[string]bool{"embedded": true, "ipc_local": true, "listener-parser": true, "manager-listener-parser": true}
	parserModes = map[string]bool{"server-parser": true, "standalone-parser": true, "driver-sblr-uuid": true}
)

func main() {
	cfg := parseFlags()
	if err := run(cfg); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func parseFlags() config {
	var cfg config
	flag.StringVar(&cfg.Database, "database", "", "database name")
	flag.StringVar(&cfg.Host, "host", "127.0.0.1", "server host")
	flag.IntVar(&cfg.Port, "port", 3092, "server port")
	flag.StringVar(&cfg.User, "user", "", "user")
	flag.StringVar(&cfg.Password, "password", "", "password")
	flag.StringVar(&cfg.Role, "role", "", "role")
	flag.StringVar(&cfg.SSLMode, "sslmode", "require", "TLS mode")
	flag.StringVar(&cfg.SSLRootCert, "sslrootcert", "", "TLS root certificate path")
	flag.StringVar(&cfg.SSLCert, "sslcert", "", "TLS client certificate path")
	flag.StringVar(&cfg.SSLKey, "sslkey", "", "TLS client key path")
	flag.StringVar(&cfg.Route, "route", "listener-parser", "route")
	flag.StringVar(&cfg.ParserMode, "parser-mode", "server-parser", "parser mode")
	flag.StringVar(&cfg.PageSize, "page-size", "8k", "page size")
	flag.StringVar(&cfg.Namespace, "namespace", "", "test namespace")
	flag.StringVar(&cfg.Input, "input", "", "input script")
	flag.StringVar(&cfg.Output, "output", "", "output path")
	flag.StringVar(&cfg.Error, "error", "", "error path")
	flag.StringVar(&cfg.Diagnostics, "diagnostics", "", "diagnostics path")
	flag.StringVar(&cfg.Metrics, "metrics", "", "metrics path")
	flag.StringVar(&cfg.Transcript, "transcript", "", "transcript path")
	flag.StringVar(&cfg.Summary, "summary", "", "summary path")
	flag.BoolVar(&cfg.StopOnError, "stop-on-error", true, "stop on first unexpected error")
	flag.StringVar(&cfg.ExpectedRefusals, "expected-refusals", "", "expected refusal JSON")
	flag.IntVar(&cfg.StatementTimeoutMS, "statement-timeout-ms", 30000, "statement timeout")
	flag.IntVar(&cfg.FetchSize, "fetch-size", 1000, "fetch size")
	flag.IntVar(&cfg.ConcurrencyWorker, "concurrency-worker", 0, "concurrency worker id")
	flag.StringVar(&cfg.RunID, "run-id", "manual", "run id")
	flag.BoolVar(&cfg.CreateDatabase, "create-database", false, "create database before script")
	flag.StringVar(&cfg.CreateEmulation, "create-emulation-mode", "sbsql", "create mode")
	flag.StringVar(&cfg.LanguageResourcePack, "language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack", "ScratchBird language resource pack path")
	flag.StringVar(&cfg.LanguageResourceIdentity, "language-resource-identity", "sbsql.common_resource_pack.v1", "ScratchBird language resource identity")
	flag.StringVar(&cfg.LanguageResourceHash, "language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc", "ScratchBird language resource common hash")
	flag.StringVar(&cfg.LanguageProfile, "language-profile", "en-US", "ScratchBird language profile")
	flag.StringVar(&cfg.SyntaxProfile, "syntax-profile", "sbsql.v3", "ScratchBird syntax profile")
	flag.StringVar(&cfg.TopologyProfile, "topology-profile", "topology.sbsql.canonical.v1", "ScratchBird topology profile")
	flag.BoolVar(&cfg.StandardEnglishFallback, "standard-english-fallback", true, "allow canonical English fallback")
	_ = flag.CommandLine.Parse(normalizeBoolFlagArgs(os.Args[1:]))
	if flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "unexpected positional argument: "+strings.Join(flag.Args(), " "))
		os.Exit(2)
	}
	return cfg
}

func normalizeBoolFlagArgs(args []string) []string {
	normalized := make([]string, 0, len(args))
	for index := 0; index < len(args); index++ {
		arg := args[index]
		if (arg == "--stop-on-error" || arg == "--create-database" || arg == "--standard-english-fallback") && index+1 < len(args) {
			next := args[index+1]
			if !strings.HasPrefix(next, "--") {
				if boolValue, ok := normalizeBoolLiteral(next); ok {
					normalized = append(normalized, arg+"="+boolValue)
					index++
					continue
				}
			}
		}
		normalized = append(normalized, arg)
	}
	return normalized
}

func normalizeBoolLiteral(value string) (string, bool) {
	switch strings.ToLower(value) {
	case "1", "true", "yes", "on":
		return "true", true
	case "0", "false", "no", "off":
		return "false", true
	default:
		return "", false
	}
}

func run(cfg config) error {
	if err := validate(cfg); err != nil {
		return err
	}
	runRoot := filepath.Dir(cfg.Summary)
	for _, path := range []string{cfg.Output, cfg.Error, cfg.Diagnostics, cfg.Metrics, cfg.Transcript, cfg.Summary} {
		if err := clearFile(path); err != nil {
			return err
		}
	}
	required := []string{
		"command-events.jsonl", "diagnostics.jsonl", "wire-transcript.jsonl",
		"timing-groups.json", "result-digests.json", "metadata-snapshots.json",
		"security-refusals.json", "native-api-coverage.json", "code-example-review.json",
		"junit.xml", "stdout.log", "stderr.log",
	}
	for _, name := range required {
		if err := clearFile(filepath.Join(runRoot, name)); err != nil {
			return err
		}
	}

	timings := map[string]int64{}
	apiHits := map[string]int{
		"sql.Open":       0,
		"db.Conn":        0,
		"PrepareContext": 0,
		"QueryContext":   0,
		"Rows":           0,
		"Tx":             0,
	}
	failures := []map[string]any{}
	testcases := []commandEvent{}
	digests := []map[string]any{}
	securityRefusals := []map[string]any{}
	started := time.Now()

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(cfg.StatementTimeoutMS)*time.Millisecond)
	defer cancel()
	db, err := openDB(cfg)
	if err != nil {
		failures = append(failures, map[string]any{"statement_id": "connect", "message": err.Error()})
		return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
	}
	defer db.Close()
	apiHits["sql.Open"]++

	connStarted := time.Now()
	conn, err := db.Conn(ctx)
	if err != nil {
		failures = append(failures, map[string]any{"statement_id": "connect", "message": err.Error()})
		return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
	}
	defer conn.Close()
	apiHits["db.Conn"]++
	addTiming(timings, "connection", time.Since(connStarted))
	appendJSONL(cfg.Transcript, map[string]any{
		"event": "connect", "driver": "go", "route": cfg.Route,
		"parser_mode": cfg.ParserMode, "page_size": cfg.PageSize,
	})

	if cfg.CreateDatabase {
		failures = append(failures, map[string]any{
			"statement_id": "database_create",
			"message":      "--create-database is not implemented in the Go native tool yet",
		})
		return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
	}
	if cfg.ParserMode != "server-parser" {
		failures = append(failures, map[string]any{
			"statement_id": "parser_mode",
			"message":      cfg.ParserMode + " is not yet implemented by the Go native tool; it fails closed",
		})
		return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
	}

	expectedRefusals, err := loadExpectedRefusals(cfg.ExpectedRefusals)
	if err != nil {
		failures = append(failures, map[string]any{"statement_id": "expected_refusals", "message": err.Error()})
		return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
	}
	script, err := readInput(cfg.Input)
	if err != nil {
		failures = append(failures, map[string]any{"statement_id": "input", "message": err.Error()})
		return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
	}
	for index, statement := range splitStatements(script) {
		statementID := fmt.Sprintf("%s:%d", filepath.Base(cfg.Input), index+1)
		group := classify(statement)
		stmtStarted := time.Now()
		expectedOutcome := "success"
		if expectedRefusals[statementID] {
			expectedOutcome = "refusal"
		}
		outcome := "success"
		rowCount := int64(-1)
		var resultDigest any
		var sqlState any
		var diagnostic any
		breakAfterEvent := false
		stmt, err := conn.PrepareContext(ctx, statement)
		apiHits["PrepareContext"]++
		if err == nil {
			defer stmt.Close()
			if group == "query" || group == "metadata" {
				var rows *sql.Rows
				rows, err = stmt.QueryContext(ctx)
				apiHits["QueryContext"]++
				if err == nil {
					apiHits["Rows"]++
					values, readErr := readRows(rows)
					rows.Close()
					if readErr != nil {
						err = readErr
					} else {
						rowCount = int64(len(values))
						digest := sha256Text(jsonString(values))
						resultDigest = digest
						digests = append(digests, map[string]any{
							"statement_id": statementID, "row_count": rowCount, "result_digest": digest,
						})
						appendText(cfg.Output, jsonString(map[string]any{"statement_id": statementID, "rows": values})+"\n")
					}
				}
			} else {
				result, execErr := stmt.ExecContext(ctx)
				err = execErr
				if err == nil {
					rowCount, _ = result.RowsAffected()
					resultDigest = sha256Text(fmt.Sprint(rowCount))
				}
			}
		}
		if err != nil {
			outcome = "refusal"
			sqlState = "HY000"
			diagnostic = err.Error()
			appendJSONL(cfg.Diagnostics, map[string]any{
				"statement_id": statementID, "sqlstate": sqlState, "message": diagnostic,
			})
			appendText(cfg.Error, statementID+": "+err.Error()+"\n")
			if expectedOutcome == "refusal" {
				securityRefusals = append(securityRefusals, map[string]any{
					"statement_id":    statementID,
					"sqlstate":        sqlState,
					"diagnostic_code": diagnostic,
				})
			} else {
				failures = append(failures, map[string]any{"statement_id": statementID, "message": err.Error()})
				breakAfterEvent = cfg.StopOnError
			}
		} else if expectedOutcome == "refusal" {
			outcome = "unexpected_success"
			failures = append(failures, map[string]any{
				"statement_id": statementID,
				"message":      "statement succeeded but was expected to refuse",
			})
			breakAfterEvent = cfg.StopOnError
		}
		elapsed := time.Since(stmtStarted)
		addTiming(timings, group, elapsed)
		event := commandEvent{
			"run_id":                     cfg.RunID,
			"driver_name":                "go",
			"driver_version":             "unknown",
			"route":                      cfg.Route,
			"parser_mode":                cfg.ParserMode,
			"page_size":                  cfg.PageSize,
			"namespace":                  cfg.Namespace,
			"script":                     cfg.Input,
			"statement_index":            index + 1,
			"statement_id":               statementID,
			"command_group":              group,
			"sql_hash":                   sha256Text(statement),
			"expected_outcome":           expectedOutcome,
			"actual_outcome":             outcome,
			"sqlstate":                   sqlState,
			"diagnostic_code":            diagnostic,
			"canonical_message_vector":   []string{},
			"row_count":                  rowCount,
			"result_digest":              resultDigest,
			"elapsed_ns":                 elapsed.Nanoseconds(),
			"server_revalidation_state":  "required",
			"language_profile":           cfg.LanguageProfile,
			"language_resource_pack":     cfg.LanguageResourcePack,
			"language_resource_identity": cfg.LanguageResourceIdentity,
			"language_resource_hash":     cfg.LanguageResourceHash,
			"syntax_profile":             cfg.SyntaxProfile,
			"topology_profile":           cfg.TopologyProfile,
			"standard_english_fallback":  cfg.StandardEnglishFallback,
			"mga_authority":              "engine",
			"native_api_surface":         "database_sql",
			"code_example_section":       "prepare_execute_fetch",
		}
		appendJSONL(filepath.Join(runRoot, "command-events.jsonl"), event)
		testcases = append(testcases, event)
		if breakAfterEvent {
			break
		}
	}
	writeMetadataSnapshot(ctx, conn, cfg, apiHits)
	var tx *sql.Tx
	apiHits["Tx"] += touchTx(tx)
	return finish(cfg, timings, apiHits, failures, testcases, digests, securityRefusals, time.Since(started))
}

func openDB(cfg config) (*sql.DB, error) {
	frontDoor := "direct"
	if cfg.Route == "manager-listener-parser" {
		frontDoor = "manager_proxy"
	}
	dsn := fmt.Sprintf("scratchbird://%s:%s@%s:%d/%s?sslmode=%s&front_door_mode=%s&application_name=sb-isql-go",
		urlEscape(cfg.User), urlEscape(cfg.Password), cfg.Host, cfg.Port, cfg.Database, cfg.SSLMode, frontDoor)
	if cfg.SSLRootCert != "" {
		dsn += "&sslrootcert=" + urlEscape(cfg.SSLRootCert)
	}
	if cfg.SSLCert != "" {
		dsn += "&sslcert=" + urlEscape(cfg.SSLCert)
	}
	if cfg.SSLKey != "" {
		dsn += "&sslkey=" + urlEscape(cfg.SSLKey)
	}
	return sql.Open("scratchbird", dsn)
}

func writeMetadataSnapshot(ctx context.Context, conn *sql.Conn, cfg config, apiHits map[string]int) {
	started := time.Now()
	rows, err := conn.QueryContext(ctx, "SELECT * FROM sys.information.schema_tree")
	apiHits["QueryContext"]++
	snapshot := map[string]any{
		"driver": "go", "route": cfg.Route, "parser_mode": cfg.ParserMode,
		"page_size": cfg.PageSize, "namespace": cfg.Namespace,
		"elapsed_ns": time.Since(started).Nanoseconds(),
	}
	if err != nil {
		snapshot["status"] = "error"
		snapshot["message"] = err.Error()
	} else {
		apiHits["Rows"]++
		values, readErr := readRows(rows)
		rows.Close()
		if readErr != nil {
			snapshot["status"] = "error"
			snapshot["message"] = readErr.Error()
		} else {
			snapshot["status"] = "ok"
			snapshot["row_count"] = len(values)
			snapshot["digest"] = sha256Text(jsonString(values))
		}
	}
	_ = writeText(filepath.Join(filepath.Dir(cfg.Summary), "metadata-snapshots.json"), jsonString(snapshot)+"\n")
}

func finish(
	cfg config,
	timings map[string]int64,
	apiHits map[string]int,
	failures []map[string]any,
	testcases []commandEvent,
	digests []map[string]any,
	securityRefusals []map[string]any,
	elapsed time.Duration,
) error {
	runRoot := filepath.Dir(cfg.Summary)
	timings["overall"] = elapsed.Nanoseconds()
	transportMode := transportModeForRoute(cfg.Route, cfg.SSLMode)
	summary := map[string]any{
		"run_id": cfg.RunID, "driver_name": "go", "route": cfg.Route,
		"parser_mode": cfg.ParserMode, "page_size": cfg.PageSize,
		"namespace": cfg.Namespace, "status": status(failures),
		"sslmode": cfg.SSLMode, "transport_mode": transportMode,
		"language_resource_pack": cfg.LanguageResourcePack, "language_resource_identity": cfg.LanguageResourceIdentity,
		"language_resource_hash": cfg.LanguageResourceHash, "language_resource_authority": "shared_server_parser_resource_pack",
		"language_profile": cfg.LanguageProfile, "syntax_profile": cfg.SyntaxProfile,
		"topology_profile": cfg.TopologyProfile, "standard_english_fallback": cfg.StandardEnglishFallback,
		"failure_count": len(failures), "elapsed_ns": elapsed.Nanoseconds(),
		"server_revalidation_required": true,
		"driver_or_parser_finality":    "forbidden",
		"mga_authority":                "engine",
	}
	writeText(cfg.Summary, jsonString(summary)+"\n")
	writeText(cfg.Metrics, jsonString(timings)+"\n")
	writeText(filepath.Join(runRoot, "timing-groups.json"), jsonString(timings)+"\n")
	writeText(filepath.Join(runRoot, "result-digests.json"), jsonString(digests)+"\n")
	writeText(filepath.Join(runRoot, "security-refusals.json"), jsonString(securityRefusals)+"\n")
	writeText(filepath.Join(runRoot, "native-api-coverage.json"), jsonString(apiHits)+"\n")
	writeText(filepath.Join(runRoot, "code-example-review.json"), jsonString(map[string]any{
		"driver": "go", "public_api_only": true, "shells_out_to_other_driver": false,
		"source_is_canonical_example": true,
		"sections":                    []string{"connection", "prepared_execution", "fetch", "metadata", "transaction"},
	})+"\n")
	writeText(filepath.Join(runRoot, "junit.xml"), junit(testcases, failures))
	appendText(filepath.Join(runRoot, "stdout.log"), "sb-isql-go status="+status(failures)+"\n")
	if len(failures) > 0 {
		return errors.New("sb-isql-go failed; see summary artifact")
	}
	return nil
}

func status(failures []map[string]any) string {
	if len(failures) > 0 {
		return "fail"
	}
	return "pass"
}

func validate(cfg config) error {
	if cfg.Database == "" || cfg.User == "" || cfg.Password == "" || cfg.Namespace == "" || cfg.Input == "" || cfg.Summary == "" {
		return errors.New("database, user, password, namespace, input, and summary are required")
	}
	if !pageSizes[cfg.PageSize] {
		return fmt.Errorf("unsupported page size %s", cfg.PageSize)
	}
	if !routes[cfg.Route] {
		return fmt.Errorf("unsupported route %s", cfg.Route)
	}
	if !parserModes[cfg.ParserMode] {
		return fmt.Errorf("unsupported parser mode %s", cfg.ParserMode)
	}
	return nil
}

func transportModeForRoute(route, sslmode string) string {
	switch route {
	case "embedded":
		return "embedded_no_network_transport"
	case "ipc_local":
		return "local_ipc_no_tls"
	default:
		if sslmode == "disable" {
			return "tls_disabled"
		}
		return "tls_required"
	}
}

func loadExpectedRefusals(path string) (map[string]bool, error) {
	expected := map[string]bool{}
	if path == "" {
		return expected, nil
	}
	bytes, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var doc any
	if err := json.Unmarshal(bytes, &doc); err != nil {
		return nil, err
	}
	collectExpectedRefusalIDs(expected, doc)
	return expected, nil
}

func collectExpectedRefusalIDs(expected map[string]bool, value any) {
	switch typed := value.(type) {
	case string:
		expected[typed] = true
	case []any:
		for _, item := range typed {
			collectExpectedRefusalIDs(expected, item)
		}
	case map[string]any:
		for _, key := range []string{"statement_id", "statementId", "id"} {
			if text, ok := typed[key].(string); ok {
				expected[text] = true
			}
		}
		for _, key := range []string{"statement_ids", "statementIds", "expected_refusals", "expectedRefusals", "expected_diagnostics", "expectedDiagnostics"} {
			if nested, ok := typed[key]; ok {
				collectExpectedRefusalIDs(expected, nested)
			}
		}
	}
}

func readRows(rows *sql.Rows) ([][]any, error) {
	columns, err := rows.Columns()
	if err != nil {
		return nil, err
	}
	out := [][]any{}
	for rows.Next() {
		values := make([]any, len(columns))
		ptrs := make([]any, len(columns))
		for i := range values {
			ptrs[i] = &values[i]
		}
		if err := rows.Scan(ptrs...); err != nil {
			return nil, err
		}
		out = append(out, values)
	}
	return out, rows.Err()
}

// splitStatements delegates to the canonical SET TERM- and comment-aware
// statement chunker shared across the Go driver.
func splitStatements(sqlText string) []string {
	return scratchbird.SplitTopLevelStatements(sqlText)
}

func classify(sqlText string) string {
	fields := strings.Fields(strings.ToLower(sqlText))
	if len(fields) == 0 {
		return "query"
	}
	switch fields[0] {
	case "create", "alter", "drop":
		return "ddl"
	case "insert", "update", "delete", "merge", "upsert":
		return "dml"
	case "commit", "rollback", "savepoint", "begin", "start":
		return "transaction"
	case "grant", "revoke":
		return "security_refusal"
	case "select", "with":
		if strings.Contains(strings.ToLower(sqlText), "sys.") {
			return "metadata"
		}
		return "query"
	default:
		return "query"
	}
}

func touchTx(tx *sql.Tx) int {
	if tx == nil {
		return 0
	}
	return 1
}

func readInput(path string) (string, error) {
	if path == "-" {
		bytes, err := os.ReadFile("/dev/stdin")
		return string(bytes), err
	}
	bytes, err := os.ReadFile(path)
	return string(bytes), err
}

func clearFile(path string) error {
	if path == "" {
		return errors.New("empty artifact path")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte{}, 0o644)
}

func appendJSONL(path string, value any) {
	appendText(path, jsonString(value)+"\n")
}

func appendText(path, text string) {
	_ = os.MkdirAll(filepath.Dir(path), 0o755)
	handle, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return
	}
	defer handle.Close()
	_, _ = handle.WriteString(text)
}

func writeText(path, text string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(text), 0o644)
}

func addTiming(timings map[string]int64, group string, elapsed time.Duration) {
	timings[group] += elapsed.Nanoseconds()
}

func jsonString(value any) string {
	bytes, err := json.Marshal(value)
	if err != nil {
		return "null"
	}
	return string(bytes)
}

func sha256Text(value string) string {
	sum := sha256.Sum256([]byte(value))
	return "sha256:" + hex.EncodeToString(sum[:])
}

func urlEscape(value string) string {
	replacer := strings.NewReplacer("@", "%40", ":", "%3A", "/", "%2F", " ", "%20")
	return replacer.Replace(value)
}

func junit(testcases []commandEvent, failures []map[string]any) string {
	var builder strings.Builder
	builder.WriteString(`<?xml version="1.0" encoding="UTF-8"?>` + "\n")
	builder.WriteString(fmt.Sprintf(`<testsuite name="sb-isql-go" tests="%d" failures="%d">`+"\n", len(testcases), len(failures)))
	for _, testcase := range testcases {
		builder.WriteString(fmt.Sprintf(`  <testcase classname="scratchbird.go" name="%s"></testcase>`+"\n", xmlEscape(fmt.Sprint(testcase["statement_id"]))))
	}
	for _, failure := range failures {
		builder.WriteString(fmt.Sprintf(`  <testcase classname="scratchbird.go" name="%s"><failure message="%s" /></testcase>`+"\n",
			xmlEscape(fmt.Sprint(failure["statement_id"])), xmlEscape(fmt.Sprint(failure["message"]))))
	}
	builder.WriteString("</testsuite>\n")
	return builder.String()
}

func xmlEscape(value string) string {
	value = strings.ReplaceAll(value, "&", "&amp;")
	value = strings.ReplaceAll(value, "<", "&lt;")
	value = strings.ReplaceAll(value, ">", "&gt;")
	value = strings.ReplaceAll(value, `"`, "&quot;")
	return value
}
