# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule SBIsqlElixir.Json do
  def encode(value) when is_map(value) do
    "{" <>
      (value
       |> Enum.map(fn {key, val} -> encode(to_string(key)) <> ":" <> encode(val) end)
       |> Enum.join(",")) <> "}"
  end

  def encode(value) when is_list(value) do
    "[" <> (value |> Enum.map(&encode/1) |> Enum.join(",")) <> "]"
  end

  def encode(value) when is_binary(value) do
    "\"" <>
      (value
       |> String.replace("\\", "\\\\")
       |> String.replace("\"", "\\\"")
       |> String.replace("\n", "\\n")
       |> String.replace("\r", "\\r")
       |> String.replace("\t", "\\t")) <> "\""
  end

  def encode(value) when is_integer(value) or is_float(value), do: to_string(value)
  def encode(value) when is_boolean(value), do: if(value, do: "true", else: "false")
  def encode(nil), do: "null"
  def encode(value), do: encode(inspect(value))
end

defmodule SBIsqlElixir do
  @page_sizes ~w(4k 8k 16k 32k 64k 128k)
  @routes ~w(embedded ipc_local listener-parser manager-listener-parser)
  @parser_modes ~w(server-parser standalone-parser driver-sblr-uuid)

  def main(raw) do
    args = parse_args(raw)
    System.halt(run(args))
  rescue
    error ->
      IO.puts(:stderr, Exception.message(error))
      System.halt(1)
  end

  def run(args) do
    validate!(args)
    run_root = args |> required!("--summary") |> Path.dirname()
    File.mkdir_p!(run_root)

    paths = %{
      events: Path.join(run_root, "command-events.jsonl"),
      wire: Path.join(run_root, "wire-transcript.jsonl"),
      timing: Path.join(run_root, "timing-groups.json"),
      digests: Path.join(run_root, "result-digests.json"),
      metadata: Path.join(run_root, "metadata-snapshots.json"),
      refusals: Path.join(run_root, "security-refusals.json"),
      api: Path.join(run_root, "native-api-coverage.json"),
      review: Path.join(run_root, "code-example-review.json"),
      junit: Path.join(run_root, "junit.xml"),
      stdout: Path.join(run_root, "stdout.log"),
      stderr: Path.join(run_root, "stderr.log")
    }

    [
      required!(args, "--output"),
      required!(args, "--error"),
      required!(args, "--diagnostics"),
      required!(args, "--metrics"),
      required!(args, "--transcript"),
      required!(args, "--summary")
      | Map.values(paths)
    ]
    |> Enum.each(&write_text(&1, ""))

    started = monotonic_ns()

    state = %{
      timings: %{},
      api: %{
        "ScratchBird.Connection.connect" => 0,
        "ScratchBird.Connection.query" => 0,
        "ScratchBird.Connection.begin" => 0,
        "ScratchBird.Connection.commit" => 0,
        "ScratchBird.Connection.rollback" => 0,
        "ScratchBird.Connection.close" => 0
      },
      testcases: [],
      failures: [],
      digests: [],
      refusals: [],
      connection: nil
    }

    state =
      try do
        opts = [
          host: required!(args, "--host"),
          port: args |> required!("--port") |> String.to_integer(),
          database: required!(args, "--database"),
          user: required!(args, "--user"),
          password: required!(args, "--password"),
          role: Map.get(args, "--role", ""),
          sslmode: Map.get(args, "--sslmode", "require"),
          front_door_mode:
            if(required!(args, "--route") == "manager-listener-parser",
              do: "manager_proxy",
              else: "direct"
            ),
          metadata_expand_schema_parents: true,
          application_name: "SBIsqlElixir"
        ]

        connect_started = monotonic_ns()

        case ScratchBird.Connection.connect(opts) do
          {:ok, conn} ->
            state =
              state
              |> put_in([:connection], conn)
              |> bump_api("ScratchBird.Connection.connect")
              |> add_timing("connection", connect_started)

            append_jsonl(required!(args, "--transcript"), %{
              event: "connect",
              driver: "elixir",
              route: required!(args, "--route"),
              parser_mode: required!(args, "--parser-mode"),
              page_size: required!(args, "--page-size")
            })

            append_jsonl(paths.wire, %{
              event: "server_admission_required",
              driver_or_parser_finality: "forbidden"
            })

            state

          {:error, reason} ->
            add_failure(state, "connect", inspect(reason))
        end
      rescue
        error -> add_failure(state, "connect", Exception.message(error))
      end

    state =
      cond do
        state.failures != [] ->
          state

        Map.has_key?(args, "--create-database") ->
          add_failure(
            state,
            "database_create",
            "--create-database is not implemented in the Elixir native tool yet"
          )

        required!(args, "--parser-mode") != "server-parser" ->
          add_failure(
            state,
            "parser_mode",
            "#{required!(args, "--parser-mode")} is not yet implemented by the Elixir native tool; it fails closed"
          )

        true ->
          run_statements(state, args, paths)
      end

    state =
      if state.failures == [] do
        metadata_started = monotonic_ns()

        case ScratchBird.Connection.query(
               state.connection,
               "SELECT * FROM sys.metadata.tables",
               []
             ) do
          {:ok, result, conn} ->
            write_text(
              paths.metadata,
              SBIsqlElixir.Json.encode(%{
                tables_digest: sha256(inspect(result.rows)),
                row_count: length(result.rows)
              }) <> "\n"
            )

            %{state | connection: conn}
            |> bump_api("ScratchBird.Connection.query")
            |> add_timing("metadata", metadata_started)

          {:error, reason, conn} ->
            %{state | connection: conn}
            |> add_failure("metadata", inspect(reason))
        end
      else
        state
      end

    state =
      if state.connection do
        ScratchBird.Connection.close(state.connection)
        bump_api(state, "ScratchBird.Connection.close")
      else
        state
      end

    elapsed = monotonic_ns() - started
    timings = Map.put(state.timings, "overall", elapsed)

    summary = %{
      run_id: Map.get(args, "--run-id", "manual"),
      driver_name: "elixir",
      route: required!(args, "--route"),
      parser_mode: required!(args, "--parser-mode"),
      page_size: required!(args, "--page-size"),
      namespace: required!(args, "--namespace"),
      status: if(state.failures == [], do: "pass", else: "fail"),
      failure_count: length(state.failures),
      elapsed_ns: elapsed,
      server_revalidation_required: true,
      driver_or_parser_finality: "forbidden",
      mga_authority: "engine"
    }

    write_text(required!(args, "--summary"), SBIsqlElixir.Json.encode(summary) <> "\n")
    write_text(required!(args, "--metrics"), SBIsqlElixir.Json.encode(timings) <> "\n")
    write_text(paths.timing, SBIsqlElixir.Json.encode(timings) <> "\n")
    write_text(paths.digests, SBIsqlElixir.Json.encode(Enum.reverse(state.digests)) <> "\n")
    write_text(paths.refusals, SBIsqlElixir.Json.encode(Enum.reverse(state.refusals)) <> "\n")
    write_text(paths.api, SBIsqlElixir.Json.encode(state.api) <> "\n")

    write_text(
      paths.review,
      SBIsqlElixir.Json.encode(%{
        driver: "elixir",
        public_api_only: true,
        shells_out_to_other_driver: false,
        source_is_canonical_example: true,
        sections: ["connection", "query", "fetch", "metadata", "diagnostics", "transaction"]
      }) <> "\n"
    )

    write_text(
      paths.junit,
      junit(
        "SBIsqlElixir",
        "scratchbird.elixir",
        Enum.reverse(state.testcases),
        Enum.reverse(state.failures)
      )
    )

    append_text(paths.stdout, "SBIsqlElixir status=#{summary.status}\n")

    if state.failures == [], do: 0, else: 1
  end

  defp run_statements(state, args, paths) do
    required!(args, "--input")
    |> read_input()
    |> split_statements()
    |> Enum.with_index(1)
    |> Enum.reduce_while(state, fn {sql, index}, state ->
      statement_id = "#{Path.basename(required!(args, "--input"))}:#{index}"
      group = classify(sql)
      started = monotonic_ns()

      {state, outcome, row_count, result_digest, sqlstate, diagnostic} =
        case execute_statement(state, sql, group) do
          {:ok, result, state} ->
            rows = Map.get(result, :rows, [])

            append_text(
              required!(args, "--output"),
              SBIsqlElixir.Json.encode(%{statement_id: statement_id, rows: rows}) <> "\n"
            )

            digest = sha256(inspect(rows))

            state = %{
              state
              | digests: [
                  %{statement_id: statement_id, row_count: length(rows), result_digest: digest}
                  | state.digests
                ]
            }

            {state, "success", length(rows), digest, nil, nil}

          {:error, reason, state} ->
            message = inspect(reason)

            append_jsonl(required!(args, "--diagnostics"), %{
              statement_id: statement_id,
              sqlstate: "HY000",
              message: message
            })

            append_text(required!(args, "--error"), "#{statement_id}: #{message}\n")
            state = add_failure(state, statement_id, message)
            {state, "refusal", -1, nil, "HY000", message}
        end

      elapsed = monotonic_ns() - started
      state = add_timing(state, group, started)

      event = %{
        run_id: Map.get(args, "--run-id", "manual"),
        driver_name: "elixir",
        driver_version: "unknown",
        route: required!(args, "--route"),
        parser_mode: required!(args, "--parser-mode"),
        page_size: required!(args, "--page-size"),
        namespace: required!(args, "--namespace"),
        script: required!(args, "--input"),
        statement_index: index,
        statement_id: statement_id,
        command_group: group,
        sql_hash: sha256(sql),
        expected_outcome: "success",
        actual_outcome: outcome,
        sqlstate: sqlstate,
        diagnostic_code: diagnostic,
        canonical_message_vector: [],
        row_count: row_count,
        result_digest: result_digest,
        elapsed_ns: elapsed,
        server_revalidation_state: "required",
        transaction_id_observed: nil,
        mga_authority: "engine",
        native_api_surface: "elixir",
        code_example_section: "connection_query_fetch"
      }

      append_jsonl(paths.events, event)
      state = %{state | testcases: [event | state.testcases]}

      if Map.has_key?(args, "--stop-on-error") and state.failures != [] do
        {:halt, state}
      else
        {:cont, state}
      end
    end)
  end

  defp execute_statement(state, sql, "transaction") do
    first = sql |> String.trim() |> String.downcase() |> String.split(~r/\s+/, parts: 2) |> hd()

    case first do
      "commit" ->
        case ScratchBird.Connection.commit(state.connection) do
          {:ok, conn} ->
            {:ok, %{rows: []},
             %{state | connection: conn} |> bump_api("ScratchBird.Connection.commit")}

          {:error, reason, conn} ->
            {:error, reason, %{state | connection: conn}}
        end

      "rollback" ->
        case ScratchBird.Connection.rollback(state.connection) do
          {:ok, conn} ->
            {:ok, %{rows: []},
             %{state | connection: conn} |> bump_api("ScratchBird.Connection.rollback")}

          {:error, reason, conn} ->
            {:error, reason, %{state | connection: conn}}
        end

      _ ->
        case ScratchBird.Connection.begin(state.connection) do
          {:ok, conn} ->
            {:ok, %{rows: []},
             %{state | connection: conn} |> bump_api("ScratchBird.Connection.begin")}

          {:error, reason, conn} ->
            {:error, reason, %{state | connection: conn}}
        end
    end
  end

  defp execute_statement(state, sql, _group) do
    case ScratchBird.Connection.query(state.connection, sql, []) do
      {:ok, result, conn} ->
        {:ok, result, %{state | connection: conn} |> bump_api("ScratchBird.Connection.query")}

      {:error, reason, conn} ->
        {:error, reason, %{state | connection: conn}}
    end
  end

  defp parse_args(raw) do
    parse_args(raw, %{})
  end

  defp parse_args([], args), do: args

  defp parse_args(["--stop-on-error" | rest], args),
    do: parse_args(rest, Map.put(args, "--stop-on-error", "true"))

  defp parse_args(["--create-database" | rest], args),
    do: parse_args(rest, Map.put(args, "--create-database", "true"))

  defp parse_args([key, value | rest], args) do
    cond do
      not String.starts_with?(key, "--") ->
        raise("unexpected positional argument: #{key}")

      String.starts_with?(value, "--") ->
        raise("missing value for #{key}")

      true ->
        parse_args(rest, Map.put(args, key, value))
    end
  end

  defp parse_args([key | _], _args), do: raise("missing value for #{key}")

  defp validate!(args) do
    unless required!(args, "--page-size") in @page_sizes,
      do: raise("unsupported page size: #{required!(args, "--page-size")}")

    unless required!(args, "--route") in @routes,
      do: raise("unsupported route: #{required!(args, "--route")}")

    unless required!(args, "--parser-mode") in @parser_modes,
      do: raise("unsupported parser mode: #{required!(args, "--parser-mode")}")
  end

  defp split_statements(script) do
    script
    |> String.split(";")
    |> Enum.map(&String.trim/1)
    |> Enum.reject(&(&1 == ""))
  end

  defp classify(sql) do
    trimmed = String.downcase(String.trim(sql))
    first = trimmed |> String.split(~r/\s+/, parts: 2) |> hd()

    cond do
      first in ["create", "alter", "drop"] -> "ddl"
      first in ["insert", "update", "delete", "merge", "upsert"] -> "dml"
      first in ["commit", "rollback", "savepoint", "begin", "start"] -> "transaction"
      first in ["grant", "revoke"] -> "security_refusal"
      String.contains?(trimmed, "sys.") -> "metadata"
      true -> "query"
    end
  end

  defp read_input("-"), do: IO.read(:stdio, :all)
  defp read_input(path), do: File.read!(path)

  defp required!(args, key) do
    case Map.get(args, key) do
      nil -> raise("missing required argument #{key}")
      "" -> raise("missing required argument #{key}")
      value -> value
    end
  end

  defp monotonic_ns, do: System.monotonic_time(:nanosecond)

  defp add_timing(%{timings: timings} = state, group, started) do
    %{
      state
      | timings:
          Map.update(timings, group, monotonic_ns() - started, &(&1 + monotonic_ns() - started))
    }
  end

  defp add_timing(timings, group, started) do
    Map.update(timings, group, monotonic_ns() - started, &(&1 + monotonic_ns() - started))
  end

  defp bump_api(%{api: api} = state, key), do: %{state | api: Map.update(api, key, 1, &(&1 + 1))}

  defp add_failure(state, statement_id, message),
    do: %{state | failures: [%{statement_id: statement_id, message: message} | state.failures]}

  defp sha256(text), do: "sha256:" <> (:crypto.hash(:sha256, text) |> Base.encode16(case: :lower))

  defp write_text(path, text) do
    File.mkdir_p!(Path.dirname(path))
    File.write!(path, text)
  end

  defp append_text(path, text) do
    File.mkdir_p!(Path.dirname(path))
    File.write!(path, text, [:append])
  end

  defp append_jsonl(path, record), do: append_text(path, SBIsqlElixir.Json.encode(record) <> "\n")

  defp junit(suite, class, testcases, failures) do
    rows = [
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
      "<testsuite name=\"#{escape_xml(suite)}\" tests=\"#{max(length(testcases), 1)}\" failures=\"#{length(failures)}\">"
    ]

    rows =
      if testcases == [] do
        rows ++ ["  <testcase classname=\"#{escape_xml(class)}\" name=\"run\"></testcase>"]
      else
        rows ++
          Enum.map(
            testcases,
            &"  <testcase classname=\"#{escape_xml(class)}\" name=\"#{escape_xml(to_string(&1.statement_id))}\"></testcase>"
          )
      end

    rows =
      rows ++
        Enum.map(failures, fn failure ->
          "  <testcase classname=\"#{escape_xml(class)}\" name=\"#{escape_xml(to_string(failure.statement_id))}\"><failure message=\"#{escape_xml(to_string(failure.message))}\" /></testcase>"
        end)

    Enum.join(rows ++ ["</testsuite>", ""], "\n")
  end

  defp escape_xml(text) do
    text
    |> String.replace("&", "&amp;")
    |> String.replace("\"", "&quot;")
    |> String.replace("<", "&lt;")
    |> String.replace(">", "&gt;")
  end
end

SBIsqlElixir.main(System.argv())
