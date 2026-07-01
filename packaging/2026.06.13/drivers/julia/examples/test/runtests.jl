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
