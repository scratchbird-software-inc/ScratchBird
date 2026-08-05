// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::parser::ipc {

// Family-neutral parser/server transport configuration.  It intentionally
// contains no grammar, binder, lowering, cache, renderer, or dialect-default
// behavior.  Parser families supply their own values.
struct ParserClientConfig {
  std::string database_token;
  std::string server_endpoint;
  std::string dialect;
  std::string profile_id;
  std::string default_language_profile;
  std::string input_syntax_profile;
  std::string common_resource_hash;
  std::string resource_compatibility_identity;
  std::string resource_version_identity;
  std::string dialect_profile_uuid;
  std::vector<std::string> default_search_path{"sys", "public"};
  std::uint32_t registry_version{1};
  // Opt-in keeps the legacy hello byte-for-byte unchanged for every existing
  // parser.  A parser that routes independent transactions must require the
  // V2 capability during hello and fail closed if the server omits it.
  bool require_transaction_routing_v2{false};
  // An opted-in parser may execute an engine-owned prepared object on a
  // different exact transaction selector from the selector used at prepare.
  // The transfer binding is opaque and server-owned: no metadata snapshot,
  // catalog generation, or transfer token is exposed to the parser.
  bool require_prepared_metadata_transfer_v1{false};
  // Persisted relation projection has an independent capability bit.  It
  // implies exact V2 transaction routing but remains opt-in so legacy parser
  // clients preserve their existing HELLO bytes and server contract.
  bool require_relation_descriptor_projection_v3{false};
};

// A transaction selector is an opaque routing identity published by the
// engine.  Parser families may retain and return it, but must never synthesize
// either component or treat it as transaction authority.
struct ParserTransactionSelector {
  std::uint64_t local_transaction_id{0};
  std::string transaction_uuid;

  [[nodiscard]] bool present() const {
    return local_transaction_id != 0 && !transaction_uuid.empty();
  }
};

enum class ParserTransactionRoute : std::uint8_t {
  // Source- and wire-compatible behavior for SBPS V1 callers.  The server
  // selects the session's engine-owned default transaction.
  kLegacyDefault = 0,
  // Route the request through the exact engine-issued selector supplied by
  // the caller.  Both the id and UUID must match the owning session.
  kSelected = 1,
  // Ask the engine to create one additional independent transaction in the
  // existing session.  No caller-supplied selector is accepted for this mode.
  kBeginAdditional = 2,
};

struct ParserTransactionRouting {
  ParserTransactionRoute route{ParserTransactionRoute::kLegacyDefault};
  ParserTransactionSelector selector;
};

// Bounded parser projection of one engine-issued statement context. The
// private receipt, complete visibility vector, resource policy, and optimizer
// state remain server/engine-owned and are intentionally absent.
struct ParserStatementContext {
  struct DescriptorProfile {
    std::uint8_t profile_kind{0};
    std::uint16_t slot{0};
    std::string descriptor_uuid;
    std::string type_uuid;
    std::string collation_uuid;
    bool nullable{false};
    std::uint32_t width{0};
    std::uint32_t precision{0};
    std::uint32_t scale{0};
  };

  bool acquired{false};
  std::string statement_uuid;
  ParserTransactionSelector transaction;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::string bound_ast_uuid;
  std::string count_function_uuid;
  std::string sum_function_uuid;
  std::string avg_function_uuid;
  std::string min_function_uuid;
  std::string max_function_uuid;
  std::vector<DescriptorProfile> descriptor_profiles;

  [[nodiscard]] bool complete() const {
    return acquired && transaction.present() && !statement_uuid.empty() &&
           !statement_snapshot_uuid.empty() &&
           !statement_metadata_snapshot_uuid.empty() &&
           !catalog_epoch_uuid.empty() && !security_context_uuid.empty();
  }
};

// Exact canonical ingress bytes for one engine-issued statement identity.
// The private receipt never crosses SBPS and is intentionally absent here.
struct ParserCanonicalSblrSubmission {
  std::string statement_uuid;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;

  [[nodiscard]] bool complete() const {
    return !statement_uuid.empty() && !canonical_container_bytes.empty() &&
           !canonical_execution_envelope_bytes.empty();
  }
};

enum class ParserTransactionFinality : std::uint8_t {
  kNotApplicable = 0,
  kKnownApplied = 1,
  kKnownNotApplied = 2,
  kUnknown = 3,
};

enum class ParserTransactionReplacementReason : std::uint8_t {
  kNone = 0,
  kRetaining = 1,
  kLastActiveReady = 2,
  kAutocommitReady = 3,
};

// Server-published session state used only to frame SBPS requests.  The engine
// remains the authority for transaction identity, visibility, and finality.
struct ParserSessionContext {
  bool authenticated{false};
  bool transaction_routing_v2_negotiated{false};
  bool prepared_metadata_transfer_v1_negotiated{false};
  bool relation_descriptor_projection_v3_negotiated{false};
  // Copied from the exact HELLO bytes used on this physical parser channel.
  // These fields are identity evidence only; the server independently keeps
  // and cross-checks the admitted values.
  std::string admitted_parser_package_uuid;
  std::string admitted_dialect_profile_uuid;
  std::uint32_t admitted_parser_package_version_major{0};
  std::uint32_t admitted_parser_package_version_minor{0};
  std::uint32_t admitted_parser_package_version_patch{0};
  std::string session_uuid;
  std::string connection_uuid;
  std::string database_uuid;
  std::string authenticated_user_uuid;
  std::string principal_claim;
  std::string auth_provider_family;
  std::vector<std::string> effective_role_uuids;
  std::vector<std::string> effective_group_uuids;
  std::string default_language{"en"};
  std::string language_profile;
  std::string language_tag{"en"};
  std::string input_syntax_profile;
  std::string input_language_fallback_tag;
  std::string common_resource_hash;
  std::string dialect_profile_uuid;
  std::string policy_profile_uuid{"default"};
  std::string resource_compatibility_identity;
  std::string resource_version_identity;
  std::uint64_t language_resource_epoch{0};
  std::uint64_t localized_name_epoch{0};
  std::uint64_t message_resource_epoch{0};
  std::uint64_t udr_epoch{0};
  std::vector<std::string> search_path;
  std::string transaction_context;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::string transaction_uuid;
  std::string transaction_timestamp;
  std::uint64_t security_policy_epoch{0};
  std::uint64_t grant_epoch{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::string result_rendering_policy{"default"};
  std::string metric_redaction_policy{"default"};
};

} // namespace scratchbird::parser::ipc
