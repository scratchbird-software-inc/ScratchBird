// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_event_notification.hpp"

#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kHeader = 80, kDigest = 32, kMaximum = 16384;
struct Spec { SblrEventNotificationOpcode code; std::string_view op, mnemonic, type; bool transaction; std::initializer_list<std::uint16_t> tags; };
const std::array<Spec, 10>& Specs() { static const std::array<Spec, 10> specs{{
 {SblrEventNotificationOpcode::channel_create,"engine.op.event_channel_create","SBLR_EVENT_CHANNEL_CREATE","event.channel_create.v1",true,{1,2,3,4,5,6}},
 {SblrEventNotificationOpcode::channel_alter,"engine.op.event_channel_alter","SBLR_EVENT_CHANNEL_ALTER","event.channel_alter.v1",true,{1,2,3,4,5,6,7}},
 {SblrEventNotificationOpcode::channel_drop,"engine.op.event_channel_drop","SBLR_EVENT_CHANNEL_DROP","event.channel_drop.v1",true,{1,2}},
 {SblrEventNotificationOpcode::channel_listen,"engine.op.event_channel_listen","SBLR_EVENT_CHANNEL_LISTEN","event.channel_listen.v1",true,{1,2,3,4}},
 {SblrEventNotificationOpcode::channel_unlisten,"engine.op.event_channel_unlisten","SBLR_EVENT_CHANNEL_UNLISTEN","event.channel_unlisten.v1",true,{1,2,3}},
 {SblrEventNotificationOpcode::channel_unlisten_all,"engine.op.event_channel_unlisten_all","SBLR_EVENT_CHANNEL_UNLISTEN_ALL","event.session_unlisten_all.v1",true,{1}},
 {SblrEventNotificationOpcode::channel_notify,"engine.op.event_channel_notify","SBLR_EVENT_CHANNEL_NOTIFY","event.channel_notify.v1",true,{1,2,3,4,5,6}},
 {SblrEventNotificationOpcode::subscription_list,"engine.op.event_subscription_list","SBLR_EVENT_SUBSCRIPTION_LIST","event.subscription_list.v1",false,{1,2,3}},
 {SblrEventNotificationOpcode::delivery_poll,"engine.op.event_delivery_poll","SBLR_EVENT_DELIVERY_POLL","event.delivery_poll.v1",false,{1,2,3,4}},
 {SblrEventNotificationOpcode::delivery_ack,"engine.op.event_delivery_ack","SBLR_EVENT_DELIVERY_ACK","event.delivery_ack.v1",false,{1,2,3,4,5}},
 }}; return specs; }
const Spec* Find(SblrEventNotificationOpcode opcode) { for (const auto& spec : Specs()) if (spec.code == opcode) return &spec; return nullptr; }
const Spec* Find(std::string_view operation) { for (const auto& spec : Specs()) if (spec.op == operation) return &spec; return nullptr; }
void Put16(Bytes* out, std::size_t i, std::uint16_t n) { (*out)[i]=n; (*out)[i+1]=n>>8; }
void Put32(Bytes* out, std::size_t i, std::uint32_t n) { for(unsigned s=0;s<32;s+=8)(*out)[i+s/8]=n>>s; }
void Put64(Bytes* out, std::size_t i, std::uint64_t n) { for(unsigned s=0;s<64;s+=8)(*out)[i+s/8]=n>>s; }
std::uint16_t Get16(const std::uint8_t* p) { return p[0] | static_cast<std::uint16_t>(p[1])<<8; }
std::uint32_t Get32(const std::uint8_t* p) { return p[0]|static_cast<std::uint32_t>(p[1])<<8|static_cast<std::uint32_t>(p[2])<<16|static_cast<std::uint32_t>(p[3])<<24; }
std::uint64_t Get64(const std::uint8_t* p) { std::uint64_t n=0; for(unsigned s=0;s<64;s+=8)n|=static_cast<std::uint64_t>(p[s/8])<<s; return n; }
bool Zero(const std::uint8_t* p, std::size_t n) { return std::all_of(p,p+n,[](std::uint8_t v){return v==0;}); }
bool Uuid(const Bytes& value, bool zero_allowed = false) {
  return value.size() == 16 && (zero_allowed || !Zero(value.data(), value.size()));
}
bool Text(const Bytes& value, std::size_t minimum, std::size_t maximum) {
  if (value.size() < minimum || value.size() > maximum) return false;
  for (std::size_t at = 0; at < value.size();) {
    const std::uint8_t lead = value[at++];
    if (lead < 0x80) {
      if (lead < 0x20 || lead == 0x7f) return false;
      continue;
    }
    std::uint32_t codepoint = 0;
    unsigned continuation = 0;
    if ((lead & 0xe0) == 0xc0) { codepoint = lead & 0x1f; continuation = 1; }
    else if ((lead & 0xf0) == 0xe0) { codepoint = lead & 0x0f; continuation = 2; }
    else if ((lead & 0xf8) == 0xf0) { codepoint = lead & 0x07; continuation = 3; }
    else return false;
    if (at + continuation > value.size()) return false;
    for (unsigned index = 0; index < continuation; ++index) {
      const std::uint8_t byte = value[at++];
      if ((byte & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if ((continuation == 1 && codepoint < 0x80) ||
        (continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        (codepoint >= 0x80 && codepoint <= 0x9f)) return false;
  }
  return true;
}
bool Enum(const Bytes& value, std::initializer_list<std::string_view> admitted) {
  if (value.empty()) return false;
  const std::string_view text(reinterpret_cast<const char*>(value.data()), value.size());
  return std::find(admitted.begin(), admitted.end(), text) != admitted.end();
}
bool U32(const Bytes& value, std::uint32_t minimum, std::uint32_t maximum) {
  if (value.size() != 4) return false;
  const auto number = Get32(value.data());
  return number >= minimum && number <= maximum;
}
bool U64Nonzero(const Bytes& value) {
  return value.size() == 8 && Get64(value.data()) != 0;
}
bool ValidField(SblrEventNotificationOpcode opcode,
                std::uint16_t tag,
                const Bytes& value) {
  switch (opcode) {
    case SblrEventNotificationOpcode::channel_create:
      switch (tag) {
        case 1: case 3: case 4: return Uuid(value);
        case 2: return Text(value, 1, 128);
        case 5: return Enum(value, {"normal", "hidden"});
        case 6: return Enum(value, {"none", "redact_payload"});
      }
      break;
    case SblrEventNotificationOpcode::channel_alter:
      switch (tag) {
        case 1: case 3: case 4: return Uuid(value);
        case 2: return Text(value, 0, 128);
        case 5: return Enum(value, {"active", "disabled"});
        case 6: return Enum(value, {"normal", "hidden"});
        case 7: return Enum(value, {"none", "redact_payload"});
      }
      break;
    case SblrEventNotificationOpcode::channel_drop:
      if (tag == 1) return Uuid(value);
      if (tag == 2) return Enum(value, {"restrict", "retire"});
      break;
    case SblrEventNotificationOpcode::channel_listen:
      if (tag >= 1 && tag <= 3) return Uuid(value);
      if (tag == 4) return Enum(value, {"native_message", "firebird_compat"});
      break;
    case SblrEventNotificationOpcode::channel_unlisten:
      return tag >= 1 && tag <= 3 && Uuid(value);
    case SblrEventNotificationOpcode::channel_unlisten_all:
      return tag == 1 && Uuid(value);
    case SblrEventNotificationOpcode::channel_notify:
      switch (tag) {
        case 1: case 3: case 6: return Uuid(value);
        case 2: return Enum(value, {"native_message", "firebird_compat"});
        case 4: return Text(value, 0, 8192);
        case 5: return Uuid(value, true);
      }
      break;
    case SblrEventNotificationOpcode::subscription_list:
      if (tag == 1) return Uuid(value);
      if (tag == 2) return U32(value, 1, 256);
      if (tag == 3) return value.empty() || value.size() == 32;
      break;
    case SblrEventNotificationOpcode::delivery_poll:
      if (tag == 1 || tag == 2) return Uuid(value);
      if (tag == 3) return U32(value, 1, 256);
      if (tag == 4) return value.empty() || value.size() == 32;
      break;
    case SblrEventNotificationOpcode::delivery_ack:
      if (tag == 1 || tag == 2 || tag == 3 || tag == 5) return Uuid(value);
      if (tag == 4) return U64Nonzero(value);
      break;
  }
  return false;
}
SblrEventNotificationCodecResult Fail(std::string detail) { SblrEventNotificationCodecResult out; out.diagnostic_id="EVENT.REQUEST_INVALID"; out.detail=std::move(detail); return out; }
SblrEventNotificationDispatchResult DispatchFail(std::string id, std::string detail) { SblrEventNotificationDispatchResult out; out.diagnostic_id=std::move(id); out.detail=std::move(detail); return out; }
std::string Hex16(const std::array<std::uint8_t,16>& v) { static constexpr char h[]="0123456789abcdef"; std::string out; out.reserve(32); for(auto b:v){out+=h[b>>4];out+=h[b&15];} return out; }
SblrEventNotificationCodecResult Validate(const SblrEventNotificationRecord& record) {
 const auto* spec=Find(record.opcode); if(!spec || Zero(record.request_uuid.data(),16)||Zero(record.security_context_uuid.data(),16)) return Fail("opcode_or_identity");
 if (spec->transaction != (record.transaction_id != 0)) return Fail("transaction_requirement");
 if(record.fields.size()!=spec->tags.size()) return Fail("field_count");
 for(std::size_t i=0;i<record.fields.size();++i) {
   if(record.fields[i].tag!=*(spec->tags.begin()+i)||
      record.fields[i].value.size()>8192||
      !ValidField(record.opcode, record.fields[i].tag, record.fields[i].value)) {
     return Fail("field_tag_type_or_limit");
   }
 }
 SblrEventNotificationCodecResult out; out.ok=true; out.record=record; return out;
}
}  // namespace

bool IsSblrEventNotificationOperation(std::string_view id) noexcept { return Find(id)!=nullptr; }
std::string_view SblrEventNotificationOperationId(SblrEventNotificationOpcode code) noexcept { const auto* s=Find(code); return s?s->op:std::string_view{}; }
std::string_view SblrEventNotificationMnemonic(SblrEventNotificationOpcode code) noexcept { const auto* s=Find(code); return s?s->mnemonic:std::string_view{}; }
std::string_view SblrEventNotificationOperandType(SblrEventNotificationOpcode code) noexcept { const auto* s=Find(code); return s?s->type:std::string_view{}; }
SblrEventNotificationCodecResult EncodeSblrEventNotificationRecord(const SblrEventNotificationRecord& record) {
 auto out=Validate(record); if(!out.ok)return out; std::size_t payload=0; for(const auto& f:record.fields)payload+=8+f.value.size(); const std::size_t size=kHeader+payload+kDigest; if(size<112||size>kMaximum)return Fail("frame_size"); Bytes frame(size,0); frame[0]='S';frame[1]='B';frame[2]='E';frame[3]='N';Put16(&frame,4,1);Put16(&frame,6,0);Put16(&frame,8,static_cast<std::uint16_t>(record.opcode));Put32(&frame,12,size);std::copy(record.request_uuid.begin(),record.request_uuid.end(),frame.begin()+16);std::copy(record.security_context_uuid.begin(),record.security_context_uuid.end(),frame.begin()+32);Put64(&frame,48,record.policy_epoch);Put64(&frame,56,record.transaction_id);Put32(&frame,64,payload);Put32(&frame,68,record.fields.size()); std::size_t at=kHeader;for(const auto& f:record.fields){Put16(&frame,at,f.tag);Put32(&frame,at+4,f.value.size());at+=8;std::copy(f.value.begin(),f.value.end(),frame.begin()+at);at+=f.value.size();} const auto digest=scratchbird::core::hash::ComputeSha256Digest(frame.data(),size-kDigest);if(!digest.ok())return Fail("sha256_unavailable");std::copy(digest.digest.begin(),digest.digest.end(),frame.begin()+size-kDigest);out.canonical_bytes=std::move(frame);out.sha256_hex=scratchbird::core::hash::HexLower(digest.digest);return out;
}
SblrEventNotificationCodecResult DecodeSblrEventNotificationRecord(const std::uint8_t* data,std::size_t size) {
 if(!data||size<112||size>kMaximum||data[0]!='S'||data[1]!='B'||data[2]!='E'||data[3]!='N'||Get16(data+4)!=1||Get16(data+6)!=0||Get16(data+10)!=0||Get32(data+12)!=size||Get64(data+72)!=0)return Fail("frame_header"); const auto* spec=Find(static_cast<SblrEventNotificationOpcode>(Get16(data+8)));const auto payload=Get32(data+64),count=Get32(data+68);if(!spec||count!=spec->tags.size()||kHeader+payload+kDigest!=size)return Fail("opcode_or_payload");const auto digest=scratchbird::core::hash::ComputeSha256Digest(data,size-kDigest);if(!digest.ok()||!std::equal(digest.digest.begin(),digest.digest.end(),data+size-kDigest))return Fail("sha256_mismatch");SblrEventNotificationRecord record;record.opcode=spec->code;std::copy_n(data+16,16,record.request_uuid.begin());std::copy_n(data+32,16,record.security_context_uuid.begin());record.policy_epoch=Get64(data+48);record.transaction_id=Get64(data+56);std::size_t at=kHeader;for(std::size_t i=0;i<count;++i){if(at+8>size-kDigest||Get16(data+at+2)!=0)return Fail("field_header");auto tag=Get16(data+at);auto bytes=Get32(data+at+4);at+=8;if(at+bytes>size-kDigest)return Fail("field_size");record.fields.push_back({tag,Bytes(data+at,data+at+bytes)});at+=bytes;}if(at!=size-kDigest)return Fail("payload_size");auto out=Validate(record);if(!out.ok)return out;out.canonical_bytes.assign(data,data+size);out.sha256_hex=scratchbird::core::hash::HexLower(digest.digest);return out;
}
SblrEventNotificationCodecResult DecodeSblrEventNotificationOperand(const SblrOperationEnvelope& envelope) {
 const auto* spec=Find(envelope.operation_id);if(!spec||envelope.opcode!=spec->mnemonic||envelope.opcode_code!=static_cast<std::uint16_t>(spec->code)||envelope.operands.size()!=1)return Fail("sbop_identity");const auto& o=envelope.operands.front();if(o.type!=spec->type||o.name!="request"||o.ordinal!=1||o.value_kind!=SblrValueKind::literal_typed||o.value_body.size()<24||Zero(o.value_body.data(),16))return Fail("carrier");std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=static_cast<std::uint64_t>(o.value_body[16+i])<<(8*i);if(n!=o.value_body.size()-24)return Fail("carrier_size");auto decoded=DecodeSblrEventNotificationRecord(o.value_body.data()+24,n);if(!decoded.ok)return decoded;if(decoded.record.opcode!=spec->code)return Fail("carrier_opcode_mismatch");return decoded;
}
SblrOperand MakeSblrEventNotificationOperand(const SblrEventNotificationCodecResult& encoded) { SblrOperand o;if(!encoded.ok)return o;o.type=std::string(SblrEventNotificationOperandType(encoded.record.opcode));o.name="request";o.ordinal=1;o.value_kind=SblrValueKind::literal_typed;o.value_body.assign(24,0);o.value_body[0]=1;for(unsigned i=0;i<8;++i)o.value_body[16+i]=encoded.canonical_bytes.size()>>(8*i);o.value_body.insert(o.value_body.end(),encoded.canonical_bytes.begin(),encoded.canonical_bytes.end());return o; }
SblrEventNotificationDispatchResult DispatchSblrEventNotification(const SblrOperationEnvelope& envelope,const scratchbird::engine::internal_api::EngineRequestContext& context) { const auto registry=ValidateSblrOpcodeForEnvelope(envelope);if(!registry.ok)return DispatchFail(registry.diagnostic_id,registry.detail);const auto decoded=DecodeSblrEventNotificationOperand(envelope);if(!decoded.ok)return DispatchFail(decoded.diagnostic_id,decoded.detail);if(!context.security_context_present)return DispatchFail("SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED","event notification");if(context.query_cancellation_requested&&context.query_cancellation_requested())return DispatchFail("PROCESS.CANCELLED","cancelled_before_event_state_access");SblrEventNotificationDispatchResult out;out.accepted=true;out.record=decoded.record;out.evidence={{"executor_id",std::string(SblrEventNotificationOperationId(decoded.record.opcode))},{"opcode_code",std::to_string(static_cast<std::uint16_t>(decoded.record.opcode))},{"opcode_version","1.0"},{"request_uuid",Hex16(decoded.record.request_uuid)},{"security_context_uuid",Hex16(decoded.record.security_context_uuid)},{"policy_epoch",std::to_string(decoded.record.policy_epoch)},{"transaction_id",std::to_string(decoded.record.transaction_id)},{"request_sha256",decoded.sha256_hex}};return out; }
}  // namespace scratchbird::engine::sblr
