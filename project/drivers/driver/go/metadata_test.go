// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"context"
	"database/sql/driver"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"reflect"
	"testing"
)

func TestMetadataExpandSchemaNamesDefaultKeepsPhysicalRows(t *testing.T) {
	schemas := MetadataExpandSchemaNames([]string{
		"sys",
		"users.alice.dev",
		"users.bob.dev",
		"analytics.prod",
	}, "", false)

	expected := []string{"sys", "users.alice.dev", "users.bob.dev", "analytics.prod"}
	if !reflect.DeepEqual(schemas, expected) {
		t.Fatalf("unexpected schemas: got %v want %v", schemas, expected)
	}
}

func TestMetadataExpandSchemaNamesCanExpandParentsForRecursiveNavigation(t *testing.T) {
	schemas := MetadataExpandSchemaNames([]string{
		"analytics.prod",
		"sys",
		"users.alice.dev",
		"users.bob.dev",
		"users..bob.dev",
		"",
	}, "", true)

	expected := []string{
		"analytics",
		"analytics.prod",
		"sys",
		"users",
		"users.alice",
		"users.alice.dev",
		"users.bob",
		"users.bob.dev",
	}
	if !reflect.DeepEqual(schemas, expected) {
		t.Fatalf("unexpected expanded schemas: got %v want %v", schemas, expected)
	}
}

func TestMetadataExpandSchemaNamesExpansionRespectsPattern(t *testing.T) {
	schemas := MetadataExpandSchemaNames([]string{
		"users.alice.dev",
		"users.bob.dev",
		"analytics.prod",
	}, "users.%", true)

	expected := []string{"users.alice", "users.alice.dev", "users.bob", "users.bob.dev"}
	if !reflect.DeepEqual(schemas, expected) {
		t.Fatalf("unexpected filtered schemas: got %v want %v", schemas, expected)
	}
}

func TestMetadataBuildSchemaTreeSmoke(t *testing.T) {
	roots := MetadataBuildSchemaTree([]string{
		"sys",
		"users",
		"users.alice.dev",
		"users.alice.prod",
		"users.bob.dev",
		"users.bob.dev", // duplicate input path
		"analytics.dev",
		"analytics.prod",
	})

	if len(roots) != 3 {
		t.Fatalf("expected 3 roots, got %d", len(roots))
	}

	users := findMetadataNodeByName(roots, "users")
	if users == nil {
		t.Fatalf("expected users root")
	}

	alice := findMetadataNodeByName(users.Children, "alice")
	bob := findMetadataNodeByName(users.Children, "bob")
	if alice == nil {
		t.Fatalf("expected users.alice node")
	}
	if bob == nil {
		t.Fatalf("expected users.bob node")
	}

	if findMetadataNodeByName(alice.Children, "dev") == nil {
		t.Fatalf("expected users.alice.dev node")
	}
	if findMetadataNodeByName(alice.Children, "prod") == nil {
		t.Fatalf("expected users.alice.prod node")
	}
	if findMetadataNodeByName(bob.Children, "dev") == nil {
		t.Fatalf("expected users.bob.dev node")
	}
	if len(bob.Children) != 1 {
		t.Fatalf("expected one unique users.bob child, got %d", len(bob.Children))
	}

	sys := findMetadataNodeByName(roots, "sys")
	if sys == nil || !sys.Terminal {
		t.Fatalf("expected terminal sys node")
	}
}

func TestResolveMetadataCollectionQueryAliases(t *testing.T) {
	query, err := ResolveMetadataCollectionQuery("")
	if err != nil {
		t.Fatalf("resolve default metadata query failed: %v", err)
	}
	if query != MetadataTablesQuery() {
		t.Fatalf("default metadata query mismatch: got %q want %q", query, MetadataTablesQuery())
	}

	cases := map[string]string{
		"schemas":          MetadataSchemasQuery(),
		"indexcolumns":     MetadataIndexColumnsQuery(),
		"pk":               MetadataPrimaryKeysQuery(),
		"foreign_keys":     MetadataForeignKeysQuery(),
		"table_privileges": MetadataTablePrivilegesQuery(),
		"types":            MetadataTypeInfoQuery(),
	}
	for input, expected := range cases {
		got, err := ResolveMetadataCollectionQuery(input)
		if err != nil {
			t.Fatalf("resolve metadata query for %q failed: %v", input, err)
		}
		if got != expected {
			t.Fatalf("metadata query mismatch for %q: got %q want %q", input, got, expected)
		}
	}

	if _, err := ResolveMetadataCollectionQuery("unsupported_metadata_family"); err == nil {
		t.Fatalf("expected unsupported metadata collection error")
	}
}

func TestQueryMetadataRoutesCollectionQuery(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read metadata query message: %w", err)
			return
		}
		if msg.header.typ != msgQuery {
			errCh <- fmt.Errorf("expected %v, got %v", msgQuery, msg.header.typ)
			return
		}
		sqlText, err := queryPayloadSQL(msg.body)
		if err != nil {
			errCh <- err
			return
		}
		if sqlText != MetadataTablesQuery() {
			errCh <- fmt.Errorf("metadata query mismatch: got %q want %q", sqlText, MetadataTablesQuery())
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(0, 0, "SELECT"))); err != nil {
			errCh <- fmt.Errorf("write metadata command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write metadata ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	rows, err := conn.QueryMetadata(context.Background(), "tables")
	if err != nil {
		t.Fatalf("query metadata failed: %v", err)
	}
	if rows == nil {
		t.Fatalf("expected rows, got nil")
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close metadata rows failed: %v", err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestQueryMetadataRejectsUnsupportedCollection(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	_, err := conn.QueryMetadata(context.Background(), "no_such_metadata")
	requireDriverError(t, err, ErrNotSupported, "0A000")
}

func TestFilterMetadataRowsByRestrictionsAliasesAndNull(t *testing.T) {
	columns := []string{"schema_name", "table_name", "owner_id"}
	rows := [][]driver.Value{
		{"sys", "events", nil},
		{"users", "events", nil},
		{"users", "profiles", int64(7)},
	}

	filtered := filterMetadataRowsByRestrictions(rows, columns, map[string]any{
		"schema": "users",
		"table":  "events",
	}, "tables")
	expected := [][]driver.Value{{"users", "events", nil}}
	if !reflect.DeepEqual(filtered, expected) {
		t.Fatalf("unexpected filtered rows: got %v want %v", filtered, expected)
	}

	filtered = filterMetadataRowsByRestrictions(rows, columns, map[string]any{
		"owner_id":       "null",
		"missing_filter": "ignored",
	}, "tables")
	expected = [][]driver.Value{{"sys", "events", nil}, {"users", "events", nil}}
	if !reflect.DeepEqual(filtered, expected) {
		t.Fatalf("unexpected filtered rows for null restriction: got %v want %v", filtered, expected)
	}
}

func TestQueryMetadataWithRestrictionsFiltersRows(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read metadata query message: %w", err)
			return
		}
		if msg.header.typ != msgQuery {
			errCh <- fmt.Errorf("expected %v, got %v", msgQuery, msg.header.typ)
			return
		}
		sqlText, err := queryPayloadSQL(msg.body)
		if err != nil {
			errCh <- err
			return
		}
		if sqlText != MetadataSchemasQuery() {
			errCh <- fmt.Errorf("metadata query mismatch: got %q want %q", sqlText, MetadataSchemasQuery())
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgRowDescription}, metadataTestRowDescriptionPayload("schema_name"))); err != nil {
			errCh <- fmt.Errorf("write metadata row description: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgDataRow}, metadataTestDataRowPayload("sys"))); err != nil {
			errCh <- fmt.Errorf("write metadata data row: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgDataRow}, metadataTestDataRowPayload("users"))); err != nil {
			errCh <- fmt.Errorf("write metadata data row: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(2, 0, "SELECT"))); err != nil {
			errCh <- fmt.Errorf("write metadata command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write metadata ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	rowsDriver, err := conn.QueryMetadataWithRestrictions(context.Background(), "schemas", map[string]any{"schema": "users"})
	if err != nil {
		t.Fatalf("query metadata with restrictions failed: %v", err)
	}

	rows, ok := rowsDriver.(*metadataRows)
	if !ok {
		t.Fatalf("expected *metadataRows, got %T", rowsDriver)
	}

	dest := make([]driver.Value, 1)
	if err := rows.Next(dest); err != nil {
		t.Fatalf("next filtered row failed: %v", err)
	}
	if got, want := dest[0], driver.Value("users"); got != want {
		t.Fatalf("filtered row mismatch: got %v want %v", got, want)
	}
	if err := rows.Next(dest); !errors.Is(err, io.EOF) {
		t.Fatalf("expected EOF after one filtered row, got %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close filtered rows failed: %v", err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func findMetadataNodeByName(nodes []*MetadataSchemaTreeNode, name string) *MetadataSchemaTreeNode {
	for _, node := range nodes {
		if node != nil && node.Name == name {
			return node
		}
	}
	return nil
}

func queryPayloadSQL(payload []byte) (string, error) {
	if len(payload) < 13 {
		return "", fmt.Errorf("query payload too short: %d", len(payload))
	}
	sqlWithTerminator := payload[12:]
	if sqlWithTerminator[len(sqlWithTerminator)-1] == 0 {
		sqlWithTerminator = sqlWithTerminator[:len(sqlWithTerminator)-1]
	}
	return string(sqlWithTerminator), nil
}

func metadataTestRowDescriptionPayload(columnName string) []byte {
	nameBytes := []byte(columnName)
	payload := make([]byte, 4+4+len(nameBytes)+4+2+4+2+4+1+1+2)
	binary.LittleEndian.PutUint16(payload[0:2], 1)
	offset := 4
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(nameBytes)))
	offset += 4
	copy(payload[offset:offset+len(nameBytes)], nameBytes)
	offset += len(nameBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], 0)
	offset += 4
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 1)
	offset += 2
	binary.LittleEndian.PutUint32(payload[offset:offset+4], oidText)
	offset += 4
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 0)
	offset += 2
	binary.LittleEndian.PutUint32(payload[offset:offset+4], 0)
	offset += 4
	payload[offset] = uint8(formatText)
	offset++
	payload[offset] = 1
	return payload
}

func metadataTestDataRowPayload(value string) []byte {
	valueBytes := []byte(value)
	payload := make([]byte, 2+2+1+4+len(valueBytes))
	binary.LittleEndian.PutUint16(payload[0:2], 1)
	binary.LittleEndian.PutUint16(payload[2:4], 1)
	payload[4] = 0
	binary.LittleEndian.PutUint32(payload[5:9], uint32(len(valueBytes)))
	copy(payload[9:], valueBytes)
	return payload
}
