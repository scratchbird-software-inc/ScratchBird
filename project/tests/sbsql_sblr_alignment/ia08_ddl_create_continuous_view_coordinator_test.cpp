#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_ddl_create_continuous_view_coordinator.hpp"
#include <cassert>
// The coordinator admits only an authenticated statement: it requires
// security_context_present and the receipt to BE the statement identity
// (`f99fddf9d`, "Migrate SBLR coordinator receipt contracts to binary UUIDs").
// This fixture passed a default-constructed context, whose statement identity
// is nil, so `receipt != c.statement_uuid` could never hold and the compile
// could never succeed. The later test commit only restored compilation, not
// this expectation.

int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c{};c.security_context_present=true;c.statement_uuid=scratchbird::tests::FixtureUuid(1113, 43);auto x=CompileSblrDdlCreateContinuousViewDescriptor(c,c.statement_uuid,1,1,1);assert(x.ok);auto y=ConsumeSblrDdlCreateContinuousViewDescriptor(c,x.descriptor);assert(y.ok);auto z=ConsumeSblrDdlCreateContinuousViewDescriptor(c,x.descriptor);assert(!z.ok);assert(z.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
