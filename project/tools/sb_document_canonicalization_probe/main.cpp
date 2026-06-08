// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_document.hpp"

#include <iostream>

using namespace scratchbird::core::datatypes;

namespace {

bool Expect(bool condition, const char* name) {
  std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
  return condition;
}

DocumentCanonicalizationResult Canonicalize(CanonicalTypeId type_id,
                                            const char* value,
                                            bool allow_hstore_domain = false) {
  DocumentCanonicalizationRequest request;
  request.type_id = type_id;
  request.encoded_value = value;
  request.allow_hstore_domain = allow_hstore_domain;
  return CanonicalizeDocumentValue(request);
}

}  // namespace

int main() {
  const auto json = Canonicalize(CanonicalTypeId::json_document,
                                 "{ \"b\" : [ 1, true, null ], \"a\" : { \"x\" : \" y \" } }");
  const auto invalid_json = Canonicalize(CanonicalTypeId::json_document, "{\"a\":[1,]}");
  const auto duplicate_key = Canonicalize(CanonicalTypeId::json_document, "{\"a\":1,\"a\":2}");
  const auto object_doc = Canonicalize(CanonicalTypeId::object_document, "{\"kind\":\"object\",\"value\":1}");
  const auto flattened = Canonicalize(CanonicalTypeId::flattened_object_document, "{\"a.b\":1,\"c\":2}");
  const auto generic_json = Canonicalize(CanonicalTypeId::document, " [ true, false, null ] ");
  const auto xml = Canonicalize(CanonicalTypeId::xml_document, "  <?xml version=\"1.0\"?><root><child /></root>  ");
  const auto invalid_xml = Canonicalize(CanonicalTypeId::xml_document, "<root></other>");
  const auto generic_xml = Canonicalize(CanonicalTypeId::document, "<root/>");
  const auto binary_json = Canonicalize(CanonicalTypeId::binary_json_document, "SBBJSON1;payload=00ff");
  const auto bson = Canonicalize(CanonicalTypeId::bson_document, "SBBSON1;payload=00ff");
  const auto invalid_binary = Canonicalize(CanonicalTypeId::binary_json_document, "SBBJSON1;payload=00f");
  const auto hstore_denied = Canonicalize(CanonicalTypeId::hstore_document, "SBHSTORE1;items=6b6579:76616c7565");
  const auto hstore_allowed = Canonicalize(CanonicalTypeId::hstore_document, "SBHSTORE1;items=6b6579:76616c7565", true);
  const auto unsupported = Canonicalize(CanonicalTypeId::graph_node, "node");

  const bool ok = json.ok() && json.canonical_value == "{\"b\":[1,true,null],\"a\":{\"x\":\" y \"}}" &&
                  !invalid_json.ok() && !duplicate_key.ok() &&
                  object_doc.ok() && flattened.ok() &&
                  generic_json.ok() && generic_json.canonical_type_id == CanonicalTypeId::json_document &&
                  xml.ok() && xml.canonical_value == "<root><child /></root>" &&
                  !invalid_xml.ok() &&
                  generic_xml.ok() && generic_xml.canonical_type_id == CanonicalTypeId::xml_document &&
                  binary_json.ok() && binary_json.canonical_format == "binary_json_envelope" &&
                  bson.ok() && bson.canonical_format == "bson_envelope" &&
                  !invalid_binary.ok() && !hstore_denied.ok() &&
                  hstore_allowed.ok() && hstore_allowed.canonical_format == "hstore_domain_envelope" &&
                  !unsupported.ok();

  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(json.ok() && json.canonical_value == "{\"b\":[1,true,null],\"a\":{\"x\":\" y \"}}", "json_canonicalized");
  Expect(!invalid_json.ok(), "invalid_json_rejected");
  Expect(!duplicate_key.ok(), "duplicate_json_key_rejected");
  Expect(object_doc.ok(), "object_document_canonicalized");
  Expect(flattened.ok(), "flattened_object_canonicalized");
  Expect(generic_json.ok() && generic_json.canonical_type_id == CanonicalTypeId::json_document, "generic_document_json_detected");
  Expect(xml.ok() && xml.canonical_value == "<root><child /></root>", "xml_canonicalized");
  Expect(!invalid_xml.ok(), "invalid_xml_rejected");
  Expect(generic_xml.ok() && generic_xml.canonical_type_id == CanonicalTypeId::xml_document, "generic_document_xml_detected");
  Expect(binary_json.ok(), "binary_json_envelope_validated");
  Expect(bson.ok(), "bson_envelope_validated");
  Expect(!invalid_binary.ok(), "invalid_binary_document_rejected");
  Expect(!hstore_denied.ok(), "hstore_without_domain_rejected");
  Expect(hstore_allowed.ok(), "hstore_domain_allowed");
  std::cout << "  \"unsupported_document_type_rejected\": " << (!unsupported.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
