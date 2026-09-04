// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_literal_runtime.hpp"
#include "hash_digest.hpp"
#include "sbl_numeric.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

std::optional<std::array<std::uint8_t,32>>
ComputeSblrLiteralExecutorEvidenceSha256V1(
    const SblrLiteralExecutorEvidenceV1& evidence) {
  if (evidence.executor_id != "engine.op.literal" ||
      evidence.opcode_code != 3 || evidence.opcode_version != "1.0" ||
      evidence.operand_descriptor_id != "typed_literal" ||
      !std::any_of(evidence.descriptor_uuid.begin(),evidence.descriptor_uuid.end(),[](auto byte){return byte!=0;}) || evidence.descriptor_generation == 0 ||
      evidence.result_descriptor_id != "typed_value" ||
      evidence.result_descriptor_version != 1) return std::nullopt;
  static constexpr std::string_view domain =
      "ScratchBird.SblrLiteralExecutorEvidence.V1";
  std::vector<std::uint8_t> bytes(domain.begin(),domain.end());
  const auto u16=[&](std::uint16_t value){bytes.push_back(value);bytes.push_back(value>>8);};
  const auto u64=[&](std::uint64_t value){for(unsigned i=0;i<8;++i)bytes.push_back(value>>(8*i));};
  const auto text=[&](std::string_view value){u16(static_cast<std::uint16_t>(value.size()));bytes.insert(bytes.end(),value.begin(),value.end());};
  text(evidence.executor_id);u16(evidence.opcode_code);
  text(evidence.opcode_version);text(evidence.operand_descriptor_id);
  bytes.insert(bytes.end(),evidence.descriptor_uuid.begin(),evidence.descriptor_uuid.end());
  u64(evidence.descriptor_generation);
  bytes.insert(bytes.end(),evidence.canonical_value_sha256.begin(),evidence.canonical_value_sha256.end());
  text(evidence.result_descriptor_id);u16(evidence.result_descriptor_version);
  const auto digest=core::hash::ComputeSha256Digest(bytes);
  return digest.ok()?std::optional{digest.digest}:std::nullopt;
}
namespace {
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kRecordFixedBytes = 125;
constexpr std::size_t kMaximumLiteralBytes = 65536;
constexpr std::uint32_t kMaximumNodes = 4096;
constexpr std::size_t kMaximumBoundAstBytes = 491592;
constexpr std::size_t kMaximumExpressionNodeTableBytes = 610336;
constexpr std::size_t kMaximumFinalizeRequestBytes = 1102136;

std::uint16_t U16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) |
         (static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint32_t U32(const std::uint8_t* p) {
  std::uint32_t v = 0; for (unsigned i = 0; i != 4; ++i) v |= std::uint32_t(p[i]) << (8*i); return v;
}
std::uint64_t U64(const std::uint8_t* p) {
  std::uint64_t v = 0; for (unsigned i = 0; i != 8; ++i) v |= std::uint64_t(p[i]) << (8*i); return v;
}
void Put16(std::vector<std::uint8_t>* out, std::uint16_t v) {
  out->push_back(v); out->push_back(v >> 8);
}
void Put32(std::vector<std::uint8_t>* out, std::uint32_t v) {
  for (unsigned i = 0; i != 4; ++i) out->push_back(v >> (8*i));
}
void Put64(std::vector<std::uint8_t>* out, std::uint64_t v) {
  for (unsigned i = 0; i != 8; ++i) out->push_back(v >> (8*i));
}
void PutText(std::vector<std::uint8_t>* out, std::string_view text) {
  Put16(out, static_cast<std::uint16_t>(text.size()));
  out->insert(out->end(), text.begin(), text.end());
}
bool Nonzero(const std::array<std::uint8_t,16>& u) {
  return std::any_of(u.begin(), u.end(), [](auto v){ return v != 0; });
}
SblrExpressionNodeTableCodecResultV1 Fail(std::string detail) {
  SblrExpressionNodeTableCodecResultV1 r; r.diagnostic_id="SBLR.OPERAND_INVALID"; r.detail=std::move(detail); return r;
}
bool TakeText(const std::uint8_t* bytes, std::size_t end, std::size_t* offset,
              std::string_view expected) {
  if (*offset > end || end - *offset < 2) return false;
  const auto length = U16(bytes + *offset); *offset += 2;
  if (length != expected.size() || length > end - *offset ||
      !std::equal(expected.begin(), expected.end(), bytes + *offset)) return false;
  *offset += length; return true;
}
bool Nonzero32(const std::array<std::uint8_t,32>& bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](auto value) {
    return value != 0;
  });
}

SblrLiteralExactDecimalCodecResultV1 DecimalFailure(
    const bool overflow, std::string detail) {
  SblrLiteralExactDecimalCodecResultV1 result;
  result.diagnostic_id =
      overflow ? "DATATYPE.DESCRIPTOR_INVALID" : "SBLR.OPERAND_INVALID";
  result.detail = std::move(detail);
  return result;
}

bool EndsWithAsciiInsensitive(const std::string_view value,
                              const std::string_view suffix) {
  if (suffix.size() > value.size()) return false;
  const auto offset = value.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    const auto left = static_cast<unsigned char>(value[offset + index]);
    const auto right = static_cast<unsigned char>(suffix[index]);
    if (std::toupper(left) != std::toupper(right)) return false;
  }
  return true;
}

struct DecimalLexicalPartsV1 {
  std::string numeric;
  bool overflow = false;
};

std::optional<DecimalLexicalPartsV1> ValidateDecimalLexicalV1(
    std::string_view lexical) {
  if (lexical.empty() || lexical.size() > 128) return std::nullopt;
  for (const auto suffix : {std::string_view{"DECIMAL"},
                            std::string_view{"DEC"},
                            std::string_view{"D"}}) {
    if (lexical.size() > suffix.size() &&
        EndsWithAsciiInsensitive(lexical, suffix)) {
      lexical.remove_suffix(suffix.size());
      break;
    }
  }
  if (lexical.empty()) return std::nullopt;

  DecimalLexicalPartsV1 parts;
  parts.numeric.reserve(lexical.size());
  std::size_t cursor = 0;
  if (lexical[cursor] == '+' || lexical[cursor] == '-') {
    parts.numeric.push_back(lexical[cursor++]);
    if (cursor == lexical.size()) return std::nullopt;
  }
  const auto consume_digits = [&](std::size_t* position,
                                  std::string* destination,
                                  std::uint32_t* bounded_value) {
    bool saw_digit = false;
    bool prior_digit = false;
    while (*position < lexical.size()) {
      const char byte = lexical[*position];
      if (byte >= '0' && byte <= '9') {
        saw_digit = true;
        prior_digit = true;
        destination->push_back(byte);
        if (bounded_value != nullptr) {
          if (*bounded_value > 38U / 10U ||
              (*bounded_value == 38U / 10U &&
               static_cast<std::uint32_t>(byte - '0') > 38U % 10U)) {
            parts.overflow = true;
          } else {
            *bounded_value = (*bounded_value * 10U) +
                             static_cast<std::uint32_t>(byte - '0');
          }
        }
        ++*position;
        continue;
      }
      if (byte == '_') {
        if (!prior_digit || *position + 1 >= lexical.size() ||
            lexical[*position + 1] < '0' || lexical[*position + 1] > '9') {
          return false;
        }
        prior_digit = false;
        ++*position;
        continue;
      }
      break;
    }
    return saw_digit && prior_digit;
  };

  if (!consume_digits(&cursor, &parts.numeric, nullptr)) return std::nullopt;
  if (cursor < lexical.size() && lexical[cursor] == '.') {
    parts.numeric.push_back(lexical[cursor++]);
    if (!consume_digits(&cursor, &parts.numeric, nullptr)) return std::nullopt;
  }
  if (cursor < lexical.size() &&
      (lexical[cursor] == 'e' || lexical[cursor] == 'E')) {
    parts.numeric.push_back('e');
    ++cursor;
    if (cursor < lexical.size() &&
        (lexical[cursor] == '+' || lexical[cursor] == '-')) {
      parts.numeric.push_back(lexical[cursor++]);
    }
    std::uint32_t exponent = 0;
    if (!consume_digits(&cursor, &parts.numeric, &exponent)) {
      return std::nullopt;
    }
    if (exponent > 38) parts.overflow = true;
  }
  if (cursor != lexical.size()) return std::nullopt;
  return parts;
}

std::string RenderExactDecimalV1(const bool negative,
                                 const std::string_view coefficient,
                                 const std::uint8_t scale) {
  if (coefficient == "0") return "0";
  std::string rendered;
  if (negative) rendered.push_back('-');
  if (scale == 0) {
    rendered.append(coefficient);
    return rendered;
  }
  if (coefficient.size() <= scale) {
    rendered.append("0.");
    rendered.append(scale - coefficient.size(), '0');
    rendered.append(coefficient);
    return rendered;
  }
  const auto integer_bytes = coefficient.size() - scale;
  rendered.append(coefficient.substr(0, integer_bytes));
  rendered.push_back('.');
  rendered.append(coefficient.substr(integer_bytes));
  return rendered;
}
}  // namespace

std::array<std::uint8_t, 32> ComputeSblrLiteralDescriptorProfileBindingV1(
    const SblrLiteralStatementDescriptorProfileV1& profile,
    std::uint64_t receipt_security_epoch,
    std::uint64_t receipt_resource_epoch) {
  static constexpr std::string_view domain =
      "ScratchBird.SblrLiteralStatementDescriptorProfile.V1";
  std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
  Put16(&bytes, 1);
  bytes.insert(bytes.end(), profile.profile_uuid.begin(), profile.profile_uuid.end());
  bytes.insert(bytes.end(), profile.statement_receipt_uuid.begin(), profile.statement_receipt_uuid.end());
  bytes.insert(bytes.end(), profile.catalog_snapshot_uuid.begin(), profile.catalog_snapshot_uuid.end());
  Put64(&bytes, profile.catalog_generation);
  bytes.insert(bytes.end(), profile.descriptor_uuid.begin(), profile.descriptor_uuid.end());
  Put64(&bytes, profile.descriptor_generation);
  bytes.insert(bytes.end(), profile.type_uuid.begin(), profile.type_uuid.end());
  PutText(&bytes, profile.codec_id);
  Put16(&bytes, profile.codec_version);
  Put64(&bytes, profile.codec_generation);
  bytes.push_back(profile.nullable ? 1 : 0);
  Put64(&bytes, receipt_security_epoch);
  Put64(&bytes, receipt_resource_epoch);
  const auto digest = core::hash::ComputeSha256Digest(bytes);
  return digest.ok() ? digest.digest : std::array<std::uint8_t, 32>{};
}

std::array<std::uint8_t,32> ComputeSblrLiteralDemandSequenceSha256V1(
    const std::vector<SblrLiteralDemandV1>& demands) {
  static constexpr std::string_view domain=
      "ScratchBird.SblrLiteralDemandSequence.V1";
  std::vector<std::uint8_t> bytes(domain.begin(),domain.end());
  if(demands.size()>kMaximumNodes) return {};
  Put32(&bytes,static_cast<std::uint32_t>(demands.size()));
  std::uint64_t prior=0;
  for(const auto& demand:demands){
    if(demand.occurrence_id==0||demand.occurrence_id<=prior||
       demand.lexical_class==0||demand.context_class==0) return {};
    Put64(&bytes,demand.occurrence_id);Put16(&bytes,demand.lexical_class);
    Put16(&bytes,demand.context_class);bytes.push_back(demand.nullable?1:0);
    bytes.insert(bytes.end(),3,0);
    bytes.insert(bytes.end(),demand.lexical_sha256.begin(),demand.lexical_sha256.end());
    prior=demand.occurrence_id;
  }
  const auto digest=core::hash::ComputeSha256Digest(bytes);
  return digest.ok()?digest.digest:std::array<std::uint8_t,32>{};
}

std::vector<std::uint8_t> EncodeSblrLiteralPrebindRequestV1(
    const SblrLiteralPrebindRequestV1& request){
  if(!Nonzero(request.preliminary_receipt_uuid)||!Nonzero(request.catalog_snapshot_uuid)||
     request.catalog_generation==0||
     !Nonzero(request.mga_snapshot_uuid)||request.demands.size()>kMaximumNodes||
     request.demands.size()>(std::numeric_limits<std::size_t>::max()-128)/48)return{};
  const auto hash=ComputeSblrLiteralDemandSequenceSha256V1(request.demands);
  if(hash!=request.demand_sha256)return{};
  std::vector<std::uint8_t> out;out.reserve(128+request.demands.size()*48);
  out.insert(out.end(),{'S','B','L','N'});Put16(&out,1);Put16(&out,128);
  Put32(&out,static_cast<std::uint32_t>(128+request.demands.size()*48));Put32(&out,0);
  out.insert(out.end(),request.preliminary_receipt_uuid.begin(),request.preliminary_receipt_uuid.end());
  out.insert(out.end(),request.catalog_snapshot_uuid.begin(),request.catalog_snapshot_uuid.end());
  Put64(&out,request.catalog_generation);Put64(&out,request.security_epoch);Put64(&out,request.resource_epoch);
  out.insert(out.end(),request.mga_snapshot_uuid.begin(),request.mga_snapshot_uuid.end());
  Put32(&out,static_cast<std::uint32_t>(request.demands.size()));Put32(&out,48);
  out.insert(out.end(),hash.begin(),hash.end());
  for(const auto& d:request.demands){Put64(&out,d.occurrence_id);Put16(&out,d.lexical_class);Put16(&out,d.context_class);out.push_back(d.nullable?1:0);out.insert(out.end(),3,0);out.insert(out.end(),d.lexical_sha256.begin(),d.lexical_sha256.end());}
  return out;
}

SblrLiteralPrebindRequestCodecResultV1 DecodeSblrLiteralPrebindRequestV1(
    const std::uint8_t* bytes,std::size_t size){
  SblrLiteralPrebindRequestCodecResultV1 r;r.diagnostic_id="SBLR.OPERAND_INVALID";
  const auto fail=[&](std::string d){r.detail=std::move(d);return r;};
  if(bytes==nullptr||size<128||!std::equal(bytes,bytes+4,reinterpret_cast<const std::uint8_t*>("SBLN"))||U16(bytes+4)!=1||U16(bytes+6)!=128||U32(bytes+8)!=size||U32(bytes+12)!=0||U32(bytes+92)!=48)return fail("SBLN header is noncanonical");
  const auto count=U32(bytes+88);if(count>kMaximumNodes||count>(size-128)/48||128+std::size_t(count)*48!=size)return fail("SBLN extent is invalid");
  auto& q=r.request;std::copy_n(bytes+16,16,q.preliminary_receipt_uuid.begin());std::copy_n(bytes+32,16,q.catalog_snapshot_uuid.begin());q.catalog_generation=U64(bytes+48);q.security_epoch=U64(bytes+56);q.resource_epoch=U64(bytes+64);std::copy_n(bytes+72,16,q.mga_snapshot_uuid.begin());std::copy_n(bytes+96,32,q.demand_sha256.begin());
  q.demands.reserve(count);std::size_t off=128;std::uint64_t prior=0;
  for(std::uint32_t i=0;i<count;++i,off+=48){SblrLiteralDemandV1 d;d.occurrence_id=U64(bytes+off);d.lexical_class=U16(bytes+off+8);d.context_class=U16(bytes+off+10);if(d.occurrence_id==0||d.occurrence_id<=prior||bytes[off+12]>1||bytes[off+13]||bytes[off+14]||bytes[off+15])return fail("SBLN demand is noncanonical");d.nullable=bytes[off+12]!=0;std::copy_n(bytes+off+16,32,d.lexical_sha256.begin());q.demands.push_back(d);prior=d.occurrence_id;}
  if(!Nonzero(q.preliminary_receipt_uuid)||!Nonzero(q.catalog_snapshot_uuid)||q.catalog_generation==0||!Nonzero(q.mga_snapshot_uuid)||ComputeSblrLiteralDemandSequenceSha256V1(q.demands)!=q.demand_sha256)return fail("SBLN binding is invalid");
  r.canonical_bytes=EncodeSblrLiteralPrebindRequestV1(q);if(r.canonical_bytes.size()!=size||!std::equal(r.canonical_bytes.begin(),r.canonical_bytes.end(),bytes))return fail("SBLN decode/re-encode differs");r.ok=true;return r;
}

std::array<std::uint8_t,32> ComputeSblrLiteralOrderedProfilesSha256V1(
    const std::vector<SblrLiteralProfileMappingV1>& mappings){
  static constexpr std::string_view domain=
      "ScratchBird.SblrLiteralOrderedProfiles.V1";
  if(mappings.size()>kMaximumNodes)return{};
  std::vector<std::uint8_t> bytes(domain.begin(),domain.end());
  Put32(&bytes,static_cast<std::uint32_t>(mappings.size()));
  std::uint64_t prior=0;
  for(const auto& mapping:mappings){
    if(mapping.occurrence_id==0||mapping.occurrence_id<=prior||
       mapping.sblp_bytes.size()>std::numeric_limits<std::uint32_t>::max())return{};
    const auto profile=DecodeSblrLiteralDescriptorProfileV1(
        mapping.sblp_bytes.data(),mapping.sblp_bytes.size());
    if(!profile.ok)return{};
    Put64(&bytes,mapping.occurrence_id);
    Put32(&bytes,static_cast<std::uint32_t>(mapping.sblp_bytes.size()));
    bytes.insert(bytes.end(),mapping.sblp_bytes.begin(),mapping.sblp_bytes.end());
    prior=mapping.occurrence_id;
  }
  const auto digest=core::hash::ComputeSha256Digest(bytes);
  return digest.ok()?digest.digest:std::array<std::uint8_t,32>{};
}

std::vector<std::uint8_t> EncodeSblrLiteralPrebindResultV1(
    const SblrLiteralPrebindResultV1& value){
  if(!Nonzero(value.preliminary_receipt_uuid)||!Nonzero(value.catalog_snapshot_uuid)||
     value.catalog_generation==0||!Nonzero(value.mga_snapshot_uuid)||
     value.mappings.size()>kMaximumNodes||
     ComputeSblrLiteralOrderedProfilesSha256V1(value.mappings)!=value.ordered_profile_sha256)return{};
  std::size_t suffix=0;
  for(const auto& mapping:value.mappings){
    if(mapping.sblp_bytes.size()>std::numeric_limits<std::uint32_t>::max()||
       mapping.sblp_bytes.size()>std::numeric_limits<std::size_t>::max()-12||
       12+mapping.sblp_bytes.size()>std::numeric_limits<std::size_t>::max()-suffix)return{};
    suffix+=12+mapping.sblp_bytes.size();
  }
  if(suffix>std::numeric_limits<std::uint32_t>::max()||suffix>std::numeric_limits<std::size_t>::max()-160||160+suffix>std::numeric_limits<std::uint32_t>::max())return{};
  std::vector<std::uint8_t> out;out.reserve(160+suffix);
  out.insert(out.end(),{'S','B','L','Q'});Put16(&out,1);Put16(&out,160);Put32(&out,static_cast<std::uint32_t>(160+suffix));Put32(&out,0);
  out.insert(out.end(),value.preliminary_receipt_uuid.begin(),value.preliminary_receipt_uuid.end());
  out.insert(out.end(),value.catalog_snapshot_uuid.begin(),value.catalog_snapshot_uuid.end());
  Put64(&out,value.catalog_generation);Put64(&out,value.security_epoch);Put64(&out,value.resource_epoch);
  out.insert(out.end(),value.mga_snapshot_uuid.begin(),value.mga_snapshot_uuid.end());
  out.insert(out.end(),value.demand_sha256.begin(),value.demand_sha256.end());
  Put32(&out,static_cast<std::uint32_t>(value.mappings.size()));Put32(&out,static_cast<std::uint32_t>(suffix));
  out.insert(out.end(),value.ordered_profile_sha256.begin(),value.ordered_profile_sha256.end());
  for(const auto& mapping:value.mappings){Put64(&out,mapping.occurrence_id);Put32(&out,static_cast<std::uint32_t>(mapping.sblp_bytes.size()));out.insert(out.end(),mapping.sblp_bytes.begin(),mapping.sblp_bytes.end());}
  return out;
}

bool DecodeSblrLiteralFinalizeRequestV1(const std::uint8_t* b,std::size_t n,
                                        SblrLiteralFinalizeRequestV1* out){
  if(b==nullptr||out==nullptr||n<208||n>kMaximumFinalizeRequestBytes||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBLF"))||U16(b+4)!=1||U16(b+6)!=208||U32(b+8)!=n||U32(b+12)!=0)return false;
  SblrLiteralFinalizeRequestV1 v;std::copy_n(b+16,16,v.preliminary_receipt_uuid.begin());
  std::copy_n(b+32,32,v.demand_sha256.begin());std::copy_n(b+64,32,v.ordered_profile_sha256.begin());std::copy_n(b+96,32,v.bound_ast_sha256.begin());std::copy_n(b+128,32,v.sbxn_sha256.begin());
  v.catalog_generation=U64(b+160);v.security_epoch=U64(b+168);v.resource_epoch=U64(b+176);std::copy_n(b+184,16,v.mga_snapshot_uuid.begin());
  const auto sbba_bytes=U32(b+200),sbxn_bytes=U32(b+204);if(sbba_bytes>kMaximumBoundAstBytes||sbxn_bytes>kMaximumExpressionNodeTableBytes||sbba_bytes>n-208||sbxn_bytes>n-208-sbba_bytes||208+std::size_t(sbba_bytes)+std::size_t(sbxn_bytes)!=n)return false;
  if(!Nonzero(v.preliminary_receipt_uuid)||v.catalog_generation==0||!Nonzero(v.mga_snapshot_uuid))return false;v.canonical_sbba.assign(b+208,b+208+sbba_bytes);v.canonical_sbxn.assign(b+208+sbba_bytes,b+n);SblrLiteralBoundAstV1 parsed;if(!DecodeSblrLiteralBoundAstV1(v.canonical_sbba.data(),v.canonical_sbba.size(),&parsed)||ComputeSblrLiteralBoundAstSha256V1(v.canonical_sbba)!=v.bound_ast_sha256)return false;if(v.canonical_sbxn.empty()){if(!parsed.nodes.empty()||std::any_of(v.sbxn_sha256.begin(),v.sbxn_sha256.end(),[](auto x){return x!=0;}))return false;}else{const auto table=DecodeSblrExpressionNodeTableV1(v.canonical_sbxn.data(),v.canonical_sbxn.size());const auto digest=core::hash::ComputeSha256Digest(v.canonical_sbxn);if(!table.ok||!digest.ok()||digest.digest!=v.sbxn_sha256)return false;}*out=std::move(v);return true;
}

std::vector<std::uint8_t> EncodeSblrLiteralAdmissionV1(SblrLiteralAdmissionV1* a){
  if(a==nullptr||!Nonzero(a->preliminary_receipt_uuid)||!Nonzero(a->final_receipt_uuid)||!Nonzero(a->admission_token_uuid)||a->catalog_generation==0||!Nonzero(a->mga_snapshot_uuid))return{};
  std::vector<std::uint8_t> out;out.reserve(264);out.insert(out.end(),{'S','B','L','A'});Put16(&out,1);Put16(&out,264);Put32(&out,264);Put32(&out,0);
  out.insert(out.end(),a->preliminary_receipt_uuid.begin(),a->preliminary_receipt_uuid.end());out.insert(out.end(),a->final_receipt_uuid.begin(),a->final_receipt_uuid.end());out.insert(out.end(),a->admission_token_uuid.begin(),a->admission_token_uuid.end());
  out.insert(out.end(),a->demand_sha256.begin(),a->demand_sha256.end());out.insert(out.end(),a->ordered_profile_sha256.begin(),a->ordered_profile_sha256.end());out.insert(out.end(),a->bound_ast_sha256.begin(),a->bound_ast_sha256.end());out.insert(out.end(),a->sbxn_sha256.begin(),a->sbxn_sha256.end());
  Put64(&out,a->catalog_generation);Put64(&out,a->security_epoch);Put64(&out,a->resource_epoch);out.insert(out.end(),a->mga_snapshot_uuid.begin(),a->mga_snapshot_uuid.end());
  static constexpr std::string_view domain="ScratchBird.SblrLiteralAdmissionToken.V1";std::vector<std::uint8_t> binding(domain.begin(),domain.end());binding.insert(binding.end(),out.begin()+12,out.end());const auto digest=core::hash::ComputeSha256Digest(binding);if(!digest.ok())return{};a->admission_token_binding_sha256=digest.digest;out.insert(out.end(),digest.digest.begin(),digest.digest.end());return out;
}

std::vector<std::uint8_t> EncodeSblrLiteralBoundAstV1(const SblrLiteralBoundAstV1& v){
  if(!Nonzero(v.preliminary_receipt_uuid)||v.nodes.size()>kMaximumNodes||v.nodes.size()>(std::numeric_limits<std::size_t>::max()-72)/120)return{};
  std::vector<std::uint8_t> out;out.reserve(72+v.nodes.size()*120);out.insert(out.end(),{'S','B','B','A'});Put16(&out,1);Put16(&out,72);Put32(&out,static_cast<std::uint32_t>(72+v.nodes.size()*120));Put32(&out,0);Put32(&out,static_cast<std::uint32_t>(v.nodes.size()));Put32(&out,120);out.insert(out.end(),v.preliminary_receipt_uuid.begin(),v.preliminary_receipt_uuid.end());out.insert(out.end(),v.demand_sha256.begin(),v.demand_sha256.end());
  std::uint32_t prior_ordinal=0;std::uint64_t prior_node=0;
  for(const auto& n:v.nodes){if(n.parent_operand_ordinal==0||n.node_id==0||!Nonzero(n.descriptor_uuid)||n.descriptor_generation==0||!Nonzero(n.type_uuid)||!Nonzero(n.profile_uuid)||n.occurrence_id==0||(n.parent_operand_ordinal<prior_ordinal)||(n.parent_operand_ordinal==prior_ordinal&&n.node_id<=prior_node))return{};Put32(&out,120);Put32(&out,n.parent_operand_ordinal);Put64(&out,n.node_id);out.insert(out.end(),n.descriptor_uuid.begin(),n.descriptor_uuid.end());Put64(&out,n.descriptor_generation);out.insert(out.end(),n.type_uuid.begin(),n.type_uuid.end());out.insert(out.end(),n.profile_uuid.begin(),n.profile_uuid.end());Put64(&out,n.occurrence_id);out.insert(out.end(),n.lexical_sha256.begin(),n.lexical_sha256.end());out.push_back(n.nullable?1:0);out.insert(out.end(),7,0);prior_ordinal=n.parent_operand_ordinal;prior_node=n.node_id;}
  return out;
}
bool DecodeSblrLiteralBoundAstV1(const std::uint8_t* b,std::size_t n,SblrLiteralBoundAstV1* out){
  if(b==nullptr||out==nullptr||n<72||n>kMaximumBoundAstBytes||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBBA"))||U16(b+4)!=1||U16(b+6)!=72||U32(b+8)!=n||U32(b+12)!=0||U32(b+20)!=120)return false;const auto count=U32(b+16);if(count>kMaximumNodes||count>(n-72)/120||72+std::size_t(count)*120!=n)return false;SblrLiteralBoundAstV1 v;std::copy_n(b+24,16,v.preliminary_receipt_uuid.begin());std::copy_n(b+40,32,v.demand_sha256.begin());if(!Nonzero(v.preliminary_receipt_uuid))return false;v.nodes.reserve(count);std::size_t off=72;
  for(std::uint32_t i=0;i<count;++i,off+=120){if(U32(b+off)!=120||b[off+112]>1||std::any_of(b+off+113,b+off+120,[](auto x){return x!=0;}))return false;SblrLiteralBoundAstNodeV1 x;x.parent_operand_ordinal=U32(b+off+4);x.node_id=U64(b+off+8);std::copy_n(b+off+16,16,x.descriptor_uuid.begin());x.descriptor_generation=U64(b+off+32);std::copy_n(b+off+40,16,x.type_uuid.begin());std::copy_n(b+off+56,16,x.profile_uuid.begin());x.occurrence_id=U64(b+off+72);std::copy_n(b+off+80,32,x.lexical_sha256.begin());x.nullable=b[off+112]!=0;v.nodes.push_back(x);}const auto canonical=EncodeSblrLiteralBoundAstV1(v);if(canonical.size()!=n||!std::equal(canonical.begin(),canonical.end(),b))return false;*out=std::move(v);return true;
}
std::array<std::uint8_t,32> ComputeSblrLiteralBoundAstSha256V1(const std::vector<std::uint8_t>& bytes){static constexpr std::string_view domain="ScratchBird.SblrLiteralBoundAst.V1";SblrLiteralBoundAstV1 parsed;if(!DecodeSblrLiteralBoundAstV1(bytes.data(),bytes.size(),&parsed))return{};std::vector<std::uint8_t> input(domain.begin(),domain.end());input.insert(input.end(),bytes.begin(),bytes.end());const auto digest=core::hash::ComputeSha256Digest(input);return digest.ok()?digest.digest:std::array<std::uint8_t,32>{};}

std::vector<std::uint8_t> EncodeSblrLiteralDescriptorProfileV1(
    const SblrLiteralStatementDescriptorProfileV1& profile) {
  if (!Nonzero(profile.profile_uuid) || !Nonzero(profile.statement_receipt_uuid) ||
      !Nonzero(profile.catalog_snapshot_uuid) || profile.catalog_generation == 0 ||
      !Nonzero(profile.descriptor_uuid) || profile.descriptor_generation == 0 ||
      !Nonzero(profile.type_uuid) || profile.descriptor_uuid == profile.type_uuid ||
      profile.codec_id.empty() || profile.codec_id.size() > 65535 ||
      profile.codec_id.find('\0') != std::string::npos ||
      profile.codec_version == 0 || profile.codec_generation == 0) return {};
  const std::size_t total = 164 + profile.codec_id.size();
  if (total > std::numeric_limits<std::uint32_t>::max()) return {};
  std::vector<std::uint8_t> out; out.reserve(total);
  out.insert(out.end(), {'S','B','L','P'}); Put16(&out,1); Put16(&out,164);
  Put32(&out,static_cast<std::uint32_t>(total)); Put32(&out,0);
  out.insert(out.end(),profile.profile_uuid.begin(),profile.profile_uuid.end());
  out.insert(out.end(),profile.statement_receipt_uuid.begin(),profile.statement_receipt_uuid.end());
  out.insert(out.end(),profile.catalog_snapshot_uuid.begin(),profile.catalog_snapshot_uuid.end());
  Put64(&out,profile.catalog_generation);
  out.insert(out.end(),profile.descriptor_uuid.begin(),profile.descriptor_uuid.end());
  Put64(&out,profile.descriptor_generation);
  out.insert(out.end(),profile.type_uuid.begin(),profile.type_uuid.end());
  Put16(&out,static_cast<std::uint16_t>(profile.codec_id.size()));
  Put16(&out,profile.codec_version); Put64(&out,profile.codec_generation);
  out.push_back(profile.nullable?1:0); out.insert(out.end(),7,0);
  out.insert(out.end(),profile.profile_binding_sha256.begin(),profile.profile_binding_sha256.end());
  out.insert(out.end(),profile.codec_id.begin(),profile.codec_id.end());
  return out;
}

SblrLiteralDescriptorProfileCodecResultV1 DecodeSblrLiteralDescriptorProfileV1(
    const std::uint8_t* bytes, std::size_t size) {
  SblrLiteralDescriptorProfileCodecResultV1 result;
  result.diagnostic_id="DATATYPE.DESCRIPTOR_INVALID";
  const auto fail=[&](std::string detail){result.detail=std::move(detail);return result;};
  if(bytes==nullptr||size<164||!std::equal(bytes,bytes+4,reinterpret_cast<const std::uint8_t*>("SBLP"))||
     U16(bytes+4)!=1||U16(bytes+6)!=164||U32(bytes+8)!=size||U32(bytes+12)!=0)
    return fail("SBLP header is noncanonical");
  const auto codec_bytes=U16(bytes+112);
  if(codec_bytes!=size-164||U16(bytes+114)==0||U64(bytes+116)==0||bytes[124]>1||
     std::any_of(bytes+125,bytes+132,[](auto v){return v!=0;})||
     std::find(bytes+164,bytes+size,0)!=bytes+size)
    return fail("SBLP fields are noncanonical");
  auto& p=result.profile;
  std::copy_n(bytes+16,16,p.profile_uuid.begin()); std::copy_n(bytes+32,16,p.statement_receipt_uuid.begin());
  std::copy_n(bytes+48,16,p.catalog_snapshot_uuid.begin()); p.catalog_generation=U64(bytes+64);
  std::copy_n(bytes+72,16,p.descriptor_uuid.begin()); p.descriptor_generation=U64(bytes+88);
  std::copy_n(bytes+96,16,p.type_uuid.begin()); p.codec_version=U16(bytes+114);p.codec_generation=U64(bytes+116);
  p.nullable=bytes[124]!=0;std::copy_n(bytes+132,32,p.profile_binding_sha256.begin());
  p.codec_id.assign(reinterpret_cast<const char*>(bytes+164),codec_bytes);
  result.canonical_bytes=EncodeSblrLiteralDescriptorProfileV1(p);
  if(result.canonical_bytes.size()!=size||!std::equal(result.canonical_bytes.begin(),result.canonical_bytes.end(),bytes))
    return fail("SBLP decode/re-encode differs");
  result.ok=true;return result;
}

std::array<std::uint8_t, 32> ComputeSblrLiteralDescriptorProfileBindingV2(
    const SblrLiteralStatementDescriptorProfileV2& profile,
    std::uint64_t receipt_security_epoch,
    std::uint64_t receipt_resource_epoch) {
  static constexpr std::string_view domain =
      "ScratchBird.SblrLiteralStatementDescriptorProfile.V2";
  std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
  Put16(&bytes, 2);
  bytes.insert(bytes.end(), profile.profile_uuid.begin(), profile.profile_uuid.end());
  bytes.insert(bytes.end(), profile.statement_receipt_uuid.begin(), profile.statement_receipt_uuid.end());
  bytes.insert(bytes.end(), profile.catalog_snapshot_uuid.begin(), profile.catalog_snapshot_uuid.end());
  Put64(&bytes, profile.catalog_generation);
  bytes.insert(bytes.end(), profile.descriptor_uuid.begin(), profile.descriptor_uuid.end());
  Put64(&bytes, profile.descriptor_generation);
  bytes.insert(bytes.end(), profile.type_uuid.begin(), profile.type_uuid.end());
  bytes.insert(bytes.end(), profile.persisted_descriptor_uuid.begin(),
               profile.persisted_descriptor_uuid.end());
  Put64(&bytes, profile.persisted_descriptor_generation);
  PutText(&bytes, profile.codec_id);
  Put16(&bytes, profile.codec_version);
  Put64(&bytes, profile.codec_generation);
  bytes.push_back(profile.nullable ? 1 : 0);
  Put64(&bytes, receipt_security_epoch);
  Put64(&bytes, receipt_resource_epoch);
  const auto digest = core::hash::ComputeSha256Digest(bytes);
  return digest.ok() ? digest.digest : std::array<std::uint8_t, 32>{};
}

std::vector<std::uint8_t> EncodeSblrLiteralDescriptorProfileV2(
    const SblrLiteralStatementDescriptorProfileV2& profile) {
  if (!Nonzero(profile.profile_uuid) || !Nonzero(profile.statement_receipt_uuid) ||
      !Nonzero(profile.catalog_snapshot_uuid) || profile.catalog_generation == 0 ||
      !Nonzero(profile.descriptor_uuid) || profile.descriptor_generation == 0 ||
      !Nonzero(profile.type_uuid) || profile.descriptor_uuid == profile.type_uuid ||
      !Nonzero(profile.persisted_descriptor_uuid) ||
      profile.persisted_descriptor_generation == 0 ||
      profile.persisted_descriptor_uuid == profile.descriptor_uuid ||
      profile.persisted_descriptor_uuid == profile.type_uuid ||
      profile.codec_id.empty() || profile.codec_id.size() > 65535 ||
      profile.codec_id.find('\0') != std::string::npos ||
      profile.codec_version == 0 || profile.codec_generation == 0) return {};
  const std::size_t total = 188 + profile.codec_id.size();
  if (total > std::numeric_limits<std::uint32_t>::max()) return {};
  std::vector<std::uint8_t> out; out.reserve(total);
  out.insert(out.end(), {'S','B','L','P'}); Put16(&out, 2); Put16(&out, 188);
  Put32(&out, static_cast<std::uint32_t>(total)); Put32(&out, 0);
  out.insert(out.end(), profile.profile_uuid.begin(), profile.profile_uuid.end());
  out.insert(out.end(), profile.statement_receipt_uuid.begin(), profile.statement_receipt_uuid.end());
  out.insert(out.end(), profile.catalog_snapshot_uuid.begin(), profile.catalog_snapshot_uuid.end());
  Put64(&out, profile.catalog_generation);
  out.insert(out.end(), profile.descriptor_uuid.begin(), profile.descriptor_uuid.end());
  Put64(&out, profile.descriptor_generation);
  out.insert(out.end(), profile.type_uuid.begin(), profile.type_uuid.end());
  out.insert(out.end(), profile.persisted_descriptor_uuid.begin(),
             profile.persisted_descriptor_uuid.end());
  Put64(&out, profile.persisted_descriptor_generation);
  Put16(&out, static_cast<std::uint16_t>(profile.codec_id.size()));
  Put16(&out, profile.codec_version); Put64(&out, profile.codec_generation);
  out.push_back(profile.nullable ? 1 : 0); out.insert(out.end(), 7, 0);
  out.insert(out.end(), profile.profile_binding_sha256.begin(),
             profile.profile_binding_sha256.end());
  out.insert(out.end(), profile.codec_id.begin(), profile.codec_id.end());
  return out;
}

SblrLiteralDescriptorProfileCodecResultV2
DecodeSblrLiteralDescriptorProfileV2(const std::uint8_t* bytes,
                                      std::size_t size) {
  SblrLiteralDescriptorProfileCodecResultV2 result;
  result.diagnostic_id = "DATATYPE.DESCRIPTOR_INVALID";
  const auto fail = [&](std::string detail) {
    result.detail = std::move(detail); return result;
  };
  if (bytes == nullptr || size < 188 ||
      !std::equal(bytes, bytes + 4,
                  reinterpret_cast<const std::uint8_t*>("SBLP")) ||
      U16(bytes + 4) != 2 || U16(bytes + 6) != 188 ||
      U32(bytes + 8) != size || U32(bytes + 12) != 0) {
    return fail("SBLP v2 header is noncanonical");
  }
  const auto codec_bytes = U16(bytes + 136);
  if (codec_bytes != size - 188 || U16(bytes + 138) == 0 ||
      U64(bytes + 140) == 0 || bytes[148] > 1 ||
      std::any_of(bytes + 149, bytes + 156, [](auto v) { return v != 0; }) ||
      std::find(bytes + 188, bytes + size, 0) != bytes + size) {
    return fail("SBLP v2 fields are noncanonical");
  }
  auto& p = result.profile;
  std::copy_n(bytes + 16, 16, p.profile_uuid.begin());
  std::copy_n(bytes + 32, 16, p.statement_receipt_uuid.begin());
  std::copy_n(bytes + 48, 16, p.catalog_snapshot_uuid.begin());
  p.catalog_generation = U64(bytes + 64);
  std::copy_n(bytes + 72, 16, p.descriptor_uuid.begin());
  p.descriptor_generation = U64(bytes + 88);
  std::copy_n(bytes + 96, 16, p.type_uuid.begin());
  std::copy_n(bytes + 112, 16, p.persisted_descriptor_uuid.begin());
  p.persisted_descriptor_generation = U64(bytes + 128);
  p.codec_version = U16(bytes + 138); p.codec_generation = U64(bytes + 140);
  p.nullable = bytes[148] != 0;
  std::copy_n(bytes + 156, 32, p.profile_binding_sha256.begin());
  p.codec_id.assign(reinterpret_cast<const char*>(bytes + 188), codec_bytes);
  result.canonical_bytes = EncodeSblrLiteralDescriptorProfileV2(p);
  if (result.canonical_bytes.size() != size ||
      !std::equal(result.canonical_bytes.begin(), result.canonical_bytes.end(), bytes)) {
    return fail("SBLP v2 decode/re-encode differs");
  }
  result.ok = true; return result;
}

std::optional<std::int64_t> DecodeSblrLiteralInt64LeV1(
    const std::uint8_t* bytes, std::size_t size) {
  if (bytes == nullptr || size != 8) return std::nullopt;
  std::uint64_t bits = 0;
  for (unsigned index = 0; index != 8; ++index) {
    bits |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  std::int64_t value = 0;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  if (EncodeSblrLiteralInt64LeV1(value) !=
      std::array<std::uint8_t, 8>{bytes[0], bytes[1], bytes[2], bytes[3],
                                  bytes[4], bytes[5], bytes[6], bytes[7]}) {
    return std::nullopt;
  }
  return value;
}

std::array<std::uint8_t, 8> EncodeSblrLiteralInt64LeV1(
    std::int64_t value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&bits, &value, sizeof(bits));
  std::array<std::uint8_t, 8> encoded{};
  for (unsigned index = 0; index != encoded.size(); ++index) {
    encoded[index] = static_cast<std::uint8_t>(bits >> (index * 8));
  }
  return encoded;
}

SblrLiteralExactDecimalCodecResultV1 EncodeSblrLiteralExactDecimalV1(
    const std::string_view lexical) {
  const auto parts = ValidateDecimalLexicalV1(lexical);
  if (!parts.has_value()) {
    return DecimalFailure(false, "exact decimal lexical grammar is malformed");
  }
  if (parts->overflow) {
    return DecimalFailure(true, "exact decimal exponent exceeds precision 38");
  }

  namespace numeric = scratchbird::libraries::sbl_numeric;
  numeric::NumericRequest request;
  request.operation = numeric::NumericOperation::canonicalize;
  request.type = numeric::NumericType::decimal;
  request.left = {numeric::NumericType::decimal, parts->numeric, false};
  request.context.precision = 38;
  request.context.scale = 0;
  request.context.allow_special_values = false;
  request.context.canonical_preserve_scale = true;
  const auto canonicalized = numeric::ApplyNumericOperation(request);
  if (canonicalized.status == numeric::NumericStatusCode::overflow) {
    return DecimalFailure(true, "exact decimal precision exceeds 38");
  }
  if (canonicalized.status != numeric::NumericStatusCode::ok ||
      canonicalized.value.is_null || canonicalized.value.encoded.empty()) {
    return DecimalFailure(false, "sbl_numeric refused the exact decimal lexical value");
  }

  std::string canonical = canonicalized.value.encoded;
  if (canonical == "-0") canonical = "0";
  std::size_t cursor = 0;
  const bool negative = canonical.front() == '-';
  if (negative) ++cursor;
  const auto decimal = canonical.find('.', cursor);
  const std::size_t scale = decimal == std::string::npos
                                ? 0
                                : canonical.size() - decimal - 1;
  if (scale > 38) {
    return DecimalFailure(true, "exact decimal scale exceeds 38");
  }
  std::string coefficient;
  coefficient.reserve(canonical.size());
  for (; cursor < canonical.size(); ++cursor) {
    const char byte = canonical[cursor];
    if (byte == '.') continue;
    if (byte < '0' || byte > '9') {
      return DecimalFailure(false,
                            "sbl_numeric returned noncanonical decimal text");
    }
    coefficient.push_back(byte);
  }
  const auto first_nonzero = coefficient.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    coefficient = "0";
    canonical = "0";
  } else if (first_nonzero != 0) {
    coefficient.erase(0, first_nonzero);
  }
  const std::size_t precision = std::max(coefficient.size(), scale);
  if (precision == 0 || precision > 38) {
    return DecimalFailure(true, "exact decimal normalized precision exceeds 38");
  }
  const std::size_t group_count =
      coefficient == "0" ? 1 : (coefficient.size() + 8) / 9;
  if (group_count == 0 || group_count > 5) {
    return DecimalFailure(true,
                          "exact decimal coefficient exceeds five base-1e9 groups");
  }

  SblrLiteralExactDecimalCodecResultV1 result;
  result.precision = static_cast<std::uint8_t>(precision);
  result.scale = static_cast<std::uint8_t>(scale);
  result.canonical_lexical = canonical;
  result.canonical_bytes[0] = static_cast<std::uint8_t>(scale);
  if (negative && coefficient != "0") result.canonical_bytes[0] |= 0x80U;
  result.canonical_bytes[1] = result.precision;
  result.canonical_bytes[2] = static_cast<std::uint8_t>(group_count);
  std::size_t end = coefficient.size();
  for (std::size_t group = 0; group < group_count; ++group) {
    const auto begin = end > 9 ? end - 9 : 0;
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(coefficient.data() + begin,
                                        coefficient.data() + end, value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != coefficient.data() + end || value >= 1'000'000'000U) {
      return DecimalFailure(false,
                            "exact decimal coefficient group is malformed");
    }
    const auto offset = 4 + group * 4;
    for (unsigned byte = 0; byte < 4; ++byte) {
      result.canonical_bytes[offset + byte] =
          static_cast<std::uint8_t>(value >> (byte * 8));
    }
    end = begin;
  }
  if (end != 0) {
    return DecimalFailure(true,
                          "exact decimal coefficient group extent overflowed");
  }
  result.ok = true;
  return result;
}

SblrLiteralExactDecimalCodecResultV1 DecodeSblrLiteralExactDecimalV1(
    const std::uint8_t* bytes, const std::size_t size) {
  if (bytes == nullptr || size != kSblrLiteralExactDecimalBytes) {
    return DecimalFailure(false, "exact decimal body must be exactly 24 bytes");
  }
  const bool negative = (bytes[0] & 0x80U) != 0;
  const auto scale = static_cast<std::uint8_t>(bytes[0] & 0x7fU);
  const auto precision = bytes[1];
  const auto group_count = bytes[2];
  if (scale > 38 || precision == 0 || precision > 38 ||
      group_count == 0 || group_count > 5 || bytes[3] != 0) {
    return DecimalFailure(false,
                          "exact decimal header is outside canonical bounds");
  }
  std::array<std::uint32_t, 5> groups{};
  for (std::size_t group = 0; group < groups.size(); ++group) {
    groups[group] = U32(bytes + 4 + group * 4);
    if (groups[group] >= 1'000'000'000U ||
        (group >= group_count && groups[group] != 0)) {
      return DecimalFailure(false,
                            "exact decimal coefficient group is noncanonical");
    }
  }
  if ((group_count > 1 && groups[group_count - 1] == 0) ||
      (group_count == 1 && groups[0] == 0 &&
       (negative || scale != 0 || precision != 1))) {
    return DecimalFailure(false,
                          "exact decimal coefficient is not minimally encoded");
  }

  std::string coefficient = std::to_string(groups[group_count - 1]);
  for (std::size_t remaining = group_count - 1; remaining != 0; --remaining) {
    const auto group = std::to_string(groups[remaining - 1]);
    coefficient.append(9 - group.size(), '0');
    coefficient.append(group);
  }
  const auto expected_group_count =
      coefficient == "0" ? 1 : (coefficient.size() + 8) / 9;
  const auto expected_precision = std::max(coefficient.size(),
                                            static_cast<std::size_t>(scale));
  if (expected_group_count != group_count || expected_precision != precision) {
    return DecimalFailure(false,
                          "exact decimal precision or group count is noncanonical");
  }
  const auto canonical = RenderExactDecimalV1(negative, coefficient, scale);
  const auto reencoded = EncodeSblrLiteralExactDecimalV1(canonical);
  if (!reencoded.ok || !std::equal(reencoded.canonical_bytes.begin(),
                                   reencoded.canonical_bytes.end(), bytes)) {
    return DecimalFailure(false,
                          "exact decimal decode and re-encode bytes differ");
  }
  auto result = reencoded;
  result.precision = precision;
  result.scale = scale;
  return result;
}

static SblrExpressionNodeTableCodecResultV1
DecodeSblrExpressionNodeTableWithLimitV1(
    const std::uint8_t* bytes, std::size_t size,
    const std::size_t maximum_bytes) {
  if (bytes == nullptr || size < kHeaderBytes ||
      size > maximum_bytes ||
      !std::equal(bytes, bytes + 4, reinterpret_cast<const std::uint8_t*>("SBXN")) ||
      U16(bytes + 4) != 1 || U16(bytes + 6) != kHeaderBytes ||
      U32(bytes + 12) != 0 || U64(bytes + 16) != size ||
      U64(bytes + 24) != kHeaderBytes) return Fail("SBXN header is noncanonical");
  const auto count = U32(bytes + 8);
  if (count == 0 || count > kMaximumNodes ||
      count > (size - kHeaderBytes) / kRecordFixedBytes)
    return Fail("SBXN node count is invalid");

  std::size_t offset = kHeaderBytes;
  std::uint64_t previous_id = 0;
  for (std::uint32_t i = 0; i != count; ++i) {
    if (offset > size || size - offset < kRecordFixedBytes) return Fail("SBXN record is truncated");
    const auto record_bytes = U32(bytes + offset);
    if (record_bytes < kRecordFixedBytes || record_bytes > size - offset) return Fail("SBXN record size is invalid");
    const auto end = offset + record_bytes;
    const auto node_id = U64(bytes + offset + 4);
    const auto parent_id = U64(bytes + offset + 12);
    const auto parent_ordinal = U32(bytes + offset + 20);
    const auto generation = U64(bytes + offset + 30);
    const auto literal_size = U64(bytes + offset + 54);
    if (node_id == 0 || node_id <= previous_id || generation == 0 ||
        parent_id != 0 || parent_ordinal == 0 || literal_size > kMaximumLiteralBytes ||
        U16(bytes + offset + 24) != 3 || U16(bytes + offset + 26) != 1 ||
        U16(bytes + offset + 28) != 0) return Fail("SBXN record fields are invalid");
    std::array<std::uint8_t,16> uuid{}; std::copy_n(bytes + offset + 38, 16, uuid.begin());
    if (!Nonzero(uuid)) return Fail("SBXN descriptor UUID is zero");
    std::size_t cursor = offset + 62;
    if (!TakeText(bytes,end,&cursor,"SBLR_LITERAL") ||
        !TakeText(bytes,end,&cursor,"typed_literal") ||
        literal_size > end - cursor) return Fail("SBXN literal prefix is invalid");
    cursor += static_cast<std::size_t>(literal_size);
    if (!TakeText(bytes,end,&cursor,"engine.op.literal") ||
        !TakeText(bytes,end,&cursor,"typed_value") || end - cursor != 2 ||
        U16(bytes + cursor) != 1) return Fail("SBXN literal suffix is invalid");
    previous_id = node_id; offset = end;
  }
  if (offset != size) return Fail("SBXN trailing bytes are forbidden");

  SblrExpressionNodeTableCodecResultV1 result; result.table.nodes.reserve(count);
  offset = kHeaderBytes;
  for (std::uint32_t i = 0; i != count; ++i) {
    const auto end = offset + U32(bytes + offset);
    SblrExpressionLiteralNodeV1 node;
    node.node_id=U64(bytes+offset+4); node.parent_node_id=U64(bytes+offset+12);
    node.parent_operand_ordinal=U32(bytes+offset+20); node.descriptor_generation=U64(bytes+offset+30);
    std::copy_n(bytes+offset+38,16,node.descriptor_uuid.begin());
    const auto literal_size=U64(bytes+offset+54); std::size_t cursor=offset+62;
    cursor += 2 + U16(bytes+cursor); cursor += 2 + U16(bytes+cursor);
    node.literal_body.assign(bytes+cursor,bytes+cursor+literal_size);
    result.table.nodes.push_back(std::move(node)); offset=end;
  }
  std::vector<bool> ordinals(count + 1, false);
  for (const auto& node : result.table.nodes) {
    if (node.parent_operand_ordinal > count ||
        ordinals[node.parent_operand_ordinal]) {
      return Fail("SBXN literal ordinals are not unique and dense");
    }
    ordinals[node.parent_operand_ordinal] = true;
  }
  result.canonical_bytes.assign(bytes, bytes + size);
  result.ok=true; return result;
}

SblrExpressionNodeTableCodecResultV1 DecodeSblrExpressionNodeTableV1(
    const std::uint8_t* bytes, std::size_t size) {
  auto result = DecodeSblrExpressionNodeTableWithLimitV1(
      bytes, size, kSblrExpressionNodeTableMaximumBytesV1);
  if (!result.ok) return result;
  const auto reencoded = EncodeSblrExpressionNodeTableV1(result.table);
  if (reencoded.size() != size ||
      !std::equal(reencoded.begin(), reencoded.end(), bytes)) {
    return Fail("SBXN decode/re-encode differs");
  }
  result.canonical_bytes = reencoded;
  return result;
}

SblrExpressionNodeTableCodecResultV1
DecodeSblrContextualComposedExpressionNodeTableV2(
    const std::uint8_t* bytes, std::size_t size) {
  return DecodeSblrExpressionNodeTableWithLimitV1(
      bytes, size,
      kSblrContextualComposedExpressionNodeTableMaximumBytesV2);
}

std::vector<std::uint8_t> EncodeSblrExpressionNodeTableV1(const SblrExpressionNodeTableV1& table) {
  if (table.nodes.empty() || table.nodes.size()>kMaximumNodes) return {};
  std::size_t total=kHeaderBytes; std::uint64_t previous=0;
  for (const auto& n:table.nodes) {
    if(n.node_id==0||n.node_id<=previous||n.descriptor_generation==0||!Nonzero(n.descriptor_uuid)||
       n.parent_node_id!=0||n.parent_operand_ordinal==0||n.literal_body.size()>kMaximumLiteralBytes||
       n.literal_body.size()>std::numeric_limits<std::size_t>::max()-kRecordFixedBytes) return {};
    const auto record=kRecordFixedBytes+n.literal_body.size(); if(record>std::numeric_limits<std::uint32_t>::max()||record>std::numeric_limits<std::size_t>::max()-total)return{};
    total+=record;
    if(total>kMaximumExpressionNodeTableBytes)return{};
    previous=n.node_id;
  }
  std::vector<bool> ordinals(table.nodes.size()+1,false);
  for(const auto& n:table.nodes){if(n.parent_operand_ordinal>table.nodes.size()||ordinals[n.parent_operand_ordinal])return{};ordinals[n.parent_operand_ordinal]=true;}
  std::vector<std::uint8_t> out; out.reserve(total); out.insert(out.end(),{'S','B','X','N'});
  Put16(&out,1);Put16(&out,32);Put32(&out,static_cast<std::uint32_t>(table.nodes.size()));Put32(&out,0);Put64(&out,total);Put64(&out,32);
  for(const auto& n:table.nodes){Put32(&out,static_cast<std::uint32_t>(kRecordFixedBytes+n.literal_body.size()));Put64(&out,n.node_id);Put64(&out,n.parent_node_id);Put32(&out,n.parent_operand_ordinal);Put16(&out,3);Put16(&out,1);Put16(&out,0);Put64(&out,n.descriptor_generation);out.insert(out.end(),n.descriptor_uuid.begin(),n.descriptor_uuid.end());Put64(&out,n.literal_body.size());PutText(&out,"SBLR_LITERAL");PutText(&out,"typed_literal");out.insert(out.end(),n.literal_body.begin(),n.literal_body.end());PutText(&out,"engine.op.literal");PutText(&out,"typed_value");Put16(&out,1);}
  return out;
}

bool DecodeSblrExpressionNodeReferenceV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrExpressionNodeReferenceV1* out) {
  if (bytes == nullptr || out == nullptr || size != 72 || U16(bytes) != 1 ||
      U16(bytes + 2) != 0 || U32(bytes + 4) == 0 || U64(bytes + 8) == 0) {
    return false;
  }
  SblrExpressionNodeReferenceV1 value;
  value.occurrence_ordinal = U32(bytes + 4);
  value.node_id = U64(bytes + 8);
  std::copy_n(bytes + 16, 32, value.node_table_sha256.begin());
  std::copy_n(bytes + 48, 16, value.descriptor_uuid.begin());
  value.descriptor_generation = U64(bytes + 64);
  if (!Nonzero(value.descriptor_uuid) || value.descriptor_generation == 0 ||
      std::all_of(value.node_table_sha256.begin(),
                  value.node_table_sha256.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    return false;
  }
  *out = value;
  return true;
}

bool ValidateSblrLiteralReferenceBijectionV1(
    const SblrExpressionNodeTableCodecResultV1& table,
    const std::vector<SblrExpressionNodeReferenceV1>& references) {
  if (!table.ok || references.size() != table.table.nodes.size()) return false;
  const auto digest = core::hash::ComputeSha256Digest(table.canonical_bytes);
  if (!digest.ok()) return false;
  std::vector<bool> seen(table.table.nodes.size(), false);
  for (const auto& reference : references) {
    const auto found = std::find_if(
        table.table.nodes.begin(), table.table.nodes.end(),
        [&](const auto& node) { return node.node_id == reference.node_id; });
    if (found == table.table.nodes.end()) return false;
    const auto index = static_cast<std::size_t>(found - table.table.nodes.begin());
    if (seen[index] || found->parent_operand_ordinal != reference.occurrence_ordinal ||
        found->descriptor_uuid != reference.descriptor_uuid ||
        found->descriptor_generation != reference.descriptor_generation ||
        digest.digest != reference.node_table_sha256) return false;
    seen[index] = true;
  }
  return std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
}
}  // namespace scratchbird::engine::sblr
