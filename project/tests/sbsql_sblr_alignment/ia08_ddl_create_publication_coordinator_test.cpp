#include "engine/internal_api/sblr_ddl_create_publication_coordinator.hpp"
#include <cassert>

using namespace scratchbird::engine::internal_api;

int main() {
  EngineRequestContext binder;
  binder.security_context_present = true;
  binder.statement_metadata_snapshot_engine_owned = true;
  binder.statement_uuid.canonical = "publication";
  binder.trace_tags = {"private_ddl_create_publication_binder"};
  auto made = CompileSblrDdlCreatePublicationDescriptor(
      binder, binder.statement_uuid.canonical, 1, 1, 1);
  assert(made.ok);

  auto hidden = binder;
  hidden.trace_tags = {"private_ddl_create_publication_binder"};
  assert(ConsumeSblrDdlCreatePublicationDescriptor(hidden, made.descriptor)
             .diagnostic.code == "SECURITY.ACCESS_DENIED");

  auto cancelled = binder;
  cancelled.trace_tags = {"private_ddl_create_publication"};
  cancelled.query_cancellation_requested = [] { return true; };
  assert(ConsumeSblrDdlCreatePublicationDescriptor(cancelled, made.descriptor)
             .diagnostic.code == "PROCESS.CANCELLED");

  cancelled.query_cancellation_requested = [] { return false; };
  auto consumed = ConsumeSblrDdlCreatePublicationDescriptor(cancelled, made.descriptor);
  assert(consumed.ok);
  auto replay = ConsumeSblrDdlCreatePublicationDescriptor(cancelled, made.descriptor);
  assert(!replay.ok && replay.diagnostic.code == "MGA.TRANSACTION.STALE");
  return 0;
}
