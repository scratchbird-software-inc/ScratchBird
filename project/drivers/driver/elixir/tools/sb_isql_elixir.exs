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
  @ssl_modes ~w(allow disable prefer require verify-ca verify-full)
  @supported_args ~w(
    --database
    --host
    --port
    --user
    --password
    --role
    --sslmode
    --sslrootcert
    --sslcert
    --sslkey
    --ipc-path
    --route
    --parser-mode
    --page-size
    --namespace
    --input
    --output
    --error
    --diagnostics
    --metrics
    --transcript
    --summary
    --stop-on-error
    --expected-refusals
    --statement-timeout-ms
    --fetch-size
    --concurrency-worker
    --create-database
    --create-emulation-mode
    --run-id
    --language-resource-pack
    --language-resource-identity
    --language-resource-hash
    --language-profile
    --syntax-profile
    --topology-profile
    --standard-english-fallback
  )

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
      process: Path.join(run_root, "process-metrics.jsonl"),
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
    expected_refusals = load_expected_refusals(Map.get(args, "--expected-refusals", ""))
    route = required!(args, "--route")
    sslmode = effective_sslmode(route, Map.get(args, "--sslmode", "require"))

    state = %{
      timings: %{},
      api: %{
        "ScratchBird.Connection.connect" => 0,
        "ScratchBird.Connection.query" => 0,
        "ScratchBird.Connection.attach_create" => 0,
        "ScratchBird.Connection.begin" => 0,
        "ScratchBird.Connection.commit" => 0,
        "ScratchBird.Connection.rollback" => 0,
        "ScratchBird.Connection.close" => 0,
        "ScratchBird.Connection.copy_in" => 0
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
          sslmode: sslmode,
          sslrootcert: Map.get(args, "--sslrootcert", ""),
          sslcert: Map.get(args, "--sslcert", ""),
          sslkey: Map.get(args, "--sslkey", ""),
          ipc_path: if(route == "ipc_local", do: required!(args, "--ipc-path"), else: ""),
          front_door_mode:
            if(route == "manager-listener-parser",
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
              route: route,
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

        flag_enabled?(args, "--create-database") ->
          create_started = monotonic_ns()

          case ScratchBird.Connection.attach_create(
                 state.connection,
                 Map.get(args, "--create-emulation-mode", "sbsql"),
                 required!(args, "--database")
               ) do
            {:ok, conn} ->
              state =
                %{state | connection: conn}
                |> bump_api("ScratchBird.Connection.attach_create")
                |> add_timing("database_create", create_started)

              if required!(args, "--parser-mode") != "server-parser" do
                add_failure(
                  state,
                  "parser_mode",
                  "#{required!(args, "--parser-mode")} is not accepted by the Elixir native tool lane; it fails closed"
                )
              else
                run_statements(state, args, paths, expected_refusals)
              end

            {:error, reason, conn} ->
              %{state | connection: conn}
              |> add_failure("database_create", inspect(reason))
          end

        required!(args, "--parser-mode") != "server-parser" ->
          add_failure(
            state,
            "parser_mode",
            "#{required!(args, "--parser-mode")} is not accepted by the Elixir native tool lane; it fails closed"
          )

        true ->
          run_statements(state, args, paths, expected_refusals)
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
    process_metrics = current_process_metrics()

    summary = %{
      run_id: Map.get(args, "--run-id", "manual"),
      driver_name: "elixir",
      route: route,
      parser_mode: required!(args, "--parser-mode"),
      page_size: required!(args, "--page-size"),
      namespace: required!(args, "--namespace"),
      sslmode: sslmode,
      transport_mode: transport_mode_for_route(route, sslmode),
      transport_endpoint_kind: endpoint_kind_for_route(route),
      driver_transport_implementation: transport_implementation_for_route(route),
      cpp_library_boundary: "none",
      language_resource_pack:
        Map.get(
          args,
          "--language-resource-pack",
          "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"
        ),
      language_resource_identity:
        Map.get(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
      language_resource_hash:
        Map.get(
          args,
          "--language-resource-hash",
          "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"
        ),
      language_resource_authority: "shared_server_parser_resource_pack",
      language_profile: Map.get(args, "--language-profile", "en-US"),
      syntax_profile: Map.get(args, "--syntax-profile", "sbsql.v3"),
      topology_profile: Map.get(args, "--topology-profile", "topology.sbsql.canonical.v1"),
      standard_english_fallback: flag_enabled?(args, "--standard-english-fallback", true),
      status: if(state.failures == [], do: "pass", else: "fail"),
      failure_count: length(state.failures),
      elapsed_ns: elapsed,
      process_metrics: process_metrics,
      server_revalidation_required: true,
      driver_or_parser_finality: "forbidden",
      mga_authority: "engine"
    }

    write_text(required!(args, "--summary"), SBIsqlElixir.Json.encode(summary) <> "\n")
    write_text(required!(args, "--metrics"), SBIsqlElixir.Json.encode(timings) <> "\n")
    write_text(paths.timing, SBIsqlElixir.Json.encode(timings) <> "\n")
    write_text(paths.digests, SBIsqlElixir.Json.encode(Enum.reverse(state.digests)) <> "\n")

    append_jsonl(paths.process, %{
      role: "client",
      rss_kb: process_metrics.client.last_rss_kb,
      vsize_kb: process_metrics.client.last_vsize_kb
    })

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

  defp run_statements(state, args, paths, expected_refusals) do
    required!(args, "--input")
    |> read_input()
    |> split_statements()
    |> Enum.with_index(1)
    |> Enum.reduce_while(state, fn {sql, index}, state ->
      statement_id = "#{Path.basename(required!(args, "--input"))}:#{index}"
      expected_refusal = MapSet.member?(expected_refusals, statement_id)
      expected_outcome = if expected_refusal, do: "refusal", else: "success"
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

            if expected_refusal do
              state =
                add_failure(state, statement_id, "statement succeeded but was expected to refuse")

              {state, "unexpected_success", length(rows), digest, nil,
               "statement succeeded but was expected to refuse"}
            else
              {state, "success", length(rows), digest, nil, nil}
            end

          {:error, reason, state} ->
            message = inspect(reason)

            append_jsonl(required!(args, "--diagnostics"), %{
              statement_id: statement_id,
              sqlstate: "HY000",
              message: message
            })

            append_text(required!(args, "--error"), "#{statement_id}: #{message}\n")

            state =
              if expected_refusal do
                %{
                  state
                  | refusals: [
                      %{statement_id: statement_id, sqlstate: "HY000", diagnostic_code: message}
                      | state.refusals
                    ]
                }
              else
                add_failure(state, statement_id, message)
              end

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
        expected_outcome: expected_outcome,
        actual_outcome: outcome,
        sqlstate: sqlstate,
        diagnostic_code: diagnostic,
        canonical_message_vector: [],
        row_count: row_count,
        result_digest: result_digest,
        elapsed_ns: elapsed,
        server_revalidation_state: "required",
        language_profile: Map.get(args, "--language-profile", "en-US"),
        language_resource_pack:
          Map.get(
            args,
            "--language-resource-pack",
            "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"
          ),
        language_resource_identity:
          Map.get(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
        language_resource_hash:
          Map.get(
            args,
            "--language-resource-hash",
            "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"
          ),
        syntax_profile: Map.get(args, "--syntax-profile", "sbsql.v3"),
        topology_profile: Map.get(args, "--topology-profile", "topology.sbsql.canonical.v1"),
        standard_english_fallback: flag_enabled?(args, "--standard-english-fallback", true),
        transaction_id_observed: nil,
        mga_authority: "engine",
        native_api_surface: "elixir",
        code_example_section: "connection_query_fetch"
      }

      append_jsonl(paths.events, event)
      state = %{state | testcases: [event | state.testcases]}

      if not expected_refusal and flag_enabled?(args, "--stop-on-error") and state.failures != [] do
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

  defp execute_statement(state, sql, "copy") do
    if copy_stdin_statement?(sql) do
      payload = copy_payload_for_statement(sql)

      if payload == "" do
        {:error, "COPY FROM STDIN requires SB_COPY_INPUT rows in the script", state}
      else
        case execute_copy_in(state.connection, executable_sql_without_copy_markers(sql), payload) do
          {:ok, rows_copied, conn} ->
            {:ok, %{rows: [%{copy_in: rows_copied}]},
             %{state | connection: conn} |> bump_api("ScratchBird.Connection.copy_in")}

          {:error, reason, conn} ->
            {:error, reason, %{state | connection: conn}}
        end
      end
    else
      execute_statement(state, sql, "query")
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

  defp parse_args(["--stop-on-error", value | rest], args) do
    if String.starts_with?(value, "--") do
      parse_args([value | rest], Map.put(args, "--stop-on-error", "true"))
    else
      parse_args(
        rest,
        Map.put(args, "--stop-on-error", parse_bool_value!("--stop-on-error", value))
      )
    end
  end

  defp parse_args(["--stop-on-error"], args),
    do: parse_args([], Map.put(args, "--stop-on-error", "true"))

  defp parse_args(["--create-database", value | rest], args) do
    if String.starts_with?(value, "--") do
      parse_args([value | rest], Map.put(args, "--create-database", "true"))
    else
      parse_args(
        rest,
        Map.put(args, "--create-database", parse_bool_value!("--create-database", value))
      )
    end
  end

  defp parse_args(["--create-database"], args),
    do: parse_args([], Map.put(args, "--create-database", "true"))

  defp parse_args(["--standard-english-fallback", value | rest], args) do
    if String.starts_with?(value, "--") do
      parse_args([value | rest], Map.put(args, "--standard-english-fallback", "true"))
    else
      parse_args(
        rest,
        Map.put(
          args,
          "--standard-english-fallback",
          parse_bool_value!("--standard-english-fallback", value)
        )
      )
    end
  end

  defp parse_args(["--standard-english-fallback"], args),
    do: parse_args([], Map.put(args, "--standard-english-fallback", "true"))

  defp parse_args([key, value | rest], args) do
    cond do
      not String.starts_with?(key, "--") ->
        raise("unexpected positional argument: #{key}")

      key not in @supported_args ->
        raise("unsupported argument: #{key}")

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

    route = required!(args, "--route")

    unless route in @routes,
      do: raise("unsupported route: #{route}")

    if route == "ipc_local" and String.trim(Map.get(args, "--ipc-path", "")) == "",
      do: raise("ipc_local route requires --ipc-path")

    unless required!(args, "--parser-mode") in @parser_modes,
      do: raise("unsupported parser mode: #{required!(args, "--parser-mode")}")

    sslmode = Map.get(args, "--sslmode", "require")
    unless sslmode in @ssl_modes, do: raise("unsupported sslmode: #{sslmode}")
  end

  # Split SQL into top-level statements on the active terminator.
  #
  # Quote-aware (single/double quotes) and `--` line-comment aware. Honors the
  # `SET TERM <terminator>` client directive:
  # the directive changes the active terminator and is consumed -- it is not
  # emitted as a statement and is not counted in statement indexing. This lets
  # procedural bodies contain inner `;` between `SET TERM ^` and the restoring
  # `SET TERM ;^`.
  #
  # With no `SET TERM` directive present, the behavior is identical to a plain
  # quote-aware top-level `;` split (backward compatible). Shares the
  # cross-driver oracle at
  # tests/conformance/drivers/chunker_conformance/cases.json.
  defp split_statements(script) do
    scan(script, ";", [], [], false, false)
  end

  # scan(remaining, term, buf_chars_reversed, statements_reversed, in_single, in_double)
  defp scan(<<>>, _term, buf, acc, _in_single, _in_double) do
    acc
    |> flush(buf)
    |> elem(0)
    |> Enum.reverse()
  end

  # `--` line comment outside any quote: copy verbatim to end of line (or input)
  # without scanning for the terminator or quotes inside it.
  defp scan(<<"--", _::binary>> = rest, term, buf, acc, false, false) do
    {comment, after_comment} = take_line(rest, [])
    scan(after_comment, term, prepend(buf, comment), acc, false, false)
  end

  # Single quote toggles in_single (only when not in_double).
  defp scan(<<"'", rest::binary>>, term, buf, acc, in_single, false) do
    scan(rest, term, [?' | buf], acc, not in_single, false)
  end

  # Double quote toggles in_double (only when not in_single).
  defp scan(<<"\"", rest::binary>>, term, buf, acc, false, in_double) do
    scan(rest, term, [?" | buf], acc, false, not in_double)
  end

  defp scan(rest, term, buf, acc, false = in_single, false = in_double) do
    if term != "" and String.starts_with?(rest, term) do
      # Capture the matched length BEFORE flush, which may change the active term.
      matched_len = byte_size(term)
      {acc, term} = flush(acc, buf, term)
      <<_::binary-size(matched_len), tail::binary>> = rest
      scan(tail, term, [], acc, false, false)
    else
      <<ch::utf8, tail::binary>> = rest
      scan(tail, term, [<<ch::utf8>> | buf], acc, in_single, in_double)
    end
  end

  defp scan(rest, term, buf, acc, in_single, in_double) do
    <<ch::utf8, tail::binary>> = rest
    scan(tail, term, [<<ch::utf8>> | buf], acc, in_single, in_double)
  end

  defp take_line(<<>>, acc), do: {IO.iodata_to_binary(Enum.reverse(acc)), <<>>}

  defp take_line(<<"\n", _::binary>> = rest, acc),
    do: {IO.iodata_to_binary(Enum.reverse(acc)), rest}

  defp take_line(<<ch::utf8, rest::binary>>, acc),
    do: take_line(rest, [<<ch::utf8>> | acc])

  defp prepend(buf, chunk), do: [chunk | buf]

  # Final-buffer flush keeps the active terminator implicit (returns statements only).
  defp flush(acc, buf), do: flush(acc, buf, ";")

  defp flush(acc, buf, term) do
    chunk = buf |> Enum.reverse() |> IO.iodata_to_binary() |> String.trim()

    cond do
      chunk == "" ->
        {acc, term}

      new_term = chunk_set_term(chunk) ->
        {acc, new_term}

      true ->
        {[chunk | acc], term}
    end
  end

  # Returns the new terminator if `chunk` is a `SET TERM <terminator>` client
  # directive, else nil. Leading full-line `--` comments and blank lines are
  # ignored when matching, so a directive may be preceded by comment lines.
  defp chunk_set_term(chunk) do
    meaningful =
      chunk
      |> String.split("\n")
      |> Enum.map(&String.trim/1)
      |> Enum.reject(fn line -> line == "" or String.starts_with?(line, "--") end)

    case meaningful do
      [] ->
        nil

      lines ->
        joined = Enum.join(lines, " ")

        case Regex.run(~r/^set\s+term\s+(\S.*?)\s*$/i, joined) do
          [_, rest] -> String.trim(rest)
          _ -> nil
        end
    end
  end

  defp classify(sql) do
    trimmed = sql |> executable_sql_without_copy_markers() |> String.trim() |> String.downcase()
    first = trimmed |> String.split(~r/\s+/, parts: 2) |> hd()

    cond do
      first == "copy" -> "copy"
      first in ["create", "alter", "drop"] -> "ddl"
      first in ["insert", "update", "delete", "merge", "upsert"] -> "dml"
      first in ["commit", "rollback", "savepoint", "begin", "start"] -> "transaction"
      first in ["grant", "revoke"] -> "security_refusal"
      String.contains?(trimmed, "sys.") -> "metadata"
      true -> "query"
    end
  end

  defp executable_sql_without_copy_markers(sql) do
    sql
    |> String.split(~r/\r\n|\r|\n/)
    |> Enum.reject(fn line -> String.starts_with?(String.trim_leading(line), "-- SB_COPY_INPUT ") end)
    |> Enum.join("\n")
    |> String.trim()
  end

  defp copy_payload_for_statement(sql) do
    rows =
      sql
      |> String.split(~r/\r\n|\r|\n/)
      |> Enum.flat_map(fn line ->
        stripped = String.trim_leading(line)

        if String.starts_with?(stripped, "-- SB_COPY_INPUT ") do
          [String.replace_prefix(stripped, "-- SB_COPY_INPUT ", "")]
        else
          []
        end
      end)

    if rows == [], do: "", else: Enum.join(rows, "\n") <> "\n"
  end

  defp copy_stdin_statement?(sql) do
    executable =
      sql
      |> executable_sql_without_copy_markers()
      |> String.split(~r/\r\n|\r|\n/)
      |> Enum.map(&(String.trim(&1) |> String.downcase()))
      |> Enum.reject(fn line -> line == "" or String.starts_with?(line, "--") end)
      |> Enum.join(" ")

    String.starts_with?(executable, "copy ") and String.contains?(executable, " from stdin")
  end

  defp execute_copy_in(conn, sql, payload) do
    conn =
      copy_send_message(
        conn,
        ScratchBird.Protocol.message_type(:query),
        ScratchBird.Protocol.build_query_payload(sql, 0, 0, 0)
      )

    copy_loop(conn, false, 0, payload)
  end

  defp copy_loop(conn, copy_started, rows_copied, payload) do
    case copy_recv_message(conn) do
      {:ok, msg} ->
        case copy_handle_async(conn, msg) do
          {:handled, new_conn} ->
            copy_loop(new_conn, copy_started, rows_copied, payload)

          {:ok, new_conn} ->
            cond do
              msg.type == ScratchBird.Protocol.message_type(:copy_in_response) ->
                new_conn =
                  new_conn
                  |> copy_send_message(ScratchBird.Protocol.message_type(:copy_data), payload)
                  |> copy_send_message(ScratchBird.Protocol.message_type(:copy_done), <<>>)

                copy_loop(new_conn, true, rows_copied, payload)

              msg.type == ScratchBird.Protocol.message_type(:command_complete) ->
                complete = ScratchBird.Protocol.parse_command_complete(msg.payload)
                copy_loop(new_conn, copy_started, complete.rows, payload)

              msg.type == ScratchBird.Protocol.message_type(:ready) ->
                {:ok, status, txn_id} = ScratchBird.Protocol.parse_ready(msg.payload)
                new_conn = %{new_conn | txn_id: txn_id, runtime_txn_active: status != 0}

                if copy_started do
                  {:ok, rows_copied, new_conn}
                else
                  {:error, "COPY FROM STDIN did not enter COPY input mode", new_conn}
                end

              msg.type == ScratchBird.Protocol.message_type(:error) ->
                {:error, copy_error_message(msg.payload), new_conn}

              true ->
                copy_loop(new_conn, copy_started, rows_copied, payload)
            end
        end

      {:error, reason} ->
        {:error, inspect(reason), conn}
    end
  end

  defp copy_send_message(conn, type, payload) do
    header = %{
      type: type,
      flags: 0,
      length: byte_size(payload),
      sequence: conn.sequence,
      attachment_id: conn.attachment_id,
      txn_id: conn.txn_id
    }

    data = ScratchBird.Protocol.encode_message(header, payload)

    case conn.transport do
      :ssl -> :ssl.send(conn.socket, data)
      :tcp -> :gen_tcp.send(conn.socket, data)
      :ipc -> :gen_tcp.send(conn.socket, data)
    end

    %{conn | sequence: conn.sequence + 1}
  end

  defp copy_recv_message(conn) do
    with {:ok, header_bin} <- copy_recv_exact(conn, ScratchBird.Protocol.header_size()),
         {:ok, header} <- ScratchBird.Protocol.decode_header(header_bin),
         {:ok, payload} <- copy_recv_exact(conn, header.length) do
      {:ok, Map.put(header, :payload, payload)}
    end
  end

  defp copy_recv_exact(_conn, 0), do: {:ok, <<>>}

  defp copy_recv_exact(conn, bytes) do
    case conn.transport do
      :ssl -> :ssl.recv(conn.socket, bytes)
      :tcp -> :gen_tcp.recv(conn.socket, bytes)
      :ipc -> :gen_tcp.recv(conn.socket, bytes)
    end
  end

  defp copy_handle_async(conn, msg) do
    cond do
      msg.type == ScratchBird.Protocol.message_type(:parameter_status) ->
        {:ok, name, value} = ScratchBird.Protocol.parse_parameter_status(msg.payload)
        params = Map.put(conn.params, name, value)
        {:handled, %{conn | params: params}}

      msg.type == ScratchBird.Protocol.message_type(:txn_status) ->
        {:ok, status, txn_id} = ScratchBird.Protocol.parse_txn_status(msg.payload)
        {:handled, %{conn | txn_id: txn_id, runtime_txn_active: status == ?T}}

      msg.type in [
        ScratchBird.Protocol.message_type(:notification),
        ScratchBird.Protocol.message_type(:query_plan),
        ScratchBird.Protocol.message_type(:sblr_compiled)
      ] ->
        {:handled, conn}

      true ->
        {:ok, conn}
    end
  end

  defp copy_error_message(payload) do
    payload
    |> ScratchBird.Protocol.parse_error()
    |> Map.get(:message, "COPY failed")
  end

  defp transport_mode_for_route("embedded", _sslmode), do: "embedded_no_network_transport"
  defp transport_mode_for_route("ipc_local", _sslmode), do: "local_ipc_no_tls"
  defp transport_mode_for_route(_route, "disable"), do: "tls_disabled"
  defp transport_mode_for_route(_route, _sslmode), do: "tls_required"

  defp effective_sslmode("ipc_local", _sslmode), do: "disable"
  defp effective_sslmode(_route, sslmode), do: sslmode

  defp endpoint_kind_for_route("embedded"), do: "none"
  defp endpoint_kind_for_route("ipc_local"), do: "unix_domain_socket"
  defp endpoint_kind_for_route(_route), do: "tcp"

  defp transport_implementation_for_route("embedded"),
    do: "unsupported_no_cpp_library_boundary"

  defp transport_implementation_for_route("ipc_local"),
    do: "native_elixir_unix_socket"

  defp transport_implementation_for_route(_route), do: "native_elixir_tcp"

  defp parse_bool_value!(key, value) do
    case String.downcase(value) do
      "true" -> "true"
      "false" -> "false"
      _ -> raise("#{key} expects true or false, got: #{value}")
    end
  end

  defp flag_enabled?(args, key, default \\ false),
    do: Map.get(args, key, if(default, do: "true", else: "false")) == "true"

  defp load_expected_refusals(""), do: MapSet.new()
  defp load_expected_refusals(nil), do: MapSet.new()

  defp load_expected_refusals(path) do
    unless File.regular?(path), do: raise("expected refusal file not found: #{path}")
    text = File.read!(path)

    bodies =
      if String.starts_with?(String.trim_leading(text), "[") do
        [text]
      else
        Regex.scan(~r/"(?:statement_ids|expected_refusals)"\s*:\s*\[(.*?)\]/s, text,
          capture: :all_but_first
        )
        |> Enum.map(fn [body] -> body end)
        |> Kernel.++(
          Regex.scan(~r/"expected_diagnostics"\s*:\s*\{(.*?)\n\s*\}/s, text,
            capture: :all_but_first
          )
          |> Enum.map(fn [body] -> body end)
        )
      end

    bodies
    |> Enum.flat_map(&Regex.scan(~r/"((?:\\.|[^"\\])*)"/, &1, capture: :all_but_first))
    |> Enum.map(fn [id] -> String.replace(id, "\\\"", "\"") end)
    |> MapSet.new()
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

  defp current_process_metrics do
    kb = max(1, div(:erlang.memory(:total), 1024))

    %{
      client: %{
        last_rss_kb: kb,
        last_vsize_kb: kb,
        max_rss_kb: kb,
        max_vsize_kb: kb
      }
    }
  end

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
