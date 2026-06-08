// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using scratchbird::engine::sblr::SblrResult;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  value.charset_name = "UTF-8";
  value.collation_name = "unicode_root";
  value.is_null = false;
  return value;
}

FunctionArgument Arg(std::string name, std::string descriptor, std::string input) {
  return FunctionArgument{std::move(name), TextValue(std::move(descriptor), std::move(input))};
}

SblrResult Run(const FunctionRegistry& registry,
               std::string function_id,
               std::vector<FunctionArgument> arguments = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-037-xml-multimodel-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-037-xml-multimodel-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 37037;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index].name.empty()) arguments[index].name = "arg" + std::to_string(index);
    request.arguments.push_back(std::move(arguments[index]));
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result"
              << "; status=" << static_cast<int>(result.status)
              << "; scalar_count=" << result.scalar_values.size()
              << "; mutation_attempted=" << result.mutation_attempted
              << "; mutation_committed=" << result.mutation_committed << "\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  diagnostic=" << diagnostic.diagnostic_id << "\n";
      for (const auto& field : diagnostic.fields) {
        std::cerr << "    " << field.key << "=" << field.value << "\n";
      }
    }
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.encoded_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBool(std::string_view case_id, const SblrResult& result, bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << expected_int << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInvalidInput(std::string_view case_id, const SblrResult& result) {
  if (result.ok() || result.status != SblrStatusCode::execution_failed ||
      result.diagnostics.empty() ||
      result.diagnostics.front().diagnostic_id != "SB_DIAG_FUNCTION_INVALID_INPUT" ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT refusal\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("SBSQL-0C16676374C8 SBSFC037-xmlforest",
                  Run(registry, "sb.xml.forest", {Arg("item", "character", "bird")}),
                  "xml_document", "<item>bird</item>") && ok;
  ok = ExpectText("SBSQL-2BBA1DA50B23 SBSFC037-xmlcast",
                  Run(registry, "sb.xml.cast",
                      {Arg("", "character", "<item>bird</item>")}),
                  "xml_document", "<item>bird</item>") && ok;
  ok = ExpectBool("SBSQL-104DD993AED4 SBSFC037-xmlexists",
                  Run(registry, "sb.xml.exists",
                      {Arg("", "character", "item"), Arg("", "xml_document", "<item>bird</item>")}),
                  true) && ok;
  ok = ExpectText("SBSQL-1FD7CBD0921F SBSFC037-xmlattributes",
                  Run(registry, "sb.xml.attributes", {Arg("kind", "character", "bird")}),
                  "xml", " kind=\"bird\"") && ok;
  ok = ExpectText("SBSQL-934D2E7C0508 SBSFC037-xmlconcat",
                  Run(registry, "sb.xml.concat",
                      {Arg("", "xml_document", "<a/>"), Arg("", "xml_document", "<b/>")}),
                  "xml_document", "<a/><b/>") && ok;
  ok = ExpectText("SBSQL-4F494D9A6610 SBSFC037-xmlcomment",
                  Run(registry, "sb.xml.comment", {Arg("", "character", "ok")}),
                  "xml", "<!--ok-->") && ok;
  ok = ExpectText("SBSQL-DC75730A32EA SBSFC037-xmlpi",
                  Run(registry, "sb.xml.pi",
                      {Arg("target", "character", "stylesheet"), Arg("content", "character", "href=a")}),
                  "xml", "<?stylesheet href=a?>") && ok;
  ok = ExpectText("SBSQL-52CC2FA7719D SBSFC037-xmlroot",
                  Run(registry, "sb.xml.root",
                      {Arg("", "xml_document", "<r/>"), Arg("version", "character", "1.1"),
                       Arg("standalone", "character", "yes")}),
                  "xml_document", "<?xml version=\"1.1\" standalone=\"yes\"?><r/>") && ok;
  ok = ExpectText("SBSQL-54EBF8EDE58A SBSFC037-xmlelement-name",
                  Run(registry, "sb.xml.element",
                      {Arg("name", "character", "item"), Arg("", "character", "bird")}),
                  "xml_document", "<item>bird</item>") && ok;
  ok = ExpectText("SBSQL-5702FA6BF536 SBSFC037-xmlagg",
                  Run(registry, "sb.xml.agg",
                      {Arg("", "xml_document", "<a/>"), Arg("", "xml_document", "<b/>")}),
                  "xml_document", "<a/><b/>") && ok;
  ok = ExpectText(
           "SBSQL-F0C5F1661298 SBSFC037-xmltable",
           Run(registry, "sb.xml.table",
               {Arg("", "character", "/item"), Arg("", "xml_document", "<item/>")}),
           "json_document",
           "{\"function\":\"XMLTABLE\",\"query\":\"/item\",\"document_descriptor\":\"xml_document\","
           "\"document_bytes\":7,\"document_nonempty\":true,\"passing_argument_count\":1,"
           "\"result\":\"descriptor\"}") && ok;
  ok = ExpectInvalidInput("SBSQL-7881C81BBBE8 SBSFC037-xmlcomment-invalid",
                          Run(registry, "sb.xml.comment",
                              {Arg("", "character", "bad--comment")})) && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
