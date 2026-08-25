// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// CSC-TEST-002337: explicit authenticated SOURCE_MAP process client.
#include "common/common.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: descriptor-client ENDPOINT DATABASE USER PASSWORD OPERATION APPLICATION\n";
    return 2;
  }
  scratchbird::parser::sbsql::ParserConfig config;
  config.server_endpoint = argv[1];
  config.database_token = argv[2];
  scratchbird::parser::sbsql::SbsqlTestWireSession session(config, nullptr, nullptr);
  scratchbird::parser::sbsql::AuthCredentialEnvelope credentials;
  credentials.provider_family = "local_password";
  credentials.principal = argv[3];
  credentials.requested_database = argv[2];
  credentials.application_name = argv[6];
  credentials.credential_evidence = argv[4];
  credentials.credential_evidence_present = true;
  scratchbird::parser::sbsql::MessageVectorSet messages;
  if (!session.AuthenticateCredentials(credentials, &messages)) {
    for (const auto& diagnostic : messages.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    return 3;
  }
  const std::string operation = argv[5];
  auto result = operation == "show-version"
                    ? session.RunShowVersionForWire()
                    : operation == "show-wait-events"
                    ? session.RunShowWaitEventsForWire()
                    : operation == "error-vector"
                    ? session.RunErrorVectorForWire()
                    : operation == "txn-commit"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto committed = session.RunPipeline("COMMIT TRANSACTION", true);
                              if (!committed.accepted) return committed;
                              const auto replay =
                                  session.RunRetiredTransactionCommitReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  replay.messages.diagnostics.front().code ==
                                      "MGA.TRANSACTION.STALE";
                              if (!stale) {
                                committed.accepted = false;
                                committed.messages.diagnostics.push_back(
                                    {"MGA.TRANSACTION.REPLAY_NOT_REFUSED", "ERROR",
                                     "Retired transaction handle replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return committed;
                            }()
                    : operation == "txn-rollback"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto rolled_back =
                                  session.RunPipeline("ROLLBACK TRANSACTION", true);
                              if (!rolled_back.accepted) return rolled_back;
                              const auto replay =
                                  session.RunRetiredTransactionRollbackReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  replay.messages.diagnostics.front().code ==
                                      "MGA.TRANSACTION.STALE";
                              if (!stale) {
                                rolled_back.accepted = false;
                                rolled_back.messages.diagnostics.push_back(
                                    {"MGA.TRANSACTION.REPLAY_NOT_REFUSED", "ERROR",
                                     "Retired transaction handle replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return rolled_back;
                            }()
                    : operation == "txn-savepoint"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto created =
                                  session.RunPipeline("SAVEPOINT alignment_point", true);
                              if (!created.accepted) return created;
                              const auto replay =
                                  session.RunRetiredSavepointReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  (replay.messages.diagnostics.front().code ==
                                       "MGA.SAVEPOINT.STALE" ||
                                   replay.messages.diagnostics.front().code ==
                                       "MGA.TRANSACTION.STALE");
                              if (!stale) {
                                created.accepted = false;
                                created.messages.diagnostics.push_back(
                                    {"MGA.SAVEPOINT.REPLAY_NOT_REFUSED", "ERROR",
                                     "Consumed savepoint descriptor replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return created;
                            }()
                    : operation == "txn-release-savepoint"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto created = session.RunPipeline(
                                  "SAVEPOINT alignment_point", true);
                              if (!created.accepted) return created;
                              auto descendant = session.RunPipeline(
                                  "SAVEPOINT descendant_point", true);
                              if (!descendant.accepted) return descendant;
                              auto released =
                                  session.RunReleaseParentSavepointForWire();
                              if (!released.accepted) return released;
                              const auto replay =
                                  session.RunReleasedSavepointReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  (replay.messages.diagnostics.front().code ==
                                       "MGA.SAVEPOINT.STALE" ||
                                   replay.messages.diagnostics.front().code ==
                                       "MGA.TRANSACTION.STALE");
                              if (!stale) {
                                released.accepted = false;
                                released.messages.diagnostics.push_back(
                                    {"MGA.SAVEPOINT.REPLAY_NOT_REFUSED", "ERROR",
                                     "Released savepoint handle replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return released;
                            }()
                    : operation == "txn-rollback-to-savepoint"
                          ? [&session] {
                              auto begun=session.RunPipeline("BEGIN TRANSACTION",true); if(!begun.accepted)return begun;
                              auto parent=session.RunPipeline("SAVEPOINT alignment_point",true); if(!parent.accepted)return parent;
                              auto child=session.RunPipeline("SAVEPOINT descendant_point",true); if(!child.accepted)return child;
                              auto rolled=session.RunRollbackParentSavepointForWire(); if(!rolled.accepted)return rolled;
                              const auto descendant_stale=session.RunRolledBackDescendantForWire();
                              const bool descendant_refused=!descendant_stale.accepted&&!descendant_stale.messages.diagnostics.empty()&&
                                  descendant_stale.messages.diagnostics.front().code=="MGA.SAVEPOINT.STALE";
                              if(!descendant_refused){rolled.accepted=false;rolled.messages.diagnostics.push_back({"MGA.SAVEPOINT.DESCENDANT_NOT_STALE","ERROR","Rolled-back descendant savepoint remained visible.","sbsql_sblr_alignment"});return rolled;}
                              const auto replay=session.RunRolledBackSavepointReplayForWire();
                              const bool stale=!replay.accepted&&!replay.messages.diagnostics.empty()&&
                                  (replay.messages.diagnostics.front().code=="MGA.SAVEPOINT.STALE"||replay.messages.diagnostics.front().code=="MGA.TRANSACTION.STALE");
                              if(!stale){rolled.accepted=false;rolled.messages.diagnostics.push_back({"MGA.SAVEPOINT.REPLAY_NOT_REFUSED","ERROR","Old rollback-to-savepoint authority was not stale.","sbsql_sblr_alignment"});}
                              return rolled;
                            }()
                    : operation == "psql-autonomous-frame"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunAutonomousFrameForWire():begun; }()
                    : operation == "transaction-reservation-release"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunReservationReleaseForWire():begun; }()
                    : operation == "temporary-instance-cleanup"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunTemporaryInstanceCleanupForWire():begun; }()
                    : operation == "cursor-open"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunCursorOpenForWire():begun; }()
                    : operation == "cursor-fetch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunCursorOpenForWire();return opened.accepted?session.RunCursorFetchForWire():opened; }()
                    : operation == "cursor-close"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunCursorOpenForWire();return opened.accepted?session.RunCursorCloseForWire():opened; }()
                    : operation == "read-by-key"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReadByKeyForWire():begun; }()
                    : operation == "read-range"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReadRangeForWire():begun; }()
                    : operation == "read-stream"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReadStreamForWire():begun; }()
                    : operation == "result-set-pass"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunResultSetPassForWire():begun; }()
                    : operation == "access-cursor-open"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAccessCursorOpenForWire():begun; }()
                    : operation == "access-cursor-fetch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunAccessCursorOpenForWire();return opened.accepted?session.RunAccessCursorFetchForWire():opened; }()
                    : operation == "access-cursor-close"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunAccessCursorOpenForWire();return opened.accepted?session.RunAccessCursorCloseForWire():opened; }()
                    : operation == "insert"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunInsertForWire():begun; }()
                    : operation == "update"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunUpdateForWire():begun; }()
                    : operation == "delete"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDeleteForWire():begun; }()
                    : operation == "merge"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunMergeForWire():begun; }()
                    : operation == "table-truncate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunTableTruncateForWire():begun; }()
                    : operation == "table-analyze"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunTableAnalyzeForWire():begun; }()
                    : operation == "bulk-import-stream"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunBulkImportStreamForWire():begun; }()
                    : operation == "bulk-export-stream"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunBulkExportStreamForWire():begun; }()
                    : operation == "statement-batch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunStatementBatchForWire():begun; }()
                    : operation == "atomic-cas"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAtomicCasForWire():begun; }()
                    : operation == "atomic-rmw"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAtomicRmwForWire():begun; }()
                    : operation == "advisory-lock"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAdvisoryLockForWire():begun; }()
                    : operation == "advisory-lock-release"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAdvisoryLockReleaseForWire():begun; }()
                    : operation == "function-call"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFunctionCallForWire():begun; }()
                    : operation == "operator-call"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunOperatorCallForWire():begun; }()
                    : operation == "cast"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunCastForWire():begun; }()
                    : operation == "compare"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunCompareForWire():begun; }()
                    : operation == "domain-operation"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDomainOperationForWire():begun; }()
                    : operation == "udr-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunUdrInvokeForWire():begun; }()
                    : operation == "procedure-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunProcedureInvokeForWire():begun; }()
                    : operation == "function-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFunctionInvokeForWire():begun; }()
                    : operation == "aggregate-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAggregateInvokeForWire():begun; }()
                    : operation == "sequence-nextval"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSequenceNextvalForWire():begun; }()
                    : operation == "sequence-currval"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSequenceCurrvalForWire():begun; }()
                    : operation == "sequence-setval"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSequenceSetvalForWire():begun; }()
                    : operation == "query-numeric"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunQueryNumericForWire():begun; }()
                    : operation == "advanced-datatype-family"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunQueryEvaluateAdvancedDatatypeFamilyForWire():begun; }()
                    : operation == "project"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunProjectForWire():begun; }()
                    : operation == "show-object-detail"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunShowObjectDetailForWire():begun; }()
                    : operation == "name-resolve"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunNameResolveForWire():begun; }()
                    : operation == "parse-text"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunParseTextForWire():begun; }()
                    : operation == "catalog-epoch-check"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunCatalogEpochCheckForWire():begun; }()
                    : operation == "database-attach"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseAttachForWire():begun; }()
                    : operation == "database-detach"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseDetachForWire():begun; }()
                    : operation == "database-checkpoint"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseCheckpointForWire():begun; }()
                    : operation == "database-vacuum"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseVacuumForWire():begun; }()
                    : operation == "aggregate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAggregateForWire():begun; }()
                    : operation == "group"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGroupForWire():begun; }()
                    : operation == "sort"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSortForWire():begun; }()
                    : operation == "limit"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLimitForWire():begun; }()
                    : operation == "window"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunWindowForWire():begun; }()
                    : operation == "return-result-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunReturnResultSetForWire():begun; }()
                    : operation == "kv-structured-read"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredReadForWire():begun; }()
                    : operation == "kv-structured-mutate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredMutateForWire():begun; }()
                    : operation == "kv-structured-scan"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredScanForWire():begun; }()
                    : operation == "kv-structured-stream-read"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredStreamReadForWire():begun; }()
                    : operation == "kv-structured-stream-append"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredStreamAppendForWire():begun; }()
                    : operation == "kv-structured-timeseries"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredTimeseriesForWire():begun; }()
                    : operation == "system-config-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSystemConfigSetForWire():begun; }()
                    : operation == "alter-gpu-profile-disable"
                          ? session.RunGpuProfileDisableRefusalForWire()
                    : operation == "ddl-create-domain"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateDomainForWire():begun; }()
                    : operation == "ddl-create-sequence"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateSequenceForWire():begun; }()
                    : operation == "ddl-create-materialized-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateMaterializedViewForWire():begun; }()
                    : operation == "ddl-create-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateViewForWire():begun; }()
                    : operation == "ddl-drop-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropViewForWire():begun; }()
                    : operation == "ddl-refresh-materialized-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlRefreshMaterializedViewForWire():begun; }()
                    : operation == "ddl-drop-synonym"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropSynonymForWire():begun; }()
                    : operation == "ddl-drop-foreign-table"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropForeignTableForWire():begun; }()
                    : operation == "ddl-drop-package"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropPackageForWire():begun; }()
                    : operation == "ddl-alter-package"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterPackageForWire():begun; }()
                    : operation == "ddl-alter-sequence"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterSequenceForWire():begun; }()
                    : operation == "ddl-drop-sequence"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropSequenceForWire():begun; }()
                    : operation == "ddl-drop-materialized-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropMaterializedViewForWire():begun; }()
                    : operation == "ddl-create-type"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTypeForWire():begun; }()
                    : operation == "ddl-create-table-as-query-with-data"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTableAsQueryWithDataForWire():begun; }()
                    : operation == "ddl-create-table-as-query-with-no-data"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTableAsQueryWithNoDataForWire():begun; }()
                    : operation == "ddl-alter-type"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterTypeForWire():begun; }()
                    : operation == "ddl-drop-type"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropTypeForWire():begun; }()
                    : operation == "ddl-drop-table"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropTableForWire():begun; }()
                    : operation == "ddl-create-trigger"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTriggerForWire():begun; }()
                    : operation == "ddl-alter-trigger"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterTriggerForWire():begun; }()
                    : operation == "ddl-drop-trigger"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropTriggerForWire():begun; }()
                    : operation == "ddl-create-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateProcedureForWire():begun; }()
                    : operation == "ddl-alter-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterProcedureForWire():begun; }()
                    : operation == "ddl-drop-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropProcedureForWire():begun; }()
                    : operation == "ddl-create-function"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateFunctionForWire():begun; }()
                    : operation == "ddl-alter-function"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterFunctionForWire():begun; }()
                    : operation == "ddl-create-package"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreatePackageForWire():begun; }()
                    : operation == "ddl-create-temporary-table"
                          ? session.RunDdlCreateTemporaryTableForWire()
                    : operation == "ddl-drop-temporary-table"
                          ? session.RunDdlDropTemporaryTableForWire()
                    : operation == "ddl-rename-object-vector"
                          ? session.RunDdlRenameObjectVectorForWire()
                    : operation == "ddl-rename-object"
                          ? session.RunDdlRenameObjectForWire()
                    : operation == "ddl-create-synonym"
                          ? session.RunDdlCreateSynonymForWire()
                    : operation == "ddl-create-foreign-table"
                                ? session.RunDdlCreateForeignTableForWire()
                    : operation == "ddl-create-fdw"
                                ? session.RunDdlCreateFdwForWire()
                    : operation == "ddl-drop-fdw"
                                ? session.RunDdlDropFdwForWire()
                    : operation == "ddl-create-or-replace-srs"
                          ? session.RunDdlCreateOrReplaceSrsForWire()
                    : operation == "ddl-drop-srs"
                          ? session.RunDdlDropSrsForWire()
                    : operation == "ddl-create-rewrite-rule"
                          ? session.RunDdlCreateRewriteRuleForWire()
                    : operation == "ddl-alter-rewrite-rule"
                          ? session.RunDdlAlterRewriteRuleForWire()
                    : operation == "ddl-drop-rewrite-rule"
                          ? session.RunDdlDropRewriteRuleForWire()
                    : operation == "ddl-validate-constraint"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlValidateConstraintForWire():begun; }()
                    : operation == "security-create-privilege-template"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreatePrivilegeTemplateForWire():begun; }()
                    : operation == "security-create-user"
                          ? session.RunSecurityCreateUserForWire()
                    : operation == "security-alter-user"
                          ? session.RunSecurityAlterUserForWire()
                    : operation == "security-create-role"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreateRoleForWire():begun; }()
                    : operation == "security-create-policy"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreatePolicyForWire():begun; }()
                    : operation == "security-drop-policy"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropPolicyForWire():begun; }()
                    : operation == "security-alter-policy"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAlterPolicyForWire():begun; }()
                    : operation == "security-drop-user"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropUserForWire():begun; }()
                    : operation == "security-authenticate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAuthenticateForWire():begun; }()
                    : operation == "security-deauthenticate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDeauthenticateForWire():begun; }()
                    : operation == "session-role-switch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionRoleSwitchForWire():begun; }()
                    : operation == "session-setting-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSettingSetForWire():begun; }()
                    : operation == "session-setting-reset"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSettingResetForWire():begun; }()
                    : operation == "session-setting-get"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSettingGetForWire():begun; }()
                    : operation == "session-default-qualifier-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionDefaultQualifierSetForWire():begun; }()
                    : operation == "session-discard"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionDiscardForWire():begun; }()
                    : operation == "session-snapshot-handle"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSnapshotHandleForWire():begun; }()
                    : operation == "context-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunContextSetForWire():begun; }()
                    : operation == "context-unset"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunContextUnsetForWire():begun; }()
                    : operation == "context-get"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunContextGetForWire():begun; }()
                    : operation == "stmt-prepare"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtPrepareCanonicalForWire():begun; }()
                    : operation == "stmt-execute"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtExecuteForWire():begun; }()
                    : operation == "stmt-execute-direct"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtExecuteDirectForWire():begun; }()
                    : operation == "stmt-free"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtFreeForWire():begun; }()
                    : operation == "stmt-cancel"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtCancelForWire():begun; }()
                    : operation == "parameter-bind"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunParameterBindForWire():begun; }()
                    : operation == "result-page"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunResultPageForWire():begun; }()
                    : operation == "query-execute"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunQueryExecuteForWire():begun; }()
                    : operation == "query-explain"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunQueryExplainForWire():begun; }()
                    : operation == "security-alter-role"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAlterRoleForWire():begun; }()
                    : operation == "security-create-group-mapping"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreateGroupMappingForWire():begun; }()
                    : operation == "security-drop-group-mapping"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropGroupMappingForWire():begun; }()
                    : operation == "security-grant"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityGrantForWire():begun; }()
                    : operation == "security-revoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityRevokeForWire():begun; }()
                    : operation == "security-drop-role"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropRoleForWire():begun; }()
                    : operation == "security-alter-privilege-template"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAlterPrivilegeTemplateForWire():begun; }()
                    : operation == "security-drop-privilege-template"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropPrivilegeTemplateForWire():begun; }()
                    : operation == "database-create-template-clone"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDatabaseCreateTemplateCloneForWire():begun; }()
                    : operation == "ddl-create-aggregate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateAggregateForWire():begun; }()
                    : operation == "ddl-create-macro"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateMacroForWire():begun; }()
                    : operation == "ddl-create-dictionary"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateDictionaryForWire():begun; }()
                    : operation == "ddl-drop-dictionary"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropDictionaryForWire():begun; }()
                    : operation == "ddl-alter-dictionary"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterDictionaryForWire():begun; }()
                    : operation == "ddl-create-continuous-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateContinuousViewForWire():begun; }()
                    : operation == "ddl-alter-continuous-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterContinuousViewForWire():begun; }()
                    : operation == "ddl-drop-continuous-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropContinuousViewForWire():begun; }()
                    : operation == "dml-async-insert-submit"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDmlAsyncInsertSubmitForWire():begun; }()
                    : operation == "dml-async-insert-status"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDmlAsyncInsertStatusForWire():begun; }()
                    : operation == "dml-counter-add"
                          ? session.RunDmlCounterAddForWire()
                    : operation == "dml-timeseries-schema-write"
                          ? session.RunDmlTimeseriesSchemaWriteForWire()
                    : operation == "ddl-timeseries-series-cardinality-policy"
                          ? session.RunDdlTimeseriesSeriesCardinalityPolicyForWire()
                    : operation == "ddl-create-timeseries-value-cache"
                          ? session.RunDdlCreateTimeseriesValueCacheForWire()
                    : operation == "dml-async-insert-cancel"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDmlAsyncInsertCancelForWire():begun; }()
                    : operation == "ddl-drop-macro"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropMacroForWire():begun; }()
                    : operation == "admin-register-external-relation-resolver"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunAdminRegisterExternalRelationResolverForWire():begun; }()
                    : operation == "admin-unregister-external-relation-resolver"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunAdminUnregisterExternalRelationResolverForWire():begun; }()
                    : operation == "ddl-alter-aggregate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterAggregateForWire():begun; }()
                    : operation == "ddl-drop-aggregate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropAggregateForWire():begun; }()
                    : operation == "ddl-purge-system-history"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlPurgeSystemHistoryForWire():begun; }()
                    : operation == "ddl-set-index-optimizer-eligibility"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlSetIndexOptimizerEligibilityForWire():begun; }()
                    : operation == "ddl-set-table-type-enforcement"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlSetTableTypeEnforcementForWire():begun; }()
                    : operation == "database-serialize-logical-snapshot"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDatabaseSerializeLogicalSnapshotForWire():begun; }()
                    : operation == "database-deserialize-logical-snapshot"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDatabaseDeserializeLogicalSnapshotForWire():begun; }()
                    : operation == "ddl-drop-function"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropFunctionForWire():begun; }()
                    : operation == "ddl-alter-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterViewForWire():begun; }()
                    : operation == "ddl-alter-domain"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterDomainForWire():begun; }()
                    : operation == "ddl-create-schema"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateSchemaForWire():begun; }()
                    : operation == "ddl-create-table"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTableForWire():begun; }()
                    : operation == "ddl-create-index"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateIndexForWire():begun; }()
                    : operation == "ddl-drop-index"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropIndexForWire():begun; }()
                    : operation == "txn-begin"
                          ? session.RunPipeline("BEGIN TRANSACTION", true)
                          : session.RunSourceMapForWire();
  if (!result.accepted) {
    if (result.messages.diagnostics.empty())
      std::cerr << "SBLR.DDL_CREATE_TYPE.EMPTY_FAILURE operation=" << operation
                << " payload_bytes=" << result.sblr_payload.size()
                << " result_bytes=" << result.server_result_payload.size() << '\n';
    for (const auto& diagnostic : result.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields)
        std::cerr << diagnostic.code << ':' << field.name << '=' << field.value << '\n';
    }
    return 4;
  }
  std::cout << (operation == "error-vector"
                    ? "CSC-TEST-002341 ERROR_VECTOR accepted\n"
                    : operation == "txn-commit"
                          ? "CSC-TEST-002349 TXN_COMMIT accepted\n"
                    : operation == "txn-rollback"
                          ? "CSC-TEST-002353 TXN_ROLLBACK accepted\n"
                    : operation == "txn-savepoint"
                          ? "CSC-TEST-002357 TXN_SAVEPOINT accepted\n"
                    : operation == "txn-release-savepoint"
                          ? "CSC-TEST-002361 TXN_RELEASE_SAVEPOINT accepted\n"
                    : operation == "txn-rollback-to-savepoint"
                          ? "CSC-TEST-002365 TXN_ROLLBACK_TO_SAVEPOINT accepted\n"
                    : operation == "psql-autonomous-frame"
                          ? "CSC-TEST-002369 PSQL_AUTONOMOUS_FRAME accepted\n"
                    : operation == "transaction-reservation-release"
                          ? "CSC-TEST-002373 TRANSACTION_RESERVATION_RELEASE accepted\n"
                    : operation == "temporary-instance-cleanup"
                          ? "CSC-TEST-002377 TEMPORARY_INSTANCE_CLEANUP accepted\n"
                    : operation == "cursor-open"
                          ? "CSC-TEST-002381 CURSOR_OPEN accepted\n"
                    : operation == "cursor-fetch"
                          ? "CSC-TEST-002385 CURSOR_FETCH accepted\n"
                    : operation == "cursor-close"
                          ? "CSC-TEST-002389 CURSOR_CLOSE accepted\n"
                    : operation == "read-by-key"
                          ? "CSC-TEST-002393 READ_BY_KEY accepted\n"
                    : operation == "read-range"
                          ? "CSC-TEST-002397 READ_RANGE accepted\n"
                    : operation == "read-stream"
                          ? "CSC-TEST-002401 READ_STREAM accepted\n"
                    : operation == "result-set-pass"
                          ? "CSC-TEST-002405 RESULT_SET_PASS accepted\n"
                    : operation == "access-cursor-open"
                          ? "CSC-TEST-002409 ACCESS_CURSOR_OPEN accepted\n"
                    : operation == "access-cursor-fetch"
                          ? "CSC-TEST-002413 ACCESS_CURSOR_FETCH accepted\n"
                    : operation == "access-cursor-close"
                          ? "CSC-TEST-002417 ACCESS_CURSOR_CLOSE accepted\n"
                    : operation == "ddl-create-temporary-table"
                          ? "CSC-TEST-002661 DDL_CREATE_TEMPORARY_TABLE accepted\n"
                    : operation == "ddl-drop-temporary-table"
                          ? "CSC-TEST-002665 DDL_DROP_TEMPORARY_TABLE accepted\n"
                    : operation == "alter-gpu-profile-disable"
                          ? "CSC-TEST-002913 ALTER_GPU_PROFILE_DISABLE deterministic_refusal\n"
                    : operation == "insert"
                          ? "CSC-TEST-002421 INSERT accepted\n"
                    : operation == "update"
                          ? "CSC-TEST-002425 UPDATE accepted\n"
                    : operation == "delete"
                          ? "CSC-TEST-002429 DELETE accepted\n"
                    : operation == "merge"
                          ? "CSC-TEST-002433 MERGE accepted\n"
                    : operation == "txn-begin"
                          ? "CSC-TEST-002345 TXN_BEGIN accepted\n"
                    : "CSC-TEST-002337 SOURCE_MAP accepted\n");
  return 0;
}
