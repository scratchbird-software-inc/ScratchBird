#include "sblr_dml_counter_add_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {

std::mutex g_mutex;
std::map<std::string, bool> g_live;
std::map<std::string, bool> g_used;

std::string DescriptorKey(
    const scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1& descriptor)
{
    return std::string(reinterpret_cast<const char*>(descriptor.evidence.data()),
                       descriptor.evidence.size());
}

bool FinalizeDescriptorEvidence(
    scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1* descriptor)
{
    if (descriptor == nullptr) {
        return false;
    }
    const auto wire =
        scratchbird::engine::sblr::EncodeSblrDmlCounterAddDescriptorV1(*descriptor, false);
    std::string detail;
    scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1 canonical;
    if (wire.empty() ||
        !scratchbird::engine::sblr::DecodeSblrDmlCounterAddDescriptorV1(
            wire.data(), wire.size(), &canonical, &detail, false)) {
        return false;
    }
    *descriptor = canonical;
    return true;
}

}  // namespace

SblrDmlCounterAddCoordinationResult CompileSblrDmlCounterAddDescriptor(
    const EngineRequestContext& context,
    const std::string& receipt,
    std::uint64_t occurrence,
    std::uint64_t counter_occurrence,
    std::uint64_t availability)
{
    SblrDmlCounterAddCoordinationResult result;
    if (!context.security_context_present ||
        !context.statement_metadata_snapshot_engine_owned ||
        receipt != context.statement_uuid.canonical || occurrence == 0 ||
        counter_occurrence == 0 || availability == 0) {
        result.diagnostic = MakeEngineApiDiagnostic(
            "SBLR.OPERAND.INVALID",
            "sblr.dml_counter_add.coordination_invalid",
            {},
            false);
        return result;
    }

    result.descriptor.body[0] = 1;
    result.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
    result.descriptor.body[2] = static_cast<std::uint8_t>(counter_occurrence);
    result.descriptor.availability = availability;
    if (!FinalizeDescriptorEvidence(&result.descriptor)) {
        result.diagnostic = MakeEngineApiDiagnostic(
            "SBLR.OPERAND.INVALID",
            "sblr.dml_counter_add.descriptor_invalid",
            {},
            false);
        return result;
    }

    std::lock_guard lock(g_mutex);
    g_live[DescriptorKey(result.descriptor)] = true;
    result.ok = true;
    return result;
}

SblrDmlCounterAddCoordinationResult ConsumeSblrDmlCounterAddDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1& descriptor)
{
    SblrDmlCounterAddCoordinationResult result;
    if (!context.security_context_present) {
        result.diagnostic = MakeEngineApiDiagnostic(
            "SECURITY.ACCESS_DENIED", "sblr.dml_counter_add.hidden", {}, false);
        return result;
    }

    std::lock_guard lock(g_mutex);
    const auto descriptor_key = DescriptorKey(descriptor);
    if (!g_live[descriptor_key]) {
        result.diagnostic = MakeEngineApiDiagnostic(
            g_used[descriptor_key] ? "MGA.TRANSACTION.STALE" : "SECURITY.ACCESS_DENIED",
            g_used[descriptor_key] ? "sblr.dml_counter_add.stale"
                                   : "sblr.dml_counter_add.hidden",
            {},
            false);
        return result;
    }
    if (context.query_cancellation_requested && context.query_cancellation_requested()) {
        result.diagnostic = MakeEngineApiDiagnostic(
            "PROCESS.CANCELLED", "sblr.dml_counter_add.cancelled", {}, false);
        return result;
    }

    g_live.erase(descriptor_key);
    g_used[descriptor_key] = true;
    result.ok = true;
    result.descriptor = descriptor;
    return result;
}

}  // namespace scratchbird::engine::internal_api
