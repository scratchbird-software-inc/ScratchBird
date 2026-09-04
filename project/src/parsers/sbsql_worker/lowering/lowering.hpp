// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "binder/binder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {

struct SblrOperand {
  std::string type;
  std::string name;
  std::string value;
};

struct SblrEnvelope {
  std::string operation_family;
  std::uint32_t envelope_version{3};
  std::uint64_t statement_hash{0};
  std::string surface_key;
  std::string command_family;
  std::string operation_id;
  std::string sblr_operation_key;
  std::string sblr_opcode;
  std::string engine_api_operation_id;
  std::string engine_api_function;
  std::string lifecycle_mapping_key;
  std::string result_shape_key;
  std::string diagnostic_shape_key;
  std::string resource_contract_key;
  std::string trace_key;
  std::string source_artifact_policy{"span_metadata_only"};
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_policy_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::vector<std::string> resolved_object_uuids;
  std::vector<std::string> descriptor_refs;
  std::vector<std::string> policy_refs;
  std::vector<std::string> required_rights;
  std::vector<std::string> required_authority_steps;
  std::vector<SblrOperand> operands;
  bool lifecycle_mapping{false};
  bool exact_emulated_diagnostic{false};
  bool real_file_effects{false};
  bool parser_executes_sql{false};
  std::string payload;
  MessageVectorSet messages;
};

struct SblrVerifierResult {
  bool admitted{false};
  std::size_t validated_relational_node_count{0};
  std::size_t validated_relational_expression_count{0};
  std::size_t validated_relational_record_count{0};
  std::size_t maximum_observed_relational_depth{0};
  std::size_t maximum_observed_expression_depth{0};
  MessageVectorSet messages;
};

enum class CentralImportCommandDisposition {
  kNotApplicable,
  kBulkImportStream,
  kExactRefusal,
};

struct CentralImportCommandRoute {
  CentralImportCommandDisposition disposition{
      CentralImportCommandDisposition::kNotApplicable};
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view diagnostic_id;
};

struct CanonicalNamedWindowDefinition {
  std::string name;
  std::optional<std::string> base_name;
  std::vector<std::uint32_t> partition_descriptor_ids;
  std::vector<std::uint32_t> order_descriptor_ids;
  std::optional<std::uint32_t> frame_descriptor_id;
  std::size_t resolution_depth{0};
};

struct CanonicalNamedWindowResolution {
  bool accepted{false};
  std::string diagnostic_id{"QOW-DIAG-WINDOW-NAMED"};
  std::string detail;
  std::size_t definition_index{0};
  std::vector<CanonicalNamedWindowDefinition> resolved_definitions;
  std::vector<std::size_t> reference_definition_indices;
  bool resolved_once_in_scope{false};
};

CanonicalNamedWindowResolution ResolveCanonicalNamedWindows(
    const std::vector<CanonicalNamedWindowDefinition>& definitions,
    const std::vector<std::string>& referenced_names,
    std::size_t maximum_definition_count = 1024);

CentralImportCommandRoute AnalyzeCentralImportCommandRoute(
    const CstDocument& cst);

SblrEnvelope LowerToSblr(const BoundStatement& bound, const CstDocument& cst, const SessionContext& session);
SblrVerifierResult VerifySblrEnvelope(const SblrEnvelope& envelope);

} // namespace scratchbird::parser::sbsql
