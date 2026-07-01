# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

using Test
using ScratchBird

@testset "SBWP READY transaction state" begin
    compact = zeros(UInt8, 20)
    compact[1] = 0x01
    compact[5:12] = reinterpret(UInt8, [UInt64(77)])
    compact_status, compact_txn = ScratchBird.parse_ready(compact)
    @test compact_status == 0x01
    @test compact_txn == UInt64(77)

    extended = zeros(UInt8, 76)
    extended[49:56] = reinterpret(UInt8, [UInt64(88)])
    extended[57] = UInt8('T')
    extended_status, extended_txn = ScratchBird.parse_ready(extended)
    @test extended_status == 0x01
    @test extended_txn == UInt64(88)
end

@testset "SBWP P1 row description" begin
    payload = UInt8[]
    ScratchBird.append_le!(payload, UInt16(1))
    push!(payload, 0x00)
    push!(payload, 0x01)
    ScratchBird.append_le!(payload, Int32(1))
    append!(payload, zeros(UInt8, 72 - length(payload)))

    ScratchBird.append_le!(payload, Int32(1))
    push!(payload, 0x00)
    push!(payload, 0x01)
    push!(payload, 0x00)
    push!(payload, 0x00)
    append!(payload, zeros(UInt8, 8))
    type_ref = zeros(UInt8, 144)
    type_ref[1:2] = reinterpret(UInt8, [UInt16(8)])
    type_ref[3:4] = reinterpret(UInt8, [UInt16(1)])
    append!(payload, type_ref)
    append!(payload, zeros(UInt8, 16 * 3 + 4 + 2 + 2))
    name = Vector{UInt8}(codeunits("assertion_id"))
    push!(payload, 0x01)
    ScratchBird.append_le!(payload, Int32(length(name)))
    append!(payload, name)

    names, types = ScratchBird.parse_row_description(payload)
    @test names == ["assertion_id"]
    @test types == UInt32[25]
    @test !occursin('\0', names[1])
end

@testset "SBWP COPY message constants" begin
    @test ScratchBird.MSG_COPY_DATA == 0x0d
    @test ScratchBird.MSG_COPY_DONE == 0x0e
    @test ScratchBird.MSG_COPY_FAIL == 0x0f
    @test ScratchBird.MSG_COPY_IN_RESPONSE == 0x51
    @test ScratchBird.MSG_COPY_OUT_RESPONSE == 0x52
    @test ScratchBird.MSG_COPY_BOTH_RESPONSE == 0x53
end

@testset "SBWP transaction savepoint frames" begin
    @test ScratchBird.FEATURE_SAVEPOINTS == UInt64(1) << 9
    @test (ScratchBird.DEFAULT_STARTUP_FEATURES & ScratchBird.FEATURE_SAVEPOINTS) != 0
    @test ScratchBird.MSG_TXN_SAVEPOINT == 0x18
    @test ScratchBird.MSG_TXN_RELEASE == 0x19
    @test ScratchBird.MSG_TXN_ROLLBACK_TO == 0x1a
    payload = ScratchBird.savepoint_payload("sp_keep")
    @test payload[1:4] == reinterpret(UInt8, [UInt32(7)])
    @test String(payload[5:end]) == "sp_keep"
    @test_throws ScratchBird.ScratchBirdError ScratchBird.savepoint_payload("  ")
end

@testset "SBWP query payload builder" begin
    payload = ScratchBird.build_query_payload("COPY users.public.t FROM STDIN")
    @test length(payload) == 12 + length("COPY users.public.t FROM STDIN") + 1
    @test payload[1:4] == reinterpret(UInt8, [ScratchBird.QUERY_FLAG_BINARY_RESULT])
    @test payload[5:8] == reinterpret(UInt8, [UInt32(0)])
    @test payload[9:12] == reinterpret(UInt8, [UInt32(0)])
    @test payload[end] == 0x00
    @test String(payload[13:end - 1]) == "COPY users.public.t FROM STDIN"

    with_flags = ScratchBird.build_query_payload("SELECT 1"; extra_flags = ScratchBird.QUERY_FLAG_RETURN_SBLR)
    expected_flags = ScratchBird.QUERY_FLAG_BINARY_RESULT | ScratchBird.QUERY_FLAG_RETURN_SBLR
    @test with_flags[1:4] == reinterpret(UInt8, [expected_flags])
end

@testset "sb_isql_julia fail-closed artifacts" begin
    driver_root = normpath(joinpath(@__DIR__, ".."))
    tool = joinpath(driver_root, "tools", "sb_isql_julia.jl")
    mktempdir() do dir
        input = joinpath(dir, "input.sbsql")
        write(input, "SELECT 1;\n")
        run_root = joinpath(dir, "run")
        mkpath(run_root)
        command = Cmd(vcat([Base.julia_cmd().exec[1]], [
            "--startup-file=no",
            "--history-file=no",
            "--project=$(driver_root)",
            tool,
            "--database", "contract.sbdb",
            "--host", "127.0.0.1",
            "--port", "3092",
            "--user", "sysdba",
            "--password", "masterkey",
            "--role", "",
            "--sslmode", "require",
            "--sslrootcert", "",
            "--sslcert", "",
            "--sslkey", "",
            "--ipc-path", "",
            "--route", "listener-parser",
            "--parser-mode", "server-parser",
            "--page-size", "8k",
            "--namespace", "users.public.examples.julia.test",
            "--input", input,
            "--output", joinpath(run_root, "stdout.log"),
            "--error", joinpath(run_root, "stderr.log"),
            "--diagnostics", joinpath(run_root, "diagnostics.jsonl"),
            "--metrics", joinpath(run_root, "process-metrics.jsonl"),
            "--transcript", joinpath(run_root, "wire-transcript.jsonl"),
            "--summary", joinpath(run_root, "summary.json"),
        ]))
        child_stdout = joinpath(run_root, "child_stdout.log")
        child_stderr = joinpath(run_root, "child_stderr.log")
        process = run(pipeline(command, stdout = child_stdout, stderr = child_stderr); wait = false)
        wait(process)
        @test process.exitcode != 0
        if !isfile(joinpath(run_root, "summary.json"))
            println("sb_isql_julia child stdout:")
            isfile(child_stdout) && println(read(child_stdout, String))
            println("sb_isql_julia child stderr:")
            isfile(child_stderr) && println(read(child_stderr, String))
            println("sb_isql_julia child command:")
            println(command)
        end
        for name in [
            "summary.json",
            "diagnostics.jsonl",
            "wire-transcript.jsonl",
            "command-events.jsonl",
            "timing-groups.json",
            "result-digests.json",
            "metadata-snapshots.json",
            "route-environment.json",
            "process-metrics.jsonl",
            "security-refusals.json",
            "native-api-coverage.json",
            "code-example-review.json",
            "junit.xml",
            "stdout.log",
            "stderr.log",
        ]
            @test isfile(joinpath(run_root, name))
        end
        summary = read(joinpath(run_root, "summary.json"), String)
        diagnostics = read(joinpath(run_root, "diagnostics.jsonl"), String)
        @test occursin("\"driver_name\":\"julia\"", summary)
        @test occursin("\"status\":\"fail\"", summary)
        @test occursin("missing Julia package", diagnostics) ||
              occursin("no TLS transport implementation", diagnostics) ||
              occursin("TCP connect failed", diagnostics) ||
              occursin("connection refused", diagnostics)
    end
end
