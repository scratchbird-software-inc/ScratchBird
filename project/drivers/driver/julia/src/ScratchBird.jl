# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

module ScratchBird

import DBInterface
import Tables
import OpenSSL
using Sockets

export ScratchBirdDriver,
    ScratchBirdConnection,
    ScratchBirdStatement,
    ScratchBirdResult,
    SblrCompiled,
    ScratchBirdError,
    connect,
    query_metadata,
    begin_transaction!,
    commit!,
    rollback!,
    close

struct ScratchBirdError <: Exception
    sqlstate::String
    message::String
end

Base.showerror(io::IO, err::ScratchBirdError) = print(io, err.sqlstate, ": ", err.message)

struct ScratchBirdDriver end

mutable struct ScratchBirdConnection
    host::String
    port::Int
    database::String
    user::String
    password::String
    role::String
    sslmode::String
    transport::String
    ipc_path::String
    application_name::String
    parser_mode::String
    io::Any
    closed::Bool
    current_txn_id::Union{Nothing,String}
    attachment_id::Vector{UInt8}
    txn_id::UInt64
    sequence::UInt32
    parameters::Dict{String,String}
end

struct ScratchBirdStatement
    conn::ScratchBirdConnection
    sql::String
end

struct SblrCompiled
    hash::UInt64
    version::UInt32
    bytecode::Vector{UInt8}
end

struct ScratchBirdResult
    columns::Vector{Symbol}
    rows::Vector{NamedTuple}
    rowcount::Int
    command_tag::String
end

const PROTOCOL_MAGIC = UInt8[0x53, 0x42, 0x57, 0x50]
const PROTOCOL_VERSION = UInt16(0x0101)
const HEADER_SIZE = 40
const P1_ROW_DESCRIPTION_HEADER_BYTES = 72
const P1_CANONICAL_TYPE_REF_BYTES = 144
const FEATURE_SBLR = UInt64(1) << 2
const FEATURE_NOTIFICATIONS = UInt64(1) << 4
const FEATURE_QUERY_PLAN = UInt64(1) << 5
const FEATURE_STREAMING = UInt64(1) << 1
const QUERY_FLAG_BINARY_RESULT = UInt32(0x04)
const QUERY_FLAG_RETURN_SBLR = UInt32(0x10)
const MSG_STARTUP = UInt8(0x01)
const MSG_AUTH_RESPONSE = UInt8(0x02)
const MSG_QUERY = UInt8(0x03)
const MSG_SYNC = UInt8(0x09)
const MSG_TERMINATE = UInt8(0x0c)
const MSG_COPY_DATA = UInt8(0x0d)
const MSG_COPY_DONE = UInt8(0x0e)
const MSG_COPY_FAIL = UInt8(0x0f)
const MSG_SBLR_EXECUTE = UInt8(0x10)
const MSG_TXN_BEGIN = UInt8(0x15)
const MSG_TXN_COMMIT = UInt8(0x16)
const MSG_TXN_ROLLBACK = UInt8(0x17)
const MSG_AUTH_REQUEST = UInt8(0x40)
const MSG_AUTH_OK = UInt8(0x41)
const MSG_AUTH_CONTINUE = UInt8(0x42)
const MSG_READY = UInt8(0x43)
const MSG_ROW_DESCRIPTION = UInt8(0x44)
const MSG_DATA_ROW = UInt8(0x45)
const MSG_COMMAND_COMPLETE = UInt8(0x46)
const MSG_ERROR = UInt8(0x48)
const MSG_NOTICE = UInt8(0x49)
const MSG_PARAMETER_STATUS = UInt8(0x4f)
const MSG_COPY_IN_RESPONSE = UInt8(0x51)
const MSG_COPY_OUT_RESPONSE = UInt8(0x52)
const MSG_COPY_BOTH_RESPONSE = UInt8(0x53)
const MSG_SBLR_COMPILED = UInt8(0x57)
const MSG_TXN_STATUS = UInt8(0x5c)
const AUTH_OK = UInt8(0)
const AUTH_PASSWORD = UInt8(1)

Tables.istable(::Type{ScratchBirdResult}) = true
Tables.rowaccess(::Type{ScratchBirdResult}) = true
Tables.rows(result::ScratchBirdResult) = result.rows
Tables.columnnames(result::ScratchBirdResult) = Tuple(result.columns)
Tables.schema(result::ScratchBirdResult) = Tables.Schema(Tuple(result.columns), fill(Any, length(result.columns)))

connect(; kwargs...) = DBInterface.connect(ScratchBirdDriver(); kwargs...)

function DBInterface.connect(
    ::ScratchBirdDriver;
    database::AbstractString,
    user::AbstractString,
    password::AbstractString,
    host::AbstractString = "127.0.0.1",
    port::Integer = 3092,
    role::AbstractString = "",
    sslmode::AbstractString = "require",
    sslrootcert::AbstractString = "",
    sslcert::AbstractString = "",
    sslkey::AbstractString = "",
    transport::AbstractString = "inet",
    ipc_path::AbstractString = "",
    application_name::AbstractString = "ScratchBirdJulia",
    parser_mode::AbstractString = "server-parser",
    kwargs...,
)
    io = open_transport(
        String(transport),
        String(host),
        Int(port),
        String(ipc_path),
        String(sslmode),
        String(sslrootcert),
        String(sslcert),
        String(sslkey),
    )
    conn = ScratchBirdConnection(
        String(host),
        Int(port),
        String(database),
        String(user),
        String(password),
        String(role),
        String(sslmode),
        String(transport),
        String(ipc_path),
        String(application_name),
        String(parser_mode),
        io,
        false,
        nothing,
        zeros(UInt8, 16),
        UInt64(0),
        UInt32(0),
        Dict{String,String}(),
    )
    startup_and_auth!(conn)
    return conn
end

function open_transport(transport::String, host::String, port::Int, ipc_path::String, sslmode::String, sslrootcert::String, sslcert::String, sslkey::String)
    if transport == "embedded"
        throw(ScratchBirdError("0A000", "embedded transport is unavailable in the Julia lane because no ScratchBird C++ library boundary is exposed"))
    elseif transport == "ipc"
        isempty(ipc_path) && throw(ScratchBirdError("08001", "ipc_path is required for local IPC transport"))
        return Sockets.connect(ipc_path)
    elseif transport == "inet"
        if sslmode != "disable"
            return open_tls_transport(host, port, sslmode, sslrootcert, sslcert, sslkey)
        end
        return Sockets.connect(host, port)
    end
    throw(ScratchBirdError("08001", "unsupported ScratchBird transport: $(transport)"))
end

function open_tls_transport(host::String, port::Int, sslmode::String, sslrootcert::String, sslcert::String, sslkey::String)
    socket = Sockets.connect(host, port)
    try
        verify_peer = sslmode == "verify-ca" || sslmode == "verify-full"
        if !isempty(sslcert) || !isempty(sslkey)
            (!isempty(sslcert) && !isempty(sslkey)) || throw(ScratchBirdError("08001", "sslcert and sslkey must be supplied together"))
        end
        verify_file = isempty(sslrootcert) ? "" : sslrootcert
        ctx = OpenSSL.SSLContext(OpenSSL.TLSClientMethod(), verify_file)
        OpenSSL.ssl_set_min_protocol_version(ctx, OpenSSL.TLS1_3_VERSION)
        OpenSSL.ssl_set_options(ctx, OpenSSL.SSL_OP_NO_COMPRESSION)
        if !isempty(sslcert)
            OpenSSL.ssl_use_certificate(ctx, OpenSSL.X509Certificate(read(sslcert)))
            OpenSSL.ssl_use_private_key(ctx, OpenSSL.EvpPKey(read(sslkey)))
        end
        tls = OpenSSL.SSLStream(ctx, socket)
        OpenSSL.hostname!(tls, host)
        Sockets.connect(tls; require_ssl_verification = verify_peer)
        return tls
    catch err
        try
            Base.close(socket)
        catch
        end
        if err isa ScratchBirdError
            throw(err)
        end
        throw(ScratchBirdError("08001", "Julia TLS context creation failed for sslmode=$(sslmode): $(sprint(Base.showerror, err))"))
    end
end

function DBInterface.prepare(conn::ScratchBirdConnection, sql::AbstractString)
    ensure_open(conn)
    return ScratchBirdStatement(conn, String(sql))
end

function DBInterface.execute(stmt::ScratchBirdStatement, params = nothing; kwargs...)
    ensure_open(stmt.conn)
    if params !== nothing
        throw(ScratchBirdError("07002", "Julia ScratchBird DBInterface execution currently accepts SQL text without client-side bind rewriting"))
    end
    if stmt.conn.parser_mode == "server-parser"
        return execute_query!(stmt.conn, stmt.sql)
    end
    compiled = compile_sblr!(stmt.conn, stmt.sql)
    return execute_sblr!(stmt.conn, compiled)
end

function DBInterface.execute(conn::ScratchBirdConnection, sql::AbstractString, params = nothing; kwargs...)
    return DBInterface.execute(DBInterface.prepare(conn, sql), params; kwargs...)
end

function begin_transaction!(conn::ScratchBirdConnection; kwargs...)
    ensure_open(conn)
    payload = UInt8[]
    append_le!(payload, UInt16(0))
    append!(payload, UInt8[0, 0, 1, 0, 0, 0])
    append_le!(payload, UInt32(0))
    send_message!(conn, MSG_TXN_BEGIN, payload)
    drain_until_ready!(conn)
    return nothing
end

function commit!(conn::ScratchBirdConnection)
    ensure_open(conn)
    send_message!(conn, MSG_TXN_COMMIT, UInt8[0, 0, 0, 0])
    drain_until_ready!(conn)
    return nothing
end

function rollback!(conn::ScratchBirdConnection)
    ensure_open(conn)
    send_message!(conn, MSG_TXN_ROLLBACK, UInt8[0, 0, 0, 0])
    drain_until_ready!(conn)
    return nothing
end

function query_metadata(conn::ScratchBirdConnection, collection::AbstractString)
    ensure_open(conn)
    sql = metadata_sql(String(collection))
    return DBInterface.execute(conn, sql)
end

function metadata_sql(collection::String)
    if collection == "schemas"
        return "SELECT * FROM sys.schemas"
    elseif collection == "tables"
        return "SELECT * FROM sys.tables"
    elseif collection == "columns"
        return "SELECT * FROM sys.columns"
    elseif collection == "indexes"
        return "SELECT * FROM sys.indexes"
    elseif collection == "procedures"
        return "SELECT * FROM sys.procedures"
    elseif collection == "functions"
        return "SELECT * FROM sys.functions"
    end
    throw(ScratchBirdError("HY000", "unsupported metadata collection: $(collection)"))
end

function Base.close(conn::ScratchBirdConnection)
    if !conn.closed
        try
            send_message!(conn, MSG_TERMINATE, UInt8[])
        catch
        end
        try
            close(conn.io)
        catch
        end
        conn.closed = true
    end
    return nothing
end

function ensure_open(conn::ScratchBirdConnection)
    conn.closed && throw(ScratchBirdError("08003", "ScratchBird connection is closed"))
    return nothing
end

function startup_and_auth!(conn::ScratchBirdConnection)
    params = Dict{String,String}(
        "database" => conn.database,
        "user" => conn.user,
        "client_flags" => "256",
        "application_name" => conn.application_name,
    )
    if !isempty(conn.role)
        params["role"] = conn.role
    end
    payload = build_startup_payload(FEATURE_SBLR | FEATURE_NOTIFICATIONS | FEATURE_QUERY_PLAN | FEATURE_STREAMING, UInt64(0), params)
    send_message!(conn, MSG_STARTUP, payload; force_zero = true)
    while true
        msg_type, _flags, _sequence, attachment, txn_id, payload = recv_message(conn)
        if handle_async!(conn, msg_type, attachment, txn_id, payload)
            continue
        elseif msg_type == MSG_AUTH_REQUEST
            length(payload) < 1 && throw(ScratchBirdError("08006", "authentication request is truncated"))
            method = payload[1]
            if method == AUTH_OK
                continue
            elseif method == AUTH_PASSWORD
                send_message!(conn, MSG_AUTH_RESPONSE, Vector{UInt8}(codeunits(conn.password)); force_zero = true)
            else
                throw(ScratchBirdError("0A000", "Julia driver supports PASSWORD authentication for this release path; requested method $(method) requires a broker or dedicated auth plugin"))
            end
        elseif msg_type == MSG_AUTH_CONTINUE
            throw(ScratchBirdError("0A000", "Julia driver authentication continuation requires broker or external ceremony support for this release path"))
        elseif msg_type == MSG_AUTH_OK
            conn.attachment_id = attachment
            conn.txn_id = txn_id
        elseif msg_type == MSG_READY
            apply_ready!(conn, payload)
            return nothing
        elseif msg_type == MSG_ERROR
            raise_error(payload)
        end
    end
end

function execute_query!(conn::ScratchBirdConnection, sql::String; extra_flags::UInt32 = UInt32(0))
    send_message!(conn, MSG_QUERY, build_query_payload(sql; extra_flags = extra_flags))
    return read_result!(conn)
end

function build_query_payload(sql::String; extra_flags::UInt32 = UInt32(0), max_rows::UInt32 = UInt32(0), timeout_ms::UInt32 = UInt32(0))::Vector{UInt8}
    flags = QUERY_FLAG_BINARY_RESULT | extra_flags
    payload = UInt8[]
    append_le!(payload, flags)
    append_le!(payload, max_rows)
    append_le!(payload, timeout_ms)
    append!(payload, Vector{UInt8}(codeunits(sql)))
    push!(payload, 0)
    return payload
end

function compile_sblr!(conn::ScratchBirdConnection, sql::String)::SblrCompiled
    execute_query!(conn, sql; extra_flags = QUERY_FLAG_RETURN_SBLR)
    if !haskey(conn.parameters, "__last_sblr_hash")
        throw(ScratchBirdError("HY000", "parser did not return compiled SBLR"))
    end
    hash = parse(UInt64, conn.parameters["__last_sblr_hash"])
    version = UInt32(parse(UInt64, conn.parameters["__last_sblr_version"]))
    bytecode = hex2bytes(conn.parameters["__last_sblr_bytecode"])
    delete!(conn.parameters, "__last_sblr_hash")
    delete!(conn.parameters, "__last_sblr_version")
    delete!(conn.parameters, "__last_sblr_bytecode")
    return SblrCompiled(hash, version, bytecode)
end

function execute_sblr!(conn::ScratchBirdConnection, compiled::SblrCompiled)
    payload = UInt8[]
    append_le!(payload, compiled.hash)
    append_le!(payload, UInt32(length(compiled.bytecode)))
    append_le!(payload, UInt16(0))
    append_le!(payload, UInt16(0))
    append!(payload, compiled.bytecode)
    send_message!(conn, MSG_SBLR_EXECUTE, payload)
    send_message!(conn, MSG_SYNC, UInt8[])
    return read_result!(conn)
end

function read_result!(conn::ScratchBirdConnection)::ScratchBirdResult
    columns = Symbol[]
    column_types = UInt32[]
    rows = NamedTuple[]
    rowcount = -1
    command_tag = ""
    while true
        msg_type, _flags, _sequence, attachment, txn_id, payload = recv_message(conn)
        if handle_async!(conn, msg_type, attachment, txn_id, payload)
            continue
        elseif msg_type == MSG_ROW_DESCRIPTION
            names, types = parse_row_description(payload)
            columns = Symbol.(names)
            column_types = types
        elseif msg_type == MSG_DATA_ROW
            values = parse_data_row(payload, length(columns), column_types)
            if isempty(columns)
                columns = [Symbol("column$(idx)") for idx in 1:length(values)]
            end
            names = Tuple(columns)
            push!(rows, NamedTuple{names}(Tuple(values)))
        elseif msg_type == MSG_COMMAND_COMPLETE
            affected, tag = parse_command_complete(payload)
            rowcount = Int(min(affected, UInt64(typemax(Int))))
            command_tag = tag
        elseif msg_type == MSG_READY
            apply_ready!(conn, payload)
            return ScratchBirdResult(columns, rows, rowcount < 0 ? length(rows) : rowcount, command_tag)
        elseif msg_type == MSG_ERROR
            err = error_from_payload(payload)
            drain_until_ready!(conn)
            throw(err)
        end
    end
end

function drain_until_ready!(conn::ScratchBirdConnection)
    while true
        msg_type, _flags, _sequence, attachment, txn_id, payload = recv_message(conn)
        if handle_async!(conn, msg_type, attachment, txn_id, payload)
            continue
        elseif msg_type == MSG_READY
            apply_ready!(conn, payload)
            return nothing
        elseif msg_type == MSG_ERROR
            raise_error(payload)
        end
    end
end

function send_message!(conn::ScratchBirdConnection, msg_type::UInt8, payload::Vector{UInt8}; flags::UInt8 = UInt8(0), force_zero::Bool = false)
    attachment = force_zero ? zeros(UInt8, 16) : conn.attachment_id
    txn = force_zero ? UInt64(0) : conn.txn_id
    frame = UInt8[]
    append!(frame, PROTOCOL_MAGIC)
    append!(frame, UInt8[0x01, 0x01, msg_type, flags])
    append_le!(frame, UInt32(length(payload)))
    append_le!(frame, conn.sequence)
    append!(frame, attachment)
    append_le!(frame, txn)
    append!(frame, payload)
    conn.sequence += UInt32(1)
    write(conn.io, frame)
    flush(conn.io)
    return nothing
end

function recv_message(conn::ScratchBirdConnection)
    header = read_exact(conn.io, HEADER_SIZE)
    header[1:4] == PROTOCOL_MAGIC || throw(ScratchBirdError("08006", "invalid SBWP protocol magic"))
    header[5] == 0x01 && header[6] == 0x01 || throw(ScratchBirdError("08006", "unsupported SBWP protocol version"))
    length = read_u32(header, 9)
    sequence = read_u32(header, 13)
    attachment = Vector{UInt8}(header[17:32])
    txn_id = read_u64(header, 33)
    payload = length == 0 ? UInt8[] : read_exact(conn.io, Int(length))
    return header[7], header[8], sequence, attachment, txn_id, payload
end

function read_exact(io, n::Int)::Vector{UInt8}
    out = UInt8[]
    while length(out) < n
        chunk = Vector{UInt8}(undef, n - length(out))
        try
            GC.@preserve chunk unsafe_read(io, pointer(chunk), UInt(length(chunk)))
        catch err
            if err isa EOFError
                throw(ScratchBirdError("08006", "connection closed while reading from ScratchBird"))
            end
            rethrow()
        end
        append!(out, chunk)
    end
    return out
end

function build_startup_payload(client_features::UInt64, required_features::UInt64, params::Dict{String,String})::Vector{UInt8}
    out = UInt8[]
    append_le!(out, PROTOCOL_VERSION)
    append_le!(out, PROTOCOL_VERSION)
    append_le!(out, UInt32(0))
    append_le!(out, client_features)
    append_le!(out, required_features)
    append_le!(out, UInt64(0))
    append!(out, fill(UInt8(0x11), 16))
    append!(out, zeros(UInt8, 16))
    append!(out, zeros(UInt8, 16))
    append_le!(out, UInt32(length(params)))
    for key in sort(collect(keys(params)))
        append_lprefixed!(out, key)
        append!(out, UInt8[0x01, 0x00])
        value = Vector{UInt8}(codeunits(params[key]))
        append_le!(out, UInt32(length(value)))
        append!(out, value)
    end
    append_le!(out, UInt32(0))
    return out
end

function handle_async!(conn::ScratchBirdConnection, msg_type::UInt8, attachment::Vector{UInt8}, txn_id::UInt64, payload::Vector{UInt8})::Bool
    if msg_type == MSG_PARAMETER_STATUS
        for (name, value) in parse_parameter_status(payload)
            conn.parameters[name] = value
            if name == "attachment_id"
                parsed = parse_uuid_bytes(value)
                parsed !== nothing && (conn.attachment_id = parsed)
            elseif name == "current_txn_id"
                try
                    conn.txn_id = parse(UInt64, value)
                catch
                end
            end
        end
        return true
    elseif msg_type == MSG_NOTICE
        return true
    elseif msg_type == MSG_TXN_STATUS
        length(payload) >= 12 && (conn.txn_id = read_u64(payload, 5))
        return true
    elseif msg_type == MSG_SBLR_COMPILED
        hash, version, bytecode = parse_sblr_compiled(payload)
        conn.parameters["__last_sblr_hash"] = string(hash)
        conn.parameters["__last_sblr_version"] = string(version)
        conn.parameters["__last_sblr_bytecode"] = bytes2hex(bytecode)
        return true
    end
    return false
end

function parse_parameter_status(payload::Vector{UInt8})
    values = Tuple{String,String}[]
    if length(payload) >= 8
        count = read_u32(payload, 1)
        offset = 5
        ok = count > 0 && count <= 256
        if ok
            try
                for _ in 1:count
                    name, offset = read_lprefixed_string(payload, offset)
                    offset += 3
                    value, offset = read_lprefixed_string(payload, offset)
                    push!(values, (name, value))
                end
                offset == length(payload) + 1 && return values
            catch
                empty!(values)
            end
        end
    end
    offset = 1
    name, offset = read_lprefixed_string(payload, offset)
    value, _ = read_lprefixed_string(payload, offset)
    push!(values, (name, value))
    return values
end

function parse_row_description(payload::Vector{UInt8})
    if is_p1_row_description(payload)
        return parse_p1_row_description(payload)
    end
    length(payload) >= 4 || throw(ScratchBirdError("08006", "row description is truncated"))
    count = Int(read_u16(payload, 1))
    offset = 5
    names = String[]
    types = UInt32[]
    for _ in 1:count
        name, offset = read_lprefixed_string(payload, offset)
        offset + 17 <= length(payload) || throw(ScratchBirdError("08006", "row description field is truncated"))
        offset += 4
        offset += 2
        push!(types, read_u32(payload, offset))
        offset += 4
        offset += 2
        offset += 4
        offset += 1
        offset += 1
        offset += 2
        push!(names, isempty(name) ? "column$(length(names) + 1)" : name)
    end
    return names, types
end

function is_p1_row_description(payload::Vector{UInt8})::Bool
    return length(payload) >= P1_ROW_DESCRIPTION_HEADER_BYTES &&
        read_u16(payload, 1) == UInt16(1) &&
        payload[4] == UInt8(1)
end

function parse_p1_row_description(payload::Vector{UInt8})
    count = Int(read_i32(payload, 5))
    count >= 0 || throw(ScratchBirdError("08006", "P1 row description column count is invalid"))
    offset = P1_ROW_DESCRIPTION_HEADER_BYTES + 1
    names = String[]
    types = UInt32[]
    fixed_column_bytes = 4 + 4 + 8 + P1_CANONICAL_TYPE_REF_BYTES + 56
    for idx in 1:count
        offset + fixed_column_bytes - 1 <= length(payload) || throw(ScratchBirdError("08006", "P1 row description is truncated"))
        offset += 4
        offset += 1
        offset += 1
        offset += 1
        offset += 1
        offset += 8
        type_oid = oid_from_canonical_type_ref(payload, offset)
        offset += P1_CANONICAL_TYPE_REF_BYTES
        offset += 16 * 3
        offset += 4
        offset += 2
        offset += 2
        name, offset = read_nullable_text(payload, offset)
        push!(names, isempty(name) ? "column$(idx)" : name)
        push!(types, type_oid)
    end
    return names, types
end

function oid_from_canonical_type_ref(payload::Vector{UInt8}, offset::Int)::UInt32
    offset + 3 <= length(payload) || return UInt32(25)
    family = read_u16(payload, offset)
    code = read_u16(payload, offset + 2)
    if family == 1 && code == 1
        return UInt32(16)
    elseif family == 2 && code == 3
        return UInt32(23)
    elseif family == 2 && code == 4
        return UInt32(20)
    elseif family == 4 && code == 1
        return UInt32(1700)
    elseif family == 6 && code == 2
        return UInt32(701)
    elseif family == 8 && code == 1
        return UInt32(25)
    elseif family == 9
        return UInt32(17)
    elseif family == 11
        return code == 1 ? UInt32(1082) : (code == 2 ? UInt32(1083) : UInt32(1114))
    elseif family == 12
        return UInt32(1186)
    elseif family == 13
        return UInt32(2950)
    elseif family == 19
        return code == 3 ? UInt32(829) : UInt32(869)
    elseif family == 20
        return UInt32(114)
    end
    return UInt32(25)
end

function read_nullable_text(payload::Vector{UInt8}, offset::Int)
    offset + 4 <= length(payload) || throw(ScratchBirdError("08006", "nullable text is truncated"))
    tag = payload[offset]
    length_value = Int(read_i32(payload, offset + 1))
    length_value >= 0 || throw(ScratchBirdError("08006", "nullable text length is invalid"))
    offset += 5
    tag == 0 && return "", offset
    offset + length_value - 1 <= length(payload) || throw(ScratchBirdError("08006", "nullable text is truncated"))
    return String(payload[offset:offset + length_value - 1]), offset + length_value
end

function parse_data_row(payload::Vector{UInt8}, expected_count::Int, column_types::Vector{UInt32})
    length(payload) >= 4 || throw(ScratchBirdError("08006", "data row is truncated"))
    count = Int(read_u16(payload, 1))
    null_bytes = Int(read_u16(payload, 3))
    expected_count > 0 && count != expected_count && throw(ScratchBirdError("08006", "data row column count mismatch"))
    offset = 5 + null_bytes
    null_bitmap = payload[5:4 + null_bytes]
    values = Any[]
    for idx in 1:count
        byte_idx = div(idx - 1, 8) + 1
        bit_idx = mod(idx - 1, 8)
        is_null = byte_idx <= length(null_bitmap) && (null_bitmap[byte_idx] & (UInt8(1) << bit_idx)) != 0
        if is_null
            push!(values, nothing)
            continue
        end
        len = read_i32(payload, offset)
        offset += 4
        if len < 0
            push!(values, nothing)
            continue
        end
        data = payload[offset:offset + len - 1]
        offset += len
        oid = idx <= length(column_types) ? column_types[idx] : UInt32(0)
        push!(values, decode_value(oid, data))
    end
    return values
end

function decode_value(oid::UInt32, data::Vector{UInt8})
    if oid == 16 && length(data) >= 1
        return data[1] != 0
    elseif oid == 23 && length(data) == 4
        return reinterpret(Int32, data)[1]
    elseif oid == 20 && length(data) == 8
        return reinterpret(Int64, data)[1]
    elseif oid == 700 && length(data) == 4
        return reinterpret(Float32, data)[1]
    elseif oid == 701 && length(data) == 8
        return reinterpret(Float64, data)[1]
    end
    return String(data)
end

function parse_command_complete(payload::Vector{UInt8})
    length(payload) >= 20 || throw(ScratchBirdError("08006", "command complete is truncated"))
    rows = read_u64(payload, 5)
    tag_bytes = payload[21:end]
    nul = findfirst(==(UInt8(0)), tag_bytes)
    if nul !== nothing
        tag_bytes = tag_bytes[1:nul - 1]
    end
    return rows, String(tag_bytes)
end

function parse_sblr_compiled(payload::Vector{UInt8})
    length(payload) >= 16 || throw(ScratchBirdError("08006", "compiled SBLR payload is truncated"))
    hash = read_u64(payload, 1)
    version = read_u32(payload, 9)
    length_value = read_u32(payload, 13)
    bytecode = payload[17:16 + Int(length_value)]
    return hash, version, bytecode
end

function apply_ready!(conn::ScratchBirdConnection, payload::Vector{UInt8})
    status, txn = parse_ready(payload)
    conn.txn_id = status == 0 ? UInt64(0) : txn
    conn.current_txn_id = status == 0 ? nothing : string(txn)
end

function parse_ready(payload::Vector{UInt8})
    if length(payload) >= 76
        status_byte = payload[57]
        if status_byte in UInt8[0x49, 0x54, 0x45, 0x52, 0x41]
            txn = read_u64(payload, 49)
            status = (status_byte == 0x54 || status_byte == 0x45) ? UInt8(1) : UInt8(0)
            return status, txn
        end
    end
    length(payload) >= 20 || throw(ScratchBirdError("08006", "ready message is truncated"))
    return payload[1], read_u64(payload, 5)
end

function error_from_payload(payload::Vector{UInt8})::ScratchBirdError
    sqlstate = "HY000"
    message = "ScratchBird server returned an error"
    offset = 1
    while offset <= length(payload)
        code = Char(payload[offset])
        offset += 1
        code == Char(0) && break
        nextnul = findnext(==(UInt8(0)), payload, offset)
        nextnul === nothing && break
        value = String(payload[offset:nextnul - 1])
        if code == 'C'
            sqlstate = value
        elseif code == 'M'
            message = value
        elseif code == 'D'
            message *= "\nDETAIL: " * value
        end
        offset = nextnul + 1
    end
    return ScratchBirdError(sqlstate, message)
end

function raise_error(payload::Vector{UInt8})
    throw(error_from_payload(payload))
end

function append_lprefixed!(out::Vector{UInt8}, value::String)
    data = Vector{UInt8}(codeunits(value))
    append_le!(out, UInt32(length(data)))
    append!(out, data)
end

function read_lprefixed_string(payload::Vector{UInt8}, offset::Int)
    offset + 3 <= length(payload) || throw(ScratchBirdError("08006", "length-prefixed string is truncated"))
    len = Int(read_u32(payload, offset))
    start = offset + 4
    stop = start + len - 1
    stop <= length(payload) || throw(ScratchBirdError("08006", "length-prefixed string data is truncated"))
    return String(payload[start:stop]), stop + 1
end

function append_le!(out::Vector{UInt8}, value)
    io = IOBuffer()
    write(io, value)
    append!(out, take!(io))
end

read_u16(data::Vector{UInt8}, offset::Int)::UInt16 = reinterpret(UInt16, data[offset:offset + 1])[1]
read_u32(data::Vector{UInt8}, offset::Int)::UInt32 = reinterpret(UInt32, data[offset:offset + 3])[1]
read_u64(data::Vector{UInt8}, offset::Int)::UInt64 = reinterpret(UInt64, data[offset:offset + 7])[1]
read_i32(data::Vector{UInt8}, offset::Int)::Int32 = reinterpret(Int32, data[offset:offset + 3])[1]

function parse_uuid_bytes(value::String)
    hex = replace(strip(value), "-" => "")
    length(hex) == 32 || return nothing
    try
        return hex2bytes(hex)
    catch
        return nothing
    end
end

end
