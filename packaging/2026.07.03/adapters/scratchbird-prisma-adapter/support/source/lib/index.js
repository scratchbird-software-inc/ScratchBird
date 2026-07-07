// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const {
  parseScratchbirdConnectionUrl,
  normalizeScratchbirdQueryParams,
  validatePrismaSchemaText,
} = require("./connection-url");
const {
  normalizeTypeName,
  mapScratchBirdTypeToPrisma,
  isPrismaSupportedScratchBirdType,
} = require("./type-map");
const { generatePrismaSchemaFromMetadata } = require("./schema-generator");
const {
  buildDeterministicMigrationPlan,
  runReflectionRoundTripContract,
} = require("./workflow");

module.exports = {
  parseScratchbirdConnectionUrl,
  normalizeScratchbirdQueryParams,
  validatePrismaSchemaText,
  normalizeTypeName,
  mapScratchBirdTypeToPrisma,
  isPrismaSupportedScratchBirdType,
  generatePrismaSchemaFromMetadata,
  buildDeterministicMigrationPlan,
  runReflectionRoundTripContract,
};
