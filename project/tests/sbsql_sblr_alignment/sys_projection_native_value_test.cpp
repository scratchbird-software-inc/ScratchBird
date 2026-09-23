// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/catalog/sys_information_projection.cpp"
#include <cstdlib>
#include <iostream>

namespace api = scratchbird::engine::internal_api;
void Check(bool ok) { if (!ok) { std::abort(); } }
int main() {
  api::EngineUuid uuid;
  uuid.bytes = {0, 10, 13, 9, 255, 0, 0x70, 0, 0x80, 0, 1, 2, 3, 4, 5, 6};
  const auto typed = api::SysInformationTypedValue(uuid);
  Check(typed.descriptor.canonical_type_name == "uuid" && typed.encoded_value.empty());
  Check(typed.binary_value.size() == 16 &&
        std::equal(typed.binary_value.begin(), typed.binary_value.end(), uuid.bytes.begin()));
  Check(api::VisibleUuid(uuid) == uuid);
  const auto redacted = api::SysInformationTypedValue(api::VisibleUuid(uuid, false));
  Check(redacted.descriptor.canonical_type_name == "uuid" && redacted.is_null &&
        redacted.binary_value.empty() && redacted.encoded_value.empty());
  const auto absent = api::SysInformationTypedValue(api::EngineUuid{});
  Check(absent.is_null && absent.state == api::EngineValueState::sql_null && absent.binary_value.empty());
  api::SysInformationCatalogObjectSource object;
  object.object_uuid = uuid; object.object_class = "table";
  const auto node = api::NavigatorObjectNodeId(object);
  Check(node.substr(node.size() - 16) == api::NativeUuidBytes(uuid));
  Check(api::NavigatorKey({"a:b", "c"}) != api::NavigatorKey({"a", "b:c"}));
  Check(api::NavigatorKey({"ab", "c"}) != api::NavigatorKey({"a", "bc"}));
  api::SysInformationProjectionResult navigator;
  api::AddNavigatorRow(&navigator, node, {}, uuid, {}, "s/t", "t", "table", "table",
                       "table", "s.t", "s", "s", 1, 1, 1, false, false);
  const auto* binary = api::SysInformationField(navigator.rows[0], "node_id");
  Check(binary && std::holds_alternative<api::SysInformationBinaryValue>(*binary));
  const auto wire = api::SysInformationTypedValue(*binary);
  Check(wire.descriptor.canonical_type_name == "binary" && wire.encoded_value.empty());
  Check(std::string(wire.binary_value.begin(), wire.binary_value.end()) == node);
  Check(!api::SysInformationFieldIsNull(api::SysInformationField(navigator.rows[0], "object_id")));
  Check(api::SysInformationFieldIsNull(api::SysInformationField(navigator.rows[0], "parent_object_id")));
  api::SysInformationResolverNameSource name;
  name.object_uuid = uuid; name.scope_uuid = uuid;
  name.object_class = "view"; name.display_name = "tables";
  std::map<std::string, api::EngineUuid> schemas{{"sys.information", uuid}};
  Check(api::SysInformationObjectIdentity({name}, schemas, "sys.information.tables", "view") == uuid);
  Check(api::SysInformationObjectIdentity({}, schemas, "sys.information.tables", "view").is_nil());
  auto duplicate = name; duplicate.object_uuid.bytes.back() ^= 1;
  Check(api::SysInformationObjectIdentity({name, duplicate}, schemas, "sys.information.tables", "view").is_nil());
  Check(api::LogicalTypeForProjectionColumn("node_id") == "binary");
  Check(api::LogicalTypeForProjectionColumn("object_id") == "uuid");
  std::cout << "sys projection native value regression PASS\n";
}
