// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"database/sql/driver"
	"fmt"
	"regexp"
	"strings"
)

// Metadata helper queries (sys.*) per METADATA_SCHEMA_CONTRACT.md.
const (
	metadataCatalogsQuery     = "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
	metadataSchemasQuery      = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
	metadataTablesQuery       = "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
	metadataColumnsQuery      = "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
	metadataIndexesQuery      = "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"
	metadataIndexColumnsQuery = "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"
	metadataConstraintsQuery  = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"
	metadataPrimaryKeysQuery  = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name"
	metadataForeignKeysQuery  = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name"
	metadataTablePrivsQuery   = "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name"
	metadataColumnPrivsQuery  = "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
	metadataProceduresQuery   = "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name"
	metadataFunctionsQuery    = "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name"
	metadataTypeInfoQuery     = "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name"
)

func MetadataCatalogsQuery() string     { return metadataCatalogsQuery }
func MetadataSchemasQuery() string      { return metadataSchemasQuery }
func MetadataTablesQuery() string       { return metadataTablesQuery }
func MetadataColumnsQuery() string      { return metadataColumnsQuery }
func MetadataIndexesQuery() string      { return metadataIndexesQuery }
func MetadataIndexColumnsQuery() string { return metadataIndexColumnsQuery }
func MetadataConstraintsQuery() string  { return metadataConstraintsQuery }
func MetadataPrimaryKeysQuery() string  { return metadataPrimaryKeysQuery }
func MetadataForeignKeysQuery() string  { return metadataForeignKeysQuery }
func MetadataTablePrivilegesQuery() string {
	return metadataTablePrivsQuery
}
func MetadataColumnPrivilegesQuery() string {
	return metadataColumnPrivsQuery
}
func MetadataProceduresQuery() string { return metadataProceduresQuery }
func MetadataFunctionsQuery() string  { return metadataFunctionsQuery }
func MetadataTypeInfoQuery() string   { return metadataTypeInfoQuery }

var metadataCollectionQueries = map[string]string{
	"catalogs":          metadataCatalogsQuery,
	"schemas":           metadataSchemasQuery,
	"tables":            metadataTablesQuery,
	"columns":           metadataColumnsQuery,
	"indexes":           metadataIndexesQuery,
	"index_columns":     metadataIndexColumnsQuery,
	"constraints":       metadataConstraintsQuery,
	"primary_keys":      metadataPrimaryKeysQuery,
	"foreign_keys":      metadataForeignKeysQuery,
	"table_privileges":  metadataTablePrivsQuery,
	"column_privileges": metadataColumnPrivsQuery,
	"procedures":        metadataProceduresQuery,
	"functions":         metadataFunctionsQuery,
	"type_info":         metadataTypeInfoQuery,
}

var metadataCollectionAliases = map[string]string{
	"catalog":           "catalogs",
	"catalogs":          "catalogs",
	"schema":            "schemas",
	"schemas":           "schemas",
	"table":             "tables",
	"tables":            "tables",
	"column":            "columns",
	"columns":           "columns",
	"index":             "indexes",
	"indexes":           "indexes",
	"indexcolumns":      "index_columns",
	"index_columns":     "index_columns",
	"constraint":        "constraints",
	"constraints":       "constraints",
	"primarykey":        "primary_keys",
	"primarykeys":       "primary_keys",
	"primary_keys":      "primary_keys",
	"pk":                "primary_keys",
	"foreignkey":        "foreign_keys",
	"foreignkeys":       "foreign_keys",
	"foreign_keys":      "foreign_keys",
	"fk":                "foreign_keys",
	"tableprivileges":   "table_privileges",
	"table_privileges":  "table_privileges",
	"columnprivileges":  "column_privileges",
	"column_privileges": "column_privileges",
	"procedure":         "procedures",
	"procedures":        "procedures",
	"function":          "functions",
	"functions":         "functions",
	"typeinfo":          "type_info",
	"type_info":         "type_info",
	"types":             "type_info",
}

var metadataRestrictionKeyAliases = map[string][]string{
	"catalog":    {"catalog_name", "table_catalog", "table_cat", "catalog"},
	"schema":     {"schema_name", "table_schema", "table_schem", "schema"},
	"table":      {"table_name", "table", "relname"},
	"column":     {"column_name", "column"},
	"index":      {"index_name", "index"},
	"constraint": {"constraint_name", "constraint"},
	"procedure":  {"procedure_name", "routine_name", "procedure"},
	"function":   {"function_name", "routine_name", "function"},
	"type":       {"type_name", "data_type_name", "data_type", "udt_name"},
}

var metadataCollectionRestrictionKeys = map[string][]string{
	"catalogs":          {"catalog"},
	"schemas":           {"catalog", "schema"},
	"tables":            {"catalog", "schema", "table", "type"},
	"columns":           {"catalog", "schema", "table", "column", "type"},
	"indexes":           {"catalog", "schema", "table", "index"},
	"index_columns":     {"catalog", "schema", "table", "index", "column"},
	"constraints":       {"catalog", "schema", "table", "constraint"},
	"primary_keys":      {"catalog", "schema", "table", "constraint"},
	"foreign_keys":      {"catalog", "schema", "table", "constraint"},
	"table_privileges":  {"catalog", "schema", "table"},
	"column_privileges": {"catalog", "schema", "table", "column"},
	"procedures":        {"catalog", "schema", "procedure"},
	"functions":         {"catalog", "schema", "function"},
	"type_info":         {"type"},
}

type metadataRestrictionBinding struct {
	columnIndexes []int
	expectNull    bool
	expectedText  string
}

func NormalizeMetadataCollectionName(collection string) (string, error) {
	normalized := strings.ToLower(strings.TrimSpace(collection))
	if normalized == "" {
		normalized = "tables"
	}
	resolved, ok := metadataCollectionAliases[normalized]
	if !ok {
		return "", fmt.Errorf("metadata collection %q is not supported", collection)
	}
	return resolved, nil
}

func ResolveMetadataCollectionQuery(collection string) (string, error) {
	resolved, err := NormalizeMetadataCollectionName(collection)
	if err != nil {
		return "", err
	}
	query, ok := metadataCollectionQueries[resolved]
	if !ok {
		return "", fmt.Errorf("metadata collection %q is not supported", collection)
	}
	return query, nil
}

func normalizeMetadataRestrictions(restrictions map[string]any) map[string]any {
	if len(restrictions) == 0 {
		return map[string]any{}
	}

	normalized := make(map[string]any, len(restrictions))
	for key, value := range restrictions {
		normalizedKey := metadataNormalizeIdentifier(key)
		if normalizedKey == "" {
			continue
		}
		normalized[normalizedKey] = value
	}
	return normalized
}

func filterMetadataRowsByRestrictions(rows [][]driver.Value, columnNames []string, restrictions map[string]any, collection string) [][]driver.Value {
	normalizedRestrictions := normalizeMetadataRestrictions(restrictions)
	if len(normalizedRestrictions) == 0 {
		return rows
	}
	bindings := metadataBuildRestrictionBindings(columnNames, normalizedRestrictions, collection)
	if len(bindings) == 0 {
		return rows
	}

	filtered := make([][]driver.Value, 0, len(rows))
	for _, row := range rows {
		if metadataRowMatchesBindings(row, bindings) {
			filtered = append(filtered, row)
		}
	}
	return filtered
}

// MetadataSchemaTreeNode represents one node in a dotted-schema navigation tree.
type MetadataSchemaTreeNode struct {
	Name     string
	FullPath string
	Terminal bool
	Children []*MetadataSchemaTreeNode
}

// MetadataExpandSchemaNames optionally expands dotted parent schemas while preserving insertion order.
func MetadataExpandSchemaNames(schemaNames []string, schemaPattern string, expandParents bool) []string {
	matches := metadataSchemaPatternMatcher(schemaPattern)
	out := make([]string, 0, len(schemaNames))
	seen := make(map[string]struct{}, len(schemaNames))

	for _, raw := range schemaNames {
		schemaName := normalizeMetadataSchemaPath(raw)
		if schemaName == "" {
			continue
		}
		if expandParents {
			appendSchemaWithParents(&out, seen, matches, schemaName)
			continue
		}
		if matches(schemaName) {
			appendSchemaUnique(&out, seen, schemaName)
		}
	}
	return out
}

// MetadataBuildSchemaTree converts dotted schema paths into a recursive tree shape.
func MetadataBuildSchemaTree(schemaPaths []string) []*MetadataSchemaTreeNode {
	nodesByPath := make(map[string]*MetadataSchemaTreeNode, len(schemaPaths))
	roots := make([]*MetadataSchemaTreeNode, 0)

	for _, raw := range schemaPaths {
		normalized := normalizeMetadataSchemaPath(raw)
		if normalized == "" {
			continue
		}

		var parent *MetadataSchemaTreeNode
		var currentPath strings.Builder
		for _, segment := range strings.Split(normalized, ".") {
			if segment == "" {
				continue
			}
			if currentPath.Len() > 0 {
				currentPath.WriteByte('.')
			}
			currentPath.WriteString(segment)
			fullPath := currentPath.String()

			node, exists := nodesByPath[fullPath]
			if !exists {
				node = &MetadataSchemaTreeNode{Name: segment, FullPath: fullPath}
				nodesByPath[fullPath] = node
				if parent == nil {
					roots = append(roots, node)
				} else {
					parent.Children = append(parent.Children, node)
				}
			}
			parent = node
		}
		if parent != nil {
			parent.Terminal = true
		}
	}

	return roots
}

func appendSchemaWithParents(out *[]string, seen map[string]struct{}, matches func(string) bool, schemaName string) {
	var current strings.Builder
	for _, segment := range strings.Split(schemaName, ".") {
		if segment == "" {
			continue
		}
		if current.Len() > 0 {
			current.WriteByte('.')
		}
		current.WriteString(segment)
		candidate := current.String()
		if matches(candidate) {
			appendSchemaUnique(out, seen, candidate)
		}
	}
}

func appendSchemaUnique(out *[]string, seen map[string]struct{}, schemaName string) {
	if _, exists := seen[schemaName]; exists {
		return
	}
	seen[schemaName] = struct{}{}
	*out = append(*out, schemaName)
}

func normalizeMetadataSchemaPath(schemaPath string) string {
	segments := strings.Split(schemaPath, ".")
	out := make([]string, 0, len(segments))
	for _, segment := range segments {
		segment = strings.TrimSpace(segment)
		if segment == "" {
			continue
		}
		out = append(out, segment)
	}
	return strings.Join(out, ".")
}

func metadataSchemaPatternMatcher(pattern string) func(string) bool {
	pattern = strings.TrimSpace(pattern)
	if pattern == "" {
		return func(string) bool { return true }
	}

	rx, err := regexp.Compile(metadataPatternToRegex(pattern))
	if err != nil {
		return func(string) bool { return false }
	}

	return func(value string) bool {
		if value == "" {
			return false
		}
		return rx.MatchString(value)
	}
}

func metadataPatternToRegex(pattern string) string {
	var out strings.Builder
	out.WriteString("(?i)^")
	escaped := false

	for _, ch := range pattern {
		if escaped {
			out.WriteString(regexp.QuoteMeta(string(ch)))
			escaped = false
			continue
		}
		switch ch {
		case '\\':
			escaped = true
		case '%':
			out.WriteString(".*")
		case '_':
			out.WriteByte('.')
		default:
			out.WriteString(regexp.QuoteMeta(string(ch)))
		}
	}

	out.WriteByte('$')
	return out.String()
}

func metadataBuildRestrictionBindings(columnNames []string, restrictions map[string]any, collection string) []metadataRestrictionBinding {
	if len(columnNames) == 0 || len(restrictions) == 0 {
		return nil
	}

	allowedAliases := map[string]struct{}{}
	if resolvedCollection, err := NormalizeMetadataCollectionName(collection); err == nil {
		for _, restrictionKey := range metadataCollectionRestrictionKeys[resolvedCollection] {
			for _, alias := range metadataRestrictionAliases(restrictionKey) {
				allowedAliases[metadataNormalizeIdentifier(alias)] = struct{}{}
			}
		}
	}

	columnIndexesByName := make(map[string][]int, len(columnNames))
	for idx, columnName := range columnNames {
		normalized := metadataNormalizeIdentifier(columnName)
		if normalized == "" {
			continue
		}
		columnIndexesByName[normalized] = append(columnIndexesByName[normalized], idx)
	}

	bindings := make([]metadataRestrictionBinding, 0, len(restrictions))
	for restrictionKey, restrictionValue := range restrictions {
		aliases := make(map[string]struct{})
		for _, alias := range metadataRestrictionAliases(restrictionKey) {
			normalizedAlias := metadataNormalizeIdentifier(alias)
			if normalizedAlias == "" {
				continue
			}
			if len(allowedAliases) > 0 {
				if _, ok := allowedAliases[normalizedAlias]; !ok && normalizedAlias != restrictionKey {
					continue
				}
			}
			aliases[normalizedAlias] = struct{}{}
		}
		if len(aliases) == 0 {
			continue
		}

		indexSet := map[int]struct{}{}
		for alias := range aliases {
			for _, idx := range columnIndexesByName[alias] {
				indexSet[idx] = struct{}{}
			}
		}
		if len(indexSet) == 0 {
			continue
		}

		columnIndexes := make([]int, 0, len(indexSet))
		for idx := range indexSet {
			columnIndexes = append(columnIndexes, idx)
		}

		expected := metadataNormalizeMatchText(restrictionValue)
		bindings = append(bindings, metadataRestrictionBinding{
			columnIndexes: columnIndexes,
			expectNull:    expected == "null",
			expectedText:  expected,
		})
	}
	return bindings
}

func metadataRowMatchesBindings(row []driver.Value, bindings []metadataRestrictionBinding) bool {
	for _, binding := range bindings {
		matched := false
		for _, idx := range binding.columnIndexes {
			if idx < 0 || idx >= len(row) {
				continue
			}
			value := row[idx]
			if binding.expectNull {
				if value == nil {
					matched = true
					break
				}
				continue
			}
			if value == nil {
				continue
			}
			if metadataNormalizeMatchText(value) == binding.expectedText {
				matched = true
				break
			}
		}
		if !matched {
			return false
		}
	}
	return true
}

func metadataRestrictionAliases(key string) []string {
	canonical := metadataNormalizeIdentifier(key)
	aliases := append([]string{}, metadataRestrictionKeyAliases[canonical]...)
	if canonical != "" {
		aliases = append(aliases, canonical)
	}
	return aliases
}

func metadataNormalizeIdentifier(value string) string {
	value = strings.ToLower(strings.TrimSpace(value))
	if value == "" {
		return ""
	}
	var out strings.Builder
	out.Grow(len(value))
	for _, ch := range value {
		if (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') {
			out.WriteRune(ch)
		}
	}
	return out.String()
}

func metadataNormalizeMatchText(value any) string {
	return strings.ToLower(strings.TrimSpace(fmt.Sprint(value)))
}
