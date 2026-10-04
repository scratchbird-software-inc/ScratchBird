// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_date.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>
namespace allocation_probe { bool counting=false;std::size_t allocations=0; }
void* operator new(std::size_t n){if(allocation_probe::counting)++allocation_probe::allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace { namespace dt=scratchbird::core::datatypes; namespace p=scratchbird::core::platform; unsigned checks=0;
[[noreturn]] void Fail(std::string_view s){std::cerr<<"FAIL: "<<s<<'\n';std::exit(1);} void Check(bool v,std::string_view s){++checks;if(!v)Fail(s);}
std::string_view Detail(const p::DiagnosticRecord& diagnostic){for(const auto& argument:diagnostic.arguments)if(argument.key=="detail")if(const auto* text=argument.text())return *text;return {};}
std::string_view Detail(const dt::DateDiagnosticFactV3& diagnostic){return diagnostic.detail;}
p::Uuid D710(){return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};}
std::shared_ptr<const dt::DateValidatedProfileHandleV3> Profile(){auto r=dt::BuildCurrentDateValidatedProfileHandleV3(D710());Check(r.ok(),"profile");return std::make_shared<dt::DateValidatedProfileHandleV3>(std::move(r.profile));}
scratchbird::engine::ExecutionTypeDescriptor CatalogDescriptor(dt::CanonicalTypeId type){auto m=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(m.ok(),"catalog manifest");auto row=dt::LookupDatatypeCatalogRow(m.manifest,type);Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"catalog descriptor row");dt::CatalogExecutionTypeMetadata md;md.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;md.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;auto d=dt::LookupExecutionTypeDescriptorFromCatalog(type,md);Check(d.ok(),"catalog execution descriptor");return d.descriptor;}
scratchbird::engine::ExecutionTypeDescriptor CharacterDescriptor(){return CatalogDescriptor(dt::CanonicalTypeId::character);}
const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.canonical_binary_type_code==static_cast<uint32_t>(type)&&row.legacy_fields.catalog_snapshot_uuid==D710()&&row.legacy_fields.catalog_generation==10&&row.legacy_fields.registry_generation==10)return &row;return nullptr;}
scratchbird::engine::ExecutionTypeDescriptor PeerDescriptor(dt::CanonicalTypeId type,const dt::DatatypeTypeCodecIdentityRowV3* identity){Check(identity!=nullptr,"peer identity required");auto d=CatalogDescriptor(type);Check(std::equal(std::begin(d.descriptor_uuid.bytes),std::end(d.descriptor_uuid.bytes),identity->legacy_fields.descriptor_uuid.bytes.begin())&&d.descriptor_epoch==identity->legacy_fields.descriptor_generation,"peer descriptor binds V3 identity");return d;}
struct CastShape{bool incoming=false,contextual=false,exact=false;dt::CanonicalTypeId peer=dt::CanonicalTypeId::unknown;};
CastShape Shape(unsigned row){static constexpr std::array<dt::CanonicalTypeId,29> a{{dt::CanonicalTypeId::boolean,dt::CanonicalTypeId::int8,dt::CanonicalTypeId::int16,dt::CanonicalTypeId::int32,dt::CanonicalTypeId::int64,dt::CanonicalTypeId::int128,dt::CanonicalTypeId::uint8,dt::CanonicalTypeId::uint16,dt::CanonicalTypeId::uint32,dt::CanonicalTypeId::uint64,dt::CanonicalTypeId::uint128,dt::CanonicalTypeId::bfloat16,dt::CanonicalTypeId::real16,dt::CanonicalTypeId::real32,dt::CanonicalTypeId::real64,dt::CanonicalTypeId::real128,dt::CanonicalTypeId::decimal,dt::CanonicalTypeId::decimal_float,dt::CanonicalTypeId::uuid,dt::CanonicalTypeId::ip_address,dt::CanonicalTypeId::network_prefix,dt::CanonicalTypeId::mac_address,dt::CanonicalTypeId::character,dt::CanonicalTypeId::binary,dt::CanonicalTypeId::bit_string,dt::CanonicalTypeId::date,dt::CanonicalTypeId::time,dt::CanonicalTypeId::timestamp,dt::CanonicalTypeId::interval}};static constexpr std::array<dt::CanonicalTypeId,56>b{{dt::CanonicalTypeId::blob,dt::CanonicalTypeId::document,dt::CanonicalTypeId::json_document,dt::CanonicalTypeId::binary_json_document,dt::CanonicalTypeId::bson_document,dt::CanonicalTypeId::xml_document,dt::CanonicalTypeId::hstore_document,dt::CanonicalTypeId::object_document,dt::CanonicalTypeId::flattened_object_document,dt::CanonicalTypeId::enum_value,dt::CanonicalTypeId::set_value,dt::CanonicalTypeId::array,dt::CanonicalTypeId::list,dt::CanonicalTypeId::map,dt::CanonicalTypeId::row,dt::CanonicalTypeId::composite,dt::CanonicalTypeId::variant,dt::CanonicalTypeId::range,dt::CanonicalTypeId::multirange,dt::CanonicalTypeId::token_stream,dt::CanonicalTypeId::search_query,dt::CanonicalTypeId::search_rank_feature,dt::CanonicalTypeId::search_completion,dt::CanonicalTypeId::search_percolator,dt::CanonicalTypeId::geometry,dt::CanonicalTypeId::geography,dt::CanonicalTypeId::point,dt::CanonicalTypeId::shape,dt::CanonicalTypeId::raster,dt::CanonicalTypeId::vector,dt::CanonicalTypeId::dense_vector,dt::CanonicalTypeId::sparse_vector,dt::CanonicalTypeId::binary_vector,dt::CanonicalTypeId::quantized_vector,dt::CanonicalTypeId::graph_node,dt::CanonicalTypeId::graph_edge,dt::CanonicalTypeId::graph_path,dt::CanonicalTypeId::time_series_value,dt::CanonicalTypeId::columnar_segment,dt::CanonicalTypeId::aggregate_state,dt::CanonicalTypeId::hll_sketch,dt::CanonicalTypeId::bloom_filter,dt::CanonicalTypeId::quantile_sketch,dt::CanonicalTypeId::histogram_sketch,dt::CanonicalTypeId::ranking_summary,dt::CanonicalTypeId::vector_summary,dt::CanonicalTypeId::lob_locator,dt::CanonicalTypeId::external_file_locator,dt::CanonicalTypeId::remote_object_locator,dt::CanonicalTypeId::bridge_handle,dt::CanonicalTypeId::cursor_handle,dt::CanonicalTypeId::system_reference,dt::CanonicalTypeId::opaque_extension,dt::CanonicalTypeId::cursor,dt::CanonicalTypeId::result_set,dt::CanonicalTypeId::table_value}};if(row==1)return{true,true,false,dt::CanonicalTypeId::null_type};bool incoming=row>=138;unsigned out=incoming?row-84:row;if(row>=2&&row<=26)return{true,false,true,a[row-2]};if(out>=28&&out<=56)return{incoming,false,true,a[out-28]};if(out>=57&&out<=112){bool exact=out==59||out==66||out==69||out==81;return{incoming,false,exact,b[out-57]};}if(out==27)return{false,false,false,dt::CanonicalTypeId::null_type};return{incoming,false,false,dt::CanonicalTypeId::unknown};}
struct Cancel{unsigned calls=0,at=0;};bool Stop(void* p) noexcept {auto& c=*static_cast<Cancel*>(p);return ++c.calls==c.at;}
struct PinProbe{unsigned calls=0,at=0;std::weak_ptr<const dt::DateValidatedProfileHandleV3> profile;long baseline=0;long observed=0;};
bool StopWithPin(void* p) noexcept{auto& x=*static_cast<PinProbe*>(p);++x.calls;x.observed=x.profile.use_count();return x.calls==x.at;}
std::vector<p::byte> Hex(std::string_view s){auto n=[](char c){return unsigned(c<='9'?c-'0':10+(c|32)-'a');};std::vector<p::byte> r(s.size()/2);for(size_t i=0;i<r.size();++i)r[i]=p::byte((n(s[2*i])<<4)|n(s[2*i+1]));return r;}
struct SortV{int64_t day;bool is_null;dt::DateSortDirectionV3 dir;dt::DateNullModeV3 mode;std::string_view hex;};
constexpr SortV sorts[]={
{0LL,true,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000000000"},
{0LL,true,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010200"},
{0LL,true,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000000"},
{0LL,true,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010200"},
{-2147483648LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000000010400000000"},
{-2147483648LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010400000000"},
{-2147483648LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104ffffffff"},
{-2147483648LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104ffffffff"},
{-2147483647LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000000010400000001"},
{-2147483647LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010400000001"},
{-2147483647LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104fffffffe"},
{-2147483647LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104fffffffe"},
{-865566LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff2cae2"},
{-865566LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff2cae2"},
{-865566LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800d351d"},
{-865566LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800d351d"},
{-755993LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff476e7"},
{-755993LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff476e7"},
{-755993LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800b8918"},
{-755993LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800b8918"},
{-720930LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff4ffde"},
{-720930LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff4ffde"},
{-720930LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800b0021"},
{-720930LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800b0021"},
{-719893LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff503eb"},
{-719893LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff503eb"},
{-719893LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800afc14"},
{-719893LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800afc14"},
{-719528LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff50558"},
{-719528LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff50558"},
{-719528LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800afaa7"},
{-719528LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800afaa7"},
{-719469LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff50593"},
{-719469LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff50593"},
{-719469LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800afa6c"},
{-719469LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800afa6c"},
{-719162LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ff506c6"},
{-719162LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ff506c6"},
{-719162LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800af939"},
{-719162LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800af939"},
{-135081LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047ffdf057"},
{-135081LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047ffdf057"},
{-135081LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000100010480020fa8"},
{-135081LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000101010480020fa8"},
{-25508LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047fff9c5c"},
{-25508LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047fff9c5c"},
{-25508LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001000104800063a3"},
{-25508LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000001010104800063a3"},
{-1LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000001047fffffff"},
{-1LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101047fffffff"},
{-1LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000100010480000000"},
{-1LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000101010480000000"},
{0LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000000010480000000"},
{0LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000"},
{0LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010001047fffffff"},
{0LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010101047fffffff"},
{1LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000000010480000001"},
{1LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000001"},
{1LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010001047ffffffe"},
{1LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010101047ffffffe"},
{11016LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000000010480002b08"},
{11016LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480002b08"},
{11016LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010001047fffd4f7"},
{11016LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010101047fffd4f7"},
{18628LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000000104800048c4"},
{18628LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010104800048c4"},
{18628LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010001047fffb73b"},
{18628LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010101047fffb73b"},
{2932896LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000000104802cc0a0"},
{2932896LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010104802cc0a0"},
{2932896LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010001047fd33f5f"},
{2932896LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010101047fd33f5f"},
{2932897LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000000104802cc0a1"},
{2932897LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010104802cc0a1"},
{2932897LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010001047fd33f5e"},
{2932897LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000010101047fd33f5e"},
{2147483646LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000000104fffffffe"},
{2147483646LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010104fffffffe"},
{2147483646LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000100010400000001"},
{2147483646LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000101010400000001"},
{2147483647LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000000104ffffffff"},
{2147483647LL,false,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010104ffffffff"},
{2147483647LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_first,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000100010400000000"},
{2147483647LL,false,dt::DateSortDirectionV3::descending,dt::DateNullModeV3::nulls_last,"53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000101010400000000"},
};
struct HashV{int64_t day;bool is_null;std::string_view hex;}; constexpr HashV hashes[]={
{0LL,true,"bc751611cb58ecf949a5428302ded46afff356ea216bfcf96fab7514c88643e9"},
{-2147483648LL,false,"57b64e489ab19e23df36abf6c45761ae38737821a665fd4b2829f7564e42d56e"},
{-2147483647LL,false,"bc6fcf32754905c027af610fbf6dbaeeb1802da1f4f54cda674307c18bae1e2c"},
{-865566LL,false,"36dd675b36083664bf79e5236619cfc788d78e6e39b4ef8fcaf82d70262c64c5"},
{-755993LL,false,"416e4386fb093e041be40838da3776659fd6b8daae9106efada080330c1fc905"},
{-720930LL,false,"3216cd3b0826637c479e22594ba14eab945a7face7dc14aa9da20b304e820cb1"},
{-719893LL,false,"e696f7889f20084840240b8e7d53842971487907d4d7f698e1b205927a3e2e89"},
{-719528LL,false,"3c5650c9248d5d4d30ac12eabb23d7c98df460cbdb8851a74402d16be3ce2c9e"},
{-719469LL,false,"c9077965347d71a0f5ecc999eb5b6587af9bc5efaf319ffa2bd32cba7f894fda"},
{-719162LL,false,"bfb2c2e783e838e2a28502b76772eef8ce3e30b50c81dd3c7b78ca71686ae73f"},
{-135081LL,false,"df308a8970c12f02ada32d7379fad86f854e14a0e0144d2d6a59593a861a4651"},
{-25508LL,false,"2851a66c55e567543126019334e3384318a9d048a07b69c5480bba8c36417cac"},
{-1LL,false,"c00231766b45f1c94727995a6a56cf28fc8fff6bef4808894795467e7bde147d"},
{0LL,false,"ee3175d6b5a2d432ef7e69fe350c38c96fd87af38f1ddd5c8fda237060a8c7cf"},
{1LL,false,"59471c02d755d8e0250417bc68ed7916c0187e305097c292d36a16ff891a0f77"},
{11016LL,false,"3dab7e15867e66c806f4893db9efd590321c97484dbe93aa0f878c31ac4effe6"},
{18628LL,false,"de619ebdf07cadcb6700944f6a55ca6fcc5b459b8237b26cd466a781b49ad305"},
{2932896LL,false,"7462d21a4dfb64134fa60395824c4c841f2422837d9e2dc96685af9d24ec7901"},
{2932897LL,false,"97de1af52eb58c463f3c6e087ec0c9ce2115ab2e72f4d2ed7339c1efcb6c494f"},
{2147483646LL,false,"61978d4f7971cd8e630f68a2bf02d791945cc11763c06139a857fec488f1b9a1"},
{2147483647LL,false,"8c41a1b3e5b1d19656c7c2842236e3c1d6540768da5fbe292ea5a007febfc447"},
};
struct CanonicalCastFixture{std::string_view id,text;std::int32_t day;};
constexpr std::array<CanonicalCastFixture,20> canonical_casts{{
  {"carrier_minimum","-5877641-06-23",-2147483648},
  {"carrier_minimum_plus_one","-5877641-06-24",-2147483647},
  {"negative_400_year_leap","-0000400-02-29",-865566},
  {"negative_100_year_common","-0000100-03-01",-755993},
  {"negative_4_year_leap","-0000004-02-29",-720930},
  {"negative_1_year","-0000001-01-01",-719893},
  {"astronomical_year_zero","0000-01-01",-719528},
  {"astronomical_year_zero_leap","0000-02-29",-719469},
  {"civil_year_one","0001-01-01",-719162},
  {"gregorian_400_leap","1600-02-29",-135081},
  {"gregorian_100_common","1900-03-01",-25508},
  {"before_epoch","1969-12-31",-1},
  {"epoch","1970-01-01",0},
  {"after_epoch","1970-01-02",1},
  {"leap_2000","2000-02-29",11016},
  {"iso_week_boundary","2021-01-01",18628},
  {"four_digit_maximum","9999-12-31",2932896},
  {"expanded_positive_start","+0010000-01-01",2932897},
  {"carrier_maximum_minus_one","+5881580-07-10",2147483646},
  {"carrier_maximum","+5881580-07-11",2147483647},
}};
struct InvalidTextCastFixture{std::string_view id,text,code;};
constexpr std::array<InvalidTextCastFixture,21> invalid_text_casts{{
  {"empty","","CTI.TEMPORAL.INVALID_LITERAL"},
  {"leading_space"," 1970-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"trailing_space","1970-01-01 ","CTI.TEMPORAL.INVALID_LITERAL"},
  {"short_year","970-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"plus_four_digit","+1970-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"unsigned_expanded","10000-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"short_expanded","+010000-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"long_expanded","+00010000-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"zero_month","1970-00-01","CTI.TEMPORAL.ZERO_DATE_REFUSED"},
  {"zero_day","1970-01-00","CTI.TEMPORAL.ZERO_DATE_REFUSED"},
  {"zero_date_sentinel","0000-00-00","CTI.TEMPORAL.ZERO_DATE_REFUSED"},
  {"month_thirteen","1970-13-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"common_feb_29","1900-02-29","CTI.TEMPORAL.INVALID_LITERAL"},
  {"day_too_large","2000-04-31","CTI.TEMPORAL.INVALID_LITERAL"},
  {"era_suffix","0001-01-01 BC","CTI.TEMPORAL.INVALID_LITERAL"},
  {"time_suffix","1970-01-01T00:00:00","CTI.TEMPORAL.INVALID_LITERAL"},
  {"below_minimum","-5877641-06-22","CTI.TEMPORAL.RANGE_EXCEEDED"},
  {"above_maximum","+5881580-07-12","CTI.TEMPORAL.RANGE_EXCEEDED"},
  {"expanded_alias_year_zero","+0000000-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"expanded_alias_1970","+0001970-01-01","CTI.TEMPORAL.INVALID_LITERAL"},
  {"expanded_alias_9999","+0009999-12-31","CTI.TEMPORAL.INVALID_LITERAL"},
}};
struct InvalidByteCastFixture{std::string_view id,hex,code;};
constexpr std::array<InvalidByteCastFixture,9> invalid_byte_casts{{
  {"plus_signed_zero_year","2b303030303030302d30312d3031","CTI.TEMPORAL.INVALID_LITERAL"},
  {"minus_signed_zero_year","2d303030303030302d30312d3031","CTI.TEMPORAL.INVALID_LITERAL"},
  {"minus_four_digit","2d303030312d30312d3031","CTI.TEMPORAL.INVALID_LITERAL"},
  {"non_ascii_arabic_indic_digits","d9a1d9a9d9a7d9a02dd9a0d9a12dd9a0d9a1","CTI.TEMPORAL.INVALID_LITERAL"},
  {"unicode_minus","e28892303030303030312d30312d3031","CTI.TEMPORAL.INVALID_LITERAL"},
  {"embedded_nul","313937302d30312d003031","CTI.TEMPORAL.INVALID_LITERAL"},
  {"trailing_lf","313937302d30312d30310a","CTI.TEMPORAL.INVALID_LITERAL"},
  {"trailing_crlf","313937302d30312d30310d0a","CTI.TEMPORAL.INVALID_LITERAL"},
  {"invalid_utf8","313937302d30312dff31","CTI.TEMPORAL.INVALID_LITERAL"},
}};
constexpr std::array<std::string_view,239> cast_semantic_ids{{
  "null_binding/implicit/success",
  "null_binding/implicit/dirty_null",
  "null_binding/implicit/target_nonnullable",
  "null_binding/implicit/wrong_target_handle",
  "null_binding/implicit/cancel_before_publish",
  "null_binding/implicit/zero_resource_grant_success",
  "null_binding/assignment/success",
  "null_binding/assignment/dirty_null",
  "null_binding/assignment/target_nonnullable",
  "null_binding/assignment/wrong_target_handle",
  "null_binding/assignment/cancel_before_publish",
  "null_binding/assignment/zero_resource_grant_success",
  "null_binding/explicit/success",
  "null_binding/explicit/dirty_null",
  "null_binding/explicit/target_nonnullable",
  "null_binding/explicit/wrong_target_handle",
  "null_binding/explicit/cancel_before_publish",
  "null_binding/explicit/zero_resource_grant_success",
  "character_to_date/implicit/forbidden",
  "character_to_date/assignment/forbidden",
  "character_to_date/explicit/canonical/carrier_minimum",
  "character_to_date/explicit/canonical/carrier_minimum_plus_one",
  "character_to_date/explicit/canonical/negative_400_year_leap",
  "character_to_date/explicit/canonical/negative_100_year_common",
  "character_to_date/explicit/canonical/negative_4_year_leap",
  "character_to_date/explicit/canonical/negative_1_year",
  "character_to_date/explicit/canonical/astronomical_year_zero",
  "character_to_date/explicit/canonical/astronomical_year_zero_leap",
  "character_to_date/explicit/canonical/civil_year_one",
  "character_to_date/explicit/canonical/gregorian_400_leap",
  "character_to_date/explicit/canonical/gregorian_100_common",
  "character_to_date/explicit/canonical/before_epoch",
  "character_to_date/explicit/canonical/epoch",
  "character_to_date/explicit/canonical/after_epoch",
  "character_to_date/explicit/canonical/leap_2000",
  "character_to_date/explicit/canonical/iso_week_boundary",
  "character_to_date/explicit/canonical/four_digit_maximum",
  "character_to_date/explicit/canonical/expanded_positive_start",
  "character_to_date/explicit/canonical/carrier_maximum_minus_one",
  "character_to_date/explicit/canonical/carrier_maximum",
  "character_to_date/explicit/invalid_text/empty",
  "character_to_date/explicit/invalid_text/leading_space",
  "character_to_date/explicit/invalid_text/trailing_space",
  "character_to_date/explicit/invalid_text/short_year",
  "character_to_date/explicit/invalid_text/plus_four_digit",
  "character_to_date/explicit/invalid_text/unsigned_expanded",
  "character_to_date/explicit/invalid_text/short_expanded",
  "character_to_date/explicit/invalid_text/long_expanded",
  "character_to_date/explicit/invalid_text/zero_month",
  "character_to_date/explicit/invalid_text/zero_day",
  "character_to_date/explicit/invalid_text/zero_date_sentinel",
  "character_to_date/explicit/invalid_text/month_thirteen",
  "character_to_date/explicit/invalid_text/common_feb_29",
  "character_to_date/explicit/invalid_text/day_too_large",
  "character_to_date/explicit/invalid_text/era_suffix",
  "character_to_date/explicit/invalid_text/time_suffix",
  "character_to_date/explicit/invalid_text/below_minimum",
  "character_to_date/explicit/invalid_text/above_maximum",
  "character_to_date/explicit/invalid_text/expanded_alias_year_zero",
  "character_to_date/explicit/invalid_text/expanded_alias_1970",
  "character_to_date/explicit/invalid_text/expanded_alias_9999",
  "character_to_date/explicit/invalid_bytes/plus_signed_zero_year",
  "character_to_date/explicit/invalid_bytes/minus_signed_zero_year",
  "character_to_date/explicit/invalid_bytes/minus_four_digit",
  "character_to_date/explicit/invalid_bytes/non_ascii_arabic_indic_digits",
  "character_to_date/explicit/invalid_bytes/unicode_minus",
  "character_to_date/explicit/invalid_bytes/embedded_nul",
  "character_to_date/explicit/invalid_bytes/trailing_lf",
  "character_to_date/explicit/invalid_bytes/trailing_crlf",
  "character_to_date/explicit/invalid_bytes/invalid_utf8",
  "character_to_date/explicit/sql_null",
  "character_to_date/explicit/sql_null_nonnullable",
  "character_to_date/explicit/dirty_null",
  "character_to_date/explicit/wrong_text_descriptor",
  "character_to_date/explicit/wrong_text_profile",
  "character_to_date/explicit/wrong_target_date_handle",
  "character_to_date/explicit/present_wrong_source_type",
  "character_to_date/explicit/cancel_before_parse",
  "character_to_date/explicit/cancel_before_publish",
  "character_to_date/explicit/resource_short",
  "character_to_date/explicit/both_source_target_invalid",
  "character_to_date/explicit/valid_source_invalid_target_dirty",
  "character_to_date/explicit/valid_handles_dirty_null_poison",
  "date_to_character/implicit/forbidden",
  "date_to_character/assignment/forbidden",
  "date_to_character/explicit/exact_capacity/carrier_minimum",
  "date_to_character/explicit/one_short/carrier_minimum",
  "date_to_character/explicit/exact_capacity/carrier_minimum_plus_one",
  "date_to_character/explicit/one_short/carrier_minimum_plus_one",
  "date_to_character/explicit/exact_capacity/negative_400_year_leap",
  "date_to_character/explicit/one_short/negative_400_year_leap",
  "date_to_character/explicit/exact_capacity/negative_100_year_common",
  "date_to_character/explicit/one_short/negative_100_year_common",
  "date_to_character/explicit/exact_capacity/negative_4_year_leap",
  "date_to_character/explicit/one_short/negative_4_year_leap",
  "date_to_character/explicit/exact_capacity/negative_1_year",
  "date_to_character/explicit/one_short/negative_1_year",
  "date_to_character/explicit/exact_capacity/astronomical_year_zero",
  "date_to_character/explicit/one_short/astronomical_year_zero",
  "date_to_character/explicit/exact_capacity/astronomical_year_zero_leap",
  "date_to_character/explicit/one_short/astronomical_year_zero_leap",
  "date_to_character/explicit/exact_capacity/civil_year_one",
  "date_to_character/explicit/one_short/civil_year_one",
  "date_to_character/explicit/exact_capacity/gregorian_400_leap",
  "date_to_character/explicit/one_short/gregorian_400_leap",
  "date_to_character/explicit/exact_capacity/gregorian_100_common",
  "date_to_character/explicit/one_short/gregorian_100_common",
  "date_to_character/explicit/exact_capacity/before_epoch",
  "date_to_character/explicit/one_short/before_epoch",
  "date_to_character/explicit/exact_capacity/epoch",
  "date_to_character/explicit/one_short/epoch",
  "date_to_character/explicit/exact_capacity/after_epoch",
  "date_to_character/explicit/one_short/after_epoch",
  "date_to_character/explicit/exact_capacity/leap_2000",
  "date_to_character/explicit/one_short/leap_2000",
  "date_to_character/explicit/exact_capacity/iso_week_boundary",
  "date_to_character/explicit/one_short/iso_week_boundary",
  "date_to_character/explicit/exact_capacity/four_digit_maximum",
  "date_to_character/explicit/one_short/four_digit_maximum",
  "date_to_character/explicit/exact_capacity/expanded_positive_start",
  "date_to_character/explicit/one_short/expanded_positive_start",
  "date_to_character/explicit/exact_capacity/carrier_maximum_minus_one",
  "date_to_character/explicit/one_short/carrier_maximum_minus_one",
  "date_to_character/explicit/exact_capacity/carrier_maximum",
  "date_to_character/explicit/one_short/carrier_maximum",
  "date_to_character/explicit/sql_null_zero_capacity",
  "date_to_character/explicit/sql_null_nonnullable",
  "date_to_character/explicit/dirty_null",
  "date_to_character/explicit/wrong_source_date_handle",
  "date_to_character/explicit/wrong_target_text_descriptor",
  "date_to_character/explicit/wrong_target_text_profile",
  "date_to_character/explicit/present_wrong_host_bool",
  "date_to_character/explicit/present_wrong_host_string",
  "date_to_character/explicit/present_out_of_i32_range",
  "date_to_character/explicit/cancel_before_render",
  "date_to_character/explicit/cancel_before_publish",
  "date_to_character/explicit/resource_short",
  "date_to_character/explicit/both_source_target_invalid",
  "date_to_character/explicit/valid_source_invalid_target_dirty",
  "date_to_character/explicit/valid_handles_dirty_null_poison",
  "date_identity/implicit/value/carrier_minimum",
  "date_identity/implicit/value/carrier_minimum_plus_one",
  "date_identity/implicit/value/negative_400_year_leap",
  "date_identity/implicit/value/negative_100_year_common",
  "date_identity/implicit/value/negative_4_year_leap",
  "date_identity/implicit/value/negative_1_year",
  "date_identity/implicit/value/astronomical_year_zero",
  "date_identity/implicit/value/astronomical_year_zero_leap",
  "date_identity/implicit/value/civil_year_one",
  "date_identity/implicit/value/gregorian_400_leap",
  "date_identity/implicit/value/gregorian_100_common",
  "date_identity/implicit/value/before_epoch",
  "date_identity/implicit/value/epoch",
  "date_identity/implicit/value/after_epoch",
  "date_identity/implicit/value/leap_2000",
  "date_identity/implicit/value/iso_week_boundary",
  "date_identity/implicit/value/four_digit_maximum",
  "date_identity/implicit/value/expanded_positive_start",
  "date_identity/implicit/value/carrier_maximum_minus_one",
  "date_identity/implicit/value/carrier_maximum",
  "date_identity/implicit/sql_null",
  "date_identity/implicit/sql_null_nonnullable",
  "date_identity/implicit/dirty_null",
  "date_identity/implicit/wrong_source_handle",
  "date_identity/implicit/wrong_target_handle",
  "date_identity/implicit/present_wrong_host_bool",
  "date_identity/implicit/present_wrong_host_string",
  "date_identity/implicit/present_out_of_i32_range",
  "date_identity/implicit/cancel_before_publish",
  "date_identity/implicit/resource_short",
  "date_identity/implicit/both_source_target_invalid",
  "date_identity/implicit/valid_source_invalid_target_dirty",
  "date_identity/implicit/valid_handles_dirty_null_poison",
  "date_identity/assignment/value/carrier_minimum",
  "date_identity/assignment/value/carrier_minimum_plus_one",
  "date_identity/assignment/value/negative_400_year_leap",
  "date_identity/assignment/value/negative_100_year_common",
  "date_identity/assignment/value/negative_4_year_leap",
  "date_identity/assignment/value/negative_1_year",
  "date_identity/assignment/value/astronomical_year_zero",
  "date_identity/assignment/value/astronomical_year_zero_leap",
  "date_identity/assignment/value/civil_year_one",
  "date_identity/assignment/value/gregorian_400_leap",
  "date_identity/assignment/value/gregorian_100_common",
  "date_identity/assignment/value/before_epoch",
  "date_identity/assignment/value/epoch",
  "date_identity/assignment/value/after_epoch",
  "date_identity/assignment/value/leap_2000",
  "date_identity/assignment/value/iso_week_boundary",
  "date_identity/assignment/value/four_digit_maximum",
  "date_identity/assignment/value/expanded_positive_start",
  "date_identity/assignment/value/carrier_maximum_minus_one",
  "date_identity/assignment/value/carrier_maximum",
  "date_identity/assignment/sql_null",
  "date_identity/assignment/sql_null_nonnullable",
  "date_identity/assignment/dirty_null",
  "date_identity/assignment/wrong_source_handle",
  "date_identity/assignment/wrong_target_handle",
  "date_identity/assignment/present_wrong_host_bool",
  "date_identity/assignment/present_wrong_host_string",
  "date_identity/assignment/present_out_of_i32_range",
  "date_identity/assignment/cancel_before_publish",
  "date_identity/assignment/resource_short",
  "date_identity/assignment/both_source_target_invalid",
  "date_identity/assignment/valid_source_invalid_target_dirty",
  "date_identity/assignment/valid_handles_dirty_null_poison",
  "date_identity/explicit/value/carrier_minimum",
  "date_identity/explicit/value/carrier_minimum_plus_one",
  "date_identity/explicit/value/negative_400_year_leap",
  "date_identity/explicit/value/negative_100_year_common",
  "date_identity/explicit/value/negative_4_year_leap",
  "date_identity/explicit/value/negative_1_year",
  "date_identity/explicit/value/astronomical_year_zero",
  "date_identity/explicit/value/astronomical_year_zero_leap",
  "date_identity/explicit/value/civil_year_one",
  "date_identity/explicit/value/gregorian_400_leap",
  "date_identity/explicit/value/gregorian_100_common",
  "date_identity/explicit/value/before_epoch",
  "date_identity/explicit/value/epoch",
  "date_identity/explicit/value/after_epoch",
  "date_identity/explicit/value/leap_2000",
  "date_identity/explicit/value/iso_week_boundary",
  "date_identity/explicit/value/four_digit_maximum",
  "date_identity/explicit/value/expanded_positive_start",
  "date_identity/explicit/value/carrier_maximum_minus_one",
  "date_identity/explicit/value/carrier_maximum",
  "date_identity/explicit/sql_null",
  "date_identity/explicit/sql_null_nonnullable",
  "date_identity/explicit/dirty_null",
  "date_identity/explicit/wrong_source_handle",
  "date_identity/explicit/wrong_target_handle",
  "date_identity/explicit/present_wrong_host_bool",
  "date_identity/explicit/present_wrong_host_string",
  "date_identity/explicit/present_out_of_i32_range",
  "date_identity/explicit/cancel_before_publish",
  "date_identity/explicit/resource_short",
  "date_identity/explicit/both_source_target_invalid",
  "date_identity/explicit/valid_source_invalid_target_dirty",
  "date_identity/explicit/valid_handles_dirty_null_poison",
}};
struct KeyMutationFixture{std::string_view id,hex,code,gate;};
constexpr std::array<KeyMutationFixture,16> key_mutations{{
  {"bad_magic","58424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K01"},
  {"bad_magic_plus_receipt_mismatch","58424441544b30310000000000000000000000000000000008000000000000000800000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K01"},
  {"catalog_generation_mismatch","53424441544b3031019d000000007000800000000000d70806000000000000000800000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000","CTI.TEMPORAL.DESCRIPTOR_INVALID","K02"},
  {"comparison_fingerprint_mismatch","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000000000000000000000000000000000000000000000000000000000000000000001a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000","CTI.TEMPORAL.DESCRIPTOR_INVALID","K02"},
  {"direction_invalid","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000201010480000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K03"},
  {"long_or_trailing_extent_105","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde380100000000000000000101048000000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K01"},
  {"null_mode_invalid","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000002010480000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K03"},
  {"ordering_generation_mismatch","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3802000000000000000001010480000000","CTI.TEMPORAL.DESCRIPTOR_INVALID","K02"},
  {"ordering_uuid_mismatch","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a42370000000000000000000000000000000001000000000000000001010480000000","CTI.TEMPORAL.DESCRIPTOR_INVALID","K02"},
  {"payload_length_invalid","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010380000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K03"},
  {"rank_invalid","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001030480000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K03"},
  {"rank_state_null_mode_contradiction","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001020480000000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K03"},
  {"receipt_mismatch","53424441544b3031019d000000007000800000000000d70608000000000000000800000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000","CTI.TEMPORAL.DESCRIPTOR_INVALID","K02"},
  {"registry_generation_mismatch","53424441544b3031019d000000007000800000000000d70808000000000000000600000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde3801000000000000000001010480000000","CTI.TEMPORAL.DESCRIPTOR_INVALID","K02"},
  {"short_extent_103","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010104800000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K01"},
  {"short_plus_rank_contradiction","53424441544b3031019d000000007000800000000000d7100a000000000000000a00000000000000ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a423701a1008eb7f27feb85a82d74fe2fde38010000000000000000010204800000","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","K01"},
}};
void CastPolicy(){
  unsigned admitted=0, runtime=0, exact_rows=0, placeholder_rows=0, contextual_rows=0;
  auto profile=Profile();auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);
  dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,42};
  dt::DatatypeOperationValue scalar;
  for(unsigned row=1;row<=221;++row){
    const auto shape=Shape(row);if(shape.contextual)++contextual_rows;else if(shape.exact)++exact_rows;else ++placeholder_rows;
    for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,dt::DatatypeCastContext::explicit_cast}){
      auto want=dt::DateCastPolicyDispositionV3::forbidden;
      if(row==1)want=dt::DateCastPolicyDispositionV3::contextual_null;
      else if(row==53)want=dt::DateCastPolicyDispositionV3::identity;
      else if(row==24&&context==dt::DatatypeCastContext::explicit_cast)want=dt::DateCastPolicyDispositionV3::explicit_character_to_date;
      else if(row==50&&context==dt::DatatypeCastContext::explicit_cast)want=dt::DateCastPolicyDispositionV3::explicit_date_to_character;
      Check(dt::ClassifyDateCastPolicyRowV3(row,context)==want,"221x3 cast classification");
      dt::DateCastRequestV3 q;q.one_based_policy_row=row;q.context=context;
      if(row==53){q.date_source=&date;q.date_target=&profile;}
      else if(shape.incoming){
        scalar={};scalar.type_id=shape.peer;scalar.is_null=shape.contextual;scalar.encoded_value=shape.contextual?"":(row==24?"1970-01-01":"poison-not-read");
        if(shape.exact){q.scalar_source_identity=Identity(shape.peer);Check(q.scalar_source_identity!=nullptr,"exact incoming identity exists");scalar.descriptor=PeerDescriptor(shape.peer,q.scalar_source_identity);}
        q.scalar_source=&scalar;q.date_target=&profile;q.date_target_descriptor=&date_descriptor;
      }else{
        q.date_source=&date;q.scalar_target=shape.peer;
        if(shape.exact){q.scalar_target_identity=Identity(shape.peer);Check(q.scalar_target_identity!=nullptr,"exact outgoing identity exists");q.scalar_target_descriptor=PeerDescriptor(shape.peer,q.scalar_target_identity);}
      }
      auto got=dt::CastDateValueV3(q);++runtime;
      if(want==dt::DateCastPolicyDispositionV3::forbidden){Check(!got.ok()&&got.diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","221x3 runtime forbidden");}
      else {Check(got.ok(),"221x3 runtime admitted");++admitted;}
    }
  }
  Check(runtime==663,"runtime cast matrix count");Check(admitted==8,"runtime cast admitted partition");Check(exact_rows==65&&placeholder_rows==155&&contextual_rows==1,"cast identity readiness partition");
}
void ExactPeerDescriptorShapes(){
  auto profile=Profile();dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,0};auto* identity=Identity(dt::CanonicalTypeId::int32);Check(identity!=nullptr,"int32 V3 identity");auto baseline=PeerDescriptor(dt::CanonicalTypeId::int32,identity);
  auto refusal=[&](const scratchbird::engine::ExecutionTypeDescriptor& descriptor,const dt::DatatypeTypeCodecIdentityRowV3* supplied=static_cast<const dt::DatatypeTypeCodecIdentityRowV3*>(nullptr)){dt::DateCastRequestV3 q;q.one_based_policy_row=31;q.context=dt::DatatypeCastContext::implicit;q.date_source=&date;q.scalar_target=dt::CanonicalTypeId::int32;q.scalar_target_identity=supplied==nullptr?identity:supplied;q.scalar_target_descriptor=descriptor;return dt::CastDateValueV3(q);};
  Check(refusal(baseline).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","full live int32 descriptor reaches policy");
  for(unsigned field=0;field<22;++field){auto changed=baseline;switch(field){
    case 0:changed.descriptor_uuid.bytes[0]^=1;break;case 1:++changed.descriptor_epoch;break;case 2:++changed.canonical_type_id;break;
    case 3:changed.family=scratchbird::engine::ExecutionTypeFamily::character;break;case 4:changed.width_class=scratchbird::engine::ExecutionTypeWidthClass::variable;break;
    case 5:++changed.bit_width;break;case 6:++changed.precision;break;case 7:++changed.scale;break;case 8:++changed.length;break;
    case 9:++changed.vector_dimensions;break;case 10:++changed.container_rank;break;case 11:changed.modifier_flags^=1ull<<63;break;
    case 12:changed.domain_uuid.bytes[0]=1;break;case 13:changed.domain_stack.push_back(changed.descriptor_uuid);break;
    case 14:changed.charset_uuid.bytes[0]=1;break;case 15:changed.collation_uuid.bytes[0]=1;break;case 16:changed.timezone_uuid.bytes[0]=1;break;
    case 17:changed.element_descriptor_uuid.bytes[0]=1;break;case 18:changed.security_policy_uuid.bytes[0]=1;break;
    case 19:changed.nullable_allowed=!changed.nullable_allowed;break;case 20:changed.descriptor_authoritative=false;break;case 21:changed.parser_independent=false;break;}
    auto r=refusal(changed);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","one-field peer descriptor mutation refused before policy");
  }
  auto alias=baseline;alias.stable_name="INT32 presentation alias";Check(refusal(alias).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","descriptor presentation alias is non-authoritative");
  auto identity_alias=*identity;identity_alias.legacy_fields.canonical_name="INTEGER presentation alias";identity_alias.legacy_fields.codec_id="codec presentation alias";Check(refusal(baseline,&identity_alias).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","identity and codec labels are non-authoritative");
  struct ShapeMutation{unsigned row;dt::CanonicalTypeId type;unsigned field;};
  constexpr std::array<ShapeMutation,7> shape_mutations{{{44,dt::CanonicalTypeId::decimal,6},{50,dt::CanonicalTypeId::character,11},{51,dt::CanonicalTypeId::binary,8},{52,dt::CanonicalTypeId::bit_string,8},{54,dt::CanonicalTypeId::time,16},{59,dt::CanonicalTypeId::json_document,10},{69,dt::CanonicalTypeId::list,17}}};
  for(const auto& fixture:shape_mutations){auto* peer_identity=Identity(fixture.type);Check(peer_identity!=nullptr,"shape-class V3 identity");auto descriptor=PeerDescriptor(fixture.type,peer_identity);switch(fixture.field){case 6:++descriptor.precision;break;case 8:++descriptor.length;break;case 10:++descriptor.container_rank;break;case 11:descriptor.modifier_flags^=1ull<<63;break;case 16:descriptor.timezone_uuid.bytes[0]=1;break;case 17:descriptor.element_descriptor_uuid.bytes[0]=1;break;default:Fail("unknown shape mutation");}dt::DateCastRequestV3 q;q.one_based_policy_row=fixture.row;q.context=dt::DatatypeCastContext::implicit;q.date_source=&date;q.scalar_target=fixture.type;q.scalar_target_identity=peer_identity;q.scalar_target_descriptor=descriptor;auto r=dt::CastDateValueV3(q);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","family-specific descriptor mutation refused");}
  auto* character_identity=Identity(dt::CanonicalTypeId::character);auto bounded=PeerDescriptor(dt::CanonicalTypeId::character,character_identity);bounded.length=5;bounded.modifier_flags=scratchbird::engine::ExecutionTypeModifierFlagBit(scratchbird::engine::ExecutionTypeModifierFlag::length);dt::DateCastRequestV3 bounded_q;bounded_q.one_based_policy_row=50;bounded_q.context=dt::DatatypeCastContext::implicit;bounded_q.date_source=&date;bounded_q.scalar_target=dt::CanonicalTypeId::character;bounded_q.scalar_target_identity=character_identity;bounded_q.scalar_target_descriptor=bounded;Check(dt::CastDateValueV3(bounded_q).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","admitted character length remains a parameter, not identity");
  auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);dt::DatatypeOperationValue scalar{dt::CanonicalTypeId::int32,"poison-not-read",false,baseline};dt::DateCastRequestV3 incoming;incoming.one_based_policy_row=5;incoming.context=dt::DatatypeCastContext::implicit;incoming.scalar_source=&scalar;incoming.scalar_source_identity=identity;incoming.date_target=&profile;incoming.date_target_descriptor=&date_descriptor;Check(dt::CastDateValueV3(incoming).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","full live incoming descriptor reaches policy");scalar.descriptor.family=scratchbird::engine::ExecutionTypeFamily::character;Check(dt::CastDateValueV3(incoming).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","incoming descriptor shape mutation precedes policy and payload");
  auto text_descriptor=CharacterDescriptor();auto* text_identity=Identity(dt::CanonicalTypeId::character);dt::DateTextOperandV3 operand{text_identity,&text_descriptor,dt::DateTextCarrierKindV3::utf8_bytes,dt::DateValueStateV3::value,"1970-01-01",10};allocation_probe::allocations=0;allocation_probe::counting=true;const auto parsed=dt::ParseCanonicalDateOperandV3(profile,operand,true);allocation_probe::counting=false;Check(parsed.ok()&&parsed.value.day==0&&allocation_probe::allocations==0,"exact peer/character validation is allocation-free");
}
void ContextualNullDescriptorless(){
  auto profile=Profile();auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);auto invalid_mutable=std::make_shared<dt::DateValidatedProfileHandleV3>(*profile);invalid_mutable->profile_material[0]^=1;std::shared_ptr<const dt::DateValidatedProfileHandleV3> invalid_profile=invalid_mutable;
  for(unsigned field=0;field<23;++field){dt::DatatypeOperationValue source{dt::CanonicalTypeId::null_type,"",true,{}};auto& descriptor=source.descriptor;switch(field){case 0:descriptor.descriptor_uuid.bytes[0]=1;break;case 1:descriptor.descriptor_epoch=1;break;case 2:descriptor.canonical_type_id=1;break;case 3:descriptor.family=scratchbird::engine::ExecutionTypeFamily::boolean;break;case 4:descriptor.width_class=scratchbird::engine::ExecutionTypeWidthClass::fixed;break;case 5:descriptor.stable_name="NULL";break;case 6:descriptor.bit_width=1;break;case 7:descriptor.precision=1;break;case 8:descriptor.scale=1;break;case 9:descriptor.length=1;break;case 10:descriptor.vector_dimensions=1;break;case 11:descriptor.container_rank=1;break;case 12:descriptor.modifier_flags=1;break;case 13:descriptor.domain_uuid.bytes[0]=1;break;case 14:descriptor.domain_stack.push_back(descriptor.descriptor_uuid);break;case 15:descriptor.charset_uuid.bytes[0]=1;break;case 16:descriptor.collation_uuid.bytes[0]=1;break;case 17:descriptor.timezone_uuid.bytes[0]=1;break;case 18:descriptor.element_descriptor_uuid.bytes[0]=1;break;case 19:descriptor.security_policy_uuid.bytes[0]=1;break;case 20:descriptor.nullable_allowed=false;break;case 21:descriptor.descriptor_authoritative=false;break;case 22:descriptor.parser_independent=false;break;}dt::DateCastRequestV3 request;request.one_based_policy_row=1;request.context=dt::DatatypeCastContext::implicit;request.scalar_source=&source;request.date_target=&profile;request.date_target_descriptor=&date_descriptor;const auto result=dt::CastDateValueV3(request);Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID"&&Detail(result.diagnostic)=="contextual_null_descriptor_forbidden","base.null refuses every descriptor-bearing field");if(field==5){source.encoded_value="dirty";request.date_target=&invalid_profile;const auto combined=dt::CastDateValueV3(request);Check(!combined.ok()&&Detail(combined.diagnostic)=="date_target_invalid","base.null target profile precedes descriptor/state");request.date_target=&profile;const auto descriptor_before_state=dt::CastDateValueV3(request);Check(!descriptor_before_state.ok()&&Detail(descriptor_before_state.diagnostic)=="contextual_null_descriptor_forbidden","base.null descriptor precedes dirty state");}}
  dt::DatatypeOperationValue clean{dt::CanonicalTypeId::null_type,"",true,{}};for(unsigned field=0;field<22;++field){auto changed=date_descriptor;switch(field){case 0:changed.descriptor_uuid.bytes[0]^=1;break;case 1:++changed.descriptor_epoch;break;case 2:++changed.canonical_type_id;break;case 3:changed.family=scratchbird::engine::ExecutionTypeFamily::character;break;case 4:changed.width_class=scratchbird::engine::ExecutionTypeWidthClass::variable;break;case 5:++changed.bit_width;break;case 6:++changed.precision;break;case 7:++changed.scale;break;case 8:++changed.length;break;case 9:++changed.vector_dimensions;break;case 10:++changed.container_rank;break;case 11:changed.modifier_flags^=1;break;case 12:changed.domain_uuid.bytes[0]=1;break;case 13:changed.domain_stack.push_back(changed.descriptor_uuid);break;case 14:changed.charset_uuid.bytes[0]=1;break;case 15:changed.collation_uuid.bytes[0]=1;break;case 16:changed.timezone_uuid.bytes[0]=1;break;case 17:changed.element_descriptor_uuid.bytes[0]=1;break;case 18:changed.security_policy_uuid.bytes[0]=1;break;case 19:changed.nullable_allowed=false;break;case 20:changed.descriptor_authoritative=false;break;case 21:changed.parser_independent=false;break;}dt::DateCastRequestV3 request;request.one_based_policy_row=1;request.context=dt::DatatypeCastContext::implicit;request.scalar_source=&clean;request.date_target=&profile;request.date_target_descriptor=&changed;auto result=dt::CastDateValueV3(request);Check(!result.ok()&&Detail(result.diagnostic)=="date_target_admission_invalid","date target descriptor field mutation refused");}auto alias=date_descriptor;alias.stable_name="DATE localized presentation alias";dt::DateCastRequestV3 alias_request;alias_request.one_based_policy_row=1;alias_request.scalar_source=&clean;alias_request.date_target=&profile;alias_request.date_target_descriptor=&alias;Check(dt::CastDateValueV3(alias_request).ok(),"date target presentation name excluded from identity");
}

void CastRequestShapeAndContext(){auto profile=Profile();auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);auto text_descriptor=CharacterDescriptor();auto* text_identity=Identity(dt::CanonicalTypeId::character);dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,0};dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"1970-01-01",false,text_descriptor};dt::DatatypeOperationValue nullv{dt::CanonicalTypeId::null_type,"",true,{}};char byte='x';
  dt::DateCastRequestV3 incoming;incoming.one_based_policy_row=24;incoming.context=dt::DatatypeCastContext::explicit_cast;incoming.scalar_source=&text;incoming.scalar_source_identity=text_identity;incoming.date_target=&profile;incoming.date_target_descriptor=&date_descriptor;Check(dt::CastDateValueV3(incoming).ok(),"incoming baseline shape");for(unsigned variant=0;variant<4;++variant){auto q=incoming;if(variant==0)q.use_character_output_buffer=true;else if(variant==1)q.character_output=&byte;else if(variant==2)q.character_output_capacity=1;else q.scalar_target_descriptor.stable_name="extra";Check(Detail(dt::CastDateValueV3(q).diagnostic)=="incoming_cast_shape_invalid","incoming rejects output-only request fields");}
  dt::DateCastRequestV3 identity;identity.one_based_policy_row=53;identity.date_source=&date;identity.date_target=&profile;Check(dt::CastDateValueV3(identity).ok(),"identity baseline shape");for(unsigned variant=0;variant<5;++variant){auto q=identity;if(variant==0)q.date_target_descriptor=&date_descriptor;else if(variant==1)q.use_character_output_buffer=true;else if(variant==2)q.character_output=&byte;else if(variant==3)q.character_output_capacity=1;else q.scalar_target_descriptor.stable_name="extra";Check(Detail(dt::CastDateValueV3(q).diagnostic)=="identity_cast_shape_invalid","identity rejects unrelated request fields");}
  dt::DateCastRequestV3 outgoing;outgoing.one_based_policy_row=50;outgoing.context=dt::DatatypeCastContext::explicit_cast;outgoing.date_source=&date;outgoing.scalar_target=dt::CanonicalTypeId::character;outgoing.scalar_target_identity=text_identity;outgoing.scalar_target_descriptor=text_descriptor;Check(dt::CastDateValueV3(outgoing).ok(),"outgoing baseline shape");auto q=outgoing;q.date_target_descriptor=&date_descriptor;Check(Detail(dt::CastDateValueV3(q).diagnostic)=="outgoing_cast_shape_invalid","outgoing rejects incoming target descriptor");q=outgoing;q.character_output=&byte;Check(Detail(dt::CastDateValueV3(q).diagnostic)=="outgoing_cast_shape_invalid","owning outgoing rejects inactive buffer pointer");
  const auto invalid=static_cast<dt::DatatypeCastContext>(0xff);auto check_forbidden=[&](dt::DateCastRequestV3 r){r.context=invalid;Check(dt::CastDateValueV3(r).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","closed cast context refused after endpoint gates");};check_forbidden(incoming);check_forbidden(outgoing);check_forbidden(identity);dt::DateCastRequestV3 contextual;contextual.one_based_policy_row=1;contextual.scalar_source=&nullv;contextual.date_target=&profile;contextual.date_target_descriptor=&date_descriptor;check_forbidden(contextual);auto* int_identity=Identity(dt::CanonicalTypeId::int32);auto int_descriptor=PeerDescriptor(dt::CanonicalTypeId::int32,int_identity);dt::DateCastRequestV3 forbidden;forbidden.one_based_policy_row=31;forbidden.date_source=&date;forbidden.scalar_target=dt::CanonicalTypeId::int32;forbidden.scalar_target_identity=int_identity;forbidden.scalar_target_descriptor=int_descriptor;check_forbidden(forbidden);
}
void RuntimeCasts(){
  auto profile=Profile();auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);auto cd=CharacterDescriptor();auto* ci=Identity(dt::CanonicalTypeId::character);Check(ci!=nullptr,"character V3 identity");
  dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"1970-01-01",false,cd};
  dt::DateCastRequestV3 in;in.one_based_policy_row=24;in.scalar_source=&text;in.scalar_source_identity=ci;in.date_target=&profile;in.date_target_descriptor=&date_descriptor;in.context=dt::DatatypeCastContext::explicit_cast;
  auto ir=dt::CastDateValueV3(in);Check(ir.ok()&&ir.produced_date&&ir.date_value.day==0,"character to date");
  in.context=dt::DatatypeCastContext::implicit;Check(!dt::CastDateValueV3(in).ok(),"implicit character refusal");in.context=dt::DatatypeCastContext::assignment;Check(!dt::CastDateValueV3(in).ok(),"assignment character refusal");in.context=dt::DatatypeCastContext::explicit_cast;
  auto wrong_identity=*ci;wrong_identity.legacy_fields.type_generation++;in.scalar_source_identity=&wrong_identity;auto wr=dt::CastDateValueV3(in);Check(!wr.ok()&&wr.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","character V3 mutation");in.scalar_source_identity=ci;
  auto bad=text;bad.descriptor.scale=1;in.scalar_source=&bad;Check(!dt::CastDateValueV3(in).ok(),"character descriptor mutation");bad=text;bad.descriptor.length=5;bad.descriptor.modifier_flags=scratchbird::engine::ExecutionTypeModifierFlagBit(scratchbird::engine::ExecutionTypeModifierFlag::length);in.scalar_source=&bad;auto lr=dt::CastDateValueV3(in);Check(!lr.ok()&&lr.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED","source length bound");
  bad.encoded_value="not-a-date!";in.scalar_source=&bad;auto malformed_short=dt::CastDateValueV3(in);Check(!malformed_short.ok()&&malformed_short.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","malformed grammar precedes declared source length");
  std::string overlong(16'777'217,'0');bad=text;bad.encoded_value=std::move(overlong);in.scalar_source=&bad;auto too_long=dt::CastDateValueV3(in);Check(!too_long.ok()&&too_long.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","global overlong character source is malformed canonical grammar");
  dt::DatatypeOperationValue nullv{dt::CanonicalTypeId::null_type,"",true,{}};in={};in.one_based_policy_row=1;in.scalar_source=&nullv;in.date_target=&profile;in.date_target_descriptor=&date_descriptor;in.context=dt::DatatypeCastContext::implicit;auto nr=dt::CastDateValueV3(in);Check(nr.ok()&&nr.date_value.state==dt::DateValueStateV3::sql_null,"contextual null");in.target_null_allowed=false;Check(!dt::CastDateValueV3(in).ok(),"null not admitted");nullv.encoded_value="x";in.scalar_source=&nullv;in.target_null_allowed=true;auto dirty=dt::CastDateValueV3(in);Check(!dirty.ok()&&dirty.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","dirty contextual null");nullv.encoded_value.clear();in.scalar_source_identity=ci;Check(!dt::CastDateValueV3(in).ok(),"base.null forbids fabricated identity");
  dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,0};dt::DateCastRequestV3 out;out.one_based_policy_row=50;out.date_source=&date;out.scalar_target=dt::CanonicalTypeId::character;out.scalar_target_descriptor=cd;out.scalar_target_identity=ci;out.context=dt::DatatypeCastContext::explicit_cast;auto orr=dt::CastDateValueV3(out);Check(orr.ok()&&orr.scalar_value.encoded_value=="1970-01-01","date to character");out.scalar_target_descriptor.length=5;out.scalar_target_descriptor.modifier_flags=scratchbird::engine::ExecutionTypeModifierFlagBit(scratchbird::engine::ExecutionTypeModifierFlag::length);auto ol=dt::CastDateValueV3(out);Check(!ol.ok()&&ol.diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED","target length bound");out.scalar_target_descriptor=cd;out.control.maximum_allocation_bytes=9;Check(!dt::CastDateValueV3(out).ok(),"cast resource propagation");Cancel c{0,1};out.control={~uint64_t{0},Stop,&c};auto cr=dt::CastDateValueV3(out);Check(!cr.ok()&&cr.diagnostic.diagnostic_code=="PROCESS.CANCELLED","cast cancellation propagation");
  out.control={};out.scalar_source=&text;Check(!dt::CastDateValueV3(out).ok(),"contradictory outgoing source shape");dt::DateCastRequestV3 both;both.one_based_policy_row=53;both.date_source=&date;both.date_target=&profile;both.scalar_source=&text;Check(!dt::CastDateValueV3(both).ok(),"contradictory identity shape");dt::DateCastRequestV3 none;Check(!dt::CastDateValueV3(none).ok(),"missing policy row/endpoints");
}
void CastCallerBufferAtomicity(){
  auto profile=Profile();auto descriptor=CharacterDescriptor();auto* identity=Identity(dt::CanonicalTypeId::character);Check(identity!=nullptr,"caller-buffer text identity");dt::DateOwnedValueV3 value{profile,dt::DateValueStateV3::value,0};dt::DateCastRequestV3 request;request.one_based_policy_row=50;request.context=dt::DatatypeCastContext::explicit_cast;request.date_source=&value;request.scalar_target=dt::CanonicalTypeId::character;request.scalar_target_identity=identity;request.scalar_target_descriptor=descriptor;request.use_character_output_buffer=true;
  std::array<char,10> output;output.fill('x');request.character_output=output.data();request.character_output_capacity=output.size();auto result=dt::CastDateValueV3(request);Check(result.ok()&&result.used_character_output_buffer&&result.bytes_required==10&&result.bytes_written==10&&std::string_view(output.data(),output.size())=="1970-01-01"&&result.scalar_value.encoded_value.empty(),"caller-buffer exact allocation-free result");
  output.fill('x');auto before=output;request.character_output_capacity=9;result=dt::CastDateValueV3(request);Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED"&&result.bytes_required==10&&output==before,"caller-buffer one-short unchanged");
  auto material=profile->profile_material;request.character_output=reinterpret_cast<char*>(const_cast<p::byte*>(profile->profile_material.data()));request.character_output_capacity=10;result=dt::CastDateValueV3(request);Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&profile->profile_material==material,"caller-buffer profile overlap unchanged");
  auto value_before=value;request.character_output=reinterpret_cast<char*>(&value);result=dt::CastDateValueV3(request);Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&value.profile==value_before.profile&&value.state==value_before.state&&value.day==value_before.day,"caller-buffer value overlap unchanged");
  output.fill('x');before=output;request.character_output=output.data();Cancel cancel{0,2};request.control={~uint64_t{0},Stop,&cancel};result=dt::CastDateValueV3(request);Check(!result.ok()&&result.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&output==before,"caller-buffer final cancellation unchanged");
}
struct SemanticCursor{
  std::size_t position=0,executed=0;
  void Execute(const std::string& id){Check(position<cast_semantic_ids.size()&&cast_semantic_ids[position]==id,"exact cast semantic case order");++position;++executed;}
};
dt::DateExecutionControlV3 Control(Cancel& cancel,std::uint64_t grant=~std::uint64_t{0}){return {grant,Stop,&cancel};}
dt::DateCastRequestV3 Incoming(unsigned row,dt::DatatypeCastContext context,dt::DatatypeOperationValue* scalar,const dt::DatatypeTypeCodecIdentityRowV3* identity,const std::shared_ptr<const dt::DateValidatedProfileHandleV3>* target){static const auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);dt::DateCastRequestV3 q;q.one_based_policy_row=row;q.context=context;q.scalar_source=scalar;q.scalar_source_identity=identity;q.date_target=target;q.date_target_descriptor=&date_descriptor;return q;}
dt::DateCastRequestV3 Outgoing(unsigned row,dt::DatatypeCastContext context,dt::DateOwnedValueV3* date,const dt::DatatypeTypeCodecIdentityRowV3* identity,const scratchbird::engine::ExecutionTypeDescriptor& descriptor){dt::DateCastRequestV3 q;q.one_based_policy_row=row;q.context=context;q.date_source=date;q.scalar_target=row==53?dt::CanonicalTypeId::unknown:dt::CanonicalTypeId::character;q.scalar_target_identity=row==53?nullptr:identity;q.scalar_target_descriptor=descriptor;return q;}
void ExactCastSemanticClosure(){
  SemanticCursor cursor;auto profile=Profile();auto text_descriptor=CharacterDescriptor();auto* text_identity=Identity(dt::CanonicalTypeId::character);Check(text_identity!=nullptr,"semantic text identity");
  constexpr std::array<std::pair<std::string_view,dt::DatatypeCastContext>,3> contexts{{{"implicit",dt::DatatypeCastContext::implicit},{"assignment",dt::DatatypeCastContext::assignment},{"explicit",dt::DatatypeCastContext::explicit_cast}}};
  auto invalid_mutable=std::make_shared<dt::DateValidatedProfileHandleV3>(*profile);invalid_mutable->profile_material[0]^=1;std::shared_ptr<const dt::DateValidatedProfileHandleV3> invalid_profile=invalid_mutable;
  for(const auto& [name,context]:contexts){
    auto id=[&](std::string_view tail){return std::string("null_binding/")+std::string(name)+"/"+std::string(tail);};
    dt::DatatypeOperationValue n{dt::CanonicalTypeId::null_type,"",true,{}};auto q=Incoming(1,context,&n,nullptr,&profile);cursor.Execute(id("success"));Check(dt::CastDateValueV3(q).ok(),"semantic contextual NULL success");
    n.encoded_value="x";cursor.Execute(id("dirty_null"));Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","semantic contextual dirty NULL");n.encoded_value.clear();
    q.target_null_allowed=false;cursor.Execute(id("target_nonnullable"));Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED","semantic contextual nonnullable");q.target_null_allowed=true;
    q.date_target=&invalid_profile;cursor.Execute(id("wrong_target_handle"));Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","semantic contextual target handle");q.date_target=&profile;
    Cancel cancel{0,1};q.control=Control(cancel);cursor.Execute(id("cancel_before_publish"));Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="PROCESS.CANCELLED","semantic contextual cancel");q.control={};
    q.control.maximum_allocation_bytes=0;cursor.Execute(id("zero_resource_grant_success"));Check(dt::CastDateValueV3(q).ok(),"semantic contextual zero grant");
  }
  dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"poison-not-read",false,text_descriptor};
  for(const auto& [name,context]:std::array<std::pair<std::string_view,dt::DatatypeCastContext>,2>{{{"implicit",dt::DatatypeCastContext::implicit},{"assignment",dt::DatatypeCastContext::assignment}}}){auto q=Incoming(24,context,&text,text_identity,&profile);cursor.Execute(std::string("character_to_date/")+std::string(name)+"/forbidden");Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","semantic parse forbidden");}
  for(const auto& fixture:canonical_casts){text.encoded_value=fixture.text;auto q=Incoming(24,dt::DatatypeCastContext::explicit_cast,&text,text_identity,&profile);cursor.Execute(std::string("character_to_date/explicit/canonical/")+std::string(fixture.id));auto r=dt::CastDateValueV3(q);Check(r.ok()&&r.date_value.day==fixture.day,"semantic canonical parse");}
  for(const auto& fixture:invalid_text_casts){text.encoded_value=fixture.text;auto q=Incoming(24,dt::DatatypeCastContext::explicit_cast,&text,text_identity,&profile);cursor.Execute(std::string("character_to_date/explicit/invalid_text/")+std::string(fixture.id));Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code==fixture.code,"semantic invalid text");}
  for(const auto& fixture:invalid_byte_casts){auto bytes=Hex(fixture.hex);text.encoded_value.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());auto q=Incoming(24,dt::DatatypeCastContext::explicit_cast,&text,text_identity,&profile);cursor.Execute(std::string("character_to_date/explicit/invalid_bytes/")+std::string(fixture.id));Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code==fixture.code,"semantic invalid text bytes");}
  auto parse_case=[&](std::string_view tail,dt::DatatypeOperationValue source,const dt::DatatypeTypeCodecIdentityRowV3* identity,const std::shared_ptr<const dt::DateValidatedProfileHandleV3>* target,bool nullable,dt::DateExecutionControlV3 control,std::string_view code){auto q=Incoming(24,dt::DatatypeCastContext::explicit_cast,&source,identity,target);q.target_null_allowed=nullable;q.control=control;cursor.Execute(std::string("character_to_date/explicit/")+std::string(tail));auto r=dt::CastDateValueV3(q);Check((code.empty()&&r.ok())||(!code.empty()&&r.diagnostic.diagnostic_code==code),"semantic parse special");};
  text={dt::CanonicalTypeId::character,"",true,text_descriptor};parse_case("sql_null",text,text_identity,&profile,true,{},"");parse_case("sql_null_nonnullable",text,text_identity,&profile,false,{},"DATATYPE.NULL_NOT_ADMITTED");text.encoded_value="x";parse_case("dirty_null",text,text_identity,&profile,true,{},"DATATYPE.NULL_STATE.INVALID");
  text={dt::CanonicalTypeId::character,"poison",false,text_descriptor};auto bad_descriptor=text_descriptor;bad_descriptor.descriptor_uuid.bytes[0]^=1;auto bad_text=text;bad_text.descriptor=bad_descriptor;parse_case("wrong_text_descriptor",bad_text,text_identity,&profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");auto bad_identity=*text_identity;bad_identity.legacy_fields.codec_uuid.bytes[0]^=1;parse_case("wrong_text_profile",text,&bad_identity,&profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");parse_case("wrong_target_date_handle",text,text_identity,&invalid_profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");auto wrong_type=text;wrong_type.type_id=dt::CanonicalTypeId::boolean;parse_case("present_wrong_source_type",wrong_type,text_identity,&profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");
  text.encoded_value="1970-01-01";Cancel pc1{0,1};parse_case("cancel_before_parse",text,text_identity,&profile,true,Control(pc1),"PROCESS.CANCELLED");Cancel pc2{0,2};parse_case("cancel_before_publish",text,text_identity,&profile,true,Control(pc2),"PROCESS.CANCELLED");dt::DateExecutionControlV3 short_grant;short_grant.maximum_allocation_bytes=3;parse_case("resource_short",text,text_identity,&profile,true,short_grant,"RESOURCE.BUDGET_EXCEEDED");
  parse_case("both_source_target_invalid",bad_text,text_identity,&invalid_profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");text={dt::CanonicalTypeId::character,"x",true,text_descriptor};parse_case("valid_source_invalid_target_dirty",text,text_identity,&invalid_profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");parse_case("valid_handles_dirty_null_poison",text,text_identity,&profile,true,{},"DATATYPE.NULL_STATE.INVALID");
  for(const auto& [name,context]:std::array<std::pair<std::string_view,dt::DatatypeCastContext>,2>{{{"implicit",dt::DatatypeCastContext::implicit},{"assignment",dt::DatatypeCastContext::assignment}}}){dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,0};auto q=Outgoing(50,context,&date,text_identity,text_descriptor);cursor.Execute(std::string("date_to_character/")+std::string(name)+"/forbidden");Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","semantic render forbidden");}
  for(const auto& fixture:canonical_casts){dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,fixture.day};std::array<char,14> output;output.fill('x');auto q=Outgoing(50,dt::DatatypeCastContext::explicit_cast,&date,text_identity,text_descriptor);q.use_character_output_buffer=true;q.character_output=output.data();q.character_output_capacity=fixture.text.size();cursor.Execute(std::string("date_to_character/explicit/exact_capacity/")+std::string(fixture.id));auto r=dt::CastDateValueV3(q);Check(r.ok()&&r.used_character_output_buffer&&r.bytes_written==fixture.text.size()&&std::string_view(output.data(),r.bytes_written)==fixture.text,"semantic render exact buffer");output.fill('x');auto before=output;q.character_output_capacity=fixture.text.size()-1;cursor.Execute(std::string("date_to_character/explicit/one_short/")+std::string(fixture.id));r=dt::CastDateValueV3(q);Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED"&&r.bytes_required==fixture.text.size()&&output==before,"semantic render one short atomic");}
  auto render_case=[&](std::string_view tail,dt::DateOwnedValueV3 value,const dt::DatatypeTypeCodecIdentityRowV3* identity,const scratchbird::engine::ExecutionTypeDescriptor& descriptor,bool nullable,dt::DateExecutionControlV3 control,std::string_view code){std::array<char,14> output;output.fill('x');auto before=output;auto q=Outgoing(50,dt::DatatypeCastContext::explicit_cast,&value,identity,descriptor);q.target_null_allowed=nullable;q.use_character_output_buffer=true;q.character_output=output.data();q.character_output_capacity=output.size();if(tail=="sql_null_zero_capacity"){q.character_output=nullptr;q.character_output_capacity=0;}q.control=control;cursor.Execute(std::string("date_to_character/explicit/")+std::string(tail));auto r=dt::CastDateValueV3(q);Check((code.empty()&&r.ok())||(!code.empty()&&r.diagnostic.diagnostic_code==code),"semantic render special");if(!code.empty())Check(output==before,"semantic render failure sentinel");};
  dt::DateOwnedValueV3 null_date{profile,dt::DateValueStateV3::sql_null,0};render_case("sql_null_zero_capacity",null_date,text_identity,text_descriptor,true,{},"");render_case("sql_null_nonnullable",null_date,text_identity,text_descriptor,false,{},"DATATYPE.NULL_NOT_ADMITTED");auto dirty_date=null_date;dirty_date.day=1;render_case("dirty_null",dirty_date,text_identity,text_descriptor,true,{},"DATATYPE.NULL_STATE.INVALID");dt::DateOwnedValueV3 epoch{profile,dt::DateValueStateV3::value,0};dt::DateOwnedValueV3 bad_source{invalid_profile,dt::DateValueStateV3::value,0};render_case("wrong_source_date_handle",bad_source,text_identity,text_descriptor,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");render_case("wrong_target_text_descriptor",epoch,text_identity,bad_descriptor,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");render_case("wrong_target_text_profile",epoch,&bad_identity,text_descriptor,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");
  auto dynamic_refusal=[&](unsigned row,dt::DatatypeCastContext context,std::string_view case_id,dt::DateDayCarrierKindV3 carrier,std::int64_t day){
    dt::DateOperandV3 operand{profile,dt::DateValueStateV3::value,carrier,day};dt::DateCastRequestV3 q;q.one_based_policy_row=row;q.context=context;q.dynamic_date_source=&operand;
    std::array<char,14> output;output.fill('x');const auto before=output;
    if(row==50){q.scalar_target=dt::CanonicalTypeId::character;q.scalar_target_identity=text_identity;q.scalar_target_descriptor=text_descriptor;q.use_character_output_buffer=true;q.character_output=output.data();q.character_output_capacity=output.size();}
    else q.date_target=&profile;
    cursor.Execute(std::string(case_id));const auto r=dt::CastDateValueV3(q);
    Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&!r.produced_date&&!r.used_character_output_buffer&&r.bytes_written==0&&r.scalar_value.encoded_value.empty()&&output==before,"dynamic DateOperand cast refusal is atomic");
  };
  dynamic_refusal(50,dt::DatatypeCastContext::explicit_cast,"date_to_character/explicit/present_wrong_host_bool",dt::DateDayCarrierKindV3::wrong_host_boolean,0);
  dynamic_refusal(50,dt::DatatypeCastContext::explicit_cast,"date_to_character/explicit/present_wrong_host_string",dt::DateDayCarrierKindV3::wrong_host_string,0);
  dynamic_refusal(50,dt::DatatypeCastContext::explicit_cast,"date_to_character/explicit/present_out_of_i32_range",dt::DateDayCarrierKindV3::above_i32,std::int64_t{INT32_MAX}+1);
  Cancel rc1{0,1};render_case("cancel_before_render",epoch,text_identity,text_descriptor,true,Control(rc1),"PROCESS.CANCELLED");Cancel rc3{0,2};render_case("cancel_before_publish",epoch,text_identity,text_descriptor,true,Control(rc3),"PROCESS.CANCELLED");dt::DateExecutionControlV3 render_grant;render_grant.maximum_allocation_bytes=9;render_case("resource_short",epoch,text_identity,text_descriptor,true,render_grant,"RESOURCE.BUDGET_EXCEEDED");render_case("both_source_target_invalid",bad_source,text_identity,bad_descriptor,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");render_case("valid_source_invalid_target_dirty",dirty_date,text_identity,bad_descriptor,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");render_case("valid_handles_dirty_null_poison",dirty_date,text_identity,text_descriptor,true,{},"DATATYPE.NULL_STATE.INVALID");
  for(const auto& [name,context]:contexts){
    for(const auto& fixture:canonical_casts){dt::DateOwnedValueV3 date{profile,dt::DateValueStateV3::value,fixture.day};dt::DateCastRequestV3 q;q.one_based_policy_row=53;q.context=context;q.date_source=&date;q.date_target=&profile;cursor.Execute(std::string("date_identity/")+std::string(name)+"/value/"+std::string(fixture.id));auto r=dt::CastDateValueV3(q);Check(r.ok()&&r.date_value.day==fixture.day,"semantic identity value");}
    auto identity_case=[&](std::string_view tail,dt::DateOwnedValueV3 value,const std::shared_ptr<const dt::DateValidatedProfileHandleV3>* target,bool nullable,dt::DateExecutionControlV3 control,std::string_view code){dt::DateCastRequestV3 q;q.one_based_policy_row=53;q.context=context;q.date_source=&value;q.date_target=target;q.target_null_allowed=nullable;q.control=control;cursor.Execute(std::string("date_identity/")+std::string(name)+"/"+std::string(tail));auto r=dt::CastDateValueV3(q);Check((code.empty()&&r.ok())||(!code.empty()&&r.diagnostic.diagnostic_code==code),"semantic identity special");};
    identity_case("sql_null",null_date,&profile,true,{},"");identity_case("sql_null_nonnullable",null_date,&profile,false,{},"DATATYPE.NULL_NOT_ADMITTED");identity_case("dirty_null",dirty_date,&profile,true,{},"DATATYPE.NULL_STATE.INVALID");identity_case("wrong_source_handle",bad_source,&profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");identity_case("wrong_target_handle",epoch,&invalid_profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    dynamic_refusal(53,context,std::string("date_identity/")+std::string(name)+"/present_wrong_host_bool",dt::DateDayCarrierKindV3::wrong_host_boolean,0);
    dynamic_refusal(53,context,std::string("date_identity/")+std::string(name)+"/present_wrong_host_string",dt::DateDayCarrierKindV3::wrong_host_string,0);
    dynamic_refusal(53,context,std::string("date_identity/")+std::string(name)+"/present_out_of_i32_range",dt::DateDayCarrierKindV3::above_i32,std::int64_t{INT32_MAX}+1);
    Cancel ic1{0,1};identity_case("cancel_before_publish",epoch,&profile,true,Control(ic1),"PROCESS.CANCELLED");dt::DateExecutionControlV3 identity_grant;identity_grant.maximum_allocation_bytes=3;identity_case("resource_short",epoch,&profile,true,identity_grant,"RESOURCE.BUDGET_EXCEEDED");identity_case("both_source_target_invalid",bad_source,&invalid_profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");identity_case("valid_source_invalid_target_dirty",dirty_date,&invalid_profile,true,{},"CTI.TEMPORAL.DESCRIPTOR_INVALID");identity_case("valid_handles_dirty_null_poison",dirty_date,&profile,true,{},"DATATYPE.NULL_STATE.INVALID");
  }
  Check(cursor.position==239&&cursor.executed==239,"exact 239 cast semantic rows executed through typed production APIs");
}
void Hashes(){auto profile=Profile();for(const auto& v:hashes){dt::DateOwnedValueV3 x{profile,v.is_null?dt::DateValueStateV3::sql_null:dt::DateValueStateV3::value,static_cast<int32_t>(v.day)};auto r=dt::HashDateValueV3(x);Check(r.ok()&&r.bytes==Hex(v.hex),"exact hash vector");}}
void SortKeys(){auto profile=Profile();for(const auto& v:sorts){dt::DateOwnedValueV3 x{profile,v.is_null?dt::DateValueStateV3::sql_null:dt::DateValueStateV3::value,static_cast<int32_t>(v.day)};auto r=dt::MakeDateSortKeyV3(x,v.dir,v.mode);auto expected=Hex(v.hex);Check(r.ok()&&r.bytes==expected,"exact sort key");auto d=dt::DecodeDateSortKeyNoAllocV3(*profile,expected);Check(d.ok()&&d.value.state==x.state&&d.value.day==x.day&&d.value.direction==v.dir&&d.value.null_mode==v.mode,"sort decode");}for(const auto& mutation:key_mutations){auto decoded=dt::DecodeDateSortKeyNoAllocV3(*profile,Hex(mutation.hex));Check(!decoded.ok()&&decoded.diagnostic.diagnostic_code==mutation.code&&decoded.diagnostic.detail.starts_with(mutation.gate),"exact SBDATK mutation gate/diagnostic");}}
void SortKeyModePrecedence(){
  auto profile=Profile();std::array<p::byte,dt::kDateValueSortKeyBytesV3> output;output.fill(0xa5);const auto before=output;
  dt::DateOwnedValueV3 poison{profile,static_cast<dt::DateValueStateV3>(0xff),std::numeric_limits<std::int32_t>::min()};
  auto result=dt::MakeDateSortKeyIntoNoAllocV3(poison,static_cast<dt::DateSortDirectionV3>(0xff),dt::DateNullModeV3::nulls_last,output.data(),output.size());Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.INDEX_KEY_REFUSED"&&result.bytes_written==0&&output==before,"ordered_key_invalid_direction exact precedence/no read/no publication");
  result=dt::MakeDateSortKeyIntoNoAllocV3(poison,dt::DateSortDirectionV3::ascending,static_cast<dt::DateNullModeV3>(0xff),output.data(),output.size());Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.INDEX_KEY_REFUSED"&&result.bytes_written==0&&output==before,"ordered_key_invalid_null_mode exact precedence/no read/no publication");
  poison.profile=nullptr;result=dt::MakeDateSortKeyIntoNoAllocV3(poison,static_cast<dt::DateSortDirectionV3>(0xff),static_cast<dt::DateNullModeV3>(0xff),output.data(),output.size());Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID"&&result.bytes_written==0&&output==before,"ordered_key_invalid_handle_precedes_invalid_mode");
}
void CastCancellationPrecedence(){
  auto profile=Profile();auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);auto descriptor=CharacterDescriptor();auto* identity=Identity(dt::CanonicalTypeId::character);Check(identity!=nullptr,"cancellation text identity");
  auto incoming=[&](bool is_null,std::string payload,bool nullable,dt::DatatypeCastContext context){dt::DatatypeOperationValue scalar{dt::CanonicalTypeId::character,std::move(payload),is_null,descriptor};dt::DateCastRequestV3 q;q.one_based_policy_row=24;q.context=context;q.scalar_source=&scalar;q.scalar_source_identity=identity;q.date_target=&profile;q.date_target_descriptor=&date_descriptor;q.target_null_allowed=nullable;Cancel cancel{0,1};q.control=Control(cancel);return dt::CastDateValueV3(q);};
  Check(incoming(true,"dirty",true,dt::DatatypeCastContext::explicit_cast).diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","incoming dirty NULL precedes cancellation");
  Check(incoming(true,"",false,dt::DatatypeCastContext::explicit_cast).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED","incoming nonnullable precedes cancellation");
  Check(incoming(false,"poison-not-read",true,dt::DatatypeCastContext::implicit).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","incoming forbidden pair precedes cancellation and conversion");
  Check(incoming(false,"1970-01-01",true,dt::DatatypeCastContext::explicit_cast).diagnostic.diagnostic_code=="PROCESS.CANCELLED","incoming admitted conversion observes cancellation");
  {dt::DatatypeOperationValue scalar{dt::CanonicalTypeId::character,"1970-01-01",false,descriptor};dt::DateCastRequestV3 q;q.one_based_policy_row=24;q.context=dt::DatatypeCastContext::explicit_cast;q.scalar_source=&scalar;q.scalar_source_identity=identity;q.date_target=&profile;q.date_target_descriptor=&date_descriptor;Cancel cancel{0,1};q.control=Control(cancel,3);auto r=dt::CastDateValueV3(q);Check(r.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&r.bytes_required==4,"incoming resource precedes cancellation with extent");}
  auto outgoing=[&](dt::DateValueStateV3 state,std::int32_t day,bool nullable,dt::DatatypeCastContext context){dt::DateOwnedValueV3 value{profile,state,day};dt::DateCastRequestV3 q;q.one_based_policy_row=50;q.context=context;q.date_source=&value;q.scalar_target=dt::CanonicalTypeId::character;q.scalar_target_identity=identity;q.scalar_target_descriptor=descriptor;q.target_null_allowed=nullable;Cancel cancel{0,1};q.control=Control(cancel);return dt::CastDateValueV3(q);};
  Check(outgoing(dt::DateValueStateV3::sql_null,1,true,dt::DatatypeCastContext::explicit_cast).diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","outgoing dirty NULL precedes cancellation");
  Check(outgoing(dt::DateValueStateV3::sql_null,0,false,dt::DatatypeCastContext::explicit_cast).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED","outgoing nonnullable precedes cancellation");
  Check(outgoing(dt::DateValueStateV3::value,0,true,dt::DatatypeCastContext::implicit).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","outgoing forbidden pair precedes cancellation and render");
  Check(outgoing(dt::DateValueStateV3::value,0,true,dt::DatatypeCastContext::explicit_cast).diagnostic.diagnostic_code=="PROCESS.CANCELLED","outgoing admitted conversion observes cancellation");
  {dt::DateOwnedValueV3 value{profile,dt::DateValueStateV3::value,0};dt::DateCastRequestV3 q;q.one_based_policy_row=50;q.context=dt::DatatypeCastContext::explicit_cast;q.date_source=&value;q.scalar_target=dt::CanonicalTypeId::character;q.scalar_target_identity=identity;q.scalar_target_descriptor=descriptor;Cancel cancel{0,1};q.control=Control(cancel,9);auto resource=dt::CastDateValueV3(q);Check(resource.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&resource.bytes_required==10,"outgoing resource precedes cancellation with extent");q.control=Control(cancel);q.scalar_target_descriptor.length=5;q.scalar_target_descriptor.modifier_flags=scratchbird::engine::ExecutionTypeModifierFlagBit(scratchbird::engine::ExecutionTypeModifierFlag::length);auto length=dt::CastDateValueV3(q);Check(length.diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED"&&length.bytes_required==10,"outgoing declared length precedes cancellation with extent");q.scalar_target_descriptor=descriptor;q.use_character_output_buffer=true;std::array<char,10> buffer;buffer.fill('x');auto before=buffer;q.character_output=buffer.data();q.character_output_capacity=9;Check(dt::CastDateValueV3(q).diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED"&&buffer==before,"outgoing caller capacity precedes cancellation without publication");}
  dt::DateOwnedValueV3 final_value{profile,dt::DateValueStateV3::value,0};dt::DateCastRequestV3 final_q;final_q.one_based_policy_row=50;final_q.context=dt::DatatypeCastContext::explicit_cast;final_q.date_source=&final_value;final_q.scalar_target=dt::CanonicalTypeId::character;final_q.scalar_target_identity=identity;final_q.scalar_target_descriptor=descriptor;Cancel final_cancel{0,2};final_q.control=Control(final_cancel);auto final_result=dt::CastDateValueV3(final_q);Check(!final_result.ok()&&final_result.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&final_result.scalar_value.encoded_value.empty()&&final_result.bytes_written==0,"owning cast final cancellation clears unpublished text");
  dt::DateOwnedValueV3 identity_value{profile,dt::DateValueStateV3::sql_null,1};dt::DateCastRequestV3 identity_q;identity_q.one_based_policy_row=53;identity_q.date_source=&identity_value;identity_q.date_target=&profile;Cancel identity_cancel{0,1};identity_q.control=Control(identity_cancel);Check(dt::CastDateValueV3(identity_q).diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","identity dirty NULL precedes cancellation");identity_value.day=0;identity_q.target_null_allowed=false;Check(dt::CastDateValueV3(identity_q).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED","identity nonnullable precedes cancellation");identity_value.state=dt::DateValueStateV3::value;identity_q.target_null_allowed=true;identity_q.control={};identity_q.control.maximum_allocation_bytes=3;auto identity_resource=dt::CastDateValueV3(identity_q);Check(identity_resource.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&identity_resource.bytes_required==4,"identity resource refusal carries fixed extent");
}
void BoundedAliasPrecedence(){
  auto profile=Profile();dt::DateOwnedValueV3 value{profile,dt::DateValueStateV3::value,0};dt::DateExecutionControlV3 zero;zero.maximum_allocation_bytes=0;
  auto component=dt::EncodeCanonicalDateComponentIntoNoAllocV3(value,reinterpret_cast<p::byte*>(&value),0,zero);Check(!component.ok()&&component.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component alias precedes short/resource");
  auto rendered=dt::RenderCanonicalDateIntoNoAllocV3(value,false,reinterpret_cast<char*>(&value),0,zero);Check(!rendered.ok()&&rendered.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","render alias precedes short/resource");
  auto hashed=dt::HashDateValueIntoNoAllocV3(value,const_cast<p::byte*>(profile->profile_material.data()),0,zero);Check(!hashed.ok()&&hashed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","hash profile alias precedes short/resource");
  auto keyed=dt::MakeDateSortKeyIntoNoAllocV3(value,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,const_cast<p::byte*>(profile->profile_material.data()),0,zero);Check(!keyed.ok()&&keyed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key profile alias precedes short/resource");
  auto* identity=Identity(dt::CanonicalTypeId::character);auto descriptor=CharacterDescriptor();dt::DateCastRequestV3 cast;cast.one_based_policy_row=50;cast.context=dt::DatatypeCastContext::explicit_cast;cast.date_source=&value;cast.scalar_target=dt::CanonicalTypeId::character;cast.scalar_target_identity=identity;cast.scalar_target_descriptor=descriptor;cast.use_character_output_buffer=true;cast.character_output=reinterpret_cast<char*>(const_cast<p::byte*>(profile->profile_material.data()));cast.character_output_capacity=0;cast.control=zero;auto cast_result=dt::CastDateValueV3(cast);Check(!cast_result.ok()&&cast_result.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&cast_result.bytes_written==0,"cast profile alias precedes short/resource");
  const auto& dynamic_profile=profile->identity.legacy_fields.canonical_representation;Check(!dynamic_profile.empty(),"profile has dynamic canonical representation authority");const auto dynamic_before=dynamic_profile;component=dt::EncodeCanonicalDateComponentIntoNoAllocV3(value,reinterpret_cast<p::byte*>(const_cast<char*>(dynamic_profile.data())),0,zero);Check(!component.ok()&&component.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&dynamic_profile==dynamic_before,"component dynamic profile buffer overlap");rendered=dt::RenderCanonicalDateIntoNoAllocV3(value,false,const_cast<char*>(dynamic_profile.data()),0,zero);Check(!rendered.ok()&&rendered.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&dynamic_profile==dynamic_before,"render dynamic profile buffer overlap");hashed=dt::HashDateValueIntoNoAllocV3(value,reinterpret_cast<p::byte*>(const_cast<char*>(dynamic_profile.data())),0,zero);Check(!hashed.ok()&&hashed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&dynamic_profile==dynamic_before,"hash dynamic profile buffer overlap");keyed=dt::MakeDateSortKeyIntoNoAllocV3(value,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,reinterpret_cast<p::byte*>(const_cast<char*>(dynamic_profile.data())),0,zero);Check(!keyed.ok()&&keyed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&dynamic_profile==dynamic_before,"key dynamic profile buffer overlap");
  dt::DateExecutionControlV3 control_alias;control_alias.maximum_allocation_bytes=0;component=dt::EncodeCanonicalDateComponentIntoNoAllocV3(value,reinterpret_cast<p::byte*>(&control_alias),0,control_alias);Check(!component.ok()&&component.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component control overlap precedes short/resource");rendered=dt::RenderCanonicalDateIntoNoAllocV3(value,false,reinterpret_cast<char*>(&control_alias),0,control_alias);Check(!rendered.ok()&&rendered.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","render control overlap precedes short/resource");hashed=dt::HashDateValueIntoNoAllocV3(value,reinterpret_cast<p::byte*>(&control_alias),0,control_alias);Check(!hashed.ok()&&hashed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","hash control overlap precedes short/resource");keyed=dt::MakeDateSortKeyIntoNoAllocV3(value,dt::DateSortDirectionV3::ascending,dt::DateNullModeV3::nulls_last,reinterpret_cast<p::byte*>(&control_alias),0,control_alias);Check(!keyed.ok()&&keyed.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key control overlap precedes short/resource");
  auto identity_alias=*identity;identity_alias.legacy_fields.canonical_name="CHARACTER presentation alias stored outside identity object";cast.scalar_target_identity=&identity_alias;cast.scalar_target_descriptor=descriptor;cast.character_output=identity_alias.legacy_fields.canonical_name.data();cast.character_output_capacity=0;cast_result=dt::CastDateValueV3(cast);Check(!cast_result.ok()&&cast_result.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","cast identity dynamic-buffer overlap");cast.scalar_target_identity=identity;cast.scalar_target_descriptor.stable_name="CHARACTER descriptor presentation alias stored outside object";cast.character_output=cast.scalar_target_descriptor.stable_name.data();cast_result=dt::CastDateValueV3(cast);Check(!cast_result.ok()&&cast_result.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","cast descriptor dynamic-buffer overlap");
}
void CastPinObservability(){auto profile=Profile();auto date_descriptor=CatalogDescriptor(dt::CanonicalTypeId::date);auto text_descriptor=CharacterDescriptor();auto* text_identity=Identity(dt::CanonicalTypeId::character);Check(text_identity!=nullptr,"pin evidence text identity");
  dt::DateOwnedValueV3 source{profile,dt::DateValueStateV3::value,0};
  auto check_cancel=[&](dt::DateCastRequestV3 request,unsigned expected_pins,std::string_view label){const long baseline=profile.use_count();PinProbe probe{0,1,profile,baseline,0};request.control={~std::uint64_t{0},StopWithPin,&probe};auto result=dt::CastDateValueV3(request);Check(!result.ok()&&result.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&probe.calls==1&&probe.observed==baseline+expected_pins&&profile.use_count()==baseline,label);};
  dt::DateCastRequestV3 identity;identity.one_based_policy_row=53;identity.context=dt::DatatypeCastContext::implicit;identity.date_source=&source;identity.date_target=&profile;check_cancel(identity,2,"identity cast holds two endpoint pins at cancellation and releases both");
  dt::DatatypeOperationValue scalar{dt::CanonicalTypeId::character,"1970-01-01",false,text_descriptor};dt::DateCastRequestV3 incoming;incoming.one_based_policy_row=24;incoming.context=dt::DatatypeCastContext::explicit_cast;incoming.scalar_source=&scalar;incoming.scalar_source_identity=text_identity;incoming.date_target=&profile;incoming.date_target_descriptor=&date_descriptor;check_cancel(incoming,1,"incoming cast holds target pin at cancellation and releases it");
  dt::DateCastRequestV3 outgoing;outgoing.one_based_policy_row=50;outgoing.context=dt::DatatypeCastContext::explicit_cast;outgoing.date_source=&source;outgoing.scalar_target=dt::CanonicalTypeId::character;outgoing.scalar_target_identity=text_identity;outgoing.scalar_target_descriptor=text_descriptor;check_cancel(outgoing,1,"outgoing cast holds source pin at cancellation and releases it");
  const long baseline=profile.use_count();PinProbe no_callback{0,1,profile,baseline,0};outgoing.control={~std::uint64_t{0},StopWithPin,&no_callback};outgoing.use_character_output_buffer=true;std::array<char,10> sentinel;sentinel.fill('x');const auto before=sentinel;outgoing.character_output=sentinel.data();outgoing.character_output_capacity=9;auto short_result=dt::CastDateValueV3(outgoing);Check(!short_result.ok()&&short_result.diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED"&&no_callback.calls==0&&sentinel==before&&profile.use_count()==baseline,"cast short capacity precedes callback, preserves output, releases pin");
  no_callback={0,1,profile,baseline,0};incoming.control={3,StopWithPin,&no_callback};auto resource_result=dt::CastDateValueV3(incoming);Check(!resource_result.ok()&&resource_result.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&no_callback.calls==0&&profile.use_count()==baseline,"incoming resource refusal precedes callback and releases target pin");
}
void Comparisons(){auto p=Profile();dt::DateValueViewV3 a{p.get(),dt::DateValueStateV3::value,-1},b{p.get(),dt::DateValueStateV3::value,0},n{p.get(),dt::DateValueStateV3::sql_null,0};Check(dt::CompareDateValuesV3(a,b).fact==dt::DateComparisonFactV3::less,"less");Check(dt::CompareDateValuesV3(b,a).fact==dt::DateComparisonFactV3::greater,"greater");auto e=dt::CompareDateValuesV3(a,a);Check(e.ok()&&e.fact==dt::DateComparisonFactV3::equal&&e.grouping_equivalent,"equal");auto nn=dt::CompareDateValuesV3(n,n);Check(nn.ok()&&nn.fact==dt::DateComparisonFactV3::unordered_null&&nn.null_equivalent&&nn.grouping_equivalent,"null facts");Check(dt::CompareDateValuesV3(n,a).fact==dt::DateComparisonFactV3::unordered_null,"null unordered");auto left_dirty=n,right_dirty=n;left_dirty.day=1;right_dirty.day=2;auto dirty=dt::CompareDateValuesV3(left_dirty,right_dirty);Check(!dirty.ok()&&Detail(dirty.diagnostic)=="comparison_left_state_invalid","both dirty comparison validates left state before right");left_dirty.day=0;dirty=dt::CompareDateValuesV3(left_dirty,right_dirty);Check(!dirty.ok()&&Detail(dirty.diagnostic)=="comparison_right_state_invalid","right dirty comparison state gate");auto other=p->comparison_fingerprint;other[0]^=1;auto mismatch=dt::CompareDateValuesWithValidatedCohortForConformanceV3(a,b,other);Check(!mismatch.ok()&&mismatch.diagnostic.diagnostic_code=="CTI.TEMPORAL.ORDERING_REFUSED"&&Detail(mismatch.diagnostic)=="comparison_cohort_mismatch","validated other cohort maps to ordering refusal");}
}
int main(){CastPolicy();ExactPeerDescriptorShapes();ContextualNullDescriptorless();CastRequestShapeAndContext();RuntimeCasts();CastCallerBufferAtomicity();ExactCastSemanticClosure();Hashes();SortKeys();SortKeyModePrecedence();CastCancellationPrecedence();BoundedAliasPrecedence();CastPinObservability();Comparisons();std::cout<<"PASS base.date operations/casts checks="<<checks<<'\n';}
