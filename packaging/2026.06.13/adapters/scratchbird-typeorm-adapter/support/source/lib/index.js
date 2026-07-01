// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const { normalizeTypeOrmOptions, enforceGuardrails } = require("./options");
const { mapScratchBirdTypeToTypeOrm, normalizeTypeName } = require("./type-map");
const { generateEntitySchemas, buildEntityName } = require("./entity-schema");
const { buildNestedCrudTransactionPlan } = require("./transaction-contract");

module.exports = {
  normalizeTypeOrmOptions,
  enforceGuardrails,
  mapScratchBirdTypeToTypeOrm,
  normalizeTypeName,
  generateEntitySchemas,
  buildEntityName,
  buildNestedCrudTransactionPlan,
};
