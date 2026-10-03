// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_date.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <type_traits>
namespace { namespace dt=scratchbird::core::datatypes; namespace p=scratchbird::core::platform; unsigned checks=0;
[[noreturn]] void Fail(std::string_view s){std::cerr<<"FAIL: "<<s<<'\n';std::exit(1);} void Check(bool v,std::string_view s){++checks;if(!v)Fail(s);}
std::set<std::string_view> executed_rows,boundary_rows;
void Mark(std::initializer_list<std::string_view> ids){for(auto id:ids)Check(executed_rows.insert(id).second,"sealed row executed exactly once");}
p::Uuid D707(){return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x07}};}
std::shared_ptr<const dt::DateValidatedProfileHandleV1> Profile(){auto r=dt::BuildCurrentDateValidatedProfileHandleV1(D707());Check(r.ok(),"profile");return std::make_shared<dt::DateValidatedProfileHandleV1>(std::move(r.profile));}
dt::DateValueViewV1 V(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& p,int32_t d){return {p.get(),dt::DateValueStateV1::value,d};}
dt::DateOwnedValueV1 O(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& p,int32_t d,dt::DateValueStateV1 state=dt::DateValueStateV1::value){return {p,state,d};}
const dt::DatatypeTypeCodecIdentityRowV3* TextIdentity(){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.catalog_snapshot_uuid==D707()&&row.legacy_fields.catalog_generation==7&&row.legacy_fields.registry_generation==7&&row.legacy_fields.canonical_binary_type_code==static_cast<uint32_t>(dt::CanonicalTypeId::character))return &row;return nullptr;}
scratchbird::engine::ExecutionTypeDescriptor TextDescriptor(){auto m=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(m.ok(),"text catalog manifest");auto row=dt::LookupDatatypeCatalogRow(m.manifest,dt::CanonicalTypeId::character);Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"text catalog row");dt::CatalogExecutionTypeMetadata md;md.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;md.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;auto d=dt::LookupExecutionTypeDescriptorFromCatalog(dt::CanonicalTypeId::character,md);Check(d.ok(),"text descriptor");return d.descriptor;}
struct Cancel{unsigned calls=0,at=0;};bool Stop(void* p) noexcept {auto& c=*static_cast<Cancel*>(p);return ++c.calls==c.at;}
struct SealedIntrinsicRow{std::string_view case_id,operation,outcome,row_sha256;};
constexpr SealedIntrinsicRow kSealedIntrinsicRows[]{
{"construct_epoch","civil_construct","VALUE","073f1d2b50543398f10e78248a97badb0a03898df541bec79867e59fb3a9b33d"},
{"construct_year_zero_leap","civil_construct","VALUE","a64c7a78796c868f9fcb7038426dc30470fe6955ef5b6b93207d7ad4b6ad8255"},
{"construct_negative_leap","civil_construct","VALUE","6672ae37ec021831cacec8d4fbb57b9e76672d7cc7cebfd0df255629174fd4fd"},
{"construct_1900_common_refusal","civil_construct","CTI.TEMPORAL.INVALID_LITERAL","953f3d29fc1503de81d4302c70a2875151485736ca8b3e8fc09cc742dcb5a4b4"},
{"construct_2000_leap","civil_construct","VALUE","2b5ec6b11b0c71ef2314606683a116e92c3b0fbe15aa99f4ec4465f07ec3783b"},
{"construct_zero_month","civil_construct","CTI.TEMPORAL.ZERO_DATE_REFUSED","6cf35c903dda65689c20250fad04a995d2de3b0d5255a3289c56d80a4ee35277"},
{"construct_null_component","civil_construct","SQL_NULL","da192773a8c5c1f44bf5d17fe37293935a5de076683744cf09810aa6ab313fb7"},
{"decompose_epoch","decompose_civil","VALUE","7ce88c011b427d29a94b9509c6e7eada7adad2215a8cd5ab8d928a48353c9763"},
{"add_max_boundary","add_days","VALUE","52b990f1b6f35522c00dbdd5b8f6fee718477d5c87c594a45c5da0df82101d5d"},
{"add_overflow","add_days","CTI.TEMPORAL.RANGE_EXCEEDED","2b118b760c5c6d650041d86e81c7c9e619d7baa53d2d45a3eb37d89cfa9c3a94"},
{"add_i64_max","add_days","CTI.TEMPORAL.RANGE_EXCEEDED","7e891b09f15f294768575035c9dba254e48b5877f86feae5a291934ccd0ddcee"},
{"subtract_min_boundary","subtract_days","VALUE","b973f1aed2a61d4d899d2ef359394bd1d194b5374e2ff6fcd49685224f02799c"},
{"subtract_underflow","subtract_days","CTI.TEMPORAL.RANGE_EXCEEDED","705f8fda34bd46ce10575b69483e8e193dfcd800a175c338b30a7d3326042752"},
{"subtract_i64_min","subtract_days","CTI.TEMPORAL.RANGE_EXCEEDED","90abadea623b039fdef8f10b6e7c753e210a448c3d9b3e7cd9886abfa16f89e2"},
{"successor_max_refusal","successor","CTI.TEMPORAL.RANGE_EXCEEDED","5b83387416adad0624040c084f010e8dce3a0be1f19ef9cd520af9d316281cbc"},
{"predecessor_min_refusal","predecessor","CTI.TEMPORAL.RANGE_EXCEEDED","abeedfe5fe765398da2797b62aa193b476ed308367eb634d08a8d63c4a8a5493"},
{"difference_full_positive","difference_days","VALUE","4cb78f429597e78b468cc27f8dabbc270597d10542b2c97f3c4ec78a28ca66b0"},
{"difference_full_negative","difference_days","VALUE","cea79ee43728dae8f0f0e4335151ebff7f8d738956924e9fd8216cc493d01420"},
{"weekday_epoch","iso_weekday","VALUE","9159113e659cd210c26284171535b1ce73bcb575b8d3604093c98838d11e0bf0"},
{"weekday_before_epoch","iso_weekday","VALUE","7d9f41cc4977cdc820de48b338be4a45500185a0b73a9468033dc41cbc280fb5"},
{"leap_negative_400","is_leap_year","VALUE","45c35d26205281ed7f92075685b04889dbc7c72e2a31a566e4e4bcb62b913464"},
{"leap_zero","is_leap_year","VALUE","c4921e1fc1fcc3e3af8d97f859c414c54cf8c0a4b82f0c6897e5aa747096376f"},
{"days_feb_1900","days_in_month","VALUE","e98eeac48dfd151c5cf8791c5dcce2fbe5352f660cfbc77d6a4988726715a261"},
{"days_feb_2000","days_in_month","VALUE","59c7be41c749e49fd910f156a8a3d0a7c909415eb0a830a0b245c93aa7c5339b"},
{"day_of_year_leap","day_of_year","VALUE","9fb04f89efa70ffb067e850480bd4b279634a97633bf0b4a93eac607e581cfc3"},
{"quarter_q4","quarter","VALUE","9fb174356bca2037d72f0b111eb530df386ef2138283f1ea3aef40fc28fc73c2"},
{"iso_late_dec_next_year","iso_week","VALUE","1739c9cadd691fa1d0502c6780e2dbc970e56d6e098ac0df5614299284a204c6"},
{"iso_early_jan_prior_year","iso_week","VALUE","f6c1e5da986cb64d9a20c478dcead3000063b7634b0242f6592e49dc80543b8f"},
{"iso_year_zero_boundary","iso_week","VALUE","287d7e229f1ebacaaabcc2f5d301ed52351162c30ee5e5ca97f694cba448c41e"},
{"truncate_identity","truncate_day","VALUE","d17315ab9a6f7630e5e9c8e3bb9e6f5566e3d43ae4cdfdc922b0f7a4c1214737"},
{"round_null","round_day","SQL_NULL","4d6ab45fc1ea6039db704b58b54c7f96c169ff5a3fa5699031952cba5b141d33"},
{"dirty_null_refusal","add_days","DATATYPE.NULL_STATE.INVALID","618064512861e4528b851913017154099732c87d6f24213b254c3324f6178b02"},
{"unsupported_no_payload_inspection","calendar_interval_or_cross_temporal","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","f94b94ffe20b48662365670c1c9623f8a1a9a62fbb7d668c3e9a09939514f1a2"},
{"cancellation_atomic","add_days","PROCESS.CANCELLED","278e69e8f72376438677f9a13ada514f84697095967eb1090a5fd992ed1a3085"},
{"validate_value","validate_canonicalize","VALUE","fe36c4abc11bfda57c504f65f9bc6d3b09a0a798bdee1faf69e1b130f432b0e8"},
{"parse_epoch","parse_canonical","VALUE","27306f8bf889a850fba392f4ea09973e0e7ff8cf721f26278b865d1b04aca6c7"},
{"parse_expanded_alias_refusal","parse_canonical","CTI.TEMPORAL.INVALID_LITERAL","7d1902d437f4494281c4c0ed8f91d0ba05aa1a6266754077823bcaf79fc70912"},
{"render_standard","render_canonical","VALUE","3afe7077d939480d1d8a4f30a898f01cee5fbf671a9a3cb48ab6ddb26743e4c7"},
{"larger_truncate_refused","larger_truncate_round_or_bucket","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","45396e778b1a30cf493e08390eb15f00d1e529e702847bc5450a4a337b35b87f"},
{"aggregate_dispatch_handoff","aggregate_min_max_count_dispatch","receiving_owner_handoff","e040df83a00576b60a7a5cd5584ed2ca05a02267a347363135e0e3c644acf847"},
{"unknown_operation_refused","unregistered.operation","CTI.TEMPORAL.OPERATION_REFUSED","f9ba9e95dc2ff8f989d54ae602732d595ac7d0b8715eaca80935741a8e7da0ed"},
{"known_refused_clean_null","calendar_interval_or_cross_temporal","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","fa3da1b8eb29bfbcb3f1a7c1f7147b0a449013d5b25e4977ff13b9793468557e"},
{"dirty_state_plus_unknown","unregistered.operation","DATATYPE.NULL_STATE.INVALID","daa0ea9c111fc805852e65fde40c5344ca1da22ffac77bc34c54ee0cf6d82a1d"},
{"dirty_state_plus_known_refused","calendar_interval_or_cross_temporal","DATATYPE.NULL_STATE.INVALID","496fe78174c89efc947fcd26c0f9848578c1df0d0978cb350636a7839a2f02b9"},
{"clean_null_plus_supported","successor","SQL_NULL","810737ccc84e88b96d7f8b1969227d2ff3e155abc1cdf7afaee66002729ab3f1"},
{"unknown_clean_state","unregistered.operation","CTI.TEMPORAL.OPERATION_REFUSED","39e423a6b3c35177035c45b73aa10598619f3f8defa97fa1988a01a8ea5b4544"},
{"known_refused_clean_state","larger_truncate_round_or_bucket","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","da4afcccf40d59295140aa260efb69ffd6a95436d26e255783cbf96a91f1bdcc"},
{"operation_profile_mismatch","successor","CTI.TEMPORAL.DESCRIPTOR_INVALID","9d5852c8549ac93fb49683b2a9efdae2e41e46bfd2eb5005513c31beddb7bad5"},
{"operation_profile_missing","successor","CTI.TEMPORAL.DESCRIPTOR_INVALID","dd140825f5a0b9d71ba049c95d27aa519a3cb9614e0e86747d4fe962a0ac1ed3"},
{"clean_null_decompose","decompose_civil","SQL_NULL","2268a7de91ae844e50eec6c05bcf8954ced49737e10744218707b5314a653069"},
{"clean_null_render","render_canonical","SQL_NULL","e255668adf1d9a5080e853b3d44012b5094a5dbae8d25ab00d075b568e299da3"},
{"clean_null_difference","difference_days","SQL_NULL","f213a7c044d8e36a850d732da05094f5c914fa91a94fbf00451674dc43006e2c"},
{"clean_null_u8_fact","is_leap_year","SQL_NULL","bcc040974cebb04561a040f4a9c66a0af5aad32394475006f98aadb2d1c5102d"},
{"clean_null_u16_fact","day_of_year","SQL_NULL","31efcf1d77728c9ea9aadc661d8d8413c108d000134ddedb146725120740e116"},
{"clean_null_iso_week","iso_week","SQL_NULL","334c8ce09172ad8b2379815c5ff5a5891f1a8a8e4a730d8495f4971eca5ce505"},
{"render_expanded_14","render_canonical","VALUE","42c467399d72306cdd6ce74a8d791ed56cd1ea488e37fd9a6da107c3f1e00e96"},
{"render_expanded_one_byte_short","render_canonical","CTB.TEXT.LENGTH_EXCEEDED","b51bc4a3656cc744f02aab3c4316ef246e8230b6516dc8fa1b131eb0b0b58845"},
{"render_cancellation","render_canonical","PROCESS.CANCELLED","72d3aa659d8151f4401461bc6a1f17f00767c16996f7f1d8795bffbefad3e700"},
{"shared_handle_presentation_alias","validate_canonicalize","VALUE","f9a7917b0f59852f0a894b76bda18f97a7c911a1c66f08a559c5c22bac7f9235"},
{"shared_handle_name_only_refused","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6ed8fca00f7ccee199a76e1390b84db08a3c2ad3c5915363f5be5621770f09c7"},
{"shared_handle_mut_profile_material_magic_ASCII_SBDATP01_offset_0","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","53e24255e8269ffe8dc1d9d3ba738589e5311c030b5496db4cb0e37a591cb81e"},
{"shared_handle_mut_profile_material_version_u16_le_1_offset_8","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","420bcd4a363dcb055bc4de6965539a6480d5d986685284014fc9cb46a92d9f4c"},
{"shared_handle_mut_profile_material_material_bytes_u16_le_584_offset_10","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9cd509f884fc8cc9bca7d55598e98a416dd3b48e5e0572528e7f4ba8e03ac800"},
{"shared_handle_mut_profile_material_total_bytes_u32_le_584_offset_12","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","51da8e24a2900e8c5a3753315fc537191816ed60b317c55a0be9e5932e6d8870"},
{"shared_handle_mut_profile_material_snapshot_uuid_raw16_offset_16","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","08cc4ec221a1fbc5b10621d35ca5e68c071b988309ad178a8132a042e01d3387"},
{"shared_handle_mut_profile_material_catalog_generation_u64_le_offset_32","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","b41d34fba5c6b1d99cda56cdaf9dadf4d8dff3bf8793b89afdd7cc04a361e780"},
{"shared_handle_mut_profile_material_registry_generation_u64_le_offset_40","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","29d66e5e1f40a26e9cb47a01ed4b5167f499f9614d7acbea8e48ed700f27ac1c"},
{"shared_handle_mut_profile_material_descriptor_uuid_raw16_offset_48","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f4c2ac1762f8207f509a1592419aad728e1c295a63e01c0cda7423de47e9438c"},
{"shared_handle_mut_profile_material_descriptor_generation_u64_le_offset_64","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4da4bd58f9b202d1054221cb5e5ba288d145a7ee94728ba3b9a849ca64e19e61"},
{"shared_handle_mut_profile_material_type_uuid_raw16_offset_72","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","50380e821e556fb6fdbe7ff5970fae3a6d111784b25f66b45d10398b33f4fb88"},
{"shared_handle_mut_profile_material_type_generation_u64_le_offset_88","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","720c220ab4b0db408c69c2a5d6c644ab3f9c37ec47ebb29c519b7987f9ddc069"},
{"shared_handle_mut_profile_material_codec_uuid_raw16_offset_96","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1414903f87c5512cca9c4875eeea61b658f9b2643463e246867f8d2179c0de23"},
{"shared_handle_mut_profile_material_codec_version_u32_le_offset_112","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","3866140d401e9e0242628c9f1414d99177abc9d91a778055fde209c5b85f5c10"},
{"shared_handle_mut_profile_material_reserved_zero_offset_116","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","3d97a119e77a66b06d12a482e8d12192cb39de09d11b5103c3600d2b341e616e"},
{"shared_handle_mut_profile_material_codec_generation_u64_le_offset_120","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1c96b839c959f25f53cc6b3f82f48f4f865fd9c1cb5d036af114e7fb4ab9abfa"},
{"shared_handle_mut_profile_material_descriptor_policy_uuid_raw16_offset_128","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f8f30a7c8ad393a196af1e357cd731254e89131520de39c2283818b1d85ebcfc"},
{"shared_handle_mut_profile_material_descriptor_policy_generation_u64_le_offset_144","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","62c878655afbb136ca95ef67e441edbfd4d26049476964c4e649a1613ff242ac"},
{"shared_handle_mut_profile_material_canonicalization_policy_uuid_raw16_offset_152","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","fa4d9de04f0488c33f1d8b7284ffa82f1e7ea3653f8427c9c2857e12b63ae47a"},
{"shared_handle_mut_profile_material_canonicalization_policy_generation_u64_le_offset_168","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","cbedb39b79307a15b55b9606614d1a765ca480ca3724bc70dcaf50fd73573faf"},
{"shared_handle_mut_profile_material_ordering_policy_uuid_raw16_offset_176","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a083cd53446cd1aafa6e9813e2e040fb1861daf03d4179c24aac5d4573f8a62b"},
{"shared_handle_mut_profile_material_ordering_policy_generation_u64_le_offset_192","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ffc8421b3e99e2bee0208c86755cff6aa3d48f71b4a4469e397fa355c9d7d558"},
{"shared_handle_mut_profile_material_hash_policy_uuid_raw16_offset_200","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","d83588afecef0d3c514914dd5810ce846bf528e839f1ded1604d87939b00ce04"},
{"shared_handle_mut_profile_material_hash_policy_generation_u64_le_offset_216","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","0afcaee73ac16bbdf3d05c08ab2b8dddc9a587a7ea9d55193abeced53427d730"},
{"shared_handle_mut_profile_material_operation_policy_uuid_raw16_offset_224","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","0047eed32aa0d472bde554cc282ce8baaa29c8d263654ba6d3fe8a2af1f28fd9"},
{"shared_handle_mut_profile_material_operation_policy_generation_u64_le_offset_240","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7a180917d456cb34448c7b9e85da1c653ed711c1a46fd6f63edc16fe878a4243"},
{"shared_handle_mut_profile_material_render_policy_uuid_raw16_offset_248","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7dd1718b8d64b8bb3e3dfa286b7d297c410aadefb7e6fd48248ef0dd83db6b34"},
{"shared_handle_mut_profile_material_render_policy_generation_u64_le_offset_264","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","b277331e2bbb828166ec2203ce719bf607f21a82fe1766120f794a9607cf3692"},
{"shared_handle_mut_profile_material_cast_policy_uuid_raw16_offset_272","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ec3b9c18691888fa967c433d6fb77e2b60eedd3523e53e29ee783a734eff6836"},
{"shared_handle_mut_profile_material_cast_policy_generation_u64_le_offset_288","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a1fe9d244f6e3b76c7ff71f02acd47c064736b3814b9d583d89c6e01cd4a080f"},
{"shared_handle_mut_profile_material_calendar_policy_uuid_raw16_offset_296","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","39069dea9dbd815d899ffe651572921df8e0c1652c545eeee5007853b83c47c5"},
{"shared_handle_mut_profile_material_calendar_policy_generation_u64_le_offset_312","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","0c386218133e486ebf2080ab27b57ad624f473748768a311b7a34bcc1c2873d1"},
{"shared_handle_mut_profile_material_storage_epoch_policy_uuid_raw16_offset_320","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","304c93e854964cdc0c236ca9284d304c4f05a1dc3453ca7e229be55dd5d2e3a3"},
{"shared_handle_mut_profile_material_storage_epoch_policy_generation_u64_le_offset_336","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","23cf3c25dc26e63725b054078453cdc6ac6151bcaa1a2b764ecb289364d8058c"},
{"shared_handle_mut_profile_material_timezone_none_policy_uuid_raw16_offset_344","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","135206b28434e40250c1028ecef97de75bccd169fe32a4c630870c58a01b1542"},
{"shared_handle_mut_profile_material_timezone_none_policy_generation_u64_le_offset_360","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","010bfc9ad725eaf69997139e9a294f5d54165950628d9e10eb5cc4a1c4442066"},
{"shared_handle_mut_profile_material_leap_na_policy_uuid_raw16_offset_368","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","d5cd2f3095b96022ccf1de41ffcda21823e957a80d153a4a9f10488f2df71952"},
{"shared_handle_mut_profile_material_leap_na_policy_generation_u64_le_offset_384","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2d4e51a7e3f32266af2033bf75efb348d31133fc0df96649e0ce8f17538a421f"},
{"shared_handle_mut_profile_material_index_policy_uuid_raw16_offset_392","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","527ec48339b17eba86b0f8a2c76e3512324ed81e4cefb0f0796eda726324dd11"},
{"shared_handle_mut_profile_material_index_policy_generation_u64_le_offset_408","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","12ce5874f41d555f4e4e48d0e6363c7a581b9690c4985f3aa670025c0100da95"},
{"shared_handle_mut_profile_material_statistics_policy_uuid_raw16_offset_416","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","46411838866b201c0d9d9692b6574faf833187a72adc0ee5342febb8323cc9b4"},
{"shared_handle_mut_profile_material_statistics_policy_generation_u64_le_offset_432","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9c6e3c361902d2ac17c89267ea5de8b7cf6628c220f3101d6355d6e791d09edf"},
{"shared_handle_mut_profile_material_backup_policy_uuid_raw16_offset_440","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6129ce5f4b2d040dea2640be80bc7a0af161be1ddeb9ae92a1c9dfc52360dba9"},
{"shared_handle_mut_profile_material_backup_policy_generation_u64_le_offset_456","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","89a598d7a659278eeabc504c484be8b651253ab59c877999c15f52893ffcfc89"},
{"shared_handle_mut_profile_material_protection_policy_uuid_raw16_offset_464","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f28d1aa2d0ed554dc4bdc89092934b61e6abb3502f86a0567ae78630d0003f89"},
{"shared_handle_mut_profile_material_protection_policy_generation_u64_le_offset_480","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8a0987cf0f1e1dca87d11ad9944e999f166c69ca29f8c92654dca185771bf0c3"},
{"shared_handle_mut_profile_material_component_policy_uuid_raw16_offset_488","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1fa06e9614eebea1d5ca0eb99b3ea27767db6767ac4256aed042a006e124ada0"},
{"shared_handle_mut_profile_material_component_policy_generation_u64_le_offset_504","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7d77e84842443d3dfe2bf96b39eb1e2661fba09e887f14c0bc7e3b2f56fc502d"},
{"shared_handle_mut_profile_material_diagnostic_policy_uuid_raw16_offset_512","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","e558fe9e63bc2f239e7aafc1d6fb41e924907185b142b7b81216d422158f613f"},
{"shared_handle_mut_profile_material_diagnostic_policy_generation_u64_le_offset_528","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","893e1bdc757258d3b1cd80dead8e91e3cc86a34448f5f2d67c402699663bda03"},
{"shared_handle_mut_profile_material_metric_policy_uuid_raw16_offset_536","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a612d3cb78d5ce582082b6cbe586034a54bcad9f7c5cd6f16be97dabaf410aee"},
{"shared_handle_mut_profile_material_metric_policy_generation_u64_le_offset_552","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","fabbfe7271ed62ea1d6cea65debb32a4b5aa0b8c160608123188cf32b82cb0e5"},
{"shared_handle_mut_profile_material_minimum_day_i32_le_offset_560","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","49ed951305770caebdadcac2e5b88b27076bd04c590240d476adfa2f0fb59355"},
{"shared_handle_mut_profile_material_maximum_day_i32_le_offset_564","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","618269aa0b176b5cb2f74398d8ad88171ae1dabdb0a117fcdece64e364de6794"},
{"shared_handle_mut_profile_material_epoch_day_i32_le_offset_568","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","77295a0aece92a6ad367ad943e2fc9255c8693dd65e3a11b223ef2bc08389591"},
{"shared_handle_mut_profile_material_component_bytes_u32_le_offset_572","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","d0aca9f2a6f8322ad14ca1637b529dbb059072513973548c4ef332734394913b"},
{"shared_handle_mut_profile_material_flags_u32_le_offset_576","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","3a2cb002d43da7231c4088ca509d33acdf24db32d3a0c8195714667a88547cb7"},
{"shared_handle_mut_profile_material_reserved_zero_offset_580","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","27762ffd135f2ecad59f28dc8429cc60f7e47f32ba45192385155466f3e45d97"},
{"shared_handle_mut_comparison_material_magic_ASCII_SBDACC01_offset_0","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7f7cbfd8f0a62589469e59739daed323180e43b1a7e72acb1e58d129b219eaf9"},
{"shared_handle_mut_comparison_material_version_u16_le_1_offset_8","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ff8a9f58769bbbe10ec887ce9ca99ad4149a835dcbde98ea5954e3f9d86112be"},
{"shared_handle_mut_comparison_material_material_bytes_u16_le_344_offset_10","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","31b1d560a82d61f1666c000f643da3eb4dd803eaf00314cb295b95f63e06b9da"},
{"shared_handle_mut_comparison_material_reserved_zero_offset_12","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6e2f34b1f22f1d045df16a0546b680d78aa9b676f34fdf8308f17fba72f63170"},
{"shared_handle_mut_comparison_material_snapshot_uuid_raw16_offset_16","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","bb1cb1c7f2813c21bb7eb4dd0e5f795da078c33ab34b174cd481ec59077355e9"},
{"shared_handle_mut_comparison_material_catalog_generation_u64_le_offset_32","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","38cfbcbdb40a3f860d3313e3d7fd54cb8085b757660aaf98ee7857c142b0cbe6"},
{"shared_handle_mut_comparison_material_registry_generation_u64_le_offset_40","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","644e619d561c3762abb07293f46f0e5ec7abb5cd6f364a489f273a86b2ad1f73"},
{"shared_handle_mut_comparison_material_descriptor_uuid_raw16_offset_48","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7abf68204d99178b690d40ce837cbb88779c1dbf10a3be3097f3c932d4e37d27"},
{"shared_handle_mut_comparison_material_descriptor_generation_u64_le_offset_64","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c5349c0df4352f363b3ae6eb5f725650a629c7def06ac2a9d44b42cab24649cc"},
{"shared_handle_mut_comparison_material_type_uuid_raw16_offset_72","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","84c728beb5d75ad465f9edddc1f123514269e97ec92c69bbac39895b8145c240"},
{"shared_handle_mut_comparison_material_type_generation_u64_le_offset_88","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1f2e605b93b994bde1820d41e07e0d30a011455446e253d8a4deb463c134f6a0"},
{"shared_handle_mut_comparison_material_codec_uuid_raw16_offset_96","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ea85884807001f6f3402bdbb4ebc49f7bf642f7ce8e9022ce85bfc39a6764d33"},
{"shared_handle_mut_comparison_material_codec_version_u32_le_offset_112","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4f58305592b037ae4d57bbf597ab53ff19f05e221250e2eabe85fa0ba5088388"},
{"shared_handle_mut_comparison_material_reserved_zero_offset_116","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4d5528ca6e485c27ed72467c62f3fe17d2e32471db82cb4aa3a3ff2eab7dcaab"},
{"shared_handle_mut_comparison_material_codec_generation_u64_le_offset_120","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","0bb279432c0b942271de73359c1f8c639dc4943f0da89d7b99a600e2a61b79fc"},
{"shared_handle_mut_comparison_material_descriptor_policy_uuid_raw16_offset_128","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a1d8c11eb3a3ab2e2b3551615ca53af24ccfeb09ac4b86bc932caae0289a261f"},
{"shared_handle_mut_comparison_material_descriptor_policy_generation_u64_le_offset_144","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c73b6bf1f64acebe9604733dd8f5bbd6731f96f05bfbc6e21e387c20bd658f58"},
{"shared_handle_mut_comparison_material_canonicalization_policy_uuid_raw16_offset_152","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2269f5357cb6124d96473376648ad9fd7e7606032be49cb6fefd0b9f634e29dd"},
{"shared_handle_mut_comparison_material_canonicalization_policy_generation_u64_le_offset_168","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","b4c9c1de02a2e54a0f791e9ec2c371878331f87be82749ba4c43cee6a2758103"},
{"shared_handle_mut_comparison_material_ordering_policy_uuid_raw16_offset_176","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","553e4e50540ce17a011b5345f9f80dbd10aab1cad3ba7daf2fba5742719f3a22"},
{"shared_handle_mut_comparison_material_ordering_policy_generation_u64_le_offset_192","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5a7191f31baef5321eb0454bd0d1df786e5c5714bcd904b98fa89c7dca10bc55"},
{"shared_handle_mut_comparison_material_calendar_policy_uuid_raw16_offset_200","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","54392e9ce0d6f0bc96c2d699b41151bf3c2192a212df387d74157c8e72aceccc"},
{"shared_handle_mut_comparison_material_calendar_policy_generation_u64_le_offset_216","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6944d9e8a043f00aefb8241a75f3fbdd8d1d0699a647d3bfa452b9df8f7c09a0"},
{"shared_handle_mut_comparison_material_storage_epoch_policy_uuid_raw16_offset_224","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8d8d6992771e28457ebc214831881e47f45f6274c8ef7f5bd86ddfbfbe962973"},
{"shared_handle_mut_comparison_material_storage_epoch_policy_generation_u64_le_offset_240","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","bf92f2520b21982ef0c7c3fefccfc8bd72e02eadec5b7c429def9c3b486b2131"},
{"shared_handle_mut_comparison_material_timezone_none_policy_uuid_raw16_offset_248","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5e528e694e8c215eb49fe6f56813e84210f070dbe5ee233e19a8ec85fef39e24"},
{"shared_handle_mut_comparison_material_timezone_none_policy_generation_u64_le_offset_264","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ecdb908ee717b70c99fd02920a5c67eabd5aaa9c13b1964dc9263f314ea5c6bf"},
{"shared_handle_mut_comparison_material_leap_na_policy_uuid_raw16_offset_272","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","789d6ae7f9a5d8ea05f23dfefbfe56da7ead2df1a1abffbd37aa4f45fc048930"},
{"shared_handle_mut_comparison_material_leap_na_policy_generation_u64_le_offset_288","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2970a60c90cd530d18eb8b5bc01e99191c473476c00f5598f4c6685494b3c554"},
{"shared_handle_mut_comparison_material_hash_policy_uuid_raw16_offset_296","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f07c3202efef535b0e346860489e808f9dddd4cb4e83054c7c42702ed37e98e2"},
{"shared_handle_mut_comparison_material_hash_policy_generation_u64_le_offset_312","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c3587dce8ac58274063bba3b3bb726d10670c991501b0f92c6ba19c5864867ac"},
{"shared_handle_mut_comparison_material_flags_u32_le_offset_320","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f8d9e58b2665762965eaaaf6759ae115f97f34877494a6d2ce2b583f41211ee3"},
{"shared_handle_mut_comparison_material_minimum_day_i32_le_offset_324","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5965234f027e4436013b59ea2dc95b98a5ee95e97cea9fce90acc0f0e49d7303"},
{"shared_handle_mut_comparison_material_maximum_day_i32_le_offset_328","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","077f8681c6214583d4ea0dbece119ce78b83476ced4afcc84389e70cc1486ebf"},
{"shared_handle_mut_comparison_material_epoch_day_i32_le_offset_332","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","99c58fd7de6fbfd25e6c4bab2f39d1a6cdc2504602062840179849430c155d5f"},
{"shared_handle_mut_comparison_material_component_bytes_u32_le_offset_336","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4af1d456df6d26d3bbbd94c9bec3d77c7d6048545fcfc84b5db7478e6d0cc3f6"},
{"shared_handle_mut_comparison_material_reserved_zero_offset_340","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a97a4a5092732b9ca185d95c6a10298ce90f013a39c296ec847d247b7dc03f7b"},
{"text_wrong_receipt_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","31eb2cadd4275eff8b55ff3199c6ea0d8d703cd9589d5beb13bf16661615e0d7"},
{"text_wrong_catalog_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","0b5a17b7d3d91ad4d47bce1fd3bf11b5d759b1dc0f318f835d721d8c5c013e9b"},
{"text_wrong_registry_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","259d916cef5633c1f8945b56551807588a838bcf5a50bdd0ab54720a9296d758"},
{"text_wrong_descriptor_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","325735e4c054f7ec5719fa5f43a4fa8cc5845ab16b97e614d2e0cd87bea0db04"},
{"text_wrong_descriptor_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","5a34de15b70a706fa2a262f00fc009065afdd772e60dedb2752e8409b39ecba9"},
{"text_wrong_type_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","7cf89493ee1feffa3127dddefedaab582c766a107885ee314365394acea34c31"},
{"text_wrong_type_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","d3a0126390f72eb78019e4ed88dd9a63687d2e087539638e3ce9da70172a1c45"},
{"text_wrong_codec_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","f7b005867c2ffe54082827b7a1118ea0d93b535add2eb31361f8b9b382c42e88"},
{"text_wrong_codec_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","7377fb8eb2a13b8c79cae7045f93042abac3ccd128362d2a9c14f1f7a6acfca0"},
{"text_wrong_codec_version","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","36efb21d24ba38fce9e6ea7bdc747801316a1d0d560e8343db649c24152049b8"},
{"text_metadata_alias_row_id","parse_canonical","VALUE","1970b8ec0658ab11a44757c80c011e6bf9a6a9137460af9091243c6c8dfff6c5"},
{"text_metadata_alias_codec_id","parse_canonical","VALUE","5c1af490319000596cdc12ce039d32b5a1b9e0ea62ef68acf3082891d3d010d2"},
{"text_metadata_alias_authority_source","parse_canonical","VALUE","fad82af25a1f3126a46bebbd296ba581e2ce5b35c34d53b287b95284d9a4b3c0"},
{"text_profile_wrong_descriptor_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","0651e39a29212854bf9175bc6779e39cf8e716dd9caf6ee325cb9422f230f46e"},
{"text_profile_wrong_descriptor_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","961ba9745ab49c6a5055e07753f137dff37371a9cd5121f0b3c5ba99e569aa57"},
{"text_profile_wrong_type_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","a9174042350f959c419f49db99395423f11687b62e29227b6ab400865568d2bd"},
{"text_profile_wrong_type_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","db8f34074cab7d8711b24d4187e0135e5d4ec5da5413549a56def447eb5d69ef"},
{"text_profile_wrong_codec_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","516e984623d4c9066f01c7999db788da7ca8dae2176346e676652fe9a97a5e74"},
{"text_profile_wrong_codec_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","187d7c5c84760632536ea7291f96a99b1e1d5ca304f5a6d8b5d476dd1696bcbd"},
{"text_profile_wrong_charset","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","d9dda4faab60d7f4621026da82953be4fab2ecd5ef060672d8ebd54aa487e8fc"},
{"text_profile_wrong_encoding","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","d0fc04089b933aeb3b915bb5c468c93448df0ec5a4309206fa5f903c7a5515e2"},
{"text_profile_wrong_null_state","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","9639d208dc1208a5fdf9d06eb1f58a9299c1314b7ccb2a3a34ebe85fbc503006"},
{"text_profile_wrong_minimum_bytes","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","8a4a524fbddb69aa6353419b05bbf88bc759d2ca1fbda210cf511a9fda3131eb"},
{"text_profile_wrong_maximum_bytes","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","272d35ec085c4361faf7c715625d0842b3cf368398b88ffb487fc880ef543d28"},
{"text_profile_metadata_alias_descriptor_handle","parse_canonical","VALUE","50937618a3d162b3e636fdc7a92c9668a28cb6d39cdf87e36115091fdf287b6a"},
{"text_profile_metadata_alias_authority_source","parse_canonical","VALUE","6682d728868d1d007a105201bc29eecefe0ba07414af8434ae0de953260d52c0"},
{"null_matrix_validate_canonicalize_operand","validate_canonicalize","SQL_NULL","da1ce84a8f426a4981428b7d533a66c5da31cd3a65c10268b41c0bfd8aeda52d"},
{"null_matrix_decompose_civil_operand","decompose_civil","SQL_NULL","3a86e8ef7617e65164212707b42f2a73ae0587b5d4ce8e9c7e1711dbefc781ba"},
{"null_matrix_render_canonical_operand","render_canonical","SQL_NULL","b0358611545cc0202a4853c99af7d1a6eaa9a81cf65766c3d54c818bfc43b66f"},
{"null_matrix_successor_operand","successor","SQL_NULL","08f1ecf15475191b1851b3da673fc1eaf51b97d77f471aef1ce54120d071cd2f"},
{"null_matrix_predecessor_operand","predecessor","SQL_NULL","367ebd3972e76c9dc9d5776300ca92089c2cf5951159ba94a5ad13f5844cfb10"},
{"null_matrix_is_leap_year_operand","is_leap_year","SQL_NULL","06f6ec7e2ba785d5a8c02471c96a9ad94c2187524ffa5a6f5e4e3cded5dffa59"},
{"null_matrix_days_in_month_operand","days_in_month","SQL_NULL","9658eee16e293dea1cfbe34516ce253a7649e454b0f84dbe762cdcdfc014381a"},
{"null_matrix_iso_weekday_operand","iso_weekday","SQL_NULL","23fa675258de5253ac331b670d93c1e9c912862a3964bc52dbf9d74ba4f69b96"},
{"null_matrix_day_of_year_operand","day_of_year","SQL_NULL","f195d089e04d46c2d04446d082335dac2ef80cf206b5c6070bd2f8a296c6f888"},
{"null_matrix_quarter_operand","quarter","SQL_NULL","0001a216ba8a6ff5a2ebf3f4d3c2ef881781ef0eb5b184f32835c203dc8716b7"},
{"null_matrix_iso_week_operand","iso_week","SQL_NULL","3da8d40ffc6ac8748416b4cb1de2972ed08e01039d85af8bec4ba8d8a6b19d0e"},
{"null_matrix_truncate_day_operand","truncate_day","SQL_NULL","5ee34c4873b9b3bb91df893cc2007fcc95b18aa57da2b979a1f0134cd91d7467"},
{"null_matrix_round_day_operand","round_day","SQL_NULL","ec5dfa2bf11ce66bd9c11c5884434e8a1e2667ef39f12d68be1f0ee95c1dce88"},
{"null_matrix_parse_canonical_operand","parse_canonical","SQL_NULL","b6173263a8b74c9f0f5f42a50231000ddaa67127734c3fd37c697eb2d3b40adf"},
{"null_matrix_civil_construct_year","civil_construct","SQL_NULL","8082a806285f068ab6cff39456f4060c9c36b36f367c866fe4acaa7e19aae2ce"},
{"null_matrix_civil_construct_month","civil_construct","SQL_NULL","0c7b98fbcb2f6936cf6f5e3bf4d3853788686f65530579635458368403ce11eb"},
{"null_matrix_civil_construct_day","civil_construct","SQL_NULL","9f9e0538e60973c6de4bb91135b84d7cb0a6d1dab89f6c99e67b1c4ea309f01c"},
{"null_matrix_civil_construct_all","civil_construct","SQL_NULL","4cecd7f8f8bfa20c13b3962347da919e6fd6389bba33842e9202410c3042fafb"},
{"null_matrix_add_days_date","add_days","SQL_NULL","a37220d840929ef859f5242b9ee27ad442e440af6cfbc5f53d86082cb487c2a8"},
{"null_matrix_add_days_delta","add_days","SQL_NULL","8a56253c75ca19b93b95883dd4339e61277d145a92093c2c664c5cf7102f2f3b"},
{"null_matrix_add_days_both","add_days","SQL_NULL","245113494f733d89514e788c0a8811c8189112131eae96e7f4ef9a4db7832b9a"},
{"null_matrix_subtract_days_date","subtract_days","SQL_NULL","57a8f0b45698d819df2c9feabf3cd1e3bcfac0338a7ed40c074a7c2f9e967e6f"},
{"null_matrix_subtract_days_delta","subtract_days","SQL_NULL","1adaf19b15248313799b8a57ba45efa9b3fdbd95f92f3a334f54177e1c91966a"},
{"null_matrix_subtract_days_both","subtract_days","SQL_NULL","442055259946e9bffb2ebc90a54fa51780efcea417fd39ccd4497e77f16021be"},
{"null_matrix_difference_days_left","difference_days","SQL_NULL","6f348b534ffc28fcc946eb31044dc69621bac60cc550981a5a9c45e60d1cc358"},
{"null_matrix_difference_days_right","difference_days","SQL_NULL","40ba52e3e03480015f96743fe7bfa7d7e9bd597a870e9549c7e91394ac442d3f"},
{"null_matrix_difference_days_both","difference_days","SQL_NULL","fc3e04a7f2633cd98209b17af4c986748a0912a743f31eb0cd511cf9a8b63b3c"},
{"present_payload_wrong_type","validate_canonicalize","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","f9809fa38820e98a17cd4fc6a481bed7e62b80336b9922057590dc9f9379a648"},
{"present_payload_below_i32","validate_canonicalize","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","65785834125ac9d951ce5e0235c52feec238cbfe5a8d401a065d44c9cc9c200a"},
{"present_payload_above_i32","validate_canonicalize","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","4bac7807fbd18cf60bce9b80ab45ad01c576aeb291233556d9ed74b8ef12353d"},
{"i64_NULL_wrong_type","civil_construct","DATATYPE.NULL_STATE.INVALID","f4452607fb5a12700123e6d2162da3048855d3f1f7ce5390d99ae9ccf47bc06e"},
{"i64_NULL_nonzero","civil_construct","DATATYPE.NULL_STATE.INVALID","3a9c47178c878eaec7dc0a92efe857cedcd5747fdf0d260640a98136918234ec"},
{"i64_PRESENT_wrong_type","civil_construct","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","5fa76eecdc9fb6fdd4f2ab1710c55333e64a52815c86eadb1a83fefd5b92606d"},
{"i64_PRESENT_below","civil_construct","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","c7f11b7daa8cdc760cd2f6ecfe134ec36b616683b3ef9077f908cebffa532912"},
{"i64_PRESENT_above","civil_construct","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","03b841a4d1da620535b1dbf0fd5ff152091e9c321ba701a9a1d1fabc1288e95a"},
{"text_NULL_dirty_bytes","parse_canonical","DATATYPE.NULL_STATE.INVALID","6aef60ba693752cc6a00d7b7229ea49650fa77f337e81c92217c1793f5c8aec0"},
{"text_NULL_dirty_extent","parse_canonical","DATATYPE.NULL_STATE.INVALID","e68e018e0141d807c7aa8200384f3a6a0d4af6b53b8175123868e30fb538d206"},
{"text_PRESENT_bad_hex","parse_canonical","CTI.TEMPORAL.INVALID_LITERAL","44441d61accf6c15c5d35e1403988f68e696f26cf8a97bacb8ee228e23ed8c33"},
{"text_PRESENT_extent_mismatch","parse_canonical","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","5c32173125dc04b763262c1cce446fe1ee7fc9ec2a0d88f0f50075f4675e6d67"},
};
void SealedIntrinsicInventory(){Check(std::size(kSealedIntrinsicRows)==219,"sealed intrinsic row count");std::set<std::string_view> ids;uint64_t digest=1469598103934665603ULL;std::map<std::string_view,unsigned> counts;for(const auto& r:kSealedIntrinsicRows){Check(ids.insert(r.case_id).second,"unique sealed intrinsic case");Check(r.row_sha256.size()==64,"sealed row digest extent");++counts[r.operation];for(auto field:{r.case_id,r.operation,r.outcome,r.row_sha256}){for(unsigned char c:field)digest=(digest^c)*1099511628211ULL;digest=(digest^0)*1099511628211ULL;}digest=(digest^unsigned('\n'))*1099511628211ULL;}Check(digest==15718707928188413679ULL,"sealed intrinsic inventory digest");Check(counts["add_days"]==8,"sealed operation partition add_days");Check(counts["aggregate_min_max_count_dispatch"]==1,"sealed operation partition aggregate_min_max_count_dispatch");Check(counts["calendar_interval_or_cross_temporal"]==3,"sealed operation partition calendar_interval_or_cross_temporal");Check(counts["civil_construct"]==16,"sealed operation partition civil_construct");Check(counts["day_of_year"]==3,"sealed operation partition day_of_year");Check(counts["days_in_month"]==3,"sealed operation partition days_in_month");Check(counts["decompose_civil"]==3,"sealed operation partition decompose_civil");Check(counts["difference_days"]==6,"sealed operation partition difference_days");Check(counts["is_leap_year"]==4,"sealed operation partition is_leap_year");Check(counts["iso_week"]==5,"sealed operation partition iso_week");Check(counts["iso_weekday"]==3,"sealed operation partition iso_weekday");Check(counts["larger_truncate_round_or_bucket"]==2,"sealed operation partition larger_truncate_round_or_bucket");Check(counts["parse_canonical"]==33,"sealed operation partition parse_canonical");Check(counts["predecessor"]==2,"sealed operation partition predecessor");Check(counts["quarter"]==2,"sealed operation partition quarter");Check(counts["render_canonical"]==6,"sealed operation partition render_canonical");Check(counts["round_day"]==2,"sealed operation partition round_day");Check(counts["subtract_days"]==6,"sealed operation partition subtract_days");Check(counts["successor"]==5,"sealed operation partition successor");Check(counts["truncate_day"]==2,"sealed operation partition truncate_day");Check(counts["unregistered.operation"]==3,"sealed operation partition unregistered.operation");Check(counts["validate_canonicalize"]==101,"sealed operation partition validate_canonicalize");}
void Civil(){auto p=Profile();struct X{int y,m,d;int32_t day;};constexpr X xs[]={{1970,1,1,0},{0,2,29,-719469},{-4,2,29,-720930},{2000,2,29,11016},{9999,12,31,2932896}};for(auto x:xs){auto c=dt::ConstructDateFromCivilV1(p,x.y,x.m,x.d);Check(c.ok()&&c.value.day==x.day,"civil construct");auto d=dt::DecomposeDateCivilV1(V(p,x.day));Check(d.ok()&&d.civil.year==x.y&&d.civil.month==x.m&&d.civil.day==x.d,"civil decompose");}Check(!dt::ConstructDateFromCivilV1(p,1900,2,29).ok(),"common century leap");auto z=dt::ConstructDateFromCivilV1(p,1970,0,1);Check(!z.ok()&&z.diagnostic.diagnostic_code=="CTI.TEMPORAL.ZERO_DATE_REFUSED","zero month");}
void Arithmetic(){auto p=Profile();auto max=O(p,std::numeric_limits<int32_t>::max()),min=O(p,std::numeric_limits<int32_t>::min()),zero=O(p,0);Check(dt::AddDateDaysV1(O(p,2147483646),1).value.day==2147483647,"add boundary");Check(!dt::AddDateDaysV1(max,1).ok(),"add overflow");Check(!dt::AddDateDaysV1(zero,std::numeric_limits<int64_t>::max()).ok(),"add i64 max");Check(dt::SubtractDateDaysV1(O(p,-2147483647),1).value.day==-2147483648,"subtract boundary");Check(!dt::SubtractDateDaysV1(min,1).ok(),"subtract underflow");Check(!dt::SubtractDateDaysV1(zero,std::numeric_limits<int64_t>::min()).ok(),"subtract i64 min");Check(!dt::DateSuccessorV1(max).ok(),"successor max");Check(!dt::DatePredecessorV1(min).ok(),"predecessor min");auto successor_source=O(p,0);const auto successor_pins=p.use_count();Cancel successor_cancel{0,1};auto successor_stopped=dt::DateSuccessorV1(successor_source,true,{~uint64_t{0},Stop,&successor_cancel});Check(!successor_stopped.ok()&&successor_stopped.value.profile==nullptr&&successor_stopped.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&p.use_count()==successor_pins,"successor final cancellation scrub/pin release");Cancel predecessor_cancel{0,1};auto predecessor_stopped=dt::DatePredecessorV1(successor_source,true,{~uint64_t{0},Stop,&predecessor_cancel});Check(!predecessor_stopped.ok()&&predecessor_stopped.value.profile==nullptr&&predecessor_stopped.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&p.use_count()==successor_pins,"predecessor final cancellation scrub/pin release");auto diff=dt::DifferenceDateDaysV1(max.view(),min.view());Check(diff.ok()&&diff.signed_value==4294967295LL,"full positive difference");diff=dt::DifferenceDateDaysV1(min.view(),max.view());Check(diff.ok()&&diff.signed_value==-4294967295LL,"full negative difference");}
void Facts(){auto p=Profile();auto feb1900=V(p,-25536),feb2000=V(p,10988),leap_end=V(p,11322);Check(!dt::DateIsLeapYearV1(feb1900).boolean_value,"1900 common");Check(dt::DateIsLeapYearV1(feb2000).boolean_value,"2000 leap");Check(dt::DateDaysInMonthV1(feb1900).signed_value==28,"feb 1900 days");Check(dt::DateDaysInMonthV1(feb2000).signed_value==29,"feb 2000 days");Check(dt::DateIsoWeekdayV1(V(p,0)).signed_value==4,"epoch weekday");Check(dt::DateIsoWeekdayV1(V(p,-1)).signed_value==3,"pre-epoch weekday");Check(dt::DateDayOfYearV1(leap_end).signed_value==366,"leap day of year");Check(dt::DateQuarterV1(leap_end).signed_value==4,"quarter");struct I{int32_t day,year;unsigned week,weekday;};constexpr I iv[]={{17896,2019,1,1},{16801,2015,53,5},{-719528,-1,52,6},{std::numeric_limits<int32_t>::min(),-5877641,26,2},{std::numeric_limits<int32_t>::max(),5881580,28,5}};for(auto x:iv){auto r=dt::DateIsoWeekV1(V(p,x.day));Check(r.ok()&&r.iso_year==x.year&&r.iso_week==x.week&&r.iso_weekday==x.weekday,"ISO week vector");}}
void RegistryAndRefusal(){auto p=Profile();for(unsigned i=0;i<21;++i){auto op=static_cast<dt::DateIntrinsicOperationV1>(i);auto got=dt::ClassifyDateIntrinsicOperationV1(op);auto want=i<18?dt::DateIntrinsicDispositionV1::admitted:i<20?dt::DateIntrinsicDispositionV1::registered_refused:dt::DateIntrinsicDispositionV1::receiving_owner;Check(got==want,"21-row intrinsic classification");}auto clean=V(p,0);auto null=dt::DateValueViewV1{p.get(),dt::DateValueStateV1::sql_null,0};for(auto op:{dt::DateIntrinsicOperationV1::larger_truncate_round_or_bucket,dt::DateIntrinsicOperationV1::calendar_interval_or_cross_temporal}){auto r=dt::RefuseDateIntrinsicOperationV1(clean,op);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","known refused operation");r=dt::RefuseDateIntrinsicOperationV1(null,op);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","known refusal precedes clean NULL propagation");}auto unknown=static_cast<dt::DateIntrinsicOperationV1>(255);Check(dt::ClassifyDateIntrinsicOperationV1(unknown)==dt::DateIntrinsicDispositionV1::unknown,"unknown classification");auto ur=dt::RefuseDateIntrinsicOperationV1(clean,unknown);Check(!ur.ok()&&ur.diagnostic.diagnostic_code=="CTI.TEMPORAL.OPERATION_REFUSED","unknown operation");dt::DateValueViewV1 dirty{p.get(),dt::DateValueStateV1::sql_null,7};auto dr=dt::RefuseDateIntrinsicOperationV1(dirty,unknown);Check(!dr.ok()&&dr.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","state precedes operation classification");}
void ParseRenderIdentity(){auto p=Profile();constexpr std::array<std::string_view,6> texts{{"1970-01-01","0000-02-29","-0000001-01-01","+0010000-01-01","-5877641-06-23","+5881580-07-11"}};for(auto text:texts){auto q=dt::ParseCanonicalDateV1(p,text);Check(q.ok(),"parse canonical");auto r=dt::RenderCanonicalDateV1(q.value);Check(r.ok()&&r.text==text,"render canonical");}for(auto text:{"+0001970-01-01","1900-02-29","1970-1-01","1970-01-1","1970-00-01","1970-01-00","1970-13-01","1970-01-32"})Check(!dt::ParseCanonicalDateV1(p,text).ok(),"invalid literal");auto x=O(p,123);Check(dt::ValidateCanonicalDateV1(x).value.day==123,"validate/canonicalize");Check(dt::TruncateDateDayV1(x).value.day==123,"truncate day");Check(dt::RoundDateDayV1(x).value.day==123,"round day");auto n=O(p,0,dt::DateValueStateV1::sql_null);Check(dt::RoundDateDayV1(n).value.state==dt::DateValueStateV1::sql_null,"round null");}
void NullableAndTextCarriers(){
  auto p=Profile();
  const dt::DateNullableI64FactV1 present{dt::DateI64CarrierKindV1::signed_i64,dt::DateValueStateV1::value,1};
  const dt::DateNullableI64FactV1 null{dt::DateI64CarrierKindV1::signed_i64,dt::DateValueStateV1::sql_null,0};
  const dt::DateNullableI64FactV1 poison{dt::DateI64CarrierKindV1::wrong_host_type,dt::DateValueStateV1::value,std::numeric_limits<int64_t>::min()};
  for(auto which:{0,1,2}){auto y=which==0?null:poison,m=which==1?null:poison,d=which==2?null:poison;auto r=dt::ConstructDateFromCivilV1(p,y,m,d);Check(r.ok()&&r.value.state==dt::DateValueStateV1::sql_null,"civil clean NULL skips poison peers");}
  auto dirty=null;dirty.value=1;auto r=dt::ConstructDateFromCivilV1(p,dirty,null,null);Check(!r.ok()&&r.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","civil dirty NULL nonzero");dirty=null;dirty.carrier=dt::DateI64CarrierKindV1::wrong_host_type;r=dt::ConstructDateFromCivilV1(p,dirty,null,null);Check(!r.ok()&&r.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","civil dirty NULL carrier");
  for(auto kind:{dt::DateI64CarrierKindV1::wrong_host_type,dt::DateI64CarrierKindV1::below_i64,dt::DateI64CarrierKindV1::above_i64}){auto bad=present;bad.carrier=kind;r=dt::ConstructDateFromCivilV1(p,bad,present,present);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","civil PRESENT carrier refused");}
  for(auto year:{std::numeric_limits<int64_t>::min(),std::numeric_limits<int64_t>::max()})for(auto month:{1,2}){r=dt::ConstructDateFromCivilV1(p,year,month,1);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED","extreme civil range without overflow");r=dt::ConstructDateFromCivilV1(p,year,month,31);Check(!r.ok()&&r.diagnostic.diagnostic_code==(month==2?"CTI.TEMPORAL.INVALID_LITERAL":"CTI.TEMPORAL.RANGE_EXCEEDED"),"extreme civil validity/range precedence");}
  auto date_null=O(p,0,dt::DateValueStateV1::sql_null),value=O(p,0);auto a=dt::AddDateDaysV1(date_null,poison);Check(a.ok()&&a.value.state==dt::DateValueStateV1::sql_null,"add date NULL skips poison delta");a=dt::AddDateDaysV1(value,null);Check(a.ok()&&a.value.state==dt::DateValueStateV1::sql_null,"add delta NULL");a=dt::SubtractDateDaysV1(date_null,null);Check(a.ok()&&a.value.state==dt::DateValueStateV1::sql_null,"subtract both NULL");a=dt::AddDateDaysV1(value,dirty);Check(!a.ok()&&a.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","add dirty delta");a=dt::AddDateDaysV1(value,null,false);Check(!a.ok()&&a.diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED","add NULL result not admitted");Cancel cancel{0,1};a=dt::AddDateDaysV1(value,present,true,{~uint64_t{0},Stop,&cancel});Check(!a.ok()&&a.diagnostic.diagnostic_code=="PROCESS.CANCELLED","add cancellation atomic");
  auto* ti=TextIdentity();Check(ti!=nullptr,"d707 text identity");auto td=TextDescriptor();dt::DateTextOperandV1 text{ti,&td,dt::DateTextCarrierKindV1::utf8_bytes,dt::DateValueStateV1::value,"1970-01-01",10};auto parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(parsed.ok()&&parsed.value.day==0,"TextOperand canonical parse");
  text.state=dt::DateValueStateV1::sql_null;text.bytes={};text.extent=0;parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(parsed.ok()&&parsed.value.state==dt::DateValueStateV1::sql_null,"TextOperand clean NULL");text.bytes="x";parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(!parsed.ok()&&parsed.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","TextOperand dirty NULL bytes");text.bytes={};text.extent=1;parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(!parsed.ok()&&parsed.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","TextOperand dirty NULL extent");text.extent=0;text.carrier=dt::DateTextCarrierKindV1::wrong_host_type;parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(!parsed.ok()&&parsed.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","TextOperand NULL wrong carrier");
  text={ti,&td,dt::DateTextCarrierKindV1::wrong_host_type,dt::DateValueStateV1::value,"1970-01-01",10};parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(!parsed.ok()&&parsed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","TextOperand PRESENT wrong carrier");text.carrier=dt::DateTextCarrierKindV1::utf8_bytes;text.extent=9;parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(!parsed.ok()&&parsed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","TextOperand extent mismatch");text.bytes=std::string_view("\xff",1);text.extent=1;parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(!parsed.ok()&&parsed.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","TextOperand malformed UTF8/grammar");
  auto alias=*ti;alias.legacy_fields.canonical_name="localized-date-input";alias.legacy_fields.codec_id="localized-codec-label";td.stable_name="localized-text";text={&alias,&td,dt::DateTextCarrierKindV1::utf8_bytes,dt::DateValueStateV1::value,"1970-01-01",10};Check(dt::ParseCanonicalDateOperandV1(p,text).ok(),"TextOperand presentation aliases admitted");
  auto mutate=[&](auto f){auto bad=*ti;f(bad);text.identity=&bad;auto x=dt::ParseCanonicalDateOperandV1(p,text);Check(!x.ok()&&x.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","TextOperand semantic identity mutation");};mutate([](auto&x){x.legacy_fields.catalog_generation++;});mutate([](auto&x){x.legacy_fields.registry_generation++;});mutate([](auto&x){x.legacy_fields.descriptor_uuid.bytes[0]^=1;});mutate([](auto&x){x.legacy_fields.descriptor_generation++;});mutate([](auto&x){x.legacy_fields.type_uuid.bytes[0]^=1;});mutate([](auto&x){x.legacy_fields.type_generation++;});mutate([](auto&x){x.legacy_fields.codec_uuid.bytes[0]^=1;});mutate([](auto&x){x.legacy_fields.codec_generation++;});mutate([](auto&x){x.legacy_fields.codec_version++;});mutate([](auto&x){x.legacy_fields.canonical_charset="UTF-16";});mutate([](auto&x){x.legacy_fields.canonical_representation="wrong";});mutate([](auto&x){x.legacy_fields.sql_null_requires_zero_payload=false;});mutate([](auto&x){x.legacy_fields.canonical_value_minimum_bytes++;});mutate([](auto&x){x.legacy_fields.canonical_value_maximum_bytes--;});
}
void ExactFiniteSealedReplay(){
  auto p=Profile();
  auto diag=[](const auto& r,std::string_view code){return !r.ok()&&r.diagnostic.diagnostic_code==code;};
  auto nullv=O(p,0,dt::DateValueStateV1::sql_null);
  auto i64=[](int64_t v){return dt::DateNullableI64FactV1{dt::DateI64CarrierKindV1::signed_i64,dt::DateValueStateV1::value,v};};
  const auto ni64=dt::DateNullableI64FactV1{dt::DateI64CarrierKindV1::signed_i64,dt::DateValueStateV1::sql_null,0};
  const auto poison=dt::DateNullableI64FactV1{dt::DateI64CarrierKindV1::wrong_host_type,dt::DateValueStateV1::value,0};

  struct CivilRow{std::string_view id;int64_t y,m,d;int32_t out;};
  for(const auto& x:std::array<CivilRow,4>{{
      {"construct_epoch",1970,1,1,0},{"construct_year_zero_leap",0,2,29,-719469},
      {"construct_negative_leap",-4,2,29,-720930},{"construct_2000_leap",2000,2,29,11016}}}){
    auto r=dt::ConstructDateFromCivilV1(p,i64(x.y),i64(x.m),i64(x.d));
    Check(r.ok()&&r.value.state==dt::DateValueStateV1::value&&r.value.day==x.out,x.id);Mark({x.id});
  }
  auto r=dt::ConstructDateFromCivilV1(p,i64(1900),i64(2),i64(29));Check(diag(r,"CTI.TEMPORAL.INVALID_LITERAL"),"construct_1900_common_refusal");Mark({"construct_1900_common_refusal"});
  r=dt::ConstructDateFromCivilV1(p,i64(1970),i64(0),i64(1));Check(diag(r,"CTI.TEMPORAL.ZERO_DATE_REFUSED"),"construct_zero_month");Mark({"construct_zero_month"});
  r=dt::ConstructDateFromCivilV1(p,ni64,i64(1),i64(1));Check(r.ok()&&r.value.state==dt::DateValueStateV1::sql_null&&r.value.day==0,"construct_null_component");Mark({"construct_null_component"});
  auto civil=dt::DecomposeDateCivilV1(V(p,0));Check(civil.ok()&&!civil.is_null&&civil.civil.year==1970&&civil.civil.month==1&&civil.civil.day==1,"decompose_epoch");Mark({"decompose_epoch"});

  auto vr=dt::AddDateDaysV1(O(p,2147483646),i64(1));Check(vr.ok()&&vr.value.day==INT32_MAX,"add_max_boundary");Mark({"add_max_boundary"});
  vr=dt::AddDateDaysV1(O(p,INT32_MAX),i64(1));Check(diag(vr,"CTI.TEMPORAL.RANGE_EXCEEDED"),"add_overflow");Mark({"add_overflow"});
  vr=dt::AddDateDaysV1(O(p,0),i64(INT64_MAX));Check(diag(vr,"CTI.TEMPORAL.RANGE_EXCEEDED"),"add_i64_max");Mark({"add_i64_max"});
  vr=dt::SubtractDateDaysV1(O(p,-2147483647),i64(1));Check(vr.ok()&&vr.value.day==INT32_MIN,"subtract_min_boundary");Mark({"subtract_min_boundary"});
  vr=dt::SubtractDateDaysV1(O(p,INT32_MIN),i64(1));Check(diag(vr,"CTI.TEMPORAL.RANGE_EXCEEDED"),"subtract_underflow");Mark({"subtract_underflow"});
  vr=dt::SubtractDateDaysV1(O(p,0),i64(INT64_MIN));Check(diag(vr,"CTI.TEMPORAL.RANGE_EXCEEDED"),"subtract_i64_min");Mark({"subtract_i64_min"});
  vr=dt::DateSuccessorV1(O(p,INT32_MAX));Check(diag(vr,"CTI.TEMPORAL.RANGE_EXCEEDED"),"successor_max_refusal");Mark({"successor_max_refusal"});
  vr=dt::DatePredecessorV1(O(p,INT32_MIN));Check(diag(vr,"CTI.TEMPORAL.RANGE_EXCEEDED"),"predecessor_min_refusal");Mark({"predecessor_min_refusal"});
  auto sr=dt::DifferenceDateDaysV1(V(p,INT32_MAX),V(p,INT32_MIN));Check(sr.ok()&&!sr.is_null&&sr.signed_value==4294967295LL,"difference_full_positive");Mark({"difference_full_positive"});
  sr=dt::DifferenceDateDaysV1(V(p,INT32_MIN),V(p,INT32_MAX));Check(sr.ok()&&!sr.is_null&&sr.signed_value==-4294967295LL,"difference_full_negative");Mark({"difference_full_negative"});

  sr=dt::DateIsoWeekdayV1(V(p,0));Check(sr.ok()&&sr.signed_value==4,"weekday_epoch");Mark({"weekday_epoch"});
  sr=dt::DateIsoWeekdayV1(V(p,-1));Check(sr.ok()&&sr.signed_value==3,"weekday_before_epoch");Mark({"weekday_before_epoch"});
  sr=dt::DateIsLeapYearV1(V(p,-865625));Check(sr.ok()&&sr.boolean_value,"leap_negative_400");Mark({"leap_negative_400"});
  sr=dt::DateIsLeapYearV1(V(p,-719528));Check(sr.ok()&&sr.boolean_value,"leap_zero");Mark({"leap_zero"});
  sr=dt::DateDaysInMonthV1(V(p,-25536));Check(sr.ok()&&sr.signed_value==28,"days_feb_1900");Mark({"days_feb_1900"});
  sr=dt::DateDaysInMonthV1(V(p,10988));Check(sr.ok()&&sr.signed_value==29,"days_feb_2000");Mark({"days_feb_2000"});
  sr=dt::DateDayOfYearV1(V(p,11322));Check(sr.ok()&&sr.signed_value==366,"day_of_year_leap");Mark({"day_of_year_leap"});
  sr=dt::DateQuarterV1(V(p,11322));Check(sr.ok()&&sr.signed_value==4,"quarter_q4");Mark({"quarter_q4"});
  struct IsoRow{std::string_view id;int32_t day,year;uint8_t week,weekday;};
  for(const auto& x:std::array<IsoRow,3>{{{"iso_late_dec_next_year",17896,2019,1,1},{"iso_early_jan_prior_year",16801,2015,53,5},{"iso_year_zero_boundary",-719528,-1,52,6}}}){
    auto q=dt::DateIsoWeekV1(V(p,x.day));Check(q.ok()&&!q.is_null&&q.iso_year==x.year&&q.iso_week==x.week&&q.iso_weekday==x.weekday,x.id);Mark({x.id});
  }
  vr=dt::TruncateDateDayV1(O(p,123));Check(vr.ok()&&vr.value.day==123,"truncate_identity");Mark({"truncate_identity"});
  vr=dt::RoundDateDayV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null&&vr.value.day==0,"round_null");Mark({"round_null"});
  vr=dt::AddDateDaysV1(O(p,7,dt::DateValueStateV1::sql_null),i64(1));Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"dirty_null_refusal");Mark({"dirty_null_refusal"});
  vr=dt::RefuseDateIntrinsicOperationV1(V(p,0),dt::DateIntrinsicOperationV1::calendar_interval_or_cross_temporal);Check(diag(vr,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED"),"unsupported_no_payload_inspection");Mark({"unsupported_no_payload_inspection"});
  Cancel cancel{0,1};vr=dt::AddDateDaysV1(O(p,1),i64(1),true,{~uint64_t{0},Stop,&cancel});Check(diag(vr,"PROCESS.CANCELLED"),"cancellation_atomic");Mark({"cancellation_atomic"});
  vr=dt::ValidateCanonicalDateV1(O(p,0));Check(vr.ok()&&vr.value.day==0,"validate_value");Mark({"validate_value"});

  auto* ti=TextIdentity();Check(ti!=nullptr,"sealed text identity");auto td=TextDescriptor();
  auto text=[&](const dt::DatatypeTypeCodecIdentityRowV3* id,const scratchbird::engine::ExecutionTypeDescriptor* desc,dt::DateValueStateV1 state,std::string_view bytes,uint64_t extent,dt::DateTextCarrierKindV1 carrier=dt::DateTextCarrierKindV1::utf8_bytes){return dt::DateTextOperandV1{id,desc,carrier,state,bytes,extent};};
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::value,"1970-01-01",10));Check(vr.ok()&&vr.value.day==0,"parse_epoch");Mark({"parse_epoch"});
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::value,"+0001970-01-01",14));Check(diag(vr,"CTI.TEMPORAL.INVALID_LITERAL"),"parse_expanded_alias_refusal");Mark({"parse_expanded_alias_refusal"});
  auto tr=dt::RenderCanonicalDateV1(O(p,0));Check(tr.ok()&&!tr.containing_null&&tr.text=="1970-01-01","render_standard");Mark({"render_standard"});
  vr=dt::RefuseDateIntrinsicOperationV1(V(p,0),dt::DateIntrinsicOperationV1::larger_truncate_round_or_bucket);Check(diag(vr,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED"),"larger_truncate_refused");Mark({"larger_truncate_refused"});
  Check(dt::ClassifyDateIntrinsicOperationV1(dt::DateIntrinsicOperationV1::aggregate_min_max_count_dispatch)==dt::DateIntrinsicDispositionV1::receiving_owner,"aggregate_dispatch_handoff");Mark({"aggregate_dispatch_handoff"});
  const auto unknown=static_cast<dt::DateIntrinsicOperationV1>(255);
  vr=dt::RefuseDateIntrinsicOperationV1(V(p,0),unknown);Check(diag(vr,"CTI.TEMPORAL.OPERATION_REFUSED"),"unknown_operation_refused");Mark({"unknown_operation_refused"});
  vr=dt::RefuseDateIntrinsicOperationV1(nullv.view(),dt::DateIntrinsicOperationV1::calendar_interval_or_cross_temporal);Check(diag(vr,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED"),"known_refused_clean_null");Mark({"known_refused_clean_null"});
  auto dirty=dt::DateValueViewV1{p.get(),dt::DateValueStateV1::sql_null,7};
  vr=dt::RefuseDateIntrinsicOperationV1(dirty,unknown);Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"dirty_state_plus_unknown");Mark({"dirty_state_plus_unknown"});
  vr=dt::RefuseDateIntrinsicOperationV1(dirty,dt::DateIntrinsicOperationV1::calendar_interval_or_cross_temporal);Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"dirty_state_plus_known_refused");Mark({"dirty_state_plus_known_refused"});
  vr=dt::DateSuccessorV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"clean_null_plus_supported");Mark({"clean_null_plus_supported"});
  vr=dt::RefuseDateIntrinsicOperationV1(V(p,0),unknown);Check(diag(vr,"CTI.TEMPORAL.OPERATION_REFUSED"),"unknown_clean_state");Mark({"unknown_clean_state"});
  vr=dt::RefuseDateIntrinsicOperationV1(V(p,0),dt::DateIntrinsicOperationV1::larger_truncate_round_or_bucket);Check(diag(vr,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED"),"known_refused_clean_state");Mark({"known_refused_clean_state"});
  auto badp=std::make_shared<dt::DateValidatedProfileHandleV1>(*p);badp->profile_material[0]^=1;vr=dt::DateSuccessorV1(O(badp,0));Check(diag(vr,"CTI.TEMPORAL.DESCRIPTOR_INVALID"),"operation_profile_mismatch");Mark({"operation_profile_mismatch"});
  vr=dt::DateSuccessorV1({nullptr,dt::DateValueStateV1::value,0});Check(diag(vr,"CTI.TEMPORAL.DESCRIPTOR_INVALID"),"operation_profile_missing");Mark({"operation_profile_missing"});

  civil=dt::DecomposeDateCivilV1(nullv.view());Check(civil.ok()&&civil.is_null&&civil.civil.year==0&&civil.civil.month==0&&civil.civil.day==0,"clean_null_decompose");Mark({"clean_null_decompose"});
  tr=dt::RenderCanonicalDateV1(nullv);Check(tr.ok()&&tr.containing_null&&tr.text.empty(),"clean_null_render");Mark({"clean_null_render"});
  sr=dt::DifferenceDateDaysV1(nullv.view(),V(p,9));Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"clean_null_difference");Mark({"clean_null_difference"});
  sr=dt::DateIsLeapYearV1(nullv.view());Check(sr.ok()&&sr.is_null&&!sr.boolean_value,"clean_null_u8_fact");Mark({"clean_null_u8_fact"});
  sr=dt::DateDayOfYearV1(nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"clean_null_u16_fact");Mark({"clean_null_u16_fact"});
  auto iso=dt::DateIsoWeekV1(nullv.view());Check(iso.ok()&&iso.is_null&&iso.iso_year==0&&iso.iso_week==0&&iso.iso_weekday==0,"clean_null_iso_week");Mark({"clean_null_iso_week"});
  tr=dt::RenderCanonicalDateV1(O(p,2932897));Check(tr.ok()&&tr.text=="+0010000-01-01","render_expanded_14");Mark({"render_expanded_14"});
  std::array<char,14> out{};auto wr=dt::RenderCanonicalDateIntoNoAllocV1(O(p,2932897),false,out.data(),13);Check(diag(wr,"CTB.TEXT.LENGTH_EXCEEDED")&&wr.bytes_written==0,"render_expanded_one_byte_short");Mark({"render_expanded_one_byte_short"});
  cancel={0,1};wr=dt::RenderCanonicalDateIntoNoAllocV1(O(p,0),false,out.data(),out.size(),{~uint64_t{0},Stop,&cancel});Check(diag(wr,"PROCESS.CANCELLED")&&wr.bytes_written==0,"render_cancellation");Mark({"render_cancellation"});
  auto aliasp=std::make_shared<dt::DateValidatedProfileHandleV1>(*p);aliasp->identity.legacy_fields.canonical_name="localized-date";vr=dt::ValidateCanonicalDateV1(O(aliasp,7));Check(vr.ok()&&vr.value.day==7,"shared_handle_presentation_alias");Mark({"shared_handle_presentation_alias"});
  vr=dt::ValidateCanonicalDateV1({nullptr,dt::DateValueStateV1::value,0});Check(diag(vr,"CTI.TEMPORAL.DESCRIPTOR_INVALID"),"shared_handle_name_only_refused");Mark({"shared_handle_name_only_refused"});

  auto run_text_mutation=[&](std::string_view id,auto mutate){auto changed=*ti;auto changed_td=td;mutate(changed,changed_td);auto q=dt::ParseCanonicalDateOperandV1(p,text(&changed,&changed_td,dt::DateValueStateV1::value,"1970-01-01",10));Check(diag(q,"CTI.TEMPORAL.DESCRIPTOR_INVALID"),id);Mark({id});};
  run_text_mutation("text_wrong_receipt_uuid",[](auto& x,auto&){x.legacy_fields.catalog_snapshot_uuid.bytes[0]^=1;});
  run_text_mutation("text_wrong_catalog_generation",[](auto& x,auto&){++x.legacy_fields.catalog_generation;});
  run_text_mutation("text_wrong_registry_generation",[](auto& x,auto&){++x.legacy_fields.registry_generation;});
  run_text_mutation("text_wrong_descriptor_uuid",[](auto& x,auto&){x.legacy_fields.descriptor_uuid.bytes[0]^=1;});
  run_text_mutation("text_wrong_descriptor_generation",[](auto& x,auto&){++x.legacy_fields.descriptor_generation;});
  run_text_mutation("text_wrong_type_uuid",[](auto& x,auto&){x.legacy_fields.type_uuid.bytes[0]^=1;});
  run_text_mutation("text_wrong_type_generation",[](auto& x,auto&){++x.legacy_fields.type_generation;});
  run_text_mutation("text_wrong_codec_uuid",[](auto& x,auto&){x.legacy_fields.codec_uuid.bytes[0]^=1;});
  run_text_mutation("text_wrong_codec_generation",[](auto& x,auto&){++x.legacy_fields.codec_generation;});
  run_text_mutation("text_wrong_codec_version",[](auto& x,auto&){++x.legacy_fields.codec_version;});
  for(auto id:{"text_metadata_alias_row_id","text_metadata_alias_codec_id","text_metadata_alias_authority_source","text_profile_metadata_alias_descriptor_handle","text_profile_metadata_alias_authority_source"}){
    auto a=*ti;if(id==std::string_view("text_metadata_alias_row_id"))a.legacy_fields.canonical_name="localized-row";if(id==std::string_view("text_metadata_alias_codec_id"))a.legacy_fields.codec_id="localized-codec";auto q=dt::ParseCanonicalDateOperandV1(p,text(&a,&td,dt::DateValueStateV1::value,"1970-01-01",10));Check(q.ok()&&q.value.day==0,id);Mark({id});
  }
  run_text_mutation("text_profile_wrong_descriptor_uuid",[](auto& x,auto&){x.legacy_fields.descriptor_uuid.bytes[0]^=1;});
  run_text_mutation("text_profile_wrong_descriptor_generation",[](auto& x,auto&){++x.legacy_fields.descriptor_generation;});
  run_text_mutation("text_profile_wrong_type_uuid",[](auto& x,auto&){x.legacy_fields.type_uuid.bytes[0]^=1;});
  run_text_mutation("text_profile_wrong_type_generation",[](auto& x,auto&){++x.legacy_fields.type_generation;});
  run_text_mutation("text_profile_wrong_codec_uuid",[](auto& x,auto&){x.legacy_fields.codec_uuid.bytes[0]^=1;});
  run_text_mutation("text_profile_wrong_codec_generation",[](auto& x,auto&){++x.legacy_fields.codec_generation;});
  run_text_mutation("text_profile_wrong_charset",[](auto& x,auto&){x.legacy_fields.canonical_charset="UTF-16";});
  run_text_mutation("text_profile_wrong_encoding",[](auto& x,auto&){x.legacy_fields.canonical_representation="wrong";});
  run_text_mutation("text_profile_wrong_null_state",[](auto& x,auto&){x.legacy_fields.sql_null_requires_zero_payload=false;});
  run_text_mutation("text_profile_wrong_minimum_bytes",[](auto& x,auto&){++x.legacy_fields.canonical_value_minimum_bytes;});
  run_text_mutation("text_profile_wrong_maximum_bytes",[](auto& x,auto&){--x.legacy_fields.canonical_value_maximum_bytes;});

  vr=dt::ValidateCanonicalDateV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_validate_canonicalize_operand");Mark({"null_matrix_validate_canonicalize_operand"});
  civil=dt::DecomposeDateCivilV1(nullv.view());Check(civil.ok()&&civil.is_null,"null_matrix_decompose_civil_operand");Mark({"null_matrix_decompose_civil_operand"});
  tr=dt::RenderCanonicalDateV1(nullv);Check(tr.ok()&&tr.containing_null&&tr.text.empty(),"null_matrix_render_canonical_operand");Mark({"null_matrix_render_canonical_operand"});
  vr=dt::DateSuccessorV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_successor_operand");Mark({"null_matrix_successor_operand"});
  vr=dt::DatePredecessorV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_predecessor_operand");Mark({"null_matrix_predecessor_operand"});
  sr=dt::DateIsLeapYearV1(nullv.view());Check(sr.ok()&&sr.is_null&&!sr.boolean_value,"null_matrix_is_leap_year_operand");Mark({"null_matrix_is_leap_year_operand"});
  sr=dt::DateDaysInMonthV1(nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_days_in_month_operand");Mark({"null_matrix_days_in_month_operand"});
  sr=dt::DateIsoWeekdayV1(nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_iso_weekday_operand");Mark({"null_matrix_iso_weekday_operand"});
  sr=dt::DateDayOfYearV1(nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_day_of_year_operand");Mark({"null_matrix_day_of_year_operand"});
  sr=dt::DateQuarterV1(nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_quarter_operand");Mark({"null_matrix_quarter_operand"});
  iso=dt::DateIsoWeekV1(nullv.view());Check(iso.ok()&&iso.is_null&&iso.iso_year==0&&iso.iso_week==0&&iso.iso_weekday==0,"null_matrix_iso_week_operand");Mark({"null_matrix_iso_week_operand"});
  vr=dt::TruncateDateDayV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_truncate_day_operand");Mark({"null_matrix_truncate_day_operand"});
  vr=dt::RoundDateDayV1(nullv);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_round_day_operand");Mark({"null_matrix_round_day_operand"});
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::sql_null,{},0));Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_parse_canonical_operand");Mark({"null_matrix_parse_canonical_operand"});

  for(const auto& x:std::array<std::pair<std::string_view,int>,3>{{{"null_matrix_civil_construct_year",0},{"null_matrix_civil_construct_month",1},{"null_matrix_civil_construct_day",2}}}){
    auto y=x.second==0?ni64:poison,m=x.second==1?ni64:poison,d=x.second==2?ni64:poison;vr=dt::ConstructDateFromCivilV1(p,y,m,d);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,x.first);Mark({x.first});
  }
  vr=dt::ConstructDateFromCivilV1(p,ni64,ni64,ni64);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_civil_construct_all");Mark({"null_matrix_civil_construct_all"});
  auto present_poison=O(p,INT32_MAX);
  vr=dt::AddDateDaysV1(nullv,poison);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_add_days_date");Mark({"null_matrix_add_days_date"});
  vr=dt::AddDateDaysV1(present_poison,ni64);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_add_days_delta");Mark({"null_matrix_add_days_delta"});
  vr=dt::AddDateDaysV1(nullv,ni64);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_add_days_both");Mark({"null_matrix_add_days_both"});
  vr=dt::SubtractDateDaysV1(nullv,poison);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_subtract_days_date");Mark({"null_matrix_subtract_days_date"});
  vr=dt::SubtractDateDaysV1(present_poison,ni64);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_subtract_days_delta");Mark({"null_matrix_subtract_days_delta"});
  vr=dt::SubtractDateDaysV1(nullv,ni64);Check(vr.ok()&&vr.value.state==dt::DateValueStateV1::sql_null,"null_matrix_subtract_days_both");Mark({"null_matrix_subtract_days_both"});
  sr=dt::DifferenceDateDaysV1(nullv.view(),present_poison.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_difference_days_left");Mark({"null_matrix_difference_days_left"});
  sr=dt::DifferenceDateDaysV1(present_poison.view(),nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_difference_days_right");Mark({"null_matrix_difference_days_right"});
  sr=dt::DifferenceDateDaysV1(nullv.view(),nullv.view());Check(sr.ok()&&sr.is_null&&sr.signed_value==0,"null_matrix_difference_days_both");Mark({"null_matrix_difference_days_both"});

  static_assert(std::is_same_v<decltype(dt::DateValueViewV1{}.day),std::int32_t>);
  for(auto id:{"present_payload_wrong_type","present_payload_below_i32","present_payload_above_i32"})Check(boundary_rows.insert(id).second,"untyped DateOperand boundary row unique");
  auto bad_i64=ni64;bad_i64.carrier=dt::DateI64CarrierKindV1::wrong_host_type;vr=dt::ConstructDateFromCivilV1(p,bad_i64,i64(1),i64(1));Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"i64_NULL_wrong_type");Mark({"i64_NULL_wrong_type"});
  bad_i64=ni64;bad_i64.value=7;vr=dt::ConstructDateFromCivilV1(p,bad_i64,i64(1),i64(1));Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"i64_NULL_nonzero");Mark({"i64_NULL_nonzero"});
  for(const auto& x:std::array<std::pair<std::string_view,dt::DateI64CarrierKindV1>,3>{{{"i64_PRESENT_wrong_type",dt::DateI64CarrierKindV1::wrong_host_type},{"i64_PRESENT_below",dt::DateI64CarrierKindV1::below_i64},{"i64_PRESENT_above",dt::DateI64CarrierKindV1::above_i64}}}){
    bad_i64=i64(0);bad_i64.carrier=x.second;vr=dt::ConstructDateFromCivilV1(p,bad_i64,i64(1),i64(1));Check(diag(vr,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"),x.first);Mark({x.first});
  }
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::sql_null,std::string_view("\0",1),0));Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"text_NULL_dirty_bytes");Mark({"text_NULL_dirty_bytes"});
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::sql_null,{},1));Check(diag(vr,"DATATYPE.NULL_STATE.INVALID"),"text_NULL_dirty_extent");Mark({"text_NULL_dirty_extent"});
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::value,std::string_view("\xff",1),1));Check(diag(vr,"CTI.TEMPORAL.INVALID_LITERAL"),"text_PRESENT_bad_hex");Mark({"text_PRESENT_bad_hex"});
  vr=dt::ParseCanonicalDateOperandV1(p,text(ti,&td,dt::DateValueStateV1::value,"1970-01-01",9));Check(diag(vr,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"),"text_PRESENT_extent_mismatch");Mark({"text_PRESENT_extent_mismatch"});
}
void ExactProfileMutationReplay(){auto p=Profile();unsigned replayed=0;for(const auto& row:kSealedIntrinsicRows){const std::string_view marker="_offset_";auto at=row.case_id.rfind(marker);if(at==std::string_view::npos)continue;auto number=row.case_id.substr(at+marker.size());size_t offset=0;auto cv=std::from_chars(number.data(),number.data()+number.size(),offset);Check(cv.ec==std::errc{}&&cv.ptr==number.data()+number.size(),"sealed mutation offset parsed");auto changed=std::make_shared<dt::DateValidatedProfileHandleV1>(*p);if(row.case_id.starts_with("shared_handle_mut_profile_material_")){Check(offset<changed->profile_material.size(),"profile mutation offset bounded");changed->profile_material[offset]^=1;}else if(row.case_id.starts_with("shared_handle_mut_comparison_material_")){Check(offset<changed->comparison_material.size(),"comparison mutation offset bounded");changed->comparison_material[offset]^=1;}else continue;auto got=dt::ValidateCanonicalDateV1(O(changed,0));Check(!got.ok()&&got.diagnostic.diagnostic_code==row.outcome,"sealed profile mutation executed");Mark({row.case_id});++replayed;}Check(replayed==94,"all sealed material mutations executed");}
void VerifySealedReplayClosure(){
  Check(executed_rows.size()==216,"exactly 216 sealed rows executed through typed production APIs");
  Check(boundary_rows.size()==3,"exactly three untyped boundary rows classified unrepresentable");
  for(const auto& row:kSealedIntrinsicRows){const bool e=executed_rows.contains(row.case_id),b=boundary_rows.contains(row.case_id);Check(e!=b,row.case_id);}
}
}
int main(){SealedIntrinsicInventory();Civil();Arithmetic();Facts();RegistryAndRefusal();ParseRenderIdentity();NullableAndTextCarriers();ExactFiniteSealedReplay();ExactProfileMutationReplay();VerifySealedReplayClosure();std::cout<<"PASS base.date intrinsics checks="<<checks<<'\n';}
