// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"
#include "common/common.hpp"
#include "diagnostics.hpp"
#include "rendering/rendering.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sbsql = scratchbird::parser::sbsql;
namespace sbps = scratchbird::server::sbps;

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string_view message) {
    if (condition) return;
    ok = false;
    ++failures;
    if (failures <= 100) std::cerr << message << '\n';
  }
};

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  bool quoted = false;
  for (const char ch : line) {
    if (ch == '"') {
      quoted = !quoted;
      continue;
    }
    if (ch == ',' && !quoted) {
      fields.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  fields.push_back(current);
  return fields;
}

std::vector<std::map<std::string, std::string>> ReadCsv(
    const std::filesystem::path& path,
    Harness* harness) {
  std::ifstream in(path);
  harness->Check(in.good(), "fixture CSV could not be opened");
  if (!in.good()) return {};

  std::string header_line;
  std::getline(in, header_line);
  const auto headers = SplitCsvLine(header_line);
  std::vector<std::map<std::string, std::string>> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    std::map<std::string, std::string> row;
    for (std::size_t i = 0; i < headers.size() && i < fields.size(); ++i) {
      row[headers[i]] = fields[i];
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

bool HasForbiddenLeak(std::string_view text) {
  constexpr std::array<std::string_view, 10> forbidden = {
      "hidden_table",
      "missing_table",
      "/tmp/secret",
      "policy.secret",
      "provider.local_password",
      "00000000-0000-7000-8000-000000000010",
      "019e05df-f012-7000-8000-0000000000f1",
      "\"_sb_row_version\"",
      "\"hidden\":true",
      "\"system\":true",
  };
  for (const auto token : forbidden) {
    if (Contains(text, token)) return true;
  }
  return false;
}

void ValidateFixtureCoverage(const std::filesystem::path& fixture_path,
                             Harness* harness) {
  const auto rows = ReadCsv(fixture_path, harness);
  harness->Check(rows.size() == 6, "security fixture row count mismatch");
  const std::map<std::string, std::string> expected_fields_by_id{
      {"FSPE012G-HIDDEN-NAME", "code;severity;message;component"},
      {"FSPE012G-MISSING-NAME", "code;severity;message;component"},
      {"FSPE012G-DIAGNOSTIC-REDACTION",
       "code;message_key;severity;safe_message;reason_code"},
      {"FSPE012G-CACHE-AUTHORITY",
       "security_policy_epoch;grant_epoch;role_set_hash;search_path_hash;language_profile"},
      {"FSPE012G-METADATA-PROJECTION", "value_column_only"},
      {"FSPE012G-REFERENCE-RENDERING", "code;severity;message;component"},
  };
  std::set<std::string> ids;
  const std::map<std::string, std::string>* hidden_row = nullptr;
  const std::map<std::string, std::string>* missing_row = nullptr;
  const std::map<std::string, std::string>* reference_row = nullptr;
  for (const auto& row : rows) {
    const auto id = row.at("fixture_id");
    ids.insert(id);
    const auto expected_fields = expected_fields_by_id.find(id);
    harness->Check(expected_fields != expected_fields_by_id.end(),
                   "unexpected security fixture id");
    if (expected_fields != expected_fields_by_id.end()) {
      harness->Check(row.at("returned_fields") == expected_fields->second,
                     "security fixture returned-fields contract mismatch");
    }
    harness->Check(row.at("closure_status") == "ready",
                   "security fixture is not marked ready");
    if (id == "FSPE012G-HIDDEN-NAME" || id == "FSPE012G-MISSING-NAME") {
      harness->Check(row.at("expected_message_vector") ==
                         "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE",
                     "hidden/missing fixture does not share public diagnostic");
      harness->Check(row.at("elapsed_time_class") == "bounded_same_class",
                     "hidden/missing fixture timing class is not bounded");
    }
    if (id == "FSPE012G-HIDDEN-NAME") hidden_row = &row;
    if (id == "FSPE012G-MISSING-NAME") missing_row = &row;
    if (id == "FSPE012G-REFERENCE-RENDERING") reference_row = &row;
  }
  harness->Check(ids.contains("FSPE012G-HIDDEN-NAME"), "hidden-name fixture missing");
  harness->Check(ids.contains("FSPE012G-MISSING-NAME"), "missing-name fixture missing");
  harness->Check(ids.contains("FSPE012G-DIAGNOSTIC-REDACTION"),
                 "diagnostic-redaction fixture missing");
  harness->Check(ids.contains("FSPE012G-CACHE-AUTHORITY"),
                 "cache-authority fixture missing");
  harness->Check(ids.contains("FSPE012G-METADATA-PROJECTION"),
                 "metadata-projection fixture missing");
  harness->Check(ids.contains("FSPE012G-REFERENCE-RENDERING"),
                 "reference-rendering fixture missing");
  if (hidden_row != nullptr && missing_row != nullptr) {
    harness->Check(hidden_row->at("expected_message_vector") ==
                       missing_row->at("expected_message_vector"),
                   "hidden and missing fixture diagnostics are distinguishable");
    harness->Check(hidden_row->at("returned_fields") ==
                       missing_row->at("returned_fields"),
                   "hidden and missing fixture fields are distinguishable");
    harness->Check(hidden_row->at("elapsed_time_class") ==
                       missing_row->at("elapsed_time_class"),
                   "hidden and missing fixture timing classes are distinguishable");
  }
  if (reference_row != nullptr && hidden_row != nullptr) {
    harness->Check(reference_row->at("profile") == "postgres_compat",
                   "reference-rendering fixture does not bind reference profile");
    harness->Check(reference_row->at("expected_message_vector") ==
                       hidden_row->at("expected_message_vector"),
                   "reference rendering does not preserve hidden-as-missing diagnostic");
    harness->Check(reference_row->at("returned_fields") ==
                       hidden_row->at("returned_fields"),
                   "reference rendering returned-fields contract diverges");
  }
}

sbsql::MessageVectorSet SensitiveParserMessages() {
  sbsql::MessageVectorSet messages;
  messages.diagnostics.push_back(sbsql::MakeDiagnostic(
      "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE",
      "ERROR",
      "object name could not be resolved or is not visible",
      "sbp_sbsql.sbps_client",
      {{"object_uuid", "00000000-0000-7000-8000-000000000010"},
       {"policy_id", "policy.secret.internal"},
       {"provider_detail", "provider.local_password"},
       {"path", "/tmp/secret/sb.db"},
       {"presented_name", "hidden_table"},
       {"object_name", "missing_table"},
       {"expected_token_set", "relation_name"},
       {"line", "1"},
       {"operation_uuid", "not_assigned"}}));
  return messages;
}

void ValidateParserDiagnosticRedaction(Harness* harness) {
  const auto messages = SensitiveParserMessages();
  const auto json = sbsql::MessageVectorToJson(messages);
  const auto rendered = sbsql::RenderMessageVectorSet(messages);

  harness->Check(Contains(json, "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE"),
                 "parser diagnostic JSON lost public diagnostic code");
  harness->Check(Contains(json, "\"line\":\"1\""),
                 "parser diagnostic JSON lost safe line field");
  harness->Check(Contains(json, "\"expected_token_set\":\"relation_name\""),
                 "parser diagnostic JSON lost safe expected-token field");
  harness->Check(Contains(json, "\"operation_uuid\":\"not_assigned\""),
                 "parser diagnostic JSON lost safe operation UUID sentinel");
  harness->Check(!HasForbiddenLeak(json),
                 "parser diagnostic JSON leaked sensitive authority detail");

  harness->Check(Contains(rendered, "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE"),
                 "parser rendered diagnostic lost public diagnostic code");
  harness->Check(Contains(rendered, "line=1"),
                 "parser rendered diagnostic lost safe line field");
  harness->Check(Contains(rendered, "operation_uuid=not_assigned"),
                 "parser rendered diagnostic lost safe operation UUID sentinel");
  harness->Check(!HasForbiddenLeak(rendered),
                 "parser rendered diagnostic leaked sensitive authority detail");
}

scratchbird::server::ServerDiagnostic SensitiveServerDiagnostic() {
  return scratchbird::server::ServerDiagnostic{
      "SERVER.ADMISSION.REFUSED",
      "server.admission.refused",
      scratchbird::server::ServerDiagnosticSeverity::kError,
      "The request could not be admitted.",
      {{"operation_family", "sblr.query.relational.v3"},
       {"reason_code", "not_authorized_or_not_found"},
       {"operation_uuid", "00000000-0000-7000-8000-000000000010"},
       {"policy_uuid", "019e05df-f012-7000-8000-0000000000f1"},
       {"provider_detail", "provider.local_password"},
       {"database_path", "/tmp/secret/sb.db"},
       {"hidden_object_name", "hidden_table"},
       {"presented_name", "missing_table"}}};
}

void ValidateServerDiagnosticRedaction(Harness* harness) {
  const auto diagnostic = SensitiveServerDiagnostic();
  const auto json_line = scratchbird::server::ToMessageVectorJsonLine(diagnostic);
  harness->Check(Contains(json_line, "SERVER.ADMISSION.REFUSED"),
                 "server diagnostic JSON lost public diagnostic code");
  harness->Check(Contains(json_line, "reason_code"),
                 "server diagnostic JSON lost safe reason code");
  harness->Check(!HasForbiddenLeak(json_line),
                 "server diagnostic JSON leaked sensitive authority detail");

  const auto payload = sbps::EncodeMessageVectorSet(
      {diagnostic}, sbps::MakeUuidV7Bytes());
  const std::string raw(payload.begin(), payload.end());
  harness->Check(Contains(raw, "SERVER.ADMISSION.REFUSED"),
                 "SBPS message-vector payload lost public diagnostic code");
  harness->Check(Contains(raw, "reason_code"),
                 "SBPS message-vector payload lost safe reason code");
  harness->Check(!HasForbiddenLeak(raw),
                 "SBPS message-vector payload leaked sensitive authority detail");
}

std::string BaseEnvelope(std::string_view operation_family,
                         std::string_view result_shape,
                         std::string_view trace_key) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"";
  out += operation_family;
  out += "\",\"surface_key\":\"fspe012g.fixture\",";
  out += "\"sblr_operation_key\":\"op.fspe012g.fixture\",";
  out += "\"result_shape\":\"";
  out += result_shape;
  out += "\",\"diagnostic_shape\":\"diag.fspe012g.v1\",";
  out += "\"resource_contract\":\"resource.fspe012g.v1\",";
  out += "\"trace_key\":\"";
  out += trace_key;
  out += "\",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f012-7000-8000-0000000000ff\"],";
  out += "\"descriptor_refs\":[\"descriptor.fspe012g.security\"],";
  out += "\"policy_refs\":[\"policy.fspe012g.redaction\"]";
  return out;
}

std::string SyntheticRowsetEnvelope(std::uint64_t stream_rows) {
  std::string out = BaseEnvelope("sblr.query.relational.v3",
                                 "result.shape.rowset",
                                 "FSPE-012G-ROWSET");
  out += ",\"stream_row_count\":";
  out += std::to_string(stream_rows);
  out += "}";
  return out;
}

scratchbird::server::HostedEngineState MakeEngineState() {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_security_redaction_gate.sbdb";
  database.database_uuid = "019e05df-f012-7000-8000-0000000000f6";
  state.databases.push_back(database);
  return state;
}

scratchbird::server::ServerSessionRegistry MakeRegistry(
    std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_security_redaction_gate.sbdb";
  session.database_uuid = "019e05df-f012-7000-8000-0000000000f6";
  session.catalog_generation = 1;
  session.security_epoch = 1;
  session.descriptor_epoch = 1;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.role_set_hash = "roles/app_reader";
  session.group_set_hash = "groups/reporting";
  session.search_path_hash = "search_path/sys_public";
  session.language_profile = "en-US";
  *session_uuid = session.session_uuid;

  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] =
      session;
  return registry;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodePrepareSblrPayloadForTest(session_uuid, encoded);
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_uuid,
                         const std::string& encoded,
                         bool cursor_requested = false) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, prepared_uuid, encoded, cursor_requested);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor_uuid, max_rows, 0);
  return frame;
}

void ValidatePublicMetadataProjection(Harness* harness) {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();
  const auto execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, SyntheticRowsetEnvelope(1), true));
  harness->Check(execute.accepted, "server execute rejected metadata projection fixture");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  harness->Check(cursor_uuid.has_value(), "metadata projection fixture did not return cursor");
  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid.value_or(std::array<std::uint8_t, 16>{}), 1));
  harness->Check(fetch.accepted, "server fetch rejected metadata projection fixture");
  const auto decoded = scratchbird::server::DecodeFetchResultForTest(fetch.payload);
  harness->Check(decoded.has_value(), "server fetch result did not decode");
  const std::string internal_packet = decoded ? decoded->row_packet : "";

  harness->Check(Contains(internal_packet, "\"_sb_row_version\""),
                 "internal result metadata lost hidden row-version column");
  harness->Check(Contains(internal_packet, "\"hidden\":true") &&
                     Contains(internal_packet, "\"system\":true"),
                 "internal result metadata lost system-column visibility bits");

  sbsql::PipelineResult result;
  result.accepted = true;
  result.operation_family = "sblr.query.relational.v3";
  result.server_operation_id = "sblr.query.relational.v3";
  result.server_row_count = 1;
  result.server_result_payload = internal_packet;
  const auto rendered = sbsql::RenderPipelineResult(result);
  harness->Check(Contains(rendered, "RESULT"), "public result rendering missing result line");
  harness->Check(Contains(rendered, "\"name\":\"value\""),
                 "public result rendering lost visible value column");
  harness->Check(!HasForbiddenLeak(rendered),
                 "public result rendering leaked hidden/system metadata");
}

sbsql::CacheKey BaseCacheKey() {
  sbsql::CacheKey key;
  key.shape_hash = 0x012f012f;
  key.registry_version = 3;
  key.catalog_epoch = 11;
  key.security_policy_epoch = 12;
  key.grant_epoch = 13;
  key.descriptor_epoch = 14;
  key.udr_epoch = 15;
  key.role_set_hash = "roles/app_reader";
  key.group_set_hash = "groups/reporting";
  key.search_path_hash = "search_path/sys_public";
  key.language_profile = "en-US";
  key.policy_profile = "policy/public";
  key.parser_profile = "sbsql/default";
  key.result_contract_hash = "result/rowset/v1";
  return key;
}

void ValidateCacheSideChannelDimensions(Harness* harness) {
  const auto base = BaseCacheKey();
  sbsql::SblrTemplateCache cache;
  cache.Store(base, "SBLR(BASE)");
  harness->Check(cache.Lookup(base).value_or("") == "SBLR(BASE)",
                 "base cache entry did not round trip");

  auto CheckMiss = [&](std::string_view label, auto mutator) {
    auto mutated = base;
    mutator(&mutated);
    harness->Check(!cache.Lookup(mutated).has_value(),
                   std::string("cache lookup did not miss for authority dimension: ") +
                       std::string(label));
  };

  CheckMiss("security_policy_epoch",
            [](sbsql::CacheKey* key) { key->security_policy_epoch += 1; });
  CheckMiss("registry_version",
            [](sbsql::CacheKey* key) { key->registry_version += 1; });
  CheckMiss("grant_epoch", [](sbsql::CacheKey* key) { key->grant_epoch += 1; });
  CheckMiss("descriptor_epoch", [](sbsql::CacheKey* key) { key->descriptor_epoch += 1; });
  CheckMiss("role_set_hash",
            [](sbsql::CacheKey* key) { key->role_set_hash = "roles/app_writer"; });
  CheckMiss("group_set_hash",
            [](sbsql::CacheKey* key) { key->group_set_hash = "groups/admin"; });
  CheckMiss("search_path_hash",
            [](sbsql::CacheKey* key) { key->search_path_hash = "search_path/private"; });
  CheckMiss("language_profile",
            [](sbsql::CacheKey* key) { key->language_profile = "fr-CA"; });
  CheckMiss("policy_profile",
            [](sbsql::CacheKey* key) { key->policy_profile = "policy/private"; });
  CheckMiss("result_contract_hash",
            [](sbsql::CacheKey* key) { key->result_contract_hash = "result/secret/v1"; });

  auto CheckInvalidator = [&](std::string_view label, auto invalidator) {
    sbsql::SblrTemplateCache scoped_cache;
    scoped_cache.Store(base, "SBLR(BASE)");
    invalidator(&scoped_cache);
    harness->Check(scoped_cache.Size() == 0,
                   std::string("cache invalidator retained stale authority entry: ") +
                       std::string(label));
  };
  CheckInvalidator("grant_epoch", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateGrantEpoch(999);
  });
  CheckInvalidator("security_policy_epoch", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateSecurityPolicyEpoch(999);
  });
  CheckInvalidator("descriptor_epoch", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateDescriptorEpoch(999);
  });
  CheckInvalidator("registry_version", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateRegistryVersion(999);
  });
  CheckInvalidator("role_set_hash", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateRoleSetHash("roles/app_writer");
  });
  CheckInvalidator("group_set_hash", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateGroupSetHash("groups/admin");
  });
  CheckInvalidator("search_path_hash", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateSearchPathHash("search_path/private");
  });
  CheckInvalidator("language_profile", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateLanguageProfile("fr-CA");
  });
  CheckInvalidator("policy_profile", [](sbsql::SblrTemplateCache* c) {
    c->InvalidatePolicyProfile("policy/private");
  });
  CheckInvalidator("result_contract_hash", [](sbsql::SblrTemplateCache* c) {
    c->InvalidateResultContractHash("result/secret/v1");
  });
}

void ValidatePreparedAuthorityStaleChecks(Harness* harness) {
  const auto engine_state = MakeEngineState();
  const auto encoded = SyntheticRowsetEnvelope(1);

  auto CheckStale = [&](std::string_view label,
                        const std::function<void(scratchbird::server::ServerSessionRecord*)>& mutate) {
    std::array<std::uint8_t, 16> session_uuid{};
    auto registry = MakeRegistry(&session_uuid);
    const auto prepare = scratchbird::server::HandlePrepareSblr(
        &registry, engine_state, PrepareFrame(session_uuid, encoded));
    harness->Check(prepare.accepted,
                   std::string("prepare failed for stale-check case: ") + std::string(label));
    const auto prepared_uuid =
        scratchbird::server::DecodePreparedStatementUuidForTest(prepare.payload);
    harness->Check(prepared_uuid.has_value(),
                   std::string("prepared UUID missing for stale-check case: ") + std::string(label));

    auto& session =
        registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session_uuid)];
    mutate(&session);

    const auto execute = scratchbird::server::HandleExecuteSblr(
        &registry,
        engine_state,
        ExecuteFrame(session_uuid,
                     prepared_uuid.value_or(std::array<std::uint8_t, 16>{}),
                     "",
                     false));
    harness->Check(!execute.accepted,
                   std::string("stale prepared statement was accepted for: ") + std::string(label));
    harness->Check(!execute.diagnostics.empty() &&
                       execute.diagnostics.front().code ==
                           "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                   std::string("stale prepared statement diagnostic mismatch for: ") +
                       std::string(label));
  };

  CheckStale("descriptor_epoch",
             [](scratchbird::server::ServerSessionRecord* session) {
               session->descriptor_epoch += 1;
             });
  CheckStale("grant_epoch", [](scratchbird::server::ServerSessionRecord* session) {
    session->grant_epoch += 1;
  });
  CheckStale("role_set_hash", [](scratchbird::server::ServerSessionRecord* session) {
    session->role_set_hash = "roles/app_writer";
  });
  CheckStale("group_set_hash", [](scratchbird::server::ServerSessionRecord* session) {
    session->group_set_hash = "groups/admin";
  });
  CheckStale("search_path_hash", [](scratchbird::server::ServerSessionRecord* session) {
    session->search_path_hash = "search_path/private";
  });
  CheckStale("language_profile", [](scratchbird::server::ServerSessionRecord* session) {
    session->language_profile = "fr-CA";
  });
}

void ValidateHiddenAsMissingSourceContract(const std::filesystem::path& repo_root,
                                           Harness* harness) {
  const auto path = repo_root / "project/src/server/ipc_server.cpp";
  std::ifstream in(path);
  harness->Check(in.good(), "server IPC source could not be opened");
  const std::string source((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  harness->Check(Contains(source, "normalized == \"sys.version\""),
                 "name resolver lost visible sys.version allow-list");
  harness->Check(Contains(source, "EncodePsNameResolvePayload(\"not_found_or_not_visible\""),
                 "name resolver does not collapse hidden and missing names");
  harness->Check(Contains(source, "not_found_or_not_visible"),
                 "name resolver missing public non-disclosure outcome");
}

}  // namespace

int main(int argc, char** argv) {
  Harness harness;
  const std::filesystem::path repo_root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::current_path().parent_path();
  const std::filesystem::path fixture_path =
      argc > 2 ? std::filesystem::path(argv[2])
               : repo_root /
                     "project/tests/sbsql_parser_worker/generated/security/"
                     "SECURITY_REDACTION_SIDE_CHANNEL_FIXTURES.csv";

  ValidateFixtureCoverage(fixture_path, &harness);
  ValidateParserDiagnosticRedaction(&harness);
  ValidateServerDiagnosticRedaction(&harness);
  ValidatePublicMetadataProjection(&harness);
  ValidateCacheSideChannelDimensions(&harness);
  ValidatePreparedAuthorityStaleChecks(&harness);
  ValidateHiddenAsMissingSourceContract(repo_root, &harness);

  if (!harness.ok) {
    std::cerr << "sbsql_security_redaction_side_channel_gate failed with "
              << harness.failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "sbsql_security_redaction_side_channel_gate passed\n";
  return EXIT_SUCCESS;
}
