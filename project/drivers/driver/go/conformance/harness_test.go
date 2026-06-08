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
	"encoding/json"
	"os"
	"testing"
	"time"
)

func TestConformanceManifest(t *testing.T) {
	dsn := os.Getenv("SCRATCHBIRD_GO_URL")
	if dsn == "" {
		t.Skip("SCRATCHBIRD_GO_URL not set")
	}
	manifestPath := os.Getenv("SCRATCHBIRD_CONFORMANCE_MANIFEST")
	if manifestPath == "" {
		t.Skip("SCRATCHBIRD_CONFORMANCE_MANIFEST not set")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	summary, err := RunManifest(ctx, dsn, manifestPath)
	if err != nil {
		t.Fatalf("conformance run failed: %v", err)
	}
	if len(summary.Results) == 0 {
		t.Fatalf("conformance run returned no results")
	}
	for _, result := range summary.Results {
		if result.Status != "ok" && result.Status != "skipped" {
			payload, _ := json.Marshal(summary)
			t.Fatalf("conformance test %q failed: %s", result.TestID, string(payload))
		}
	}
}
