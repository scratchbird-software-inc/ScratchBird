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
p::Uuid D708(){return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x08}};}
std::shared_ptr<const dt::DateValidatedProfileHandleV1> Profile(){auto r=dt::BuildCurrentDateValidatedProfileHandleV1(D708());Check(r.ok(),"profile");return std::make_shared<dt::DateValidatedProfileHandleV1>(std::move(r.profile));}
dt::DateValueViewV1 V(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& p,int32_t d){return {p.get(),dt::DateValueStateV1::value,d};}
dt::DateOwnedValueV1 O(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& p,int32_t d,dt::DateValueStateV1 state=dt::DateValueStateV1::value){return {p,state,d};}
const dt::DatatypeTypeCodecIdentityRowV3* TextIdentity(){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.catalog_snapshot_uuid==D708()&&row.legacy_fields.catalog_generation==8&&row.legacy_fields.registry_generation==8&&row.legacy_fields.canonical_binary_type_code==static_cast<uint32_t>(dt::CanonicalTypeId::character))return &row;return nullptr;}
scratchbird::engine::ExecutionTypeDescriptor TextDescriptor(){auto m=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(m.ok(),"text catalog manifest");auto row=dt::LookupDatatypeCatalogRow(m.manifest,dt::CanonicalTypeId::character);Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"text catalog row");dt::CatalogExecutionTypeMetadata md;md.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;md.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;auto d=dt::LookupExecutionTypeDescriptorFromCatalog(dt::CanonicalTypeId::character,md);Check(d.ok(),"text descriptor");return d.descriptor;}
struct Cancel{unsigned calls=0,at=0;};bool Stop(void* p) noexcept {auto& c=*static_cast<Cancel*>(p);return ++c.calls==c.at;}
struct SealedIntrinsicRow{std::string_view case_id,operation,outcome,row_sha256;};
constexpr SealedIntrinsicRow kSealedIntrinsicRows[]{
{"construct_epoch","civil_construct","VALUE","27d5c2e5cbfd315450db8106d62370cb9d0ea1ac2ed9b4a1324b11541b2dfb30"},
{"construct_year_zero_leap","civil_construct","VALUE","e5775a2884cf371768389033f8f9c5f8839a2ffa5aa78b03de184641e953ddda"},
{"construct_negative_leap","civil_construct","VALUE","e79206c7b4f5145a51c66ded20018a5bbb4d3a529858879d540bae012ddb5c24"},
{"construct_1900_common_refusal","civil_construct","CTI.TEMPORAL.INVALID_LITERAL","7b2a37efca701ceee849bec5875b521b42ded526d9447c659c4d1beabc09ff4d"},
{"construct_2000_leap","civil_construct","VALUE","81647ddc468ca1e1c063fa87fc997bd27f3db51a8d16ca341b6d41d4d86405d6"},
{"construct_zero_month","civil_construct","CTI.TEMPORAL.ZERO_DATE_REFUSED","4760b395b7a73ba42520cb4b79ff1c3bd08af1f667c6ef9985127c43c2993ee6"},
{"construct_null_component","civil_construct","SQL_NULL","bbdedab072aef1ac178150f68dbc0c62f52f5442d4972382d024eded25efe367"},
{"decompose_epoch","decompose_civil","VALUE","d1a5ccdd03e168fd1cdedbacee809a3a1e3864a91953e5e3b9b5d595768e2fae"},
{"add_max_boundary","add_days","VALUE","5b3f6dee66a580de81a86450ad9f2e8e5edbb54a17d4f0f1a69e65691856e619"},
{"add_overflow","add_days","CTI.TEMPORAL.RANGE_EXCEEDED","204904b70a4a1973320208720c20a39fbed9c8408919aae1963e374be6b54e96"},
{"add_i64_max","add_days","CTI.TEMPORAL.RANGE_EXCEEDED","ca249192df74351323c49147caeb1e607d6fc922ed6cb93fa8b76941d365447a"},
{"subtract_min_boundary","subtract_days","VALUE","ae17d8493d88e80e98c75c9ed29f75540eb0296008add61c67a68f03d9a07a48"},
{"subtract_underflow","subtract_days","CTI.TEMPORAL.RANGE_EXCEEDED","1ef933e1515d196a045b1cb34907b89d954c5bfd31ca2beb6cb68fed0cf8dfde"},
{"subtract_i64_min","subtract_days","CTI.TEMPORAL.RANGE_EXCEEDED","f6c572493051e67f14719eb267ad43ba1add08e3135a3bd676a708772ea21ffc"},
{"successor_max_refusal","successor","CTI.TEMPORAL.RANGE_EXCEEDED","8457363db4124a365f577d6af23b2f228d6654ef9d2b48fca743f4db6910f222"},
{"predecessor_min_refusal","predecessor","CTI.TEMPORAL.RANGE_EXCEEDED","0558f2a4eeb86dee5d2107ec7da328466260232e8a54ced0d531aae9b8c8832b"},
{"difference_full_positive","difference_days","VALUE","62bb5ffc1b8878f70f589f6a9880e1e6e9563332c2c53ba2717b7312f10de6af"},
{"difference_full_negative","difference_days","VALUE","facc35a77e44e2c595484a2100ec9cd1b31a1061037d994146729f59f3fd4f7c"},
{"weekday_epoch","iso_weekday","VALUE","560e9229829d7d067bd3a20b7101d6c5c29eaebf4ea7524eb8ca10e852649d89"},
{"weekday_before_epoch","iso_weekday","VALUE","e3835e7ef4f25383d3d79d27f8a2b1d5bfc80a8b1fc0091fb7b908b2392c71bd"},
{"leap_negative_400","is_leap_year","VALUE","7b53621ac2e3048f05f9e2464355eed3ca9c5b704871f05cdfc1f15be4e5cfe2"},
{"leap_zero","is_leap_year","VALUE","ca51a04dd4afd0aefd3f141c34d6dce677c62282ebade139abc56a18abb9b1e5"},
{"days_feb_1900","days_in_month","VALUE","1acfb52e0d9dabe9f9bc212769649ac48bb0fa354932959ac1d12d8355544be4"},
{"days_feb_2000","days_in_month","VALUE","8c6b254c985ac8f51a78ac694e1fb51f84632b1145eec19679345e51460897fb"},
{"day_of_year_leap","day_of_year","VALUE","66a4ba60f61af163b9eb441fb455feb57ea5b237a548c19223b17709f1340857"},
{"quarter_q4","quarter","VALUE","e488ad7dd2693e7efd14935bddc62071b778a9d114ab12eb51c9869a6f4e4772"},
{"iso_late_dec_next_year","iso_week","VALUE","57a6425448e919cd01189b0e9490799611c1fd58ac95824d91e28e5eab209a9c"},
{"iso_early_jan_prior_year","iso_week","VALUE","187241e6936d4316e53446503cda3a9f703d1f5f29c662e8f371caeef2b02680"},
{"iso_year_zero_boundary","iso_week","VALUE","f91a95bf3bbf40750c94a6c77412bf0c43db8fad912409c42c971e40cc7b5c9b"},
{"truncate_identity","truncate_day","VALUE","7681c312792ff40f1d1d005a408237d065e093b4bfe50bca7d789be9fc16d762"},
{"round_null","round_day","SQL_NULL","a023ca0f6b47fd31a86aaac3e5e99f363c608f1c9116d0be1e667837364a6279"},
{"dirty_null_refusal","add_days","DATATYPE.NULL_STATE.INVALID","24d81d75a05c6a21049533eadaf1a7947ec5dd394d2f1a9b8a5111f157069c33"},
{"unsupported_no_payload_inspection","calendar_interval_or_cross_temporal","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","4b7c308823135b4e335213e968d04ea3bcab5ad16d0588b065c19a51d45b8fee"},
{"cancellation_atomic","add_days","PROCESS.CANCELLED","7774a896e727f21a483bcc3ea2b9a12d444041c01d96d5a778984e4cfa98b981"},
{"validate_value","validate_canonicalize","VALUE","e302b8fd76b416fe427a270022d9edf12901a27aba18e5136481b5bca143631a"},
{"parse_epoch","parse_canonical","VALUE","e496b39fb1ff7a0b126b6563691d20b3aaf137f1c71f91873dd5d0f39a71e9bc"},
{"parse_expanded_alias_refusal","parse_canonical","CTI.TEMPORAL.INVALID_LITERAL","c037590382c1dbb2058fdd8f7ba98506445bdc5192922546707fb4602a94885c"},
{"render_standard","render_canonical","VALUE","514c1417319666d959718b0bf3c4f19f4caba62d4fa1c28ee15c935561478d6e"},
{"larger_truncate_refused","larger_truncate_round_or_bucket","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","86820a257eb5f2f1e4078116d8096e91b072e45f59bf5bb1557e9869d9157803"},
{"aggregate_dispatch_handoff","aggregate_min_max_count_dispatch","receiving_owner_handoff","be3893829a392976d1a07a1d45322c9f3f166cf5e6f3f4f3e19330d82ad2c63c"},
{"unknown_operation_refused","unregistered.operation","CTI.TEMPORAL.OPERATION_REFUSED","b62b703342e51fe4f1aa2c7ffa0e7c37bbafb15cb79a9519c55ad79c0414b578"},
{"known_refused_clean_null","calendar_interval_or_cross_temporal","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","fff0a4fb9d4d1b1f729354d7672d63bccdef54d589a1a2f31508944c38a71428"},
{"dirty_state_plus_unknown","unregistered.operation","DATATYPE.NULL_STATE.INVALID","991bdd74c12a087f6d74af99b7bee0a65874b83b5b7e91c488fdaec263f294bc"},
{"dirty_state_plus_known_refused","calendar_interval_or_cross_temporal","DATATYPE.NULL_STATE.INVALID","331b444477a77bee13f4b219498faffb27663d46c24988a253f3f3530b478af0"},
{"clean_null_plus_supported","successor","SQL_NULL","35d252c0576abf5a563009a12f64e5f9201b73a2f360afba63fe7afd25a1bfb3"},
{"unknown_clean_state","unregistered.operation","CTI.TEMPORAL.OPERATION_REFUSED","6005e6fe68f5e6d1df5b17b59ab78b9b1fec5923915bb0d64a9e067028a7d56c"},
{"known_refused_clean_state","larger_truncate_round_or_bucket","CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","93355d6662fde5caee01624c9a383693118bf1a2fe3569876effefd012bf2c3e"},
{"operation_profile_mismatch","successor","CTI.TEMPORAL.DESCRIPTOR_INVALID","8f08b4a872b2ea82db21e65222336b6ca15fcf48fcf2b7798ea381bfbaf3e573"},
{"operation_profile_missing","successor","CTI.TEMPORAL.DESCRIPTOR_INVALID","ddaa3560d87ffeb64e146d24e5d517dc1b4f2ac68e6320d3594a4674d8fb13ad"},
{"clean_null_decompose","decompose_civil","SQL_NULL","3e08e300ca09d08cf71e9a42f1de13d925b47c0205432112eded4d32fe911574"},
{"clean_null_render","render_canonical","SQL_NULL","4fb8a1bb63c035f52355adb7ffb2c558b1b45a434b241c01c12333c1444d1101"},
{"clean_null_difference","difference_days","SQL_NULL","7a5e049adba6468e9e219d14a7120a71265b84adf9702dd7b24c314524f5e7a9"},
{"clean_null_u8_fact","is_leap_year","SQL_NULL","943d752a5c6c857c0b2a8ac1d94ba14f08cb98a820043c998de0a4890ca6b534"},
{"clean_null_u16_fact","day_of_year","SQL_NULL","c18e3a1e72c3b467fc75d83d88659f780bd2e35c084306d6dc176525e568946a"},
{"clean_null_iso_week","iso_week","SQL_NULL","be8a0af437d64fa9e750e9c2e106ad1c8359e082befca842ecb1f6f8036e2f0e"},
{"render_expanded_14","render_canonical","VALUE","cfe14485d7ec62e974a714a44587dafac66325c64542731f69ec71ada8d3ba76"},
{"render_expanded_one_byte_short","render_canonical","CTB.TEXT.LENGTH_EXCEEDED","f2d242f2d664bb01d44d1d40bd650baa0f2a186b93710b9fbf0f101af996c11a"},
{"render_cancellation","render_canonical","PROCESS.CANCELLED","962ef893c98807e5b568b92380bfbc62e28563b86a0d9208638b6cac188c5af4"},
{"shared_handle_presentation_alias","validate_canonicalize","VALUE","09d6c4ed0e87fd59751d168d1b68884bf6c3cc7fec11fea1df88ab8deab44ebb"},
{"shared_handle_name_only_refused","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5576271ee1e82f34e9c40e3c4f55440e38015038ed0739db922d45a1b5bd760b"},
{"shared_handle_mut_profile_material_magic_ASCII_SBDATP01_offset_0","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8a48603a7476764260d440e4e6a8c64a8bc61a15432045018e850cba88c23b23"},
{"shared_handle_mut_profile_material_version_u16_le_1_offset_8","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","53c316cd349ec8b27ed6df85918e2fe6ccfa328d4e3140ba6675b1b1bbf5fbfd"},
{"shared_handle_mut_profile_material_material_bytes_u16_le_584_offset_10","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2d78fbb8d63d515e1ae75388c395f71a78115cdb4cc65866a8100ae4b24a93f0"},
{"shared_handle_mut_profile_material_total_bytes_u32_le_584_offset_12","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","99e7e981ca68be0f16c097c0c349c8644e1a673220bb463a6a849f5b678a30ff"},
{"shared_handle_mut_profile_material_snapshot_uuid_raw16_offset_16","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","15a085214340f14037162d5140b379200b8924106dbc417d54e715c0d6f6122e"},
{"shared_handle_mut_profile_material_catalog_generation_u64_le_offset_32","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","d5100514b676acceda2ddab39ece56467fb2515a06c1bef9b978e3904994054f"},
{"shared_handle_mut_profile_material_registry_generation_u64_le_offset_40","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9161fc4d3e7156c5b2f2973ce2f2f454da5665c347941bb3a992b761037ffc67"},
{"shared_handle_mut_profile_material_descriptor_uuid_raw16_offset_48","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4e7741e890a8ffc4af0ba2b83a961c42b5a99374378c46e7ef5a2bd6ead74104"},
{"shared_handle_mut_profile_material_descriptor_generation_u64_le_offset_64","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c4674b3f23f093dd3de82cbfc33874ddb05d6681dc310d9c44410269f7b0d9f3"},
{"shared_handle_mut_profile_material_type_uuid_raw16_offset_72","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","b0a19010b7aff53bf47dce5495903b5842ba6012709e7fb2e58fcc39a43e7744"},
{"shared_handle_mut_profile_material_type_generation_u64_le_offset_88","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","54c951a1dc205f0c7f764e756e144f7802a4e9e484c44d7e0ba7130a77670afc"},
{"shared_handle_mut_profile_material_codec_uuid_raw16_offset_96","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4f939bdfb8e22304020e92ca07095aa078f031f18d1076bb4b6fc00cbdc4aec2"},
{"shared_handle_mut_profile_material_codec_version_u32_le_offset_112","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","df2a76dc02552844cc56c72b0cc1aa584488b437f313896821bdf97e3eb58eea"},
{"shared_handle_mut_profile_material_reserved_zero_offset_116","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a4dfdf31229d725930ee08e4e37f96e59aaf197e7d08de2f1af05fa8c07ead5f"},
{"shared_handle_mut_profile_material_codec_generation_u64_le_offset_120","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a8084edcfe06481d1dd34ff62d32577ba40fd5da6412297d371c09eddbe610c8"},
{"shared_handle_mut_profile_material_descriptor_policy_uuid_raw16_offset_128","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9a38f5c1d3581ee5de24348f6c43bf687036a67a0ab1159dad92b723420163b3"},
{"shared_handle_mut_profile_material_descriptor_policy_generation_u64_le_offset_144","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","945842c6ec315b23c7223c6726ce79a7e9fed35750d00041969900787240f823"},
{"shared_handle_mut_profile_material_canonicalization_policy_uuid_raw16_offset_152","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2446ec05622af4b3186607621fb7eca4dfcb8390d311bd06d4c076f84e16a730"},
{"shared_handle_mut_profile_material_canonicalization_policy_generation_u64_le_offset_168","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","0e4f956c214ff0bdfdde70c532664ac317370938b76dff2feb97926f8be6f9f8"},
{"shared_handle_mut_profile_material_ordering_policy_uuid_raw16_offset_176","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7a1d6afbc573d0626995dab373f127c74a12b7e6e66bc90f96cc6ee0c9062013"},
{"shared_handle_mut_profile_material_ordering_policy_generation_u64_le_offset_192","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a6e96ba783e165a8584e528f571953df0895e77c7bee91b2fe56e33ffc0570e6"},
{"shared_handle_mut_profile_material_hash_policy_uuid_raw16_offset_200","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","3a9d5d9823d36d3d9a69c4162281d532adefd24e5e3738e7160c97fc621a56d8"},
{"shared_handle_mut_profile_material_hash_policy_generation_u64_le_offset_216","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","289eead8815370b83edd2f84fb13914adc2ff488728b0ba5032c0bc2ffa7608e"},
{"shared_handle_mut_profile_material_operation_policy_uuid_raw16_offset_224","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6ea87d15bdd0576c41ed1894f416df2be6dcfa953ecf18a3d221db3acf77c724"},
{"shared_handle_mut_profile_material_operation_policy_generation_u64_le_offset_240","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","78d4cc3418408ccb01a8f2b6c6c2195bd82f698950124bca14150e4c40c5f516"},
{"shared_handle_mut_profile_material_render_policy_uuid_raw16_offset_248","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a1f7f36555bbc8f9009d16931114af96b7eac96a9373a850028a9ada0aff6095"},
{"shared_handle_mut_profile_material_render_policy_generation_u64_le_offset_264","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","3bcbe26acd4f6f8af139961f1bc84616b477369c9e38a7dc5cfce2979a29d410"},
{"shared_handle_mut_profile_material_cast_policy_uuid_raw16_offset_272","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ae5ef6a6465be38039e06fa1ed208785ee6947080fb7beeda684fe3c1c7ebcce"},
{"shared_handle_mut_profile_material_cast_policy_generation_u64_le_offset_288","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","aea2f356847fc5c4048dc7e40f28c3a60286270569c5ea16261af03e6faac112"},
{"shared_handle_mut_profile_material_calendar_policy_uuid_raw16_offset_296","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","601d4e3c64c40fd637aa50e4f5ca703d70003da3d58a9e53d2371c933657d304"},
{"shared_handle_mut_profile_material_calendar_policy_generation_u64_le_offset_312","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c1b489380f89dc24862d516acb27032025b80e75ca7b7b47e5e4ad0786bd3a8a"},
{"shared_handle_mut_profile_material_storage_epoch_policy_uuid_raw16_offset_320","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","17413c5cf6f8f60fea76647f3e94273d9426c30bd5e5d7bd60f0bf7e932d97fa"},
{"shared_handle_mut_profile_material_storage_epoch_policy_generation_u64_le_offset_336","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","768a1958d2c7e0e0e9a6024565da821f5788a3c5331951e28769c33558a147de"},
{"shared_handle_mut_profile_material_timezone_none_policy_uuid_raw16_offset_344","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","047e53564d33bc65640f6707acb2e945eb48bed249ddb310ddc3a19a1ec963cb"},
{"shared_handle_mut_profile_material_timezone_none_policy_generation_u64_le_offset_360","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2d288648e7250db07ba25cd9b8d43b2710a94927b6b71fe7e989e9387ff25ce1"},
{"shared_handle_mut_profile_material_leap_na_policy_uuid_raw16_offset_368","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1c3698d12a2d995a1e4b94568463f006b9d5cfcd3ca01c33452938c93575458f"},
{"shared_handle_mut_profile_material_leap_na_policy_generation_u64_le_offset_384","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","26374e22792a1686357a6cc2c3214b6e6e63985f9720a2b43cf09efc52ff4ff2"},
{"shared_handle_mut_profile_material_index_policy_uuid_raw16_offset_392","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","63366b5e3944c99825ff1d62bb5934499b6ef6d9dc988c5c8dec6e72b7862e01"},
{"shared_handle_mut_profile_material_index_policy_generation_u64_le_offset_408","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","66b46b1a334dc060abb2431daae5081f719eaf97c4593ae5f9c086908aa658f0"},
{"shared_handle_mut_profile_material_statistics_policy_uuid_raw16_offset_416","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","3ef923f038995f4c4453f779bf933aef476c6521a7dc3fd129ffbc929c0ce772"},
{"shared_handle_mut_profile_material_statistics_policy_generation_u64_le_offset_432","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","77817236a44f08a5e5f927d8200c8ea4acb3fc8c7f05df71a3c79cbe58bb5159"},
{"shared_handle_mut_profile_material_backup_policy_uuid_raw16_offset_440","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c37a47659b5f16565481bde0428f9278db973c9e31e6c64cd8cccaee89e935fc"},
{"shared_handle_mut_profile_material_backup_policy_generation_u64_le_offset_456","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2ee1433f572e9f1c6a8413b0ea96b2cf6d47eaa368595b726122d9b84ff0329c"},
{"shared_handle_mut_profile_material_protection_policy_uuid_raw16_offset_464","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","4d91980d839c8dc817abb96820d130ec1ae60a3eb757b5a152efbf4fbe8e5a99"},
{"shared_handle_mut_profile_material_protection_policy_generation_u64_le_offset_480","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","e51034307bb94b190c650f4e54cdb61b1d50d6d8277c79df7b34a9c4a128a7f6"},
{"shared_handle_mut_profile_material_component_policy_uuid_raw16_offset_488","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","270d8d6eef63042555014058fed2a9dced6a3c328ff086f402ad9df26385600a"},
{"shared_handle_mut_profile_material_component_policy_generation_u64_le_offset_504","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","21071f5bf6e694ef93bbb342028f8591500c2dacf14694d9e815c8703a1f918e"},
{"shared_handle_mut_profile_material_diagnostic_policy_uuid_raw16_offset_512","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5501682e167d587bd06242df8ab5ddc39c0210f463d302aec00ac9e029b9c502"},
{"shared_handle_mut_profile_material_diagnostic_policy_generation_u64_le_offset_528","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","17e3768dba958c2fe1cef85d40e3f1aa1d7f00142c6d7f48b5cfe2196138c6f0"},
{"shared_handle_mut_profile_material_metric_policy_uuid_raw16_offset_536","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","833af3ba72292184ebd5622cace47c07f5b788c2d8b593693606bab627ac9c95"},
{"shared_handle_mut_profile_material_metric_policy_generation_u64_le_offset_552","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1659753253572701110b67424a6bb450b3ab091ebb090db764ee400fe89350cf"},
{"shared_handle_mut_profile_material_minimum_day_i32_le_offset_560","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","22d6a6a88f731a522bbabcfd9bd5c10d83dad84b97e59e80968238d15d78c3a6"},
{"shared_handle_mut_profile_material_maximum_day_i32_le_offset_564","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6a0dff1cee8dcea0340270f57c37f8a4f967324832b9029be81ffd6c2f2ccd1e"},
{"shared_handle_mut_profile_material_epoch_day_i32_le_offset_568","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","a57aa24884109b7c055473807420f72397425bda7dfd299f68e9d47504b60e0b"},
{"shared_handle_mut_profile_material_component_bytes_u32_le_offset_572","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","1e551efdfae990bde4e98ce9206a6e96651672946202216233392809c9c94659"},
{"shared_handle_mut_profile_material_flags_u32_le_offset_576","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","7dfb556073539ebc3cf6ace953ebcb953bf6665bb0a8a7a144f0d8cc6352cb94"},
{"shared_handle_mut_profile_material_reserved_zero_offset_580","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6804b78b34782951f0ba14a14eb4482c57b24949cc48947593da8ad344498ff8"},
{"shared_handle_mut_comparison_material_magic_ASCII_SBDACC01_offset_0","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c83d5d47d0a5f46c6f02e9cf2e1e2be4cc9207233aedcd54342ed9c13888dc1a"},
{"shared_handle_mut_comparison_material_version_u16_le_1_offset_8","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8836914109f899d28f066f6a565d6a8827f6abfeeeffaa287c205400b6132fe9"},
{"shared_handle_mut_comparison_material_material_bytes_u16_le_344_offset_10","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","879a0fa4f9151254ae8dd94c4c98d214a45cc80b634120ca9ea1fc82927b74a6"},
{"shared_handle_mut_comparison_material_reserved_zero_offset_12","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","45fc853311887214707f2891fea943f4e0b58c9971ad51b640172766eb49d26b"},
{"shared_handle_mut_comparison_material_snapshot_uuid_raw16_offset_16","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f68720acdeaa87a7857ef2bea64fed3e9dab5fbb87aee918df42c51da15ce0a3"},
{"shared_handle_mut_comparison_material_catalog_generation_u64_le_offset_32","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","d971148567854844d7ee10153b3c8df2e2b648c69bbc99d5b3ed3cdaa95033c3"},
{"shared_handle_mut_comparison_material_registry_generation_u64_le_offset_40","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","af32c52ab0e289e96a725b235aeada230dd892cd5337c7e103b98fe838544ed1"},
{"shared_handle_mut_comparison_material_descriptor_uuid_raw16_offset_48","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","317e2e1f06ca33fec8446abd5be315f1a9948b5f36354ee00c2f37952371d90b"},
{"shared_handle_mut_comparison_material_descriptor_generation_u64_le_offset_64","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","382c34972dece2ac404781406908cbe9548a813091013f25ef7bc8215e860d3c"},
{"shared_handle_mut_comparison_material_type_uuid_raw16_offset_72","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","0657fc2c97254c2bb25a95da67b778d85aa56591c610f96b9b5f97b2aeaade2f"},
{"shared_handle_mut_comparison_material_type_generation_u64_le_offset_88","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","df2f716e89a0c40e9239ae977ef6d5f4d40dd50a68223fe002b24b0ab50f38bf"},
{"shared_handle_mut_comparison_material_codec_uuid_raw16_offset_96","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","fe776bd87ddc8bc8f2eb7e406feeb118e798a291081a76f73b0f1d35d704dd3a"},
{"shared_handle_mut_comparison_material_codec_version_u32_le_offset_112","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","70e87f23b231bb01e6724f03e909750767d81cd9a0d4b969badfbaf09742a667"},
{"shared_handle_mut_comparison_material_reserved_zero_offset_116","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","53bb6acb44efcff5c5263c034cc91d8263d525443ef470a7d9536ba62ecbe1c3"},
{"shared_handle_mut_comparison_material_codec_generation_u64_le_offset_120","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9c53f70b10d1a7f5f931d0696dbb7a84de64d6175d911fbb9dbeea8cb07ff1cc"},
{"shared_handle_mut_comparison_material_descriptor_policy_uuid_raw16_offset_128","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","b81e7e98c3ab3c6f5f9fe72f8714ca58ed2e6cf6e5e07b91b0718404c3ef52fd"},
{"shared_handle_mut_comparison_material_descriptor_policy_generation_u64_le_offset_144","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5a3d939b1be2362a52d1b57560ac6a824f58f13b73bcad5d89b48c2d61e4cc46"},
{"shared_handle_mut_comparison_material_canonicalization_policy_uuid_raw16_offset_152","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8d8338f2d9be967546457d5c0ebabf1e4cf8b03a8fab315a18e3600e1b8946df"},
{"shared_handle_mut_comparison_material_canonicalization_policy_generation_u64_le_offset_168","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","335a6c6d0fd36c7f3e18194c4525d94a7248358c9e121698114658b8030e74e8"},
{"shared_handle_mut_comparison_material_ordering_policy_uuid_raw16_offset_176","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","e93c8f5c730100c41bdf2632d6e8ed893dbd56a43115da9effa8245c33a8c86e"},
{"shared_handle_mut_comparison_material_ordering_policy_generation_u64_le_offset_192","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9619b8a3f35562ec73a551b290e945d678e441cb4e2450005e7ed725dbf09dda"},
{"shared_handle_mut_comparison_material_calendar_policy_uuid_raw16_offset_200","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","b4eaac963593e9a5a9acdb2458cbedbee51d53ef925739981108c51154d7353c"},
{"shared_handle_mut_comparison_material_calendar_policy_generation_u64_le_offset_216","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8c7bd1c0c0b3531e9a6ba07cb54ef2b687fc4b00364c494fe4ad0751bb1242be"},
{"shared_handle_mut_comparison_material_storage_epoch_policy_uuid_raw16_offset_224","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","c13fea2c4ac1d5390a058a73b8f34cb33506ebe81fcf19f3f957d41a117518a7"},
{"shared_handle_mut_comparison_material_storage_epoch_policy_generation_u64_le_offset_240","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ce8bf7e4f945396e01574f5f8ce0a23ac84909c37eecbb4bc6fbc402d93f6cf5"},
{"shared_handle_mut_comparison_material_timezone_none_policy_uuid_raw16_offset_248","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","6b4bfe1067d7c26f2a24c560ba25756e3b7895e30d11351ae4291594a2e24b7f"},
{"shared_handle_mut_comparison_material_timezone_none_policy_generation_u64_le_offset_264","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","f96b767428024a46cf658d0bbc5ae774bb06bd225fab65667ca6e588beddc520"},
{"shared_handle_mut_comparison_material_leap_na_policy_uuid_raw16_offset_272","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","cf06cb99f15432428d7541eb3761242f1b385cc2cf040dfd6ee7e4e2d7d6a231"},
{"shared_handle_mut_comparison_material_leap_na_policy_generation_u64_le_offset_288","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","5e754cbca8a12e115186ae717ef73a5c301d6b1492a46944322eae4c63d83b91"},
{"shared_handle_mut_comparison_material_hash_policy_uuid_raw16_offset_296","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","ae895479710d8ff2eaab174eba7d949a70f69544f84caed639529d2aaf989cd1"},
{"shared_handle_mut_comparison_material_hash_policy_generation_u64_le_offset_312","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","24c5ba62de5ea5162f2f46667eff4fd87e0b33f686201140b1618e107bb4e1c7"},
{"shared_handle_mut_comparison_material_flags_u32_le_offset_320","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","92b83c525d31d9dc37d87d79dd695b2c88edb6a4ee3b8f5ddd97248c68583ba9"},
{"shared_handle_mut_comparison_material_minimum_day_i32_le_offset_324","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","9e706f57d8aee460d77f2c7b6b58b8b585ef38ef1f096257402cc30b50d2ae85"},
{"shared_handle_mut_comparison_material_maximum_day_i32_le_offset_328","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","d3ad6416f7baa3d8672b21a950d8337d0ac84c6d9c5508f9cf189ff545e42ca6"},
{"shared_handle_mut_comparison_material_epoch_day_i32_le_offset_332","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","2587ef62c68b6241940c84ebd4f60aeaba73d33a3f21a3bcef584cd8d949e7dc"},
{"shared_handle_mut_comparison_material_component_bytes_u32_le_offset_336","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","31ff80f366ace7c0632d4999f5f227d3e3ff001d89bb413d10068d122831f46a"},
{"shared_handle_mut_comparison_material_reserved_zero_offset_340","validate_canonicalize","CTI.TEMPORAL.DESCRIPTOR_INVALID","8b9d18fa736e2674f3e2bdb3c48641f50d5dcbda5c7a81d317a0a55e9944c53c"},
{"text_wrong_receipt_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","23b1bc6729b580e17c02146f25f1eae7731cc54b6e1c268150643fddf61dbf72"},
{"text_wrong_catalog_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","84cae7d8a460179b123471a5886297470f0d2066bd153ea91290084df8cfc167"},
{"text_wrong_registry_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","9c1bcda9f3826e7b6e87830ac6e44f7565c093c7864f07bd496728623d64f46e"},
{"text_wrong_descriptor_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","3be69769f71bac1c3c8177512c27f60c136f10dd427da26a1bea6f638c9a907e"},
{"text_wrong_descriptor_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","28ef66902ad20965574d8c001e0df26b65c9587dbcefbd5ceaa0b87d6e8175aa"},
{"text_wrong_type_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","db2dfdef08f0d7d496dbf84e611677513ad8d52f42417a0e8d5e1090c7eea532"},
{"text_wrong_type_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","82d32b82e473a7e984d11eadb4d6f7be602c2198892ae47fb52d14c2f3e456e8"},
{"text_wrong_codec_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","000b1bfc7992a4d78e5920b4239d1e8194c841eec6b0638adc97245db0842a33"},
{"text_wrong_codec_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","5fb6cbe6650d0ecf3ba3d58f7bf27a2b5bb82c7032ce335b7f479c7b603d3722"},
{"text_wrong_codec_version","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","550f92a6f972dac24abf18e2565c236b4a0bd1c609b9bb6b8629b124408e52ec"},
{"text_metadata_alias_row_id","parse_canonical","VALUE","d76b394b97ff92cbd3fba8574597ffeeab9337b55f706e4a925717229e2df0e3"},
{"text_metadata_alias_codec_id","parse_canonical","VALUE","c861641639c9be1ad571b65244eef597b9f055e22988f811523f7d0882e7af90"},
{"text_metadata_alias_authority_source","parse_canonical","VALUE","d97be42f87b8f271e6f9fb3b6cbec9b9802435618dd81ea2772a730958652b86"},
{"text_profile_wrong_descriptor_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","ca8982259a6675b17e17e1601b72e7a1cfc6308f351eba754cf2eebe042c08ce"},
{"text_profile_wrong_descriptor_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","e56ffd0c4153bd1a82e52154c23580ebd531da1c7821d82a9a2fa284b2cba015"},
{"text_profile_wrong_type_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","84774a80d280f28e5bc3de5d32c85a1894eb30ab8e60077f342ce1f6d93d4677"},
{"text_profile_wrong_type_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","c5ff41700298caf3f826e69bda6139fb7377b1e6f3e1583efcd122e02508da65"},
{"text_profile_wrong_codec_uuid","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","5fc8d8bc849d70b2572a04206c130be7b6ba1140394e5e9e2380827ea698df93"},
{"text_profile_wrong_codec_generation","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","035bda5178a510c01c3c3a4d5e43154e52c554d4efd1855a71955317394e133f"},
{"text_profile_wrong_charset","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","7a0f62ae97f8b6864702dea930def9e5388a5915b9260de780d9f6e5d91e226f"},
{"text_profile_wrong_encoding","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","44783078602be686edc09b66011392279ada42130e47c97d296e2c90382d8d02"},
{"text_profile_wrong_null_state","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","93b3ad3e1a18f28b00a839435140533665e27e2c2c2024ce68a89d6283cabe06"},
{"text_profile_wrong_minimum_bytes","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","433d17149ef07a36770d3d05f3f125ae5d98928e436eb2d91aabedecc85effd9"},
{"text_profile_wrong_maximum_bytes","parse_canonical","CTI.TEMPORAL.DESCRIPTOR_INVALID","5feb8908384766ca0b669bfb3932ca635147a5ec2d129c5a93c2d36185c38d61"},
{"text_profile_metadata_alias_descriptor_handle","parse_canonical","VALUE","4ad305a4648fd939e75155a3a0f18536523f43919218a73b42a35bef00760d6e"},
{"text_profile_metadata_alias_authority_source","parse_canonical","VALUE","ca7513f99b0add995d2928a628f990f40d0acca78f3616c128b4ca9db2ebdd1e"},
{"null_matrix_validate_canonicalize_operand","validate_canonicalize","SQL_NULL","81e2581d99668d6964b924969252cf882ad050ea3e5cc4d02795a1a839b555da"},
{"null_matrix_decompose_civil_operand","decompose_civil","SQL_NULL","933981d7cfd99f85360cb0d1fb6740171ab67395538870e24768d2180f5d15eb"},
{"null_matrix_render_canonical_operand","render_canonical","SQL_NULL","7aad6fda701813f9945a343069825240f8736d424e7014edfc7850edb84fce6b"},
{"null_matrix_successor_operand","successor","SQL_NULL","4c78046901efbf9ea98de3a5070842c6ed940e7caef4d651bc47b3a8808a3f44"},
{"null_matrix_predecessor_operand","predecessor","SQL_NULL","dbe82dd384ed35587bc345ac9c7e22466c75f66a2c05045c878f97c4f24d823c"},
{"null_matrix_is_leap_year_operand","is_leap_year","SQL_NULL","76bdb3298d528691d71ac98bc962775baa620087361e68d46c99c82a99029fae"},
{"null_matrix_days_in_month_operand","days_in_month","SQL_NULL","7de37cfb794985d1384eff6191921f47e8609a31ed2b38e52990bc381851a08d"},
{"null_matrix_iso_weekday_operand","iso_weekday","SQL_NULL","744b2d187589fe4298a73d0d5b6f03caf2875761eba1b230c83a5eb2d0f24726"},
{"null_matrix_day_of_year_operand","day_of_year","SQL_NULL","76733e0f50f39ecb5f7bda412825487188850bc8a54bc214e5d02dbc1c4b6571"},
{"null_matrix_quarter_operand","quarter","SQL_NULL","c58631df0b3fe84779e4faa7cf636ad16a363be9ac73b6c75874ee787fd03341"},
{"null_matrix_iso_week_operand","iso_week","SQL_NULL","0e5ab3fc48e67c5458fe0bb2b18f9f3fcb68d16098788189698356a86213c2b4"},
{"null_matrix_truncate_day_operand","truncate_day","SQL_NULL","a50605a36975e6179a49b736efccd012e723a60eeed10e6db2343accb8b4d913"},
{"null_matrix_round_day_operand","round_day","SQL_NULL","9314eccda5d21f3b524d60c4239b7de57bfc9757dd27fca963ff60e3ef1de32a"},
{"null_matrix_parse_canonical_operand","parse_canonical","SQL_NULL","5e4b807a2235d37848babf54525908f14015beb258d3519111b1be905577e8f8"},
{"null_matrix_civil_construct_year","civil_construct","SQL_NULL","866f10b2b971a9eb391c5d3fa949c66a43eea0032c6f325d8548d578c12400df"},
{"null_matrix_civil_construct_month","civil_construct","SQL_NULL","56888761538c698a37ab29c61e702640e9031348122fa5e58749daf9955d5c26"},
{"null_matrix_civil_construct_day","civil_construct","SQL_NULL","85b23f9a745620f181dd42198a23bb1e8271177ec73fc246d01e395b0dcd198c"},
{"null_matrix_civil_construct_all","civil_construct","SQL_NULL","36dc1fbbf5fafedbc20e66f6196a085ab4dbef8816350ce21e79ccae9a28532c"},
{"null_matrix_add_days_date","add_days","SQL_NULL","b5769d43dc61236c29d3cbcd6cac816395e748872b584e88ad15c49d3b7ab29f"},
{"null_matrix_add_days_delta","add_days","SQL_NULL","c6ea3205ea3aaf9d97c44209a5045d93f3b762548b5550d49b4a01c3774206e5"},
{"null_matrix_add_days_both","add_days","SQL_NULL","898d8ef5ed759722615671024d9e53a169675b601c3adf24cff705248753b6ec"},
{"null_matrix_subtract_days_date","subtract_days","SQL_NULL","2d9f88c5d6e3db23cc76449ce61bedad847100a89dc3c38bae20800c1d236eaa"},
{"null_matrix_subtract_days_delta","subtract_days","SQL_NULL","650df9af604bdfa215e6c3d0a0f2621f069436d9348d64ad22ffca8a94ea25b1"},
{"null_matrix_subtract_days_both","subtract_days","SQL_NULL","4ce0994177d14d7a7daf15425615a03f16e885c72e1524de3adfa03b20053cb7"},
{"null_matrix_difference_days_left","difference_days","SQL_NULL","1ef38a3e621726bc1c3ed648467842fbee82154046a3083cbe3f7c2c113bc058"},
{"null_matrix_difference_days_right","difference_days","SQL_NULL","2003da5d2b8e9c7a471008824af0146ce7d1e8bfc3e2a0e783050ea51a0d58c2"},
{"null_matrix_difference_days_both","difference_days","SQL_NULL","1913754372d04f2143a248cc0410ae830b6f7331953710164debde98272454df"},
{"present_payload_wrong_type","validate_canonicalize","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","2fe0e358c3031531c0646164deed30cda15ed11c8bbacc87f4e4098ea8da1eb2"},
{"present_payload_below_i32","validate_canonicalize","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","09770e1dad639ffc9353e89debbe4cf51bd773075365db35d3b8452bc3c6e9ab"},
{"present_payload_above_i32","validate_canonicalize","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","25d6bd6024b9f8567811f2d86a87afc328657c2116365a52b13f67d98426db1b"},
{"i64_NULL_wrong_type","civil_construct","DATATYPE.NULL_STATE.INVALID","e5c5e25863677282b20eb8073a494b10688cf6326539e9356f0d8ef2ecef750a"},
{"i64_NULL_nonzero","civil_construct","DATATYPE.NULL_STATE.INVALID","b343752e57027139dd342822459ac981f2d1d03b299e0367554d6bdc0f6b7607"},
{"i64_PRESENT_wrong_type","civil_construct","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","cbf78dd68368908b935ff7b5afcbf04a6502d6c4cd26cecc55d6646f80bb889a"},
{"i64_PRESENT_below","civil_construct","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","a29070b31adea3dc05d99f25ae57eb4559c359ed82e3e5fb4295afaa7e3dab63"},
{"i64_PRESENT_above","civil_construct","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","33cc213db472f7253f683c7dd6392ac491620f092836da941cdd8804e91f7c44"},
{"text_NULL_dirty_bytes","parse_canonical","DATATYPE.NULL_STATE.INVALID","fcdbbd658c37bd4a49262868b25befe3353b359ffd7f3ef48e4e796c9e54d5e1"},
{"text_NULL_dirty_extent","parse_canonical","DATATYPE.NULL_STATE.INVALID","567cc9e776ce0f57e259eaccc36802d60f7bcd481c0db392c84188f2303dbf91"},
{"text_PRESENT_bad_hex","parse_canonical","CTI.TEMPORAL.INVALID_LITERAL","f226c2137e9744d816210d8d01f4043188703a5b821294bb4bf0a3a0056e1677"},
{"text_PRESENT_extent_mismatch","parse_canonical","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","62d41e32fd0a6757c284d6eedfa5734467ae3afce4cdbaa040820a6b7fd3c637"},
};
void SealedIntrinsicInventory(){Check(std::size(kSealedIntrinsicRows)==219,"sealed intrinsic row count");std::set<std::string_view> ids;uint64_t digest=1469598103934665603ULL;std::map<std::string_view,unsigned> counts;for(const auto& r:kSealedIntrinsicRows){Check(ids.insert(r.case_id).second,"unique sealed intrinsic case");Check(r.row_sha256.size()==64,"sealed row digest extent");++counts[r.operation];for(auto field:{r.case_id,r.operation,r.outcome,r.row_sha256}){for(unsigned char c:field)digest=(digest^c)*1099511628211ULL;digest=(digest^0)*1099511628211ULL;}digest=(digest^unsigned('\n'))*1099511628211ULL;}Check(digest==13594146058017175085ULL,"sealed intrinsic inventory digest");Check(counts["add_days"]==8,"sealed operation partition add_days");Check(counts["aggregate_min_max_count_dispatch"]==1,"sealed operation partition aggregate_min_max_count_dispatch");Check(counts["calendar_interval_or_cross_temporal"]==3,"sealed operation partition calendar_interval_or_cross_temporal");Check(counts["civil_construct"]==16,"sealed operation partition civil_construct");Check(counts["day_of_year"]==3,"sealed operation partition day_of_year");Check(counts["days_in_month"]==3,"sealed operation partition days_in_month");Check(counts["decompose_civil"]==3,"sealed operation partition decompose_civil");Check(counts["difference_days"]==6,"sealed operation partition difference_days");Check(counts["is_leap_year"]==4,"sealed operation partition is_leap_year");Check(counts["iso_week"]==5,"sealed operation partition iso_week");Check(counts["iso_weekday"]==3,"sealed operation partition iso_weekday");Check(counts["larger_truncate_round_or_bucket"]==2,"sealed operation partition larger_truncate_round_or_bucket");Check(counts["parse_canonical"]==33,"sealed operation partition parse_canonical");Check(counts["predecessor"]==2,"sealed operation partition predecessor");Check(counts["quarter"]==2,"sealed operation partition quarter");Check(counts["render_canonical"]==6,"sealed operation partition render_canonical");Check(counts["round_day"]==2,"sealed operation partition round_day");Check(counts["subtract_days"]==6,"sealed operation partition subtract_days");Check(counts["successor"]==5,"sealed operation partition successor");Check(counts["truncate_day"]==2,"sealed operation partition truncate_day");Check(counts["unregistered.operation"]==3,"sealed operation partition unregistered.operation");Check(counts["validate_canonicalize"]==101,"sealed operation partition validate_canonicalize");}
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
  auto* ti=TextIdentity();Check(ti!=nullptr,"current D708 text identity");auto td=TextDescriptor();dt::DateTextOperandV1 text{ti,&td,dt::DateTextCarrierKindV1::utf8_bytes,dt::DateValueStateV1::value,"1970-01-01",10};auto parsed=dt::ParseCanonicalDateOperandV1(p,text);Check(parsed.ok()&&parsed.value.day==0,"TextOperand canonical parse");
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
