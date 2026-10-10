// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include "crud_support/bound_ordered_index_key.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "disk_device.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace num = scratchbird::libraries::sbl_numeric;
namespace dt = scratchbird::core::datatypes;
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace idx = api::bound_index_key;
namespace disk = scratchbird::storage::disk;
using boost::multiprecision::cpp_int;
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
num::ExactDecimalProfile Profile(unsigned p, unsigned s) {
  return {p <= 38 ? num::ExactDecimalCodec::le24_v1 : num::ExactDecimalCodec::le40_v1, p, s};
}
std::string Fixed(std::string digits, unsigned scale) {
  if (!scale) return digits;
  if (digits.size() <= scale) digits.insert(0, scale-digits.size()+1, '0');
  digits.insert(digits.size()-scale, 1, '.');
  return digits;
}
std::vector<std::uint8_t> IndependentBytes(const std::string& digits, unsigned scale,
                                         bool negative, unsigned width) {
  cpp_int coefficient(digits);
  std::vector<std::uint8_t> b(width);
  b[0] = scale | (negative ? 128 : 0);
  b[1] = std::max(digits.size(), std::size_t(scale));
  for (unsigned g = 0; coefficient != 0; ++g) {
    const auto group = (coefficient % 1000000000).convert_to<std::uint32_t>();
    coefficient /= 1000000000;
    for (unsigned i = 0; i < 4; ++i) b[4 + 4*g + i] = group >> (8*i);
    ++b[2];
  }
  return b;
}
void Matrix() {
  for (unsigned p = 1; p <= 76; ++p) for (unsigned s = 0; s <= p; ++s) {
    const auto profile = Profile(p,s);
    const std::string largest = Fixed(std::string(p,'9'),s);
    std::array<std::uint8_t,40> prior{};
    bool first = true;
    for (const auto& text : {"-"+largest, std::string("0"), largest}) {
      const auto encoded = num::EncodeBoundExactDecimal(text, profile);
      Check(encoded.ok() && encoded.bytes.size() == (p <= 38 ? 24u : 40u), "bound encode or width");
      const auto decoded = num::DecodeBoundExactDecimal(encoded.bytes.data(), encoded.bytes.size(), profile, true);
      Check(decoded.ok() && decoded.text == text && decoded.bytes == encoded.bytes, "roundtrip bound value");
      const auto key = num::MakeExactDecimalOrderKey(encoded.bytes.data(), encoded.bytes.size(), profile);
      Check(key.key && (first || prior < *key.key), "signed boundary order");
      prior = *key.key; first = false;
      if (text != "0") Check(encoded.bytes == IndependentBytes(std::string(p,'9'),s,text.front()=='-',encoded.bytes.size()),
                             "independent coefficient oracle");
      if (p <= 38) {
        const auto legacy = num::EncodeExactDecimalLittleEndian(text);
        Check(legacy.ok && std::equal(encoded.bytes.begin(),encoded.bytes.end(),legacy.canonical_bytes.begin()),
              "narrow byte compatibility");
      }
    }
    const auto quantum = num::EncodeBoundExactDecimal(Fixed("1",s), profile);
    Check(quantum.ok(), "minimum positive quantum");
    const auto over = num::EncodeBoundExactDecimal(Fixed("1"+std::string(p,'0'),s), profile);
    Check(!over.ok() && over.error == num::ExactDecimalError::precision_overflow && over.bytes.empty(), "overflow must not round");
    const auto under = num::EncodeBoundExactDecimal(Fixed("1",s+1), profile);
    Check(!under.ok() && under.error == num::ExactDecimalError::scale_loss && under.bytes.empty(), "scale must not round");
    auto malformed = quantum.bytes;
    malformed[3] = 1;
    Check(!num::MakeExactDecimalOrderKey(malformed.data(),malformed.size(),profile).key, "reserved byte accepted");
    const auto wrong = Profile(p <= 38 ? 39 : 38, std::min(s,38u));
    Check(num::ValidateExactDecimal(quantum.bytes.data(),quantum.bytes.size(),wrong) != num::ExactDecimalError::none,
          "codec selected by value width");
  }
  for (const auto profile : {Profile(0,0), Profile(77,0), Profile(38,39),
                             num::ExactDecimalProfile{num::ExactDecimalCodec::le24_v1,39,0},
                             num::ExactDecimalProfile{static_cast<num::ExactDecimalCodec>(255),38,0}})
    Check(!num::EncodeBoundExactDecimal("0",profile).ok(), "invalid profile accepted");
  for (const auto& text : {"NaN","Infinity","1x","","1e999999999999"})
    Check(!num::EncodeBoundExactDecimal(text,Profile(76,0)).ok(), "invalid lexical accepted");
  Check(num::EncodeBoundExactDecimal("0e999999999999",Profile(76,0)).ok() &&
        num::EncodeBoundExactDecimal("-0e-999999999999",Profile(76,0)).text=="0",
        "bounded exponent scan changed zero semantics");
  Check(num::EncodeBoundExactDecimal("1e999999",Profile(76,0)).error == num::ExactDecimalError::precision_overflow &&
        num::EncodeBoundExactDecimal("1e-999999",Profile(76,76)).error == num::ExactDecimalError::scale_loss,
        "exponent extent refusal classification");
  for(const auto& text : {"-99999999999999999999.123456789012345678", "-1", "0", "0.000000000000000001", "1"}) {
    const auto narrow=num::EncodeBoundExactDecimal(text,Profile(38,18));
    const auto wide=num::EncodeBoundExactDecimal(text,Profile(76,18));
    Check(narrow.ok() && wide.ok() &&
        num::MakeExactDecimalOrderKey(narrow.bytes.data(),narrow.bytes.size(),Profile(38,18)).key ==
        num::MakeExactDecimalOrderKey(wide.bytes.data(),wide.bytes.size(),Profile(76,18)).key,
        "cross-codec numeric equality");
  }
  for (const auto profile : {Profile(38,4),Profile(76,4)}) {
    const auto canonical = num::EncodeBoundExactDecimal("1.25",profile);
    Check(canonical.ok() && num::EncodeBoundExactDecimal("+01.2500",profile).bytes == canonical.bytes,
          "lexical aliases differ");
    std::array<std::uint8_t,40> expected_key{};
    expected_key[0]=2; expected_key[1]=76; expected_key[2]=0x12; expected_key[3]=0x50;
    Check(num::MakeExactDecimalOrderKey(canonical.bytes.data(),canonical.bytes.size(),profile).key == expected_key,
          "independent exact key byte oracle");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto b = canonical.bytes;
      switch(mutation) {
        case 0: b[1]=0; break;
        case 1: b[1]=77; break;
        case 2: b[2]=0; break;
        case 3: b[2]=10; break;
        case 4: b.back()=1; break;
        case 5: b[4]=0; b[5]=0xca; b[6]=0x9a; b[7]=0x3b; break; // group1e9
        case 6: b[2]=2; break;
        case 7: b[4]=120; break; // trailing fractional zero
      }
      Check(!num::MakeExactDecimalOrderKey(b.data(),b.size(),profile).key, "malformed binary accepted");
    }
    auto zero = num::EncodeBoundExactDecimal("0",profile).bytes;
    zero[0]=128;
    Check(num::ValidateExactDecimal(zero.data(),zero.size(),profile) == num::ExactDecimalError::invalid_encoding,
          "negative zero accepted");
  }
}
void RationalOracle() {
  struct Sample { cpp_int units; std::array<std::uint8_t,40> key; };
  std::vector<Sample> values;
  std::uint64_t state = 41;
  const auto next = [&] { state ^= state<<13; state ^= state>>7; state ^= state<<17; return state; };
  for (unsigned i = 0; i < 600; ++i) {
    const unsigned digits = 1 + next()%38;
    const unsigned scale = next()%39;
    std::string coefficient(1,static_cast<char>('1'+next()%9));
    while(coefficient.size()<digits) coefficient += static_cast<char>('0'+next()%10);
    coefficient.back() = '1'+next()%9;
    const bool negative = next()&1;
    auto b = IndependentBytes(coefficient,scale,negative,40);
    const auto key = num::MakeExactDecimalOrderKey(b.data(),b.size(),Profile(76,38));
    Check(key.key.has_value(), "oracle profile input refused");
    cpp_int units(coefficient);
    for(unsigned s=scale;s<38;++s) units *= 10;
    if(negative) units = -units;
    values.push_back({units,*key.key});
  }
  std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.units<b.units;});
  for(unsigned i=1;i<values.size();++i)
    Check((values[i-1].units == values[i].units) ? values[i-1].key == values[i].key : values[i-1].key < values[i].key,
          "rational oracle order mismatch");
}
idx::OrderedIndexColumn Binding(unsigned p, unsigned s) {
  auto source = exec::MakeExecutorDescriptor("decimal", "nullability=nullable;precision="+
      std::to_string(p)+";scale="+std::to_string(s));
  if (p > 38) {
    api::CatalogColumnMetadata fields;
    fields.text = {{"nullability","nullable"},{"precision",std::to_string(p)},
                   {"scale",std::to_string(s)},{"decimal_codec_generation","1"}};
    fields.identities["decimal_codec_uuid"] = {{
        0x01,0xa1,0x18,0x6b,0xdf,0x04,0x7b,0x1b,0x9c,0xaf,0x87,0xb3,0xbd,0x29,0x94,0x3e}};
    Check(api::EncodeCatalogColumnMetadata(fields,&source.encoded_descriptor), "binary codec metadata encode");
  }
  idx::OrderedIndexColumn result;
  Check(dt::LookupDatatypeStorageIdentityV3(api::kBootstrapDatatypeCatalogUuid,
      api::kBootstrapDatatypeCatalogGeneration,api::kBootstrapDatatypeRegistryGeneration,
      source.datatype_descriptor_uuid,source.datatype_descriptor_generation,&result.datatype), "storage base identity missing");
  result.descriptor = {scratchbird::core::platform::UuidKind::object,source.datatype_descriptor_uuid};
  std::string detail;
  Check(idx::BuildOrderedColumnExecutionDescriptor(source,result.datatype,true,&result.execution_descriptor,&detail), "bound descriptor missing");
  return result;
}
void BindingsAndDisk() {
  const auto wide_binding=Binding(76,38);
  auto missing=exec::MakeExecutorDescriptor("decimal","nullability=nullable;precision=76;scale=38");
  auto unchanged=wide_binding.execution_descriptor;
  std::string detail;
  Check(!idx::BuildOrderedColumnExecutionDescriptor(missing,wide_binding.datatype,true,&unchanged,&detail) &&
        unchanged.precision==76, "wide codec was inferred without occurrence identity");
  api::CatalogColumnMetadata fields;
  fields.text={{"nullability","nullable"},{"precision","76"},{"scale","38"},{"decimal_codec_generation","1"}};
  fields.identities["decimal_codec_uuid"]={{
      0x01,0xa1,0x18,0x6b,0xdf,0x04,0x7b,0x1b,0x9c,0xaf,0x87,0xb3,0xbd,0x29,0x94,0x3e}};
  for(unsigned mutation=0;mutation<7;++mutation) {
    auto changed=fields;
    switch(mutation) {
      case 0: changed.identities["decimal_codec_uuid"].bytes[15]^=1; break;
      case 1: changed.text["decimal_codec_generation"]="2"; break;
      case 2: changed.text["codec_id"]="datatype.decimal.base1e9.le.v1"; break;
      case 3: changed.text["precision"]="38"; break;
      case 4: changed.text["precision"]="0"; break;
      case 5: changed.text.erase("scale"); break;
      case 6: changed.identities["codec_uuid"]={{
          0x01,0xa1,0x18,0x6b,0xdf,0x04,0x76,0x0a,0xa3,0xd5,0xd0,0x2d,0x2e,0xe3,0x01,0x54}};
          changed.text["codec_generation"]="1"; break;
    }
    Check(api::EncodeCatalogColumnMetadata(changed,&missing.encoded_descriptor),"mutation metadata encoding");
    Check(!idx::BuildOrderedColumnExecutionDescriptor(missing,wide_binding.datatype,true,&unchanged,&detail),
          "contradictory occurrence codec admitted");
  }
  std::vector<std::uint8_t> retained;
  struct Record { unsigned p,s; std::vector<std::uint8_t> bytes; std::string key; };
  std::vector<Record> records;
  for(const auto profile : {Profile(38,1),Profile(39,1),Profile(76,38),Profile(76,76)}) {
    const auto binding = Binding(profile.precision,profile.scale);
    std::string last;
    for (const auto& text : {"-"+Fixed(std::string(profile.precision,'9'),profile.scale), std::string("0"),
                             Fixed("1",profile.scale), Fixed(std::string(profile.precision,'9'),profile.scale)}) {
      auto v = num::EncodeBoundExactDecimal(text,profile);
      Check(v.ok(), "storage fixture encode failed");
      std::string key; bool null_key=true; api::EngineApiDiagnostic diagnostic;
      const std::string binary(v.bytes.begin(),v.bytes.end());
      Check(idx::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({binary}),{binding},&key,&null_key,&diagnostic) && !null_key &&
          (last.empty() || last < key), "bound physical index order failed");
      last=key;
      records.push_back({profile.precision,profile.scale,v.bytes,key});
      // Explicit fixture envelope; not a claimed production catalog/page format.
      retained.push_back(profile.precision); retained.push_back(profile.scale);
      dt::DatatypeSortKeyRequest request;
      request.value = {dt::CanonicalTypeId::decimal,binary,false};
      Check(dt::BindExactDecimalSortKeyProfile(binding.execution_descriptor,&request), "Core profile bind failed");
      for(auto byte:request.decimal_codec_uuid.bytes) retained.push_back(byte);
      for(unsigned i=0;i<8;++i) retained.push_back(request.decimal_codec_generation>>(8*i));
      retained.insert(retained.end(),v.bytes.begin(),v.bytes.end());
      const auto core_key=dt::MakeDatatypeSortKey(request);
      Check(core_key.ok() && core_key.sort_key.size()==116 &&
          static_cast<unsigned char>(core_key.sort_key[72])==profile.precision &&
          static_cast<unsigned char>(core_key.sort_key[73])==profile.scale &&
          core_key.sort_key[74]==0 && core_key.sort_key[75]==1,
          "Core decimal sort framing failed");
      const std::array<std::uint8_t,16> order_uuid{{
          0x01,0xa1,0x18,0x6b,0xdf,0x04,0x75,0x4f,0x98,0xf7,0x47,0x0b,0x3b,0x66,0x47,0xa4}};
      Check(std::equal(order_uuid.begin(),order_uuid.end(),core_key.sort_key.begin(),
          [](unsigned char expected,char actual){return expected==static_cast<unsigned char>(actual);}) &&
          core_key.sort_key.substr(16,8)==std::string("\0\0\0\0\0\0\0\1",8),
          "ordering binary identity/generation framing");
      for(unsigned mutation=0;mutation<9;++mutation) {
        auto bad=request;
        switch(mutation) {
          case 0: bad.decimal_codec_uuid.bytes[0]^=1; break;
          case 1: ++bad.decimal_codec_generation; break;
          case 2: bad.decimal_ordering_uuid.bytes[0]^=1; break;
          case 3: ++bad.decimal_ordering_generation; break;
          case 4: ++bad.value.descriptor.descriptor_epoch; break;
          case 5: bad.value.descriptor.precision=77; break;
          case 6: bad.value.encoded_value="1.0"; break;
          case 7: bad.value.descriptor.domain_stack.push_back(bad.value.descriptor.descriptor_uuid); break;
          case 8: bad.case_insensitive_character_compare=true; break;
        }
        auto refused = dt::MakeDatatypeSortKey(bad);
        Check(!refused.ok() && refused.sort_key.empty(), "substituted profile accepted or output leaked");
      }
    }
    auto null_binding=binding;
    std::string result="unchanged"; bool is_null=false; api::EngineApiDiagnostic diagnostic;
    Check(idx::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({api::CrudStoredValue::SqlNull()}),{binding},
        &result,&is_null,&diagnostic) && is_null, "nullable decimal NULL rejected");
    null_binding.execution_descriptor.nullable_allowed=false;
    result="unchanged"; is_null=false;
    Check(!idx::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({api::CrudStoredValue::SqlNull()}),{null_binding},
        &result,&is_null,&diagnostic) && result=="unchanged" && !is_null, "nonnullable decimal published NULL");
  }
  const auto path=std::filesystem::temp_directory_path()/
      ("sb-exact-decimal-profiles-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Check(std::filesystem::create_directory(path), "fixture directory collision");
  struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);} } cleanup{path};
  disk::FileDevice file;
  Check(file.Open((path/"values.bin").string(),disk::FileOpenMode::create_new).ok(), "create real file");
  const auto written=file.WriteAt(0,retained.data(),retained.size());
  Check(written.ok() && written.bytes_transferred==retained.size() && file.Sync().ok() && file.Close().ok(), "durable write");
  Check(file.Open((path/"values.bin").string(),disk::FileOpenMode::open_existing_read_only).ok(), "reopen");
  std::vector<std::uint8_t> recovered(retained.size());
  const auto read=file.ReadAt(0,recovered.data(),recovered.size());
  Check(read.ok() && read.bytes_transferred==recovered.size() && recovered==retained && file.Close().ok(), "recovered binary identity");
  unsigned offset=0;
  for(const auto& record:records) {
    unsigned p=recovered[offset++],s=recovered[offset++];
    Check(p==record.p && s==record.s, "recovered occurrence parameters");
    auto binding=Binding(p,s);
    dt::DatatypeSortKeyRequest expected;
    Check(dt::BindExactDecimalSortKeyProfile(binding.execution_descriptor,&expected),"reopened codec profile");
    for(auto byte:expected.decimal_codec_uuid.bytes)
      Check(recovered[offset++]==byte,"recovered binary codec identity");
    for(unsigned i=0;i<8;++i)
      Check(recovered[offset++]==static_cast<std::uint8_t>(expected.decimal_codec_generation>>(8*i)),"recovered codec generation");
    const auto size=p<=38?24u:40u;
    std::string value(reinterpret_cast<const char*>(recovered.data()+offset),size);
    std::string key; bool null_key=false; api::EngineApiDiagnostic diagnostic;
    Check(idx::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}),{binding},&key,&null_key,&diagnostic) &&
          key==record.key, "reopened physical key differs");
    offset+=size;
  }
}
void ValidationTiming() {
  const auto profile=Profile(38,2);
  const auto value=num::EncodeBoundExactDecimal("123456789012345678901234567890123456.78",profile);
  Check(value.ok(),"validation timing fixture");
  constexpr unsigned iterations=10000;
  unsigned legacy_ok=0,binary_ok=0;
  auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<iterations;++i)
    legacy_ok += num::DecodeExactDecimalLittleEndian(value.bytes.data(),value.bytes.size()).ok;
  auto middle=std::chrono::steady_clock::now();
  for(unsigned i=0;i<iterations;++i)
    binary_ok += num::ValidateExactDecimal(value.bytes.data(),value.bytes.size(),profile)==num::ExactDecimalError::none;
  auto end=std::chrono::steady_clock::now();
  Check(legacy_ok==iterations && binary_ok==iterations,"validation timing results differ");
  std::cout<<"validation-only microbenchmark "<<iterations<<" values: legacy_render_reencode_us="
      <<std::chrono::duration_cast<std::chrono::microseconds>(middle-start).count()
      <<" bound_binary_us="<<std::chrono::duration_cast<std::chrono::microseconds>(end-middle).count()<<'\n';
}
int main() {
  try { Matrix(); RationalOracle(); BindingsAndDisk(); ValidationTiming(); }
  catch(const std::exception& e){std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;}
  std::cout<<checks<<" exact decimal profile checks passed\n";
}
