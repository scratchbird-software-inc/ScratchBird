// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import "errors"

type Result struct {
	tag          string
	rowsAffected int64
	lastInsertID int64
}

func (r *Result) LastInsertId() (int64, error) {
	if r == nil {
		return 0, errors.New("no result")
	}
	if r.lastInsertID == 0 {
		return 0, errors.New("LastInsertId not available")
	}
	return r.lastInsertID, nil
}

func (r *Result) RowsAffected() (int64, error) {
	if r == nil {
		return 0, errors.New("no result")
	}
	return r.rowsAffected, nil
}
