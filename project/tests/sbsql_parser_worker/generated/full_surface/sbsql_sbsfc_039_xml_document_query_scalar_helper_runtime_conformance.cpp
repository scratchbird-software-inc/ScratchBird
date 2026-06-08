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
  request.context.sblr_context.database_uuid = "SBSFC-039-xml-document-query-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-039-xml-document-query-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 39039;
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

  ok = ExpectText("SBSQL-253585ABE51D SBSFC039-xmldocument",
                  Run(registry, "sb.xml.document",
                      {Arg("", "character", "<r><item>bird</item></r>")}),
                  "xml_document", "<r><item>bird</item></r>") && ok;
  ok = ExpectText("SBSQL-5753A90D2A1C SBSFC039-xmldocument-expr",
                  Run(registry, "sb.xml.document", {Arg("", "character", "<r/>")}),
                  "xml_document", "<r/>") && ok;
  ok = ExpectText("SBSQL-9D96355276FC SBSFC039-xmlnamespaces",
                  Run(registry, "sb.xml.namespaces",
                      {Arg("", "character", "p"), Arg("", "character", "urn:test")}),
                  "xml", " xmlns:p=\"urn:test\"") && ok;
  ok = ExpectText("SBSQL-4F9AE84DDF5A SBSFC039-xmlnamespaces-default",
                  Run(registry, "sb.xml.namespaces",
                      {Arg("", "character", "default"), Arg("", "character", "urn:default")}),
                  "xml", " xmlns=\"urn:default\"") && ok;
  ok = ExpectText("SBSQL-965B96256EB3 SBSFC039-xmlparse",
                  Run(registry, "sb.xml.parse", {Arg("", "character", "<r/>")}),
                  "xml_document", "<r/>") && ok;
  ok = ExpectText("SBSQL-F48761720168 SBSFC039-xmlparse-document",
                  Run(registry, "sb.xml.parse",
                      {Arg("mode", "character", "document"), Arg("", "character", "<r/>"),
                       Arg("whitespace", "character", "preserve")}),
                  "xml_document", "<r/>") && ok;
  ok = ExpectText("SBSQL-B9BD61883168 SBSFC039-xmlquery-root",
                  Run(registry, "sb.xml.query",
                      {Arg("", "character", "/r"),
                       Arg("", "xml_document", "<r><item>bird</item></r>")}),
                  "xml_document", "<r><item>bird</item></r>") && ok;
  ok = ExpectText("SBSQL-04FE00443530 SBSFC039-xmlquery-descendant",
                  Run(registry, "sb.xml.query",
                      {Arg("", "character", "//item"),
                       Arg("", "xml_document", "<r><item>bird</item></r>")}),
                  "xml_document", "<item>bird</item>") && ok;
  ok = ExpectText("SBSQL-24C067DA97B0 SBSFC039-xmlserialize",
                  Run(registry, "sb.xml.serialize", {Arg("", "xml_document", "<r/>")}),
                  "character", "<r/>") && ok;
  ok = ExpectText("SBSQL-C9809EF23816 SBSFC039-xmlserialize-document",
                  Run(registry, "sb.xml.serialize",
                      {Arg("mode", "character", "document"), Arg("", "xml_document", "<r/>"),
                       Arg("type", "character", "character")}),
                  "character", "<r/>") && ok;
  ok = ExpectText("SBSQL-82BBA556D880 SBSFC039-xmltext",
                  Run(registry, "sb.xml.text", {Arg("", "character", "a < b & c")}),
                  "xml", "a &lt; b &amp; c") && ok;
  ok = ExpectText("SBSQL-D53A57E7DD0B SBSFC039-xmltext-text",
                  Run(registry, "sb.xml.text", {Arg("", "character", "bird")}),
                  "xml", "bird") && ok;
  ok = ExpectText("SBSQL-666EAE033CFC SBSFC039-xmlvalidate",
                  Run(registry, "sb.xml.validate", {Arg("", "character", "<r/>")}),
                  "xml_document", "<r/>") && ok;
  ok = ExpectText("SBSQL-B4880446510E SBSFC039-xmlvalidate-document",
                  Run(registry, "sb.xml.validate",
                      {Arg("mode", "character", "document"), Arg("", "character", "<r/>")}),
                  "xml_document", "<r/>") && ok;
  ok = ExpectText("SBSQL-663D565ADA02 SBSFC039-xml-alias",
                  Run(registry, "sb.xml.document", {Arg("", "character", "<r/>")}),
                  "xml_document", "<r/>") && ok;
  ok = ExpectText("SBSQL-5F496C39F6E8 SBSFC039-xml-attrs",
                  Run(registry, "sb.xml.attrs",
                      {Arg("", "character", "id"), Arg("", "character", "42")}),
                  "xml", " id=\"42\"") && ok;
  ok = ExpectText("SBSQL-2ABE2825F6A1 SBSFC039-xml-ns",
                  Run(registry, "sb.xml.ns",
                      {Arg("", "character", "p"), Arg("", "character", "urn:test")}),
                  "xml", " xmlns:p=\"urn:test\"") && ok;
  ok = ExpectInvalidInput("SBSQL-5753A90D2A1C SBSFC039-xmldocument-invalid",
                          Run(registry, "sb.xml.document",
                              {Arg("", "character", "<r><bad></r>")})) && ok;
  ok = ExpectInvalidInput("SBSQL-5F496C39F6E8 SBSFC039-xml-attrs-invalid",
                          Run(registry, "sb.xml.attrs",
                              {Arg("", "character", "bad name"), Arg("", "character", "42")})) && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
