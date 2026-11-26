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

struct Refusal2 {
  const char* sql;
  const char* code;          // the parser-visible diagnostic code (the table's own code)
  const char* server_status; // the status token the server's diagnostic carries ("" when the statement never reaches the engine)
  const char* asked;         // the exact rows the production table was asked, in order, from the log-only recorder ("(NOT listed)" marks the refusal)
  const char* what;
};

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
  const std::vector<Refusal2> refused = {
      {"SELECT 1 + 0", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "INVALID_ARGUMENT", "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ",
       "an operator expression: the executor is refused on the operator-call row (opcode_code 1025)"},
      {"SELECT 1 + 0 AS s", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "INVALID_ARGUMENT", "engine.op.name_resolve#4865(listed) engine.op.operator_call#1025(NOT listed) ",
       "an aliased operator projection (0x040E): the same operator-call row"},
      {"SELECT 'a'", "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ",
       "a text literal (the table cannot tell it from 1; the vector builder refuses: no text reaches a native client)"},
      {"SELECT 1 FROM t", "ENGINE.NATIVE.WIRE_NAME_RESOLVE_S5", "", "",
       "a table reference (the parser's own refusal: the engine is never asked)"},
      {"VALUES (1),(2)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) engine.op.literal#3(listed) ",
       "a multi-row VALUES: refused at the dispatch gate (no execution-time row was asked)"},
      {"CREATE TABLE t (a INT)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "UNSUPPORTED", "engine.op.name_resolve#4865(listed) ",
       "DDL: refused at the dispatch gate (only the bind row was asked)"},
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
      if (round == 0) {
        std::string asked;
        for (const auto& r : ran.recorded) asked += RowKey(r.row) + (r.in_allow_list ? "(listed) " : "(NOT listed) ");
        Check(asked == shape.asked, std::string("(d) '") + shape.sql + "': the production table was asked exactly [" + shape.asked + "], got [" + asked + "]");
      }
      Check(active(order1) && owner_of(one) && one.order() == order1, std::string("(e) the transaction is still active and owned after the refused '") + shape.sql + "'");
    }
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
  // the engine's OWN opcode field for the gate-refused shapes (the server does not forward the engine's diagnostic fields to the parser: the
  // parser sees only the code and a status token, as the pins above show), read at the engine's dispatch gate with hand-framed envelopes
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
  // an explicitly named VALUES relation: SBsql's own behaviour is observed (the parser takes no column list from a VALUES alias today)
  {
    const FileEvidence before;
    const auto ran = RunSql(one, "SELECT * FROM (VALUES (1)) AS t(x)", 0);
    CheckUntouched(before, "SELECT * FROM (VALUES (1)) AS t(x)");
    // SBsql today has no derived-table VALUES with a column list (a follow-up under the default rule: other engines support it)
    Check(!ran.result.accepted && FirstCode(ran.result) == "QOW-DIAG-QRY-001-AST-MALFORMED",
          "the explicitly named VALUES form is refused by the parser today with QOW-DIAG-QRY-001-AST-MALFORMED (no engine involved): " + AllCodes(ran.result));
    Check(ran.server_diagnostics.empty(), "and it never reached the engine");
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
      {"VALUES (1),(2)", false, 0, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"}, {"SELECT 1", true, 1, ""}};
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
  Mark("verify");
  Check(Image(served.path) == image_before, "the statements wrote nothing: the file's bytes are identical");
  Check(ListingOf(served.directory) == listing_before, "and no file appeared");
  Mark("disconnect");
  one->End();
  one.reset();
}

std::optional<int> ExtraReceiptStateForTest(const std::string& state, const Offline&) {
  if (state == "route_b") { RouteBState(); return 0; }
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
  if (argc == 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    RunState(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), "route_b");
    artifacts.Cleanup();
    std::printf("native_route_b: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
