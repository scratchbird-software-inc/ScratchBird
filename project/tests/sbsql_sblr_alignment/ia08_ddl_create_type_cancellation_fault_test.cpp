#include "engine/internal_api/sblr_ddl_create_type_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <string>

#ifdef NDEBUG
#undef assert
#define assert(condition) \
  ((condition) ? static_cast<void>(0) : std::abort())
#endif

namespace {
template <typename Descriptor>
bool IsZero(const Descriptor& descriptor) {
  return descriptor.availability == 0 &&
         std::all_of(descriptor.body.begin(), descriptor.body.end(),
                     [](std::uint8_t byte) { return byte == 0; }) &&
         std::all_of(descriptor.evidence.begin(), descriptor.evidence.end(),
                     [](std::uint8_t byte) { return byte == 0; });
}
}  // namespace

int main() {
  namespace api = scratchbird::engine::internal_api;
  namespace sblr = scratchbird::engine::sblr;

  std::atomic<unsigned> cancellation_probes{0};
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_uuid.canonical =
      "019d0000-0000-7000-8000-000000002958";
  context.trace_tags = {"private_ddl_create_type_binder"};
  context.query_cancellation_requested = [&]() {
    ++cancellation_probes;
    return true;
  };

  const auto refused = api::CompileSblrDdlCreateTypeDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 1);
  assert(!refused.ok);
  assert(refused.diagnostic.code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(IsZero(refused.descriptor));
  assert(cancellation_probes.load() == 0);

  const auto refused_again = api::CompileSblrDdlCreateTypeDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 1);
  assert(!refused_again.ok);
  assert(refused_again.diagnostic.code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(IsZero(refused_again.descriptor));
  assert(cancellation_probes.load() == 0);

  const auto missing_occurrence = api::CompileSblrDdlCreateTypeDescriptor(
      context, context.statement_uuid.canonical, 0, 1, 1);
  assert(!missing_occurrence.ok);
  assert(missing_occurrence.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(IsZero(missing_occurrence.descriptor));

  const auto missing_domain_occurrence =
      api::CompileSblrDdlCreateTypeDescriptor(
          context, context.statement_uuid.canonical, 1, 0, 1);
  assert(!missing_domain_occurrence.ok);
  assert(missing_domain_occurrence.diagnostic.code == "SBLR.OPERAND_INVALID");

  const auto missing_availability = api::CompileSblrDdlCreateTypeDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 0);
  assert(!missing_availability.ok);
  assert(missing_availability.diagnostic.code == "SBLR.OPERAND_INVALID");

  auto missing_binder = context;
  missing_binder.trace_tags.clear();
  const auto unbound = api::CompileSblrDdlCreateTypeDescriptor(
      missing_binder, context.statement_uuid.canonical, 1, 1, 1);
  assert(!unbound.ok);
  assert(unbound.diagnostic.code == "SBLR.OPERAND_INVALID");

  auto missing_snapshot = context;
  missing_snapshot.statement_metadata_snapshot_engine_owned = false;
  const auto snapshot_invalid = api::CompileSblrDdlCreateTypeDescriptor(
      missing_snapshot, context.statement_uuid.canonical, 1, 1, 1);
  assert(!snapshot_invalid.ok);
  assert(snapshot_invalid.diagnostic.code == "SBLR.OPERAND_INVALID");

  const auto receipt_invalid = api::CompileSblrDdlCreateTypeDescriptor(
      context, "019d0000-0000-7000-8000-000000002959", 1, 1, 1);
  assert(!receipt_invalid.ok);
  assert(receipt_invalid.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(cancellation_probes.load() == 0);

  sblr::SblrDdlCreateTypeDescriptorV1 seed;
  seed.body[0] = 1;
  seed.availability = 1;
  const auto encoded_seed =
      sblr::EncodeSblrDdlCreateTypeDescriptorV1(seed, false);
  sblr::SblrDdlCreateTypeDescriptorV1 canonical;
  std::string decode_detail;
  assert(!encoded_seed.empty());
  assert(sblr::DecodeSblrDdlCreateTypeDescriptorV1(
      encoded_seed.data(), encoded_seed.size(), &canonical, &decode_detail,
      false));
  assert(!sblr::EncodeSblrDdlCreateTypeDescriptorV1(canonical, true).empty());

  auto malformed_descriptor = canonical;
  malformed_descriptor.evidence[0] ^= 1;
  const auto malformed = api::ConsumeSblrDdlCreateTypeDescriptor(
      context, malformed_descriptor);
  assert(!malformed.ok);
  assert(malformed.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(IsZero(malformed.descriptor));
  assert(cancellation_probes.load() == 0);

  const auto hidden =
      api::ConsumeSblrDdlCreateTypeDescriptor(context, canonical);
  assert(!hidden.ok);
  assert(hidden.diagnostic.code == "SECURITY.ACCESS_DENIED");
  assert(IsZero(hidden.descriptor));
  assert(cancellation_probes.load() == 0);

  auto unauthorized = context;
  unauthorized.security_context_present = false;
  unauthorized.trace_tags = {"private_ddl_create_type"};
  const auto denied =
      api::ConsumeSblrDdlCreateTypeDescriptor(unauthorized, canonical);
  assert(!denied.ok);
  assert(denied.diagnostic.code == "SECURITY.ACCESS_DENIED");
  assert(IsZero(denied.descriptor));
  assert(cancellation_probes.load() == 0);

  context.trace_tags = {"private_ddl_create_type"};
  const auto consume_refused =
      api::ConsumeSblrDdlCreateTypeDescriptor(context, canonical);
  assert(!consume_refused.ok);
  assert(consume_refused.diagnostic.code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(IsZero(consume_refused.descriptor));
  assert(cancellation_probes.load() == 0);

  const auto consume_refused_again =
      api::ConsumeSblrDdlCreateTypeDescriptor(context, canonical);
  assert(!consume_refused_again.ok);
  assert(consume_refused_again.diagnostic.code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(IsZero(consume_refused_again.descriptor));
  assert(cancellation_probes.load() == 0);
  return 0;
}
