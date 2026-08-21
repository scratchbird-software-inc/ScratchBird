#include <cassert>
#include "engine/internal_api/sblr_admin_register_external_relation_resolver_coordinator.hpp"
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="resolver";c.trace_tags={"private_external_relation_resolver_binder"};auto x=CompileSblrAdminRegisterExternalRelationResolverDescriptor(c,"resolver",1,1,1);assert(x.ok);auto y=c;y.trace_tags={"private_external_relation_resolver"};auto z=ConsumeSblrAdminRegisterExternalRelationResolverDescriptor(y,x.descriptor);assert(z.ok);auto r=ConsumeSblrAdminRegisterExternalRelationResolverDescriptor(y,x.descriptor);assert(!r.ok&&r.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
