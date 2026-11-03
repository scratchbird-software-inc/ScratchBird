#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "../support/scratch_database_path.hpp"
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path=scratchbird::tests::ScratchAvailabilityDatabasePath("sb_drop_procedure_2643");c.database_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002643");c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{a::kSblrDdlDropProcedureExecutorId,1556,"1.0",a::kSblrDdlDropProcedureOperandDescriptorId,a::kSblrDdlDropProcedureResultDescriptorId,1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);return s.ok&&s.snapshot.installed?0:1;}
