// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_query_spatial_columnar_composition.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace api = scratchbird::engine::internal_api;
namespace nosql = api::nosql;
namespace s = scratchbird::engine::sblr;
namespace exec = scratchbird::engine::executor;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1175,n); }
exec::PhysicalMgaStatementContext Mga() {
  exec::PhysicalMgaStatementContext c;
  c.statement_uuid=Id(1); c.owning_transaction_uuid=Id(2);
  c.statement_snapshot_uuid=Id(3); c.statement_metadata_snapshot_uuid=Id(4);
  c.owning_local_transaction_id=40; c.visible_committed_high_watermark=39;
  c.oldest_active_transaction_id=30; c.oldest_interesting_transaction_id=29;
  c.oldest_snapshot_transaction_id=29; c.retention_horizon_transaction_id=29;
  c.active_excluded_local_transaction_ids={40}; c.snapshot_kind="statement_stable";
  c.publication_inventory_next_local_transaction_id=41;
  c.inventory_authoritative=true; c.complete=true; c.current=true;
  return c;
}
int main() {
  auto row=Id(10); row.bytes[10]=0; row.bytes[15]=255;
  std::string bytes(reinterpret_cast<const char*>(row.bytes.data()),16);
  Check(s::Rcp079NativeUuidCell(&bytes)==std::optional(row));
  bytes.pop_back(); Check(!s::Rcp079NativeUuidCell(&bytes));
  bytes="019f0000-0000-7000-8000-000000000001";
  Check(!s::Rcp079NativeUuidCell(&bytes));
  api::CatalogColumnMetadata metadata;
  metadata.identities={{"column_uuid",row},{"type_uuid",Id(11)},{"crs_uuid",Id(12)}};
  metadata.text={{"canonical","geometry"},{"nullable","false"},{"crs_generation","1"}};
  api::EngineDescriptor descriptor;
  Check(api::EncodeCatalogColumnMetadata(metadata,&descriptor.encoded_descriptor));
  const auto fields=s::Rcp079ExactDescriptorFieldsV1(descriptor);
  Check(fields && fields->identity("column_uuid")==std::optional(row) && fields->size()==6);
  Check(fields->exact("crs_uuid",Id(12)) && !fields->exact("crs_uuid",Id(13)));
  metadata.text["type_uuid"]="legacy";
  Check(!api::EncodeCatalogColumnMetadata(metadata,&bytes) ||
      ([&] { descriptor.encoded_descriptor=bytes; return !s::Rcp079ExactDescriptorFieldsV1(descriptor); })());
  descriptor.encoded_descriptor="type_uuid=019f0000-0000-7000-8000-000000000001";
  Check(!s::Rcp079ExactDescriptorFieldsV1(descriptor));
  nosql::SpatialExecutionRequestV1 request;
  request.operation_id="SPATIAL_NEAREST"; request.object_uuid=Id(20);
  request.geometry_descriptor_uuid=Id(21); request.geometry_type_uuid=Id(22);
  request.crs_uuid=Id(23); request.query_crs_uuid=request.crs_uuid;
  request.crs_generation=request.source_generation=request.catalog_generation=request.policy_generation=1;
  request.security_generation=request.resource_generation=request.route_generation=1;
  request.statement_context=Mga(); request.current_statement_context=request.statement_context;
  request.source_rows={{row,nosql::EncodeSpatialPoint2dV1({3,4}),request.crs_uuid},
      {Id(24),nosql::EncodeSpatialPoint2dV1({0,0}),request.crs_uuid}};
  request.encoded_query_point=nosql::EncodeSpatialPoint2dV1({0,0});
  request.maximum_rows=16; request.top_k=2; request.security_admitted=true;
  request.exact_scan_fallback_available=true;
  auto result=nosql::ExecuteSpatialNativeV1(request);
  Check(result.accepted && result.rows.size()==2 && result.rows[0].row_uuid==Id(24) &&
      result.rows[1].row_uuid==row && result.rows[1].distance==5 && result.rows[1].crs_uuid==request.crs_uuid);
  request.source_rows[1].row_uuid=row;
  Check(!nosql::ExecuteSpatialNativeV1(request).accepted);
  request.source_rows[1].row_uuid=Id(24); request.source_rows[0].crs_uuid=Id(25);
  Check(!nosql::ExecuteSpatialNativeV1(request).accepted);
  request.source_rows[0].crs_uuid=request.crs_uuid; request.object_uuid={};
  Check(!nosql::ExecuteSpatialNativeV1(request).accepted);
  request.object_uuid=Id(20); request.provider_finality_authority_claimed=true;
  Check(!nosql::ExecuteSpatialNativeV1(request).accepted);
  request.provider_finality_authority_claimed=false; request.current_statement_context.statement_snapshot_uuid=Id(99);
  Check(!nosql::ExecuteSpatialNativeV1(request).accepted);
}
