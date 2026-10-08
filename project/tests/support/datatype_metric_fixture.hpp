// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_projection_fixture.hpp"

namespace scratchbird::tests {
// Finite component series shared by native datatype producer and numeric/domain
// regressions. Registration emits nothing and does not claim runtime activation.
inline void AdmitDatatypeMetricFixtureSeries(MetricProjectionFixture& fixture,
                                             core::metrics::MetricUuid domain) {
  fixture.Admit("sb_datatype_operation_total", {{"component", "datatype.runtime"},
      {"canonical_type", "decimal"}, {"operation", "canonicalize"}, {"result", "ok"}, {"reason", "none"}});
  fixture.Admit("sb_datatype_cast_total", {{"component", "datatype.runtime"},
      {"source_type", "character"}, {"target_type", "int128"},
      {"result", "overflow"}, {"reason", "integer_out_of_range"}});
  fixture.Admit("sb_datatype_numeric_backend_total", {{"component", "datatype.runtime"},
      {"numeric_backend", "sbl_numeric"}, {"canonical_type", "real128"},
      {"operation", "add"}, {"result", "ok"}, {"reason", "none"}});
  fixture.Admit("sb_domain_method_invocation_total", {{"component", "datatype.domain"},
      {"domain_uuid", domain}, {"method", "upper"}, {"result", "ok"}, {"reason", "none"}});
  fixture.Admit("sb_datatype_catalog_descriptors", {{"component", "datatype.catalog"}, {"result", "ok"}});
}
}  // namespace scratchbird::tests
