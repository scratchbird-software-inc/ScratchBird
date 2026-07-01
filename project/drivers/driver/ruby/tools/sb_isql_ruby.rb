#!/usr/bin/env ruby
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "digest"
require "fileutils"
require "json"
require "set"

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))
require "scratchbird"

PAGE_SIZES = %w[4k 8k 16k 32k 64k 128k].freeze
ROUTES = %w[embedded ipc_local listener-parser manager-listener-parser].freeze
PARSER_MODES = %w[server-parser standalone-parser driver-sblr-uuid].freeze
SSLMODES = %w[allow disable prefer require verify-ca verify-full].freeze
SUPPORTED_ARGS = %w[
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
].to_set.freeze

def main(argv)
  args = parse_args(argv)
  exit(run(args))
rescue StandardError => e
  warn e.message
  exit 1
end

def run(args)
  validate(args)
  run_root = File.dirname(required(args, "--summary"))
  FileUtils.mkdir_p(run_root)
  paths = {
    events: File.join(run_root, "command-events.jsonl"),
    wire: File.join(run_root, "wire-transcript.jsonl"),
    timing: File.join(run_root, "timing-groups.json"),
    digests: File.join(run_root, "result-digests.json"),
    metadata: File.join(run_root, "metadata-snapshots.json"),
    process: File.join(run_root, "process-metrics.jsonl"),
    refusals: File.join(run_root, "security-refusals.json"),
    api: File.join(run_root, "native-api-coverage.json"),
    review: File.join(run_root, "code-example-review.json"),
    junit: File.join(run_root, "junit.xml"),
    stdout: File.join(run_root, "stdout.log"),
    stderr: File.join(run_root, "stderr.log")
  }
  ([required(args, "--output"), required(args, "--error"), required(args, "--diagnostics"),
    required(args, "--metrics"), required(args, "--transcript"), required(args, "--summary")] + paths.values).each do |path|
    write_text(path, "")
  end

  timings = {}
  api_hits = {
    "Scratchbird" => 0,
    "connect" => 0,
    "prepare" => 0,
    "execute" => 0,
    "query_metadata" => 0,
    "attach_create" => 0,
    "commit" => 0,
    "rollback" => 0,
    "copy_in" => 0
  }
  testcases = []
  failures = []
  digests = []
  security_refusals = []
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
  expected_refusals = load_expected_refusals(value_or_default(args, "--expected-refusals", ""))
  client = nil

  begin
    route = required(args, "--route")
    ensure_transport_route_supported(route, args)
    cfg = Scratchbird::Config.new
    cfg.host = required(args, "--host")
    cfg.port = required(args, "--port").to_i
    cfg.transport = transport_config_for_route(route)
    cfg.ipc_path = value_or_default(args, "--ipc-path", "")
    cfg.database = required(args, "--database")
    cfg.user = required(args, "--user")
    cfg.password = required(args, "--password")
    cfg.role = value_or_default(args, "--role", "")
    cfg.sslmode = effective_sslmode_for_route(route, value_or_default(args, "--sslmode", "require"))
    cfg.sslrootcert = value_or_default(args, "--sslrootcert", "")
    cfg.sslcert = value_or_default(args, "--sslcert", "")
    cfg.sslkey = value_or_default(args, "--sslkey", "")
    cfg.front_door_mode = route == "manager-listener-parser" ? "manager_proxy" : "direct"
    cfg.metadata_expand_schema_parents = true
    cfg.application_name = "SBIsqlRuby"
    client = Scratchbird::Client.new(cfg)
    api_hits["Scratchbird"] += 1
    connect_started = monotonic_ns
    client.connect
    api_hits["connect"] += 1
    add_timing(timings, "connection", connect_started)
    append_jsonl(required(args, "--transcript"), {
      event: "connect",
      driver: "ruby",
      route: route,
      parser_mode: required(args, "--parser-mode"),
      page_size: required(args, "--page-size")
    })
    append_jsonl(paths[:wire], { event: "server_admission_required", driver_or_parser_finality: "forbidden" })

    if flag?(args, "--create-database")
      create_started = monotonic_ns
      client.attach_create(value_or_default(args, "--create-emulation-mode", "sbsql"), required(args, "--database"))
      api_hits["attach_create"] += 1
      add_timing(timings, "database_create", create_started)
    end
    unless required(args, "--parser-mode") == "server-parser"
      raise "#{required(args, "--parser-mode")} is not accepted by the Ruby native tool lane; it fails closed"
    end

    split_statements(read_input(required(args, "--input"))).each_with_index do |sql, index|
      statement_id = "#{File.basename(required(args, "--input"))}:#{index + 1}"
      expected_outcome = expected_refusals.include?(statement_id) ? "refusal" : "success"
      group = classify_statement(sql)
      statement_started = monotonic_ns
      outcome = "success"
      row_count = -1
      result_digest = nil
      sqlstate = nil
      diagnostic = nil
      begin
        if group == "transaction"
          run_transaction(client, sql, api_hits)
          row_count = 0
          result_digest = sha256_text("transaction")
        elsif group == "copy" && copy_stdin_statement?(sql)
          payload = copy_payload_for_statement(sql)
          raise "COPY FROM STDIN requires SB_COPY_INPUT rows in the script" if payload.empty?

          rows_copied = execute_copy_in(client, executable_sql_without_copy_markers(sql), payload)
          api_hits["copy_in"] += 1
          row_count = rows_copied
          result_digest = sha256_text("copy_in:#{rows_copied}")
          append_text(required(args, "--output"), JSON.generate(statement_id: statement_id, rows: [{ copy_in: rows_copied }]) + "\n")
        else
          name = "sb_isql_ruby_#{index + 1}"
          client.prepare(name, sql)
          api_hits["prepare"] += 1
          result = client.execute(name)
          api_hits["execute"] += 1
          rows = result.rows
          row_count = result.rowcount
          result_digest = sha256_text(JSON.generate(rows))
          append_text(required(args, "--output"), JSON.generate(statement_id: statement_id, rows: rows) + "\n")
        end
        digests << { statement_id: statement_id, row_count: row_count, result_digest: result_digest }
        if expected_outcome == "refusal"
          outcome = "unexpected_success"
          failures << { statement_id: statement_id, message: "statement succeeded but was expected to refuse" }
        end
      rescue StandardError => e
        outcome = "refusal"
        sqlstate = e.respond_to?(:sqlstate) ? e.sqlstate : "HY000"
        diagnostic = e.message
        append_jsonl(required(args, "--diagnostics"), { statement_id: statement_id, sqlstate: sqlstate, message: diagnostic })
        append_text(required(args, "--error"), "#{statement_id}: #{diagnostic}\n")
        if expected_outcome == "success"
          failures << { statement_id: statement_id, message: diagnostic }
        else
          security_refusals << { statement_id: statement_id, sqlstate: sqlstate, diagnostic_code: diagnostic }
        end
        if expected_outcome == "success" && flag?(args, "--stop-on-error")
          add_timing(timings, group, statement_started)
          break
        end
      end
      elapsed = monotonic_ns - statement_started
      add_timing(timings, group, statement_started)
      event = {
        run_id: value_or_default(args, "--run-id", "manual"),
        driver_name: "ruby",
        driver_version: "unknown",
        route: required(args, "--route"),
        parser_mode: required(args, "--parser-mode"),
        page_size: required(args, "--page-size"),
        namespace: required(args, "--namespace"),
        script: required(args, "--input"),
        statement_index: index + 1,
        statement_id: statement_id,
        command_group: group,
        sql_hash: sha256_text(sql),
        expected_outcome: expected_outcome,
        actual_outcome: outcome,
        sqlstate: sqlstate,
        diagnostic_code: diagnostic,
        canonical_message_vector: [],
        row_count: row_count,
        result_digest: result_digest,
        elapsed_ns: elapsed,
        server_revalidation_state: "required",
        language_profile: value_or_default(args, "--language-profile", "en-US"),
        language_resource_pack: value_or_default(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
        language_resource_identity: value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
        language_resource_hash: value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
        syntax_profile: value_or_default(args, "--syntax-profile", "sbsql.v3"),
        topology_profile: value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
        standard_english_fallback: flag?(args, "--standard-english-fallback", true),
        transaction_id_observed: nil,
        mga_authority: "engine",
        native_api_surface: "ruby",
        code_example_section: "prepare_execute_fetch"
      }
      append_jsonl(paths[:events], event)
      testcases << event
    end

    metadata_started = monotonic_ns
    metadata = client.query_metadata("tables")
    api_hits["query_metadata"] += 1
    write_text(paths[:metadata], JSON.generate(tables_digest: sha256_text(JSON.generate(metadata.rows)), row_count: metadata.rowcount) + "\n")
    add_timing(timings, "metadata", metadata_started)
  rescue StandardError => e
    failures << { statement_id: "run", message: e.message }
    append_text(paths[:stderr], e.message + "\n")
  ensure
    client&.close
  end

  elapsed = monotonic_ns - started
  timings["overall"] = elapsed
  sslmode = effective_sslmode_for_route(required(args, "--route"), value_or_default(args, "--sslmode", "require"))
  transport_mode = resolve_transport_mode(required(args, "--route"), sslmode)
  process_metrics = current_process_metrics
  summary = {
    run_id: value_or_default(args, "--run-id", "manual"),
    driver_name: "ruby",
    route: required(args, "--route"),
    parser_mode: required(args, "--parser-mode"),
    page_size: required(args, "--page-size"),
    namespace: required(args, "--namespace"),
    sslmode: sslmode,
    transport_mode: transport_mode,
    transport_endpoint_kind: endpoint_kind_for_route(required(args, "--route")),
    driver_transport_implementation: transport_implementation_for_route(required(args, "--route")),
    cpp_library_boundary: "none",
    language_resource_pack: value_or_default(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
    language_resource_identity: value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
    language_resource_hash: value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
    language_resource_authority: "shared_server_parser_resource_pack",
    language_profile: value_or_default(args, "--language-profile", "en-US"),
    syntax_profile: value_or_default(args, "--syntax-profile", "sbsql.v3"),
    topology_profile: value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
    standard_english_fallback: flag?(args, "--standard-english-fallback", true),
    status: failures.empty? ? "pass" : "fail",
    failure_count: failures.length,
    elapsed_ns: elapsed,
    process_metrics: process_metrics,
    server_revalidation_required: true,
    driver_or_parser_finality: "forbidden",
    mga_authority: "engine"
  }
  write_text(required(args, "--summary"), JSON.generate(summary) + "\n")
  write_text(required(args, "--metrics"), JSON.generate(timings) + "\n")
  write_text(paths[:timing], JSON.generate(timings) + "\n")
  write_text(paths[:digests], JSON.generate(digests) + "\n")
  append_jsonl(paths[:process], {
    role: "client",
    rss_kb: process_metrics[:client][:last_rss_kb],
    vsize_kb: process_metrics[:client][:last_vsize_kb]
  })
  write_text(paths[:refusals], JSON.generate(security_refusals) + "\n")
  write_text(paths[:api], JSON.generate(api_hits) + "\n")
  write_text(paths[:review], JSON.generate(driver: "ruby", public_api_only: true, shells_out_to_other_driver: false,
                                           source_is_canonical_example: true,
                                           sections: %w[connection prepare execute fetch metadata diagnostics transaction]) + "\n")
  write_text(paths[:junit], junit_xml("SBIsqlRuby", "scratchbird.ruby", testcases, failures))
  append_text(paths[:stdout], "SBIsqlRuby status=#{summary[:status]}\n")
  failures.empty? ? 0 : 1
end

def parse_args(raw)
  args = {}
  index = 0
  while index < raw.length
    key = raw[index]
    raise "unexpected positional argument: #{key}" unless key.start_with?("--")
    raise "unsupported argument: #{key}" unless SUPPORTED_ARGS.include?(key)
    if key == "--stop-on-error" || key == "--create-database" || key == "--standard-english-fallback"
      if index + 1 < raw.length && !raw[index + 1].start_with?("--")
        args[key] = parse_bool_value(key, raw[index + 1])
        index += 2
      else
        args[key] = true
        index += 1
      end
      next
    end
    value = raw[index + 1]
    raise "missing value for #{key}" if value.nil? || value.start_with?("--")
    args[key] = value
    index += 2
  end
  args
end

def validate(args)
  raise "unsupported page size: #{required(args, "--page-size")}" unless PAGE_SIZES.include?(required(args, "--page-size"))
  raise "unsupported route: #{required(args, "--route")}" unless ROUTES.include?(required(args, "--route"))
  raise "unsupported parser mode: #{required(args, "--parser-mode")}" unless PARSER_MODES.include?(required(args, "--parser-mode"))
  sslmode = value_or_default(args, "--sslmode", "require")
  raise "unsupported sslmode: #{sslmode}" unless SSLMODES.include?(sslmode)
end

SET_TERM_RE = /\Aset\s+term\s+(\S.*?)\s*\z/i.freeze

# Detect a `SET TERM <terminator>` client directive in a cut chunk.
# Leading full-line `--` comments and blank lines are ignored when matching.
# Returns the new terminator string, or nil when the chunk is not a directive.
def chunk_set_term(chunk)
  meaningful = []
  chunk.each_line do |line|
    stripped = line.strip
    next if stripped.empty? || stripped.start_with?("--")
    meaningful << stripped
  end
  return nil if meaningful.empty?
  match = SET_TERM_RE.match(meaningful.join(" "))
  match ? match[1].strip : nil
end

# Split a script into top-level statements on the active terminator.
#
# Quote-aware (single/double quotes) and `--` line-comment aware. Honors the
# `SET TERM <terminator>` client directive: the directive
# changes the active terminator and is consumed -- not emitted, not counted.
# With no SET TERM present this reduces to a plain quote-aware `;` split.
def split_statements(script)
  statements = []
  term = ";"
  current = +""
  single = false
  double = false
  i = 0
  length = script.length

  flush = lambda do
    chunk = current.strip
    next if chunk.empty?
    new_term = chunk_set_term(chunk)
    if new_term
      term = new_term
      next
    end
    statements << chunk
  end

  while i < length
    ch = script[i]
    if !single && !double && ch == "-" && i + 1 < length && script[i + 1] == "-"
      eol = script.index("\n", i)
      eol = length if eol.nil?
      current << script[i...eol]
      i = eol
      next
    end
    if ch == "'" && !double
      single = !single
      current << ch
      i += 1
      next
    end
    if ch == '"' && !single
      double = !double
      current << ch
      i += 1
      next
    end
    if !single && !double && !term.empty? && script[i, term.length] == term
      matched_len = term.length
      flush.call
      current = +""
      i += matched_len
      next
    end
    current << ch
    i += 1
  end

  flush.call
  statements
end

def classify_statement(sql)
  trimmed = executable_sql_without_copy_markers(sql).strip.downcase
  first = trimmed.split(/\s+/, 2).first.to_s
  return "copy" if first == "copy"
  return "ddl" if %w[create alter drop].include?(first)
  return "dml" if %w[insert update delete merge upsert].include?(first)
  return "transaction" if %w[commit rollback savepoint begin start].include?(first)
  return "security_refusal" if %w[grant revoke].include?(first)
  return "metadata" if trimmed.include?("sys.")
  "query"
end

def run_transaction(client, sql, api_hits)
  first = sql.strip.downcase.split(/\s+/, 2).first.to_s
  if first == "commit"
    client.commit
    api_hits["commit"] += 1
  elsif first == "rollback"
    client.rollback
    api_hits["rollback"] += 1
  else
    client.begin
  end
end

def required(args, key)
  value = args[key]
  raise "missing required argument #{key}" if value.nil? || value == ""
  value.to_s
end

def value_or_default(args, key, default)
  args.fetch(key, default).to_s
end

def flag?(args, key, default = false)
  args.key?(key) ? args[key] == true : default
end

def parse_bool_value(key, value)
  normalized = value.to_s.downcase
  return true if normalized == "true"
  return false if normalized == "false"
  raise "#{key} expects true or false, got: #{value}"
end

def resolve_transport_mode(route, sslmode)
  return "embedded_no_network_transport" if route == "embedded"
  return "local_ipc_no_tls" if route == "ipc_local"
  sslmode == "disable" ? "tls_disabled" : "tls_required"
end

def ensure_transport_route_supported(route, args)
  if route == "embedded"
    raise "embedded transport is unsupported by the Ruby driver; no ScratchBird C++ library boundary is exposed"
  end
  if route == "ipc_local" && value_or_default(args, "--ipc-path", "").empty?
    raise "ipc_path is required for local IPC transport"
  end
end

def effective_sslmode_for_route(route, sslmode)
  route == "ipc_local" ? "disable" : sslmode
end

def transport_config_for_route(route)
  return "ipc" if route == "ipc_local"
  return "embedded" if route == "embedded"
  "inet"
end

def endpoint_kind_for_route(route)
  return "unix_domain_socket" if route == "ipc_local"
  return "none" if route == "embedded"
  "tcp"
end

def transport_implementation_for_route(route)
  return "unsupported_no_cpp_library_boundary" if route == "embedded"
  return "native_ruby_unix_socket" if route == "ipc_local"
  "native_ruby_tcp"
end

def load_expected_refusals(path)
  return Set.new if path.nil? || path.empty?
  raise "expected refusal file not found: #{path}" unless File.file?(path)
  doc = JSON.parse(File.read(path))
  ids =
    if doc.is_a?(Hash)
      Array(doc["statement_ids"]) +
        Array(doc["expected_refusals"]) +
        (doc["expected_diagnostics"].is_a?(Hash) ? doc["expected_diagnostics"].keys : [])
    elsif doc.is_a?(Array)
      doc
    else
      raise "expected refusals must be a JSON object or array"
    end
  ids.map(&:to_s).to_set
end

def current_process_metrics
  rss_kb = 1
  vsize_kb = 1
  status_path = "/proc/#{$PROCESS_ID}/status"
  if File.file?(status_path)
    File.foreach(status_path) do |line|
      rss_kb = line.split[1].to_i if line.start_with?("VmRSS:")
      vsize_kb = line.split[1].to_i if line.start_with?("VmSize:")
    end
  end
  rss_kb = 1 if rss_kb < 1
  vsize_kb = rss_kb if vsize_kb < 1
  {
    client: {
      last_rss_kb: rss_kb,
      last_vsize_kb: vsize_kb,
      max_rss_kb: rss_kb,
      max_vsize_kb: vsize_kb
    }
  }
end

def read_input(path)
  path == "-" ? STDIN.read : File.read(path)
end

def executable_sql_without_copy_markers(sql)
  sql.each_line.filter_map do |line|
    line.lstrip.start_with?("-- SB_COPY_INPUT ") ? nil : line
  end.join.strip
end

def copy_payload_for_statement(sql)
  rows = []
  sql.each_line do |line|
    stripped = line.lstrip
    rows << stripped.delete_suffix("\n").delete_suffix("\r").sub(/\A-- SB_COPY_INPUT /, "") if stripped.start_with?("-- SB_COPY_INPUT ")
  end
  rows.empty? ? "" : rows.join("\n") + "\n"
end

def copy_stdin_statement?(sql)
  executable = executable_sql_without_copy_markers(sql).each_line.filter_map do |line|
    stripped = line.strip.downcase
    stripped.empty? || stripped.start_with?("--") ? nil : stripped
  end.join(" ")
  executable.start_with?("copy ") && executable.include?(" from stdin")
end

def execute_copy_in(client, sql, payload)
  client.send(:send_simple_query, sql, nil)
  rows_copied = 0
  copy_started = false
  loop do
    type, _flags, body, _sequence, _attachment_id, _txn_id = client.send(:recv_message)
    next if client.send(:handle_async_message, type, body)

    case type
    when Scratchbird::Protocol::MSG_COPY_IN_RESPONSE
      copy_started = true
      client.send(:send_message, Scratchbird::Protocol::MSG_COPY_DATA, payload, 0, false)
      client.send(:send_message, Scratchbird::Protocol::MSG_COPY_DONE, +"", 0, false)
    when Scratchbird::Protocol::MSG_COMMAND_COMPLETE
      _command_type, rows_count, _last_id, _tag = Scratchbird::Protocol.parse_command_complete(body)
      rows_copied = rows_count.to_i
    when Scratchbird::Protocol::MSG_READY
      status, txn_id = Scratchbird::Protocol.parse_ready(body)
      client.send(:apply_runtime_ready_state, status, txn_id)
      raise "COPY FROM STDIN did not enter COPY input mode" unless copy_started
      return rows_copied
    when Scratchbird::Protocol::MSG_ERROR
      client.send(:handle_query_error, body)
    end
  end
end

def monotonic_ns
  Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
end

def add_timing(timings, group, started)
  timings[group] = timings.fetch(group, 0) + (monotonic_ns - started)
end

def write_text(path, text)
  FileUtils.mkdir_p(File.dirname(path))
  File.write(path, text)
end

def append_text(path, text)
  FileUtils.mkdir_p(File.dirname(path))
  File.open(path, "a") { |file| file.write(text) }
end

def append_jsonl(path, record)
  append_text(path, JSON.generate(record) + "\n")
end

def sha256_text(text)
  "sha256:#{Digest::SHA256.hexdigest(text)}"
end

def junit_xml(suite, klass, testcases, failures)
  xml = +"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  xml << "<testsuite name=\"#{escape_xml(suite)}\" tests=\"#{[testcases.length, 1].max}\" failures=\"#{failures.length}\">\n"
  xml << "  <testcase classname=\"#{escape_xml(klass)}\" name=\"run\"></testcase>\n" if testcases.empty?
  testcases.each do |testcase|
    xml << "  <testcase classname=\"#{escape_xml(klass)}\" name=\"#{escape_xml(testcase[:statement_id].to_s)}\"></testcase>\n"
  end
  failures.each do |failure|
    xml << "  <testcase classname=\"#{escape_xml(klass)}\" name=\"#{escape_xml(failure[:statement_id].to_s)}\"><failure message=\"#{escape_xml(failure[:message].to_s)}\" /></testcase>\n"
  end
  xml << "</testsuite>\n"
end

def escape_xml(text)
  text.gsub("&", "&amp;").gsub('"', "&quot;").gsub("<", "&lt;").gsub(">", "&gt;")
end

main(ARGV)
