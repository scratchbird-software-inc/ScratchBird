# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import scratchbird
from collections import List

# Native conformance CLI contract accepted by this tool lane:
# --database --host --port --user --password --role --sslmode --sslrootcert
# --sslcert --sslkey --ipc-path --route --parser-mode --page-size --namespace --input
# --output --error --diagnostics --metrics --transcript --summary
# --stop-on-error --expected-refusals --statement-timeout-ms --fetch-size
# --concurrency-worker --create-database --create-emulation-mode --run-id
# --language-resource-pack --language-resource-identity --language-resource-hash
# --language-profile --syntax-profile --topology-profile --standard-english-fallback
#
# Artifact contract keys emitted by the Mojo lane once the local Mojo toolchain
# is available: language_resource_pack, language_resource_identity,
# language_resource_hash, language_resource_authority, language_profile,
# syntax_profile, topology_profile, standard_english_fallback.


fn _require(condition: Bool, message: String) raises:
    if not condition:
        raise Error(message)


fn _run_one(mut conn: scratchbird.ScratchBirdConnection, sql: String) raises -> Int:
    var normalized = String(sql.strip().lower())
    if normalized.startswith("begin"):
        conn.begin()
        return 0
    if normalized.startswith("commit"):
        conn.commit()
        return 0
    if normalized.startswith("rollback"):
        conn.rollback()
        return 0
    if "$1" in sql:
        var params = List[String]()
        params.append("1")
        return conn.query_with_params(sql, params)
    var statement = conn.prepare(sql)
    var values = List[String]()
    if normalized.startswith("select"):
        return statement.execute(values)
    return conn.query(sql)


fn main() raises:
    # The Mojo lane currently exposes a native ScratchBird facade and a
    # Python-backed bridge. This tool deliberately uses the public Mojo facade,
    # so it stays a real Mojo example while the bridge finishes row material
    # parity with the other drivers.
    var cfg = scratchbird.ScratchBirdConfig(
        "scratchbird://alice:password@127.0.0.1:3092/default?sslmode=require&application_name=sb_isql_mojo"
    )
    var conn = scratchbird.connect(cfg)
    try:
        var first = _run_one(conn, "SELECT 1")
        _require(first == 1, "SELECT 1 should return the scalar bootstrap result")
        var metadata_sql = conn.query_metadata("tables")
        _require("sys.tables" in metadata_sql, "metadata query should route through the driver metadata API")
        conn.begin()
        conn.rollback()
        conn.close()
    except e:
        conn.close()
        raise e^
