// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package conformance

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	scratchbird "github.com/scratchbird/scratchbird-go"
)

type Manifest struct {
	SchemaVersion   string     `json:"schema_version"`
	ProtocolVersion string     `json:"protocol_version"`
	Suite           string     `json:"suite"`
	Fixtures        []string   `json:"fixtures"`
	Requires        []string   `json:"requires"`
	Tests           []TestSpec `json:"tests"`
}

type TestSpec struct {
	ID             string        `json:"id"`
	Kind           string        `json:"kind"`
	SQL            string        `json:"sql,omitempty"`
	Params         []interface{} `json:"params,omitempty"`
	ExpectRows     *int          `json:"expect_rows,omitempty"`
	ExpectSQLState string        `json:"expect_sqlstate,omitempty"`
	TimeoutMs      *int          `json:"timeout_ms,omitempty"`
	DsnAppend      string        `json:"dsn_append,omitempty"`
	Requires       []string      `json:"requires,omitempty"`
	CancelAfter    *int          `json:"cancel_after_rows,omitempty"`
}

type TestResult struct {
	TestID  string   `json:"test_id"`
	Status  string   `json:"status"`
	Rows    [][]any  `json:"rows,omitempty"`
	Columns []string `json:"columns,omitempty"`
	Errors  []string `json:"errors,omitempty"`
}

type Summary struct {
	SchemaVersion   string       `json:"schema_version"`
	ProtocolVersion string       `json:"protocol_version"`
	Suite           string       `json:"suite"`
	Results         []TestResult `json:"results"`
}

func LoadManifest(path string) (*Manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var manifest Manifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, err
	}
	return &manifest, nil
}

func RunManifest(ctx context.Context, dsn, manifestPath string) (*Summary, error) {
	manifest, err := LoadManifest(manifestPath)
	if err != nil {
		return nil, err
	}
	fixtureDir := filepath.Dir(manifestPath)
	return Run(ctx, dsn, fixtureDir, manifest)
}

func Run(ctx context.Context, dsn, fixtureDir string, manifest *Manifest) (*Summary, error) {
	if manifest == nil {
		return nil, errors.New("manifest is required")
	}
	if dsn == "" {
		return nil, errors.New("dsn is required")
	}
	results, err := runTests(ctx, dsn, fixtureDir, manifest.Fixtures, manifest.Requires, manifest.Tests)
	if err != nil {
		return nil, err
	}
	return &Summary{
		SchemaVersion:   manifest.SchemaVersion,
		ProtocolVersion: manifest.ProtocolVersion,
		Suite:           manifest.Suite,
		Results:         results,
	}, nil
}

func applyFixtures(ctx context.Context, db *sql.DB, fixtureDir string, fixtures []string) error {
	if len(fixtures) == 0 {
		return nil
	}
	for _, fixture := range fixtures {
		path := filepath.Join(fixtureDir, fixture)
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		statements := splitSQLStatements(string(data))
		for _, statement := range statements {
			if tableName, ok := createdTableName(statement); ok && tableExists(ctx, db, tableName) {
				continue
			}
			if _, err := db.ExecContext(ctx, statement); err != nil {
				if isAlreadyExistsError(err) {
					continue
				}
				return err
			}
		}
	}
	return nil
}

func runTests(ctx context.Context,
	dsn, fixtureDir string,
	fixtures []string,
	manifestRequires []string,
	tests []TestSpec) ([]TestResult, error) {
	results := make([]TestResult, 0, len(tests))
	baseDB, err := sql.Open("scratchbird", dsn)
	if err != nil {
		return nil, err
	}
	configureDB(baseDB)
	if err := applyFixtures(ctx, baseDB, fixtureDir, fixtures); err != nil {
		baseDB.Close()
		return nil, err
	}
	baseDB.Close()
	for _, test := range tests {
		result := TestResult{TestID: test.ID}
		required := make([]string, 0, len(manifestRequires)+len(test.Requires))
		required = append(required, manifestRequires...)
		required = append(required, test.Requires...)
		if !requirementsSatisfied(required) {
			result.Status = "skipped"
			results = append(results, result)
			continue
		}
		testDsn := buildTestDSN(dsn, test.DsnAppend)
		db, err := sql.Open("scratchbird", testDsn)
		if err != nil {
			return nil, err
		}
		configureDB(db)
		switch normalizeTestKind(test.Kind) {
		case "auth":
			err = db.PingContext(ctx)
		case "native_prepare_bind":
			result, err = runPrepareBind(ctx, db, test)
		case "native_query":
			result, err = runQuery(ctx, db, test)
		case "cancel":
			result, err = runCancel(ctx, db, test)
		default:
			err = errors.New("unknown test kind: " + test.Kind)
		}
		db.Close()
		if test.ExpectSQLState != "" {
			if err == nil {
				result.Status = "error"
				result.Errors = append(result.Errors, "expected SQLSTATE "+test.ExpectSQLState)
			} else if !strings.EqualFold(extractSQLState(err), test.ExpectSQLState) {
				result.Status = "error"
				result.Errors = append(result.Errors, err.Error())
			} else {
				result.Status = "ok"
				err = nil
			}
		}
		if err != nil {
			result.Status = "error"
			result.Errors = append(result.Errors, err.Error())
		} else if result.Status == "" {
			result.Status = "ok"
		}
		results = append(results, result)
		err = nil
	}
	return results, nil
}

func runQuery(ctx context.Context, db *sql.DB, test TestSpec) (TestResult, error) {
	sqlText := strings.TrimSpace(test.SQL)
	if sqlText == "" {
		sqlText = "SELECT 1"
	}
	queryCtx := ctx
	if test.TimeoutMs != nil && *test.TimeoutMs > 0 {
		timeoutCtx, cancel := context.WithTimeout(ctx, time.Duration(*test.TimeoutMs)*time.Millisecond)
		defer cancel()
		queryCtx = timeoutCtx
	}
	rows, err := db.QueryContext(queryCtx, sqlText)
	if err != nil {
		return TestResult{TestID: test.ID}, err
	}
	defer rows.Close()
	result, err := readRows(test.ID, rows)
	if err == nil && test.ExpectRows != nil && len(result.Rows) != *test.ExpectRows {
		return result, errors.New("unexpected row count")
	}
	return result, err
}

func runPrepareBind(ctx context.Context, db *sql.DB, test TestSpec) (TestResult, error) {
	if strings.TrimSpace(test.SQL) == "" {
		return TestResult{TestID: test.ID}, errors.New("prepare_bind requires sql")
	}
	queryCtx := ctx
	if test.TimeoutMs != nil && *test.TimeoutMs > 0 {
		timeoutCtx, cancel := context.WithTimeout(ctx, time.Duration(*test.TimeoutMs)*time.Millisecond)
		defer cancel()
		queryCtx = timeoutCtx
	}
	stmt, err := db.PrepareContext(queryCtx, test.SQL)
	if err != nil {
		return TestResult{TestID: test.ID}, err
	}
	defer stmt.Close()
	params := normalizeParams(test.Params)
	rows, err := stmt.QueryContext(queryCtx, params...)
	if err != nil {
		return TestResult{TestID: test.ID}, err
	}
	defer rows.Close()
	result, err := readRows(test.ID, rows)
	if err == nil && test.ExpectRows != nil && len(result.Rows) != *test.ExpectRows {
		return result, errors.New("unexpected row count")
	}
	return result, err
}

func runCancel(ctx context.Context, db *sql.DB, test TestSpec) (TestResult, error) {
	sqlText := strings.TrimSpace(test.SQL)
	if sqlText == "" {
		return TestResult{TestID: test.ID}, errors.New("cancel requires sql")
	}
	cancelAfter := 1
	if test.CancelAfter != nil && *test.CancelAfter > 0 {
		cancelAfter = *test.CancelAfter
	}
	cancelCtx, cancel := context.WithCancel(ctx)
	rows, err := db.QueryContext(cancelCtx, sqlText)
	if err != nil {
		cancel()
		return TestResult{TestID: test.ID}, err
	}
	defer rows.Close()
	cols, err := rows.Columns()
	if err != nil {
		cancel()
		return TestResult{TestID: test.ID}, err
	}
	result := TestResult{TestID: test.ID, Columns: cols}
	count := 0
	for rows.Next() {
		dest := make([]any, len(cols))
		for i := range dest {
			var holder any
			dest[i] = &holder
		}
		if err := rows.Scan(dest...); err != nil {
			cancel()
			return result, err
		}
		row := make([]any, len(cols))
		for i, item := range dest {
			row[i] = *(item.(*any))
		}
		result.Rows = append(result.Rows, row)
		count++
		if count >= cancelAfter {
			cancel()
			break
		}
	}
	if err := rows.Err(); err != nil {
		return result, err
	}
	return result, nil
}

func readRows(testID string, rows *sql.Rows) (TestResult, error) {
	cols, err := rows.Columns()
	if err != nil {
		return TestResult{TestID: testID}, err
	}
	result := TestResult{TestID: testID, Columns: cols, Status: "ok"}
	for rows.Next() {
		dest := make([]any, len(cols))
		for i := range dest {
			var holder any
			dest[i] = &holder
		}
		if err := rows.Scan(dest...); err != nil {
			return TestResult{TestID: testID}, err
		}
		row := make([]any, len(cols))
		for i, item := range dest {
			row[i] = *(item.(*any))
		}
		result.Rows = append(result.Rows, row)
	}
	if err := rows.Err(); err != nil {
		return TestResult{TestID: testID}, err
	}
	return result, nil
}

func splitSQLStatements(input string) []string {
	lines := strings.Split(input, "\n")
	filtered := make([]string, 0, len(lines))
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "--") || trimmed == "" {
			continue
		}
		filtered = append(filtered, line)
	}
	joined := strings.Join(filtered, "\n")
	parts := strings.Split(joined, ";")
	statements := make([]string, 0, len(parts))
	for _, part := range parts {
		trimmed := strings.TrimSpace(part)
		if trimmed == "" {
			continue
		}
		statements = append(statements, trimmed)
	}
	return statements
}

var createTablePattern = regexp.MustCompile(`(?is)^\s*create\s+table\s+(?:if\s+not\s+exists\s+)?("?[\w.]+"?)`)

func createdTableName(statement string) (string, bool) {
	matches := createTablePattern.FindStringSubmatch(statement)
	if len(matches) < 2 {
		return "", false
	}
	return strings.TrimSpace(matches[1]), true
}

func normalizeParams(params []any) []any {
	if len(params) == 0 {
		return params
	}
	out := make([]any, 0, len(params))
	for _, param := range params {
		switch v := param.(type) {
		case float64:
			if v == float64(int64(v)) {
				out = append(out, int64(v))
			} else {
				out = append(out, v)
			}
		default:
			out = append(out, param)
		}
	}
	return out
}

func buildTestDSN(base, appendQuery string) string {
	if strings.TrimSpace(appendQuery) == "" {
		return base
	}
	if strings.Contains(base, "://") {
		sep := "?"
		if strings.Contains(base, "?") {
			sep = "&"
		}
		return base + sep + appendQuery
	}
	if strings.Contains(base, ";") {
		return base + ";" + appendQuery
	}
	return base + " " + appendQuery
}

func requirementsSatisfied(requires []string) bool {
	if len(requires) == 0 {
		return true
	}
	for _, req := range requires {
		env := "SCRATCHBIRD_CONFORMANCE_" + strings.ToUpper(req)
		val := strings.ToLower(os.Getenv(env))
		if val != "1" && val != "true" && val != "yes" {
			return false
		}
	}
	return true
}

func extractSQLState(err error) string {
	if err == nil {
		return ""
	}
	var sbErr *scratchbird.Error
	if errors.As(err, &sbErr) && sbErr != nil {
		return sbErr.SQLState
	}
	type sqlStateCarrier interface {
		SQLState() string
	}
	var carrier sqlStateCarrier
	if errors.As(err, &carrier) {
		return carrier.SQLState()
	}
	return ""
}

func isAlreadyExistsError(err error) bool {
	if err == nil {
		return false
	}
	lower := strings.ToLower(err.Error())
	return strings.Contains(lower, "already exists")
}

func tableExists(ctx context.Context, db *sql.DB, qualifiedName string) bool {
	query := fmt.Sprintf("SELECT 1 FROM %s", qualifiedName)
	rows, err := db.QueryContext(ctx, query)
	if err != nil {
		return false
	}
	rows.Close()
	return true
}

func configureDB(db *sql.DB) {
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
}

func normalizeTestKind(kind string) string {
	switch strings.ToLower(strings.TrimSpace(kind)) {
	case "query":
		return "native_query"
	case "prepare_bind":
		return "native_prepare_bind"
	default:
		return strings.ToLower(strings.TrimSpace(kind))
	}
}
