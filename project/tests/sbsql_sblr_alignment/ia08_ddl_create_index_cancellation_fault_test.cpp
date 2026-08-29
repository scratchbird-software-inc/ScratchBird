#include "engine/internal_api/sblr_ddl_create_index_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>

#ifdef NDEBUG
#undef assert
#define assert(condition) \
  ((condition) ? static_cast<void>(0) : std::abort())
#endif

int main() {
  namespace api = scratchbird::engine::internal_api;

  std::atomic<unsigned> cancellation_probes{0};
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_uuid.canonical =
      "019d0000-0000-7000-8000-000000002572";
  context.trace_tags = {"private_ddl_create_index_binder"};
  context.query_cancellation_requested = [&]() {
    ++cancellation_probes;
    return true;
  };

  const auto refused = api::CompileSblrDdlCreateIndexDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 1);
  assert(!refused.ok);
  assert(refused.diagnostic.code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(cancellation_probes.load() == 0);
  assert(std::all_of(refused.descriptor.evidence.begin(),
                     refused.descriptor.evidence.end(),
                     [](std::uint8_t byte) { return byte == 0; }));

  const auto malformed = api::CompileSblrDdlCreateIndexDescriptor(
      context, context.statement_uuid.canonical, 0, 1, 1);
  assert(!malformed.ok);
  assert(malformed.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(cancellation_probes.load() == 0);

  const auto missing_availability = api::CompileSblrDdlCreateIndexDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 0);
  assert(!missing_availability.ok);
  assert(missing_availability.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(cancellation_probes.load() == 0);

  auto empty_receipt_context = context;
  empty_receipt_context.statement_uuid.canonical.clear();
  const auto empty_receipt = api::CompileSblrDdlCreateIndexDescriptor(
      empty_receipt_context, {}, 1, 1, 1);
  assert(!empty_receipt.ok);
  assert(empty_receipt.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(cancellation_probes.load() == 0);

  scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1 foreign;
  foreign.evidence[0] = 1;

  const auto malformed_consume =
      api::ConsumeSblrDdlCreateIndexDescriptor(context, foreign);
  assert(!malformed_consume.ok);
  assert(malformed_consume.diagnostic.code == "SBLR.OPERAND_INVALID");
  assert(cancellation_probes.load() == 0);

  scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1 seed;
  seed.body[0] = 1;
  seed.availability = 1;
  const auto encoded_seed =
      scratchbird::engine::sblr::EncodeSblrDdlCreateIndexDescriptorV1(
          seed, false);
  scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1 canonical;
  std::string decode_detail;
  assert(!encoded_seed.empty());
  assert(scratchbird::engine::sblr::DecodeSblrDdlCreateIndexDescriptorV1(
      encoded_seed.data(), encoded_seed.size(), &canonical, &decode_detail,
      false));
  assert(!scratchbird::engine::sblr::EncodeSblrDdlCreateIndexDescriptorV1(
              canonical, true)
              .empty());

  const auto hidden =
      api::ConsumeSblrDdlCreateIndexDescriptor(context, canonical);
  assert(!hidden.ok);
  assert(hidden.diagnostic.code == "SECURITY.ACCESS_DENIED");
  assert(cancellation_probes.load() == 0);

  context.trace_tags = {"private_ddl_create_index"};
  const auto consume_refused =
      api::ConsumeSblrDdlCreateIndexDescriptor(context, canonical);
  assert(!consume_refused.ok);
  assert(consume_refused.diagnostic.code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(cancellation_probes.load() == 0);
  return 0;
}
