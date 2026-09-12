// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_query_result_metadata.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <new>

namespace scratchbird::engine::sblr {
namespace api = internal_api;
namespace dt = core::datatypes;
namespace {

bool BinaryIdentity(const api::EngineUuid& identity, wire::TypedResultUuid* out) {
  if (!core::uuid::IsEngineIdentityUuid(identity)) return false;
  *out = identity.bytes;
  return true;
}

bool ExactIdentity(const api::RelationalTypeDescriptor& descriptor,
                   const dt::DatatypeTypeCodecIdentityRowV1& row) {
  return descriptor.descriptor_generation == row.descriptor_generation &&
         descriptor.type_uuid == row.type_uuid &&
         descriptor.type_generation == row.type_generation &&
         descriptor.codec_id == row.codec_id &&
         descriptor.codec_version == row.codec_version &&
         descriptor.codec_generation == row.codec_generation;
}

void ObserveSchema(const api::EngineQueryResultMetadataV1& metadata,
                   std::size_t row_count) noexcept {
  // Optional diagnostic observation only: neither trace success nor a label
  // issues authority or changes the query outcome. No names/UUIDs/data escape.
  try {
    const char* path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (!path || !*path) return;
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) return;
    out << "layer=canonical_query_result_schema\tcolumns=" << metadata.columns.size()
        << "\trows=" << row_count << "\tcatalog_generation="
        << metadata.datatype_catalog_generation << "\tregistry_generation="
        << metadata.datatype_registry_generation << '\n';
  } catch (...) {
    // Optional diagnostic I/O is never the result-publication authority.
  }
}

}  // namespace

bool PreserveCanonicalQueryResultMetadataV1(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    api::EngineResultShape* shape,
    std::string* diagnostic_code,
    std::string* detail) {
  if (shape) {
    shape->query_metadata.reset();
    shape->query_values.reset();
  }
  if (!diagnostic_code || !detail) return false;
  diagnostic_code->clear();
  detail->clear();
  const auto refuse = [&](const char* why,
                          const char* code = "DATATYPE.DESCRIPTOR.INVALID") {
    *diagnostic_code = code;
    *detail = why;
    return false;
  };
  if (!shape || shape->result_kind != "rows")
    return refuse("query result requires an explicit row-bearing shape");
  try {
    const auto cancelled = [&] {
      return context.query_cancellation_requested &&
             context.query_cancellation_requested();
    };
    if (cancelled())
      return refuse("query result metadata publication cancelled", "PROCESS.CANCELLED");
    auto metadata = std::make_shared<api::EngineQueryResultMetadataV1>();
    if (!BinaryIdentity(context.statement_receipt_uuid,
                        &metadata->statement_receipt_uuid) ||
        !BinaryIdentity(context.statement_snapshot_uuid,
                        &metadata->statement_snapshot_uuid) ||
        !BinaryIdentity(context.datatype_catalog_snapshot_uuid,
                        &metadata->datatype_catalog_snapshot_uuid) ||
        context.datatype_catalog_generation == 0 ||
        context.datatype_registry_generation == 0)
      return refuse("query result has no exact live receipt/catalog cohort");
    metadata->datatype_catalog_generation = context.datatype_catalog_generation;
    metadata->datatype_registry_generation = context.datatype_registry_generation;

    std::map<std::uint32_t, const api::RelationalDagNode*> nodes;
    for (const auto& node : dag.nodes) {
      if (node.node_id == 0 || !nodes.emplace(node.node_id, &node).second)
        return refuse("query node identity is absent or ambiguous");
    }
    const auto root_it = nodes.find(dag.root_node_id);
    if (root_it == nodes.end())
      return refuse("query output root identity is absent");
    const auto* root = root_it->second;
    const auto* schema_owner = root;
    std::vector<const api::RelationalOutputRecord*> outputs;
    std::size_t forwarded = 0;
    for (;;) {
      for (const auto& output : dag.outputs)
        if (output.relation_node_id == schema_owner->node_id) outputs.push_back(&output);
      if (!outputs.empty()) break;
      // A nonrecursive CTE or LIMIT carries the producer's schema, not another
      // set of executable output records. Follow its actual single input and prove
      // exact descriptor order at every edge; never choose the first leaf.
      if ((schema_owner->node_kind != api::RelationalDagNodeKind::kCte &&
           schema_owner->node_kind != api::RelationalDagNodeKind::kLimit) ||
          schema_owner->input_node_ids.size() != 1 || ++forwarded > nodes.size())
        return refuse("query root has no unambiguous output schema");
      if (cancelled())
        return refuse("query result metadata publication cancelled", "PROCESS.CANCELLED");
      const auto input = nodes.find(schema_owner->input_node_ids.front());
      if (input == nodes.end() ||
          input->second->output_descriptor_ids != schema_owner->output_descriptor_ids)
        return refuse("forwarded output schema differs from its bound producer");
      schema_owner = input->second;
    }
    std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
    if (outputs.size() != root->output_descriptor_ids.size())
      return refuse("query root output descriptor coverage is incomplete");
    if (shape->columns.empty())
      return refuse("row-bearing query result has no column descriptor");
    if (shape->columns.size() > 16384)
      return refuse("query output column limit exceeded", "RESOURCE.BUDGET_EXCEEDED");

    std::map<std::uint32_t, const api::RelationalTypeDescriptor*> descriptors;
    for (const auto& descriptor : dag.descriptors)
      if (descriptor.descriptor_id == 0 ||
          !descriptors.emplace(descriptor.descriptor_id, &descriptor).second)
        return refuse("query descriptor handle is absent or duplicated");
    // Enumerate the actual admitted catalog descriptors under this receipt.
    // No synthetic snapshot, stable-name lookup, or nearest generation is used.
    std::vector<dt::DatatypeTypeCodecIdentityRowV1> identities;
    for (const auto& candidate : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
      if (candidate.catalog_snapshot_uuid == context.datatype_catalog_snapshot_uuid &&
          candidate.catalog_generation == context.datatype_catalog_generation &&
          candidate.registry_generation == context.datatype_registry_generation)
        identities.push_back(candidate);
    }
    const auto ceiling = context.maximum_typed_result_transport_bytes_per_packet;
    if (ceiling < api::kMinimumTypedResultTransportBytesPerPacket ||
        ceiling > api::kMaximumTypedResultTransportBytesPerPacket)
      return refuse("query descriptor packet budget is not engine-admitted", "RESOURCE.BUDGET_EXCEEDED");
    std::uint64_t bytes = wire::kTypedResultRowDescriptorHeaderBytes;
    std::map<std::string, std::uint32_t> occurrences;
    for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
      if (cancelled())
        return refuse("query result metadata publication cancelled", "PROCESS.CANCELLED");
      const auto& output = *outputs[ordinal];
      if (output.ordinal != ordinal ||
          output.descriptor_id != root->output_descriptor_ids[ordinal])
        return refuse("query root output order or descriptor binding is contradictory");
      if (!output.visible) continue;
      const auto found = descriptors.find(output.descriptor_id);
      if (found == descriptors.end()) return refuse("query output descriptor is absent");
      const auto& bound = *found->second;
      if (!bound.datatype_identity_authoritative ||
          bound.statement_receipt_uuid != context.statement_receipt_uuid ||
          bound.datatype_catalog_snapshot_uuid != context.datatype_catalog_snapshot_uuid ||
          bound.datatype_catalog_generation != context.datatype_catalog_generation ||
          bound.datatype_registry_generation != context.datatype_registry_generation)
        return refuse("query output descriptor is stale or belongs to another receipt");
      const dt::DatatypeTypeCodecIdentityRowV1* identity = nullptr;
      for (const auto& candidate : identities) {
        if (!ExactIdentity(bound, candidate)) continue;
        if (identity) return refuse("query datatype codec identity is ambiguous");
        identity = &candidate;
      }
      if (!identity) return refuse("query datatype codec identity does not resolve");
      const auto published = metadata->columns.size();
      if (published >= shape->columns.size() ||
          shape->columns[published].descriptor_uuid != bound.descriptor_uuid)
        return refuse("executed output descriptor differs from the bound query output");
      if (shape->columns[published].type_uuid != bound.type_uuid)
        return refuse("executed output carries a contradictory type identity");
      if (shape->columns[published].collation_uuid !=
          bound.collation_uuid.value_or(api::EngineUuid{}))
        return refuse("executed output carries a contradictory collation identity");
      api::EngineQueryResultColumnV1 column;
      if (!BinaryIdentity(bound.descriptor_uuid, &column.bound_descriptor_uuid) ||
          !BinaryIdentity(identity->descriptor_uuid, &column.transport.descriptor_uuid) ||
          !BinaryIdentity(identity->type_uuid, &column.transport.type_uuid))
        return refuse("query output system identity is not canonical UUIDv7");
      if (bound.collation_uuid) {
        wire::TypedResultUuid collation{};
        if (!BinaryIdentity(*bound.collation_uuid, &collation))
          return refuse("query output collation identity is invalid");
        column.collation_uuid = collation;
      }
      column.timezone_profile_id = bound.timezone_profile_id;
      column.width = bound.width;
      column.precision = bound.precision;
      column.scale = bound.scale;
      auto& transport = column.transport;
      transport.ordinal = static_cast<std::uint32_t>(published);
      transport.name = output.output_name_utf8;
      if (!wire::ValidTypedResultColumnName(transport.name))
        return refuse("query output name is invalid");
      transport.name_occurrence = occurrences[transport.name]++;
      switch (bound.nullability) {
        case api::RelationalNullability::kNonNull:
          transport.nullability = wire::TypedResultNullability::not_null; break;
        case api::RelationalNullability::kNullable:
          transport.nullability = wire::TypedResultNullability::nullable; break;
        case api::RelationalNullability::kUnknown:
          transport.nullability = wire::TypedResultNullability::unknown; break;
        default: return refuse("query output nullability is invalid");
      }
      transport.descriptor_generation = identity->descriptor_generation;
      transport.type_generation = identity->type_generation;
      transport.canonical_type_id = static_cast<dt::CanonicalTypeId>(identity->canonical_binary_type_code);
      transport.codec_id = identity->codec_id;
      transport.codec_version = identity->codec_version;
      transport.codec_generation = identity->codec_generation;
      transport.canonical_value_bytes = identity->canonical_value_bytes;
      bytes += wire::kTypedResultColumnDescriptorPrefixBytes +
               transport.name.size() + transport.codec_id.size();
      if (bytes > ceiling)
        return refuse("query descriptor exceeds the live packet budget", "RESOURCE.BUDGET_EXCEEDED");
      metadata->columns.push_back(std::move(column));
    }
    if (metadata->columns.size() != shape->columns.size())
      return refuse("executed output column count differs from visible query outputs");
    for (const auto& row : shape->rows) {
      if (cancelled())
        return refuse("query result metadata publication cancelled", "PROCESS.CANCELLED");
      if (row.fields.size() != metadata->columns.size())
        return refuse("executed row width differs from query schema");
      for (std::size_t index = 0; index < row.fields.size(); ++index) {
        const auto& field = row.fields[index];
        if (field.first != metadata->columns[index].transport.name ||
            !SameExactEngineDescriptorV1(field.second.descriptor, shape->columns[index]))
          return refuse("executed field identity differs from ordered query schema");
        const auto& value = field.second;
        if (value.state != api::EngineValueState::value &&
            value.state != api::EngineValueState::sql_null)
          return refuse("executed row contains a non-result value state");
        if (value.isSqlNull() &&
            (metadata->columns[index].transport.nullability == wire::TypedResultNullability::not_null ||
             !value.encoded_value.empty() || !value.binary_value.empty()))
          return refuse("executed SQL NULL contradicts the result descriptor or carries payload");
      }
    }
    if (cancelled())
      return refuse("query result metadata publication cancelled", "PROCESS.CANCELLED");
    shape->query_metadata = std::move(metadata);
    ObserveSchema(*shape->query_metadata, shape->rows.size());
    return true;
  } catch (const std::bad_alloc&) {
    return refuse("query result metadata allocation failed", "RESOURCE.BUDGET_EXCEEDED");
  } catch (...) {
    return refuse("query result metadata publication failed", "SBLR.EXECUTION_FAILED");
  }
}

}  // namespace scratchbird::engine::sblr
