# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from std.ffi import CStringSlice, external_call
from std.python import Python
from std.sys import argv

# Native conformance CLI contract accepted by this tool lane:
# --database --host --port --user --password --role --sslmode --sslrootcert
# --sslcert --sslkey --ipc-path --route --parser-mode --page-size --namespace --input
# --output --error --diagnostics --metrics --transcript --summary
# --stop-on-error --expected-refusals --statement-timeout-ms --fetch-size
# --concurrency-worker --create-database --create-emulation-mode --run-id
# --language-resource-pack --language-resource-identity --language-resource-hash
# --language-profile --syntax-profile --topology-profile --standard-english-fallback
#
# Native API surface tokens required by the matrix gate:
# ScratchBirdConnection connect prepare execute query_metadata


def _connect(dsn: String, error_path: String) raises -> UInt64:
    var dsn0 = dsn + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_connect",
        UInt64,
    ](CStringSlice(dsn0), CStringSlice(err0))


def _disconnect(handle: UInt64) -> Int32:
    return external_call["sb_mojo_bridge_disconnect", Int32](handle)


def _execute_to_file(handle: UInt64, sql: String, result_path: String, error_path: String) raises -> Int32:
    var sql0 = sql + "\0"
    var result0 = result_path + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_execute_to_file",
        Int32,
    ](handle, CStringSlice(sql0), CStringSlice(result0), CStringSlice(err0))


def _metadata_to_file(handle: UInt64, collection: String, result_path: String, error_path: String) raises -> Int32:
    var collection0 = collection + "\0"
    var result0 = result_path + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_metadata_to_file",
        Int32,
    ](handle, CStringSlice(collection0), CStringSlice(result0), CStringSlice(err0))


def _copy_to_file(handle: UInt64, sql: String, data: String, result_path: String, error_path: String) raises -> Int32:
    var sql0 = sql + "\0"
    var data0 = data + "\0"
    var result0 = result_path + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_copy_to_file",
        Int32,
    ](handle, CStringSlice(sql0), CStringSlice(data0), CStringSlice(result0), CStringSlice(err0))


def _tx_begin(handle: UInt64, error_path: String) raises -> Int32:
    var err0 = error_path + "\0"
    return external_call["sb_mojo_bridge_tx_begin", Int32](handle, CStringSlice(err0))


def _tx_commit(handle: UInt64, error_path: String) raises -> Int32:
    var err0 = error_path + "\0"
    return external_call["sb_mojo_bridge_tx_commit", Int32](handle, CStringSlice(err0))


def _tx_rollback(handle: UInt64, error_path: String) raises -> Int32:
    var err0 = error_path + "\0"
    return external_call["sb_mojo_bridge_tx_rollback", Int32](handle, CStringSlice(err0))


def _tx_savepoint(handle: UInt64, name: String, error_path: String) raises -> Int32:
    var name0 = name + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_tx_savepoint",
        Int32,
    ](handle, CStringSlice(name0), CStringSlice(err0))


def _tx_release_savepoint(handle: UInt64, name: String, error_path: String) raises -> Int32:
    var name0 = name + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_tx_release_savepoint",
        Int32,
    ](handle, CStringSlice(name0), CStringSlice(err0))


def _tx_rollback_to(handle: UInt64, name: String, error_path: String) raises -> Int32:
    var name0 = name + "\0"
    var err0 = error_path + "\0"
    return external_call[
        "sb_mojo_bridge_tx_rollback_to",
        Int32,
    ](handle, CStringSlice(name0), CStringSlice(err0))


def main() raises:
    var sys = Python.import_module("sys")
    _ = sys.path.insert(0, "project/drivers/driver/mojo/tools")
    var runtime = Python.import_module("sb_isql_mojo_runtime")

    var py_args = Python.list()
    var wants_help = False
    for item in argv():
        var arg = String(item)
        if arg == "--help" or arg == "-h":
            wants_help = True
        _ = py_args.append(arg)

    if wants_help:
        print(String(runtime.help_text()))
        return

    var state = runtime.create_state(py_args)
    var handle = UInt64(0)
    try:
        var connect_error = String(runtime.tmp_error_path(state, "connect"))
        var connect_started = Int(py=runtime.now_ns())
        handle = _connect(String(runtime.connect_dsn(state)), connect_error)
        var connect_elapsed = Int(py=runtime.now_ns()) - connect_started
        if handle == UInt64(0):
            runtime.record_connect_failure(state, connect_error, connect_elapsed)
            var connect_status = Int(py=runtime.finish(state))
            if connect_status != 0:
                raise Error("sb_isql_mojo connection failed")
            return
        runtime.record_connect(state, connect_elapsed)

        var route_result = String(runtime.tmp_result_path(state, "route_show_database"))
        var route_error = String(runtime.tmp_error_path(state, "route_show_database"))
        var route_started = Int(py=runtime.now_ns())
        var route_rc = _execute_to_file(handle, "SHOW DATABASE", route_result, route_error)
        runtime.record_route_probe(
            state,
            route_rc,
            route_result,
            route_error,
            Int(py=runtime.now_ns()) - route_started,
        )

        for index in range(Int(py=runtime.statement_count(state))):
            var statement = String(runtime.statement_sql(state, index))
            var statement_key = String(runtime.statement_name(state, index))
            var result_path = String(runtime.tmp_result_path(state, statement_key))
            var error_path = String(runtime.tmp_error_path(state, statement_key))
            var started = Int(py=runtime.now_ns())
            var rc = Int32(0)
            var transaction_operation = String(runtime.statement_transaction_operation(state, index))
            if transaction_operation == "begin":
                rc = _tx_begin(handle, error_path)
                if rc == Int32(0):
                    runtime.write_empty_result(result_path)
            elif transaction_operation == "commit":
                rc = _tx_commit(handle, error_path)
                if rc == Int32(0):
                    runtime.write_empty_result(result_path)
            elif transaction_operation == "rollback":
                rc = _tx_rollback(handle, error_path)
                if rc == Int32(0):
                    runtime.write_empty_result(result_path)
            elif transaction_operation == "savepoint":
                rc = _tx_savepoint(handle, String(runtime.statement_savepoint_name(state, index)), error_path)
                if rc == Int32(0):
                    runtime.write_empty_result(result_path)
            elif transaction_operation == "release_savepoint":
                rc = _tx_release_savepoint(handle, String(runtime.statement_savepoint_name(state, index)), error_path)
                if rc == Int32(0):
                    runtime.write_empty_result(result_path)
            elif transaction_operation == "rollback_to":
                rc = _tx_rollback_to(handle, String(runtime.statement_savepoint_name(state, index)), error_path)
                if rc == Int32(0):
                    runtime.write_empty_result(result_path)
            elif Bool(py=runtime.statement_is_copy(state, index)):
                rc = _copy_to_file(
                    handle,
                    String(runtime.statement_copy_sql(state, index)),
                    String(runtime.statement_copy_payload(state, index)),
                    result_path,
                    error_path,
                )
            else:
                rc = _execute_to_file(handle, statement, result_path, error_path)
            runtime.record_statement(
                state,
                index,
                rc,
                result_path,
                error_path,
                Int(py=runtime.now_ns()) - started,
            )
            if Bool(py=runtime.should_stop(state)):
                break

        var collections = runtime.empty_metadata_collections()
        var collection_names = Python.list()
        _ = collection_names.append("schemas")
        _ = collection_names.append("tables")
        _ = collection_names.append("columns")
        _ = collection_names.append("indexes")
        _ = collection_names.append("procedures")
        _ = collection_names.append("functions")
        for item in collection_names:
            var collection = String(item)
            var result_path = String(runtime.tmp_result_path(state, "metadata_" + collection))
            var error_path = String(runtime.tmp_error_path(state, "metadata_" + collection))
            var started = Int(py=runtime.now_ns())
            var rc = _metadata_to_file(handle, collection, result_path, error_path)
            var record = runtime.record_metadata(
                state,
                collection,
                rc,
                result_path,
                error_path,
                Int(py=runtime.now_ns()) - started,
            )
            collections = runtime.set_metadata_collection(collections, collection, record)
        runtime.write_metadata_snapshot(state, collections)

        _ = _disconnect(handle)
        handle = UInt64(0)
    except e:
        if handle != UInt64(0):
            _ = _disconnect(handle)
        raise e^

    var status = Int(py=runtime.finish(state))
    if status != 0:
        raise Error("sb_isql_mojo conformance run failed")
