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
	"errors"
	"io"
)

type FieldSummary struct {
	Name     string
	TypeOID  uint32
	Format   uint16
	Nullable bool
}

type ResultSetSummary struct {
	Rows         [][]driver.Value
	RowCount     int64
	Fields       []FieldSummary
	Command      string
	LastInsertID int64
}

type BatchItemSummary struct {
	Index        int
	RowCount     int64
	Fields       []FieldSummary
	Command      string
	LastInsertID int64
}

type BatchSummary struct {
	Items         []BatchItemSummary
	TotalRowCount int64
}

func (c *Conn) NativeSQL(query string, args []driver.NamedValue) (string, error) {
	normalized, err := normalizeQuery(query, args)
	if err != nil {
		return "", normalizeSurfaceError(err)
	}
	return normalized.sql, nil
}

func (c *Conn) NativeCallableSQL(query string, args []driver.NamedValue) (string, error) {
	normalized, err := normalizeCallableQuery(query, args)
	if err != nil {
		return "", normalizeSurfaceError(err)
	}
	return normalized.sql, nil
}

func (c *Conn) CallContext(ctx context.Context, query string, args []driver.NamedValue) (driver.Rows, error) {
	normalized, err := normalizeCallableQuery(query, args)
	if err != nil {
		return nil, normalizeSurfaceError(err)
	}
	return c.QueryContext(ctx, normalized.sql, normalized.args)
}

func (c *Conn) QueryMultiContext(ctx context.Context, query string, args []driver.NamedValue) ([]ResultSetSummary, error) {
	rowsIface, err := c.QueryContext(ctx, query, args)
	if err != nil {
		return nil, err
	}
	rows, ok := rowsIface.(*Rows)
	if !ok {
		_ = rowsIface.Close()
		return nil, &Error{Kind: ErrInternal, Message: "unexpected rows implementation", SQLState: "XX000"}
	}
	defer func() { _ = rows.Close() }()

	summaries := make([]ResultSetSummary, 0, 1)
	for {
		dataRows := make([][]driver.Value, 0)
		for {
			row, err := rows.nextRow()
			if err == nil {
				copied := make([]driver.Value, len(row))
				copy(copied, row)
				dataRows = append(dataRows, copied)
				continue
			}
			if errors.Is(err, io.EOF) {
				break
			}
			return nil, err
		}
		fields := summarizeFields(rows.columns)
		summaries = append(summaries, ResultSetSummary{
			Rows:         dataRows,
			RowCount:     rows.rowsAffected,
			Fields:       fields,
			Command:      rows.commandTag,
			LastInsertID: rows.lastInsertID,
		})
		if !rows.HasNextResultSet() {
			break
		}
		if err := rows.NextResultSet(); err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			return nil, err
		}
	}
	return summaries, nil
}

func (c *Conn) QueryAllContext(ctx context.Context, query string, args []driver.NamedValue) ([]ResultSetSummary, error) {
	if err := c.ensureOpen(ctx); err != nil {
		return nil, err
	}
	normalized, err := normalizeQuery(query, args)
	if err != nil {
		return nil, err
	}
	if len(normalized.args) != 0 {
		return c.QueryMultiContext(ctx, normalized.sql, normalized.args)
	}
	span, err := c.beginOperation("query_all", normalized.sql)
	if err != nil {
		return nil, err
	}
	if err := c.sendSimpleQuery(normalized.sql, ctx); err != nil {
		c.endOperation(span, false)
		return nil, err
	}

	sets := make([]ResultSetSummary, 0, 1)
	current := ResultSetSummary{}
	columns := []columnInfo{}
	haveCurrent := false
	appendCurrent := func() {
		if !haveCurrent {
			return
		}
		sets = append(sets, current)
		current = ResultSetSummary{}
		columns = nil
		haveCurrent = false
	}

	for {
		select {
		case <-ctx.Done():
			_ = c.sendMessage(msgCancel, buildCancelPayload(0, 0), msgFlagUrgent, false)
			c.endOperation(span, false)
			return nil, ctx.Err()
		default:
		}
		msg, err := c.receive()
		if err != nil {
			c.endOperation(span, false)
			return nil, err
		}
		if c.handleAsyncMessage(msg) {
			continue
		}
		switch msg.header.typ {
		case msgError:
			queryErr := buildProtocolError(msg.body)
			_, _, _, _ = c.drainUntilReady(ctx)
			c.endOperationWithError(span, queryErr)
			return nil, queryErr
		case msgRowDescription:
			parsed, err := parseRowDescription(msg.body)
			if err != nil {
				c.endOperation(span, false)
				return nil, err
			}
			if haveCurrent && (len(current.Rows) > 0 || current.Command != "" || current.RowCount != 0) {
				appendCurrent()
			}
			columns = parsed
			current.Fields = summarizeFields(columns)
			haveCurrent = true
		case msgDataRow:
			if !haveCurrent {
				haveCurrent = true
			}
			values, err := parseDataRow(msg.body, len(columns))
			if err != nil {
				c.endOperation(span, false)
				return nil, err
			}
			row := make([]driver.Value, len(values))
			for index, value := range values {
				if value.null {
					row[index] = nil
					continue
				}
				col := columnInfo{}
				if index < len(columns) {
					col = columns[index]
				}
				decoded, err := decodeColumnValue(col, value.data)
				if err != nil {
					c.endOperation(span, false)
					return nil, err
				}
				row[index] = decoded
			}
			current.Rows = append(current.Rows, row)
		case msgCommandComplete:
			_, affected, lastID, tag, err := parseCommandComplete(msg.body)
			if err != nil {
				c.endOperation(span, false)
				return nil, err
			}
			if !haveCurrent {
				haveCurrent = true
			}
			current.RowCount = int64(affected)
			current.Command = tag
			current.LastInsertID = int64(lastID)
			appendCurrent()
		case msgParseComplete, msgBindComplete, msgCloseComplete, msgNoData, msgParameterDescription:
			continue
		case msgPortalSuspended:
			if err := c.sendMessage(msgExecute, buildExecutePayload("", c.config.FetchSize), 0, false); err != nil {
				c.endOperation(span, false)
				return nil, err
			}
		case msgReady:
			status, txnID, _, err := parseReady(msg.body)
			if err != nil {
				c.endOperation(span, false)
				return nil, err
			}
			c.applyRuntimeReadyState(status, txnID)
			appendCurrent()
			c.endOperation(span, true)
			return sets, nil
		default:
			continue
		}
	}
}

func (c *Conn) ExecuteMultiContext(ctx context.Context, query string, args []driver.NamedValue) ([]ResultSetSummary, error) {
	return c.QueryMultiContext(ctx, query, args)
}

func (c *Conn) ExecuteBatchContext(ctx context.Context, query string, batchArgs [][]driver.NamedValue) (BatchSummary, error) {
	if batchArgs == nil {
		return BatchSummary{}, &Error{Kind: ErrSyntax, Message: "batch arguments are required", SQLState: "07001"}
	}
	summary := BatchSummary{
		Items: make([]BatchItemSummary, 0, len(batchArgs)),
	}
	for index, args := range batchArgs {
		result, err := c.ExecContext(ctx, query, args)
		if err != nil {
			return summary, err
		}
		rowCount, err := result.RowsAffected()
		if err != nil {
			rowCount = -1
		}
		lastInsertID := int64(0)
		if value, err := result.LastInsertId(); err == nil {
			lastInsertID = value
		}
		command := ""
		if typedResult, ok := result.(*Result); ok {
			command = typedResult.tag
		}
		if rowCount > 0 {
			summary.TotalRowCount += rowCount
		}
		summary.Items = append(summary.Items, BatchItemSummary{
			Index:        index,
			RowCount:     rowCount,
			Fields:       nil,
			Command:      command,
			LastInsertID: lastInsertID,
		})
	}
	return summary, nil
}

func (c *Conn) QueryBatchContext(ctx context.Context, query string, batchArgs [][]driver.NamedValue) (BatchSummary, error) {
	return c.ExecuteBatchContext(ctx, query, batchArgs)
}

func (c *Conn) ExecuteWithGeneratedKeysContext(ctx context.Context, query string, args []driver.NamedValue) ([]int64, error) {
	sets, err := c.QueryMultiContext(ctx, query, args)
	if err != nil {
		return nil, err
	}
	keys := make([]int64, 0, len(sets))
	for _, set := range sets {
		if set.LastInsertID != 0 {
			keys = append(keys, set.LastInsertID)
		}
	}
	return keys, nil
}

func summarizeFields(columns []columnInfo) []FieldSummary {
	if len(columns) == 0 {
		return nil
	}
	out := make([]FieldSummary, 0, len(columns))
	for _, col := range columns {
		out = append(out, FieldSummary{
			Name:     col.name,
			TypeOID:  col.typeOID,
			Format:   uint16(col.format),
			Nullable: col.nullable,
		})
	}
	return out
}

func normalizeSurfaceError(err error) error {
	if err == nil {
		return nil
	}
	if _, ok := err.(*Error); ok {
		return err
	}
	return &Error{Kind: ErrSyntax, Message: err.Error(), SQLState: "07001"}
}
