// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "firebird_global_aggregate_projection.hpp"
#include "firebird_relation_projection_view.hpp"
#include "firebird_scalar_projection.hpp"
#include "parser_server_client.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {

struct FirebirdDerivedTableDiagnostic;
struct ParseResult;

// Standalone Firebird presentation of a neutral, persisted relation
// descriptor.  This value is deliberately detached from worker session state
// so a fresh attachment can describe an existing physical relation without a
// parser-owned DDL overlay.
struct FirebirdPersistedColumnMetadata {
  std::string name;
  std::string column_uuid;
  std::string type_descriptor_uuid;
  std::string canonical_type_name;
  std::string charset_uuid;
  std::string collation_uuid;
  std::string character_set;
  std::string collation;
  std::uint32_t sql_type{0};
  std::uint32_t length{0};
  std::uint32_t character_length{0};
  bool nullable{true};
};

struct FirebirdPersistedRelationMetadata {
  bool present{false};
  std::string name;
  std::string descriptor_uuid;
  std::string relation_uuid;
  std::uint64_t descriptor_generation{0};
  std::uint64_t validated_resource_epoch{0};
  std::vector<FirebirdPersistedColumnMetadata> columns;
};

// Family-private wire-boundary values for an admitted constant scalar
// projection. The engine remains the value/execution authority; the Firebird
// worker only validates and renders the complete neutral result packet.
struct FirebirdScalarProjectionWireCell {
  bool is_null{false};
  std::string text;
  bool has_blob_id{false};
  std::uint32_t blob_id_high{0};
  std::uint32_t blob_id_low{0};
};

struct FirebirdScalarProjectionWireRow {
  std::vector<FirebirdScalarProjectionWireCell> cells;
};

// Family-private presentation descriptor projected from a bound scalar item.
// This is Firebird wire metadata only; it carries no catalog or execution
// authority.  The seam keeps exact SQLDA assertions independent from the
// worker's internal session structures.
struct FirebirdScalarProjectionWireDescriptor {
  std::string name;
  std::string source_name;
  std::string relation;
  std::string owner;
  std::uint32_t sql_type{0};
  std::uint32_t length{0};
  std::string character_set;
  std::uint32_t character_length{0};
  std::int16_t scale{0};
  std::int16_t subtype{0};
  bool nullable{true};
};

// Firebird-owned rendering of one exact, structured neutral-engine
// conversion-input diagnostic. The worker does not inspect source SQL or
// diagnostic prose; it consumes only the public diagnostic code and bounded
// `conversion_input_text` field.
struct FirebirdConversionErrorPresentation {
  std::string conversion_input_text;
  std::string response_json;
  std::vector<std::uint8_t> encoded_status_vector;
};

// Strict JSON-string encoding used only by the structured conversion
// diagnostic presenter. Valid UTF-8 is preserved, every C0 control is emitted
// as a JSON Unicode escape, and malformed UTF-8 is rejected.
std::optional<std::string>
EscapeFirebirdConversionDiagnosticJsonString(std::string_view text);

std::optional<FirebirdConversionErrorPresentation>
PresentFirebirdConversionInputDiagnostic(
    const ipc::MessageVectorSet& messages,
    std::string_view operation);

// Exact Firebird status-vector presentation for the registered structured FK
// diagnostic. Any missing, duplicate, reordered, or invalid field refuses.
std::optional<std::vector<std::uint8_t>>
RenderFirebirdForeignKeyDiagnosticPacket(
    const ipc::MessageVectorSet& messages,
    std::string_view operation);

// Exact Firebird native status-vector presentation for a standalone binder
// refusal. The binder result is final for parsing only; no engine request or
// SBLR execution is permitted for this path.
std::optional<std::vector<std::uint8_t>>
RenderFirebirdDerivedTableDiagnosticPacket(
    const FirebirdDerivedTableDiagnostic& diagnostic,
    std::string_view operation);

// Production parse-failure dispatch keyed by Firebird wire opcode. Only the
// three statement surfaces that can carry parser SQL are admitted. A returned
// packet is terminal for that request and must be written before any engine
// route is considered.
std::optional<std::vector<std::uint8_t>>
DispatchFirebirdParserFailurePacketForOpcode(
    std::uint32_t opcode,
    const ParseResult& parsed);

// True only when worker-side SQLDA preparation may project a physical source
// relation for a SELECT-family statement.  DML statements containing FROM
// (notably DELETE) must reach their own standalone binder instead.
bool FirebirdStatementRequiresPhysicalSelectDescriptor(
    std::string_view sql_text);

std::vector<FirebirdScalarProjectionWireDescriptor>
DescribeFirebirdScalarProjectionWireDescriptors(
    const FirebirdScalarProjectionRoute& route);

// Validates and performs presentation-only decoding for an already evaluated
// neutral scalar result. Recognized constant scalar routes require exactly one
// complete row.
bool DecodeFirebirdScalarProjectionRows(
    const FirebirdScalarProjectionRoute& route,
    std::vector<FirebirdScalarProjectionWireRow>* rows,
    std::string* diagnostic);

// Cross-checks the decoded packet against engine execution metadata. The
// execute response may publish a total cardinality of one while withholding
// that row behind the engine's normal deferred cursor transport; the fetched
// scalar packet itself must contain exactly one row and report that the cursor
// is exhausted.
bool ValidateFirebirdScalarProjectionCompletePacket(
    const FirebirdScalarProjectionRoute& route,
    std::size_t decoded_row_count,
    std::uint64_t server_row_count,
    bool cursor_present,
    std::string* diagnostic);

// Exact Firebird SQLDA projection for the canonical three-output global COUNT
// route.  These are presentation descriptors only; values remain engine-owned.
std::vector<FirebirdScalarProjectionWireDescriptor>
DescribeFirebirdGlobalCountProjectionWireDescriptors(
    const FirebirdGlobalCountProjectionRoute& route);

// Validates the complete neutral one-row packet and its engine evidence before
// any row is exposed to Firebird fetch.  On refusal `rows` is always cleared.
bool ValidateFirebirdGlobalCountProjectionCompletePacket(
    const FirebirdGlobalCountProjectionRoute& route,
    std::string_view server_result_payload,
    const std::vector<FirebirdScalarProjectionWireDescriptor>& descriptors,
    std::vector<FirebirdScalarProjectionWireRow>* rows,
    std::uint64_t server_row_count,
    bool cursor_present,
    std::string* diagnostic);

// Exact nullable SQLDA projection for direct-relation AVG. The source
// descriptor selects SQL_INT64 or SQL_DOUBLE presentation; no value is
// calculated by the worker.
std::vector<FirebirdScalarProjectionWireDescriptor>
DescribeFirebirdGlobalAvgProjectionWireDescriptors(
    const FirebirdGlobalAvgProjectionRoute& route,
    FirebirdGlobalAvgResultKind result_kind);

bool ValidateFirebirdGlobalAvgProjectionCompletePacket(
    const FirebirdGlobalAvgProjectionRoute& route,
    FirebirdGlobalAvgResultKind result_kind,
    std::string_view server_result_payload,
    const std::vector<FirebirdScalarProjectionWireDescriptor>& descriptors,
    std::vector<FirebirdScalarProjectionWireRow>* rows,
    std::uint64_t server_row_count,
    bool cursor_present,
    std::string* diagnostic);

// Validates the complete engine-owned rowset for the bounded persisted
// relation view.  SQLDA names/nullability come only from the current rpvs1
// semantic descriptor; values come only from this engine packet.
bool ValidateFirebirdRelationProjectionViewCompletePacket(
    const FirebirdRelationProjectionViewSelectRoute& route,
    const std::vector<FirebirdRelationProjectionViewOutputDescriptor>& outputs,
    std::string_view view_uuid,
    std::string_view view_descriptor_uuid,
    std::uint64_t view_descriptor_generation,
    std::string_view server_result_payload,
    std::vector<FirebirdScalarProjectionWireRow>* rows,
    std::uint64_t server_row_count,
    bool cursor_present,
    std::string* diagnostic);

FirebirdPersistedRelationMetadata ProjectFirebirdPersistedRelationMetadata(
    const ipc::PublicNameResolutionResult& resolved,
    std::string_view fallback_name = {});

int ServeFirebirdWorkerSession(int fd);

} // namespace scratchbird::parser::firebird
