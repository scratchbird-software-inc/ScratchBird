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
#include "engine/sblr/sblr_literal_runtime.hpp"  // the engine decimal codec (the value_types state's byte oracle)
#include "sblr_dispatch_server.hpp"  // SB_SBMV_TYPED_UUID_FIELDS seams: RegisteredEngineDiagnosticIdentityFieldsForTest, PeerAcceptsTypedUuidDiagnosticFieldsForTest
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
  // OLD (before 2026-12-25): 4 + 4 * columns + columns * rows (four fields per column). NEW: six (column_precision and column_scale follow the generation; 0, 0 for int64 and boolean)
  // OLD (before 2026-12-26): 4 + 6 * columns + columns * rows. NEW: SEVEN fields per column (column_nullability follows column_scale; 0 for a column that holds no NULL)
  Check(fields.size() == 4 + 7 * columns + columns * rows, label + ": the vector holds exactly the header, seven fields per column and one value per cell (no name, no text)");
  for (std::size_t column = 0; column < columns; ++column) {
    const auto* cnull = find("column_nullability[" + std::to_string(column) + "]");
    Check(cnull && cnull->kind == pr::Kind::unsigned_integer && cnull->value.size() == 8 && LE64(cnull->value) == 0, label + ": column " + std::to_string(column) + " declares not-null (column_nullability 0)");
    const auto* cp = find("column_precision[" + std::to_string(column) + "]");
    const auto* cs = find("column_scale[" + std::to_string(column) + "]");
    Check(cp && cs && cp->kind == pr::Kind::unsigned_integer && cs->kind == pr::Kind::unsigned_integer && LE64(cp->value) == 0 && LE64(cs->value) == 0,
          label + ": column " + std::to_string(column) + " declares no decimal shape (precision 0, scale 0)");
  }
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
  // OLD (b906cae90): for EVERY field of every diagnostic, Check(f.name.find("uuid") == npos, "no identity UUID field is forwarded") (a typed binary field was an owner question).
  // NEW (2026-12-17; owner 2026-11-26 "yes, native-only"; SB_SBMV_TYPED_UUID_FIELDS): a TEXT field named *uuid is still never forwarded (unchanged); a field of kind
  // binary_uuid is allowed ONLY for a registered (code, name) pair and only as exactly 16 bytes (the exact bytes are asserted in the `typed_uuid` state and by native_wire_exchange.py).
  for (const auto& d : ran.result.messages.diagnostics)
    for (const auto& f : d.fields) {
      if (f.kind == scratchbird::parser::ipc::FieldKind::binary_uuid) {
        const bool registered = (d.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && (f.name == "availability_table_uuid" || f.name == "database_uuid")) ||
                                (d.code == "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY" && f.name == "database_uuid");
        Check(registered && f.value.size() == 16, "(D) '" + sql + "': a binary identity field is a registered (code, name) of exactly 16 bytes: " + d.code + " " + f.name);
      } else {
        Check(f.name.find("uuid") == std::string::npos, "(D) '" + sql + "': no uuid-named TEXT field is forwarded: " + f.name);
      }
    }
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
  // and carries ONE byte; the parser renders true / false. [OLD: `SELECT TRUE` is NOT a VALUES relation (a scalar projection) and still stops in the parser. NEW (2026-12-24): the bare
  // `SELECT TRUE` IS the VALUES relation, see the `value_types` state.]
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
        // OLD (2026-12-15): {"SELECT -9223372036854775809", "DATATYPE.DESCRIPTOR.INVALID", [name_resolve], "a negative int64-overflow literal: the PARSER cannot encode it under the
        // int64 profile (decimal promotion is queued)"}, {"SELECT 1.5" and "SELECT 0.1", "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1", [name_resolve literal literal], "a decimal
        // literal: stops in the executor's descriptor batch validation (DECIMAL column profile)"}, {"SELECT TRUE", "DATATYPE.DESCRIPTOR.INVALID", [name_resolve], "a boolean literal
        // as a bare SELECT: the scalar-projection stopgap excludes boolean"} and {"SELECT 9223372036854775808", "DATATYPE.DESCRIPTOR.INVALID", [name_resolve], "an int64-overflow
        // literal: the SBXN proof cannot encode it under the int64 profile"}.
        // NEW (2026-12-24, owner rulings 2026-11-26 items 2, 3 and 5): ALL of these run natively (the `value_types` state of this executable: bare boolean projection, decimal
        // literals, int64-overflow promotion to the exact-decimal literal profile); they are no longer refusals and are pinned there as accepted statements.
        // OLD (2026-12-13): {"VALUES (-1)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "name_resolve(listed) literal#3(listed)", "a negative literal in VALUES: the unary-minus
        // expression is not the literal template"}. NEW (2026-12-15): `VALUES (-1)` runs natively (the fold makes it the literal template; see (A) in RouteB2State).
        {"VALUES ('a')", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) ",
         "a text literal in VALUES: no numeric or boolean literal profile exists for it, refused at the dispatch gate (the bare `SELECT 'a'` is a scalar projection with its own token, above)", "opcode_code=4615"},
        {"VALUES (NULL)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "engine.op.name_resolve#4865(listed) ",
         "a NULL in VALUES: not the literal template, refused at the dispatch gate", "opcode_code=4615"},
        // OLD (2026-12-13): {"VALUES (9223372036854775808)", "DATATYPE.DESCRIPTOR.INVALID", [name_resolve], "an int64-overflow literal in VALUES"} and {"VALUES (1.5)",
        // "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1", [name_resolve literal literal], "a decimal literal in VALUES"}. NEW (2026-12-24): both run natively (see `value_types`).
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

// ---- SB_SBMV_TYPED_UUID_FIELDS (2026-12-17; owner 2026-11-26 "Yes, native-only") ----------------------------------------------------------------------------
// The engine diagnostic's identity UUIDs (availability_table_uuid, database_uuid) reach the parser as SBMV v1 registered value type 5 (system_uuid) TLVs, native
// sessions only. State `typed_uuid` of this executable: (1) the real pipeline (RunPipeline over the embedded client and the real server handlers) hands the
// parser the EXACT 16 bytes the engine put; (2) the encoder writes them only for a negotiating peer and only for a registered (code, name); (3) the decoder
// refuses every malformed form with its own token; (4) a text TLV with a uuid-named field is still refused.
namespace tu {
namespace ipcns = scratchbird::parser::ipc;
using Bytes16 = std::array<std::uint8_t, 16>;
struct RawTlv { std::string name; std::uint16_t type; std::string value; std::uint32_t declared_len = 0xffffffffu; };
void PutLe(std::vector<std::uint8_t>* out, std::size_t at, std::uint64_t value, unsigned width) {
  if (out->size() < at + width) out->resize(at + width, 0);
  for (unsigned i = 0; i < width; ++i) (*out)[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void PutPadded(std::vector<std::uint8_t>* record, const std::string& text) {
  record->insert(record->end(), text.begin(), text.end());
  while (record->size() % 4 != 0) record->push_back(0);
}
// An SBMV v1 set with ONE record, built independently of the server's encoder (the decoder's own framing rules: sizes, CRCs, padding), carrying `tlvs` as its fields.
std::vector<std::uint8_t> BuildSbmv(const std::string& code, const Bytes16& request, const std::vector<RawTlv>& tlvs, std::uint16_t declared_field_count = 0xffff,
                                    std::uint16_t declared_detail_count = 0) {
  std::vector<std::uint8_t> record(112, 0);
  PutLe(&record, 8, 1, 2);
  record[10] = 0;
  record[11] = 2;
  const auto vector_uuid = srv::sbps::MakeUuidV7Bytes();
  std::copy(vector_uuid.begin(), vector_uuid.end(), record.begin() + 16);
  const auto occurrence = srv::sbps::MakeUuidV7Bytes();
  std::copy(occurrence.begin(), occurrence.end(), record.begin() + 32);
  std::copy(request.begin(), request.end(), record.begin() + 48);
  PutLe(&record, 88, 0x52565253u, 4);
  const std::string language = "en", message_key = "tu.key", safe = "typed uuid test";
  PutLe(&record, 92, language.size(), 2);
  PutLe(&record, 94, code.size(), 2);
  PutLe(&record, 96, message_key.size(), 2);
  PutLe(&record, 98, 0, 2);
  PutLe(&record, 100, safe.size(), 2);
  PutLe(&record, 102, declared_field_count == 0xffff ? tlvs.size() : declared_field_count, 2);
  PutLe(&record, 104, declared_detail_count, 2);
  PutPadded(&record, language);
  PutPadded(&record, code);
  PutPadded(&record, message_key);
  PutPadded(&record, std::string());
  PutPadded(&record, safe);
  for (const auto& tlv : tlvs) {
    const auto at = record.size();
    PutLe(&record, at, tlv.name.size(), 2);
    PutLe(&record, at + 2, tlv.type, 2);
    PutLe(&record, at + 4, tlv.declared_len != 0xffffffffu ? tlv.declared_len : tlv.value.size(), 4);
    record.insert(record.end(), tlv.name.begin(), tlv.name.end());
    record.insert(record.end(), tlv.value.begin(), tlv.value.end());
    while (record.size() % 4 != 0) record.push_back(0);
  }
  PutLe(&record, 0, record.size(), 4);
  PutLe(&record, 4, 0, 4);
  PutLe(&record, 4, srv::sbps::Crc32c(record.data(), record.size()), 4);
  std::vector<std::uint8_t> set(64, 0);
  PutLe(&set, 0, 0x564d4253u, 4);
  PutLe(&set, 4, 64, 2);
  PutLe(&set, 6, 1, 2);
  PutLe(&set, 12, 1, 4);
  PutLe(&set, 16, 64 + record.size(), 4);
  PutLe(&set, 20, srv::sbps::Crc32c(record.data(), record.size()), 4);
  PutLe(&set, 24, 1, 8);
  const auto set_uuid = srv::sbps::MakeUuidV7Bytes();
  std::copy(set_uuid.begin(), set_uuid.end(), set.begin() + 32);
  PutLe(&set, 48, 65536, 4);
  PutLe(&set, 52, 0, 4);
  PutLe(&set, 52, srv::sbps::Crc32c(set.data(), set.size()), 4);
  set.insert(set.end(), record.begin(), record.end());
  return set;
}
std::string Raw(const Bytes16& bytes) { return std::string(reinterpret_cast<const char*>(bytes.data()), 16); }
// A UUIDv7-shaped test identity (nothing in a catalog): version 7, RFC variant.
Bytes16 V7(std::uint8_t tag) {
  Bytes16 bytes{0x01, 0x9e, 0x5a, 0x11, 0x22, 0x33, 0x70, 0x44, 0x80, 0x55, tag, tag, tag, tag, tag, tag};
  return bytes;
}
struct Decoded { bool ok = false; std::string reason; ipcns::MessageVectorSet messages; };
Decoded Decode(const std::vector<std::uint8_t>& set, const Bytes16& request, bool negotiated) {
  Decoded out;
  out.ok = ipcns::DecodeTypedMessageVectorForTest(set, request, negotiated, &out.reason, &out.messages);
  return out;
}
std::vector<const ipcns::Field*> BinaryFields(const Decoded& d) {
  std::vector<const ipcns::Field*> found;
  for (const auto& diagnostic : d.messages.diagnostics)
    for (const auto& field : diagnostic.fields)
      if (field.kind == ipcns::FieldKind::binary_uuid) found.push_back(&field);
  return found;
}
// Everything random in an encoded set (the set and vector identities and the CRCs they feed) zeroed, so two encodings of the same diagnostics compare byte for byte.
std::vector<std::uint8_t> Normalised(std::vector<std::uint8_t> set) {
  const auto zero = [&](std::size_t at, std::size_t n) { for (std::size_t i = 0; i < n && at + i < set.size(); ++i) set[at + i] = 0; };
  zero(20, 4); zero(32, 16); zero(52, 4);
  std::size_t at = 64;
  while (at + 112 <= set.size()) {
    const auto size = static_cast<std::size_t>(set[at]) | (static_cast<std::size_t>(set[at + 1]) << 8) | (static_cast<std::size_t>(set[at + 2]) << 16) | (static_cast<std::size_t>(set[at + 3]) << 24);
    zero(at + 4, 4); zero(at + 16, 16);
    if (size < 112) break;
    at += size;
  }
  return set;
}
bool ContainsTypeFive(const std::vector<std::uint8_t>& set) {
  // walk the one-record sets this test builds: the field TLVs start after the five padded strings
  std::size_t at = 64;
  while (at + 112 <= set.size()) {
    const auto size = static_cast<std::size_t>(set[at]) | (static_cast<std::size_t>(set[at + 1]) << 8) | (static_cast<std::size_t>(set[at + 2]) << 16) | (static_cast<std::size_t>(set[at + 3]) << 24);
    if (size < 112 || at + size > set.size()) return false;
    const auto len = [&](std::size_t off) { return static_cast<std::size_t>(set[at + off]) | (static_cast<std::size_t>(set[at + off + 1]) << 8); };
    const auto padded = [](std::size_t n) { return (n + 3) & ~std::size_t(3); };
    std::size_t cursor = at + 112 + padded(len(92)) + padded(len(94)) + padded(len(96)) + padded(len(98)) + padded(len(100));
    for (std::size_t i = 0; i < len(102); ++i) {
      if (cursor + 8 > at + size) return false;
      const auto key = static_cast<std::size_t>(set[cursor]) | (static_cast<std::size_t>(set[cursor + 1]) << 8);
      const auto type = static_cast<std::size_t>(set[cursor + 2]) | (static_cast<std::size_t>(set[cursor + 3]) << 8);
      const auto value = static_cast<std::size_t>(set[cursor + 4]) | (static_cast<std::size_t>(set[cursor + 5]) << 8) | (static_cast<std::size_t>(set[cursor + 6]) << 16);
      if (type == 5) return true;
      cursor += 8 + padded(key + value) ;
    }
    at += size;
  }
  return false;
}
}  // namespace tu

void TypedUuidState() {
  (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
  using tu::Bytes16;
  namespace ipcns = scratchbird::parser::ipc;
  const Bytes16 table_uuid = api::NativeExecutorAvailabilityTableUuid().bytes;
  const Bytes16 database_uuid = served.facts->database_uuid.bytes;
  Check(table_uuid != database_uuid && tu::V7(1) != tu::V7(2), "(tu) the two engine identities are distinct (an exact-bytes comparison can tell them apart)");
  srv::ServerSessionRegistry registry;
  Pipe one(registry);
  const auto order1 = one.order();
  Check(order1 != 0, "(tu) the session's login began a transaction");
  const auto binary_of = [](const ps2::PipelineResult& result, const std::string& code) {
    std::vector<std::pair<std::string, std::string>> found;
    for (const auto& d : result.messages.diagnostics)
      if (d.code == code)
        for (const auto& f : d.fields)
          if (f.kind == ipcns::FieldKind::binary_uuid) found.emplace_back(f.name, f.value);
    return found;
  };
  const auto all_binary = [](const ps2::PipelineResult& result) {
    std::size_t count = 0;
    for (const auto& d : result.messages.diagnostics) for (const auto& f : d.fields) if (f.kind == ipcns::FieldKind::binary_uuid) ++count;
    return count;
  };

  // ---- (1) THE REAL PIPELINE: the parser receives the exact 16 bytes the engine put ----------------------------------------------------------------------
  // OLD (b906cae90): the refusals' identity fields (availability_table_uuid, database_uuid) were NOT forwarded: the engine dropped them before the snapshot and the
  // SBMV v1 text TLV refuses uuid-named fields. NEW: the dispatch-gate refusal carries them as type 5 TLVs to a native session; the parser's diagnostic holds them as
  // binary_uuid fields. `SELECT 1 + 0` is the operator-call row (opcode 1025); the aggregate over VALUES is the gate shape 4615.
  const std::vector<std::pair<std::string, std::string>> gate_shapes = {
      {"SELECT 1 + 0", "1025"},
      {"SELECT key_a, COUNT(*), SUM(amount) FROM (VALUES (1, 5), (1, 7)) AS input(key_a, amount) GROUP BY key_a", "4615"}};
  for (const auto& [sql, opcode] : gate_shapes) {
    const auto ran = RunSql(one, sql, 0);
    const auto found = binary_of(ran.result, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED");
    Check(!ran.result.accepted && FirstCode(ran.result) == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && FieldNamed(ran.result, "opcode_code") == opcode,
          "(tu) '" + sql + "' is refused at the dispatch gate with opcode_code=" + opcode + " (text field, unchanged): " + AllCodes(ran.result));
    Check(found.size() == 2 && found[0].first == "availability_table_uuid" && found[1].first == "database_uuid",
          "(tu) '" + sql + "': exactly two binary fields, availability_table_uuid then database_uuid: " + std::to_string(found.size()));
    Check(found.size() == 2 && found[0].second == tu::Raw(table_uuid),
          "(tu) '" + sql + "': availability_table_uuid is EXACTLY the engine's 16 table-identity bytes (NativeExecutorAvailabilityTableUuid)");
    Check(found.size() == 2 && found[1].second == tu::Raw(database_uuid),
          "(tu) '" + sql + "': database_uuid is EXACTLY the 16 bytes page zero gave the served database (served.facts->database_uuid)");
    // the public renderers drop a binary_uuid by design (owner Q194): neither the raw bytes, their hex nor their canonical text reach a rendering
    const auto json = ipcns::MessageVectorToJson(ran.result.messages);
    for (const auto& field : ran.result.messages.diagnostics.front().fields) {
      if (field.kind != ipcns::FieldKind::binary_uuid) continue;
      Check(json.find(ipcns::RenderFieldValue(field)) == std::string::npos && json.find(field.name) == std::string::npos,
            "(tu) the public rendering of '" + sql + "' does not show the identity field " + field.name);
    }
    // the server's own text view of the diagnostic (code{key=value}) holds no identity key: the text registry still refuses a uuid-named field
    Check(ran.server_diagnostics.find("availability_table_uuid") == std::string::npos && ran.server_diagnostics.find("database_uuid") == std::string::npos,
          "(tu) '" + sql + "': the server's text fields carry no uuid-named key: " + ran.server_diagnostics);
    Check(one.order() == order1, "(tu) the transaction is unchanged after '" + sql + "'");
  }
  // refusals whose engine diagnostic carries NO registered identity forward none; a parser-only refusal never reached the engine
  for (const std::string sql : {"SELECT 'a'", "VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(10),(11),(12),(13),(14),(15),(16),(17)", "SELECT * FROM (VALUES (1)) AS t(x, y)"}) {
    const auto ran = RunSql(one, sql, 0);
    Check(!ran.result.accepted && all_binary(ran.result) == 0, "(tu) '" + sql + "' is refused and carries no binary identity field: " + AllCodes(ran.result));
  }
  // an accepted statement has no diagnostics at all
  {
    const auto ran = RunSql(one, "SELECT 1", 0);
    Check(ran.result.accepted && all_binary(ran.result) == 0, "(tu) an accepted statement carries no identity field");
  }

  // ---- (2) THE SERVER'S REGISTRY AND THE ONE NEGOTIATION DECISION ------------------------------------------------------------------------------------------
  {
    using Id = scratchbird::server_engine_bridge::EngineDiagnosticIdentityField;
    const Bytes16 v4{0x01, 0x9e, 0x5a, 0x11, 0x22, 0x33, 0x40, 0x44, 0x80, 0x55, 1, 1, 1, 1, 1, 1};        // version 4
    const Bytes16 bad_variant{0x01, 0x9e, 0x5a, 0x11, 0x22, 0x33, 0x70, 0x44, 0xc0, 0x55, 1, 1, 1, 1, 1, 1};  // variant 11xx
    const std::vector<Id> candidates = {{"availability_table_uuid", table_uuid}, {"object_uuid", tu::V7(3)}, {"database_uuid", database_uuid},
                                        {"database_uuid", tu::V7(4)}, {"facts_database_uuid", tu::V7(5)}};
    const auto kept = srv::RegisteredEngineDiagnosticIdentityFieldsForTest("SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", candidates);
    Check(kept.size() == 2 && kept[0].first == "availability_table_uuid" && kept[0].second == table_uuid && kept[1].first == "database_uuid" && kept[1].second == database_uuid,
          "(tu) the registry keeps the two registered names for NATIVE_NOT_IMPLEMENTED in order, the first of a repeated name, drops unregistered names");
    const auto restricted = srv::RegisteredEngineDiagnosticIdentityFieldsForTest("ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY", candidates);
    Check(restricted.size() == 1 && restricted[0].first == "database_uuid" && restricted[0].second == database_uuid,
          "(tu) DATABASE_RESTRICTED_READ_ONLY may carry database_uuid only (availability_table_uuid is not registered for it)");
    for (const char* code : {"ENGINE.NATIVE.OPERATION_NOT_BUILT", "ENGINE.NATIVE.RESULT_ROW_BOUND_EXCEEDED", "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT", "ENGINE.STATEMENT_CONTEXT.NATIVE_DATABASE_FAILED", "SBLR.EXECUTOR.NATIVE_AVAILABILITY_IMMUTABLE"})
      Check(srv::RegisteredEngineDiagnosticIdentityFieldsForTest(code, candidates).empty(), std::string("(tu) ") + code + " has no registered identity field");
    const std::vector<Id> invalid = {{"database_uuid", Bytes16{}}, {"availability_table_uuid", v4}};
    Check(srv::RegisteredEngineDiagnosticIdentityFieldsForTest("SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", invalid).empty(), "(tu) a nil identity and a version-4 identity are dropped");
    Check(srv::RegisteredEngineDiagnosticIdentityFieldsForTest("SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", {{"database_uuid", bad_variant}}).empty(), "(tu) a non-RFC-variant identity is dropped");
    srv::ServerSessionRecord legacy;
    const auto found = registry.sessions_by_uuid.find(one.session().session_uuid);
    Check(found != registry.sessions_by_uuid.end() && found->second.native_session_approved, "(tu) the pipeline's session is a native session");
    Check(found != registry.sessions_by_uuid.end() && srv::PeerAcceptsTypedUuidDiagnosticFieldsForTest(found->second), "(tu) a native session negotiated typed UUID fields");
    Check(!legacy.native_session_approved && !srv::PeerAcceptsTypedUuidDiagnosticFieldsForTest(legacy), "(tu) a legacy session did NOT negotiate them");
    // OLD (to 2026-12-20): the capability was `native_session_approved` alone (receipt extension 75). NEW (owner 2026-12-21): the capability is the receipt extension
    // the session announced: 76 negotiates the typed UUID fields, a native session that announced 75 did NOT, and no other value does.
    Check(found != registry.sessions_by_uuid.end() && found->second.native_receipt_wire_extension == 76, "(tu) the pipeline's native session announced receipt extension 76");
    for (const auto& [extension, negotiates] : std::vector<std::pair<std::uint16_t, bool>>{{76, true}, {75, false}, {74, false}, {77, false}, {0, false}}) {
      srv::ServerSessionRecord native;
      native.native_session_approved = true;
      native.native_receipt_wire_extension = extension;
      Check(srv::PeerAcceptsTypedUuidDiagnosticFieldsForTest(native) == negotiates,
            "(tu) a native session that announced extension " + std::to_string(extension) + (negotiates ? " negotiated" : " did NOT negotiate") + " typed UUID fields");
    }
    {
      // a legacy session never negotiates, whatever its (unused) extension field says
      srv::ServerSessionRecord legacy_with_76;
      legacy_with_76.native_receipt_wire_extension = 76;
      Check(!srv::PeerAcceptsTypedUuidDiagnosticFieldsForTest(legacy_with_76), "(tu) a non-native session never negotiates typed UUID fields, even with extension 76 recorded");
    }
    // The real pipeline under a session that announced 75: the receipt the parser decodes is the 75 variant (it did not negotiate) and the server sends NO type 5, so
    // the refusal is the same specific refusal with its text field and carries no binary field; with 76 restored the two identity fields return.
    if (found != registry.sessions_by_uuid.end()) {
      found->second.native_receipt_wire_extension = 75;
      const auto ran75 = RunSql(one, "SELECT 1 + 0", 0);
      Check(!ran75.result.accepted && FirstCode(ran75.result) == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && FieldNamed(ran75.result, "opcode_code") == "1025" &&
                all_binary(ran75.result) == 0,
            "(tu) under a session that announced extension 75 the gate refusal arrives without any binary identity field: " + AllCodes(ran75.result));
      found->second.native_receipt_wire_extension = 76;
      const auto ran76 = RunSql(one, "SELECT 1 + 0", 0);
      Check(!ran76.result.accepted && binary_of(ran76.result, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED").size() == 2,
            "(tu) with extension 76 restored the same refusal carries the two binary identity fields again: " + AllCodes(ran76.result));
    }
  }

  // ---- (3) THE ENCODER: typed fields only to a negotiating peer, only registered, never text -------------------------------------------------------------------
  {
    const Bytes16 request = tu::V7(9);
    srv::ServerDiagnostic base;
    base.code = "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED";
    base.message_key = "sblr.executor.native_not_implemented";
    base.safe_message = "refused";
    base.fields.push_back({"opcode_code", "1025"});
    auto native = base;
    native.typed_uuid_fields = {{"availability_table_uuid", table_uuid}, {"database_uuid", database_uuid}};
    const auto plain_bytes = tu::Normalised(srv::sbps::EncodeMessageVectorSet({base}, request));
    // a legacy peer: the two-argument overload and the flag-false overload are BYTE-IDENTICAL to a diagnostic that carries no typed field
    const auto legacy_two = srv::sbps::EncodeMessageVectorSet({native}, request);
    const auto legacy_three = srv::sbps::EncodeMessageVectorSet({native}, request, false);
    Check(tu::Normalised(legacy_two) == plain_bytes && tu::Normalised(legacy_three) == plain_bytes,
          "(tu) a diagnostic WITH typed fields encoded for a non-negotiating peer is byte-identical (identities, CRCs normalised) to the same diagnostic without them");
    Check(!tu::ContainsTypeFive(legacy_two) && !tu::ContainsTypeFive(legacy_three), "(tu) a non-negotiating peer's set holds no type 5 TLV");
    const auto negotiated = srv::sbps::EncodeMessageVectorSet({native}, request, true);
    Check(tu::ContainsTypeFive(negotiated), "(tu) a negotiating peer's set holds a type 5 TLV");
    Check(negotiated.size() == legacy_two.size() + 2 * (8 + 24 + 16) + 0 || negotiated.size() > legacy_two.size(), "(tu) and is longer by the two TLVs");
    const auto got = tu::Decode(negotiated, request, true);
    const auto fields = tu::BinaryFields(got);
    Check(got.ok && fields.size() == 2 && fields[0]->name == "availability_table_uuid" && fields[0]->value == tu::Raw(table_uuid) && fields[1]->name == "database_uuid" &&
              fields[1]->value == tu::Raw(database_uuid),
          "(tu) the decoder gives the encoder's exact bytes back, in order, as binary_uuid fields: " + got.reason);
    Check(got.ok && !got.messages.diagnostics.empty() && got.messages.diagnostics.front().code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && [&] {
      for (const auto& f : got.messages.diagnostics.front().fields) if (f.name == "opcode_code" && f.kind == ipcns::FieldKind::text && f.value == "1025") return true;
      return false; }(), "(tu) the text field opcode_code rides beside them, unchanged");
    const auto refused_unnegotiated = tu::Decode(negotiated, request, false);
    Check(!refused_unnegotiated.ok && refused_unnegotiated.reason == "typed_uuid_not_negotiated", "(tu) a decoder that did not negotiate refuses a typed field: " + refused_unnegotiated.reason);
    Check(tu::Decode(legacy_two, request, false).ok && tu::Decode(legacy_two, request, true).ok, "(tu) the legacy bytes decode with either decoder");
    // registry bypass attempts: every one is DROPPED by the encoder (the set equals the plain set), never written, never converted to text
    const auto dropped = [&](srv::ServerDiagnostic diagnostic, const std::string& what) {
      Check(tu::Normalised(srv::sbps::EncodeMessageVectorSet({diagnostic}, request, true)) == tu::Normalised(srv::sbps::EncodeMessageVectorSet({[&] { auto d = diagnostic; d.typed_uuid_fields.clear(); return d; }()}, request, true)) &&
                !tu::ContainsTypeFive(srv::sbps::EncodeMessageVectorSet({diagnostic}, request, true)),
            "(tu) the encoder drops " + what);
    };
    { auto d = base; d.typed_uuid_fields = {{"object_uuid", database_uuid}}; dropped(d, "an unregistered name"); }
    { auto d = base; d.code = "ENGINE.NATIVE.OPERATION_NOT_BUILT"; d.typed_uuid_fields = {{"database_uuid", database_uuid}}; dropped(d, "a registered name on a code that does not register it"); }
    { auto d = base; d.code = "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY"; d.typed_uuid_fields = {{"availability_table_uuid", table_uuid}}; dropped(d, "availability_table_uuid on a code that registers only database_uuid"); }
    { auto d = base; d.typed_uuid_fields = {{"database_uuid", Bytes16{}}}; dropped(d, "a nil identity"); }
    { auto d = base; d.typed_uuid_fields = {{"database_uuid", Bytes16{0x01, 0x9e, 0x5a, 0x11, 0x22, 0x33, 0x40, 0x44, 0x80, 0x55, 1, 1, 1, 1, 1, 1}}}; dropped(d, "a version-4 identity"); }
    {
      auto d = base;
      d.typed_uuid_fields = {{"database_uuid", tu::V7(7)}, {"database_uuid", tu::V7(8)}};
      const auto set = srv::sbps::EncodeMessageVectorSet({d}, request, true);
      const auto got_dup = tu::Decode(set, request, true);
      const auto dup_fields = tu::BinaryFields(got_dup);
      Check(got_dup.ok && dup_fields.size() == 1 && dup_fields[0]->value == tu::Raw(tu::V7(7)), "(tu) a repeated name is written once, the first");
    }
    // a uuid-named TEXT field is still refused by the encoder's public filter (not converted to a typed field)
    {
      auto d = base;
      d.fields.push_back({"database_uuid", "not_a_uuid_but_named_one"});
      const auto set = srv::sbps::EncodeMessageVectorSet({d}, request, true);
      const auto got_text = tu::Decode(set, request, true);
      bool text_named = false;
      for (const auto& diagnostic : got_text.messages.diagnostics) for (const auto& f : diagnostic.fields) if (f.name == "database_uuid") text_named = true;
      Check(got_text.ok && !text_named && tu::BinaryFields(got_text).empty(), "(tu) a text field named database_uuid is dropped by the encoder (still refused), no typed field appears");
    }
  }

  // ---- (4) THE DECODER: malformed forms, each refused with its own token -----------------------------------------------------------------------------------
  {
    const Bytes16 request = tu::V7(10);
    const std::string code = "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED";
    const std::string good = tu::Raw(table_uuid);
    const auto run = [&](const std::string& with_code, std::vector<tu::RawTlv> tlvs, bool negotiated, std::uint16_t fields = 0xffff, std::uint16_t details = 0) {
      return tu::Decode(tu::BuildSbmv(with_code, request, tlvs, fields, details), request, negotiated);
    };
    {
      const auto ok = run(code, {{"opcode_code", 1, "1025"}, {"availability_table_uuid", 5, good}}, true);
      const auto fields = tu::BinaryFields(ok);
      Check(ok.ok && fields.size() == 1 && fields[0]->value == good, "(tu) control: an independently built set with one valid typed field decodes to its exact bytes");
    }
    struct Case { const char* what; std::vector<tu::RawTlv> tlvs; const char* reason; };
    const std::vector<Case> cases = {
        {"15 bytes", {{"availability_table_uuid", 5, good.substr(0, 15)}}, "typed_uuid_length_invalid"},
        {"17 bytes", {{"availability_table_uuid", 5, good + "x"}}, "typed_uuid_length_invalid"},
        {"0 bytes", {{"availability_table_uuid", 5, ""}}, "typed_uuid_length_invalid"},
        {"an unregistered name", {{"object_uuid", 5, good}}, "typed_uuid_field_not_registered"},
        {"another unregistered name", {{"sessions_uuid", 5, good}}, "typed_uuid_field_not_registered"},
        {"a repeated name", {{"availability_table_uuid", 5, good}, {"availability_table_uuid", 5, good}}, "typed_uuid_field_duplicate"},
        {"a nil identity", {{"database_uuid", 5, std::string(16, '\0')}}, "typed_uuid_value_invalid"},
        {"a version-4 identity", {{"database_uuid", 5, tu::Raw(Bytes16{0x01, 0x9e, 0x5a, 0x11, 0x22, 0x33, 0x40, 0x44, 0x80, 0x55, 1, 1, 1, 1, 1, 1})}}, "typed_uuid_value_invalid"},
        {"a wrong-variant identity", {{"database_uuid", 5, tu::Raw(Bytes16{0x01, 0x9e, 0x5a, 0x11, 0x22, 0x33, 0x70, 0x44, 0xc0, 0x55, 1, 1, 1, 1, 1, 1})}}, "typed_uuid_value_invalid"},
        {"a truncated value (declared 16, 4 present)", {{"database_uuid", 5, "abcd", 16}}, "typed_uuid_truncated"},
        {"a text TLV carrying a uuid-named field", {{"database_uuid", 1, "abc"}}, "text_field_not_public"},
        {"a text TLV carrying a uuid-named field with canonical UUID text", {{"database_uuid", 1, "019e5a00-0000-7000-8000-0000000000aa"}}, "text_field_not_public"},
        {"a text TLV carrying a canonical UUID value under another name", {{"owner", 1, "019e5a00-0000-7000-8000-0000000000aa"}}, "text_field_not_public"},
    };
    for (const auto& c : cases) {
      const auto got = run(code, c.tlvs, true);
      Check(!got.ok && got.reason == c.reason && got.messages.diagnostics.empty(), std::string("(tu) ") + c.what + " is refused with " + c.reason + ": ok=" + std::to_string(got.ok) + " reason=" + got.reason);
    }
    {
      const auto got = run("ENGINE.NATIVE.OPERATION_NOT_BUILT", {{"database_uuid", 5, good}}, true);
      Check(!got.ok && got.reason == "typed_uuid_field_not_registered", "(tu) database_uuid on ENGINE.NATIVE.OPERATION_NOT_BUILT (registered for other codes only) is refused: " + got.reason);
      const auto got2 = run("ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY", {{"availability_table_uuid", 5, good}}, true);
      Check(!got2.ok && got2.reason == "typed_uuid_field_not_registered", "(tu) availability_table_uuid on DATABASE_RESTRICTED_READ_ONLY is refused: " + got2.reason);
      const auto got3 = run("ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY", {{"database_uuid", 5, good}}, true);
      Check(got3.ok && tu::BinaryFields(got3).size() == 1, "(tu) database_uuid on DATABASE_RESTRICTED_READ_ONLY is accepted");
    }
    {
      const auto got = run(code, {{"database_uuid", 5, good}}, false);
      Check(!got.ok && got.reason == "typed_uuid_not_negotiated", "(tu) a typed field to a decoder that did not negotiate is refused: " + got.reason);
      const auto in_details = run(code, {{"database_uuid", 5, good}}, true, 0, 1);
      Check(!in_details.ok && in_details.reason == "typed_uuid_wrong_section", "(tu) a typed field in the details section (not the field section) is refused: " + in_details.reason);
      const auto unknown_type = run(code, {{"opcode_code", 2, std::string(1, '\x01')}}, true);
      Check(!unknown_type.ok && unknown_type.reason.empty(), "(tu) any other type (2 = boolean) is still refused exactly as before: " + unknown_type.reason);
    }
  }
  Check(one.order() == order1 && off::ReadFile(served.path, served.facts->page_size_bytes).words.at(order1).state == pgs::TipStateCode::active, "(tu) the transaction is still active at the end");
  one.End();
}

// SB_PGP_REFUSAL_NATIVE_ROUTE (2026-12-20; owner 2026-11-26 PGP 2 a / PGP 3 a): what the real pipeline does with the four pgp_* functions and the pgcrypto spelling on a
// production-created native single-file database. FINDING (this state pins it): NO scalar function call reaches the engine on a native database yet. The parser
// lowers `pgp_*(...)` to the function-call operation (opcode 1024), the native dispatch gate does not list that row, and the call is refused with
// SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED (opcode_code=1024) BEFORE any engine function (so before the engine's own CRYPTO.PGP.PROFILE_UNAVAILABLE refusal, which the in-process
// parser-to-engine route test sbsql_pgp_refusal_route_conformance proves). A native-database proof of the engine's refusal is therefore not possible until the
// native executor lists the function-call row (native scalar function execution: not built here). `pgcrypto()` never leaves the parser (no scalar route).
// The sentinels below are distinctive plaintext/key literals: neither the raw text, its base64 nor its hex may appear in any diagnostic code, field or detail.
void PgpRefusalState() {
  (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
  srv::ServerSessionRegistry registry;
  Pipe one(registry);
  const auto order1 = one.order();
  Check(order1 != 0, "(pgp) the session's login began a transaction");
  const std::string plain = "SENTINEL-PLAIN-5e1d", key = "SENTINEL-KEY-9b27";
  const auto b64 = [](const std::string& in) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
      const std::uint32_t a = static_cast<unsigned char>(in[i]), b2 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0, c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
      const std::uint32_t t = (a << 16) | (b2 << 8) | c;
      out += kAlphabet[(t >> 18) & 63]; out += kAlphabet[(t >> 12) & 63];
      out += i + 1 < in.size() ? kAlphabet[(t >> 6) & 63] : '='; out += i + 2 < in.size() ? kAlphabet[t & 63] : '=';
    }
    return out;
  };
  const auto hex = [](const std::string& in) {
    std::string out;
    for (const unsigned char ch : in) { out += "0123456789abcdef"[ch >> 4]; out += "0123456789abcdef"[ch & 15]; }
    return out;
  };
  struct Shape { std::string sql; };
  const std::vector<Shape> shapes = {
      {"SELECT pgp_sym_encrypt('" + plain + "','" + key + "')"},
      {"SELECT pgp_sym_decrypt(X'" + hex(plain) + "','" + key + "')"},
      {"SELECT pgp_pub_encrypt(X'" + hex(plain) + "',X'" + hex(key) + "')"},
      {"SELECT pgp_pub_decrypt(X'" + hex(plain) + "',X'" + hex(key) + "')"},
      // an old SBPGP1 envelope as the message: it is not accepted anywhere, it never reaches a decryptor
      {"SELECT pgp_sym_decrypt(X'" + hex("SBPGP1:sym:" + b64(key) + ":" + b64(plain)) + "','" + key + "')"},
  };
  const FileEvidence group_before;
  for (const auto& shape : shapes) {
    const auto ran = RunSql(one, shape.sql, 2);
    std::string asked;
    for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
    const std::string label = "(pgp) '" + shape.sql.substr(0, 32) + "...'";
    Check(!ran.result.accepted && FirstCode(ran.result) == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED",
          label + " is refused at the native dispatch gate, before any engine function: " + AllCodes(ran.result));
    Check(FieldNamed(ran.result, "opcode_code") == "1024", label + " carries opcode_code=1024 (the function-call row): " + FieldNamed(ran.result, "opcode_code"));
    Check(asked == "engine.op.name_resolve#4865(listed) engine.op.function_call#1024(NOT listed) ",
          label + ": the production table was asked name-resolve (listed) then function-call 1024 (NOT listed), got [" + asked + "]");
    Check(ran.result.server_result_payload.empty(), label + " returns no result payload");
    const std::string everything = AllCodes(ran.result) + ran.server_diagnostics + ran.raw_vector + ran.result.server_result_payload;
    for (const auto& needle : {plain, key, b64(plain), b64(key), hex(plain), hex(key)})
      Check(everything.find(needle) == std::string::npos, label + ": no plaintext/key material (raw, base64 or hex form `" + needle.substr(0, 12) + "...`) in any code, field, detail or payload");
    Check(everything.find("SBPGP1") == std::string::npos, label + ": the retired SBPGP1 envelope is nowhere in the response");
    Check(one.order() == order1, label + ": the transaction is unchanged");
  }
  CheckUntouched(group_before, "the pgp_* statements on a native database");
  {
    // OLD (to 2026-12-20): `SELECT pgcrypto()` was refused by the PARSER with the generic SBSQL.QUERY.PROJECTION_INVALID (no scalar route) and the test asserted the user
    // did NOT see CRYPTO.PACKAGE.NOT_CALLABLE. NEW (owner 2026-12-21, lowering.cpp SB_PGCRYPTO_PACKAGE_PROBE): the parser answers the call spelling as the package capability
    // probe the spec describes, with the engine's own code CRYPTO.PACKAGE.NOT_CALLABLE and the function name as the data field `function`; the engine is still never asked, so the
    // answer is the same on a native database as anywhere (a lowered call would have stopped at the opcode-1024 gate with NATIVE_NOT_IMPLEMENTED, which says "not built"
    // about something that is never callable). Every argument shape gives the same specific outcome.
    for (const std::string sql : {"SELECT pgcrypto()", "SELECT pgcrypto('abc')", "SELECT pgcrypto(NULL)", "SELECT PGCRYPTO(1, 2)", "SELECT upper(pgcrypto())"}) {
      const auto ran = RunSql(one, sql, 2);
      Check(!ran.result.accepted && FirstCode(ran.result) == "CRYPTO.PACKAGE.NOT_CALLABLE" && ran.recorded.empty() && ran.server_diagnostics.empty(),
            "(pgp) `" + sql + "` is answered by the PARSER with CRYPTO.PACKAGE.NOT_CALLABLE (the capability probe; the engine is never asked): " + AllCodes(ran.result));
      Check(FieldNamed(ran.result, "function") == "pgcrypto", "(pgp) `" + sql + "` carries the function name as the data field `function`: " + FieldNamed(ran.result, "function"));
      Check(AllCodes(ran.result).find("SBSQL.QUERY.PROJECTION_INVALID") == std::string::npos && AllCodes(ran.result).find("NATIVE_NOT_IMPLEMENTED") == std::string::npos,
            "(pgp) `" + sql + "`: neither the generic projection refusal nor the native not-built refusal reaches the user: " + AllCodes(ran.result));
      Check(one.order() == order1, "(pgp) `" + sql + "`: the transaction is unchanged");
    }
  }
  // the session is still usable after every refusal
  {
    const auto ran = RunSql(one, "SELECT 1", 0);
    Check(ran.result.accepted && !ran.result.messages.has_errors(), "(pgp) the session still runs SELECT 1 after the refusals: " + AllCodes(ran.result));
  }
  one.End();
}

// ============================================================================================================================================================
// SB_NATIVE_VALUE_TYPES (2026-12-24; owner rulings 2026-11-26 items 2, 3 and 5): the `value_types` state. A bare `SELECT TRUE` / `SELECT FALSE` is the literal VALUES
// relation (like the numeric bare projection), exact-decimal literals run natively (the vector carries the cohort's own 24-byte decimal codec body as bytes plus the
// decimal cohort identity; the parser decodes it with the engine's codec and renders it), and an integer literal that does not fit int64 is the exact-decimal
// literal profile's (INT64_MAX stays int64, INT64_MIN stays int64). Every accepted statement is checked for: the parser's client row and its column metadata, the
// engine's ONE vector (per-column cohort identity and the exact value bytes, no name, no rendered text), the recorder's three rows, and (per group) a byte-identical
// file / unchanged directory / unchanged Next / transaction still active and owned.
// ============================================================================================================================================================
CohortInt64 CohortDecimalOf(const api::EngineRequestContext& context) {
  CohortInt64 out;
  int found = 0;
  for (const auto& row3 : scratchbird::core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = row3.legacy_fields;
    if (row.codec_id == "datatype.decimal.base1e9.le.v1" && row.canonical_name == "decimal" && row.catalog_snapshot_uuid == context.datatype_catalog_snapshot_uuid &&
        row.catalog_generation == context.datatype_catalog_generation && row.registry_generation == context.datatype_registry_generation) {
      out = {row.type_uuid, row.descriptor_uuid, row.descriptor_generation};
      ++found;
    }
  }
  Check(found == 1, "exactly one exact-decimal identity in the receipt's datatype cohort (" + std::to_string(found) + ")");
  return out;
}
// One expected cell. 'i' int64 (shown = the decimal digits), 'b' boolean (shown true / false), 'd' exact decimal: the lexical the oracle encodes with the engine's own
// codec, the parser's expected display, and the header constants written out here (byte 0 = sign bit | scale, byte 1 = precision).
struct Cell {
  char kind;
  std::string shown;
  std::string lexical;
  unsigned sign_scale = 0;
  unsigned precision = 0;
  // SB_NATIVE_VALUES_DECIMAL_SCALE (2026-12-25): the column's DECLARED decimal precision and scale as the engine's vector carries them (0, 0 for int64 and boolean)
  unsigned column_precision = 0;
  unsigned column_scale = 0;
};
Cell I(std::int64_t value) { return {'i', std::to_string(value), "", 0, 0}; }
Cell B(bool value) { return {'b', value ? "true" : "false", "", 0, 0}; }
// D(lexical, shown, body byte 0 (sign bit | scale), body byte 1 (precision), the column's declared precision, the column's declared scale). OLD (before 2026-12-25) D had four arguments:
// the shown text was the codec's canonical spelling (0.10 showed 0.1); NEW: the parser pads the fraction to the COLUMN's declared scale (0.10 shows 0.10).
Cell D(std::string lexical, std::string shown, unsigned sign_scale, unsigned precision, unsigned column_precision, unsigned column_scale) {
  return {'d', std::move(shown), std::move(lexical), sign_scale, precision, column_precision, column_scale};
}

void CheckAcceptedCells(const Ran& ran, const std::string& sql, const std::vector<std::vector<Cell>>& expected, const CohortInt64& int64_cohort, const CohortInt64& boolean_cohort,
                        const CohortInt64& decimal_cohort, const std::vector<std::string>& names = {}) {
  namespace pr = scratchbird::wire::public_result;
  const auto& r = ran.result;
  const std::string label = "'" + sql.substr(0, 60) + "'";
  Check(r.accepted && !r.messages.has_errors(), label + " is accepted through the real pipeline: " + AllCodes(r));
  Check(r.server_operation_id == "query.execute", label + ": the engine operation is query.execute: " + r.server_operation_id);
  const std::size_t rows = expected.size(), columns = expected.front().size();
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> got;
  Check(ps2::DecodePublicResultRowsForTest(r.server_result_payload, &oids, &got) && got.size() == rows && oids.size() == columns,
        label + ": the parser-built table has " + std::to_string(rows) + " rows and " + std::to_string(columns) + " columns");
  for (std::size_t row = 0; row < rows && row < got.size(); ++row)
    for (std::size_t column = 0; column < columns && column < got[row].size(); ++column)
      Check(got[row][column] && *got[row][column] == expected[row][column].shown,
            label + ": parser row " + std::to_string(row) + " column " + std::to_string(column) + " displays `" + expected[row][column].shown + "`");
  std::string meta;
  for (std::size_t column = 0; column < columns; ++column) {
    const char kind = expected.front()[column].kind;
    // names: the parser's display names (an alias as written, else column<N>); the engine's vector never carries them
    meta += (column ? ";" : "") + (column < names.size() ? names[column] : std::string("column") + std::to_string(column + 1)) +
            (kind == 'b' ? ":boolean:not_null" : kind == 'd' ? ":decimal:not_null" : ":int64:not_null");
  }
  Check(r.server_result_payload.ends_with(meta), label + ": the parser's row metadata is " + meta);
  std::vector<pr::Field> fields;
  Check(pr::Decode(ran.raw_vector, &fields), label + ": the engine's vector decodes");
  const auto find = [&](const std::string& name) -> const pr::Field* {
    const pr::Field* found = nullptr;
    for (const auto& f : fields) if (f.name == name) { if (found) return nullptr; found = &f; }
    return found;
  };
  // OLD (before 2026-12-25): 4 + 4 * columns + columns * rows. NEW: SIX fields per column (column_precision and column_scale follow the generation)
  // OLD (before 2026-12-26): 4 + 6 * columns + columns * rows. NEW: SEVEN fields per column (column_nullability; 0 here: these cells hold no NULL)
  Check(fields.size() == 4 + 7 * columns + columns * rows, label + ": the vector holds exactly the header, seven fields per column and one value per cell (no name, no text): " + std::to_string(fields.size()));
  for (std::size_t column = 0; column < columns; ++column) {
    const auto* cnull = find("column_nullability[" + std::to_string(column) + "]");
    Check(cnull && cnull->kind == pr::Kind::unsigned_integer && cnull->value.size() == 8 && LE64(cnull->value) == 0, label + ": column " + std::to_string(column) + " declares not-null (column_nullability 0)");
  }
  const auto* kind_field = find("result_kind");
  const auto* rows_field = find("row_count");
  const auto* columns_field = find("column_count");
  Check(kind_field && kind_field->value == "native_scalar_projection_vector" && rows_field && LE64(rows_field->value) == rows && columns_field && LE64(columns_field->value) == columns,
        label + ": vector result_kind, row_count " + std::to_string(rows) + ", column_count " + std::to_string(columns));
  for (std::size_t column = 0; column < columns; ++column) {
    const char kind = expected.front()[column].kind;
    const auto& cohort = kind == 'b' ? boolean_cohort : kind == 'd' ? decimal_cohort : int64_cohort;
    const auto n = std::to_string(column);
    const auto* t = find("column_datatype[" + n + "]");
    const auto* td = find("column_datatype_descriptor[" + n + "]");
    const auto* g = find("column_datatype_generation[" + n + "]");
    Check(t && td && g && UuidOf(t->value) == cohort.type_uuid && UuidOf(td->value) == cohort.descriptor_uuid && LE64(g->value) == cohort.generation,
          label + ": column " + n + " carries the cohort's " + (kind == 'b' ? "BOOLEAN" : kind == 'd' ? "exact-DECIMAL" : "int64") + " identity (type, descriptor, generation)");
    const auto* cp = find("column_precision[" + n + "]");
    const auto* cs = find("column_scale[" + n + "]");
    Check(cp && cs && cp->kind == pr::Kind::unsigned_integer && cs->kind == pr::Kind::unsigned_integer && LE64(cp->value) == expected.front()[column].column_precision &&
              LE64(cs->value) == expected.front()[column].column_scale,
          label + ": column " + n + " declares precision " + std::to_string(expected.front()[column].column_precision) + " and scale " + std::to_string(expected.front()[column].column_scale) +
              " in the vector (constants written in the test)");
  }
  for (std::size_t row = 0; row < rows; ++row)
    for (std::size_t column = 0; column < columns; ++column) {
      const auto& cell = expected[row][column];
      const auto at = "value[" + std::to_string(row) + "." + std::to_string(column) + "]";
      const auto* v = find(at);
      if (cell.kind == 'b') {
        Check(v && v->kind == pr::Kind::bytes && v->value.size() == 1 && (v->value[0] == '\1') == (cell.shown == "true") && (v->value[0] == '\0' || v->value[0] == '\1'),
              label + ": " + at + " is ONE byte, 0 or 1 (a boolean, never text)");
      } else if (cell.kind == 'd') {
        const auto oracle = scratchbird::engine::sblr::EncodeSblrLiteralExactDecimalV1(cell.lexical);
        Check(oracle.ok && v && v->kind == pr::Kind::bytes && v->value.size() == 24 && v->value == std::string(reinterpret_cast<const char*>(oracle.canonical_bytes.data()), 24),
              label + ": " + at + " is the engine decimal codec's 24 bytes for " + cell.lexical + " (bytes kind, never text)");
        Check(v && v->value.size() == 24 && static_cast<unsigned>(static_cast<unsigned char>(v->value[0])) == cell.sign_scale && static_cast<unsigned>(static_cast<unsigned char>(v->value[1])) == cell.precision,
              label + ": " + at + " header byte 0 (sign bit | scale) is " + std::to_string(cell.sign_scale) + " and byte 1 (precision) is " + std::to_string(cell.precision) + " (constants, not the codec's)");
      } else {
        Check(v && v->kind == pr::Kind::signed_integer && v->value.size() == 8 && std::to_string(static_cast<std::int64_t>(LE64(v->value))) == cell.shown,
              label + ": " + at + " is the int64 " + cell.shown);
      }
    }
  Check(ran.raw_vector.find("true") == std::string::npos && ran.raw_vector.find("false") == std::string::npos && ran.raw_vector.find("column1") == std::string::npos &&
            ran.raw_vector.find("key_a") == std::string::npos,
        label + ": the engine's vector carries no rendered boolean, no rendered decimal digits as text and no column name");
  (void)int64_cohort;
}

void ValueTypesState() {
  (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto catalog_generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  auto direct = Approve();
  auto direct_public = OpenPublicFor(*direct);
  auto direct_context = ContextFor(*direct, catalog_generation);
  auto direct_receipt = Acquire(*direct_public, direct_context, direct->approval.transaction_uuid);
  Check(direct_receipt.ok(), "a direct native receipt (for the cohort identities): " + direct_receipt.code);
  const auto int64_cohort = CohortInt64Of(CopyContext(direct_receipt));
  const auto boolean_cohort = CohortBooleanOf(CopyContext(direct_receipt));
  const auto decimal_cohort = CohortDecimalOf(CopyContext(direct_receipt));
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
  const std::string kBindOnly = "engine.op.name_resolve#4865(listed) ";
  struct Accepted { std::string sql; std::vector<std::vector<Cell>> rows; std::vector<std::string> names = {}; };
  const auto run_accepted = [&](const std::string& group, const std::vector<Accepted>& list) {
    const FileEvidence group_before;  // evidence per group (see (a2) of the route_b state: per statement took minutes at 128k)
    for (const auto& item : list) {
      const auto ran = RunSql(one, item.sql, 2);
      CheckAcceptedCells(ran, item.sql, item.rows, int64_cohort, boolean_cohort, decimal_cohort, item.names);
      Check(asked_of(ran) == kThree, "(" + group + ") '" + item.sql + "' asks exactly [name_resolve (bind), literal (finalize), literal (execution)], all listed: " + asked_of(ran));
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after '" + item.sql + "'");
    }
    CheckUntouched(group_before, "the (" + group + ") statements, every one");
    Check(active(order1), "(e) the transaction is still active after the (" + group + ") statements");
  };
  struct Refused { std::string sql; std::string code; std::string asked; std::string engine_fields; std::string what; };
  const auto run_refused = [&](const std::string& group, const std::vector<Refused>& list) {
    const FileEvidence group_before;
    for (const auto& item : list) {
      const auto ran = RunSql(one, item.sql, 2);
      bool present = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == item.code) present = true;
      Check(!ran.result.accepted && present, "(" + group + ") '" + item.sql + "' (" + item.what + ") is refused with " + item.code + ": " + AllCodes(ran.result));
      Check(asked_of(ran) == item.asked, "(" + group + ") '" + item.sql + "': the production table was asked exactly [" + item.asked + "], got [" + asked_of(ran) + "]");
      CheckEngineFields(ran, item.sql, item.engine_fields, false);
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after the refused '" + item.sql + "'");
    }
    CheckUntouched(group_before, "the (" + group + ") refusals, every one");
    Check(active(order1), "(e) the transaction is still active after the (" + group + ") refusals");
  };

  // ---- (1) BARE BOOLEAN PROJECTION. OLD (3916b9e35, pinned in route_b (d2)): `SELECT TRUE` -> DATATYPE.DESCRIPTOR.INVALID (bound literal occurrence inventory changed: a
  // scalar projection, the S4h stopgap excludes boolean on purpose). NEW (owner 2026-11-26 item 5): the bare projection of boolean literals is the SAME typed VALUES
  // relation the numeric bare projection is; one boolean column named column1 printing true / false; mixed columns in ONE row are fine.
  run_accepted("1", {
      {"SELECT TRUE", {{B(true)}}}, {"SELECT FALSE", {{B(false)}}}, {"select true", {{B(true)}}}, {"Select False", {{B(false)}}}, {"SELECT TRUE;", {{B(true)}}},
      {"SELECT TRUE, FALSE", {{B(true), B(false)}}}, {"SELECT TRUE, 1", {{B(true), I(1)}}}, {"SELECT 1, TRUE", {{I(1), B(true)}}},
      {"SELECT 1, TRUE, -2", {{I(1), B(true), I(-2)}}}, {"select false , true ;", {{B(false), B(true)}}},
  });
  // OLD (part 1, 2026-12-24): `SELECT NOT TRUE` and `SELECT TRUE AND FALSE` stopped at the parser prebind with DATATYPE.DESCRIPTOR.INVALID (bound literal occurrence inventory changed:
  // the S4h stopgap excluded every statement holding a boolean literal). NEW (owner 2026-11-28 Q4): a boolean literal under an OPERATOR rides the operands like a numeric one, so the
  // statement reaches the engine's operator refusal (the same row `SELECT 1 + 0` meets). The exclusion stays for a statement whose expressions are ALL literals. Operators still do
  // NOT execute: the engine is refused on the operator-call row (opcode 1025, the real opcode read from the engine's forwarded field, never typed in as an assumption).
  // A mixed COLUMN across rows is refused by the lowering.
  const std::string kOperator = "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ";
  run_refused("1r", {
      {"SELECT NOT TRUE", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kOperator, "opcode_code=1025", "a boolean expression over a literal reaches the engine's operator refusal"},
      {"SELECT TRUE AND FALSE", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kOperator, "opcode_code=1025", "a boolean operator over literals"},
      {"SELECT 1 < 2", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kOperator, "opcode_code=1025", "a comparison over literals"},
      {"SELECT 1 + 1", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kOperator, "opcode_code=1025", "an arithmetic expression over literals keeps its refusal (unchanged)"},
      // an all-literal boolean statement that is NOT the bounded `[AS] alias` form keeps the S4h stopgap's stop (the exclusion is lifted for non-literal shapes ONLY; a mutation that lifts it
      // for literal tokens too makes these run and is caught here). An alias written WITHOUT AS is not the bounded form.
      {"SELECT TRUE flag", "DATATYPE.DESCRIPTOR.INVALID", kBindOnly, "", "an all-literal boolean projection with an AS-less alias keeps the scalar-projection stopgap"},
      {"SELECT TRUE AS flag, FALSE other", "DATATYPE.DESCRIPTOR.INVALID", kBindOnly, "", "an all-literal boolean list with one AS-less alias keeps the stopgap"},
      {"SELECT TRUE one, 1", "DATATYPE.DESCRIPTOR.INVALID", kBindOnly, "", "an AS-less aliased boolean beside an integer keeps the stopgap"},
      {"VALUES (1), (TRUE)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "a column mixing integer and boolean across rows"},
      {"VALUES (TRUE), (1)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "a column mixing boolean and integer across rows"},
  });

  // ---- (2) DECIMAL LITERALS run natively. OLD: QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1 (the executor's VALUES descriptor validation: DECIMAL column profile). NEW (owner
  // 2026-11-26 item 3): the descriptor carries the cohort's exact-decimal codec profile; the value is the codec's 24 bytes; the parser renders the codec's fixed-point
  // spelling. Scale/precision/sign are in the body header (constants asserted). The engine codec's CANONICAL form drops trailing fractional zeros (0.10 shows 0.1) and a
  // negative zero (pinned by ia01_literal_runtime_test; the exact-decimal binary profile spec also defines the VALUE canonically without fractional trailing zeros, with the display scale a
  // property of the DESCRIPTOR): so `0.10` -> `0.1` and `0.0` -> `0` are the CURRENT behaviour, pinned PENDING the owner question (reference engines keep the written scale; preserving it
  // needs the literal descriptor's scale from the lexical and the column precision/scale in the vector). They are not claimed correct.
  // OLD (2026-12-24, part 1): the codec's CANONICAL shape was the display and the descriptor shape: `0.10` -> `0.1`, `0.0` -> `0`, and the cells were D(lexical, shown, byte0, byte1) without the
  // column's declared shape. NEW (owner 2026-11-28 Q1): the descriptor carries the WRITTEN shape, the vector declares it per column (column_precision / column_scale: the last two
  // arguments of D), the value bytes stay the canonical codec body (byte 0 = sign | scale, byte 1 = precision of the VALUE) and the parser pads the fraction to the column's scale.
  // `1e2` is decimal(3,0) and shows 100 (the written exponent folds into the integer digits). UNGROUNDED: what PostgreSQL, MySQL and Firebird do for an exponent literal (an exponent
  // literal may be an APPROXIMATE type in some of them) and whether each keeps the written scale of a plain decimal literal were NOT verified against their documentation in this session
  // (owner question 1 of the report).
  run_accepted("2", {
      {"SELECT 1.5", {{D("1.5", "1.5", 1, 2, 2, 1)}}}, {"SELECT 0.1", {{D("0.1", "0.1", 1, 1, 1, 1)}}}, {"SELECT -1.5", {{D("-1.5", "-1.5", 0x81, 2, 2, 1)}}},
      {"select 123.456;", {{D("123.456", "123.456", 3, 6, 6, 3)}}}, {"SELECT 1e2", {{D("1e2", "100", 0, 3, 3, 0)}}},
      // OLD: {"SELECT 0.10", {{D("0.10", "0.1", 1, 1)}}}, {"SELECT 0.0", {{D("0.0", "0", 0, 1)}}}
      {"SELECT 0.10", {{D("0.10", "0.10", 1, 1, 2, 2)}}}, {"SELECT 0.0", {{D("0.0", "0.0", 0, 1, 1, 1)}}}, {"SELECT 1.50", {{D("1.50", "1.50", 1, 2, 3, 2)}}},
      {"SELECT -0.50", {{D("-0.50", "-0.50", 0x81, 1, 2, 2)}}}, {"SELECT 100.00", {{D("100.00", "100.00", 0, 3, 5, 2)}}},
      {"VALUES (1.5)", {{D("1.5", "1.5", 1, 2, 2, 1)}}}, {"VALUES (-0.25)", {{D("-0.25", "-0.25", 0x82, 2, 2, 2)}}},
      {"VALUES (1.5), (2.5)", {{D("1.5", "1.5", 1, 2, 2, 1)}, {D("2.5", "2.5", 1, 2, 2, 1)}}},
      {"VALUES (1.5), (-2.5), (9.9)", {{D("1.5", "1.5", 1, 2, 2, 1)}, {D("-2.5", "-2.5", 0x81, 2, 2, 1)}, {D("9.9", "9.9", 1, 2, 2, 1)}}},
      {"VALUES (1.5, 2), (2.5, 3)", {{D("1.5", "1.5", 1, 2, 2, 1), I(2)}, {D("2.5", "2.5", 1, 2, 2, 1), I(3)}}},
      {"VALUES (1.5, 2)", {{D("1.5", "1.5", 1, 2, 2, 1), I(2)}}}, {"VALUES (1, 2.25)", {{I(1), D("2.25", "2.25", 2, 3, 3, 2)}}},
      {"VALUES (1.5, 2.25, TRUE)", {{D("1.5", "1.5", 1, 2, 2, 1), D("2.25", "2.25", 2, 3, 3, 2), B(true)}}},
      {"SELECT 1.5, 2.5", {{D("1.5", "1.5", 1, 2, 2, 1), D("2.5", "2.5", 1, 2, 2, 1)}}}, {"SELECT 1, 2.5", {{I(1), D("2.5", "2.5", 1, 2, 2, 1)}}},
      {"SELECT 1.5, TRUE", {{D("1.5", "1.5", 1, 2, 2, 1), B(true)}}},
      {"VALUES (99999999999999999999999999999999999999)", {{D("99999999999999999999999999999999999999", "99999999999999999999999999999999999999", 0, 38, 38, 0)}}},
      {"SELECT 0.00000000000000000000000000000000000001", {{D("0.00000000000000000000000000000000000001", "0.00000000000000000000000000000000000001", 38, 38, 38, 38)}}},
  });
  // ---- (2u) UNIFIED DECIMAL COLUMNS (owner 2026-11-28 Q2). OLD (part 1): every row of a decimal column needed EXACTLY the first row's precision and scale, so
  // `VALUES (1.5),(2.25)`, (0.5),(1.5), (1.5),(12.5), (1),(1.5), (1.5),(1) were refused SBLR.PLAN_TREE.INVALID_HANDLE (pinned in the old (2r) list, moved here as accepted).
  // NEW: scale = max scale, precision = max integer digits + max scale (<= 38), an int64 literal in a decimal column counts as decimal(19,0) and is negotiated as an exact decimal
  // (its value bytes are the decimal codec's, never a rescale of int64 bytes). The value bytes stay canonical (header constants below); the parser pads to the column's scale.
  run_accepted("2u", {
      {"VALUES (1.5), (2.25)", {{D("1.5", "1.50", 1, 2, 3, 2)}, {D("2.25", "2.25", 2, 3, 3, 2)}}},
      {"VALUES (0.5), (1.5)", {{D("0.5", "0.5", 1, 1, 2, 1)}, {D("1.5", "1.5", 1, 2, 2, 1)}}},
      {"VALUES (1.5), (12.5)", {{D("1.5", "1.5", 1, 2, 3, 1)}, {D("12.5", "12.5", 1, 3, 3, 1)}}},
      // an int64 literal counts as decimal(19,0): max integer digits 19, max scale 1 -> decimal(20,1)
      {"VALUES (1), (1.5)", {{D("1", "1.0", 0, 1, 20, 1)}, {D("1.5", "1.5", 1, 2, 20, 1)}}},
      {"VALUES (1.5), (1)", {{D("1.5", "1.5", 1, 2, 20, 1)}, {D("1", "1.0", 0, 1, 20, 1)}}},
      {"VALUES (1.5), (2.25), (100)", {{D("1.5", "1.50", 1, 2, 21, 2)}, {D("2.25", "2.25", 2, 3, 21, 2)}, {D("100", "100.00", 0, 3, 21, 2)}}},
      {"VALUES (-1), (0.25)", {{D("-1", "-1.00", 0x80, 1, 21, 2)}, {D("0.25", "0.25", 2, 2, 21, 2)}}},
      {"VALUES (9223372036854775808), (1.5)", {{D("9223372036854775808", "9223372036854775808.0", 0, 19, 20, 1)}, {D("1.5", "1.5", 1, 2, 20, 1)}}},
      {"VALUES (1e2), (1.5)", {{D("1e2", "100.0", 0, 3, 4, 1)}, {D("1.5", "1.5", 1, 2, 4, 1)}}},
      {"VALUES (0.10), (1.5)", {{D("0.10", "0.10", 1, 1, 3, 2)}, {D("1.5", "1.50", 1, 2, 3, 2)}}},
      {"VALUES (1.5, 1), (2.25, 2)", {{D("1.5", "1.50", 1, 2, 3, 2), I(1)}, {D("2.25", "2.25", 2, 3, 3, 2), I(2)}}},
      {"VALUES (1, 7), (1.5, 8)", {{D("1", "1.0", 0, 1, 20, 1), I(7)}, {D("1.5", "1.5", 1, 2, 20, 1), I(8)}}},
      // exactly 38 digits after unification: 36 integer digits + scale 2
      {"VALUES (123456789012345678901234567890123456.5), (0.25)", {{D("123456789012345678901234567890123456.5", "123456789012345678901234567890123456.50", 1, 37, 38, 2)}, {D("0.25", "0.25", 2, 2, 38, 2)}}},
  });
  // refused, pinned: a decimal mixed with a BOOLEAN across rows (no unification between a boolean and a number); everything else of the old (2r) list moved to (2u) above
  run_refused("2r", {
      {"VALUES (1.5), (TRUE)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "a decimal then a boolean in one column"},
      {"VALUES (TRUE), (1.5)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "a boolean then a decimal in one column"},
      {"VALUES (1), (1.5), (TRUE)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "a boolean in a column that would otherwise unify"},
  });
  // the UNIFIED column's precision beyond the profile's 38 digits has the same distinct token as a literal beyond it; precision = the unified precision, never the engine asked
  {
    const FileEvidence group_before;
    struct Over { std::string sql; const char* digits; };
    const std::vector<Over> over = {{"VALUES (12345678901234567890123456789.5), (1.123456789012345678)", "47"}, {"VALUES (123456789012345678901234567890123456.5), (0.125)", "39"},
                                    {"VALUES (9223372036854775808), (0.12345678901234567890)", "39"}};
    for (const auto& item : over) {
      const auto ran = RunSql(one, item.sql, 2);
      Check(!ran.result.accepted && FirstCode(ran.result) == "SBSQL.RESOURCE.DECIMAL_LITERAL_PRECISION_EXCEEDED", "(2u) '" + item.sql + "' is refused with the distinct precision token: " + AllCodes(ran.result));
      Check(FieldNamed(ran.result, "precision") == item.digits && FieldNamed(ran.result, "limit") == "38" && FieldNamed(ran.result, "literal_class") == "decimal",
            "(2u) '" + item.sql + "' carries the UNIFIED precision " + item.digits + ", limit 38 and the class as DATA: " + AllCodes(ran.result));
      Check(ran.server_diagnostics.empty() && asked_of(ran) == kBindOnly, "(2u) '" + item.sql + "' never asked the engine to finalize: [" + asked_of(ran) + "]");
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after '" + item.sql + "'");
    }
    CheckUntouched(group_before, "the (2u) unified-precision refusals");
    Check(active(order1), "(e) the transaction is still active after the (2u) refusals");
  }

  // ---- (3) INT64 OVERFLOW PROMOTION. OLD: DATATYPE.DESCRIPTOR.INVALID (the literal was negotiated int64 and the SBXN encoder's from_chars failed). NEW (owner 2026-11-26
  // item 2, PostgreSQL's rule): a digits-only literal outside int64 is the exact-decimal literal profile's; INT64_MAX and INT64_MIN stay int64.
  run_accepted("3", {
      {"SELECT 9223372036854775807", {{I(INT64_MAX)}}}, {"SELECT -9223372036854775808", {{I(INT64_MIN)}}},
      {"SELECT 9223372036854775808", {{D("9223372036854775808", "9223372036854775808", 0, 19, 19, 0)}}},
      {"SELECT -9223372036854775809", {{D("-9223372036854775809", "-9223372036854775809", 0x80, 19, 19, 0)}}},
      {"VALUES (9223372036854775808)", {{D("9223372036854775808", "9223372036854775808", 0, 19, 19, 0)}}},
      {"VALUES (9223372036854775807, 9223372036854775808)", {{I(INT64_MAX), D("9223372036854775808", "9223372036854775808", 0, 19, 19, 0)}}},
      {"SELECT 12345678901234567890123456789012345678", {{D("12345678901234567890123456789012345678", "12345678901234567890123456789012345678", 0, 38, 38, 0)}}},
  });
  // a literal beyond the exact-decimal profile's 38 digits is refused with its OWN token (data fields: precision = the mantissa digit count as written, limit = 38,
  // literal_class); the engine is never asked to finalize (bind row only)
  {
    const FileEvidence group_before;
    struct Over { std::string sql; const char* digits; };
    // the precision field is the literal's EFFECTIVE precision (max(coefficient digits, scale) once the exponent is applied), the codec's own definition
    const std::vector<Over> over = {{"SELECT 123456789012345678901234567890123456789", "39"}, {"VALUES (1234567890123456789012345678901234567890)", "40"},
                                    {"SELECT -123456789012345678901234567890123456789", "39"}, {"SELECT 1234567890123456789012345678901234567.89", "39"},
                                    {"SELECT 1e39", "40"}, {"SELECT 1e38", "39"}, {"SELECT 1e-39", "39"}, {"SELECT 0.000000000000000000000000000000000000001", "39"}};
    for (const auto& item : over) {
      const auto ran = RunSql(one, item.sql, 2);
      Check(!ran.result.accepted && FirstCode(ran.result) == "SBSQL.RESOURCE.DECIMAL_LITERAL_PRECISION_EXCEEDED", "(3) '" + item.sql + "' is refused with the distinct token: " + AllCodes(ran.result));
      Check(FieldNamed(ran.result, "precision") == item.digits && FieldNamed(ran.result, "limit") == "38" && FieldNamed(ran.result, "literal_class") == "decimal",
            "(3) '" + item.sql + "' carries precision " + item.digits + ", limit 38 and the literal class as DATA fields: " + AllCodes(ran.result));
      Check(ran.server_diagnostics.empty() && asked_of(ran) == kBindOnly, "(3) '" + item.sql + "' never asked the engine to finalize: [" + asked_of(ran) + "]");
      bool undifferentiated = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == "DATATYPE.DESCRIPTOR.INVALID") undifferentiated = true;
      Check(!undifferentiated, "(3) '" + item.sql + "' no longer gives the undifferentiated DATATYPE.DESCRIPTOR.INVALID");
      Check(owner_of(one) && one.order() == order1, "(e) the transaction is still owned after '" + item.sql + "'");
    }
    CheckUntouched(group_before, "the (3) precision refusals");
    Check(active(order1), "(e) the transaction is still active after the (3) precision refusals");
  }
  // the 32-literal slot limit still has its own token and still counts decimal literals as numeric
  run_refused("3s", {
      {"VALUES (" + [] { std::string out; for (int i = 0; i < 33; ++i) out += std::string(i ? ", " : "") + "1.5"; return out; }() + ")", "SBSQL.RESOURCE.LITERAL_SLOT_LIMIT_EXCEEDED", kBindOnly, "",
       "33 decimal literals in one row (also the 16-column bound: the slot token wins)"},
  });

  // ---- (Q3) ALIASED BARE LITERAL PROJECTIONS route through the VALUES relation (owner 2026-11-28). OLD: `SELECT 1.5 AS x` was the scalar projection (query.evaluate_projection, one
  // finalize row, no decimal carrier: ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT) and `SELECT 1 AS one` ran on that route. NEW: the same query.execute VALUES relation as `VALUES (..)`;
  // the alias is the parser's DISPLAY name (kept as written, like the VALUES column-list alias); an unaliased item keeps column<N>. The alias never reaches the engine (asserted below).
  run_accepted("q3", {
      {"SELECT 1.5 AS x", {{D("1.5", "1.5", 1, 2, 2, 1)}}, {"x"}}, {"SELECT 1 AS one", {{I(1)}}, {"one"}}, {"SELECT TRUE AS t", {{B(true)}}, {"t"}},
      {"SELECT TRUE AS t, 1.5 AS d, 2 AS i", {{B(true), D("1.5", "1.5", 1, 2, 2, 1), I(2)}}, {"t", "d", "i"}},
      {"SELECT 1 AS X", {{I(1)}}, {"X"}}, {"SELECT 1 AS \"My Col\", 2.5 AS b", {{I(1), D("2.5", "2.5", 1, 2, 2, 1)}}, {"My Col", "b"}},
      {"SELECT 1 AS one, 2", {{I(1), I(2)}}, {"one", "column2"}}, {"SELECT 1, 2 AS two", {{I(1), I(2)}}, {"column1", "two"}},
      {"SELECT -1.50 AS neg;", {{D("-1.50", "-1.50", 0x81, 2, 3, 2)}}, {"neg"}}, {"select 0.10 as small", {{D("0.10", "0.10", 1, 1, 2, 2)}}, {"small"}},
  });
  {
    // the alias is DISPLAY only: the lowered request carries no alias text and has the plain statement's size to within one byte (OBSERVED: the request's size varies by one byte with the
    // statement's text, e.g. `SELECT 1` and `SELECT 1 AS abcdefghijklmnop` differ by 1 while their relation is identical; a byte diff showed many length fields moving by 1 and one inserted
    // byte, presumably a decimal number's digit count: not investigated further. An 11-character alias in the request would differ by 11 or more).
    const struct { const char* aliased; const char* plain; const char* alias_text; } pairs[] = {
        {"SELECT 1.5 AS xyzzy_alias", "SELECT 1.5", "xyzzy_alias"}, {"SELECT 1 AS xyzzy_alias", "SELECT 1", "xyzzy_alias"},
        {"SELECT TRUE AS xyzzy_alias, 2 AS plover_alias", "SELECT TRUE, 2", "plover_alias"}};
    for (const auto& pair : pairs) {
      const auto a = RunSql(one, pair.aliased, 2);
      const auto b = RunSql(one, pair.plain, 2);
      Check(a.result.accepted && b.result.accepted && std::abs(static_cast<long>(a.result.sblr_payload.size()) - static_cast<long>(b.result.sblr_payload.size())) <= 1 &&
                std::abs(static_cast<long>(a.result.server_request_payload_bytes) - static_cast<long>(b.result.server_request_payload_bytes)) <= 1 &&
                a.result.server_request_payload_bytes != 0 && a.result.sblr_payload.find("xyzzy_alias") == std::string::npos && a.result.sblr_payload.find(pair.alias_text) == std::string::npos &&
                KeysOf(a) == KeysOf(b),
            std::string("(q3) the alias never reaches the engine: '") + pair.aliased + "' has the lowered size within one byte (" + std::to_string(a.result.sblr_payload.size()) + " vs " +
                std::to_string(b.result.sblr_payload.size()) + "), the request size within one byte (" + std::to_string(a.result.server_request_payload_bytes) + " vs " +
                std::to_string(b.result.server_request_payload_bytes) + "), no alias text and the same recorder rows as the plain statement");
    }
  }

  // ---- (Q5) PURE-INTEGER LISTS move from the scalar-projection route to the VALUES route (owner 2026-11-28). OLD: `SELECT 1, 2` was query.evaluate_projection (its own recorder rows
  // and its own finalize path); NEW: query.execute, the same three recorder rows as the other bare lists, printed column1..N (the scalar route printed the same names: no visible change).
  // Expressions (`SELECT 1 + 1`) keep their route and refusal (asserted in (1r) above).
  run_accepted("q5", {
      {"SELECT 1, 2", {{I(1), I(2)}}}, {"SELECT 1, -2, 3", {{I(1), I(-2), I(3)}}}, {"select 7 , 8 ;", {{I(7), I(8)}}},
      {"SELECT 9223372036854775807, -9223372036854775808", {{I(INT64_MAX), I(INT64_MIN)}}},
  });
  one.End();
}

// ============================================================================================================================================================
// SB_NATIVE_VALUES_NULL_CELLS (2026-12-26; owner 2026-11-28 N1, N4, N5): the `null_text` state (its text half is NOT built: see the report). A VALUES column that mixes SQL NULL cells with
// BOOLEAN cells resolves to BOOLEAN and is nullable (N5); the engine's vector declares column_nullability per column, carries state[r.c] per cell of a nullable column and NO value field for a NULL
// cell (N1); the parser renders the NULL as the client's NULL. A NULL with no typed peer has no datatype (the spec carries no NULL identity: N4 as accepted cannot be built, owner question 1 of
// the report): every such shape keeps its refusal token exactly, pinned below. A NULL beside an integer or a decimal is NOT built yet (the literal negotiation registers a nullable tuple for
// BOOLEAN only): pinned as refused.
// ============================================================================================================================================================
// One expected cell of a nullable-aware table: 'b' boolean / 'i' int64, `null` = SQL NULL (shown is then unused).
struct NCell { char kind; bool null; std::string shown; };
NCell NB(bool v) { return {'b', false, v ? "true" : "false"}; }
NCell NI(std::int64_t v) { return {'i', false, std::to_string(v)}; }
NCell NN(char kind) { return {kind, true, ""}; }

void CheckAcceptedNullable(const Ran& ran, const std::string& sql, const std::vector<std::vector<NCell>>& expected, const CohortInt64& int64_cohort, const CohortInt64& boolean_cohort) {
  namespace pr = scratchbird::wire::public_result;
  const auto& r = ran.result;
  const std::string label = "'" + sql.substr(0, 70) + "'";
  Check(r.accepted && !r.messages.has_errors(), label + " is accepted through the real pipeline: " + AllCodes(r));
  Check(r.server_operation_id == "query.execute", label + ": the engine operation is query.execute: " + r.server_operation_id);
  const std::size_t rows = expected.size(), columns = expected.front().size();
  std::vector<bool> nullable(columns, false);
  for (const auto& row : expected) for (std::size_t c = 0; c < columns; ++c) if (row[c].null) nullable[c] = true;
  // the parser's client table: a NULL cell is the client's NULL (nullopt), the others display as written
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> got;
  Check(ps2::DecodePublicResultRowsForTest(r.server_result_payload, &oids, &got) && got.size() == rows && oids.size() == columns,
        label + ": the parser-built table has " + std::to_string(rows) + " rows and " + std::to_string(columns) + " columns");
  for (std::size_t row = 0; row < rows && row < got.size(); ++row)
    for (std::size_t column = 0; column < columns && column < got[row].size(); ++column) {
      const auto& cell = expected[row][column];
      Check(cell.null ? !got[row][column].has_value() : (got[row][column] && *got[row][column] == cell.shown),
            label + ": parser row " + std::to_string(row) + " column " + std::to_string(column) + (cell.null ? " is the client's NULL" : " displays `" + cell.shown + "`"));
    }
  // the parser's per-row metadata states null / not_null PER CELL (the parser's own existing state field), and the column type
  std::vector<pr::Field> packet;
  Check(pr::Decode(r.server_result_payload, &packet), label + ": the parser's packet decodes");
  for (std::size_t row = 0; row < rows; ++row) {
    std::string meta;
    for (std::size_t column = 0; column < columns; ++column)
      meta += (column ? ";" : "") + std::string("column") + std::to_string(column + 1) + (expected[row][column].kind == 'b' ? ":boolean" : ":int64") + (expected[row][column].null ? ":null" : ":not_null");
    bool found = false;
    for (const auto& f : packet) if (f.name == "row_meta[" + std::to_string(row) + "]" && f.value == meta) found = true;
    Check(found, label + ": row_meta[" + std::to_string(row) + "] is " + meta);
  }
  // the engine's ONE vector
  std::vector<pr::Field> fields;
  Check(pr::Decode(ran.raw_vector, &fields), label + ": the engine's vector decodes");
  std::size_t expected_fields = 4 + 7 * columns;
  for (const auto& row : expected) for (std::size_t c = 0; c < columns; ++c) expected_fields += nullable[c] ? (row[c].null ? 1 : 2) : 1;
  Check(fields.size() == expected_fields, label + ": the vector holds exactly 4 + 7 per column + the cell fields (a NULL cell is ONE state field and NO value field): " + std::to_string(fields.size()) + " vs " + std::to_string(expected_fields));
  const auto find = [&](const std::string& name) -> const pr::Field* {
    const pr::Field* found = nullptr;
    for (const auto& f : fields) if (f.name == name) { if (found) return nullptr; found = &f; }
    return found;
  };
  for (std::size_t column = 0; column < columns; ++column) {
    const auto n = std::to_string(column);
    const auto& cohort = expected.front()[column].kind == 'b' ? boolean_cohort : int64_cohort;
    const auto* t = find("column_datatype[" + n + "]");
    const auto* td = find("column_datatype_descriptor[" + n + "]");
    const auto* g = find("column_datatype_generation[" + n + "]");
    Check(t && td && g && UuidOf(t->value) == cohort.type_uuid && UuidOf(td->value) == cohort.descriptor_uuid && LE64(g->value) == cohort.generation,
          label + ": column " + n + " carries the cohort's " + (expected.front()[column].kind == 'b' ? "BOOLEAN" : "int64") + " identity (the peer's type, never a NULL identity)");
    const auto* cn = find("column_nullability[" + n + "]");
    Check(cn && cn->kind == pr::Kind::unsigned_integer && LE64(cn->value) == (nullable[column] ? 1u : 0u), label + ": column " + n + " declares column_nullability " + (nullable[column] ? "1" : "0") + " (constant written in the test)");
  }
  for (std::size_t row = 0; row < rows; ++row)
    for (std::size_t column = 0; column < columns; ++column) {
      const auto& cell = expected[row][column];
      const auto at = std::to_string(row) + "." + std::to_string(column);
      const auto* state = find("state[" + at + "]");
      const auto* value = find("value[" + at + "]");
      if (nullable[column]) {
        Check(state && state->kind == pr::Kind::unsigned_integer && state->value.size() == 8 && LE64(state->value) == (cell.null ? 1u : 0u), label + ": state[" + at + "] is " + (cell.null ? "1 (SQL NULL)" : "0 (a value)"));
      } else {
        Check(state == nullptr, label + ": a not-null column carries no state field (state[" + at + "] absent)");
      }
      if (cell.null) {
        Check(value == nullptr, label + ": the NULL cell " + at + " has NO value field (a typed absence, never an empty string)");
      } else if (cell.kind == 'b') {
        Check(value && value->kind == pr::Kind::bytes && value->value.size() == 1 && (value->value[0] == '\1') == (cell.shown == "true") && (value->value[0] == '\0' || value->value[0] == '\1'), label + ": value[" + at + "] is ONE byte, 0 or 1");
      } else {
        Check(value && value->kind == pr::Kind::signed_integer && value->value.size() == 8 && std::to_string(static_cast<std::int64_t>(LE64(value->value))) == cell.shown, label + ": value[" + at + "] is the int64 " + cell.shown);
      }
    }
  // no engine text: after the two machine tokens (operation_id, result_kind) no field is Kind::text, no value is a spelled boolean and no field value spells a NULL or a column name
  bool no_text = fields.size() > 2;
  for (std::size_t i = 2; i < fields.size(); ++i) if (fields[i].kind == pr::Kind::text) no_text = false;
  Check(no_text && ran.raw_vector.find("true") == std::string::npos && ran.raw_vector.find("false") == std::string::npos && ran.raw_vector.find("NULL") == std::string::npos && ran.raw_vector.find("column1") == std::string::npos,
        label + ": the engine's vector carries no text field, no rendered boolean, no NULL text and no column name");
}

void NullTextState() {
  (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto catalog_generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  auto direct = Approve();
  auto direct_public = OpenPublicFor(*direct);
  auto direct_context = ContextFor(*direct, catalog_generation);
  auto direct_receipt = Acquire(*direct_public, direct_context, direct->approval.transaction_uuid);
  Check(direct_receipt.ok(), "a direct native receipt (for the cohort identities): " + direct_receipt.code);
  const auto int64_cohort = CohortInt64Of(CopyContext(direct_receipt));
  const auto boolean_cohort = CohortBooleanOf(CopyContext(direct_receipt));
  {
    std::set<std::string> table;
    for (const auto& row : api::NativeExecutorAvailabilityAllowList()) table.insert(RowKey(row));
    Check(table == std::set<std::string>({"engine.op.literal#3", "engine.op.name_resolve#4865"}), "the production allow-list is exactly {name-resolve 0x1301 (4865), literal 3} (this batch adds NO row)");
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
  // the recorder rows of an accepted table: name_resolve (bind), literal (finalize), literal (execution): the same three rows whatever the cell count (as the numeric and boolean tables)
  const std::string kThree = "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) engine.op.literal#3(listed) ";
  struct Accepted { std::string sql; std::vector<std::vector<NCell>> rows; };
  const auto run_accepted = [&](const std::string& group, const std::vector<Accepted>& list) {
    const FileEvidence group_before;
    for (const auto& item : list) {
      const auto ran = RunSql(one, item.sql, 2);
      CheckAcceptedNullable(ran, item.sql, item.rows, int64_cohort, boolean_cohort);
      Check(asked_of(ran) == kThree, "(" + group + ") '" + item.sql + "' asks exactly [name_resolve (bind), literal (finalize), literal (execution)], all listed: " + asked_of(ran));
      Check(owner_of(one) && one.order() == order1, "the transaction is still owned after '" + item.sql + "'");
    }
    CheckUntouched(group_before, "the (" + group + ") statements, every one");
    Check(active(order1), "the transaction is still active after the (" + group + ") statements");
  };
  struct Refused { std::string sql; std::string code; std::string asked; std::string engine_fields; std::string what; };
  const auto run_refused = [&](const std::string& group, const std::vector<Refused>& list) {
    const FileEvidence group_before;
    for (const auto& item : list) {
      const auto ran = RunSql(one, item.sql, 2);
      bool present = false;
      for (const auto& d : ran.result.messages.diagnostics) if (d.code == item.code) present = true;
      Check(!ran.result.accepted && present, "(" + group + ") '" + item.sql + "' (" + item.what + ") is refused with " + item.code + ": " + AllCodes(ran.result));
      Check(asked_of(ran) == item.asked, "(" + group + ") '" + item.sql + "': the production table was asked exactly [" + item.asked + "], got [" + asked_of(ran) + "]");
      CheckEngineFields(ran, item.sql, item.engine_fields, false);
      Check(owner_of(one) && one.order() == order1, "the transaction is still owned after the refused '" + item.sql + "'");
    }
    CheckUntouched(group_before, "the (" + group + ") refusals, every one");
    Check(active(order1), "the transaction is still active after the (" + group + ") refusals");
  };

  // ---- (n1) a boolean column that mixes NULL with TRUE / FALSE: ACCEPTED (N5). OLD (route_b (d2) / this state's pins before 2026-12-26): VALUES (TRUE),(NULL) was refused at the dispatch gate
  // with SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED (opcode_code 4615): the gate recognized only an all-present literal table.
  run_accepted("n1", {
      {"VALUES (TRUE),(NULL),(FALSE)", {{NB(true)}, {NN('b')}, {NB(false)}}},
      {"VALUES (NULL),(TRUE)", {{NN('b')}, {NB(true)}}},
      {"VALUES (FALSE),(NULL)", {{NB(false)}, {NN('b')}}},
      {"VALUES (TRUE),(NULL),(NULL),(FALSE)", {{NB(true)}, {NN('b')}, {NN('b')}, {NB(false)}}},
      {"values (null),(true);", {{NN('b')}, {NB(true)}}},
      // column nullability is PER COLUMN: the second column holds no NULL and stays not-null
      {"VALUES (NULL, 1), (TRUE, 2)", {{NN('b'), NI(1)}, {NB(true), NI(2)}}},
      {"VALUES (TRUE, 1), (NULL, 2), (FALSE, 3)", {{NB(true), NI(1)}, {NN('b'), NI(2)}, {NB(false), NI(3)}}},
  });
  // ---- (n2) every shape with NO typed peer, or whose peer is not yet negotiable as nullable, keeps its refusal token exactly (the OLD pins, unchanged). A NULL has no datatype of its own
  // (the specification carries no NULL identity), so a column of NULLs only has nothing to resolve to: owner question 1.
  const std::string kBindOnly = "engine.op.name_resolve#4865(listed) ";
  const std::string kBindFinalize = "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ";  // the statement holds a negotiated literal: finalize asks the literal row too
  run_refused("n2", {
      {"SELECT NULL", "DATATYPE.CONTEXT_REQUIRED", "", "", "an untyped NULL stops in the PARSER (no context): unchanged"},
      {"SELECT NULL AS x", "DATATYPE.CONTEXT_REQUIRED", "", "", "an aliased untyped NULL: unchanged"},
      {"SELECT NULL, 1", "DATATYPE.CONTEXT_REQUIRED", "", "", "a NULL beside an integer in ONE row has no peer (peers are the cells of one COLUMN)"},
      {"SELECT 1, NULL AS n", "DATATYPE.CONTEXT_REQUIRED", "", "", "an aliased NULL beside an integer in one row"},
      {"SELECT TRUE, NULL", "DATATYPE.CONTEXT_REQUIRED", "", "", "a NULL beside a boolean in one row has no peer either"},
      {"VALUES (NULL)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kBindOnly, "opcode_code=4615", "a column of NULLs only: no peer, refused at the dispatch gate (unchanged, as pinned in route_b (d2))"},
      {"VALUES (NULL), (NULL)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kBindOnly, "opcode_code=4615", "an all-NULL column of two rows: no peer"},
      {"VALUES (NULL, 1)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kBindFinalize, "opcode_code=4615", "a one-row table whose first column is NULL only"},
      {"VALUES (1), (NULL)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", kBindFinalize, "opcode_code=4615", "NOT BUILT: an integer peer needs a nullable integer literal tuple the negotiation does not register (owner question 2)"},
      {"VALUES (NULL), (1)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "NOT BUILT: an integer peer after a NULL first row (owner question 2)"},
      {"VALUES (NULL), (1.5)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "NOT BUILT: a decimal peer (owner question 2)"},
      {"VALUES (TRUE), (NULL), (1)", "SBLR.PLAN_TREE.INVALID_HANDLE", kBindOnly, "", "a column mixing boolean and integer keeps its refusal, NULL or not"},
  });
  one.End();
}

std::optional<int> ExtraReceiptStateForTest(const std::string& state, const Offline&) {
  if (state == "route_b") { RouteBState(); return 0; }
  if (state == "route_b2") { RouteB2State(); return 0; }
  if (state == "value_types") { ValueTypesState(); return 0; }
  if (state == "null_text") { NullTextState(); return 0; }
  if (state == "typed_uuid") { TypedUuidState(); return 0; }
  if (state == "pgp_refusal") { PgpRefusalState(); return 0; }
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
    // `size N` runs the route_b state; `size N route_b2` (this batch's parser additions) and `size N typed_uuid` (typed binary identity fields) run their own states
    RunState(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), argc == 4 ? argv[3] : "route_b");
    artifacts.Cleanup();
    std::printf("native_route_b: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
