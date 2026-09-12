// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"
#include "engine/sblr/sblr_ddl_create_index_runtime.hpp"
#include "engine/sblr/sblr_plan_import_rows_codec.hpp"
#include "engine/sblr/sblr_source_artifact_runtime.hpp"
#include "server/sblr_dispatch_command.hpp"
#include "engine/sblr/sblr_stmt_execute_runtime.hpp"
#include "engine/sblr/sblr_stmt_execute_direct_runtime.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"
#include "engine/sblr/sblr_result_page_runtime.hpp"
#include "engine/sblr/sblr_query_explain_runtime.hpp"
#include "engine/sblr/sblr_parse_text_runtime.hpp"
#include "engine/sblr/sblr_catalog_epoch_check_runtime.hpp"
#include "core/hash/hash_digest.hpp"

namespace {

Submission BuildCreateIndexSubmission(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    bool opcode_stream = true,
    bool multi_member = false) {
  auto submission = BuildSubmission(fixture, view, parser_uuid);
  const auto package = RawUuid(view.bound_ast_uuid);

  sblr::SblrDdlCreateIndexDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.ddl_create_index", "SBLR_DDL_CREATE_INDEX",
      "ia01.admission.create_index");
  member.opcode_code = 1540;
  member.result_shape = "ddl_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.requires_cluster_authority = false;
  member.contains_sql_text = false;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "create_index_descriptor";
  operand.name = "index";
  operand.value_kind = sblr::SblrValueKind::create_index_descriptor;
  operand.value_body =
      sblr::EncodeSblrDdlCreateIndexDescriptorV1(descriptor, true);
  member.operands.push_back(std::move(operand));

  if (opcode_stream) {
    sblr::SblrOpcodeStream package_stream;
    package_stream.package_descriptor_uuid = view.bound_ast_uuid;
    package_stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
    package_stream.operations.push_back(
        Frame(true, parser_uuid, view.catalog_epoch_uuid, package));
    package_stream.operations.push_back(member);
    if (multi_member) package_stream.operations.push_back(member);
    package_stream.operations.push_back(
        Frame(false, parser_uuid, view.catalog_epoch_uuid, package));
    submission.stream = sblr::EncodeSblrOpcodeStream(package_stream);
  } else {
    const auto sbop = sblr::EncodeSblrEnvelope(member);
    submission.stream.assign(sbop.begin(), sbop.end());
  }
  Require(!submission.stream.empty(),
          "canonical CREATE INDEX SBOS encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "base canonical container decoding failed");
  container.container.canonical_anchor[100] = opcode_stream ? 1 : 2;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto outer = wire::EncodeSblrContainer(container.container);
  Require(!outer.empty(), "canonical CREATE INDEX container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "base execution envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(opcode_stream ? 1 : 2);
  fields[5] = {0};
  fields[6] = {0};
  auto& payload_reference = opcode_stream ? fields[5] : fields[6];
  payload_reference = {1};
  U64(&payload_reference, submission.stream.size());
  payload_reference.insert(payload_reference.end(), submission.stream.begin(),
                           submission.stream.end());
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(),
                                   submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!execution.empty(),
          "canonical CREATE INDEX execution envelope encoding failed");

  submission.container.assign(outer.begin(), outer.end());
  submission.ingress.assign(execution.begin(), execution.end());
  return submission;
}

Submission BuildPlanImportRowsSubmission(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    bool opcode_stream = true,
    bool multi_member = false) {
  auto submission = BuildCreateIndexSubmission(
      fixture, view, parser_uuid, opcode_stream, false);

  sblr::PlanImportRowsDescriptorRefV1 descriptor;
  descriptor.descriptor_uuid = RawUuid(view.bound_ast_uuid);
  descriptor.descriptor_generation = 1;
  std::vector<std::uint8_t> descriptor_bytes;
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Require(sblr::EncodePlanImportRowsDescriptorRefV1(
              descriptor, &descriptor_bytes, &diagnostic),
          "canonical import-plan descriptor reference encoding failed");

  auto member = sblr::MakeSblrEnvelope(
      "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS",
      "ia01.admission.plan_import_rows");
  member.opcode_code = sblr::kPlanImportRowsOpcodeCodeV1;
  member.result_shape = "import_plan_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.requires_cluster_authority = false;
  member.contains_sql_text = false;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "import_rows_plan_descriptor";
  operand.name = "request";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body = std::move(descriptor_bytes);
  member.operands.push_back(std::move(operand));

  if (opcode_stream) {
    const auto decoded = sblr::DecodeSblrOpcodeStream(std::string_view(
        reinterpret_cast<const char*>(submission.stream.data()),
        submission.stream.size()));
    Require(decoded.ok && decoded.stream.operations.size() == 3,
            "base package decoding for import planning failed");
    auto package_stream = decoded.stream;
    package_stream.operations[1] = member;
    if (multi_member) {
      package_stream.operations.insert(package_stream.operations.end() - 1,
                                       member);
    }
    submission.stream = sblr::EncodeSblrOpcodeStream(package_stream);
  } else {
    const auto sbop = sblr::EncodeSblrEnvelope(member);
    submission.stream.assign(sbop.begin(), sbop.end());
  }
  Require(!submission.stream.empty(),
          "canonical import-plan payload encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "base import-plan container decoding failed");
  container.container.canonical_anchor[100] = opcode_stream ? 1 : 2;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto outer = wire::EncodeSblrContainer(container.container);
  Require(!outer.empty(), "canonical import-plan container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "base import-plan execution envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(opcode_stream ? 1 : 2);
  fields[5] = {0};
  fields[6] = {0};
  auto& payload_reference = opcode_stream ? fields[5] : fields[6];
  payload_reference = {1};
  U64(&payload_reference, submission.stream.size());
  payload_reference.insert(payload_reference.end(), submission.stream.begin(),
                           submission.stream.end());
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(),
                                   submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!execution.empty(),
          "canonical import-plan execution envelope encoding failed");

  submission.container.assign(outer.begin(), outer.end());
  submission.ingress.assign(execution.begin(), execution.end());
  return submission;
}

Submission RepackSubmission(Submission submission,
                            const sblr::SblrOpcodeStream& package) {
  submission.stream = sblr::EncodeSblrOpcodeStream(package);
  Require(!submission.stream.empty(), "companion package encoding failed");
  auto outer = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(outer.status == wire::SblrCodecStatus::ok &&
              ingress.status == wire::SblrCodecStatus::ok,
          "companion base container decoding failed");
  outer.container.operation_payload = submission.stream;
  auto& fields = ingress.envelope.fields;
  fields[5] = {1};
  U64(&fields[5], submission.stream.size());
  fields[5].insert(fields[5].end(), submission.stream.begin(), submission.stream.end());
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(), submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto container_bytes = wire::EncodeSblrContainer(outer.container);
  const auto ingress_bytes = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!container_bytes.empty() && !ingress_bytes.empty(),
          "companion transport encoding failed");
  submission.container.assign(container_bytes.begin(), container_bytes.end());
  submission.ingress.assign(ingress_bytes.begin(), ingress_bytes.end());
  return submission;
}

void TestDispatchCommandHelpers(
    const Fixture& fixture, const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid, const server::ServerSblrAdmissionRequest& request) {
  unsigned checks = 0, failures = 0;
  const auto check = [&](bool ok, std::string_view message) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "dispatch command: " << message << '\n'; }
  };
  struct Profile { const char* id; const char* opcode; std::uint16_t code; bool terminal; };
  constexpr Profile profiles[] = {
      {"engine.op.stmt_execute", "SBLR_STMT_EXECUTE", 0x1201, true},
      {"engine.op.stmt_execute_direct", "SBLR_STMT_EXECUTE_DIRECT", 0x1202, true},
      {"engine.op.catalog_introspect", "SBLR_CATALOG_INTROSPECT", 0x1300, true},
      {"engine.op.result_page", "SBLR_RESULT_PAGE", 0x1206, false},
      {"engine.op.query_explain", "SBLR_QUERY_EXPLAIN", 0x1208, false},
      {"engine.op.parse_text", "SBLR_PARSE_TEXT", 0x1304, false},
      {"engine.op.catalog_epoch_check", "SBLR_CATALOG_EPOCH_CHECK", 0x1305, false},
      {"engine.op.txn_commit", "SBLR_TXN_COMMIT", 257, false},
      {"query.execute", "SBLR_QUERY_EXECUTE", 0x1207, false},
  };
  const auto base = BuildSubmission(fixture, view, parser_uuid);
  const auto decoded = sblr::DecodeSblrOpcodeStream(std::string_view(
      reinterpret_cast<const char*>(base.stream.data()), base.stream.size()));
  Require(decoded.ok, "dispatch fixture stream decode failed");
  auto map = sblr::MakeSblrEnvelope("engine.op.source_map", "SBLR_SOURCE_MAP", "dispatch.companion");
  map.opcode_code = 6; map.result_shape = "void"; map.diagnostic_shape = "diagnostic_vector";
  map.parser_package_uuid = parser_uuid; map.registry_snapshot_uuid = view.catalog_epoch_uuid;
  sblr::SblrOperand reference;
  reference.ordinal = 1; reference.type = "source_map.vector"; reference.name = "source_map";
  reference.value_kind = sblr::SblrValueKind::descriptor_ref;
  const auto uuid = RawUuid(view.bound_ast_uuid);
  reference.value_body.assign(uuid.begin(), uuid.end()); U64(&reference.value_body, 1);
  map.operands.push_back(reference);
  sblr::SblrTransactionCommitOptionsV1 options;
  options.transaction_uuid = RawUuid(Text(NewUuid(platform::UuidKind::transaction, 705)));
  options.local_transaction_id = 19;
  options.admitted_handle_evidence_sha256[0] = 1;
  options.commit_mode = 2; options.authority_scope = 2; options.wait_policy = 2;
  options.deadline_monotonic_ns = 123456789;
  const auto options_bytes = sblr::EncodeSblrTransactionCommitOptionsV1(&options);
  Require(!options_bytes.empty(), "commit options fixture encoding failed");
  // These canonical typed descriptors are preparation/selection fixtures,
  // not engine-issued SOURCE_MAP, cursor, or transaction authority.
  for (const auto& profile : profiles) {
    auto root = decoded.stream.operations[1];
    root.operation_id = profile.id; root.opcode = profile.opcode; root.opcode_code = profile.code;
    const auto set_operand = [&](const char* type, const char* name,
                                 sblr::SblrValueKind kind, Bytes body, const char* result) {
      Require(!body.empty(), std::string("dispatch descriptor encoding failed: ") + profile.id);
      sblr::SblrOperand operand;
      operand.ordinal = 1; operand.type = type; operand.name = name;
      operand.value_kind = kind; operand.value_body = std::move(body);
      root.operands.push_back(std::move(operand)); root.result_shape = result;
    };
    const auto receipt = RawUuid(view.receipt_uuid);
    const auto snapshot = RawUuid(view.statement_metadata_snapshot_uuid);
    const auto parser = RawUuid(parser_uuid);
    if (profile.code == 0x1201) {
      sblr::SblrStmtExecuteDescriptorV1 d;
      d.execution_uuid = uuid; d.statement_uuid = uuid; d.statement_name_uuid = uuid;
      d.statement_receipt_uuid = receipt; d.catalog_snapshot_uuid = snapshot;
      d.catalog_generation = view.catalog_generation_id; d.security_epoch = view.security_epoch;
      d.resource_epoch = view.resource_epoch; d.mga_snapshot_uuid = RawUuid(view.statement_snapshot_uuid);
      d.prepared_generation = 1; d.prepared_descriptor_sha256[0] = 1;
      d.result_descriptor_uuid = uuid; d.parser_package_uuid = parser;
      d.executor_availability_generation = 1;
      set_operand("stmt_execute_descriptor", "statement", sblr::SblrValueKind::stmt_execute_descriptor,
                  sblr::EncodeSblrStmtExecuteDescriptorV1(d), "stmt_execute_result");
    } else if (profile.code == 0x1202) {
      sblr::SblrStmtExecuteDirectDescriptorV1 d;
      d.execution_uuid = uuid; d.statement_receipt_uuid = receipt; d.catalog_snapshot_uuid = snapshot;
      d.catalog_generation = view.catalog_generation_id; d.security_epoch = view.security_epoch;
      d.resource_epoch = view.resource_epoch; d.mga_snapshot_uuid = RawUuid(view.statement_snapshot_uuid);
      d.result_descriptor_uuid = uuid; d.parser_package_uuid = parser;
      d.executor_availability_generation = 1; d.canonical_sblr_bytes = base.stream;
      set_operand("stmt_execute_direct_descriptor", "statement", sblr::SblrValueKind::stmt_execute_direct_descriptor,
                  sblr::EncodeSblrStmtExecuteDirectDescriptorV1(d), "stmt_execute_result");
    } else if (profile.code == 0x1300) {
      sblr::SblrCatalogIntrospectDescriptorV1 d;
      d.object_kind = sblr::kSblrCatalogIntrospectObjectKindTableV1;
      d.profile = sblr::kSblrCatalogIntrospectProfileShowObjectDetailV1;
      d.flags = sblr::kSblrCatalogIntrospectDetailFlagV1; d.object_uuid = uuid;
      d.catalog_epoch = view.catalog_generation_id; d.security_epoch = view.security_epoch;
      d.canonical_path_utf8 = "APP.PROBE"; d.availability = 1;
      set_operand("catalog_introspect_descriptor", "object_detail", sblr::SblrValueKind::catalog_introspect_descriptor,
                  sblr::EncodeSblrCatalogIntrospectDescriptorV1(d, true), "catalog_introspect_result");
    } else if (profile.code == 0x1206) {
      sblr::SblrResultPageDescriptorV1 d;
      d.cursor_uuid = uuid; d.cursor_generation = 1; d.statement_receipt_uuid = receipt;
      d.result_set_handle_uuid = uuid; d.result_set_handle_generation = 1;
      d.snapshot_uuid = snapshot; d.row_descriptor_uuid = uuid; d.row_descriptor_generation = 1;
      d.maximum_rows = 64; d.maximum_bytes = 4096; d.redaction_profile_uuid = uuid;
      d.redaction_generation = 1; d.policy_snapshot_uuid = uuid; d.policy_generation = 1;
      d.resource_budget_uuid = uuid; d.resource_budget_generation = 1; d.executor_availability_generation = 1;
      set_operand("result_page_descriptor.v1", "result_page", sblr::SblrValueKind::result_page_descriptor,
                  sblr::EncodeSblrResultPageDescriptorV1(d), "result_page_data");
    } else if (profile.code == 0x1208) {
      sblr::SblrQueryExplainDescriptorV1 d;
      d.explain_uuid = uuid; d.statement_receipt_uuid = receipt; d.query_snapshot_uuid = uuid;
      d.catalog_snapshot_uuid = snapshot; d.catalog_generation = view.catalog_generation_id;
      d.security_context_uuid = RawUuid(view.security_context_uuid); d.policy_snapshot_uuid = uuid;
      d.policy_generation = 1; d.plan_policy_uuid = uuid; d.resource_budget_uuid = uuid;
      d.resource_budget_generation = 1; d.redaction_profile_uuid = uuid; d.verbose = true; d.format = 2;
      d.canonical_query_sblr_bytes = base.stream; d.executor_availability_generation = 1;
      d.parser_package_uuid = parser; d.language_profile_uuid = uuid;
      set_operand("query_explain_descriptor.v1", "query", sblr::SblrValueKind::query_explain_descriptor,
                  sblr::EncodeSblrQueryExplainDescriptorV1(d), "explain_result");
    } else if (profile.code == 0x1304) {
      sblr::SblrParseTextDescriptorV1 d;
      d.parse_uuid = uuid; d.statement_receipt_uuid = receipt; d.language_profile_uuid = uuid;
      d.language_profile_generation = 1; d.parser_package_uuid = parser; d.parser_package_version_major = 1;
      d.catalog_snapshot_uuid = snapshot; d.catalog_generation = view.catalog_generation_id;
      d.security_context_uuid = RawUuid(view.security_context_uuid); d.security_epoch = view.security_epoch;
      d.resource_epoch = view.resource_epoch; d.input_byte_count = 9;
      d.canonical_input_sha256 = sblr::SblrParseTextInputSha256V1("SELECT 1;");
      d.requested_maximum_bytes = 65536; d.requested_maximum_depth = 64;
      d.executor_availability_generation = 1; d.canonical_sblr_bytes = base.stream;
      set_operand("parse_text_descriptor", "text", sblr::SblrValueKind::parse_text_descriptor,
                  sblr::EncodeSblrParseTextDescriptorV1(d), "parse_text_result");
    } else if (profile.code == 0x1305) {
      sblr::SblrCatalogEpochCheckDescriptorV1 d;
      d.check_uuid = uuid; d.statement_receipt_uuid = receipt; d.requested_catalog_epoch_uuid = RawUuid(view.catalog_epoch_uuid);
      d.requested_catalog_generation = view.catalog_generation_id; d.database_uuid = RawUuid(Text(fixture.database_uuid));
      d.schema_tree_uuid = uuid; d.schema_tree_generation = 1; d.security_context_uuid = RawUuid(view.security_context_uuid);
      d.policy_snapshot_uuid = uuid; d.policy_generation = 1; d.catalog_snapshot_uuid = snapshot;
      d.security_epoch = view.security_epoch; d.resource_epoch = view.resource_epoch; d.executor_availability_generation = 1;
      d.visibility_scope_sha256 = sblr::SblrCatalogEpochCheckVisibilityScopeSha256V1(
          false, d.database_uuid, d.schema_tree_uuid, d.schema_tree_generation);
      set_operand("catalog_epoch_check_descriptor", "catalog_epoch", sblr::SblrValueKind::catalog_epoch_check_descriptor,
                  sblr::EncodeSblrCatalogEpochCheckDescriptorV1(d), "catalog_epoch_result");
    }
    if (profile.code == 257) {
      sblr::SblrOperand operand;
      operand.ordinal = 1; operand.type = "transaction.commit.options"; operand.name = "options";
      operand.value_kind = sblr::SblrValueKind::transaction_commit_options;
      operand.value_body = options_bytes; root.operands.push_back(operand); root.result_shape = "commit_result";
    }
    for (unsigned position = 0; position != 3; ++position) {
      auto package = decoded.stream; package.operations[1] = root;
      if (position) package.operations.insert(package.operations.begin() + position, map);
      const auto submission = RepackSubmission(base, package);
      auto prepare = request;
      prepare.encoded_sblr_container = submission.container;
      prepare.encoded_execution_envelope = submission.ingress;
      prepare.package_reservation_deferred = true; prepare.package_reservation_handle = 0;
      const auto admission = server::AdmitServerSblrEnvelope(prepare);
      Require(admission.admitted && admission.admission_token,
              "dispatch selection preparation failed");
      const auto& token = admission.admission_token;
      const auto* selected = server::AdmittedServerSblrCommand(token);
      check(selected && selected->operation_id == profile.id && selected->opcode_code == profile.code &&
                selected == &token->stream.operations[position == 1 ? 2 : 1],
            "helper must select original owning record");
      check(server::ServerSblrHasTerminalCursorPayload(token) == profile.terminal,
            "companion must not suppress terminal cursor payload or expose ordinary FETCH payload");
      check(token->canonical_operation_bytes == submission.stream,
            "dispatch selection must not remove companion bytes");
      std::string detail;
      const auto commit = server::CanonicalTransactionCommitOptionsFromContainer(submission.container, &detail);
      check(commit.has_value() == (profile.code == 257), "commit extractor exact root selection");
      if (commit) {
        auto copy = *commit;
        check(sblr::EncodeSblrTransactionCommitOptionsV1(&copy) == options_bytes &&
                  commit->transaction_uuid == options.transaction_uuid &&
                  commit->local_transaction_id == options.local_transaction_id &&
                  commit->commit_mode == 2 && commit->authority_scope == 2 && commit->wait_policy == 2 &&
                  commit->deadline_monotonic_ns == options.deadline_monotonic_ns,
              "commit options must survive byte-identically");
      }
      for (unsigned mutation = 0; mutation != 5; ++mutation) {
        auto invalid = std::make_shared<server::ServerSblrAdmissionTokenData>(*token);
        if (mutation == 0) invalid->operation.operation_id = "engine.op.package_begin";
        if (mutation == 1) invalid->opcode_stream = false;
        if (mutation == 2) invalid->stream.operations.clear();
        if (mutation == 3) invalid->stream.operations.insert(invalid->stream.operations.end() - 1, root);
        if (mutation == 4) invalid->stream.operations[position == 1 ? 2 : 1].opcode_code ^= 1;
        check(server::AdmittedServerSblrCommand(invalid) == nullptr &&
                  !server::ServerSblrHasTerminalCursorPayload(invalid),
              "malformed token must not select a command or terminal payload");
      }
      if (position) {
        auto invalid = std::make_shared<server::ServerSblrAdmissionTokenData>(*token);
        invalid->stream.operations[position].operands.front().value_body[6] = 0x40;
        check(server::AdmittedServerSblrCommand(invalid) == nullptr &&
                  !server::ServerSblrHasTerminalCursorPayload(invalid),
              "malformed source map must invalidate dispatch view");
      }
      if (profile.code == 257) {
        for (unsigned mutation = 0; mutation != 5; ++mutation) {
          auto outer = wire::DecodeSblrContainerBytes(
              reinterpret_cast<const std::uint8_t*>(submission.container.data()), submission.container.size());
          Require(outer.status == wire::SblrCodecStatus::ok, "commit negative container decode failed");
          if (mutation == 0) outer.container.canonical_anchor[100] = 2;
          if (mutation == 1) outer.container.operation_payload[0] ^= 1;
          if (mutation >= 2) {
            auto wrong = package;
            if (mutation == 2) wrong.operations.erase(wrong.operations.begin() + 1, wrong.operations.end() - 1);
            if (mutation == 3) wrong.operations.insert(wrong.operations.end() - 1, root);
            if (mutation == 4) {
              if (position) wrong.operations[position].result_shape = "rowset";
              else wrong.operations[1] = decoded.stream.operations[1];
            }
            outer.container.operation_payload = sblr::EncodeSblrOpcodeStream(wrong);
            Require(!outer.container.operation_payload.empty(), "commit negative stream encoding failed");
          }
          const auto bytes = wire::EncodeSblrContainer(outer.container);
          Require(!bytes.empty(), "commit negative container encoding failed");
          check(!server::CanonicalTransactionCommitOptionsFromContainer(
                    std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &detail),
                "commit extraction must reject wrong kind corrupted stream missing/additional command or bad companion");
        }
        auto corrupted = submission.container; corrupted.back() ^= 1;
        check(!server::CanonicalTransactionCommitOptionsFromContainer(corrupted, &detail),
              "commit extraction must retain outer checksum validation");
      }
    }
  }
  check(server::AdmittedServerSblrCommand({}) == nullptr &&
            !server::ServerSblrHasTerminalCursorPayload({}), "null token must not expose payload");
  std::cout << "source_map_dispatch_helpers checks=" << checks << " failures=" << failures
            << " engine_execution=false parser_ipc_e2e=false\n";
  Require(failures == 0, "dispatch command helper regression failed");
}

void TestCompanionAdmission(
    const Fixture& fixture, const bridge::StatementContextReceiptView& view,
    bridge::StatementContextReceiptHandle receipt, std::string_view parser_uuid,
    const server::ServerSblrAdmissionRequest& base_request) {
  unsigned checks = 0, failures = 0;
  const auto check = [&](bool ok, std::string_view label) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "companion admission: " << label << '\n'; }
  };
  const auto decode = [](const Submission& submission) {
    auto decoded = sblr::DecodeSblrOpcodeStream(std::string_view(
        reinterpret_cast<const char*>(submission.stream.data()), submission.stream.size()));
    Require(decoded.ok, "companion fixture decoding failed");
    return decoded.stream;
  };
  for (unsigned root_kind = 0; root_kind != 4; ++root_kind) {
    auto base = root_kind == 1 ? BuildCreateIndexSubmission(fixture, view, parser_uuid)
              : root_kind == 2 ? BuildPlanImportRowsSubmission(fixture, view, parser_uuid)
                               : BuildSubmission(fixture, view, parser_uuid);
    auto package = decode(base);
    if (root_kind == 3) {
      auto& command = package.operations[1];
      command.operation_id = "cluster.inspect_provider";
      command.opcode = "SBLR_CLUSTER_INSPECT_PROVIDER";
      command.opcode_code = 0x0b3d;
      command.result_shape = "cluster_provider_inspection_result";
      command.operands.clear();
      base = RepackSubmission(base, package);
    }
    const auto command = package.operations[1];
    auto map = sblr::MakeSblrEnvelope("engine.op.source_map", "SBLR_SOURCE_MAP",
                                     "ia01.admission.companion");
    map.opcode_code = 6;
    map.parser_package_uuid = parser_uuid;
    map.registry_snapshot_uuid = view.catalog_epoch_uuid;
    map.result_shape = "void";
    map.diagnostic_shape = "diagnostic_vector";
    sblr::SblrOperand reference;
    reference.ordinal = 1; reference.type = "source_map.vector"; reference.name = "source_map";
    reference.value_kind = sblr::SblrValueKind::descriptor_ref;
    const auto id = RawUuid(view.bound_ast_uuid);
    reference.value_body.assign(id.begin(), id.end());
    U64(&reference.value_body, 1);
    map.operands.push_back(reference);
    // This is structural admission, not descriptor issuance or execution proof.
    for (unsigned position = 0; position != 3; ++position) {
      auto with_map = package;
      if (position) with_map.operations.insert(with_map.operations.begin() + position, map);
      const auto submission = RepackSubmission(base, with_map);
      bridge::StatementPackageAdmissionReservationRequest reserve;
      reserve.receipt = receipt;
      reserve.payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
      reserve.canonical_payload_bytes = submission.stream.data();
      reserve.canonical_payload_size = submission.stream.size();
      bridge::StatementPackageAdmissionReservationHandle handle;
      bridge::StatementPackageAdmissionReservationView reservation;
      sb_engine_result_t result = nullptr;
      Require(bridge::AcquireStatementPackageAdmissionReservation(
                  &reserve, &handle, &reservation, &result) == SB_ENGINE_STATUS_OK,
              "companion real package reservation failed");
      if (result) (void)sb_engine_result_release(result);
      auto request = base_request;
      request.encoded_sblr_container = submission.container;
      request.encoded_execution_envelope = submission.ingress;
      request.package_reservation_handle = handle.opaque_id;
      request.reserved_payload_size = reservation.payload_size;
      request.reserved_record_count = reservation.record_count;
      request.reserved_resource_policy_generation = reservation.resource_policy_generation;
      request.reserved_payload_sha256 = reservation.payload_sha256;
      for (unsigned flags = 0; flags != 8; ++flags) {
        auto active = view;
        active.cluster_context_active = (flags & 1) != 0;
        active.cluster_transaction_active = (flags & 2) != 0;
        active.route_fence_present = (flags & 4) != 0;
        server::BindServerSblrGatewayReceiptObservation(active, &request);
        const auto admission = server::AdmitServerSblrEnvelope(request);
        const bool expected = root_kind == 3 || (root_kind != 1 && flags == 0);
        check(admission.admitted == expected, "owning command route disposition");
        if (expected && admission.admission_token) {
          const auto& token = *admission.admission_token;
          check(admission.operation_id == command.operation_id &&
                    token.operation.operation_id == command.operation_id &&
                    token.operation.opcode_code == command.opcode_code,
                "published and token identities must both identify command");
          check(token.operation.requires_security_context &&
                    token.operation.requires_cluster_authority == (root_kind == 3),
                "token authority must come from command registry row");
          const auto digest = scratchbird::core::hash::ComputeSha256Digest(submission.stream);
          check(digest.ok() && token.operation_sha256 == digest.digest &&
                    token.gateway_evidence.canonical_payload_sha256 == digest.digest &&
                    token.package_executor_evidence.canonical_payload_sha256 == digest.digest,
                "all evidence must bind full package");
          check(token.canonical_operation_bytes == submission.stream &&
                    token.canonical_container_bytes == Bytes(submission.container.begin(), submission.container.end()) &&
                    token.canonical_execution_envelope_bytes == Bytes(submission.ingress.begin(), submission.ingress.end()) &&
                    token.stream.operations.size() == with_map.operations.size() &&
                    token.reserved_record_count == with_map.operations.size(),
                "companion and original transport must remain intact");
          check(token.gateway_evidence.cluster_context_active == active.cluster_context_active &&
                    token.gateway_evidence.cluster_transaction_active == active.cluster_transaction_active &&
                    token.gateway_evidence.route_fence_present == active.route_fence_present,
                "owning receipt cluster flags must survive");
        } else if (!expected) {
          check(!admission.admission_token && !admission.diagnostics.empty() &&
                    admission.diagnostics.front().code == (flags ?
                        "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN" :
                        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"),
                "companion must not bypass cluster or CREATE INDEX evidence refusal");
        } else {
          check(false, "admitted command token missing");
        }
      }
      server::BindServerSblrGatewayReceiptObservation(view, &request);
      if (root_kind == 0 && position) {
        auto prepared = request;
        prepared.package_reservation_deferred = true;
        prepared.package_reservation_handle = 0;
        const auto preparation = server::AdmitServerSblrEnvelope(prepared);
        check(preparation.admitted && preparation.admission_token &&
                  preparation.operation_id == command.operation_id &&
                  preparation.admission_token->operation.operation_id == command.operation_id &&
                  preparation.admission_token->package_reservation_handle == 0 &&
                  preparation.admission_token->gateway_evidence.source ==
                      server::ServerSblrGatewayEvidenceSource::invalid,
              "preparation selects command without inventing execution evidence");
        for (unsigned mutation = 0; mutation != 4; ++mutation) {
          auto outer = wire::DecodeSblrContainerBytes(
              reinterpret_cast<const std::uint8_t*>(submission.container.data()),
              submission.container.size());
          Require(outer.status == wire::SblrCodecStatus::ok, "artifact container decode failed");
          sblr::SblrSourceArtifactMapV1 artifact;
          artifact.artifact_uuid = RawUuid(Text(NewUuid(platform::UuidKind::object, 703)));
          artifact.container_request_uuid = RawUuid(view.statement_uuid);
          artifact.parser_package_uuid = RawUuid(parser_uuid);
          std::copy_n(outer.container.canonical_anchor.begin() + 16, 16,
                      artifact.dialect_family_uuid.begin());
          artifact.language_tag = "en-CA";
          artifact.redaction_class = sblr::SblrSourceArtifactRedactionClassV1::none;
          artifact.decompile_policy = sblr::SblrSourceArtifactDecompilePolicyV1::diagnostic_only;
          sblr::SblrSourceArtifactSpanV1 span;
          span.source_span_id = 1;
          // QUERY_EXECUTE has no operand here: node2 must not become admitted
          // merely because the SOURCE_MAP companion carries an operand.
          span.node_id = mutation == 1 ? 2 : 1;
          span.byte_length = 1;
          span.line_start = span.line_end = 1;
          span.column_start = 1; span.column_end = 2;
          span.span_kind = sblr::SblrSourceArtifactSpanKindV1::statement;
          artifact.source_spans.push_back(span);
          if (mutation == 2) artifact.parser_package_uuid[15] ^= 1;
          if (mutation == 3) artifact.container_request_uuid[15] ^= 1;
          std::string detail;
          outer.container.source_map = sblr::EncodeSblrSourceArtifactMapV1(artifact, &detail);
          Require(!outer.container.source_map.empty(), "companion source artifact encoding failed");
          const auto bytes = wire::EncodeSblrContainer(outer.container);
          Require(!bytes.empty(), "companion artifact container encoding failed");
          auto annotated = request;
          annotated.encoded_sblr_container.assign(bytes.begin(), bytes.end());
          const auto admission = server::AdmitServerSblrEnvelope(annotated);
          check(admission.admitted == (mutation == 0), "artifact must bind owning command nodes and identity");
          if (mutation == 0 && admission.admission_token) {
            check(admission.admission_token->canonical_container_bytes == bytes,
                  "artifact transport must survive token publication");
          } else if (mutation != 0) {
            check(!admission.admission_token && !admission.diagnostics.empty() &&
                      admission.diagnostics.front().code == "SBLR.SOURCE_ARTIFACT.INVALID",
                  "invalid artifact must be refused without token publication");
          }
        }
        for (unsigned mutation = 0; mutation != 5; ++mutation) {
          auto wrong = request;
          if (mutation == 0) wrong.reserved_record_count = 3;
          if (mutation == 1) wrong.reserved_payload_sha256[0] ^= 1;
          if (mutation == 2) wrong.package_reservation_handle = 0;
          if (mutation == 3) wrong.route_snapshot_engine_owned = false;
          if (mutation == 4) wrong.security_snapshot_engine_owned = false;
          const auto rejected = server::AdmitServerSblrEnvelope(wrong);
          check(!rejected.admitted && !rejected.admission_token,
                "companion must not bypass reservation or snapshot authority");
        }
        for (unsigned mutation = 0; mutation != 5; ++mutation) {
          auto malformed = with_map;
          auto& source = malformed.operations[position];
          if (mutation == 0) source.operands.front().value_body[6] = 0x40;
          if (mutation == 1) source.result_shape = "rowset";
          if (mutation == 2) source.parser_package_version_patch++;
          if (mutation == 3) malformed.operations.insert(malformed.operations.end() - 1, map);
          if (mutation == 4) malformed.operations.insert(malformed.operations.end() - 1, command);
          const auto invalid = RepackSubmission(base, malformed);
          auto wrong = request;
          wrong.encoded_sblr_container = invalid.container;
          wrong.encoded_execution_envelope = invalid.ingress;
          wrong.package_reservation_deferred = true;
          wrong.package_reservation_handle = 0;
          const auto rejected = server::AdmitServerSblrEnvelope(wrong);
          check(!rejected.admitted && !rejected.admission_token,
                "prepare must validate companion structure before deferring reservation");
        }
      }
      Require(bridge::ReleaseStatementPackageAdmissionReservation(
                  handle, bridge::StatementPackageReservationReleaseReason::kRelease) == SB_ENGINE_STATUS_OK,
              "companion package reservation cleanup failed");
    }
  }
  std::cout << "source_map_server_admission checks=" << checks << " failures=" << failures
            << " engine_execution=false parser_ipc_e2e=false\n";
  Require(failures == 0, "companion server admission regression failed");
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> cancellation_probes{0};
  auto context = BeginTransaction(fixture, &cancellation_probes);

  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t acquire_result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &acquire_result) ==
              SB_ENGINE_STATUS_OK,
          "live statement receipt acquisition failed");
  if (acquire_result) (void)sb_engine_result_release(acquire_result);

  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 702));
  const auto submission = BuildSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = receipt;
  reservation_request.canonical_payload_bytes = submission.stream.data();
  reservation_request.canonical_payload_size = submission.stream.size();
  reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation_handle;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &reservation_handle, &reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "engine package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  server::ServerSblrAdmissionRequest request;
  request.encoded_sblr_container = submission.container;
  request.encoded_execution_envelope = submission.ingress;
  request.admitted_parser_package_uuid = parser_uuid;
  request.admitted_parser_package_version_major = 1;
  request.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  request.authenticated_principal_uuid = Text(fixture.principal_uuid);
  request.catalog_snapshot_uuid = view.statement_metadata_snapshot_uuid;
  request.engine_mga_statement_uuid = view.statement_uuid;
  request.engine_mga_snapshot_uuid = view.statement_snapshot_uuid;
  request.catalog_epoch = view.catalog_generation_id;
  request.security_epoch = view.security_epoch;
  request.resource_epoch = view.resource_epoch;
  request.route_snapshot_uuid = view.optimizer_route_snapshot_uuid;
  request.route_epoch = view.optimizer_route_epoch;
  request.route_generation = view.optimizer_route_generation;
  request.security_snapshot_uuid = view.security_context_uuid;
  request.security_observation_generation = view.security_epoch;
  request.route_snapshot_engine_owned = true;
  request.security_snapshot_engine_owned = true;
  request.package_reservation_handle = reservation_handle.opaque_id;
  request.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  request.reserved_payload_size = reservation_view.payload_size;
  request.reserved_record_count = reservation_view.record_count;
  request.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  request.reserved_payload_sha256 = reservation_view.payload_sha256;
  server::BindServerSblrGatewayReceiptObservation(view, &request);

  const auto admitted = server::AdmitServerSblrEnvelope(request);
  Require(admitted.admitted && admitted.admission_token &&
              admitted.admission_token->opcode_stream &&
              admitted.admission_token->gateway_evidence.source ==
                  server::ServerSblrGatewayEvidenceSource::local_observed &&
              admitted.admission_token->gateway_evidence.disposition ==
                  server::ServerSblrGatewayDisposition::pass_through &&
              !admitted.admission_token->gateway_evidence.cluster_context_active &&
              !admitted.admission_token->gateway_evidence.cluster_transaction_active &&
              !admitted.admission_token->gateway_evidence.route_fence_present,
          "live receipt package admission did not produce exact local gateway evidence");

  TestCompanionAdmission(fixture, view, receipt, parser_uuid, request);
  TestDispatchCommandHelpers(fixture, view, parser_uuid, request);

  const auto create_index_submission =
      BuildCreateIndexSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest
      create_index_reservation_request;
  create_index_reservation_request.receipt = receipt;
  create_index_reservation_request.canonical_payload_bytes =
      create_index_submission.stream.data();
  create_index_reservation_request.canonical_payload_size =
      create_index_submission.stream.size();
  create_index_reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle
      create_index_reservation_handle;
  bridge::StatementPackageAdmissionReservationView
      create_index_reservation_view;
  reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &create_index_reservation_request,
              &create_index_reservation_handle,
              &create_index_reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "CREATE INDEX package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  auto create_index_request = request;
  create_index_request.encoded_sblr_container =
      create_index_submission.container;
  create_index_request.encoded_execution_envelope =
      create_index_submission.ingress;
  create_index_request.package_reservation_handle =
      create_index_reservation_handle.opaque_id;
  create_index_request.reserved_payload_size =
      create_index_reservation_view.payload_size;
  create_index_request.reserved_record_count =
      create_index_reservation_view.record_count;
  create_index_request.reserved_resource_policy_generation =
      create_index_reservation_view.resource_policy_generation;
  create_index_request.reserved_payload_sha256 =
      create_index_reservation_view.payload_sha256;
  const auto create_index_refused =
      server::AdmitServerSblrEnvelope(create_index_request);
  Require(!create_index_refused.admitted &&
              !create_index_refused.admission_token &&
              !create_index_refused.diagnostics.empty() &&
              create_index_refused.diagnostics.front().code ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "canonical CREATE INDEX admission did not refuse before token publication");
  Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
          "CREATE INDEX admission consulted cancellation before evidence refusal");

  auto deferred_create_index = create_index_request;
  deferred_create_index.package_reservation_deferred = true;
  deferred_create_index.package_reservation_handle = 0;
  const auto deferred_create_index_refused =
      server::AdmitServerSblrEnvelope(deferred_create_index);
  Require(!deferred_create_index_refused.admitted &&
              !deferred_create_index_refused.admission_token &&
              !deferred_create_index_refused.diagnostics.empty() &&
              deferred_create_index_refused.diagnostics.front().code ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "deferred CREATE INDEX admission bypassed static evidence refusal");

  const auto multi_create_index_submission =
      BuildCreateIndexSubmission(fixture, view, parser_uuid, true, true);
  auto multi_create_index_request = request;
  multi_create_index_request.encoded_sblr_container =
      multi_create_index_submission.container;
  multi_create_index_request.encoded_execution_envelope =
      multi_create_index_submission.ingress;
  multi_create_index_request.package_reservation_deferred = true;
  multi_create_index_request.package_reservation_handle = 0;
  const auto multi_create_index_refused =
      server::AdmitServerSblrEnvelope(multi_create_index_request);
  Require(!multi_create_index_refused.admitted &&
              !multi_create_index_refused.admission_token &&
              !multi_create_index_refused.diagnostics.empty() &&
              multi_create_index_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "deferred multi-member CREATE INDEX bypassed standalone-root refusal");

  const auto inline_create_index_submission =
      BuildCreateIndexSubmission(fixture, view, parser_uuid, false);
  auto inline_create_index_request = request;
  inline_create_index_request.encoded_sblr_container =
      inline_create_index_submission.container;
  inline_create_index_request.encoded_execution_envelope =
      inline_create_index_submission.ingress;
  inline_create_index_request.package_reservation_handle = 0;
  const auto inline_create_index_refused =
      server::AdmitServerSblrEnvelope(inline_create_index_request);
  Require(!inline_create_index_refused.admitted &&
              !inline_create_index_refused.admission_token &&
              !inline_create_index_refused.diagnostics.empty() &&
              inline_create_index_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "inline CREATE INDEX admission bypassed package-root placement refusal");

  auto create_index_cluster_view = view;
  create_index_cluster_view.cluster_transaction_active = true;
  auto create_index_clustered = create_index_request;
  server::BindServerSblrGatewayReceiptObservation(create_index_cluster_view,
                                                   &create_index_clustered);
  const auto create_index_cluster_refused =
      server::AdmitServerSblrEnvelope(create_index_clustered);
  Require(!create_index_cluster_refused.admitted &&
              !create_index_cluster_refused.admission_token &&
              !create_index_cluster_refused.diagnostics.empty() &&
              create_index_cluster_refused.diagnostics.front().code ==
                  "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
          "cluster CREATE INDEX refusal did not precede executor evidence");

  const auto plan_import_rows_submission =
      BuildPlanImportRowsSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest
      plan_import_rows_reservation_request;
  plan_import_rows_reservation_request.receipt = receipt;
  plan_import_rows_reservation_request.canonical_payload_bytes =
      plan_import_rows_submission.stream.data();
  plan_import_rows_reservation_request.canonical_payload_size =
      plan_import_rows_submission.stream.size();
  plan_import_rows_reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle
      plan_import_rows_reservation_handle;
  bridge::StatementPackageAdmissionReservationView
      plan_import_rows_reservation_view;
  reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &plan_import_rows_reservation_request,
              &plan_import_rows_reservation_handle,
              &plan_import_rows_reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "import-plan package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  auto plan_import_rows_request = request;
  plan_import_rows_request.encoded_sblr_container =
      plan_import_rows_submission.container;
  plan_import_rows_request.encoded_execution_envelope =
      plan_import_rows_submission.ingress;
  plan_import_rows_request.package_reservation_handle =
      plan_import_rows_reservation_handle.opaque_id;
  plan_import_rows_request.reserved_payload_size =
      plan_import_rows_reservation_view.payload_size;
  plan_import_rows_request.reserved_record_count =
      plan_import_rows_reservation_view.record_count;
  plan_import_rows_request.reserved_resource_policy_generation =
      plan_import_rows_reservation_view.resource_policy_generation;
  plan_import_rows_request.reserved_payload_sha256 =
      plan_import_rows_reservation_view.payload_sha256;
  const auto plan_import_rows_admitted =
      server::AdmitServerSblrEnvelope(plan_import_rows_request);
  Require(plan_import_rows_admitted.admitted &&
              plan_import_rows_admitted.admission_token &&
              plan_import_rows_admitted.operation_id ==
                  "dml.plan_import_rows" &&
              plan_import_rows_admitted.admission_token->opcode_stream &&
              plan_import_rows_admitted.admission_token->stream.operations.size() ==
                  3,
          "canonical import plan was not admitted as one standalone package root");

  const auto multi_plan_import_rows_submission =
      BuildPlanImportRowsSubmission(fixture, view, parser_uuid, true, true);
  auto multi_plan_import_rows_request = request;
  multi_plan_import_rows_request.encoded_sblr_container =
      multi_plan_import_rows_submission.container;
  multi_plan_import_rows_request.encoded_execution_envelope =
      multi_plan_import_rows_submission.ingress;
  multi_plan_import_rows_request.package_reservation_deferred = true;
  multi_plan_import_rows_request.package_reservation_handle = 0;
  const auto multi_plan_import_rows_refused =
      server::AdmitServerSblrEnvelope(multi_plan_import_rows_request);
  Require(!multi_plan_import_rows_refused.admitted &&
              !multi_plan_import_rows_refused.admission_token &&
              !multi_plan_import_rows_refused.diagnostics.empty() &&
              multi_plan_import_rows_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "multi-member import plan bypassed standalone-root refusal");

  const auto inline_plan_import_rows_submission =
      BuildPlanImportRowsSubmission(fixture, view, parser_uuid, false);
  auto inline_plan_import_rows_request = request;
  inline_plan_import_rows_request.encoded_sblr_container =
      inline_plan_import_rows_submission.container;
  inline_plan_import_rows_request.encoded_execution_envelope =
      inline_plan_import_rows_submission.ingress;
  inline_plan_import_rows_request.package_reservation_handle = 0;
  const auto inline_plan_import_rows_refused =
      server::AdmitServerSblrEnvelope(inline_plan_import_rows_request);
  Require(!inline_plan_import_rows_refused.admitted &&
              !inline_plan_import_rows_refused.admission_token &&
              !inline_plan_import_rows_refused.diagnostics.empty() &&
              inline_plan_import_rows_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "inline import plan bypassed package-root placement refusal");

  auto plan_import_rows_cluster_view = view;
  plan_import_rows_cluster_view.cluster_transaction_active = true;
  auto plan_import_rows_clustered = plan_import_rows_request;
  server::BindServerSblrGatewayReceiptObservation(
      plan_import_rows_cluster_view, &plan_import_rows_clustered);
  const auto plan_import_rows_cluster_refused =
      server::AdmitServerSblrEnvelope(plan_import_rows_clustered);
  Require(!plan_import_rows_cluster_refused.admitted &&
              !plan_import_rows_cluster_refused.admission_token &&
              !plan_import_rows_cluster_refused.diagnostics.empty() &&
              plan_import_rows_cluster_refused.diagnostics.front().code ==
                  "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
          "cluster import plan received a local fallback");

  for (unsigned predicate = 0; predicate != 3; ++predicate) {
    auto active_view = view;
    active_view.cluster_context_active = predicate == 0;
    active_view.cluster_transaction_active = predicate == 1;
    active_view.route_fence_present = predicate == 2;
    auto clustered = request;
    server::BindServerSblrGatewayReceiptObservation(active_view, &clustered);
    const auto rejected = server::AdmitServerSblrEnvelope(clustered);
    Require(!rejected.admitted && !rejected.diagnostics.empty() &&
                rejected.diagnostics.front().code ==
                    "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
            "active engine receipt predicate did not fail closed");
  }

  auto unreserved = request;
  unreserved.package_reservation_handle = 0;
  Require(!server::AdmitServerSblrEnvelope(unreserved).admitted,
          "package admission accepted missing engine reservation");
  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "live receipt cleanup failed");
  return 0;
}
