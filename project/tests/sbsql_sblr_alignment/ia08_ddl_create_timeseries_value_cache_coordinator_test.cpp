#include "engine/internal_api/sblr_ddl_create_timeseries_value_cache_coordinator.hpp"

#include <cassert>

int main()
{
    using namespace scratchbird::engine::internal_api;
    namespace sblr = scratchbird::engine::sblr;

    EngineRequestContext context{};
    context.security_context_present = true;
    context.statement_metadata_snapshot_engine_owned = true;
    context.statement_uuid.canonical = "s";
    const auto coordinated = CompileSblrDdlCreateTimeseriesValueCacheDescriptor(
        context, "s", 1, 2, 1);
    assert(coordinated.ok);

    const auto descriptor_wire =
        sblr::EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(
            coordinated.descriptor, false);
    assert(descriptor_wire.size() == 488);
    sblr::SblrDdlCreateTimeseriesValueCacheDescriptorV1 transported_descriptor;
    assert(sblr::DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(
        descriptor_wire.data(), descriptor_wire.size(), &transported_descriptor,
        nullptr, false));

    const auto consumed = ConsumeSblrDdlCreateTimeseriesValueCacheDescriptor(
        context, transported_descriptor);
    assert(consumed.ok);
    const auto replay = ConsumeSblrDdlCreateTimeseriesValueCacheDescriptor(
        context, transported_descriptor);
    assert(!replay.ok && replay.diagnostic.code == "MGA.TRANSACTION.STALE");
    return 0;
}
