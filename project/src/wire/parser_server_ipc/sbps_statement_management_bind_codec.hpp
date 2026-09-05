#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::wire::sbps_statement_management {

using Uuid = std::array<std::uint8_t, 16>;
using Hash256 = std::array<std::uint8_t, 32>;

struct PrepareBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::vector<std::uint8_t> declared_parameter_type_demands;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  Hash256 canonical_container_sha256{};
  Hash256 canonical_execution_envelope_sha256{};
  Hash256 request_evidence_sha256{};
};

struct PrepareBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid statement_name_uuid{};
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-to-engine handoff for EXECUTE DIRECT.  The parser supplies
// only canonical carriers already compiled under the authenticated receipt;
// every execution, result, and descriptor identity in the acknowledgement is
// engine-issued.
struct ExecuteDirectBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_parameter_bytes;
  Hash256 canonical_container_sha256{};
  Hash256 canonical_execution_envelope_sha256{};
  Hash256 canonical_parameter_sha256{};
  Hash256 request_evidence_sha256{};
};

struct ExecuteDirectBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid result_descriptor_uuid{};
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-to-engine handoff for EXPLAIN QUERY. The parser supplies one
// already canonical, compile-only query carrier plus rendering-only options.
// The engine rebinds the carrier to the same live receipt and owns every plan,
// redaction, snapshot, and result identity returned by the public SBEQ/SBXD
// coordination pair.
struct QueryExplainBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  bool verbose = false;
  std::uint8_t format = 1;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  Hash256 canonical_container_sha256{};
  Hash256 canonical_execution_envelope_sha256{};
  Hash256 request_evidence_sha256{};
};

struct QueryExplainBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid explain_uuid{};
  Hash256 canonical_query_sblr_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

struct NameResolveNameAtomV1 {
  std::string raw_utf8;
  bool quoted = false;
};

// Private syntax-only handoff for NAME RESOLVE. The request contains source
// identifier atoms and an optional namespace constraint only. Object UUIDs,
// namespace UUIDs, generations, redaction, and resolution identity are
// derived by the engine and remain attached to the live receipt.
struct NameResolveBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint8_t resolution_mode = 0;
  std::uint8_t object_class = 0;
  std::vector<NameResolveNameAtomV1> target_name_atoms;
  std::vector<NameResolveNameAtomV1> namespace_name_atoms;
  Hash256 target_name_atoms_sha256{};
  Hash256 namespace_name_atoms_sha256{};
  Hash256 request_evidence_sha256{};
};

struct NameResolveBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid resolution_uuid{};
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-only handoff for CATALOG EPOCH CHECK. An empty atom vector
// selects the authenticated database catalog. A non-empty vector selects one
// visible catalog object. Epochs, object identities, schema-tree identity,
// and visibility hashes are always produced by the engine.
using CatalogEpochCheckNameAtomV1 = NameResolveNameAtomV1;

struct CatalogEpochCheckBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  bool object_scoped = false;
  std::vector<CatalogEpochCheckNameAtomV1> target_name_atoms;
  Hash256 target_name_atoms_sha256{};
  Hash256 request_evidence_sha256{};
};

struct CatalogEpochCheckBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid check_uuid{};
  Uuid object_uuid{};
  std::uint64_t object_generation = 0;
  Uuid schema_tree_uuid{};
  std::uint64_t schema_tree_generation = 0;
  Hash256 visibility_scope_sha256{};
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-only handoff for DATABASE ATTACH REGISTERED. The parser may
// present only a storage-reference spelling, an alias spelling, and the
// closed access mode. Storage/filespace, database, catalog, transaction, and
// descriptor identities are resolved and frozen by the engine under the
// authenticated receipt.
using DatabaseAttachNameAtomV1 = NameResolveNameAtomV1;

struct DatabaseAttachBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint8_t mode = 0;
  std::uint8_t alias_scope = 0;
  DatabaseAttachNameAtomV1 storage_reference;
  DatabaseAttachNameAtomV1 database_alias;
  Hash256 request_evidence_sha256{};
};

struct DatabaseAttachBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid attach_uuid{};
  Uuid storage_uuid{};
  Uuid alias_uuid{};
  Uuid database_uuid{};
  Uuid catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-to-engine handoff for PARSE TEXT. Raw input text and the
// parser-produced nested carriers are confined to this authenticated bind
// request. The public SBTQ/SPTD coordination path carries only engine-frozen
// identities, hashes, limits, and the canonical nested SBLR bytes.
struct ParseTextBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::string language_profile_id;
  std::string canonical_input_utf8;
  std::uint32_t requested_maximum_bytes = 0;
  std::uint16_t requested_maximum_depth = 0;
  bool allow_donor_extensions = false;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  Hash256 canonical_input_sha256{};
  Hash256 canonical_container_sha256{};
  Hash256 canonical_execution_envelope_sha256{};
  Hash256 request_evidence_sha256{};
};

struct ParseTextBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid parse_uuid{};
  Hash256 descriptor_sha256{};
  Hash256 canonical_input_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

struct FreeBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  Hash256 request_evidence_sha256{};
};

struct FreeBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid statement_uuid{};
  Uuid statement_name_uuid{};
  std::uint64_t prepared_generation = 0;
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-only handoff for CANCEL STATEMENT. The parser supplies the
// authenticated receipt, exact statement name, and closed reason/mode
// spelling. Target execution, statement, receipt, transaction, generation,
// and cancel-operation identities are all engine-issued in the ACK.
struct CancelBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::uint8_t reason = 0;
  std::uint8_t mode = 0;
  std::uint64_t deadline_monotonic_ns = 0;
  Hash256 request_evidence_sha256{};
};

struct CancelBindAckV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  Uuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  Uuid target_execution_uuid{};
  Uuid target_statement_uuid{};
  Uuid target_statement_receipt_uuid{};
  Uuid cancel_operation_uuid{};
  Uuid target_transaction_uuid{};
  std::uint64_t target_execution_generation = 0;
  std::uint8_t reason = 0;
  std::uint8_t mode = 0;
  std::uint64_t deadline_monotonic_ns = 0;
  std::uint64_t executor_availability_generation = 0;
  Hash256 descriptor_sha256{};
  Hash256 request_evidence_sha256{};
  Hash256 acknowledgement_evidence_sha256{};
};

// Private syntax-to-engine handoff for BIND PARAMETERS.  The parser may copy
// only identities previously issued with the sealed prepared parameter
// template and one canonical SBPV value vector.  Current execution, receipt,
// catalog/MGA snapshots, and executor availability are supplied by the engine
// in the public SBKD descriptor returned by the bind bridge.
struct ParameterBindRequestV1 {
  Uuid authenticated_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  Uuid prepared_statement_uuid{};
  std::uint64_t prepared_generation = 0;
  Uuid parameter_set_uuid{};
  std::uint64_t parameter_set_generation = 0;
  Hash256 ordered_slot_table_sha256{};
  Uuid batch_uuid{};
  std::uint64_t batch_generation = 0;
  Uuid dynamic_package_uuid{};
  std::uint64_t dynamic_generation = 0;
  std::uint32_t value_count = 0;
  std::vector<std::uint8_t> canonical_value_vector;
  Hash256 value_vector_sha256{};
  Hash256 request_evidence_sha256{};
};

bool EncodePrepareBindRequestV1(const PrepareBindRequestV1&,
                                std::vector<std::uint8_t>*,
                                std::string* detail = nullptr);
bool DecodePrepareBindRequestV1(const std::uint8_t*, std::size_t,
                                PrepareBindRequestV1*,
                                std::string* detail = nullptr);
bool EncodePrepareBindAckV1(const PrepareBindAckV1&,
                            std::vector<std::uint8_t>*,
                            std::string* detail = nullptr);
bool DecodePrepareBindAckV1(const std::uint8_t*, std::size_t,
                            PrepareBindAckV1*,
                            std::string* detail = nullptr);
bool EncodeExecuteDirectBindRequestV1(const ExecuteDirectBindRequestV1&,
                                      std::vector<std::uint8_t>*,
                                      std::string* detail = nullptr);
bool DecodeExecuteDirectBindRequestV1(const std::uint8_t*, std::size_t,
                                      ExecuteDirectBindRequestV1*,
                                      std::string* detail = nullptr);
bool EncodeExecuteDirectBindAckV1(const ExecuteDirectBindAckV1&,
                                  std::vector<std::uint8_t>*,
                                  std::string* detail = nullptr);
bool DecodeExecuteDirectBindAckV1(const std::uint8_t*, std::size_t,
                                  ExecuteDirectBindAckV1*,
                                  std::string* detail = nullptr);
bool EncodeQueryExplainBindRequestV1(const QueryExplainBindRequestV1&,
                                     std::vector<std::uint8_t>*,
                                     std::string* detail = nullptr);
bool DecodeQueryExplainBindRequestV1(const std::uint8_t*, std::size_t,
                                     QueryExplainBindRequestV1*,
                                     std::string* detail = nullptr);
bool EncodeQueryExplainBindAckV1(const QueryExplainBindAckV1&,
                                 std::vector<std::uint8_t>*,
                                 std::string* detail = nullptr);
bool DecodeQueryExplainBindAckV1(const std::uint8_t*, std::size_t,
                                 QueryExplainBindAckV1*,
                                 std::string* detail = nullptr);
bool EncodeNameResolveBindRequestV1(const NameResolveBindRequestV1&,
                                    std::vector<std::uint8_t>*,
                                    std::string* detail = nullptr);
bool DecodeNameResolveBindRequestV1(const std::uint8_t*, std::size_t,
                                    NameResolveBindRequestV1*,
                                    std::string* detail = nullptr);
bool EncodeNameResolveBindAckV1(const NameResolveBindAckV1&,
                                std::vector<std::uint8_t>*,
                                std::string* detail = nullptr);
bool DecodeNameResolveBindAckV1(const std::uint8_t*, std::size_t,
                                NameResolveBindAckV1*,
                                std::string* detail = nullptr);
bool EncodeCatalogEpochCheckBindRequestV1(
    const CatalogEpochCheckBindRequestV1&, std::vector<std::uint8_t>*,
    std::string* detail = nullptr);
bool DecodeCatalogEpochCheckBindRequestV1(
    const std::uint8_t*, std::size_t, CatalogEpochCheckBindRequestV1*,
    std::string* detail = nullptr);
bool EncodeCatalogEpochCheckBindAckV1(
    const CatalogEpochCheckBindAckV1&, std::vector<std::uint8_t>*,
    std::string* detail = nullptr);
bool DecodeCatalogEpochCheckBindAckV1(
    const std::uint8_t*, std::size_t, CatalogEpochCheckBindAckV1*,
    std::string* detail = nullptr);
bool EncodeDatabaseAttachBindRequestV1(
    const DatabaseAttachBindRequestV1&, std::vector<std::uint8_t>*,
    std::string* detail = nullptr);
bool DecodeDatabaseAttachBindRequestV1(
    const std::uint8_t*, std::size_t, DatabaseAttachBindRequestV1*,
    std::string* detail = nullptr);
bool EncodeDatabaseAttachBindAckV1(
    const DatabaseAttachBindAckV1&, std::vector<std::uint8_t>*,
    std::string* detail = nullptr);
bool DecodeDatabaseAttachBindAckV1(
    const std::uint8_t*, std::size_t, DatabaseAttachBindAckV1*,
    std::string* detail = nullptr);
bool EncodeParseTextBindRequestV1(const ParseTextBindRequestV1&,
                                  std::vector<std::uint8_t>*,
                                  std::string* detail = nullptr);
bool DecodeParseTextBindRequestV1(const std::uint8_t*, std::size_t,
                                  ParseTextBindRequestV1*,
                                  std::string* detail = nullptr);
bool EncodeParseTextBindAckV1(const ParseTextBindAckV1&,
                              std::vector<std::uint8_t>*,
                              std::string* detail = nullptr);
bool DecodeParseTextBindAckV1(const std::uint8_t*, std::size_t,
                              ParseTextBindAckV1*,
                              std::string* detail = nullptr);
bool EncodeFreeBindRequestV1(const FreeBindRequestV1&,
                             std::vector<std::uint8_t>*,
                             std::string* detail = nullptr);
bool DecodeFreeBindRequestV1(const std::uint8_t*, std::size_t,
                             FreeBindRequestV1*,
                             std::string* detail = nullptr);
bool EncodeFreeBindAckV1(const FreeBindAckV1&,
                         std::vector<std::uint8_t>*,
                         std::string* detail = nullptr);
bool DecodeFreeBindAckV1(const std::uint8_t*, std::size_t,
                         FreeBindAckV1*,
                         std::string* detail = nullptr);
bool EncodeCancelBindRequestV1(const CancelBindRequestV1&,
                               std::vector<std::uint8_t>*,
                               std::string* detail = nullptr);
bool DecodeCancelBindRequestV1(const std::uint8_t*, std::size_t,
                               CancelBindRequestV1*,
                               std::string* detail = nullptr);
bool EncodeCancelBindAckV1(const CancelBindAckV1&,
                           std::vector<std::uint8_t>*,
                           std::string* detail = nullptr);
bool DecodeCancelBindAckV1(const std::uint8_t*, std::size_t,
                           CancelBindAckV1*,
                           std::string* detail = nullptr);
bool EncodeParameterBindRequestV1(const ParameterBindRequestV1&,
                                  std::vector<std::uint8_t>*,
                                  std::string* detail = nullptr);
bool DecodeParameterBindRequestV1(const std::uint8_t*, std::size_t,
                                  ParameterBindRequestV1*,
                                  std::string* detail = nullptr);

Hash256 PrepareBindRequestEvidenceV1(const PrepareBindRequestV1&);
Hash256 PrepareBindAckEvidenceV1(const PrepareBindAckV1&);
Hash256 ExecuteDirectBindRequestEvidenceV1(
    const ExecuteDirectBindRequestV1&);
Hash256 ExecuteDirectBindAckEvidenceV1(const ExecuteDirectBindAckV1&);
Hash256 QueryExplainBindRequestEvidenceV1(
    const QueryExplainBindRequestV1&);
Hash256 QueryExplainBindAckEvidenceV1(const QueryExplainBindAckV1&);
Hash256 NameResolveBindRequestEvidenceV1(
    const NameResolveBindRequestV1&);
Hash256 NameResolveBindAckEvidenceV1(const NameResolveBindAckV1&);
Hash256 CatalogEpochCheckBindRequestEvidenceV1(
    const CatalogEpochCheckBindRequestV1&);
Hash256 CatalogEpochCheckBindAckEvidenceV1(
    const CatalogEpochCheckBindAckV1&);
Hash256 DatabaseAttachBindRequestEvidenceV1(
    const DatabaseAttachBindRequestV1&);
Hash256 DatabaseAttachBindAckEvidenceV1(
    const DatabaseAttachBindAckV1&);
Hash256 ParseTextBindRequestEvidenceV1(const ParseTextBindRequestV1&);
Hash256 ParseTextBindAckEvidenceV1(const ParseTextBindAckV1&);
Hash256 FreeBindRequestEvidenceV1(const FreeBindRequestV1&);
Hash256 FreeBindAckEvidenceV1(const FreeBindAckV1&);
Hash256 CancelBindRequestEvidenceV1(const CancelBindRequestV1&);
Hash256 CancelBindAckEvidenceV1(const CancelBindAckV1&);
Hash256 ParameterBindRequestEvidenceV1(const ParameterBindRequestV1&);

}  // namespace scratchbird::wire::sbps_statement_management
