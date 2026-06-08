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
	"io"
	"reflect"
)

type Rows struct {
	conn         *Conn
	columns      []columnInfo
	rowCountHint int64
	rowsAffected int64
	lastInsertID int64
	commandTag   string
	done         bool
	hasNextSet   bool
	setBoundary  bool
	pageSize     uint32
	ctx          context.Context
	cancel       func()
}

func newRows(conn *Conn, ctx context.Context) *Rows {
	rows := &Rows{conn: conn, ctx: ctx, pageSize: conn.config.FetchSize}
	if ctx != nil {
		cancelCh := make(chan struct{})
		rows.cancel = func() { close(cancelCh) }
		go func() {
			select {
			case <-ctx.Done():
				_ = conn.sendMessage(msgCancel, buildCancelPayload(0, 0), msgFlagUrgent, false)
			case <-cancelCh:
			}
		}()
	}
	return rows
}

func (r *Rows) Columns() []string {
	names := make([]string, len(r.columns))
	for i, col := range r.columns {
		names[i] = col.name
	}
	return names
}

func (r *Rows) Close() error {
	if r.done {
		return nil
	}
	if r.cancel != nil {
		r.cancel()
	}
	for !r.done {
		if _, err := r.nextRow(); err != nil {
			if err == io.EOF {
				if r.hasNextSet {
					if err := r.NextResultSet(); err != nil && err != io.EOF {
						return err
					}
					continue
				}
				break
			}
			return err
		}
	}
	return nil
}

func (r *Rows) Next(dest []driver.Value) error {
	row, err := r.nextRow()
	if err != nil {
		return err
	}
	for i := range dest {
		if i < len(row) {
			dest[i] = row[i]
		} else {
			dest[i] = nil
		}
	}
	return nil
}

func (r *Rows) HasNextResultSet() bool {
	return r.hasNextSet
}

func (r *Rows) NextResultSet() error {
	if r.done || !r.hasNextSet {
		return io.EOF
	}
	r.hasNextSet = false
	r.setBoundary = false
	r.columns = nil
	r.rowsAffected = 0
	r.lastInsertID = 0
	r.commandTag = ""
	return nil
}

func (r *Rows) nextRow() ([]driver.Value, error) {
	if r.done {
		return nil, io.EOF
	}
	if r.setBoundary {
		return nil, io.EOF
	}
	for {
		msg, err := r.conn.receive()
		if err != nil {
			return nil, err
		}
		if r.conn.handleAsyncMessage(msg) {
			continue
		}
		switch msg.header.typ {
		case msgError:
			return nil, buildProtocolError(msg.body)
		case msgRowDescription:
			cols, err := parseRowDescription(msg.body)
			if err != nil {
				return nil, err
			}
			r.columns = cols
		case msgDataRow:
			values, err := parseDataRow(msg.body, len(r.columns))
			if err != nil {
				return nil, err
			}
			out := make([]driver.Value, len(values))
			for i, value := range values {
				if value.null {
					out[i] = nil
					continue
				}
				col := columnInfo{}
				if i < len(r.columns) {
					col = r.columns[i]
				}
				decoded, err := decodeColumnValue(col, value.data)
				if err != nil {
					return nil, err
				}
				out[i] = decoded
			}
			return out, nil
		case msgCommandComplete:
			_, rows, lastID, tag, err := parseCommandComplete(msg.body)
			if err != nil {
				return nil, err
			}
			r.commandTag = tag
			r.rowsAffected = int64(rows)
			r.lastInsertID = int64(lastID)
			if err := r.markResultSetBoundary(); err != nil {
				return nil, err
			}
			return nil, io.EOF
		case msgPortalSuspended:
			execPayload := buildExecutePayload("", r.pageSize)
			if err := r.conn.sendMessage(msgExecute, execPayload, 0, false); err != nil {
				return nil, err
			}
		case msgReady:
			status, txnID, _, err := parseReady(msg.body)
			if err == nil {
				r.conn.applyRuntimeReadyState(status, txnID)
			}
			r.done = true
			return nil, io.EOF
		default:
			continue
		}
	}
}

func (r *Rows) markResultSetBoundary() error {
	for {
		msg, err := r.conn.receive()
		if err != nil {
			return err
		}
		if r.conn.handleAsyncMessage(msg) {
			continue
		}
		if msg.header.typ == msgReady {
			status, txnID, _, err := parseReady(msg.body)
			if err == nil {
				r.conn.applyRuntimeReadyState(status, txnID)
			}
			r.done = true
			r.hasNextSet = false
			r.setBoundary = false
			return nil
		}
		r.conn.queue(msg)
		r.hasNextSet = true
		r.setBoundary = true
		return nil
	}
}

func (r *Rows) ColumnTypeDatabaseTypeName(index int) string {
	if index < 0 || index >= len(r.columns) {
		return ""
	}
	return oidName(r.columns[index].typeOID)
}

func (r *Rows) ColumnTypeNullable(index int) (nullable, ok bool) {
	if index < 0 || index >= len(r.columns) {
		return true, false
	}
	return r.columns[index].nullable, true
}

func (r *Rows) ColumnTypeLength(index int) (length int64, ok bool) {
	if index < 0 || index >= len(r.columns) {
		return 0, false
	}
	return int64(r.columns[index].typeModifier), true
}

func (r *Rows) ColumnTypePrecisionScale(index int) (precision, scale int64, ok bool) {
	return 0, 0, false
}

func (r *Rows) ColumnTypeScanType(index int) reflect.Type {
	if index < 0 || index >= len(r.columns) {
		return nil
	}
	return scanTypeForOID(r.columns[index].typeOID)
}
