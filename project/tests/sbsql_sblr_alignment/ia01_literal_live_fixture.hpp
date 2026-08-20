#pragma once

#include "engine/sblr/sblr_literal_runtime.hpp"
#include "hash_digest.hpp"

namespace literal_fixture {
namespace runtime = scratchbird::engine::sblr;

struct Binding {
  Bytes sbxn;
  Bytes sbel;
  std::array<std::uint8_t, 16> descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
};

inline Submission BuildLiteralSubmission(
    const Fixture& fixture, const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid, Binding* binding) {
  Require(binding != nullptr, "literal submission binding missing");
  const auto package = RawUuid(view.bound_ast_uuid);
  auto member = sblr::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE",
                                      "ia01.literal.fault.query");
  member.opcode_code=0x1207;member.result_shape="query_execute_result";
  member.diagnostic_shape="diagnostic_vector";member.parser_package_uuid=parser_uuid;
  member.registry_snapshot_uuid=view.catalog_epoch_uuid;
  member.parser_resolved_names_to_uuids=true;
  const auto descriptor_text = [&]{
    constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(36);
    for(std::size_t i=0;i<16;++i){if(i==4||i==6||i==8||i==10)out.push_back('-');out.push_back(hex[binding->descriptor_uuid[i]>>4]);out.push_back(hex[binding->descriptor_uuid[i]&15]);}return out;}();
  const std::string descriptor_record = descriptor_text+
      "|019d0000-0000-7000-8000-00000000d712|1|-|-|-|-|-";
  sblr::SblrOperand descriptor;descriptor.ordinal=1;
  descriptor.type="relational_descriptor_v1";descriptor.name="slot_1";
  descriptor.value_kind=sblr::SblrValueKind::literal_typed;
  const auto type_uuid=RawUuid("019d0000-0000-7000-8000-00000000d712");
  descriptor.value_body.insert(descriptor.value_body.end(),type_uuid.begin(),type_uuid.end());
  U64(&descriptor.value_body,descriptor_record.size());
  descriptor.value_body.insert(descriptor.value_body.end(),descriptor_record.begin(),descriptor_record.end());
  member.operands.push_back(std::move(descriptor));
  sblr::SblrOperand table;table.ordinal=2;table.type="expression.node_table.v1";
  table.name="expression_nodes";table.value_kind=sblr::SblrValueKind::expression_node_table;
  table.value_body=binding->sbxn;member.operands.push_back(table);
  const auto table_hash=scratchbird::core::hash::ComputeSha256Digest(binding->sbxn).digest;
  sblr::SblrOperand ref;ref.ordinal=3;ref.type="relational_expression_v1";ref.name="7";
  ref.value_kind=sblr::SblrValueKind::expression_node_ref;
  U16(&ref.value_body,1);U16(&ref.value_body,0);U32(&ref.value_body,1);U64(&ref.value_body,7);
  ref.value_body.insert(ref.value_body.end(),table_hash.begin(),table_hash.end());
  ref.value_body.insert(ref.value_body.end(),binding->descriptor_uuid.begin(),binding->descriptor_uuid.end());
  U64(&ref.value_body,binding->descriptor_generation);member.operands.push_back(ref);
  const auto member_validation=sblr::ValidateSblrEnvelope(member);
  if(!member_validation.ok){for(const auto& diagnostic:member_validation.diagnostics)
    std::cerr<<"literal-member-validation:"<<diagnostic.code<<':'<<diagnostic.message<<'\n';}
  Require(member_validation.ok,"canonical literal member validation failed");
  sblr::SblrOpcodeStream package_stream;package_stream.package_descriptor_uuid=view.bound_ast_uuid;
  package_stream.registry_snapshot_uuid=view.catalog_epoch_uuid;
  package_stream.operations={Frame(true,parser_uuid,view.catalog_epoch_uuid,package),std::move(member),Frame(false,parser_uuid,view.catalog_epoch_uuid,package)};
  const auto stream=sblr::EncodeSblrOpcodeStream(package_stream);
  Require(!stream.empty(),"canonical literal SBOS encoding failed");
  const auto stream_sha=scratchbird::core::hash::ComputeSha256Digest(stream).digest;
  std::copy(stream_sha.begin(),stream_sha.end(),binding->sbel.begin()+144);
  // Reuse the production-boundary container/execution builder, replacing only
  // its already canonical SBOS payload and corresponding CRC/length fields.
  auto base=BuildSubmission(fixture,view,parser_uuid);
  auto decoded_container=wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(base.container.data()),base.container.size());
  Require(decoded_container.status==wire::SblrCodecStatus::ok,"fixture base container decode failed");
  decoded_container.container.operation_payload=stream;
  const auto outer=wire::EncodeSblrContainer(decoded_container.container);
  auto execution=wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(base.ingress.data()),base.ingress.size());
  Require(execution.status==wire::SblrCodecStatus::ok,"fixture base ingress decode failed");
  auto& fields=execution.envelope.fields;
  fields[5]={1};U64(&fields[5],stream.size());fields[5].insert(fields[5].end(),stream.begin(),stream.end());
  fields[7]={1};U32(&fields[7],wire::SblrCrc32c(stream.data(),stream.size()));fields[8]=V64(stream.size());
  const auto ingress=wire::EncodeSblrExecutionEnvelopeV1(execution.envelope);
  return {{outer.begin(),outer.end()},{ingress.begin(),ingress.end()},stream};
}

inline Binding FinalizeLiteral(bridge::StatementContextReceiptHandle receipt,
                               const bridge::StatementContextReceiptView& view) {
  runtime::SblrLiteralPrebindRequestV1 request;
  request.preliminary_receipt_uuid = RawUuid(view.receipt_uuid);
  request.catalog_snapshot_uuid = RawUuid(view.literal_catalog_snapshot_uuid);
  request.catalog_generation = view.literal_catalog_generation;
  request.security_epoch = view.security_epoch;
  request.resource_epoch = view.resource_epoch;
  request.mga_snapshot_uuid = RawUuid(view.statement_snapshot_uuid);
  runtime::SblrLiteralDemandV1 demand;
  demand.occurrence_id = 1;
  demand.lexical_class = 1;
  demand.context_class = 1;
  const Bytes lexical{'1'};
  demand.lexical_sha256 = scratchbird::core::hash::ComputeSha256Digest(lexical).digest;
  request.demands.push_back(demand);
  request.demand_sha256 = runtime::ComputeSblrLiteralDemandSequenceSha256V1(request.demands);
  const auto sbln = runtime::EncodeSblrLiteralPrebindRequestV1(request);
  Bytes sblq;
  sb_engine_result_t result = nullptr;
  Require(bridge::NegotiateStatementLiteralDescriptorsV1(
              receipt, sbln, &sblq, &result) == SB_ENGINE_STATUS_OK,
          "live literal descriptor negotiation failed");
  if (result) (void)sb_engine_result_release(result);
  Require(sblq.size()==356 && std::equal(sblq.begin(),sblq.begin()+4,"SBLQ") &&
              scratchbird::engine::SblrReadU32(sblq.data()+120)==1 &&
              scratchbird::engine::SblrReadU64(sblq.data()+160)==1 &&
              scratchbird::engine::SblrReadU32(sblq.data()+168)==184,
          "live literal descriptor result was not canonical");
  runtime::SblrLiteralPrebindResultV1 negotiated;
  std::copy_n(sblq.begin()+128,32,negotiated.ordered_profile_sha256.begin());
  runtime::SblrLiteralProfileMappingV1 mapping;
  mapping.occurrence_id=1;mapping.sblp_bytes.assign(sblq.begin()+172,sblq.end());
  negotiated.mappings.push_back(mapping);
  const auto profile = runtime::DecodeSblrLiteralDescriptorProfileV1(
      negotiated.mappings[0].sblp_bytes.data(),
      negotiated.mappings[0].sblp_bytes.size());
  Require(profile.ok, "live literal descriptor profile was not canonical");

  runtime::SblrExpressionNodeTableV1 table;
  runtime::SblrExpressionLiteralNodeV1 node;
  node.node_id = 7;
  node.parent_operand_ordinal = 1;
  node.descriptor_uuid = profile.profile.descriptor_uuid;
  node.descriptor_generation = profile.profile.descriptor_generation;
  const auto literal_body=runtime::EncodeSblrLiteralInt64LeV1(1);
  node.literal_body.assign(literal_body.begin(),literal_body.end());
  table.nodes.push_back(node);
  const auto sbxn = runtime::EncodeSblrExpressionNodeTableV1(table);
  const auto sbxn_sha = scratchbird::core::hash::ComputeSha256Digest(sbxn).digest;

  runtime::SblrLiteralBoundAstV1 bound;
  bound.preliminary_receipt_uuid = request.preliminary_receipt_uuid;
  bound.demand_sha256 = request.demand_sha256;
  runtime::SblrLiteralBoundAstNodeV1 ast;
  ast.parent_operand_ordinal = 1;
  ast.node_id = 7;
  ast.descriptor_uuid = profile.profile.descriptor_uuid;
  ast.descriptor_generation = profile.profile.descriptor_generation;
  ast.type_uuid = profile.profile.type_uuid;
  ast.profile_uuid = profile.profile.profile_uuid;
  ast.occurrence_id = 1;
  ast.lexical_sha256 = demand.lexical_sha256;
  bound.nodes.push_back(ast);
  const auto sbba = runtime::EncodeSblrLiteralBoundAstV1(bound);
  const auto bound_sha = runtime::ComputeSblrLiteralBoundAstSha256V1(sbba);

  Bytes sblf(208, 0);
  std::copy_n("SBLF", 4, sblf.begin());
  auto store16 = [&](std::size_t o, std::uint16_t v) { sblf[o]=v; sblf[o+1]=v>>8; };
  auto store32 = [&](std::size_t o, std::uint32_t v) { for(unsigned i=0;i<4;++i)sblf[o+i]=v>>(8*i); };
  auto store64 = [&](std::size_t o, std::uint64_t v) { for(unsigned i=0;i<8;++i)sblf[o+i]=v>>(8*i); };
  store16(4,1); store16(6,208); store32(8,static_cast<std::uint32_t>(208+sbba.size()+sbxn.size()));
  std::copy(request.preliminary_receipt_uuid.begin(),request.preliminary_receipt_uuid.end(),sblf.begin()+16);
  std::copy(request.demand_sha256.begin(),request.demand_sha256.end(),sblf.begin()+32);
  std::copy(negotiated.ordered_profile_sha256.begin(),negotiated.ordered_profile_sha256.end(),sblf.begin()+64);
  std::copy(bound_sha.begin(),bound_sha.end(),sblf.begin()+96);
  std::copy(sbxn_sha.begin(),sbxn_sha.end(),sblf.begin()+128);
  store64(160,request.catalog_generation); store64(168,request.security_epoch); store64(176,request.resource_epoch);
  std::copy(request.mga_snapshot_uuid.begin(),request.mga_snapshot_uuid.end(),sblf.begin()+184);
  store32(200,static_cast<std::uint32_t>(sbba.size())); store32(204,static_cast<std::uint32_t>(sbxn.size()));
  sblf.insert(sblf.end(),sbba.begin(),sbba.end()); sblf.insert(sblf.end(),sbxn.begin(),sbxn.end());
  Bytes sbla;
  result = nullptr;
  Require(bridge::FinalizeStatementLiteralBindingV1(receipt,sblf,&sbla,&result)==SB_ENGINE_STATUS_OK,
          "live literal binding finalize failed");
  if(result)(void)sb_engine_result_release(result);
  Require(sbla.size()==264,"live literal admission record size differed");
  Binding binding; binding.sbxn=sbxn; binding.descriptor_uuid=profile.profile.descriptor_uuid;
  binding.descriptor_generation=profile.profile.descriptor_generation;
  binding.sbel.assign(176,0); std::copy_n("SBEL",4,binding.sbel.begin());
  auto b16=[&](std::size_t o,std::uint16_t v){binding.sbel[o]=v;binding.sbel[o+1]=v>>8;};
  auto b32=[&](std::size_t o,std::uint32_t v){for(unsigned i=0;i<4;++i)binding.sbel[o+i]=v>>(8*i);};
  b16(4,1);b16(6,176);b32(8,176);
  std::copy_n(sbla.begin()+32,16,binding.sbel.begin()+16);
  std::copy_n(sbla.begin()+48,16,binding.sbel.begin()+32);
  std::copy_n(sbla.begin()+232,32,binding.sbel.begin()+48);
  std::copy(bound_sha.begin(),bound_sha.end(),binding.sbel.begin()+80);
  std::copy(sbxn_sha.begin(),sbxn_sha.end(),binding.sbel.begin()+112);
  return binding;
}
}  // namespace literal_fixture
