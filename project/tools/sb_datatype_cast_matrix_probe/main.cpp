// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"

#include <iostream>
#include <string>

using namespace scratchbird::core::datatypes;

namespace {

bool Expect(bool condition, const char* name) {
  std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
  return condition;
}

std::string Bytes(std::initializer_list<unsigned char> bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (unsigned char byte : bytes) { out.push_back(static_cast<char>(byte)); }
  return out;
}

}  // namespace

int main() {
  DatatypeCastRequest numeric;
  numeric.value = {CanonicalTypeId::int32, "42", false};
  numeric.target_type_id = CanonicalTypeId::int64;
  numeric.explicit_cast = false;
  const auto int_widen = CastDatatypeValue(numeric);

  DatatypeCastRequest int128;
  int128.value = {CanonicalTypeId::character, "170141183460469231731687303715884105727", false};
  int128.target_type_id = CanonicalTypeId::int128;
  int128.explicit_cast = true;
  const auto int128_cast = CastDatatypeValue(int128);

  DatatypeCastRequest overflow = int128;
  overflow.value.encoded_value = "170141183460469231731687303715884105728";
  const auto overflow_cast = CastDatatypeValue(overflow);

  DatatypeCastRequest uuid;
  uuid.value = {CanonicalTypeId::character, "018f7f8f-7c00-7000-8000-000000000001", false};
  uuid.target_type_id = CanonicalTypeId::uuid;
  uuid.explicit_cast = true;
  const auto uuid_cast = CastDatatypeValue(uuid);

  DatatypeCastRequest forbidden;
  forbidden.value = {CanonicalTypeId::graph_node, "node", false};
  forbidden.target_type_id = CanonicalTypeId::real128;
  forbidden.explicit_cast = true;
  const auto forbidden_cast = CastDatatypeValue(forbidden);

  DatatypeCastRequest json;
  json.value = {CanonicalTypeId::character, "{\"a\":[1,true,null]}", false};
  json.target_type_id = CanonicalTypeId::json_document;
  json.explicit_cast = true;
  const auto json_cast = CastDatatypeValue(json);

  DatatypeCastRequest bad_json = json;
  bad_json.value.encoded_value = "{\"a\":[1,]}";
  const auto bad_json_cast = CastDatatypeValue(bad_json);

  DatatypeCastRequest xml;
  xml.value = {CanonicalTypeId::character, "<root><child /></root>", false};
  xml.target_type_id = CanonicalTypeId::xml_document;
  xml.explicit_cast = true;
  const auto xml_cast = CastDatatypeValue(xml);

  DatatypeCastRequest bad_xml = xml;
  bad_xml.value.encoded_value = "<root></other>";
  const auto bad_xml_cast = CastDatatypeValue(bad_xml);

  DatatypeCastRequest opaque_render;
  opaque_render.value = {CanonicalTypeId::opaque_extension, "opaque-render-token", false};
  opaque_render.target_type_id = CanonicalTypeId::character;
  opaque_render.explicit_cast = true;
  const auto opaque_render_cast = CastDatatypeValue(opaque_render);

  DatatypeCastRequest opaque_parse;
  opaque_parse.value = {CanonicalTypeId::character, "opaque-render-token", false};
  opaque_parse.target_type_id = CanonicalTypeId::opaque_extension;
  opaque_parse.explicit_cast = true;
  opaque_parse.reference_compatibility_profile = true;
  const auto opaque_parse_cast = CastDatatypeValue(opaque_parse);

  DatatypeCastRequest opaque_semantic;
  opaque_semantic.value = {CanonicalTypeId::opaque_extension, "123", false};
  opaque_semantic.target_type_id = CanonicalTypeId::int32;
  opaque_semantic.explicit_cast = true;
  opaque_semantic.reference_compatibility_profile = true;
  const auto opaque_semantic_cast = CastDatatypeValue(opaque_semantic);

  DatatypeCastRequest leap_date;
  leap_date.value = {CanonicalTypeId::character, "2024-02-29", false};
  leap_date.target_type_id = CanonicalTypeId::date;
  leap_date.explicit_cast = true;
  const auto leap_date_cast = CastDatatypeValue(leap_date);

  DatatypeCastRequest bad_date = leap_date;
  bad_date.value.encoded_value = "2023-02-29";
  const auto bad_date_cast = CastDatatypeValue(bad_date);

  DatatypeCastRequest zoned_time;
  zoned_time.value = {CanonicalTypeId::character, "23:59:58.123456789012-05:00", false};
  zoned_time.target_type_id = CanonicalTypeId::time;
  zoned_time.explicit_cast = true;
  const auto zoned_time_cast = CastDatatypeValue(zoned_time);

  DatatypeCastRequest bad_time = zoned_time;
  bad_time.value.encoded_value = "24:00:00";
  const auto bad_time_cast = CastDatatypeValue(bad_time);

  DatatypeCastRequest zoned_timestamp;
  zoned_timestamp.value = {CanonicalTypeId::character, "2026-05-01T12:34:56Z", false};
  zoned_timestamp.target_type_id = CanonicalTypeId::timestamp;
  zoned_timestamp.explicit_cast = true;
  const auto zoned_timestamp_cast = CastDatatypeValue(zoned_timestamp);

  DatatypeCastRequest bad_timestamp = zoned_timestamp;
  bad_timestamp.value.encoded_value = "2026-13-01T12:34:56Z";
  const auto bad_timestamp_cast = CastDatatypeValue(bad_timestamp);

  DatatypeCastRequest text_to_binary;
  text_to_binary.value = {CanonicalTypeId::character, "0x000102ff", false};
  text_to_binary.target_type_id = CanonicalTypeId::binary;
  text_to_binary.explicit_cast = true;
  const auto text_to_binary_cast = CastDatatypeValue(text_to_binary);

  DatatypeCastRequest bad_binary = text_to_binary;
  bad_binary.value.encoded_value = "0x001";
  const auto bad_binary_cast = CastDatatypeValue(bad_binary);

  DatatypeCastRequest binary_to_text;
  binary_to_text.value = {CanonicalTypeId::binary, Bytes({0x00, 0x01, 0x02, 0xff}), false};
  binary_to_text.target_type_id = CanonicalTypeId::character;
  binary_to_text.explicit_cast = true;
  const auto binary_to_text_cast = CastDatatypeValue(binary_to_text);

  DatatypeCastRequest uuid_to_binary;
  uuid_to_binary.value = {CanonicalTypeId::uuid, "018f7f8f-7c00-7000-8000-000000000001", false};
  uuid_to_binary.target_type_id = CanonicalTypeId::binary;
  uuid_to_binary.explicit_cast = true;
  const auto uuid_to_binary_cast = CastDatatypeValue(uuid_to_binary);

  DatatypeCastRequest binary_to_uuid;
  binary_to_uuid.value = {CanonicalTypeId::binary, uuid_to_binary_cast.value.encoded_value, false};
  binary_to_uuid.target_type_id = CanonicalTypeId::uuid;
  binary_to_uuid.explicit_cast = true;
  const auto binary_to_uuid_cast = CastDatatypeValue(binary_to_uuid);

  DatatypeCastRequest text_to_blob;
  text_to_blob.value = {CanonicalTypeId::character, "48656c6c6f", false};
  text_to_blob.target_type_id = CanonicalTypeId::blob;
  text_to_blob.explicit_cast = true;
  const auto text_to_blob_cast = CastDatatypeValue(text_to_blob);

  DatatypeCastRequest blob_to_text;
  blob_to_text.value = {CanonicalTypeId::blob, "Hello", false};
  blob_to_text.target_type_id = CanonicalTypeId::character;
  blob_to_text.explicit_cast = true;
  const auto blob_to_text_cast = CastDatatypeValue(blob_to_text);

  const bool ok = int_widen.ok() && int_widen.category == DatatypeCastCategory::lossless_implicit &&
                  int128_cast.ok() && !overflow_cast.ok() && uuid_cast.ok() && !forbidden_cast.ok() &&
                  json_cast.ok() && !bad_json_cast.ok() && xml_cast.ok() && !bad_xml_cast.ok() &&
                  opaque_render_cast.ok() && !opaque_parse_cast.ok() && !opaque_semantic_cast.ok() &&
                  leap_date_cast.ok() && !bad_date_cast.ok() && zoned_time_cast.ok() && !bad_time_cast.ok() &&
                  zoned_timestamp_cast.ok() && !bad_timestamp_cast.ok() && text_to_binary_cast.ok() &&
                  text_to_binary_cast.value.encoded_value == Bytes({0x00, 0x01, 0x02, 0xff}) &&
                  !bad_binary_cast.ok() && binary_to_text_cast.ok() &&
                  binary_to_text_cast.value.encoded_value == "000102ff" && uuid_to_binary_cast.ok() &&
                  uuid_to_binary_cast.value.encoded_value.size() == 16 && binary_to_uuid_cast.ok() &&
                  binary_to_uuid_cast.value.encoded_value == "018f7f8f-7c00-7000-8000-000000000001" &&
                  text_to_blob_cast.ok() && text_to_blob_cast.value.encoded_value == "Hello" &&
                  blob_to_text_cast.ok() && blob_to_text_cast.value.encoded_value == "48656c6c6f";
  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(int_widen.ok(), "int_widen");
  Expect(int128_cast.ok(), "int128_cast");
  Expect(!overflow_cast.ok(), "int128_overflow_rejected");
  Expect(uuid_cast.ok(), "uuid_cast");
  Expect(json_cast.ok(), "json_cast");
  Expect(!bad_json_cast.ok(), "invalid_json_rejected");
  Expect(xml_cast.ok(), "xml_cast");
  Expect(!bad_xml_cast.ok(), "invalid_xml_rejected");
  Expect(opaque_render_cast.ok(), "opaque_render_cast");
  Expect(!opaque_parse_cast.ok(), "opaque_parse_rejected");
  Expect(!opaque_semantic_cast.ok(), "opaque_semantic_rejected");
  Expect(leap_date_cast.ok(), "leap_date_cast");
  Expect(!bad_date_cast.ok(), "invalid_date_rejected");
  Expect(zoned_time_cast.ok(), "zoned_time_cast");
  Expect(!bad_time_cast.ok(), "invalid_time_rejected");
  Expect(zoned_timestamp_cast.ok(), "zoned_timestamp_cast");
  Expect(!bad_timestamp_cast.ok(), "invalid_timestamp_rejected");
  Expect(text_to_binary_cast.ok(), "text_to_binary_cast");
  Expect(!bad_binary_cast.ok(), "invalid_binary_rejected");
  Expect(binary_to_text_cast.ok(), "binary_to_text_cast");
  Expect(uuid_to_binary_cast.ok(), "uuid_to_binary_cast");
  Expect(binary_to_uuid_cast.ok(), "binary_to_uuid_cast");
  Expect(text_to_blob_cast.ok(), "text_to_blob_cast");
  Expect(blob_to_text_cast.ok(), "blob_to_text_cast");
  std::cout << "  \"forbidden_rejected\": " << (!forbidden_cast.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
