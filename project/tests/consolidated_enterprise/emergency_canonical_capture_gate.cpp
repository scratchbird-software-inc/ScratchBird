// SPDX-License-Identifier: MPL-2.0
// Reuse the independent wire fixture and process allocation-failure harness.
// This fixture proves structural transport/custody, not source authorization.
#define main LegacyMessageVectorCodecMain
#include "../sbsql_sblr_alignment/message_vector_set_codec_test.cpp"
#undef main
#include "emergency_diagnostic_queue.hpp"

int main() {
  namespace mem=scratchbird::core::memory;
  auto policy=mem::DefaultLocalEngineMemoryPolicy();
  policy.hard_limit_bytes=1024*1024;
  policy.per_context_limit_bytes=1024*1024;
  mem::MemoryManager manager(policy);
  mem::MemoryTag tag;
  tag.category=mem::MemoryCategory::diagnostics;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::owner]=Id(40);
  tag.binary_ownership[mem::MemoryBinaryScopeKind::context]=Id(41);
  const auto prepared=Base();
  const auto oracle=Raw(prepared);
  {
    mem::EmergencyDiagnosticQueue queue(manager,tag);
    Check(queue.Initialize(1,oracle.size())==mem::EmergencyCaptureStatus::ok,
          "canonical capture queue initialization failed");
    auto scratch=manager.AllocateScoped(oracle.size()*2,64,tag);
    Check(scratch.ok(),"canonical encoding/delivery scratch not admitted");
    if(!scratch.ok()) return EXIT_FAILURE;
    auto* data=static_cast<std::uint8_t*>(scratch.allocation.data());
    std::span<std::uint8_t> encoding(data,oracle.size());
    std::span<std::byte> delivery(reinterpret_cast<std::byte*>(data+oracle.size()),oracle.size());
    const auto charge=manager.Snapshot().current_bytes;
    fail_after=0;
    const auto encoded=m::EncodeMessageSetInto(prepared,encoding);
    const auto captured=queue.TryCapture(std::as_bytes(encoding));
    // Reusing producer scratch cannot alter the retained canonical record.
    std::fill(encoding.begin(),encoding.end(),0xff);
    const auto full=queue.TryCapture(std::as_bytes(encoding));
    const auto failed_sink=+[](void*,std::span<const std::byte>) noexcept {
      return mem::EmergencyDiagnosticSinkReceipt{0,false};
    };
    const auto failed=mem::DeliverEmergencyDiagnostic(queue,delivery,failed_sink,nullptr);
    const auto retained=queue.TryRead(delivery);
    fail_after=-1;
    Check(encoded.status==m::BoundedSetStatus::ok && captured.ok() &&
          full.status==mem::EmergencyCaptureStatus::full && !failed.delivered && retained.ok() &&
          retained.sequence==captured.sequence && manager.Snapshot().current_bytes==charge,
          "OOM encoding/capture/backpressure lost ownership or accounting");
    const std::span<const std::uint8_t> restored(
        reinterpret_cast<const std::uint8_t*>(delivery.data()),delivery.size());
    Check(std::equal(oracle.begin(),oracle.end(),restored.begin()),
          "retained canonical bytes differ from independent oracle");
    m::MessageSet decoded;
    Check(m::DecodeMessageSet(restored,&decoded)==E::none && decoded==prepared,
          "capture lost canonical identities, typed parameters, causes or policy");
    struct SinkState { const B* expected; bool exact=false; } state{&oracle};
    const auto sink=+[](void* context,std::span<const std::byte> bytes) noexcept {
      auto& s=*static_cast<SinkState*>(context);
      s.exact=bytes.size()==s.expected->size() && std::equal(bytes.begin(),bytes.end(),
          s.expected->begin(),[](std::byte a,std::uint8_t b){return std::to_integer<unsigned>(a)==b;});
      return mem::EmergencyDiagnosticSinkReceipt{bytes.size(),s.exact};
    };
    fail_after=0;
    const auto sent=mem::DeliverEmergencyDiagnostic(queue,delivery,sink,&state);
    fail_after=-1;
    Check(state.exact && sent.delivered && sent.retired && queue.Snapshot().queued==0 &&
          manager.Snapshot().current_bytes==charge,"canonical delivery retired wrong bytes or freed backing early");
  }
  Check(manager.Snapshot().current_bytes==0,"canonical capture leaked governed backing");
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
