// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "bound_ast_model.hpp"
#include "engine_api_bridge.hpp"
#include "native_minimal_parser.hpp"
#include "sblr_envelope.hpp"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

namespace {

struct Options {
  std::string command_text;
  std::string database_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string catalog_epoch;
  std::string registry_snapshot_uuid;
  std::string profile = "public_node";
};

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

void PrintUsage(std::ostream& out) {
  out << "Usage:\n"
      << "  sb_parse_probe --command <text> --database-uuid <uuid> --session-uuid <uuid> --principal-uuid <uuid> "
      << "--catalog-epoch <epoch> --registry-snapshot <id> [--profile <profile>]\n";
}

std::optional<Options> ParseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      ++i;
      return std::string(argv[i]);
    };

    if (arg == "--command") {
      auto v = value(); if (!v) return std::nullopt; options.command_text = *v;
    } else if (arg == "--database-uuid") {
      auto v = value(); if (!v) return std::nullopt; options.database_uuid = *v;
    } else if (arg == "--session-uuid") {
      auto v = value(); if (!v) return std::nullopt; options.session_uuid = *v;
    } else if (arg == "--principal-uuid") {
      auto v = value(); if (!v) return std::nullopt; options.principal_uuid = *v;
    } else if (arg == "--catalog-epoch") {
      auto v = value(); if (!v) return std::nullopt; options.catalog_epoch = *v;
    } else if (arg == "--registry-snapshot") {
      auto v = value(); if (!v) return std::nullopt; options.registry_snapshot_uuid = *v;
    } else if (arg == "--profile") {
      auto v = value(); if (!v) return std::nullopt; options.profile = *v;
    } else {
      return std::nullopt;
    }
  }

  if (options.command_text.empty() || options.database_uuid.empty() || options.session_uuid.empty() ||
      options.principal_uuid.empty() || options.catalog_epoch.empty() ||
      options.registry_snapshot_uuid.empty()) {
    return std::nullopt;
  }
  return options;
}

std::string IndentJson(std::string_view json, const std::string& indent) {
  std::ostringstream out;
  bool at_line_start = true;
  for (const char ch : json) {
    if (at_line_start) {
      out << indent;
      at_line_start = false;
    }
    out << ch;
    if (ch == '\n') at_line_start = true;
  }
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  auto options = ParseArgs(argc, argv);
  if (!options) {
    PrintUsage(std::cerr);
    return 5;
  }

  namespace ast = scratchbird::parser::ast;
  namespace bound = scratchbird::parser::bound_ast;
  namespace lowering = scratchbird::parser::lowering;
  namespace native = scratchbird::parser::native_v3;

  const native::ParseResult parse_result = native::ParseMinimalIdentityShow(options->command_text);
  if (!parse_result.ok()) {
    std::cout << "{\n";
    std::cout << "  \"stage\": \"parse\",\n";
    std::cout << "  \"status\": \"fail\",\n";
    std::cout << "  \"profile\": \"" << JsonEscape(options->profile) << "\",\n";
    std::cout << "  \"diagnostic\": " << IndentJson(native::SerializeParseResultToJson(parse_result), "  ") << "\n";
    std::cout << "}\n";
    return 1;
  }

  const auto& parsed_ast = std::get<ast::ShowIdentityAst>(parse_result.value);
  bound::BindingContext context;
  context.database_uuid = options->database_uuid;
  context.principal_uuid = options->principal_uuid;
  context.catalog_epoch = options->catalog_epoch;
  context.registry_snapshot_uuid = options->registry_snapshot_uuid;
  context.package_profile = options->profile;

  const bound::BindResult bind_result = bound::BindShowIdentityAst(parsed_ast, context);
  if (!bind_result.ok()) {
    std::cout << "{\n";
    std::cout << "  \"stage\": \"bind\",\n";
    std::cout << "  \"status\": \"fail\",\n";
    std::cout << "  \"profile\": \"" << JsonEscape(options->profile) << "\",\n";
    std::cout << "  \"ast\": " << IndentJson(ast::SerializeToJson(parsed_ast), "  ") << ",\n";
    std::cout << "  \"diagnostic\": " << IndentJson(bound::SerializeBindResultToJson(bind_result), "  ") << "\n";
    std::cout << "}\n";
    return 1;
  }

  const auto& bound_ast = std::get<bound::BoundShowIdentity>(bind_result.value);
  const lowering::LoweringResult lower_result = lowering::LowerBoundShowIdentity(bound_ast);
  if (!lower_result.ok()) {
    std::cout << "{\n";
    std::cout << "  \"stage\": \"lower\",\n";
    std::cout << "  \"status\": \"fail\",\n";
    std::cout << "  \"profile\": \"" << JsonEscape(options->profile) << "\",\n";
    std::cout << "  \"ast\": " << IndentJson(ast::SerializeToJson(parsed_ast), "  ") << ",\n";
    std::cout << "  \"bound_ast\": " << IndentJson(bound::SerializeToJson(bound_ast), "  ") << ",\n";
    std::cout << "  \"diagnostic\": " << IndentJson(lowering::SerializeLoweringResultToJson(lower_result), "  ") << "\n";
    std::cout << "}\n";
    return 1;
  }

  const auto& envelope = std::get<lowering::LogicalEnvelope>(lower_result.value);
  lowering::EngineApiBridgeContext bridge_context;
  bridge_context.session_uuid = options->session_uuid;
  bridge_context.cluster_authority_active = options->profile == "private_cluster";
  const lowering::EngineApiBridgeResult bridge_result =
      lowering::BridgeLogicalEnvelopeToEngineRequest(envelope, bridge_context);
  if (!bridge_result.ok()) {
    std::cout << "{\n";
    std::cout << "  \"stage\": \"engine_api_bridge\",\n";
    std::cout << "  \"status\": \"fail\",\n";
    std::cout << "  \"profile\": \"" << JsonEscape(options->profile) << "\",\n";
    std::cout << "  \"ast\": " << IndentJson(ast::SerializeToJson(parsed_ast), "  ") << ",\n";
    std::cout << "  \"bound_ast\": " << IndentJson(bound::SerializeToJson(bound_ast), "  ") << ",\n";
    std::cout << "  \"envelope\": " << IndentJson(lowering::SerializeLoweringResultToJson(lower_result), "  ") << ",\n";
    std::cout << "  \"engine_api_bridge\": "
              << IndentJson(lowering::SerializeEngineApiBridgeResultToJson(bridge_result), "  ") << "\n";
    std::cout << "}\n";
    return 1;
  }

  std::cout << "{\n";
  std::cout << "  \"stage\": \"engine_api_bridge\",\n";
  std::cout << "  \"status\": \"pass\",\n";
  std::cout << "  \"profile\": \"" << JsonEscape(options->profile) << "\",\n";
  std::cout << "  \"input\": \"" << JsonEscape(options->command_text) << "\",\n";
  std::cout << "  \"ast\": " << IndentJson(ast::SerializeToJson(parsed_ast), "  ") << ",\n";
  std::cout << "  \"bound_ast\": " << IndentJson(bound::SerializeToJson(bound_ast), "  ") << ",\n";
  std::cout << "  \"envelope\": " << IndentJson(lowering::SerializeLoweringResultToJson(lower_result), "  ") << ",\n";
  std::cout << "  \"engine_api_bridge\": "
            << IndentJson(lowering::SerializeEngineApiBridgeResultToJson(bridge_result), "  ") << "\n";
  std::cout << "}\n";
  return 0;
}
