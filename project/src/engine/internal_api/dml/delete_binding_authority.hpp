// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "dml/delete_durable_authority_codec.hpp"
#include "dml/delete_datatype_operator_authority_provider.hpp"
#include "dml/delete_security_authority_provider.hpp"
#include "dml/update_resource_authority_provider.hpp"

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteBindingAuthorityResultV1;
// Complete engine-issued live provider graph. This is prepublication material,
// not evidence of DDJR publication and never transaction-finality authority.
// The caller still owns the captured grant and must hand it to a durable owner
// or release it on proven prepublication abandonment.
class EngineDmlDeleteBindingAuthorityV1 final {
 public:
  bool valid() const noexcept { return authority_ != nullptr; }
  const DmlDeleteDurableAuthorityBundleV1* bundle() const noexcept;
 private:
  struct Authority;
  std::shared_ptr<const Authority> authority_;
  friend EngineDmlDeleteBindingAuthorityResultV1 CaptureDmlDeleteBindingAuthorityV1(
      const EngineRequestContext&, const DmlDeleteDurableAuthorityBundleV1&,
      const EngineDmlDeleteDatatypeAuthorityResultV1&, const EngineDmlDeleteSecurityAuthorityResultV1&,
      const EngineDmlDeleteEffectAuthorityResultV1&, const EngineDmlUpdateResourceHandleV1&);
  friend EngineApiDiagnostic RevalidateDmlDeleteBindingAuthorityV1(
      const EngineRequestContext&, const EngineDmlDeleteBindingAuthorityV1&);
};
struct EngineDmlDeleteBindingAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlDeleteBindingAuthorityV1 handle;
};
EngineDmlDeleteBindingAuthorityResultV1 CaptureDmlDeleteBindingAuthorityV1(
    const EngineRequestContext&, const DmlDeleteDurableAuthorityBundleV1&,
    const EngineDmlDeleteDatatypeAuthorityResultV1&, const EngineDmlDeleteSecurityAuthorityResultV1&,
    const EngineDmlDeleteEffectAuthorityResultV1&, const EngineDmlUpdateResourceHandleV1&);
EngineApiDiagnostic RevalidateDmlDeleteBindingAuthorityV1(
    const EngineRequestContext&, const EngineDmlDeleteBindingAuthorityV1&);
}  // namespace scratchbird::engine::internal_api
