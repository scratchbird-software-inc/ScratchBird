// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_ROUTE_B (thin-slice route B in process; owner Q302 / the in-process route B test, option A, 2026-11-25).
// Bare `SELECT 1` (the typed literal VALUES relation, query.execute 0x1207) on a production-created native single-file database, driven through the
// REAL SBsql pipeline IN PROCESS: SbsqlTestWireSession::RunPipeline (the parser's own submission, literal negotiation, finalization, lowering and the
// production canonical submission) over the embedded client bound to the test's own served engine state and session registry, a real HandleAuthHandoff
// session and transaction (the embedded client's own AuthenticateAndAttach), the real server handlers (acquire, execute) and the engine executor.
// What the multi-process exchange (native_wire_exchange.py) cannot show: WHICH availability rows route B asks (the recorder), the exact refusal code and
// opcode field of each refused shape, and that nothing is written around EVERY statement (byte-identical file, unchanged directory, Next unchanged,
// the transaction still active and owned).
// TEST-ONLY SEAMS used here (each compiled only into test binaries; production libraries carry none, nm-checked by native_route_b_seam_symbols):
//   (i)   ScratchBird::parser::sbsql::EmbeddedEngineClient::BindExternalEngineForTest / LastExecuteRowPacketForTest
//         (guarded by SB_EMBEDDED_CLIENT_EXTERNAL_ENGINE_TEST_SEAM; this target compiles embedded_engine_client.cpp itself);
//   (ii)  scratchbird::parser::sbsql::SbsqlTestWireSessionTestAccess (a friend declared only under SB_SBSQL_TEST_WIRE_SESSION_TEST_ACCESS: no symbol);
//   (iii) the executor-availability recorder (SB_NATIVE_EXECUTOR_RECORDER_TEST_SEAM; this target compiles the registry itself).
#define SB_NATIVE_SELECT_ONE_FIXTURE_ONLY 1
#include "native_select_one_test.cpp"

#include "embedded/embedded_engine_client.hpp"
#include "wire/sbsql_test_wire.hpp"

namespace scratchbird::parser::sbsql {
// The one door the test uses into the wire session's private members (declared a friend only under the test macro).
struct SbsqlTestWireSessionTestAccess {
  [[gnu::noinline]] static SessionContext& Session(SbsqlTestWireSession& wire) { return wire.session_; }  // noinline: the symbol exists, so the nm check is not vacuous
  [[gnu::noinline]] static EmbeddedEngineClient* Client(SbsqlTestWireSession& wire) { return wire.embedded_client_.get(); }
};
}  // namespace scratchbird::parser::sbsql

namespace {
namespace ps2 = scratchbird::parser::sbsql;

const std::string kInt64Column = "column1";

// One pipeline session: a wire session over the embedded client bound to the test's served engine and a (shared) registry.
struct Pipe {
  ps2::ParserConfig config;
  ps2::ParserMetrics metrics;
  ps2::SblrTemplateCache cache;
  std::unique_ptr<ps2::SbsqlTestWireSession> wire;
  ps2::EmbeddedEngineClient* client = nullptr;
  bool ended = false;
  std::uint64_t initial_order = 0;  // the transaction the login began; it must stay the session's, and active, until the session ends
  explicit Pipe(srv::ServerSessionRegistry& registry) {
    config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000003114");
    config.bundle_contract_id = "sbp_sbsql@native-route-b";
    config.build_id = "native-route-b";
    config.embedded_engine_direct = true;
    wire = std::make_unique<ps2::SbsqlTestWireSession>(config, &metrics, &cache);
    client = ps2::SbsqlTestWireSessionTestAccess::Client(*wire);
    Check(client != nullptr, "the wire session built its embedded client");
    client->BindExternalEngineForTest(&served.started.state, &registry);
    srv::NativeLoginThrottle().Clear();
    ps2::AuthCredentialEnvelope credentials;
    credentials.principal = std::string(kBootstrapName);
    credentials.credential_evidence = std::string(kBootstrapPassword);
    credentials.credential_evidence_present = true;
    credentials.requested_language = "en";
    credentials.application_name = "native_route_b_test";
    ps2::MessageVectorSet messages;
    const bool ok = client->AuthenticateAndAttach(credentials, &ps2::SbsqlTestWireSessionTestAccess::Session(*wire), &messages);
    std::string text;
    for (const auto& d : messages.diagnostics) text += d.code + " ";
    Check(ok && ps2::SbsqlTestWireSessionTestAccess::Session(*wire).authenticated, "the embedded client logs in through the real handoff and attach: " + text);
    initial_order = ps2::SbsqlTestWireSessionTestAccess::Session(*wire).local_transaction_id;
  }
  ps2::SessionContext& session() { return ps2::SbsqlTestWireSessionTestAccess::Session(*wire); }
  std::uint64_t order() { return session().local_transaction_id; }
  void End() {
    if (ended) return;
    ended = true;
    ps2::MessageVectorSet messages;
    (void)wire->DisconnectExecutionRoute(&messages);
  }
  ~Pipe() { End(); }
};

struct Ran {
  ps2::PipelineResult result;
  std::vector<api::NativeExecutorRecordedRowForTest> recorded;
  std::string raw_vector;  // the engine's own answer (the embedded client's last execute row packet)
  std::string server_diagnostics;  // the server's own diagnostics of the last execute (code{key=value;...}), before the parser flattens them
};
// record: 0 = no recorder (the production table decides); 1 = the recorder GRANTS every row it is asked (shows what a shape would need);
// 2 = the recorder only LOGS (the production table still decides: a refused shape's log shows the row it was refused on).
Ran RunSql(Pipe& pipe, const std::string& sql, int record) {
  Ran out;
  pipe.client->ResetLastExecuteForTest();
  if (record != 0) api::StartNativeExecutorRecordingForTest(record == 1);
  out.result = pipe.wire->RunPipeline(sql, true);
  if (record != 0) out.recorded = api::StopNativeExecutorRecordingForTest();
  out.raw_vector = pipe.client->LastExecuteRowPacketForTest();
  out.server_diagnostics = pipe.client->LastExecuteServerDiagnosticsForTest();
  return out;
}
std::vector<std::string> KeysOf(const Ran& ran) {
  std::vector<std::string> keys;
  for (const auto& r : ran.recorded) keys.push_back(RowKey(r.row));
  return keys;
}
std::string FieldNamed(const ps2::PipelineResult& result, const std::string& name) {
  for (const auto& d : result.messages.diagnostics)
    for (const auto& f : d.fields) if (f.name == name) return f.value;
  return {};
}
std::string FirstCode(const ps2::PipelineResult& result) {
  for (const auto& d : result.messages.diagnostics) return d.code;
  return {};
}
std::string AllCodes(const ps2::PipelineResult& result) {
  std::string out;
  for (const auto& d : result.messages.diagnostics) {
    out += d.code + "{";
    for (const auto& f : d.fields) out += f.name + "=" + f.value.substr(0, 300) + ";";
    out += "} ";
  }
  return out;
}

// The accepted shape: the parser-built row (name column1, int64, the value) and the engine's own vector (cohort int64 identity).
void CheckAccepted(const Ran& ran, const std::string& sql, std::int64_t value, const CohortInt64& cohort) {
  const auto& r = ran.result;
  Check(r.accepted && !r.messages.has_errors(), "'" + sql + "' is accepted through the real pipeline: " + AllCodes(r));
  Check(r.server_operation_id == "query.execute", "'" + sql + "': the engine operation is query.execute (route B): " + r.server_operation_id);
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> rows;
  Check(ps2::DecodePublicResultRowsForTest(r.server_result_payload, &oids, &rows), "'" + sql + "': the parser's client row decodes");
  Check(oids.size() == 1 && rows.size() == 1 && rows[0].size() == 1 && rows[0][0] && *rows[0][0] == std::to_string(value),
        "'" + sql + "': the parser-built row is one int64 value " + std::to_string(value));
  Check(r.server_result_payload.ends_with(kInt64Column + ":int64:not_null"),
        "'" + sql + "': the parser-built row's column is named `" + kInt64Column + "` (the parser's own rule: column1, by position)");
  Check(r.server_result_payload.find("key_a") == std::string::npos, "'" + sql + "': the fixture name key_a does not reach the client");
  const auto vector = DecodeVector(ran.raw_vector);
  Check(vector.ok && vector.operation_id == "query.execute" && vector.result_kind == "native_scalar_projection_vector" && vector.column_count == 1 &&
            vector.row_count == 1 && vector.values == std::vector<std::int64_t>{value},
        "'" + sql + "': the engine's vector is one int64 value " + std::to_string(value) + ", operation query.execute");
  if (vector.ok) {
    Check(vector.column_datatype[0] == cohort.type_uuid && vector.column_datatype_descriptor[0] == cohort.descriptor_uuid &&
              vector.column_datatype_generation[0] == cohort.generation,
          "'" + sql + "': the vector's datatype and descriptor UUIDs and generation equal the cohort's int64 identity");
  }
  Check(ran.raw_vector.find("column1") == std::string::npos && ran.raw_vector.find("key_a") == std::string::npos,
        "'" + sql + "': the engine's vector carries no column name");
}

// SB_NATIVE_LITERAL_VALUES_TABLE (2026-12-13, wider literals): a literal VALUES TABLE (R rows by C columns of int64) answers with ONE vector in one message
// (row_count R, column_count C, row-major values, the cohort int64 identity per column, no name) and the parser builds the table: columns column1..columnC,
// rows in the order written. `expected` is row-major by row.
void CheckAcceptedTable(const Ran& ran, const std::string& sql, const std::vector<std::vector<std::int64_t>>& expected, const CohortInt64& cohort) {
  const auto& r = ran.result;
  const std::string label = "'" + sql.substr(0, 60) + (sql.size() > 60 ? "..." : "") + "'";
  Check(r.accepted && !r.messages.has_errors(), label + " is accepted through the real pipeline: " + AllCodes(r));
  Check(r.server_operation_id == "query.execute", label + ": the engine operation is query.execute: " + r.server_operation_id);
  const std::size_t rows = expected.size(), columns = expected.front().size();
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> got;
  Check(ps2::DecodePublicResultRowsForTest(r.server_result_payload, &oids, &got), label + ": the parser's client rows decode");
  Check(oids.size() == columns && got.size() == rows, label + ": the parser-built table is " + std::to_string(rows) + " rows by " + std::to_string(columns) + " columns");
  for (std::size_t row = 0; row < rows && row < got.size(); ++row)
    for (std::size_t column = 0; column < columns && column < got[row].size(); ++column)
      Check(got[row][column] && *got[row][column] == std::to_string(expected[row][column]),
            label + ": parser row " + std::to_string(row) + " column " + std::to_string(column) + " is " + std::to_string(expected[row][column]) + " (row order as written)");
  std::string meta;
  for (std::size_t column = 0; column < columns; ++column) meta += (column ? ";" : "") + std::string("column") + std::to_string(column + 1) + ":int64:not_null";
  Check(r.server_result_payload.ends_with(meta), label + ": the parser names the columns column1..column" + std::to_string(columns) + " by position");
  Check(r.server_result_payload.find("key_a") == std::string::npos, label + ": the fixture name key_a does not reach the client");
  const auto vector = DecodeVector(ran.raw_vector);
  std::vector<std::int64_t> flat;
  for (const auto& row : expected) flat.insert(flat.end(), row.begin(), row.end());
  Check(vector.ok && vector.operation_id == "query.execute" && vector.result_kind == "native_scalar_projection_vector" && vector.column_count == columns &&
            vector.row_count == rows && vector.values == flat,
        label + ": the engine's ONE vector has row_count " + std::to_string(rows) + ", column_count " + std::to_string(columns) + " and the values row-major in the order written");
  if (vector.ok) {
    for (std::size_t column = 0; column < columns; ++column)
      Check(vector.column_datatype[column] == cohort.type_uuid && vector.column_datatype_descriptor[column] == cohort.descriptor_uuid &&
                vector.column_datatype_generation[column] == cohort.generation,
            label + ": column " + std::to_string(column) + "'s datatype and descriptor UUIDs and generation equal the cohort's int64 identity");
  }
  Check(ran.raw_vector.find("column1") == std::string::npos && ran.raw_vector.find("key_a") == std::string::npos, label + ": the engine's vector carries no column name");
}
// The VALUES text of R rows by C columns counting 1, 2, 3, ... row-major, and its expected rows.
std::string ValuesText(std::size_t rows, std::size_t columns) {
  std::string out = "VALUES ";
  for (std::size_t row = 0; row < rows; ++row) {
    out += row ? ", (" : "(";
    for (std::size_t column = 0; column < columns; ++column) out += (column ? ", " : "") + std::to_string(row * columns + column + 1);
    out += ")";
  }
  return out;
}
std::vector<std::vector<std::int64_t>> ValuesRows(std::size_t rows, std::size_t columns) {
  std::vector<std::vector<std::int64_t>> out(rows);
  for (std::size_t row = 0; row < rows; ++row)
    for (std::size_t column = 0; column < columns; ++column) out[row].push_back(static_cast<std::int64_t>(row * columns + column + 1));
  return out;
}

// SB_NATIVE_VALUES_BOOLEAN_LITERAL (2026-12-13): the cohort's BOOLEAN identity (datatype.boolean.u8.v1), read from the compiled registry independently of the
// engine's encoder, exactly as CohortInt64Of reads the int64 one.
CohortInt64 CohortBooleanOf(const api::EngineRequestContext& context) {
  CohortInt64 out;
  int found = 0;
  for (const auto& row3 : scratchbird::core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = row3.legacy_fields;
    if (row.codec_id == "datatype.boolean.u8.v1" && row.canonical_name == "boolean" && row.catalog_snapshot_uuid == context.datatype_catalog_snapshot_uuid &&
        row.catalog_generation == context.datatype_catalog_generation && row.registry_generation == context.datatype_registry_generation) {
      out = {row.type_uuid, row.descriptor_uuid, row.descriptor_generation};
      ++found;
    }
  }
  Check(found == 1, "exactly one boolean identity in the receipt's datatype cohort (" + std::to_string(found) + ")");
  return out;
}
// A table whose columns are int64 or boolean ('i' / 'b' per column); `expected` holds the int64 value or 0/1 for a boolean. The engine's ONE vector carries
// each column's own cohort identity and, for a boolean, ONE byte 0 or 1 (bytes kind, never text); the parser renders `true` / `false` and names the column
// `<name>:boolean:not_null`.
void CheckAcceptedMixedTable(const Ran& ran, const std::string& sql, const std::string& kinds, const std::vector<std::vector<std::int64_t>>& expected,
                             const CohortInt64& int64_cohort, const CohortInt64& boolean_cohort) {
  namespace pr = scratchbird::wire::public_result;
  const auto& r = ran.result;
  const std::string label = "'" + sql.substr(0, 60) + "'";
  Check(r.accepted && !r.messages.has_errors(), label + " is accepted through the real pipeline: " + AllCodes(r));
  const std::size_t rows = expected.size(), columns = kinds.size();
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> got;
  Check(ps2::DecodePublicResultRowsForTest(r.server_result_payload, &oids, &got) && got.size() == rows && oids.size() == columns, label + ": the parser-built table has " + std::to_string(rows) + " rows and " + std::to_string(columns) + " columns");
  for (std::size_t row = 0; row < rows && row < got.size(); ++row)
    for (std::size_t column = 0; column < columns && column < got[row].size(); ++column) {
      const std::string want = kinds[column] == 'b' ? (expected[row][column] ? "true" : "false") : std::to_string(expected[row][column]);
      Check(got[row][column] && *got[row][column] == want, label + ": parser row " + std::to_string(row) + " column " + std::to_string(column) + " is " + want + " (order as written)");
    }
  std::string meta;
  for (std::size_t column = 0; column < columns; ++column) meta += (column ? ";" : "") + std::string("column") + std::to_string(column + 1) + (kinds[column] == 'b' ? ":boolean:not_null" : ":int64:not_null");
  Check(r.server_result_payload.ends_with(meta), label + ": the parser's row metadata is " + meta);
  std::vector<pr::Field> fields;
  Check(pr::Decode(ran.raw_vector, &fields), label + ": the engine's vector decodes");
  const auto find = [&](const std::string& name) -> const pr::Field* {
    const pr::Field* found = nullptr;
    for (const auto& f : fields) if (f.name == name) { if (found) return nullptr; found = &f; }
    return found;
  };
  Check(fields.size() == 4 + 4 * columns + columns * rows, label + ": the vector holds exactly the header, four fields per column and one value per cell (no name, no text)");
  const auto* kind_field = find("result_kind");
  const auto* rows_field = find("row_count");
  const auto* columns_field = find("column_count");
  Check(kind_field && kind_field->value == "native_scalar_projection_vector" && rows_field && LE64(rows_field->value) == rows && columns_field && LE64(columns_field->value) == columns,
        label + ": vector result_kind, row_count " + std::to_string(rows) + ", column_count " + std::to_string(columns));
  for (std::size_t column = 0; column < columns; ++column) {
    const auto& cohort = kinds[column] == 'b' ? boolean_cohort : int64_cohort;
    const auto n = std::to_string(column);
    const auto* t = find("column_datatype[" + n + "]");
    const auto* td = find("column_datatype_descriptor[" + n + "]");
    const auto* g = find("column_datatype_generation[" + n + "]");
    Check(t && td && g && UuidOf(t->value) == cohort.type_uuid && UuidOf(td->value) == cohort.descriptor_uuid && LE64(g->value) == cohort.generation,
          label + ": column " + n + " carries the cohort's " + (kinds[column] == 'b' ? "BOOLEAN" : "int64") + " identity (type, descriptor, generation)");
  }
  for (std::size_t row = 0; row < rows; ++row)
    for (std::size_t column = 0; column < columns; ++column) {
      const auto* v = find("value[" + std::to_string(row) + "." + std::to_string(column) + "]");
      if (kinds[column] == 'b')
        Check(v && v->kind == pr::Kind::bytes && v->value.size() == 1 && static_cast<std::int64_t>(static_cast<unsigned char>(v->value[0])) == expected[row][column],
              label + ": value[" + std::to_string(row) + "." + std::to_string(column) + "] is ONE byte " + std::to_string(expected[row][column]) + " (a boolean, never text)");
      else
        Check(v && v->kind == pr::Kind::signed_integer && v->value.size() == 8 && static_cast<std::int64_t>(LE64(v->value)) == expected[row][column],
              label + ": value[" + std::to_string(row) + "." + std::to_string(column) + "] is the int64 " + std::to_string(expected[row][column]));
    }
  Check(ran.raw_vector.find("true") == std::string::npos && ran.raw_vector.find("false") == std::string::npos && ran.raw_vector.find("column1") == std::string::npos,
        label + ": the engine's vector carries no rendered boolean and no column name");
}

struct Refusal2 {
  const char* sql;
  const char* code;          // the parser-visible diagnostic code (the table's own code)
  const char* server_status; // the status token the server's diagnostic carries ("" when the statement never reaches the engine)
  const char* asked;         // the exact rows the production table was asked, in order, from the log-only recorder ("(NOT listed)" marks the refusal)
  const char* what;
  // SB_NATIVE_REFUSAL_FIELDS_FORWARDED (2026-12-15): the ENGINE's own diagnostic fields as the parser receives them through the real pipeline, "key=value;key=value"
  // ("" = none forwarded; `<sha256>` = a 64-char lower-case hex row identity). OLD: the server dropped every engine field, so the opcode was only visible through
  // the hand-framed envelopes below.
  const char* engine_fields = "";
};


// SB_NATIVE_REFUSAL_FIELDS_FORWARDED (2026-12-15): the engine's diagnostic fields, as the PARSER receives them through the real pipeline (server -> SBMV message
// vector -> parser). `expected` is "key=value;key=value" ("" = none; `<sha256>` = a 64-char lower-case hex). `parser_only` shapes never reached the engine: none.
void CheckEngineFields(const Ran& ran, const std::string& sql, const std::string& expected, bool parser_only) {
  std::vector<std::pair<std::string, std::string>> want;
  for (std::size_t at = 0; at < expected.size();) {
    const auto end = std::min(expected.find(';', at), expected.size());
    const auto eq = expected.find('=', at);
    want.push_back({expected.substr(at, eq - at), expected.substr(eq + 1, end - eq - 1)});
    at = end + 1;
  }
  for (const auto& key : {"opcode_code", "row_identity_sha256", "rows", "columns", "row_bound", "column_bound", "operation", "requires"}) {
    std::string expect;
    bool listed = false;
    for (const auto& [k, v] : want) if (k == key) { expect = v; listed = true; }
    const auto got = FieldNamed(ran.result, key);
    if (!listed) {
      Check(got.empty(), "(D) '" + sql + "': the parser receives no engine field `" + key + "` here, got `" + got + "`");
    } else if (expect == "<sha256>") {
      Check(got.size() == 64 && std::all_of(got.begin(), got.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
            "(D) '" + sql + "': the parser receives the engine's row_identity_sha256 as 64 hex characters: `" + got + "`");
    } else {
      Check(got == expect, "(D) '" + sql + "': the parser receives the ENGINE's field " + key + "=" + expect + " through the real pipeline, got `" + got + "`: " + AllCodes(ran.result));
    }
  }
  (void)parser_only;
  // no identity UUID rides the text message vector (a typed binary field is an owner question): no field named *uuid reaches the parser from the engine's diagnostic
  for (const auto& d : ran.result.messages.diagnostics)
    for (const auto& f : d.fields) Check(f.name.find("uuid") == std::string::npos, "(D) '" + sql + "': no identity UUID field is forwarded: " + f.name);
}

void RouteBState() {
  (void)std::setvbuf(stdout, nullptr, _IONBF, 0);  // the observations below must survive a failing check
  const auto catalog_generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  // the cohort's int64 identity, read independently of the engine's encoder, from a direct receipt (the S4f path)
  auto direct = Approve();
  auto direct_public = OpenPublicFor(*direct);
  auto direct_context = ContextFor(*direct, catalog_generation);
  auto direct_receipt = Acquire(*direct_public, direct_context, direct->approval.transaction_uuid);
  Check(direct_receipt.ok(), "a direct native receipt (for the cohort identity and the gate's framed shapes): " + direct_receipt.code);
  const auto cohort = CohortInt64Of(CopyContext(direct_receipt));

  // ---- (c) the production availability table (no recorder is active): exactly the name-resolve and literal rows ----------------------------------
  {
    std::set<std::string> table;
    for (const auto& row : api::NativeExecutorAvailabilityAllowList()) table.insert(RowKey(row));
    Check(table == std::set<std::string>({"engine.op.literal#3", "engine.op.name_resolve#4865"}),
          "(c) the production allow-list is exactly {name-resolve 0x1301 (4865), literal 3}");
    for (const auto& row : api::NativeExecutorAvailabilityAllowList())
      if (row.executor_id == api::kSblrNameResolveExecutorId) Check(row.opcode_code == 0x1301, "(c) the name-resolve row is opcode 0x1301");
    for (const std::uint16_t code : {1, 2, 0x040E}) Check(api::NativeExecutorOpcodeCodeAllowed(code), "dispatch root opcode " + std::to_string(code) + " is admitted");
    for (const std::uint16_t code : {3, 0x0101, 0x030E, 0x0601, 0x1207, 0x1301, 1024, 1025, 0})
      Check(!api::NativeExecutorOpcodeCodeAllowed(code), "opcode " + std::to_string(code) + " is not a dispatch root");
  }

  srv::ServerSessionRegistry registry;
  const auto active = [&](std::uint64_t order) { return off::ReadFile(served.path, served.facts->page_size_bytes).words.at(order).state == pgs::TipStateCode::active; };
  Pipe one(registry);
  const auto order1 = one.order();
  Check(order1 != 0 && active(order1), "session 1's transaction is active after the login");
  const auto owner_of = [&](Pipe& pipe) {
    const auto found = registry.sessions_by_uuid.find(pipe.session().session_uuid);
    return found != registry.sessions_by_uuid.end() && found->second.local_transaction_id == pipe.order() &&
           found->second.transaction_uuid == pipe.session().transaction_uuid && found->second.native_session_approved;
  };
  Check(owner_of(one), "session 1's registry record owns the transaction (order and UUID)");

  // ONE statement with its own evidence: the data file byte-identical, the directory unchanged, Next unchanged (a separate offline read of the TIP
  // header), and the transaction still the session's and still active in the file.
  const auto run = [&](Pipe& pipe, const std::string& sql, int mode) {
    const FileEvidence before;
    auto ran = RunSql(pipe, sql, mode);
    CheckUntouched(before, "'" + sql + "' (recorder mode " + std::to_string(mode) + ")");
    Check(active(pipe.initial_order) && owner_of(pipe) && pipe.order() == pipe.initial_order,
          "(e) the transaction is still active and owned after '" + sql + "' (recorder mode " + std::to_string(mode) + ")");
    return ran;
  };

  // ---- (a) the accepted shapes, with NO recorder (the production table), each around byte-identity, directory and Next -----------------------------
  const std::vector<std::pair<std::string, std::int64_t>> accepted = {
      {"SELECT 1", 1}, {"select 1", 1}, {"SELECT 1;", 1}, {"SELECT 2", 2}, {"VALUES (1)", 1}, {"SELECT 9223372036854775807", INT64_MAX}};
  for (int round = 0; round < 2; ++round) {
    for (const auto& [sql, value] : accepted) {
      const FileEvidence before;
      const auto ran = RunSql(one, sql, 0);
      CheckUntouched(before, sql + " (round " + std::to_string(round) + ")");
      CheckAccepted(ran, sql, value, cohort);
      Check(active(order1) && owner_of(one) && one.order() == order1, "(e) the transaction is still active and owned after '" + sql + "'");
    }
  }

  // ---- (a2) WIDER LITERALS (2026-12-13): literal VALUES TABLES, one vector in one message, each around byte-identity, directory and Next ------------------
  // OLD: `VALUES (1),(2)` was refused at the dispatch gate with SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED (the native result was one row of one int64). NEW: it
  // answers with row_count 2. The bound is 16 rows by 16 columns (no streaming exists); above it, the distinct token below.
  {
    struct Table { std::string sql; std::vector<std::vector<std::int64_t>> rows; };
    const std::vector<Table> tables = {
        {"VALUES (1),(2)", {{1}, {2}}},
        {"VALUES (1, 2), (3, 4)", {{1, 2}, {3, 4}}},
        {"VALUES (3), (1), (2)", {{3}, {1}, {2}}},                       // the order written, not sorted
        {"VALUES (9223372036854775807), (2)", {{INT64_MAX}, {2}}},
        {"VALUES (1, 2, 3)", {{1, 2, 3}}},                               // one row, several columns
        {ValuesText(16, 1), ValuesRows(16, 1)},                          // the row bound itself
        {ValuesText(1, 16), ValuesRows(1, 16)},                          // the column bound itself
        {ValuesText(2, 16), ValuesRows(2, 16)},                          // 32 literals: the statement cohort's whole numeric slot count
    };
    // EVIDENCE PER GROUP (cost: every FileEvidence and `active` reads and parses the whole file, which at 128K pages made the per-statement form take 180 s
    // for this target): ONE evidence before the whole group and one check after it. A write by ANY statement of the group would change the image, the
    // directory or Next and fail the group check, so the group proves each statement wrote nothing; the transaction's registry ownership and order are
    // still checked after EVERY statement (cheap), its TIP state (active) once after the group.
    const FileEvidence group_before;
    for (std::size_t index = 0; index < tables.size(); ++index) {
      const auto& table = tables[index];
      // two rounds (recorder log-only, then no recorder) for the first three shapes; the other shapes run once, recorder log-only (the production table still decides)
      for (int round = 0; round < (index < 3 ? 2 : 1); ++round) {
        const auto ran = RunSql(one, table.sql, round == 0 ? 2 : 0);
        CheckAcceptedTable(ran, table.sql, table.rows, cohort);
        Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after the table '" + table.sql.substr(0, 40) + "'");
        if (round == 0) {
          std::string asked;
          for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
          Check(asked == "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ",
                "(a2) the table '" + table.sql.substr(0, 40) + "' asks exactly [name_resolve (bind), literal (finalize), literal (execution)], all listed: " + asked);
        }
      }
    }
    CheckUntouched(group_before, "the (a2) literal VALUES tables, every one, with and without the recorder");
    Check(active(order1), "(e) the transaction is still active after the (a2) tables");
  }

  // ---- (a3) BOOLEAN literals in a VALUES table (2026-12-13; OLD: `VALUES (TRUE)` failed in the PARSER's binding context with
  // numeric_literal_profile_bijection_invalid): the negotiated boolean profile is the literal's descriptor; the engine's vector names the boolean cohort row
  // and carries ONE byte; the parser renders true / false. `SELECT TRUE` is NOT a VALUES relation (a scalar projection) and still stops in the parser.
  {
    const auto boolean_cohort = CohortBooleanOf(CopyContext(direct_receipt));
    struct Mixed { std::string sql; std::string kinds; std::vector<std::vector<std::int64_t>> rows; };
    const std::vector<Mixed> mixed = {
        {"VALUES (TRUE)", "b", {{1}}},
        {"VALUES (FALSE)", "b", {{0}}},
        {"VALUES (TRUE), (FALSE)", "b", {{1}, {0}}},
        {"VALUES (FALSE), (TRUE), (FALSE)", "b", {{0}, {1}, {0}}},
        {"VALUES (1, TRUE), (2, FALSE)", "ib", {{1, 1}, {2, 0}}},
        {"VALUES (TRUE, 7), (FALSE, 9223372036854775807)", "bi", {{1, 7}, {0, INT64_MAX}}},
    };
    const FileEvidence group_before;  // evidence per group, see (a2)
    for (std::size_t index = 0; index < mixed.size(); ++index) {
      const auto& table = mixed[index];
      for (int round = 0; round < (index < 3 ? 2 : 1); ++round) {  // two rounds for the first three shapes, the rest once (recorder log-only)
        const auto ran = RunSql(one, table.sql, round == 0 ? 2 : 0);
        CheckAcceptedMixedTable(ran, table.sql, table.kinds, table.rows, cohort, boolean_cohort);
        Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after '" + table.sql + "'");
        if (round == 0) {
          std::string asked;
          for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
          Check(asked == "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ",
                "(a3) '" + table.sql + "' asks exactly [name_resolve (bind), literal (finalize), literal (execution)], all listed: " + asked);
        }
      }
    }
    // a column that mixes a boolean with an integer is not one type: the executor refuses it (no coercion is invented), nothing is written
    for (const std::string sql : {"VALUES (1), (TRUE)", "VALUES (TRUE), (1)"}) {
      const auto ran = RunSql(one, sql, 2);
      bool code_present = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == "SBLR.PLAN_TREE.INVALID_HANDLE") code_present = true;
      std::string asked;
      for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
      Check(!ran.result.accepted && code_present && asked == "engine.op.name_resolve#4865(listed) " && ran.server_diagnostics.empty(),
            "(a3) '" + sql + "' (a column mixing boolean and integer) is refused by the PARSER's lowering with SBLR.PLAN_TREE.INVALID_HANDLE, the engine never asked to finalize: " + AllCodes(ran.result) + " " + asked);
    }
    CheckUntouched(group_before, "the (a3) boolean tables and the mixed-column refusals");
    Check(active(order1), "(e) the transaction is still active after the (a3) boolean tables");
  }

  // ---- (b) the RECORDER log for SELECT 1: the Q302 evidence -------------------------------------------------------------------------------------
  {
    const auto ran = run(one, "SELECT 1", 1);
    const auto keys = KeysOf(ran);
    std::string joined;
    for (const auto& k : keys) joined += k + " ";
    std::printf("RECORDED route B `SELECT 1`: %s\n", joined.c_str());
    Check(keys == std::vector<std::string>({"engine.op.name_resolve#4865", "engine.op.literal#3", "engine.op.literal#3"}),
          "(b) the recorder logged EXACTLY {name_resolve 4865 (bind), literal 3 (finalize), literal 3 (execution)}: " + joined);
    for (const auto& r : ran.recorded)
      Check(r.in_allow_list, "(b) every row route B asked is on the production allow-list: " + RowKey(r.row));
    Check(ran.result.accepted, "(b) the granted run still returns the row");
    const auto again = run(one, "VALUES (1)", 1);
    const auto keys_values = KeysOf(again);
    Check(keys_values == keys, "(b) `VALUES (1)` asks exactly the same rows as `SELECT 1`");
    const auto other = run(one, "SELECT 2", 1);
    Check(KeysOf(other) == keys, "(b) `SELECT 2` asks exactly the same rows as `SELECT 1`");
    // shapes that ask for rows that are NOT on the list: the recorder shows which (the evidence for the next row to consider)
    const auto sum = run(one, "SELECT 1 + 0", 1);
    Check(KeysOf(sum) == std::vector<std::string>({"engine.op.name_resolve#4865", "engine.op.operator_call#1025"}),
          "(b) `SELECT 1 + 0` with every row granted asks exactly [name_resolve, operator_call 1025] (the row the production table refuses)");
    Check(sum.result.accepted, "(b) and with the operator row granted it runs (so the production refusal is the table's, not a missing executor)");
    Check(!sum.recorded.empty() && !sum.recorded.back().in_allow_list, "(b) the operator-call row is NOT on the production allow-list");
  }
  {
    std::set<std::string> table;
    for (const auto& row : api::NativeExecutorAvailabilityAllowList()) table.insert(RowKey(row));
    Check(table == std::set<std::string>({"engine.op.literal#3", "engine.op.name_resolve#4865"}), "(c) the recording did not change the production allow-list");
  }

  // ---- (d) refused shapes: exactly their code and opcode field, and NOTHING written -------------------------------------------------------------
  const std::string kSeventeenRows = ValuesText(17, 1), kSeventeenColumns = ValuesText(1, 17), kThirtyThree = ValuesText(33, 1);
  const std::vector<Refusal2> refused = {
      {"SELECT 1 + 0", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "INVALID_ARGUMENT", "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ",
       "an operator expression: the executor is refused on the operator-call row (opcode_code 1025)", "opcode_code=1025"},
      {"SELECT 1 + 0 AS s", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "INVALID_ARGUMENT", "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ",
       "an aliased operator projection (0x040E): the same operator-call row", "opcode_code=1025"},
      {"SELECT 'a'", "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ",
       "a text literal (the table cannot tell it from 1; the vector builder refuses: no text reaches a native client)", "operation=query.evaluate_projection;requires=native_result_vector_types"},
      {"SELECT 1 FROM t", "ENGINE.NATIVE.WIRE_NAME_RESOLVE_S5", "", "",
       "a table reference (the parser's own refusal: the engine is never asked)"},
      // OLD (2026-12-11): {"VALUES (1),(2)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "UNSUPPORTED", ..., "a multi-row VALUES: refused at the dispatch gate"}.
      // NEW (2026-12-13, wider literals): a multi-row VALUES runs (see (a2)); the bound is pinned instead (the gate refuses, no execution-time row asked).
      {kSeventeenRows.c_str(), "ENGINE.NATIVE.RESULT_ROW_BOUND_EXCEEDED", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ",
       "17 rows: one over the 16-row bound of a native result (no streaming): refused at the dispatch gate with its own token", "rows=17;columns=1;row_bound=16;column_bound=16"},
      {kSeventeenColumns.c_str(), "ENGINE.NATIVE.RESULT_ROW_BOUND_EXCEEDED", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ",
       "17 columns: one over the 16-column bound: refused at the dispatch gate with the same token", "rows=1;columns=17;row_bound=16;column_bound=16"},
      // OLD (2026-12-13): {kThirtyThree, "DATATYPE.DESCRIPTOR.INVALID", "", "name_resolve(listed)", "33 literals: ... (an undifferentiated parser code today; owner question)"}.
      // NEW (2026-12-15, owner yes): the DISTINCT parser token SBSQL.RESOURCE.LITERAL_SLOT_LIMIT_EXCEEDED (its slot_limit / literal_count fields are checked in RouteB2State (B)).
      {kThirtyThree.c_str(), "SBSQL.RESOURCE.LITERAL_SLOT_LIMIT_EXCEEDED", "", "engine.op.name_resolve#4865(listed) ",
       "33 literals: the statement descriptor cohort carries 32 numeric literal slots, so the PARSER refuses before the engine is asked to finalize, with its own token"},
      {"CREATE TABLE t (a INT)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) ",
       "DDL: refused at the dispatch gate (only the bind row was asked)", "opcode_code=1537"},
  };

  for (const auto& shape : refused) {
    for (int round = 0; round < 2; ++round) {
      const FileEvidence before;
      const auto ran = RunSql(one, shape.sql, round == 0 ? 2 : 0);
      CheckUntouched(before, std::string(shape.sql) + " (refused, round " + std::to_string(round) + ")");
      const auto codes = AllCodes(ran.result);
      Check(!ran.result.accepted, std::string("(d) '") + shape.sql + "' (" + shape.what + ") is refused: " + codes);
      Check(ran.result.server_result_payload.empty() || ran.result.server_result_payload.find("int64") == std::string::npos,
            std::string("(d) '") + shape.sql + "' returned no row");
      bool code_present = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == shape.code) code_present = true;
      Check(code_present, std::string("(d) '") + shape.sql + "' is refused with " + shape.code + ": " + codes);
      if (std::string(shape.server_status).empty()) {
        Check(ran.server_diagnostics.empty(), std::string("(d) '") + shape.sql + "' never reached the engine: " + ran.server_diagnostics);
      } else {
        Check(ran.server_diagnostics.starts_with(std::string(shape.code) + "{") && ran.server_diagnostics.find(std::string("status=") + shape.server_status + ";") != std::string::npos,
              std::string("(d) '") + shape.sql + "': the server's own diagnostic is " + shape.code + " with status " + shape.server_status + ": " + ran.server_diagnostics);
      }
      CheckEngineFields(ran, shape.sql, shape.engine_fields, std::string(shape.server_status).empty());
      if (round == 0) {
        std::string asked;
        for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
        Check(asked == shape.asked, std::string("(d) '") + shape.sql + "': the production table was asked exactly [" + shape.asked + "], got [" + asked + "]");
      }
      Check(active(order1) && owner_of(one) && one.order() == order1, std::string("(e) the transaction is still active and owned after the refused '") + shape.sql + "'");
    }
  }
  // WIDER LITERALS (2026-12-13), the literals that STILL stop, pinned at the layer where each stops today (parser code, the rows the production
  // table was asked, nothing written). Each is a numbered owner question in the report; none is built.
  {
    struct Pinned { const char* sql; const char* code; const char* asked; const char* what; const char* engine_fields = ""; };
    const std::vector<Pinned> pinned = {
        // OLD (2026-12-13): {"SELECT -1", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "name_resolve(listed) operator_call#1025(NOT listed)", "a negative literal: SBsql lowers `-1` as the
        // unary-minus OPERATOR over the literal 1 (no folding rule exists)"} and {"SELECT -9223372036854775808", the same, "the int64 minimum"}.
        // NEW (2026-12-15, owner: the PARSER folds a minus applied to a numeric literal): both run natively (see (A) in RouteB2State); `-9223372036854775809`
        // (one below the int64 minimum: the digits do not fit) is pinned instead, the same stop as the positive overflow below.
        {"SELECT -9223372036854775809", "DATATYPE.DESCRIPTOR.INVALID", "engine.op.name_resolve#4865(listed) ",
         "a negative int64-overflow literal: the PARSER cannot encode it under the int64 profile (the same stop as the positive overflow; decimal promotion is queued)"},
        {"SELECT 1.5", "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ",
         "a decimal literal: passes the gate (the exact template) and stops in the executor's descriptor batch validation (DECIMAL column profile)"},
        {"SELECT 0.1", "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ",
         "a decimal literal below one: the same executor refusal"},
        {"SELECT TRUE", "DATATYPE.DESCRIPTOR.INVALID", "engine.op.name_resolve#4865(listed) ",
         "a boolean literal as a bare SELECT: a scalar projection (not a VALUES relation); the PARSER's literal finalization stops it (bound literal occurrence inventory changed: the scalar-projection stopgap excludes boolean on purpose); the engine is never asked to finalize"},
        {"SELECT 9223372036854775808", "DATATYPE.DESCRIPTOR.INVALID", "engine.op.name_resolve#4865(listed) ",
         "an int64-overflow literal: the PARSER cannot encode it under the int64 profile it was negotiated (SBXN proof); the engine is never asked to finalize"},
        // OLD (2026-12-13): {"VALUES (-1)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "name_resolve(listed) literal#3(listed)", "a negative literal in VALUES: the unary-minus
        // expression is not the literal template"}. NEW (2026-12-15): `VALUES (-1)` runs natively (the fold makes it the literal template; see (A) in RouteB2State).
        {"VALUES ('a')", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) ",
         "a text literal in VALUES: no numeric or boolean literal profile exists for it, refused at the dispatch gate (the bare `SELECT 'a'` is a scalar projection with its own token, above)", "opcode_code=4615"},
        {"VALUES (NULL)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) ",
         "a NULL in VALUES: not the literal template, refused at the dispatch gate", "opcode_code=4615"},
        {"VALUES (9223372036854775808)", "DATATYPE.DESCRIPTOR.INVALID", "engine.op.name_resolve#4865(listed) ",
         "an int64-overflow literal in VALUES: the same PARSER stop as the bare SELECT form"},
        {"VALUES (1.5)", "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ",
         "a decimal literal in VALUES: the same executor stop as the bare SELECT form"},
        {"SELECT NULL", "DATATYPE.CONTEXT_REQUIRED", "",
         "a bare NULL: the PARSER needs one concrete datatype context before SBLR; the engine is never asked anything"},
    };
    const FileEvidence group_before;  // evidence per group, see (a2)
    for (const auto& shape : pinned) {
      const auto ran = RunSql(one, shape.sql, 2);
      Check(!ran.result.accepted, std::string("(d2) '") + shape.sql + "' (" + shape.what + ") is refused: " + AllCodes(ran.result));
      bool code_present = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == shape.code) code_present = true;
      Check(code_present, std::string("(d2) '") + shape.sql + "' is refused with " + shape.code + ": " + AllCodes(ran.result));
      std::string asked;
      for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
      Check(asked == shape.asked, std::string("(d2) '") + shape.sql + "': the production table was asked exactly [" + shape.asked + "], got [" + asked + "]");
      CheckEngineFields(ran, shape.sql, shape.engine_fields, false);
      Check(owner_of(one) && one.order() == order1, std::string("(e) the transaction is still owned after the pinned refusal '") + shape.sql + "'");
    }
    CheckUntouched(group_before, "the (d2) pinned refusals, every one");
    Check(active(order1), "(e) the transaction is still active after the (d2) pinned refusals");
  }
  // two statements as one envelope: the gate refuses a package of other than the exact three records (a hand-framed [begin, query.execute x2, end])
  {
    const FileEvidence before;
    bridge::StatementContextDispatchRequest dispatch;
    dispatch.receipt = direct_receipt.receipt;
    dispatch.canonical_operation_bytes = FramedStream({0x1207, 0x1207}, direct_receipt.view);
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
    const auto refusal = ReadRefusal(status, result);
    CheckUntouched(before, "two query.execute members in one envelope");
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && refusal.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && refusal.opcode_code == "4615",
          "(d) two statements as one envelope: the gate refuses SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED with opcode_code 4615: " + refusal.code + " " + refusal.opcode_code);
  }
  // the engine's OWN opcode field for the gate-refused shapes, read at the engine's dispatch gate with hand-framed envelopes.
  // OLD (2026-12-13): "the server does not forward the engine's diagnostic fields to the parser: the parser sees only the code and a status token", so the opcode
  // was visible only here. NEW (2026-12-15, owner yes): the server forwards them (SB_NATIVE_REFUSAL_FIELDS_FORWARDED) and every refused shape above ALSO asserts the
  // engine's opcode_code (or rows/columns/bounds, operation/requires) as the PARSER receives it through the real pipeline; these hand-framed envelopes remain the
  // direct engine-side check that the two agree.
  {
    struct GateShape { const char* what; std::vector<std::uint16_t> ops; const char* opcode_code; };
    const std::vector<GateShape> gate = {
        {"a typed VALUES query that is not the exact literal template (query.execute 0x1207 with no operands)", {0x1207}, "4615"},
        {"two query.execute members in one envelope (the multi-statement shape)", {0x1207, 0x1207}, "4615"},
        {"a DDL (CREATE TABLE, 0x0601)", {0x0601}, "1537"},
        {"a DML (INSERT, 0x030E)", {0x030E}, "782"},
        {"a commit (0x0101)", {0x0101}, "257"},
    };
    for (const auto& shape : gate) {
      const FileEvidence before;
      bridge::StatementContextDispatchRequest dispatch;
      dispatch.receipt = direct_receipt.receipt;
      dispatch.canonical_operation_bytes = FramedStream(shape.ops, direct_receipt.view);
      sb_engine_result_t result = nullptr;
      const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
      const auto refusal = ReadRefusal(status, result);
      CheckUntouched(before, shape.what);
      Check(status == SB_ENGINE_STATUS_UNSUPPORTED && refusal.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && refusal.opcode_code == shape.opcode_code,
            std::string("(d) ") + shape.what + ": the engine's gate refuses SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED with opcode_code " + shape.opcode_code + ": " + refusal.code + " " + refusal.opcode_code);
    }
  }
  // an explicitly named VALUES relation. OLD (2026-12-13): the parser took no column list from a VALUES alias and refused it with QOW-DIAG-QRY-001-AST-MALFORMED
  // (no engine involved). NEW (2026-12-15, owner: implement now if simple): it runs, the parser names the column from the alias list (the full set of alias
  // statements is in RouteB2State (C), with its own per-size targets).
  {
    const FileEvidence before;
    const auto ran = RunSql(one, "SELECT * FROM (VALUES (1)) AS t(x)", 0);
    CheckUntouched(before, "SELECT * FROM (VALUES (1)) AS t(x)");
    std::vector<std::uint32_t> oids;
    std::vector<std::vector<std::optional<std::string>>> rows;
    Check(ran.result.accepted && ps2::DecodePublicResultRowsForTest(ran.result.server_result_payload, &oids, &rows) && rows.size() == 1 && rows[0].size() == 1 && rows[0][0] &&
              *rows[0][0] == "1" && ran.result.server_result_payload.ends_with("x:int64:not_null"),
          "the explicitly named VALUES form runs and the parser names its column x: " + AllCodes(ran.result));
    Check(active(order1) && owner_of(one), "(e) the transaction is still active and owned after the aliased VALUES");
  }
  // `SELECT 1; SELECT 2` typed as ONE pipeline call: the parser takes the first statement only or refuses; either way nothing is written and the
  // transaction stays; the observed behaviour is pinned
  {
    const FileEvidence before;
    const auto ran = run(one, "SELECT 1; SELECT 2", 0);
    CheckUntouched(before, "SELECT 1; SELECT 2 in one call");
    Check(!ran.result.accepted && FirstCode(ran.result) == "SBLR.OPERATION.NONCANONICAL",
          "two statements in ONE pipeline call are refused by the parser with SBLR.OPERATION.NONCANONICAL (a client splits statements; each runs on its own): " + AllCodes(ran.result));
    Check(active(order1) && owner_of(one), "(e) the transaction is still active and owned after the two-statement text");
  }

  // ---- (e) two sessions are independent, one transaction each ------------------------------------------------------------------------------------
  {
    Pipe two(registry);
    const auto order2 = two.order();
    Check(order2 != order1 && two.session().session_uuid != one.session().session_uuid && two.session().transaction_uuid != one.session().transaction_uuid,
          "(e) a second session has its own session, transaction order and UUID");
    Check(active(order2) && owner_of(two) && active(order1) && owner_of(one), "(e) both transactions are active and owned");
    const auto a = run(one, "SELECT 11", 0);
    const auto b = run(two, "SELECT 22", 0);
    CheckAccepted(a, "SELECT 11", 11, cohort);
    CheckAccepted(b, "SELECT 22", 22, cohort);
    const auto va = DecodeVector(a.raw_vector), vb = DecodeVector(b.raw_vector);
    Check(va.ok && vb.ok && va.column_descriptor[0] != vb.column_descriptor[0], "(e) each session's result column carries its own engine-issued descriptor");
    Check(active(order1) && active(order2) && one.order() == order1 && two.order() == order2, "(e) neither transaction moved");
    two.End();
    const auto ended = off::ReadFile(served.path, served.facts->page_size_bytes).words.at(order2).state;
    Check(ended != pgs::TipStateCode::active, "(e) session 2's disconnect ended its transaction");
    Check(active(order1), "(e) session 1's transaction is untouched by session 2's end");
    const auto after = run(one, "SELECT 3", 0);
    CheckAccepted(after, "SELECT 3", 3, cohort);
  }

  Release(direct_receipt);
  direct_public.reset();
  Disconnect(*direct);
  one.End();
  Check(off::ReadFile(served.path, served.facts->page_size_bytes).words.at(order1).state != pgs::TipStateCode::active, "session 1's disconnect ended its transaction");
}

// SB_NATIVE_ROUTE_B2 (2026-12-15, parser/results batch): the parser-side additions of this batch, in their OWN state (separate ctest targets per page
// size, so the largest route_b target does not grow): (A) negative literals folded by the PARSER, (B) the distinct token above the 32-literal slot limit,
// (C) the VALUES derived-table alias column list. Evidence per GROUP (see (a2) in RouteBState): one byte-identity / directory / Next check around each
// group; registry ownership and order after every statement.
void RouteB2State() {
  (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto catalog_generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  auto direct = Approve();
  auto direct_public = OpenPublicFor(*direct);
  auto direct_context = ContextFor(*direct, catalog_generation);
  auto direct_receipt = Acquire(*direct_public, direct_context, direct->approval.transaction_uuid);
  Check(direct_receipt.ok(), "a direct native receipt (for the cohort identity): " + direct_receipt.code);
  const auto cohort = CohortInt64Of(CopyContext(direct_receipt));
  {
    std::set<std::string> table;
    for (const auto& row : api::NativeExecutorAvailabilityAllowList()) table.insert(RowKey(row));
    Check(table == std::set<std::string>({"engine.op.literal#3", "engine.op.name_resolve#4865"}),
          "(c) the production allow-list is exactly {name-resolve 0x1301 (4865), literal 3} (this batch adds NO row)");
  }
  srv::ServerSessionRegistry registry;
  const auto active = [&](std::uint64_t order) { return off::ReadFile(served.path, served.facts->page_size_bytes).words.at(order).state == pgs::TipStateCode::active; };
  Pipe one(registry);
  const auto order1 = one.order();
  const auto owner_of = [&](Pipe& pipe) {
    const auto found = registry.sessions_by_uuid.find(pipe.session().session_uuid);
    return found != registry.sessions_by_uuid.end() && found->second.local_transaction_id == pipe.order() &&
           found->second.transaction_uuid == pipe.session().transaction_uuid && found->second.native_session_approved;
  };
  Check(order1 != 0 && active(order1) && owner_of(one), "session 1's transaction is active and owned after the login");
  const auto asked_of = [](const Ran& ran) {
    std::string asked;
    for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
    return asked;
  };
  const std::string kThree = "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ";

  // ---- (A) NEGATIVE LITERALS: the parser folds a minus directly applied to a numeric literal token ---------------------------------------------------
  // OLD (2026-12-13, 3916b9e35): `SELECT -1` and `SELECT -9223372036854775808` -> SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED (unary-minus OPERATOR over the
  // literal, operator_call 1025 not on the allow-list), `VALUES (-1)` -> NATIVE_NOT_IMPLEMENTED at the dispatch gate. NEW: they run as int64 values and ask
  // exactly the literal rows; the allow-list is unchanged. A minus over a non-literal is still the operator and still refused.
  {
    const FileEvidence group_before;
    const std::vector<std::pair<std::string, std::int64_t>> scalars = {
        {"SELECT -1", -1}, {"select -1", -1}, {"SELECT - 1", -1}, {"SELECT -1;", -1}, {"SELECT -0", 0}, {"SELECT -9223372036854775808", INT64_MIN}, {"SELECT -9223372036854775807", -INT64_MAX}};
    for (const auto& [sql, value] : scalars) {
      const auto ran = RunSql(one, sql, 2);
      CheckAccepted(ran, sql, value, cohort);
      Check(asked_of(ran) == kThree, "(A) '" + sql + "' asks exactly [name_resolve, literal, literal], all listed: " + asked_of(ran));
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after '" + sql + "'");
    }
    struct Table { std::string sql; std::vector<std::vector<std::int64_t>> rows; };
    const std::vector<Table> tables = {
        {"VALUES (-1)", {{-1}}},
        {"VALUES (-1),(2)", {{-1}, {2}}},
        {"VALUES (-1, -2), (3, -4)", {{-1, -2}, {3, -4}}},
        {"VALUES (-9223372036854775808), (9223372036854775807)", {{INT64_MIN}, {INT64_MAX}}},
        {"VALUES (3), (-1), (2)", {{3}, {-1}, {2}}},
    };
    for (const auto& table : tables) {
      const auto ran = RunSql(one, table.sql, 2);
      CheckAcceptedTable(ran, table.sql, table.rows, cohort);
      Check(asked_of(ran) == kThree, "(A) '" + table.sql + "' asks exactly [name_resolve, literal, literal], all listed: " + asked_of(ran));
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after '" + table.sql + "'");
    }
    CheckUntouched(group_before, "the (A) negative-literal statements, every one");
    Check(active(order1), "(e) the transaction is still active after the (A) negative literals");
  }
  // a minus NOT applied directly to a numeric literal token stays the operator (refused, nothing written; the operator_call row 1025 is NOT on the allow-list); `1 - 1`
  // and `- -1` are unchanged operator shapes (`- -1`: the outer minus is an operator over the folded literal -1; PostgreSQL, MySQL and SQLite also
  // evaluate it as an operator application, giving 1)
  {
    struct Op { const char* sql; const char* code; const char* asked; const char* engine_fields = ""; };
    // observed with the fold REMOVED as well (identical): these shapes are unchanged by the fold. `-(1)`, `1 - 1`, `2 - -1`: the PARSER's own projection
    // refusal (the engine is never asked; a binary minus has no lowering today: found, not changed); `- -1` and `-1 + 0`: the operator_call row.
    const std::vector<Op> ops = {
        {"SELECT -(1)", "SBSQL.QUERY.PROJECTION_INVALID", ""},
        {"SELECT - -1", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ", "opcode_code=1025"},
        {"SELECT 1 - 1", "SBSQL.QUERY.PROJECTION_INVALID", ""},
        {"SELECT 2 - -1", "SBSQL.QUERY.PROJECTION_INVALID", ""},
        {"SELECT -1 + 0", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ", "opcode_code=1025"},
        // in a VALUES row the operator shapes stop at the dispatch gate (a literal was finalized, no execution row asked); the fold must NOT reach them
        {"VALUES (-(1))", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ", "opcode_code=4615"},
        {"VALUES (- -1)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ", "opcode_code=4615"},
        {"VALUES (1 - 1)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ", "opcode_code=4615"},
        {"VALUES (-1 + 1)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ", "opcode_code=4615"},
    };
    const FileEvidence group_before;
    for (const auto& op : ops) {
      const auto ran = RunSql(one, op.sql, 2);
      bool code_present = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == op.code) code_present = true;
      Check(!ran.result.accepted && code_present, std::string("(A) '") + op.sql + "' is still the operator and is refused with " + op.code + ": " + AllCodes(ran.result));
      Check(asked_of(ran) == op.asked, std::string("(A) '") + op.sql + "': the production table was asked [" + op.asked + "], got [" + asked_of(ran) + "]");
      CheckEngineFields(ran, op.sql, op.engine_fields, false);
      Check(owner_of(one) && one.order() == order1, std::string("(e) the transaction is still owned after the refused '") + op.sql + "'");
    }
    CheckUntouched(group_before, "the (A) operator shapes that stay refused");
  }

  // ---- (B) the DISTINCT token above the statement cohort's literal slot limit (2026-12-15; owner yes) ----------------------------------------------------
  // OLD: DATATYPE.DESCRIPTOR.INVALID (undifferentiated). NEW: SBSQL.RESOURCE.LITERAL_SLOT_LIMIT_EXCEEDED with DATA fields literal_class, slot_limit and
  // literal_count (no rendered prose); the slot count is the context's (32) and is NOT raised: 32 still runs, 33 is refused before the engine is asked.
  {
    struct Over { std::string sql; const char* literal_class; const char* count; };
    std::string booleans = "VALUES ";
    for (int i = 0; i < 33; ++i) booleans += std::string(i ? ", " : "") + "(TRUE)";
    const std::vector<Over> over = {
        {ValuesText(33, 1), "numeric", "33"}, {ValuesText(40, 1), "numeric", "40"}, {ValuesText(3, 13), "numeric", "39"}, {booleans, "boolean", "33"}};
    const FileEvidence group_before;
    for (const auto& shape : over) {
      const auto ran = RunSql(one, shape.sql, 2);
      const auto label = "(B) '" + shape.sql.substr(0, 40) + "...' (" + shape.count + " " + shape.literal_class + " literals)";
      Check(!ran.result.accepted && FirstCode(ran.result) == "SBSQL.RESOURCE.LITERAL_SLOT_LIMIT_EXCEEDED", label + " is refused with the distinct token: " + AllCodes(ran.result));
      Check(FieldNamed(ran.result, "slot_limit") == "32" && FieldNamed(ran.result, "literal_count") == shape.count && FieldNamed(ran.result, "literal_class") == shape.literal_class,
            label + " carries slot_limit 32, literal_count " + shape.count + " and the literal class as DATA fields: " + AllCodes(ran.result));
      Check(ran.server_diagnostics.empty() && asked_of(ran) == "engine.op.name_resolve#4865(listed) ", label + " never asked the engine to finalize: [" + asked_of(ran) + "]");
      bool undifferentiated = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == "DATATYPE.DESCRIPTOR.INVALID") undifferentiated = true;
      Check(!undifferentiated, label + " no longer gives the undifferentiated DATATYPE.DESCRIPTOR.INVALID");
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after " + label);
    }
    // the limit itself still runs (32 literals; the slot count was not raised)
    const auto at_limit = RunSql(one, ValuesText(2, 16), 2);
    CheckAcceptedTable(at_limit, "VALUES (...32 literals...)", ValuesRows(2, 16), cohort);
    CheckUntouched(group_before, "the (B) slot-limit refusals and the 32-literal statement");
    Check(active(order1), "(e) the transaction is still active after the (B) refusals");
  }

  // ---- (C) the VALUES DERIVED-TABLE COLUMN LIST (2026-12-15; owner 2026-11-26: implement now if simple) ---------------------------------------------------
  // OLD: `SELECT * FROM (VALUES (1)) AS t(x)` was refused by the parser (QOW-DIAG-QRY-001-AST-MALFORMED, catalog_relation_name_required; the previous route_b
  // block pinned it). NEW: it runs natively as the SAME literal VALUES relation the bare `VALUES (1)` builds; the alias list is parser data (relation.column_aliases),
  // the column NAMES are applied by the parser when it builds the result, the engine's vector still carries no name and the lowered statement is unchanged.
  // The spelling is kept as written (quoted: its content). Evidence per group; the production allow-list is unchanged (three rows asked).
  {
    struct Aliased { std::string sql; std::vector<std::string> names; std::vector<std::vector<std::int64_t>> rows; };
    const std::vector<Aliased> aliased = {
        {"SELECT * FROM (VALUES (1)) AS t(x)", {"x"}, {{1}}},
        {"SELECT * FROM (VALUES (1, 2)) AS t(a, b)", {"a", "b"}, {{1, 2}}},
        {"SELECT * FROM (VALUES (1), (2)) AS t(x)", {"x"}, {{1}, {2}}},
        {"SELECT * FROM (VALUES (1, 2), (3, 4)) AS t(a, b);", {"a", "b"}, {{1, 2}, {3, 4}}},
        {"SELECT x FROM (VALUES (1)) AS t(x)", {"x"}, {{1}}},                       // an identity projection of every column in order
        {"SELECT a, b FROM (VALUES (1, 2)) AS t(a, b)", {"a", "b"}, {{1, 2}}},
        {"select * from (values (3), (-1)) as t(n)", {"n"}, {{3}, {-1}}},             // keywords in any case; a folded negative literal
        {"SELECT * FROM (VALUES (1)) t(x)", {"x"}, {{1}}},                          // AS is optional (the reference engines accept it)
        {"SELECT * FROM (VALUES (7)) AS t", {"column1"}, {{7}}},                    // no column list: the default names column1..
        {"SELECT * FROM (VALUES (7, 8))", {"column1", "column2"}, {{7, 8}}},        // no alias at all
        {"SELECT column1, column2 FROM (VALUES (1, 2)) AS t", {"column1", "column2"}, {{1, 2}}},   // the default names are projectable too
        {"SELECT * FROM (VALUES (1)) AS t(Abc)", {"Abc"}, {{1}}},                   // unquoted: kept as written (no case fold of the label)
        {"SELECT * FROM (VALUES (1)) AS t(\"Abc\")", {"Abc"}, {{1}}},               // quoted: its content, exact
        {"SELECT * FROM (VALUES (1)) AS t(\"x y\")", {"x y"}, {{1}}},               // a quoted name with a space
        {"SELECT * FROM (VALUES (1)) AS t(\"a:b\")", {"a:b"}, {{1}}},               // a quoted name with the metadata separators
        {"SELECT * FROM (" + ValuesText(16, 1) + ") AS t(v)", {"v"}, ValuesRows(16, 1)},   // the 16-row bound with an alias
        {"SELECT * FROM (" + ValuesText(1, 16) + ") AS t(c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16)",
         {"c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9", "c10", "c11", "c12", "c13", "c14", "c15", "c16"}, ValuesRows(1, 16)},
    };
    const FileEvidence group_before;
    for (const auto& item : aliased) {
      const auto ran = RunSql(one, item.sql, 2);
      const std::string label = "(C) '" + item.sql.substr(0, 60) + "'";
      const auto& r = ran.result;
      Check(r.accepted && !r.messages.has_errors(), label + " is accepted through the real pipeline: " + AllCodes(r));
      std::vector<std::uint32_t> oids;
      std::vector<std::vector<std::optional<std::string>>> got;
      Check(ps2::DecodePublicResultRowsForTest(r.server_result_payload, &oids, &got) && got.size() == item.rows.size() && oids.size() == item.names.size(),
            label + ": the parser-built table is " + std::to_string(item.rows.size()) + " rows by " + std::to_string(item.names.size()) + " columns");
      for (std::size_t row = 0; row < item.rows.size() && row < got.size(); ++row)
        for (std::size_t column = 0; column < item.names.size() && column < got[row].size(); ++column)
          Check(got[row][column] && *got[row][column] == std::to_string(item.rows[row][column]), label + ": row " + std::to_string(row) + " column " + std::to_string(column) + " is as written");
      std::string meta;
      for (std::size_t column = 0; column < item.names.size(); ++column) meta += (column ? ";" : "") + item.names[column] + ":int64:not_null";
      Check(r.server_result_payload.ends_with(meta), label + ": the parser's row metadata names the columns " + meta);
      const auto vector = DecodeVector(ran.raw_vector);
      Check(vector.ok && vector.column_count == item.names.size() && vector.row_count == item.rows.size(), label + ": the engine's vector has the same shape as the bare VALUES");
      bool name_in_vector = false;
      for (const auto& name : item.names) if (ran.raw_vector.find(name + ":") != std::string::npos || (name.size() > 2 && ran.raw_vector.find(name) != std::string::npos)) name_in_vector = true;
      Check(!name_in_vector && ran.raw_vector.find("column1") == std::string::npos, label + ": the engine's vector carries no name (the alias never reaches the engine)");
      Check(asked_of(ran) == kThree, label + " asks exactly [name_resolve, literal, literal], all listed: " + asked_of(ran));
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after " + label);
    }
    // the alias is NOT sent to the engine: the request the engine receives is byte for byte the bare VALUES request (same size, no alias text anywhere in it)
    {
      const std::string long_alias = "a_very_long_alias_name_that_would_change_the_request_size";
      const auto bare = RunSql(one, "VALUES (1, 2)", 0);
      const auto named = RunSql(one, "SELECT * FROM (VALUES (1, 2)) AS tbl(" + long_alias + ", second_alias_name)", 0);
      Check(bare.result.accepted && named.result.accepted, "(C) the bare and the aliased VALUES both run: " + AllCodes(named.result));
      Check(named.result.server_request_payload_bytes == bare.result.server_request_payload_bytes && named.result.server_request_payload_bytes != 0,
            "(C) the aliased request carries exactly as many bytes as the bare VALUES request (" + std::to_string(named.result.server_request_payload_bytes) + " vs " +
                std::to_string(bare.result.server_request_payload_bytes) + "): the alias never reaches the engine");
      Check(named.result.sblr_payload.find(long_alias) == std::string::npos && named.result.sblr_payload.find("second_alias_name") == std::string::npos &&
                named.result.sblr_payload.find("tbl") == std::string::npos,
            "(C) the lowered statement holds no alias text");
      Check(named.result.server_result_payload.ends_with(long_alias + ":int64:not_null;second_alias_name:int64:not_null"), "(C) yet the parser's result names the columns from the list");
    }
    // a boolean column keeps its alias and prints true / false
    {
      const auto ran = RunSql(one, "SELECT * FROM (VALUES (TRUE, -2)) AS t(b, n)", 2);
      std::vector<std::uint32_t> oids;
      std::vector<std::vector<std::optional<std::string>>> got;
      Check(ran.result.accepted && ps2::DecodePublicResultRowsForTest(ran.result.server_result_payload, &oids, &got) && got.size() == 1 && got[0].size() == 2 && got[0][0] &&
                *got[0][0] == "true" && got[0][1] && *got[0][1] == "-2" && ran.result.server_result_payload.ends_with("b:boolean:not_null;n:int64:not_null"),
            "(C) a boolean and an int64 column keep their aliases: " + AllCodes(ran.result));
    }
    CheckUntouched(group_before, "the (C) VALUES derived-table statements, every one");
    Check(active(order1), "(e) the transaction is still active after the (C) statements");
  }
  // refusals: each its own parser reason, the counts as DATA fields, the engine never asked, nothing written
  {
    struct Refused { std::string sql; const char* reason; const char* f1; const char* v1; const char* f2; const char* v2; };
    const std::vector<Refused> refused = {
        {"SELECT * FROM (VALUES (1)) AS t(x, y)", "values_alias_column_count_mismatch", "alias_count", "2", "column_count", "1"},
        {"SELECT * FROM (VALUES (1, 2, 3)) AS t(x)", "values_alias_column_count_mismatch", "alias_count", "1", "column_count", "3"},
        // a projection that subsets or reorders the columns (a projection over the VALUES relation): LARGE, not built (follow-up)
        {"SELECT x FROM (VALUES (1, 2)) AS t(x, y)", "values_alias_projection_not_built", "projected_count", "1", "column_count", "2"},
        {"SELECT y, x FROM (VALUES (1, 2)) AS t(x, y)", "values_alias_projection_not_built", "projected_count", "2", "column_count", "2"},
        {"SELECT column2 FROM (VALUES (1, 2)) AS t", "values_alias_projection_not_built", "projected_count", "1", "column_count", "2"},
        {"SELECT z FROM (VALUES (1, 2)) AS t(x, y)", "values_alias_column_not_found", "projected_count", "1", "column_count", "2"},
        {"SELECT x, x FROM (VALUES (1, 1)) AS t(x, x)", "values_alias_column_ambiguous", "projected_count", "2", "column_count", "2"},
    };
    const FileEvidence group_before;
    for (const auto& item : refused) {
      const auto ran = RunSql(one, item.sql, 2);
      const std::string label = "(C) '" + item.sql + "'";
      Check(!ran.result.accepted && FirstCode(ran.result) == "QOW-DIAG-QRY-001-AST-MALFORMED" && FieldNamed(ran.result, "reason") == item.reason &&
                FieldNamed(ran.result, item.f1) == item.v1 && FieldNamed(ran.result, item.f2) == item.v2,
            label + " is refused by the parser with reason " + item.reason + " and " + item.f1 + "=" + item.v1 + ", " + item.f2 + "=" + item.v2 + " as data: " + AllCodes(ran.result));
      Check(ran.server_diagnostics.empty() && ran.recorded.empty(), label + " never reached the engine: [" + asked_of(ran) + "]");
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after the refused " + label);
    }
    // shapes the new route does NOT claim keep their established refusal (a tail after the alias list; an empty list)
    for (const std::string sql : {"SELECT * FROM (VALUES (1)) AS t(x) WHERE x = 1", "SELECT * FROM (VALUES (1)) AS t()"}) {
      const auto ran = RunSql(one, sql, 2);
      Check(!ran.result.accepted && FirstCode(ran.result) == "QOW-DIAG-QRY-001-AST-MALFORMED" && FieldNamed(ran.result, "reason") == "catalog_relation_name_required" && ran.recorded.empty(),
            "(C) '" + sql + "' keeps its established refusal (catalog_relation_name_required), the engine never asked: " + AllCodes(ran.result));
    }
    CheckUntouched(group_before, "the (C) refusals");
    Check(active(order1), "(e) the transaction is still active after the (C) refusals");
  }

  // ---- (E) the grouped-aggregate route identifies VALUES columns by POSITION (2026-12-15; owner 2026-11-26 item 2) ---------------------------------------------
  // OLD: the parser's context mapped a column NAME to its position from the text constants key_a / key_b / amount (any other name became column 0) and the engine's
  // grouped-aggregate preparation compared output names with the text constants {key_a, key_b, row_count, total_amount, ...}. NEW: the parser maps the names of the
  // statement's own alias list to positions and the engine compares positions. The native database refuses aggregates at the dispatch gate (as before), so the
  // evidence here is that a statement spelled with ANY column names reaches exactly the same outcome and lowers to the same-size statement as the key_a form.
  {
    const FileEvidence group_before;
    const auto fixture = RunSql(one, "SELECT key_a, COUNT(*), SUM(amount) FROM (VALUES (1, 5), (1, 7)) AS input(key_a, amount) GROUP BY key_a", 2);
    const auto renamed = RunSql(one, "SELECT col_a, COUNT(*), SUM(valuez) FROM (VALUES (1, 5), (1, 7)) AS input(col_a, valuez) GROUP BY col_a", 2);
    Check(!fixture.result.accepted && FirstCode(fixture.result) == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && FieldNamed(fixture.result, "opcode_code") == "4615",
          "(E) the key_a-named aggregate reaches the dispatch gate (opcode 4615): " + AllCodes(fixture.result));
    Check(FirstCode(renamed.result) == FirstCode(fixture.result) && FieldNamed(renamed.result, "opcode_code") == "4615" && asked_of(renamed) == asked_of(fixture) &&
              renamed.result.server_request_payload_bytes == fixture.result.server_request_payload_bytes,
          "(E) the same statement spelled with the equal-length names col_a and valuez reaches the identical outcome and lowers to the BYTE-IDENTICAL statement payload (the names are mapped to positions by the parser): " + AllCodes(renamed.result));
    const auto having_fixture = RunSql(one, "SELECT key_a, COUNT(*), SUM(amount) FROM (VALUES (1, 5), (1, 7)) AS input(key_a, amount) GROUP BY key_a HAVING SUM(amount) > 5", 2);
    const auto having_renamed = RunSql(one, "SELECT col_a, COUNT(*), SUM(valuez) FROM (VALUES (1, 5), (1, 7)) AS input(col_a, valuez) GROUP BY col_a HAVING SUM(valuez) > 5", 2);
    Check(!having_fixture.result.accepted && FieldNamed(having_fixture.result, "opcode_code") == "4615" && FirstCode(having_renamed.result) == FirstCode(having_fixture.result) &&
              FieldNamed(having_renamed.result, "opcode_code") == "4615" && asked_of(having_renamed) == asked_of(having_fixture),
          "(E) the HAVING form reaches the same outcome with the renamed columns: " + AllCodes(having_renamed.result));
    const auto wrong = RunSql(one, "SELECT valuez, COUNT(*), SUM(col_a) FROM (VALUES (1, 5), (1, 7)) AS input(col_a, valuez) GROUP BY valuez", 2);
    Check(!wrong.result.accepted && FieldNamed(wrong.result, "reason") == "inline_values_projection_mismatch" && wrong.recorded.empty(),
          "(E) the key/sum order the route admits (first column key, last column sum) is still checked by position: " + AllCodes(wrong.result));
    CheckUntouched(group_before, "the (E) aggregate statements");
  }

  Release(direct_receipt);
  direct_public.reset();
  Disconnect(*direct);
  one.End();
  Check(off::ReadFile(served.path, served.facts->page_size_bytes).words.at(order1).state != pgs::TipStateCode::active, "session 1's disconnect ended its transaction");
}

// THE STRACE STATE: the per-statement region holds only the pipeline's statements (the test's own file reads sit in prepare and verify).
void RouteBTraceState() {
  srv::ServerSessionRegistry registry;
  Mark("approve");
  auto one = std::make_unique<Pipe>(registry);
  Mark("prepare");
  const auto image_before = Image(served.path);
  const auto listing_before = ListingOf(served.directory);
  Mark("statement");
  struct Traced { const char* sql; bool accepted; std::int64_t value; const char* code; };
  const std::vector<Traced> traced = {
      {"SELECT 1", true, 1, ""}, {"select 1", true, 1, ""}, {"SELECT 1;", true, 1, ""}, {"SELECT 2", true, 2, ""}, {"VALUES (1)", true, 1, ""},
      {"SELECT 1 + 0", false, 0, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"}, {"SELECT 'a'", false, 0, "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT"},
      // OLD (2026-12-11): {"VALUES (1),(2)", false, 0, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"}. NEW (2026-12-13): it runs (the table below); the bound is traced instead.
      {"VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(10),(11),(12),(13),(14),(15),(16),(17)", false, 0, "ENGINE.NATIVE.RESULT_ROW_BOUND_EXCEEDED"}, {"SELECT 1", true, 1, ""}};
  for (const auto& statement : traced) {
    const auto ran = RunSql(*one, statement.sql, 0);
    bool code_present = false;
    for (const auto& d : ran.result.messages.diagnostics) if (d.code == statement.code) code_present = true;
    if (statement.accepted) {
      std::vector<std::uint32_t> oids;
      std::vector<std::vector<std::optional<std::string>>> rows;
      Check(ran.result.accepted && ps2::DecodePublicResultRowsForTest(ran.result.server_result_payload, &oids, &rows) && rows.size() == 1 && rows[0].size() == 1 &&
                rows[0][0] && *rows[0][0] == std::to_string(statement.value) && ran.result.server_result_payload.ends_with(kInt64Column + ":int64:not_null"),
            std::string("(traced) '") + statement.sql + "' ran and returned column1 = " + std::to_string(statement.value) + ": " + AllCodes(ran.result));
    } else {
      Check(!ran.result.accepted && code_present, std::string("(traced) '") + statement.sql + "' was refused with " + statement.code + ": " + AllCodes(ran.result));
    }
  }
  {
    // wider literals (2026-12-13): a multi-row, multi-column table inside the traced region
    const auto ran = RunSql(*one, "VALUES (3, 4), (1, 2), (5, 6)", 0);
    std::vector<std::uint32_t> oids;
    std::vector<std::vector<std::optional<std::string>>> rows;
    Check(ran.result.accepted && ps2::DecodePublicResultRowsForTest(ran.result.server_result_payload, &oids, &rows) && rows.size() == 3 && oids.size() == 2 &&
              rows[0][0] && *rows[0][0] == "3" && rows[0][1] && *rows[0][1] == "4" && rows[1][0] && *rows[1][0] == "1" && rows[2][1] && *rows[2][1] == "6" &&
              ran.result.server_result_payload.ends_with("column1:int64:not_null;column2:int64:not_null"),
          "(traced) a 3-row 2-column table ran and returned its rows in the order written: " + AllCodes(ran.result));
  }
  Mark("verify");
  Check(Image(served.path) == image_before, "the statements wrote nothing: the file's bytes are identical");
  Check(ListingOf(served.directory) == listing_before, "and no file appeared");
  Mark("disconnect");
  one->End();
  one.reset();
}

std::optional<int> ExtraReceiptStateForTest(const std::string& state, const Offline&) {
  if (state == "route_b") { RouteBState(); return 0; }
  if (state == "route_b2") { RouteB2State(); return 0; }
  if (state == "trace") { RouteBTraceState(); return 0; }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if ((argc == 3 || argc == 4) && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    // `size N` runs the route_b state; `size N route_b2` (this batch's parser additions) runs its own state
    RunState(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), argc == 4 ? argv[3] : "route_b");
    artifacts.Cleanup();
    std::printf("native_route_b: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
