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
	"io"
	"reflect"
)

type metadataRows struct {
	columns []string
	colInfo []columnInfo
	rows    [][]driver.Value
	pos     int
	closed  bool
}

func newMetadataRows(columns []string, colInfo []columnInfo, rows [][]driver.Value) *metadataRows {
	copiedColumns := append([]string(nil), columns...)
	copiedInfo := append([]columnInfo(nil), colInfo...)
	copiedRows := make([][]driver.Value, len(rows))
	for idx, row := range rows {
		copiedRows[idx] = append([]driver.Value(nil), row...)
	}
	return &metadataRows{
		columns: copiedColumns,
		colInfo: copiedInfo,
		rows:    copiedRows,
	}
}

func (r *metadataRows) Columns() []string {
	return append([]string(nil), r.columns...)
}

func (r *metadataRows) Close() error {
	r.closed = true
	return nil
}

func (r *metadataRows) Next(dest []driver.Value) error {
	if r.closed || r.pos >= len(r.rows) {
		return io.EOF
	}
	row := r.rows[r.pos]
	r.pos++
	for idx := range dest {
		if idx < len(row) {
			dest[idx] = row[idx]
		} else {
			dest[idx] = nil
		}
	}
	return nil
}

func (r *metadataRows) HasNextResultSet() bool {
	return false
}

func (r *metadataRows) NextResultSet() error {
	return io.EOF
}

func (r *metadataRows) ColumnTypeDatabaseTypeName(index int) string {
	if index < 0 || index >= len(r.colInfo) {
		return ""
	}
	return oidName(r.colInfo[index].typeOID)
}

func (r *metadataRows) ColumnTypeNullable(index int) (nullable, ok bool) {
	if index < 0 || index >= len(r.colInfo) {
		return true, false
	}
	return r.colInfo[index].nullable, true
}

func (r *metadataRows) ColumnTypeLength(index int) (length int64, ok bool) {
	if index < 0 || index >= len(r.colInfo) {
		return 0, false
	}
	return int64(r.colInfo[index].typeModifier), true
}

func (r *metadataRows) ColumnTypePrecisionScale(index int) (precision, scale int64, ok bool) {
	return 0, 0, false
}

func (r *metadataRows) ColumnTypeScanType(index int) reflect.Type {
	if index < 0 || index >= len(r.colInfo) {
		return nil
	}
	return scanTypeForOID(r.colInfo[index].typeOID)
}
