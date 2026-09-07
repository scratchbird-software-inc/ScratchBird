#include "sblr_ddl_create_procedure_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
namespace scratchbird::engine::sblr { namespace { void p(std::vector<uint8_t>&o,uint64_t v,size_t n){for(size_t i=0;i<n;i++)o.push_back(uint8_t(v>>(8*i)));} uint64_t g(const uint8_t*b,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v|=uint64_t(b[i])<<(8*i);return v;} template<class T>bool nz(const T&x){return std::any_of(x.begin(),x.end(),[](auto v){return v!=0;});} bool uuid_v7(const DdlCreateProcedureUuid& value){return nz(value)&&(value[6]&0xf0u)==0x70u&&(value[8]&0xc0u)==0x80u;} std::vector<uint8_t> h(const char*m,size_t n){std::vector<uint8_t>o(m,m+4);p(o,1,2);p(o,n,2);p(o,n,4);p(o,0,4);return o;} bool vh(const uint8_t*b,size_t n,const char*m,size_t z){return b&&n==z&&std::equal(b,b+4,m)&&g(b+4,2)==1&&g(b+6,2)==z&&g(b+8,4)==z&&std::all_of(b+12,b+16,[](auto v){return v==0;});} DdlCreateProcedureSha sha(const char*d,const uint8_t*b,size_t n){std::vector<uint8_t>x(d,d+strlen(d));x.insert(x.end(),b,b+n);return scratchbird::core::hash::ComputeSha256Digest(x).digest;} }
std::vector<uint8_t> EncodeSblrDdlCreateProcedureRequestV1(const SblrDdlCreateProcedureRequestV1&v){if(!nz(v.receipt)||!v.occurrence||!v.procedure_occurrence)return{};auto o=h("PCQX",64);o.insert(o.end(),v.receipt.begin(),v.receipt.end());p(o,v.occurrence,8);p(o,v.procedure_occurrence,4);o.insert(o.end(),20,0);return o;}
bool DecodeSblrDdlCreateProcedureRequestV1(const uint8_t*b,size_t n,SblrDdlCreateProcedureRequestV1*out,std::string*d){if(!out||!vh(b,n,"PCQX",64)||std::any_of(b+44,b+64,[](auto v){return v;})){if(d)*d="PCQX invalid";return false;}SblrDdlCreateProcedureRequestV1 v;std::copy_n(b+16,16,v.receipt.begin());v.occurrence=g(b+32,8);v.procedure_occurrence=g(b+40,4);if(EncodeSblrDdlCreateProcedureRequestV1(v).empty())return false;*out=v;return true;}

namespace {
bool valid_utf8(std::string_view value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = bytes[index];
    if (lead == 0) return false;
    if (lead < 0x80) { ++index; continue; }
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xe0u) == 0xc0u) {
      continuation = 1; codepoint = lead & 0x1fu;
      if (codepoint < 2) return false;
    } else if ((lead & 0xf0u) == 0xe0u) {
      continuation = 2; codepoint = lead & 0x0fu;
    } else if ((lead & 0xf8u) == 0xf0u) {
      continuation = 3; codepoint = lead & 0x07u;
    } else {
      return false;
    }
    if (index + continuation >= value.size()) return false;
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto byte = bytes[index + offset];
      if ((byte & 0xc0u) != 0x80u) return false;
      codepoint = (codepoint << 6) | (byte & 0x3fu);
    }
    if ((continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    index += continuation + 1;
  }
  return !value.empty();
}

bool valid_atom(const SblrDdlCreateProcedureNameAtomV2& atom) {
  return atom.raw_utf8.size() >= 1 && atom.raw_utf8.size() <= 256 &&
         atom.raw_utf8.find('.') == std::string::npos &&
         valid_utf8(atom.raw_utf8);
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureBindRequestV2(
    const SblrDdlCreateProcedureBindRequestV2& value) {
  if (!nz(value.receipt) || value.occurrence == 0 ||
      value.procedure_occurrence == 0 || value.command_identity != 1 ||
      value.body_profile != 1 || value.name_atoms.empty() ||
      value.name_atoms.size() > 3 ||
      !std::all_of(value.name_atoms.begin(), value.name_atoms.end(),
                   valid_atom)) return {};
  std::size_t total = 84;
  for (const auto& atom : value.name_atoms) total += 4 + atom.raw_utf8.size();
  if (total > 1024) return {};
  std::vector<std::uint8_t> out{'P','C','Q','X'};
  p(out, 2, 2); p(out, total, 2); p(out, total, 4); p(out, 0, 4);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  p(out, value.occurrence, 8); p(out, value.procedure_occurrence, 4);
  p(out, value.command_identity, 2); p(out, value.body_profile, 2);
  p(out, value.name_atoms.size(), 1); p(out, 0, 1); p(out, 0, 1); p(out, 0, 1);
  for (const auto& atom : value.name_atoms) {
    p(out, atom.raw_utf8.size(), 2); p(out, atom.quoted ? 1 : 0, 1); p(out, 0, 1);
    out.insert(out.end(), atom.raw_utf8.begin(), atom.raw_utf8.end());
  }
  const auto evidence = sha("ScratchBird.SblrDdlCreateProcedureBindRequest.V2",
                            out.data() + 16, out.size() - 16);
  if (nz(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  return out.size() == total ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateProcedureBindRequestV2(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateProcedureBindRequestV2* out, std::string* detail) {
  const auto refuse = [&](const char* reason) {
    if (detail) *detail = reason;
    return false;
  };
  if (!out || !bytes || size < 89 || size > 1024 ||
      !std::equal(bytes, bytes + 4, "PCQX") || g(bytes + 4, 2) != 2 ||
      g(bytes + 6, 2) != size || g(bytes + 8, 4) != size ||
      std::any_of(bytes + 12, bytes + 16, [](auto v){return v != 0;}))
    return refuse("PCQX v2 header invalid");
  SblrDdlCreateProcedureBindRequestV2 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = g(bytes + 32, 8);
  value.procedure_occurrence = g(bytes + 40, 4);
  value.command_identity = static_cast<std::uint16_t>(g(bytes + 44, 2));
  value.body_profile = static_cast<std::uint16_t>(g(bytes + 46, 2));
  const auto atom_count = g(bytes + 48, 1);
  if (atom_count < 1 || atom_count > 3 ||
      std::any_of(bytes + 49, bytes + 52, [](auto v){return v != 0;}))
    return refuse("PCQX v2 fixed shape invalid");
  const std::size_t evidence_offset = size - 32;
  std::size_t offset = 52;
  for (std::size_t index = 0; index < atom_count; ++index) {
    if (offset + 4 > evidence_offset) return refuse("PCQX v2 atom truncated");
    const auto length = g(bytes + offset, 2);
    const auto quoted = bytes[offset + 2];
    if (length < 1 || length > 256 || quoted > 1 || bytes[offset + 3] != 0 ||
        offset + 4 + length > evidence_offset)
      return refuse("PCQX v2 atom invalid");
    SblrDdlCreateProcedureNameAtomV2 atom;
    atom.raw_utf8.assign(reinterpret_cast<const char*>(bytes + offset + 4),
                         static_cast<std::size_t>(length));
    atom.quoted = quoted == 1;
    if (!valid_atom(atom)) return refuse("PCQX v2 atom text invalid");
    value.name_atoms.push_back(std::move(atom));
    offset += 4 + length;
  }
  if (offset != evidence_offset) return refuse("PCQX v2 trailing bytes invalid");
  std::copy_n(bytes + evidence_offset, 32, value.evidence.begin());
  const auto expected = sha("ScratchBird.SblrDdlCreateProcedureBindRequest.V2",
                            bytes + 16, evidence_offset - 16);
  if (value.evidence != expected) return refuse("PCQX v2 evidence invalid");
  const auto canonical = EncodeSblrDdlCreateProcedureBindRequestV2(value);
  if (canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), bytes))
    return refuse("PCQX v2 canonical re-encoding differs");
  *out = std::move(value);
  return true;
}
std::vector<uint8_t> EncodeSblrDdlCreateProcedureDescriptorV1(const SblrDdlCreateProcedureDescriptorV1&v,bool op){if(!nz(v.body)||!v.availability)return{};auto o=h(op?"PCDO":"PCDX",488);o.insert(o.end(),v.body.begin(),v.body.end());auto e=sha("ScratchBird.SblrDdlCreateProcedureDescriptor.V1",o.data()+16,400);if(nz(v.evidence)&&e!=v.evidence)return{};o.insert(o.end(),e.begin(),e.end());p(o,v.availability,8);o.insert(o.end(),32,0);return o;}
bool DecodeSblrDdlCreateProcedureDescriptorV1(const uint8_t*b,size_t n,SblrDdlCreateProcedureDescriptorV1*out,std::string*d,bool op){if(!out||!vh(b,n,op?"PCDO":"PCDX",488)||std::any_of(b+456,b+488,[](auto v){return v;})){if(d)*d="PCDO invalid";return false;}SblrDdlCreateProcedureDescriptorV1 v;std::copy_n(b+16,400,v.body.begin());std::copy_n(b+416,32,v.evidence.begin());v.availability=g(b+448,8);if(EncodeSblrDdlCreateProcedureDescriptorV1(v,op).empty())return false;*out=v;return true;}
bool ValidateSblrDdlCreateProcedureAuthorityV1(
    const SblrDdlCreateProcedureAuthorityV1& value, std::string* detail) {
  const bool valid =
      nz(value.receipt) && value.occurrence != 0 &&
      value.procedure_occurrence != 0 && value.command_identity == 1 &&
      value.body_profile == 1 && uuid_v7(value.procedure_uuid) &&
      value.procedure_generation != 0 && nz(value.schema_uuid) &&
      value.schema_generation != 0 && nz(value.owning_transaction_uuid) &&
      value.owning_local_transaction_id != 0 &&
      nz(value.statement_snapshot_uuid) && nz(value.catalog_epoch_uuid) &&
      value.catalog_generation != 0 && nz(value.security_context_uuid) &&
      value.security_epoch != 0 && nz(value.policy_snapshot_uuid) &&
      value.policy_generation != 0 && nz(value.resource_grant_uuid) &&
      value.resource_generation != 0 && nz(value.owner_principal_uuid) &&
      uuid_v7(value.body_sblr_uuid) && value.body_sblr_generation != 0 &&
      nz(value.body_sblr_sha256) && uuid_v7(value.procedure_abi_uuid) &&
      value.procedure_abi_generation != 0 && nz(value.effect_set_sha256) &&
      uuid_v7(value.recovery_uuid) && value.recovery_generation != 0 &&
      nz(value.request_evidence_sha256) &&
      value.executor_availability_generation != 0 &&
      value.procedure_uuid != value.body_sblr_uuid &&
      value.procedure_uuid != value.procedure_abi_uuid &&
      value.procedure_uuid != value.recovery_uuid &&
      value.body_sblr_uuid != value.procedure_abi_uuid &&
      value.body_sblr_uuid != value.recovery_uuid &&
      value.procedure_abi_uuid != value.recovery_uuid;
  if (!valid && detail != nullptr) *detail = "PCDO authority fields are invalid";
  return valid;
}
bool DecodeSblrDdlCreateProcedureAuthorityV1(
    const SblrDdlCreateProcedureDescriptorV1& descriptor,
    SblrDdlCreateProcedureAuthorityV1* out, std::string* detail) {
  if (out == nullptr) return false;
  SblrDdlCreateProcedureAuthorityV1 value;
  const auto* body = descriptor.body.data();
  std::copy_n(body + 0, 16, value.receipt.begin());
  value.occurrence = g(body + 16, 8);
  value.procedure_occurrence = static_cast<std::uint32_t>(g(body + 24, 4));
  value.command_identity = static_cast<std::uint16_t>(g(body + 28, 2));
  value.body_profile = static_cast<std::uint16_t>(g(body + 30, 2));
  std::copy_n(body + 32, 16, value.procedure_uuid.begin());
  value.procedure_generation = g(body + 48, 8);
  std::copy_n(body + 56, 16, value.schema_uuid.begin());
  value.schema_generation = g(body + 72, 8);
  std::copy_n(body + 80, 16, value.owning_transaction_uuid.begin());
  value.owning_local_transaction_id = g(body + 96, 8);
  std::copy_n(body + 104, 16, value.statement_snapshot_uuid.begin());
  std::copy_n(body + 120, 16, value.catalog_epoch_uuid.begin());
  value.catalog_generation = g(body + 136, 8);
  std::copy_n(body + 144, 16, value.security_context_uuid.begin());
  value.security_epoch = g(body + 160, 8);
  std::copy_n(body + 168, 16, value.policy_snapshot_uuid.begin());
  value.policy_generation = g(body + 184, 8);
  std::copy_n(body + 192, 16, value.resource_grant_uuid.begin());
  value.resource_generation = g(body + 208, 8);
  std::copy_n(body + 216, 16, value.owner_principal_uuid.begin());
  std::copy_n(body + 232, 16, value.body_sblr_uuid.begin());
  value.body_sblr_generation = g(body + 248, 8);
  std::copy_n(body + 256, 32, value.body_sblr_sha256.begin());
  std::copy_n(body + 288, 16, value.procedure_abi_uuid.begin());
  value.procedure_abi_generation = g(body + 304, 8);
  std::copy_n(body + 312, 32, value.effect_set_sha256.begin());
  std::copy_n(body + 344, 16, value.recovery_uuid.begin());
  value.recovery_generation = g(body + 360, 8);
  std::copy_n(body + 368, 32, value.request_evidence_sha256.begin());
  value.executor_availability_generation = descriptor.availability;
  if (!ValidateSblrDdlCreateProcedureAuthorityV1(value, detail)) return false;
  *out = value;
  return true;
}
std::vector<uint8_t> EncodeSblrDdlCreateProcedureResultV1(const SblrDdlCreateProcedureResultV1&v){if(!nz(v.body)||v.body[24]!=1||!g(v.body.data()+56,8)||!v.availability||!nz(v.publication_barrier))return{};auto o=h("PCRS",320);o.insert(o.end(),v.body.begin(),v.body.end());auto e=sha("ScratchBird.SblrDdlCreateProcedureExecutorEvidence.V1",o.data()+16,240);if(nz(v.evidence)&&e!=v.evidence)return{};o.insert(o.end(),e.begin(),e.end());p(o,v.availability,8);o.insert(o.end(),v.publication_barrier.begin(),v.publication_barrier.end());o.insert(o.end(),8,0);return o;}
bool DecodeSblrDdlCreateProcedureResultV1(const uint8_t*b,size_t n,SblrDdlCreateProcedureResultV1*out,std::string*d){if(!out||!vh(b,n,"PCRS",320)||std::any_of(b+312,b+320,[](auto v){return v;})){if(d)*d="PCRS invalid";return false;}SblrDdlCreateProcedureResultV1 v;std::copy_n(b+16,240,v.body.begin());std::copy_n(b+256,32,v.evidence.begin());v.availability=g(b+288,8);std::copy_n(b+296,16,v.publication_barrier.begin());if(EncodeSblrDdlCreateProcedureResultV1(v).empty())return false;*out=v;return true;}
}
