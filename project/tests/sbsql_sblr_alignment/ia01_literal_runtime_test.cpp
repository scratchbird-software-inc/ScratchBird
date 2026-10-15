// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "sblr_literal_runtime.hpp"
#include "sblr_engine_envelope.hpp"
#include "hash_digest.hpp"

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace sblr = scratchbird::engine::sblr;
[[noreturn]] void Fail(const char* text){std::cerr<<text<<'\n';std::exit(EXIT_FAILURE);}
void Require(bool value,const char* text){if(!value)Fail(text);}
void U16(std::vector<std::uint8_t>* out,std::uint16_t v){out->push_back(v);out->push_back(v>>8);}
void U32(std::vector<std::uint8_t>* out,std::uint32_t v){for(unsigned i=0;i!=4;++i)out->push_back(v>>(8*i));}
void U64(std::vector<std::uint8_t>* out,std::uint64_t v){for(unsigned i=0;i!=8;++i)out->push_back(v>>(8*i));}

sblr::SblrLiteralExactDecimalCodecResultV1 RequireDecimal(
    const std::string_view lexical,
    const std::string_view canonical,
    const std::uint8_t precision,
    const std::uint8_t scale) {
  const auto encoded = sblr::EncodeSblrLiteralExactDecimalV1(lexical);
  Require(encoded.ok, "exact decimal lexical value was refused");
  Require(encoded.canonical_lexical == canonical,
          "exact decimal lexical normalization differs");
  Require(encoded.precision == precision && encoded.scale == scale,
          "exact decimal precision or scale differs");
  const auto decoded = sblr::DecodeSblrLiteralExactDecimalV1(
      encoded.canonical_bytes.data(), encoded.canonical_bytes.size());
  Require(decoded.ok && decoded.canonical_bytes == encoded.canonical_bytes &&
              decoded.canonical_lexical == canonical &&
              decoded.precision == precision && decoded.scale == scale,
          "exact decimal canonical round trip failed");
  return encoded;
}

std::array<std::uint8_t,16> Identity(std::uint8_t suffix) {
  return {1,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,suffix};
}
std::vector<std::uint8_t> StatefulGolden(const sblr::SblrExpressionNodeTableV1& table){
  std::size_t bytes=32;for(const auto& n:table.nodes)bytes+=133+n.literal_body.size();
  std::vector<std::uint8_t> out{'S','B','X','N'};U16(&out,2);U16(&out,32);
  U32(&out,table.nodes.size());U32(&out,0);U64(&out,bytes);U64(&out,32);
  const auto text=[&](std::string_view s){U16(&out,s.size());out.insert(out.end(),s.begin(),s.end());};
  for(const auto& n:table.nodes){
    U32(&out,133+n.literal_body.size());U64(&out,n.node_id);U64(&out,0);
    U32(&out,n.parent_operand_ordinal);U16(&out,3);U16(&out,1);U16(&out,0);
    U64(&out,n.descriptor_generation);out.insert(out.end(),n.descriptor_uuid.begin(),n.descriptor_uuid.end());
    out.push_back(n.value_state);out.insert(out.end(),7,0);U64(&out,n.literal_body.size());
    text("SBLR_LITERAL");text("typed_literal");out.insert(out.end(),n.literal_body.begin(),n.literal_body.end());
    text("engine.op.literal");text("typed_value");U16(&out,1);
  }
  return out;
}
void StatefulTableChecks(){
  sblr::SblrExpressionNodeTableV1 table;
  for(unsigned at=0;at<4;++at){
    sblr::SblrExpressionLiteralNodeV1 n;n.node_id=7+at;n.parent_operand_ordinal=at+1;
    n.descriptor_uuid=Identity(at+1);n.descriptor_generation=1;
    if(at==1)n.value_state=1;
    else n.literal_body={static_cast<std::uint8_t>(at==2)};
    if(at==3)n.literal_body={7,0,0,0,0,0,0,0};
    table.nodes.push_back(n);
  }
  const auto bytes=sblr::EncodeSblrStatefulExpressionNodeTableV2(table);
  Require(bytes.size()==574&&bytes==StatefulGolden(table)&&bytes[4]==2&&bytes[220]==1,
          "stateful node table differs from full independent V2 byte oracle");
  const auto decoded=sblr::DecodeSblrStatefulExpressionNodeTableV2(bytes.data(),bytes.size());
  Require(decoded.ok&&decoded.table.wire_version==2&&decoded.canonical_bytes==bytes&&
      decoded.table.nodes.size()==4&&decoded.table.nodes[0].value_state==0&&
      decoded.table.nodes[0].literal_body==std::vector<std::uint8_t>{0}&&
      decoded.table.nodes[1].value_state==1&&decoded.table.nodes[1].literal_body.empty()&&
      decoded.table.nodes[2].value_state==0&&decoded.table.nodes[2].literal_body==std::vector<std::uint8_t>{1}&&
      decoded.table.nodes[3].literal_body==table.nodes[3].literal_body&&
      sblr::EncodeSblrStatefulExpressionNodeTableV2(decoded.table)==bytes,
      "FALSE/NULL/TRUE/numeric state or version lost on structural roundtrip");
  Require(!sblr::DecodeSblrExpressionNodeTableV1(bytes.data(),bytes.size()).ok&&
      !sblr::DecodeSblrContextualComposedExpressionNodeTableV2(bytes.data(),bytes.size()).ok&&
      sblr::EncodeSblrExpressionNodeTableV1(table).empty(),
      "old ingress must refuse stateful wire until its real owning integration exists");
  const auto refuse=[&](const std::vector<std::uint8_t>& b){
    const auto d=sblr::DecodeSblrStatefulExpressionNodeTableV2(b.data(),b.size());
    Require(!d.ok&&d.table.nodes.empty()&&d.canonical_bytes.empty(),"invalid V2 returns a decoded prefix");
  };
  for(std::size_t n=0;n<bytes.size();++n)refuse({bytes.begin(),bytes.begin()+n});
  for(const auto at:{0u,4u,6u,12u,24u,32u,86u,220u}){
    auto b=bytes;b[at]=0xff;refuse(b);
  }
  for(unsigned reserved=1;reserved<8;++reserved){auto b=bytes;b[86+reserved]=1;refuse(b);}
  auto b=bytes;b.push_back(0);refuse(b);
  auto invalid=table;invalid.nodes[1].literal_body={0};refuse(StatefulGolden(invalid));
  Require(sblr::EncodeSblrStatefulExpressionNodeTableV2(invalid).empty(),"NULL payload encoded");
  invalid=table;invalid.nodes[1].value_state=0;refuse(StatefulGolden(invalid));
  invalid=table;invalid.nodes[0].value_state=2;refuse(StatefulGolden(invalid));
  Require(sblr::EncodeSblrStatefulExpressionNodeTableV2(invalid).empty(),"unknown state encoded");
  for(const auto version:{1u,3u,65535u}){
    invalid=table;invalid.wire_version=version;
    Require(sblr::EncodeSblrStatefulExpressionNodeTableV2(invalid).empty(),"pinned version silently changed");
  }
  invalid=decoded.table;invalid.nodes[1].value_state=0;
  Require(sblr::EncodeSblrStatefulExpressionNodeTableV2(invalid).empty(),"decoded V2 silently downgraded");
  auto present=table;present.nodes[1].value_state=0;present.nodes[1].literal_body={0};
  const auto old=sblr::EncodeSblrExpressionNodeTableV1(present);
  Require(!old.empty()&&old[4]==1&&sblr::EncodeSblrStatefulExpressionNodeTableV2(present)==old,
          "all-present extension changes frozen V1 bytes");
  const auto old_decoded=sblr::DecodeSblrStatefulExpressionNodeTableV2(old.data(),old.size());
  Require(old_decoded.ok&&old_decoded.table.wire_version==1&&
      sblr::EncodeSblrStatefulExpressionNodeTableV2(old_decoded.table)==old,
      "decoded V1 version/hash material not retained");
  present.nodes[0].literal_body.clear();const auto empty_present=sblr::EncodeSblrExpressionNodeTableV1(present);
  const auto empty=sblr::DecodeSblrStatefulExpressionNodeTableV2(empty_present.data(),empty_present.size());
  Require(empty.ok&&empty.table.nodes[0].value_state==0,"empty present body inferred NULL without type admission");
  sblr::SblrExpressionNodeTableV1 maximum;
  for(unsigned n=1;n<=4096;++n){auto item=table.nodes[0];item.node_id=n;item.parent_operand_ordinal=n;
    item.value_state=n==1?1:0;item.literal_body.assign(n==1?0:24,0);maximum.nodes.push_back(item);}
  const auto max=sblr::EncodeSblrStatefulExpressionNodeTableV2(maximum);
  Require(max.size()==643080&&sblr::DecodeSblrStatefulExpressionNodeTableV2(max.data(),max.size()).ok,
          "exact4096-node stateful bound refused");
  maximum.nodes.back().literal_body.push_back(0);
  Require(sblr::EncodeSblrStatefulExpressionNodeTableV2(maximum).empty(),"one-byte stateful excess encoded");
  refuse(StatefulGolden(maximum));
  maximum.nodes.back().literal_body.assign(32563,0);
  const auto composed=sblr::EncodeSblrStatefulComposedExpressionNodeTableV2(maximum);
  Require(composed.size()==675619&&
      sblr::DecodeSblrStatefulComposedExpressionNodeTableV2(composed.data(),composed.size()).ok&&
      !sblr::DecodeSblrStatefulExpressionNodeTableV2(composed.data(),composed.size()).ok,
      "composed bound or ordinary/composed separation lost");
  maximum.nodes.back().literal_body.push_back(0);
  const auto too_large=StatefulGolden(maximum);
  Require(sblr::EncodeSblrStatefulComposedExpressionNodeTableV2(maximum).empty()&&
      !sblr::DecodeSblrStatefulComposedExpressionNodeTableV2(too_large.data(),too_large.size()).ok,
      "one-byte composed excess admitted");
  for(const auto ordinal:{0u,1u,4097u,0xffffffffu}){
    invalid=table;invalid.nodes[1].parent_operand_ordinal=ordinal;
    Require(sblr::EncodeSblrStatefulExpressionNodeTableV2(invalid).empty(),"invalid/doubled ordinal encoded");
    refuse(StatefulGolden(invalid));
  }
}
int main(){
  StatefulTableChecks();
  {
    const auto hex=[](const std::array<std::uint8_t,32>& digest){
      constexpr char digits[]="0123456789abcdef";std::string out;
      for(const auto b:digest){out.push_back(digits[b>>4]);out.push_back(digits[b&15]);}
      return out;
    };
    const std::array<std::uint8_t,2> body{0,1};
    const auto no=sblr::ComputeSblrBooleanLiteralLexicalSha256V1(0,body.data(),1);
    const auto yes=sblr::ComputeSblrBooleanLiteralLexicalSha256V1(0,body.data()+1,1);
    const auto null=sblr::ComputeSblrBooleanLiteralLexicalSha256V1(1,nullptr,0);
    // Independent SHA-256 vectors for exact SBBL/version/state/count/body bytes,
    // not an expectation computed by the production framing/hash helper.
    Require(no&&hex(*no)=="0983f09b6c908a036475624ec51d863d60534832da47187d25a955966dec0414",
            "FALSE lexical framing/hash differs");
    Require(yes&&hex(*yes)=="a528a06a6bebfd2192e2bd6cba17b75898610f513e0045d3c466ee07284a6cd5",
            "TRUE lexical framing/hash differs");
    Require(null&&hex(*null)=="ee94fa875bbed7c2216f22905dde7dcb64de43ef1234263977383f8ef33cdc43"&&
        *no!=*null&&*yes!=*null&&*yes!=*no,
        "typed NULL lexical identity must differ from both present Boolean values");
    Require(sblr::ComputeSblrBooleanLiteralLexicalSha256V1(1,body.data(),0)==null,
            "empty SQL NULL must not dereference or hash backing bytes");
    for(unsigned state=0;state<256;++state){
      const auto result=sblr::ComputeSblrBooleanLiteralLexicalSha256V1(state,body.data(),1);
      Require((state==0)==result.has_value(),"unknown or NULL state accepted present payload");
      const auto empty=sblr::ComputeSblrBooleanLiteralLexicalSha256V1(state,nullptr,0);
      Require((state==1)==empty.has_value(),"empty bytes inferred NULL or accepted an unknown state");
    }
    for(unsigned value=0;value<256;++value){
      const std::uint8_t byte=static_cast<std::uint8_t>(value);
      Require((value<2)==sblr::ComputeSblrBooleanLiteralLexicalSha256V1(0,&byte,1).has_value(),
              "noncanonical Boolean byte accepted");
    }
    for(const auto bytes:{std::size_t{1},std::size_t{2},std::numeric_limits<std::size_t>::max()}){
      Require(!sblr::ComputeSblrBooleanLiteralLexicalSha256V1(0,nullptr,bytes),
              "missing present backing accepted");
      Require(!sblr::ComputeSblrBooleanLiteralLexicalSha256V1(1,body.data(),bytes),
              "SQL NULL payload accepted");
      if(bytes!=1)Require(!sblr::ComputeSblrBooleanLiteralLexicalSha256V1(0,body.data(),bytes),
              "wrong Boolean extent read or accepted");
    }
  }
  for (const auto value : {std::int64_t{0}, std::int64_t{1},
                           std::int64_t{-1},
                           std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max()}) {
    const auto bytes = sblr::EncodeSblrLiteralInt64LeV1(value);
    const auto round_trip =
        sblr::DecodeSblrLiteralInt64LeV1(bytes.data(), bytes.size());
    Require(round_trip.has_value() && *round_trip == value,
            "bigint codec round trip failed");
  }
  const std::array<std::uint8_t, 7> short_bigint{};
  Require(!sblr::DecodeSblrLiteralInt64LeV1(
               short_bigint.data(), short_bigint.size()).has_value(),
          "short bigint body was admitted");

  const auto half = RequireDecimal("0.5", "0.5", 1, 1);
  std::array<std::uint8_t, sblr::kSblrLiteralExactDecimalBytes>
      expected_half{};
  expected_half[0] = 1;
  expected_half[1] = 1;
  expected_half[2] = 1;
  expected_half[4] = 5;
  Require(half.canonical_bytes == expected_half,
          "exact decimal 0.5 bytes differ from the Core codec");
  Require(RequireDecimal("+000.5000", "0.5", 1, 1).canonical_bytes ==
              expected_half,
          "equivalent exact decimal spelling did not normalize identically");
  Require(RequireDecimal("5e-1", "0.5", 1, 1).canonical_bytes ==
              expected_half,
          "exact decimal exponent spelling did not normalize identically");
  RequireDecimal("1_000.500_0DECIMAL", "1000.5", 5, 1);
  RequireDecimal("-0.000D", "0", 1, 0);
  RequireDecimal("99999999999999999999999999999999999999",
                 "99999999999999999999999999999999999999", 38, 0);
  RequireDecimal("1e-38", "0.00000000000000000000000000000000000001",
                 38, 38);

  for (const auto overflow : {
           std::string_view{"999999999999999999999999999999999999999"},
           std::string_view{"1e-39"}, std::string_view{"1e38"}}) {
    const auto result = sblr::EncodeSblrLiteralExactDecimalV1(overflow);
    Require(!result.ok &&
                result.diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID",
            "exact decimal overflow did not refuse with descriptor diagnostic");
  }
  for (const auto malformed : {
           std::string_view{}, std::string_view{"1."},
           std::string_view{".5"}, std::string_view{"1__0.5"},
           std::string_view{"1_.5"}, std::string_view{"1.5_"},
           std::string_view{"1e"}, std::string_view{"1.5F"}}) {
    const auto result = sblr::EncodeSblrLiteralExactDecimalV1(malformed);
    Require(!result.ok && result.diagnostic_id == "SBLR.OPERAND_INVALID",
            "malformed exact decimal did not refuse with operand diagnostic");
  }

  const auto require_malformed_decimal = [&](auto bytes) {
    const auto result = sblr::DecodeSblrLiteralExactDecimalV1(
        bytes.data(), bytes.size());
    Require(!result.ok && result.diagnostic_id == "SBLR.OPERAND_INVALID",
            "noncanonical exact decimal bytes were admitted");
  };
  std::array<std::uint8_t, 23> short_decimal{};
  require_malformed_decimal(short_decimal);
  auto malformed_decimal = expected_half;
  malformed_decimal[0] = 39;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[1] = 0;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[2] = 0;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[2] = 6;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[3] = 1;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[4] = 0x00;
  malformed_decimal[5] = 0xca;
  malformed_decimal[6] = 0x9a;
  malformed_decimal[7] = 0x3b;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[8] = 1;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[2] = 2;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = {};
  malformed_decimal[0] = 0x80;
  malformed_decimal[1] = 1;
  malformed_decimal[2] = 1;
  require_malformed_decimal(malformed_decimal);
  malformed_decimal = expected_half;
  malformed_decimal[1] = 2;
  require_malformed_decimal(malformed_decimal);

  sblr::SblrLiteralExecutorEvidenceV1 executor_evidence;
  executor_evidence.descriptor_uuid=Identity(1);
  executor_evidence.descriptor_generation=1;
  executor_evidence.canonical_value_sha256[0]=1;
  Require(sblr::ComputeSblrLiteralExecutorEvidenceSha256V1(executor_evidence).has_value(),
          "exact literal executor evidence schema refused");
  for(const auto malformed : {std::string{"executor_id"},std::string{"opcode_version"},
                              std::string{"operand_descriptor_id"},std::string{"result_descriptor_id"}}){
    auto candidate=executor_evidence;
    if(malformed=="executor_id")candidate.executor_id="engine.op.other";
    if(malformed=="opcode_version")candidate.opcode_version="1";
    if(malformed=="operand_descriptor_id")candidate.operand_descriptor_id="literal";
    if(malformed=="result_descriptor_id")candidate.result_descriptor_id="scalar";
    Require(!sblr::ComputeSblrLiteralExecutorEvidenceSha256V1(candidate).has_value(),
            "malformed literal executor evidence field admitted");
  }
  auto bad_result_version=executor_evidence;bad_result_version.result_descriptor_version=2;
  Require(!sblr::ComputeSblrLiteralExecutorEvidenceSha256V1(bad_result_version).has_value(),
          "malformed typed_value result descriptor version admitted");
  sblr::SblrLiteralStatementDescriptorProfileV1 profile;
  profile.profile_uuid=Identity(1);profile.statement_receipt_uuid=Identity(2);
  profile.catalog_snapshot_uuid=Identity(3);profile.catalog_generation=4;
  profile.descriptor_uuid=Identity(5);profile.descriptor_generation=6;
  profile.type_uuid=Identity(7);profile.codec_id="datatype.int64.le.v1";
  profile.codec_version=1;profile.codec_generation=8;profile.nullable=false;
  profile.profile_binding_sha256=
      sblr::ComputeSblrLiteralDescriptorProfileBindingV1(profile,9,10);
  const auto encoded_profile=sblr::EncodeSblrLiteralDescriptorProfileV1(profile);
  Require(encoded_profile.size()==184,"SBLP bigint record is not 184 bytes");
  const auto decoded_profile=sblr::DecodeSblrLiteralDescriptorProfileV1(
      encoded_profile.data(),encoded_profile.size());
  Require(decoded_profile.ok&&decoded_profile.canonical_bytes==encoded_profile,
          "SBLP canonical round trip failed");
  Require(sblr::ComputeSblrLiteralDescriptorProfileBindingV1(
              decoded_profile.profile,9,10)==profile.profile_binding_sha256,
          "SBLP binding revalidation failed");
  auto decimal_profile = profile;
  decimal_profile.codec_id = std::string{sblr::kSblrLiteralExactDecimalCodecId};
  decimal_profile.profile_binding_sha256 =
      sblr::ComputeSblrLiteralDescriptorProfileBindingV1(decimal_profile, 9, 10);
  const auto encoded_decimal_profile =
      sblr::EncodeSblrLiteralDescriptorProfileV1(decimal_profile);
  Require(encoded_decimal_profile.size() == 194,
          "SBLP exact decimal record is not 194 bytes");
  Require(sblr::DecodeSblrLiteralDescriptorProfileV1(
              encoded_decimal_profile.data(), encoded_decimal_profile.size())
              .ok,
          "SBLP exact decimal profile round trip failed");
  auto stale_profile=encoded_profile;stale_profile[64]^=1;
  const auto decoded_stale=sblr::DecodeSblrLiteralDescriptorProfileV1(
      stale_profile.data(),stale_profile.size());
  Require(decoded_stale.ok&&
              sblr::ComputeSblrLiteralDescriptorProfileBindingV1(
                  decoded_stale.profile,9,10)!=
                  decoded_stale.profile.profile_binding_sha256,
          "SBLP stale generation did not invalidate binding");
  sblr::SblrLiteralStatementDescriptorProfileV2 profile_v2;
  profile_v2.profile_uuid=Identity(1); profile_v2.statement_receipt_uuid=Identity(2);
  profile_v2.catalog_snapshot_uuid=Identity(3); profile_v2.catalog_generation=4;
  profile_v2.descriptor_uuid=Identity(5); profile_v2.descriptor_generation=6;
  profile_v2.type_uuid=Identity(7); profile_v2.persisted_descriptor_uuid=Identity(8);
  profile_v2.persisted_descriptor_generation=9;
  profile_v2.codec_id="datatype.int64.le.v1"; profile_v2.codec_version=1;
  profile_v2.codec_generation=10;
  profile_v2.profile_binding_sha256 =
      sblr::ComputeSblrLiteralDescriptorProfileBindingV2(profile_v2,11,12);
  const auto encoded_profile_v2 =
      sblr::EncodeSblrLiteralDescriptorProfileV2(profile_v2);
  Require(encoded_profile_v2.size()==208,
          "SBLP v2 bigint record is not 208 bytes");
  const auto decoded_profile_v2 =
      sblr::DecodeSblrLiteralDescriptorProfileV2(
          encoded_profile_v2.data(),encoded_profile_v2.size());
  Require(decoded_profile_v2.ok &&
              decoded_profile_v2.canonical_bytes == encoded_profile_v2,
          "SBLP v2 canonical round trip failed");
  Require(sblr::ComputeSblrLiteralDescriptorProfileBindingV2(
              decoded_profile_v2.profile,11,12) ==
              profile_v2.profile_binding_sha256,
          "SBLP v2 binding revalidation failed");
  auto non_distinct_v2 = profile_v2;
  non_distinct_v2.persisted_descriptor_uuid = non_distinct_v2.type_uuid;
  Require(sblr::EncodeSblrLiteralDescriptorProfileV2(non_distinct_v2).empty(),
          "SBLP v2 accepted a persisted handle aliased to type authority");
  const auto check_boolean_alias=[&](auto boolean_profile,auto encode,auto decode){
    boolean_profile.descriptor_uuid={0x01,0,0,0,0x62,0x6f,0x7f,0x6c,
                                     0xa5,0x61,0x6e,0,0,0,0,0};
    boolean_profile.type_uuid=boolean_profile.descriptor_uuid;
    boolean_profile.descriptor_generation=1;
    boolean_profile.codec_id="datatype.boolean.u8.v1";
    boolean_profile.codec_version=1;boolean_profile.codec_generation=1;
    for(bool nullable:{false,true}){
      boolean_profile.nullable=nullable;
      const auto encoded=encode(boolean_profile);
      const auto decoded=decode(encoded.data(),encoded.size());
      Require(!encoded.empty()&&decoded.ok&&decoded.profile.nullable==nullable&&
              decoded.canonical_bytes==encoded,
              "SBLP exact BOOLEAN alias/nullability round trip refused");
      for(unsigned mutation=0;mutation<5;++mutation){
        auto altered=boolean_profile;
        if(mutation==0)++altered.descriptor_generation;
        if(mutation==1)++altered.codec_generation;
        if(mutation==2)++altered.codec_version;
        if(mutation==3){altered.descriptor_uuid[15]^=1;altered.type_uuid=altered.descriptor_uuid;}
        if(mutation==4)altered.codec_id="datatype.int64.le.v1";
        Require(encode(altered).empty(),"SBLP admitted altered/second BOOLEAN alias tuple");
      }
    }
  };
  check_boolean_alias(profile,sblr::EncodeSblrLiteralDescriptorProfileV1,
                      sblr::DecodeSblrLiteralDescriptorProfileV1);
  check_boolean_alias(profile_v2,sblr::EncodeSblrLiteralDescriptorProfileV2,
                      sblr::DecodeSblrLiteralDescriptorProfileV2);
  sblr::SblrLiteralPrebindRequestV1 prebind;
  prebind.preliminary_receipt_uuid=Identity(1);prebind.catalog_snapshot_uuid=Identity(2);
  prebind.catalog_generation=1;prebind.security_epoch=0;prebind.resource_epoch=0;
  prebind.mga_snapshot_uuid=Identity(4);
  sblr::SblrLiteralDemandV1 demand;demand.occurrence_id=1;
  demand.lexical_class=1;demand.context_class=1;
  prebind.demands.push_back(demand);
  prebind.demand_sha256=sblr::ComputeSblrLiteralDemandSequenceSha256V1(prebind.demands);
  const auto encoded_prebind=sblr::EncodeSblrLiteralPrebindRequestV1(prebind);
  Require(encoded_prebind.size()==176,"SBLN one-demand size differs");
  const auto decoded_prebind=sblr::DecodeSblrLiteralPrebindRequestV1(encoded_prebind.data(),encoded_prebind.size());
  Require(decoded_prebind.ok&&decoded_prebind.canonical_bytes==encoded_prebind,"SBLN canonical round trip failed");
  auto bad_prebind=encoded_prebind;bad_prebind[128+13]=1;
  Require(!sblr::DecodeSblrLiteralPrebindRequestV1(bad_prebind.data(),bad_prebind.size()).ok,"SBLN reserved demand byte admitted");
  sblr::SblrLiteralPrebindResultV1 prebind_result;
  prebind_result.preliminary_receipt_uuid=prebind.preliminary_receipt_uuid;
  prebind_result.catalog_snapshot_uuid=prebind.catalog_snapshot_uuid;
  prebind_result.catalog_generation=1;prebind_result.mga_snapshot_uuid=prebind.mga_snapshot_uuid;
  prebind_result.demand_sha256=prebind.demand_sha256;
  prebind_result.mappings.push_back({1,encoded_profile});
  prebind_result.ordered_profile_sha256=sblr::ComputeSblrLiteralOrderedProfilesSha256V1(prebind_result.mappings);
  Require(sblr::EncodeSblrLiteralPrebindResultV1(prebind_result).size()==356,"SBLQ one-profile size differs");
  sblr::SblrExpressionNodeTableV1 finalize_table;sblr::SblrExpressionLiteralNodeV1 finalize_node;finalize_node.node_id=7;finalize_node.parent_operand_ordinal=1;finalize_node.descriptor_generation=profile.descriptor_generation;finalize_node.descriptor_uuid=profile.descriptor_uuid;finalize_node.literal_body={1,0,0,0,0,0,0,0};finalize_table.nodes.push_back(finalize_node);const auto finalize_sbxn=sblr::EncodeSblrExpressionNodeTableV1(finalize_table);
  sblr::SblrLiteralBoundAstV1 finalize_bound;finalize_bound.preliminary_receipt_uuid=prebind.preliminary_receipt_uuid;finalize_bound.demand_sha256=prebind.demand_sha256;const auto finalize_sbba=sblr::EncodeSblrLiteralBoundAstV1(finalize_bound);const auto finalize_bound_hash=sblr::ComputeSblrLiteralBoundAstSha256V1(finalize_sbba);
  std::vector<std::uint8_t> sblf;sblf.insert(sblf.end(),{'S','B','L','F'});U16(&sblf,1);U16(&sblf,208);U32(&sblf,static_cast<std::uint32_t>(208+finalize_sbba.size()+finalize_sbxn.size()));U32(&sblf,0);
  sblf.insert(sblf.end(),prebind.preliminary_receipt_uuid.begin(),prebind.preliminary_receipt_uuid.end());
  sblf.insert(sblf.end(),prebind.demand_sha256.begin(),prebind.demand_sha256.end());sblf.insert(sblf.end(),prebind_result.ordered_profile_sha256.begin(),prebind_result.ordered_profile_sha256.end());
  auto bound_hash=finalize_bound_hash;auto sbxn_hash=scratchbird::core::hash::ComputeSha256Digest(finalize_sbxn).digest;
  sblf.insert(sblf.end(),bound_hash.begin(),bound_hash.end());sblf.insert(sblf.end(),sbxn_hash.begin(),sbxn_hash.end());U64(&sblf,1);U64(&sblf,0);U64(&sblf,0);sblf.insert(sblf.end(),prebind.mga_snapshot_uuid.begin(),prebind.mga_snapshot_uuid.end());U32(&sblf,static_cast<std::uint32_t>(finalize_sbba.size()));U32(&sblf,static_cast<std::uint32_t>(finalize_sbxn.size()));sblf.insert(sblf.end(),finalize_sbba.begin(),finalize_sbba.end());sblf.insert(sblf.end(),finalize_sbxn.begin(),finalize_sbxn.end());
  sblr::SblrLiteralFinalizeRequestV1 finalize;Require(sblr::DecodeSblrLiteralFinalizeRequestV1(sblf.data(),sblf.size(),&finalize),"SBLF exact decode failed");
  sblr::SblrLiteralAdmissionV1 admission;admission.preliminary_receipt_uuid=prebind.preliminary_receipt_uuid;admission.final_receipt_uuid=Identity(10);admission.admission_token_uuid=Identity(11);admission.demand_sha256=prebind.demand_sha256;admission.ordered_profile_sha256=prebind_result.ordered_profile_sha256;admission.bound_ast_sha256=bound_hash;admission.sbxn_sha256=sbxn_hash;admission.catalog_generation=1;admission.mga_snapshot_uuid=prebind.mga_snapshot_uuid;
  Require(sblr::EncodeSblrLiteralAdmissionV1(&admission).size()==264,"SBLA exact size differs");
  sblr::SblrLiteralBoundAstV1 bound_ast;bound_ast.preliminary_receipt_uuid=prebind.preliminary_receipt_uuid;bound_ast.demand_sha256=prebind.demand_sha256;
  sblr::SblrLiteralBoundAstNodeV1 bound_node;bound_node.parent_operand_ordinal=1;bound_node.node_id=7;bound_node.descriptor_uuid=profile.descriptor_uuid;bound_node.descriptor_generation=profile.descriptor_generation;bound_node.type_uuid=profile.type_uuid;bound_node.profile_uuid=profile.profile_uuid;bound_node.occurrence_id=1;bound_ast.nodes.push_back(bound_node);
  const auto sbba=sblr::EncodeSblrLiteralBoundAstV1(bound_ast);Require(sbba.size()==192,"SBBA one-node size differs");sblr::SblrLiteralBoundAstV1 decoded_sbba;Require(sblr::DecodeSblrLiteralBoundAstV1(sbba.data(),sbba.size(),&decoded_sbba),"SBBA canonical decode failed");const auto sbba_hash=sblr::ComputeSblrLiteralBoundAstSha256V1(sbba);Require(std::any_of(sbba_hash.begin(),sbba_hash.end(),[](auto v){return v!=0;}),"SBBA domain hash failed");
  sblr::SblrExpressionNodeTableV1 table;
  sblr::SblrExpressionLiteralNodeV1 node;
  node.node_id=7; node.parent_operand_ordinal=1; node.descriptor_generation=1;
  node.descriptor_uuid=Identity(1); node.literal_body={1,0,0,0,0,0,0,0};
  table.nodes.push_back(node);
  const auto encoded=sblr::EncodeSblrExpressionNodeTableV1(table);
  Require(encoded.size()==165,"SBXN exact record size differs");
  const auto decoded=sblr::DecodeSblrExpressionNodeTableV1(encoded.data(),encoded.size());
  Require(decoded.ok && decoded.canonical_bytes==encoded,"SBXN canonical round trip failed");
  for(std::size_t offset: {std::size_t(0),std::size_t(4),std::size_t(6),std::size_t(12),std::size_t(24),std::size_t(32)}){
    auto malformed=encoded; malformed[offset]^=1;
    Require(!sblr::DecodeSblrExpressionNodeTableV1(malformed.data(),malformed.size()).ok,
            "malformed SBXN field was admitted");
  }
  auto trailing=encoded; trailing.push_back(0);
  Require(!sblr::DecodeSblrExpressionNodeTableV1(trailing.data(),trailing.size()).ok,
          "SBXN trailing byte was admitted");

  sblr::SblrExpressionNodeTableV1 maximum_table;
  maximum_table.nodes.reserve(4096);
  for (std::uint32_t ordinal = 1; ordinal <= 4096; ++ordinal) {
    auto maximum_node = node;
    maximum_node.node_id = ordinal;
    maximum_node.parent_operand_ordinal = ordinal;
    maximum_node.literal_body.assign(24, 0);
    maximum_table.nodes.push_back(std::move(maximum_node));
  }
  const auto maximum_sbxn =
      sblr::EncodeSblrExpressionNodeTableV1(maximum_table);
  Require(maximum_sbxn.size() == 610336 &&
              sblr::DecodeSblrExpressionNodeTableV1(
                  maximum_sbxn.data(), maximum_sbxn.size()).ok,
          "Core maximum-size SBXN was not admitted exactly");
  maximum_table.nodes.back().literal_body.push_back(0);
  Require(sblr::EncodeSblrExpressionNodeTableV1(maximum_table).empty(),
          "SBXN above the Core maximum byte extent was admitted");
  maximum_table.nodes.back().literal_body.pop_back();
  auto excess_node = maximum_table.nodes.back();
  excess_node.node_id = 4097;
  excess_node.parent_operand_ordinal = 4097;
  maximum_table.nodes.push_back(std::move(excess_node));
  Require(sblr::EncodeSblrExpressionNodeTableV1(maximum_table).empty(),
          "SBXN above the Core maximum node count was admitted");

  sblr::SblrOperationEnvelope carrier;
  carrier.operation_id="query.execute"; carrier.opcode="SBLR_QUERY_EXECUTE";
  carrier.opcode_code=0x1207; carrier.operation_version_major=1;
  carrier.result_shape="query_execute_result.v1";carrier.diagnostic_shape="engine.diagnostic.v1";carrier.trace_key="literal-test";
  carrier.parser_package_uuid=scratchbird::tests::FixtureUuidLiteral("019dffbb-f000-7000-8000-000000000001");
  carrier.registry_snapshot_uuid=scratchbird::tests::FixtureUuidLiteral("019dffbb-f000-7000-8000-000000000002");
  sblr::SblrOperand operand;operand.ordinal=1;operand.type="expression.node_table.v1";operand.name="expression_nodes";
  operand.value_kind=sblr::SblrValueKind::expression_node_table;operand.value_body=encoded;carrier.operands.push_back(operand);
  const auto table_hash=scratchbird::core::hash::ComputeSha256Digest(encoded);
  Require(table_hash.ok(),"SBXN hash failed");
  sblr::SblrOperand reference;reference.ordinal=2;reference.type="relational_expression_v1";reference.name="7";
  reference.value_kind=sblr::SblrValueKind::expression_node_ref;
  U16(&reference.value_body,1);U16(&reference.value_body,0);U32(&reference.value_body,1);U64(&reference.value_body,node.node_id);
  reference.value_body.insert(reference.value_body.end(),table_hash.digest.begin(),table_hash.digest.end());
  reference.value_body.insert(reference.value_body.end(),node.descriptor_uuid.begin(),node.descriptor_uuid.end());U64(&reference.value_body,1);
  carrier.operands.push_back(reference);
  Require(sblr::ValidateSblrEnvelope(carrier).ok,"exact query carrier refused");
  const auto canonical_carrier=sblr::EncodeSblrEnvelope(carrier);
  Require(!canonical_carrier.empty()&&
              sblr::DecodeSblrEnvelope(canonical_carrier).ok,
          "numeric kind17 canonical decode/re-encode failed");
  carrier.operands.back().name="07";
  Require(!sblr::ValidateSblrEnvelope(carrier).ok,"noncanonical numeric expression identity admitted");
  carrier.operands.back().name="7";
  auto numeric_legacy=carrier;
  numeric_legacy.operands.erase(numeric_legacy.operands.begin());
  numeric_legacy.operands.front().ordinal=1;
  numeric_legacy.operands.front().value_kind=sblr::SblrValueKind::literal_typed;
  numeric_legacy.operands.front().value_body.assign(24,0);
  numeric_legacy.operands.front().value_body[0]=1;
  Require(!sblr::ValidateSblrEnvelope(numeric_legacy).ok,
          "numeric operand name admitted for a legacy value kind");
  carrier.operation_id="engine.op.literal";carrier.opcode="SBLR_LITERAL";carrier.opcode_code=3;
  Require(!sblr::ValidateSblrEnvelope(carrier).ok,"top-level literal was admitted");
  return EXIT_SUCCESS;
}
