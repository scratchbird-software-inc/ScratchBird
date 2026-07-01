// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const adapter = require("../lib/index");

const options = adapter.normalizeTypeOrmOptions({
  url: process.env.DATABASE_URL || "scratchbird://sb_admin:change_me@127.0.0.1:3092/main?sslmode=require&binaryTransfer=true",
  extra: {
    connectTimeout: "30",
  },
});

const metadataCatalog = {
  schemas: [
    {
      name: "sys",
      tables: [
        {
          name: "users",
          columns: [
            { name: "id", type: "BIGINT", nullable: false, identity: true },
            { name: "email", type: "VARCHAR", nullable: false },
          ],
          primaryKey: ["id"],
        },
        {
          name: "posts",
          columns: [
            { name: "id", type: "BIGINT", nullable: false, identity: true },
            { name: "user_id", type: "BIGINT", nullable: false },
            { name: "title", type: "VARCHAR", nullable: false },
          ],
          primaryKey: ["id"],
          relations: [
            {
              name: "user",
              type: "many-to-one",
              targetSchema: "sys",
              targetTable: "users",
              joinColumn: "user_id",
              referencedColumn: "id",
            },
          ],
        },
      ],
    },
  ],
};

const entities = adapter.generateEntitySchemas(metadataCatalog);
const transactionPlan = adapter.buildNestedCrudTransactionPlan({
  parentTable: "users",
  childTable: "posts",
  parentKey: "id",
  childFk: "user_id",
});

console.log("ScratchBird TypeORM sample config");
console.log(JSON.stringify({ options, entities, transactionPlan }, null, 2));
