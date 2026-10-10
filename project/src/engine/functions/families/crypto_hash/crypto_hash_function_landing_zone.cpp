// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/crypto_hash/crypto_hash_function_landing_zone.hpp"

#include "common/function_result_helpers.hpp"
#include "internal_api/api_types.hpp"
#include "uuid.hpp"
#include "blake3_digest.hpp"
#include "scrypt_kdf.hpp"
#include "openpgp_armor.hpp"
#include "../../../../core/datatypes/canonical_utf8.hpp"
#include "sblr/sblr_uint16_projection_profile.hpp"
#include "../../../../core/common/crypto_random.hpp"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

constexpr std::size_t kMaxCryptoInputBytes = 1024 * 1024;
constexpr std::size_t kMaxRandomBytes = 1024;
constexpr std::uint64_t kScryptMaxMemory = 32ULL * 1024ULL * 1024ULL;

bool IdIs(const std::string& id, std::initializer_list<std::string_view> names) {
  for (const auto name : names) {
    const std::string text(name);
    if (id == text || id == "sb.crypto." + text || id == "sb.fn.crypto." + text) return true;
  }
  return false;
}

std::string Trim(std::string_view input) {
  std::size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
  std::size_t last = input.size();
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
  return std::string(input.substr(first, last - first));
}

bool AnyNull(const FunctionCallRequest& request) {
  for (const auto& argument : request.arguments) {
    if (IsSqlNull(argument.value)) return true;
  }
  return false;
}

const unsigned char* BytesPtr(const std::vector<std::uint8_t>& bytes) {
  return bytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(bytes.data());
}

std::vector<std::uint8_t> RawBytesFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::binary) return value.binary_value;
  const auto text = ValueAsText(value);
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string HexEncode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

std::string Base64Encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t a = bytes[i];
    const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3f] : '=');
  }
  return out;
}

int Base64Value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return 26 + (ch - 'a');
  if (ch >= '0' && ch <= '9') return 52 + (ch - '0');
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

bool Base64Decode(const std::string& text, std::vector<std::uint8_t>* out) {
  if ((text.size() % 4) != 0) return false;
  out->clear();
  out->reserve((text.size() / 4) * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const char c0 = text[i];
    const char c1 = text[i + 1];
    const char c2 = text[i + 2];
    const char c3 = text[i + 3];
    const int v0 = Base64Value(c0);
    const int v1 = Base64Value(c1);
    const int v2 = c2 == '=' ? 0 : Base64Value(c2);
    const int v3 = c3 == '=' ? 0 : Base64Value(c3);
    if (v0 < 0 || v1 < 0 || (c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) return false;
    if (c2 == '=' && c3 != '=') return false;
    const std::uint32_t triple = (static_cast<std::uint32_t>(v0) << 18) |
                                 (static_cast<std::uint32_t>(v1) << 12) |
                                 (static_cast<std::uint32_t>(v2) << 6) |
                                 static_cast<std::uint32_t>(v3);
    out->push_back(static_cast<std::uint8_t>((triple >> 16) & 0xff));
    if (c2 != '=') out->push_back(static_cast<std::uint8_t>((triple >> 8) & 0xff));
    if (c3 != '=') out->push_back(static_cast<std::uint8_t>(triple & 0xff));
  }
  return true;
}

struct HmacProfile {
  const EVP_MD* digest;
  unsigned output_bytes;
};

std::optional<HmacProfile> HmacAlgorithm(std::string_view algorithm) {
  const auto matches=[&](std::string_view token) {
    if(algorithm.size()!=token.size())return false;
    for(std::size_t i=0;i<token.size();++i) {
      const auto ch=static_cast<unsigned char>(algorithm[i]);
      const auto folded=ch>='A'&&ch<='Z'?ch+('a'-'A'):ch;
      if(folded!=static_cast<unsigned char>(token[i]))return false;
    }
    return true;
  };
  if(matches("sha256"))return HmacProfile{EVP_sha256(),32};
  if(matches("sha512"))return HmacProfile{EVP_sha512(),64};
  if(matches("sha3_256")||matches("sha3-256"))return HmacProfile{EVP_sha3_256(),32};
  if(matches("sha3_512")||matches("sha3-512"))return HmacProfile{EVP_sha3_512(),64};
  if(matches("blake2b")||matches("blake2b512"))return HmacProfile{EVP_blake2b512(),64};
  return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> DigestBytes(
    const EVP_MD* md, const std::vector<std::uint8_t>& bytes, std::size_t expected_size) {
  if (!md) return std::nullopt;
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  if (EVP_Digest(BytesPtr(bytes), bytes.size(), digest.data(), &digest_len, md, nullptr) != 1) return std::nullopt;
  if (digest_len != expected_size || digest_len > digest.size()) return std::nullopt;
  return std::vector<std::uint8_t>(digest.begin(), digest.begin() + digest_len);
}

bool ParseUint64(const scratchbird::engine::sblr::SblrValue& value, std::uint64_t* out) {
  if (value.has_uint64_value) {
    *out = value.uint64_value;
    return true;
  }
  if (value.has_int64_value) {
    if (value.int64_value < 0) return false;
    *out = static_cast<std::uint64_t>(value.int64_value);
    return true;
  }
  const std::string text = Trim(ValueAsText(value));
  if (text.empty()) return false;
  std::size_t used = 0;
  try {
    const auto parsed = std::stoull(text, &used, 10);
    if (used != text.size()) return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

FunctionCallResult DependencyUnavailable(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
                                      "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE",
                                      std::move(detail));
}

template <typename DigestProvider>
FunctionCallResult DigestFunction(const FunctionCallRequest& request, DigestProvider provider) {
  const auto invalid=[&] { return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "CRYPTO.HASH.INVALID_INPUT", "fixed digest requires one canonical binary value"); };
  if (request.arguments.size() != 1) return invalid();
  const auto& value=request.arguments[0].value;
  if (value.descriptor_id != "binary" || !value.text_value.empty() ||
      !value.encoded_value.empty() || !value.charset_name.empty() || !value.collation_name.empty() ||
      value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
      !value.uuid_value.is_nil() || !value.uuid_array_value.empty()) return invalid();
  if (value.is_null) {
    if (value.payload_kind != scratchbird::engine::sblr::SblrValuePayloadKind::none || !value.binary_value.empty()) return invalid();
    return MakeFunctionSuccess(request, {MakeNullValue("binary")});
  }
  if (value.payload_kind != scratchbird::engine::sblr::SblrValuePayloadKind::binary ||
      value.binary_value.size() > kMaxCryptoInputBytes) return invalid();
  auto digest = provider(value.binary_value);
  if (!digest) return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
      "CRYPTO.PROFILE.UNAVAILABLE", "Core fixed digest provider did not produce its exact output size");
  return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(*digest))});
}

FunctionCallResult HmacFunction(const FunctionCallRequest& request) {
  const auto invalid=[&] {return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "CRYPTO.HMAC.INVALID_INPUT", "HMAC requires matching typed value/key carriers and a text algorithm");};
  if(request.arguments.size()!=3)return invalid();
  const auto& data=request.arguments[0].value;
  const auto& key=request.arguments[1].value;
  const auto& algorithm=request.arguments[2].value;
  const auto valid=[](const scratchbird::engine::sblr::SblrValue& value) {
    using Kind=scratchbird::engine::sblr::SblrValuePayloadKind;
    const bool binary=value.descriptor_id=="binary";
    if(!binary&&value.descriptor_id!="character")return false;
    if(value.has_int64_value||value.has_uint64_value||value.has_real64_value||!value.uuid_value.is_nil()||!value.uuid_array_value.empty())return false;
    if(binary&&(!value.charset_name.empty()||!value.collation_name.empty()))return false;
    if(value.is_null)return value.payload_kind==Kind::none&&value.binary_value.empty()&&value.text_value.empty()&&value.encoded_value.empty();
    if(binary)return value.payload_kind==Kind::binary&&value.text_value.empty()&&value.encoded_value.empty();
    // Text is already descriptor-encoded by its owning admission boundary. Do
    // not transcode, prefer a display mirror, normalize, or silently use UTF8.
    return value.payload_kind==Kind::text&&value.binary_value.empty()&&
           (value.encoded_value.empty()||value.encoded_value==value.text_value);
  };
  if(!valid(data)||!valid(key)||!valid(algorithm)||data.descriptor_id!=key.descriptor_id||
     algorithm.descriptor_id!="character")return invalid();
  if(data.is_null||key.is_null||algorithm.is_null)return MakeFunctionSuccess(request,{MakeNullValue("binary")});
  const bool binary=data.descriptor_id=="binary";
  const auto data_size=binary?data.binary_value.size():data.text_value.size();
  const auto key_size=binary?key.binary_value.size():key.text_value.size();
  if(data_size>kMaxCryptoInputBytes||key_size>kMaxCryptoInputBytes||key_size>static_cast<std::size_t>(std::numeric_limits<int>::max()))return invalid();
  const auto profile=HmacAlgorithm(algorithm.text_value);
  if(!profile)return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "CRYPTO.HMAC.UNSUPPORTED_ALGORITHM", "HMAC algorithm is not a registered Core profile");
  const unsigned char empty=0;
  const auto* data_bytes=data_size==0?&empty:(binary?data.binary_value.data():reinterpret_cast<const unsigned char*>(data.text_value.data()));
  const auto* key_bytes=key_size==0?&empty:(binary?key.binary_value.data():reinterpret_cast<const unsigned char*>(key.text_value.data()));
  struct Scratch {
    std::array<unsigned char,EVP_MAX_MD_SIZE> bytes{};
    ~Scratch(){OPENSSL_cleanse(bytes.data(),bytes.size());}
  } out;
  unsigned out_len=0;
  if(!profile->digest||HMAC(profile->digest,key_bytes,static_cast<int>(key_size),data_bytes,data_size,out.bytes.data(),&out_len)!=out.bytes.data()||
     out_len!=profile->output_bytes||out_len>out.bytes.size())return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
      "CRYPTO.PROFILE.UNAVAILABLE", "Core HMAC provider did not produce its exact output size");
  return MakeFunctionSuccess(request,{MakeBinaryValue("binary",{out.bytes.begin(),out.bytes.begin()+out_len})});
}

FunctionCallResult RandomBytesFunction(const FunctionCallRequest& request) {
  const auto invalid=[&] {return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "CRYPTO.RNG.INVALID_LENGTH", "random-byte count must be one uint32 in 1..1024");};
  if(request.arguments.size()!=1)return invalid();
  const auto& count=request.arguments[0].value;
  using Kind=scratchbird::engine::sblr::SblrValuePayloadKind;
  if(count.descriptor_id!="uint32"||!count.binary_value.empty()||!count.uuid_value.is_nil()||!count.uuid_array_value.empty()||
     !count.charset_name.empty()||!count.collation_name.empty()||count.has_int64_value||count.has_real64_value)return invalid();
  if(count.is_null) {
    if(count.payload_kind!=Kind::none||count.has_uint64_value||!count.text_value.empty()||!count.encoded_value.empty())return invalid();
    return MakeFunctionSuccess(request,{MakeNullValue("binary")});
  }
  // Native integer bits are authoritative. The current scalar helper's decimal
  // mirrors may be absent or exact; they are never parsed or used as a count.
  if(count.payload_kind!=Kind::unsigned_integer||!count.has_uint64_value||
     count.uint64_value<1||count.uint64_value>kMaxRandomBytes)return invalid();
  const auto decimal=std::to_string(count.uint64_value);
  if((!count.text_value.empty()&&count.text_value!=decimal)||
     (!count.encoded_value.empty()&&count.encoded_value!=decimal))return invalid();
  struct Scratch {
    std::array<unsigned char,kMaxRandomBytes> bytes{};
    ~Scratch(){OPENSSL_cleanse(bytes.data(),bytes.size());}
  } entropy;
  const auto size=static_cast<std::size_t>(count.uint64_value);
  if(!scratchbird::core::FillCryptographicRandomBytes(entropy.bytes.data(),size))return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
      "CRYPTO.RNG.UNAVAILABLE", "Core cryptographic RNG did not provide random bytes");
  return MakeFunctionSuccess(request,{MakeBinaryValue("binary",{entropy.bytes.begin(),entropy.bytes.begin()+size})});
}

FunctionCallResult RandomUuidFunction(const FunctionCallRequest& request) {
  if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "gen_random_uuid expects no arguments");
  const auto generated = scratchbird::core::uuid::GenerateCompatibilityRandomV4();
  if (!generated.ok()) {
    return RefuseFunctionWithDiagnostic(request,
        scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
        "CRYPTO.RNG.UNAVAILABLE", "Core cryptographic RNG did not provide UUID entropy");
  }
  return MakeFunctionSuccess(request, {scratchbird::engine::sblr::MakeSblrUuidValue(generated.value)});
}

FunctionCallResult GenSaltFunction(const FunctionCallRequest& request) {
  const auto error=[&](const char* code,const char* detail) {return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,code,detail);};
  const auto invalid=[&] {return error("CRYPTO.PASSWORD.INVALID_PARAMETER","salt generation requires exact text and optional uint32 operands");};
  const auto cancelled=[&] {const auto* owner=request.context.engine_request_context;
    return owner&&owner->query_cancellation_requested&&owner->query_cancellation_requested();};
  const auto cancel_result=[&] {return error("PROCESS.CANCELLED","salt generation was cancelled");};
  const bool explicit_cost=IdIs(request.context.function_id,{"gen_salt_algo"});
  if(request.arguments.empty()||request.arguments.size()>2||(explicit_cost&&request.arguments.size()!=2))return invalid();
  using Kind=scratchbird::engine::sblr::SblrValuePayloadKind;
  for(std::size_t i=0;i<request.arguments.size();++i) {
    const auto& v=request.arguments[i].value;
    if(v.descriptor_id!=(i==0?"character":"uint32")||!v.binary_value.empty()||
       !v.uuid_value.is_nil()||!v.uuid_array_value.empty()||v.has_int64_value||v.has_real64_value||
       v.int64_value!=0||v.real64_value!=0.0)return invalid();
    if(i!=0&&(!v.charset_name.empty()||!v.collation_name.empty()))return invalid();
    if(v.is_null) {
      if(v.payload_kind!=Kind::none||v.has_uint64_value||v.uint64_value!=0||
         !v.text_value.empty()||!v.encoded_value.empty())return invalid();
    } else if(i==0) {
      if(v.payload_kind!=Kind::text||v.has_uint64_value||v.uint64_value!=0)return invalid();
      if(!v.encoded_value.empty()) {
        if(v.encoded_value.size()!=v.text_value.size())return invalid();
        for(std::size_t at=0;at<v.text_value.size();) {
          if(cancelled())return cancel_result();
          const auto count=std::min(std::size_t{4096},v.text_value.size()-at);
          if(std::memcmp(v.encoded_value.data()+at,v.text_value.data()+at,count)!=0)return invalid();
          at+=count;
        }
      }
    } else {
      if(v.payload_kind!=Kind::unsigned_integer||!v.has_uint64_value||v.uint64_value>0xffffffffULL)return invalid();
      const auto decimal=std::to_string(v.uint64_value);
      if((!v.text_value.empty()&&v.text_value!=decimal)||(!v.encoded_value.empty()&&v.encoded_value!=decimal))return invalid();
    }
  }
  if(cancelled())return cancel_result();
  if(AnyNull(request)) {
    auto result=MakeFunctionSuccess(request,{MakeNullValue("character")});
    if(cancelled())return cancel_result();
    return result;
  }
  const auto& algorithm=request.arguments[0].value.text_value;
  const auto matches=[&](std::string_view token) {
    if(algorithm.size()!=token.size())return false;
    for(std::size_t i=0;i<token.size();++i) {
      const auto c=static_cast<unsigned char>(algorithm[i]);
      if((c>='A'&&c<='Z'?c+('a'-'A'):c)!=token[i])return false;
    }
    return true;
  };
  const bool bcrypt=matches("bf")||matches("bcrypt");
  if(!bcrypt&&!matches("md5"))return error("CRYPTO.PASSWORD.UNSUPPORTED_ALGORITHM","salt algorithm is not a registered Core profile");
  // These are format defaults, not permission to use a password-hashing
  // profile. Owning security policy can prohibit a weak/legacy profile.
  const auto cost=request.arguments.size()==2?request.arguments[1].value.uint64_value:(bcrypt?6u:1000u);
  if((bcrypt&&(cost<4||cost>31))||(!bcrypt&&cost!=1000))
    return error("CRYPTO.PASSWORD.INVALID_COST","salt cost is outside the selected algorithm profile");
  auto result=MakeFunctionSuccess(request,{});
  result.result.scalar_values.resize(1);
  auto& out=result.result.scalar_values[0];out=MakeTextValue("character",{});out.charset_name="utf8";
  struct Pending {
    scratchbird::engine::sblr::SblrValue& value;bool published=false;
    ~Pending(){if(!published){OPENSSL_cleanse(value.text_value.data(),value.text_value.size());
      OPENSSL_cleanse(value.encoded_value.data(),value.encoded_value.size());}}
  } pending{out};
  out.text_value.resize(bcrypt?29:12);out.encoded_value.resize(out.text_value.size());
  struct Entropy {std::array<unsigned char,16> bytes{};~Entropy(){OPENSSL_cleanse(bytes.data(),bytes.size());}} entropy;
  if(cancelled())return cancel_result();
  const auto count=bcrypt?16u:6u;
  if(!scratchbird::core::FillCryptographicRandomBytes(entropy.bytes.data(),count))
    return error("CRYPTO.RNG.UNAVAILABLE","Core cryptographic RNG did not provide salt entropy");
  if(cancelled())return cancel_result();
  const char* alphabet=bcrypt?"./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789":
                               "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  auto* destination=out.text_value.data();
  std::size_t at;
  if(bcrypt) {
    std::memcpy(destination,"$2b$",4);destination[4]=static_cast<char>('0'+cost/10);
    destination[5]=static_cast<char>('0'+cost%10);destination[6]='$';at=7;
  } else {std::memcpy(destination,"$1$",3);at=3;}
  // MSB-first radix64. Bcrypt's final character has four zero pad bits;
  // all128 entropy bits participate. The MD5 setting uses all48 bits once.
  for(std::size_t i=0;i<count;i+=3) {
    const auto left=count-i;
    const std::uint32_t bits=(std::uint32_t{entropy.bytes[i]}<<16)|
      (left>1?std::uint32_t{entropy.bytes[i+1]}<<8:0)|(left>2?entropy.bytes[i+2]:0);
    destination[at++]=alphabet[bits>>18];destination[at++]=alphabet[(bits>>12)&63];
    if(left>1)destination[at++]=alphabet[(bits>>6)&63];
    if(left>2)destination[at++]=alphabet[bits&63];
  }
  if(!bcrypt)destination[at++]='$';
  std::memcpy(out.encoded_value.data(),destination,out.text_value.size());
  if(cancelled())return cancel_result();
  pending.published=true;return result;
}

FunctionCallResult ScryptFunction(const FunctionCallRequest& request) {
  const auto invalid=[&] {return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "CRYPTO.PASSWORD.INVALID_PARAMETER", "scrypt requires six exact typed arguments and valid RFC7914 parameters");};
  if(request.arguments.size()!=6)return invalid();
  using Kind=scratchbird::engine::sblr::SblrValuePayloadKind;
  constexpr std::array<std::string_view,6> types{"character","binary","uint64","uint32","uint32","uint16"};
  for(std::size_t i=0;i<6;++i) {
    const auto& value=request.arguments[i].value;
    if(i==5) {
      if(!scratchbird::engine::sblr::SblrUint16PayloadValid(value) ||
         (value.projection_descriptor &&
          !scratchbird::engine::sblr::SblrUint16ProjectionResolved(value)))return invalid();
      continue;
    }
    if(value.descriptor_id!=types[i]||!value.uuid_value.is_nil()||value.has_int64_value||value.has_real64_value)return invalid();
    if(i!=0&&(!value.charset_name.empty()||!value.collation_name.empty()))return invalid();
    if(value.is_null) {
      if(value.payload_kind!=Kind::none||value.has_uint64_value||!value.binary_value.empty()||!value.text_value.empty()||!value.encoded_value.empty())return invalid();
    } else if(i==0) {
      if(value.payload_kind!=Kind::text||value.has_uint64_value||!value.binary_value.empty()||
         (!value.encoded_value.empty()&&value.encoded_value!=value.text_value))return invalid();
    } else if(i==1) {
      if(value.payload_kind!=Kind::binary||value.has_uint64_value||!value.text_value.empty()||!value.encoded_value.empty())return invalid();
    } else {
      if(value.payload_kind!=Kind::unsigned_integer||!value.has_uint64_value||!value.binary_value.empty())return invalid();
      const auto maximum=i==2?std::numeric_limits<std::uint64_t>::max():std::uint64_t{0xffffffff};
      if(value.uint64_value>maximum)return invalid();
      const auto decimal=std::to_string(value.uint64_value);
      if((!value.text_value.empty()&&value.text_value!=decimal)||(!value.encoded_value.empty()&&value.encoded_value!=decimal))return invalid();
    }
  }
  auto cancelled=[&] {
    const auto* owner=request.context.engine_request_context;
    return owner&&owner->query_cancellation_requested&&owner->query_cancellation_requested();
  };
  const auto cancel_result=[&] {return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "PROCESS.CANCELLED", "scrypt execution was cancelled");};
  if(cancelled())return cancel_result();
  if(AnyNull(request)) {
    auto result=MakeFunctionSuccess(request,{MakeNullValue("binary")});
    if(cancelled())return cancel_result();
    return result;
  }
  const auto& password=request.arguments[0].value.text_value;
  const auto& salt=request.arguments[1].value.binary_value;
  const auto n=request.arguments[2].value.uint64_value;
  const auto r=request.arguments[3].value.uint64_value;
  const auto p=request.arguments[4].value.uint64_value;
  const auto key_len=request.arguments[5].value.uint64_value;
  namespace crypto=scratchbird::core::crypto;
  crypto::ScryptWorkEstimate cost;
  const auto estimate=crypto::EstimateScryptWork(password.size(),salt.size(),n,r,p,key_len,cost);
  if(estimate==crypto::ScryptEstimateCode::invalid_parameters)return invalid();
  // This retained implementation guard is not an engine resource receipt.
  // The owning engine's shared memory/CPU policy must replace it; never treat
  // an arithmetic/working-capacity refusal as an invalid RFC parameter tuple.
  if(estimate!=crypto::ScryptEstimateCode::ok||cost.workspace_bytes>kScryptMaxMemory||
      cost.workspace_bytes>std::numeric_limits<std::size_t>::max()||
      password.size()>kMaxCryptoInputBytes||salt.size()>kMaxCryptoInputBytes)
    return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
      "RESOURCE.BUDGET_EXCEEDED", "scrypt working capacity was exceeded");
  struct Secret {
    std::vector<std::uint8_t> bytes;
    ~Secret(){if(!bytes.empty())OPENSSL_cleanse(bytes.data(),bytes.size());}
  } key{std::vector<std::uint8_t>(static_cast<std::size_t>(key_len))};
  crypto::ScryptExecutionCode status;
  {
    // The native primitive owns erasure after accepting these disjoint extents.
    // Until that point the workspace contains only zeroes. Its allocation is
    // freed before any result preparation or final function publication probe.
    std::vector<std::uint8_t> workspace(static_cast<std::size_t>(cost.workspace_bytes));
    status=crypto::DeriveScryptKey(
      {reinterpret_cast<const std::uint8_t*>(password.data()),password.size()},
      {salt.data(),salt.size()},n,static_cast<std::uint32_t>(r),static_cast<std::uint32_t>(p),
      {workspace.data(),workspace.size()},{key.bytes.data(),key.bytes.size()},
      {[](void* context){return (*static_cast<const decltype(cancelled)*>(context))();},
       &cancelled});
  }
  if(status==crypto::ScryptExecutionCode::cancelled)return cancel_result();
  if(status!=crypto::ScryptExecutionCode::ok)return RefuseFunctionWithDiagnostic(request,
    scratchbird::engine::sblr::SblrStatusCode::execution_failed,
    "CRYPTO.PROFILE.UNAVAILABLE", "Core scrypt rejected its prepared invocation");
  // Prepare metadata and the single result slot before copying key material.
  // Avoid allocating initializer-list copies of secret values: every populated
  // but unpublished key allocation must have a cleanup owner through the fence.
  auto result=MakeFunctionSuccess(request,{});
  result.result.scalar_values.resize(1);
  result.result.scalar_values[0]=MakeBinaryValue("binary",{});
  struct PendingKey {
    std::vector<std::uint8_t>& bytes;
    bool committed=false;
    ~PendingKey(){if(!committed&&!bytes.empty())OPENSSL_cleanse(bytes.data(),bytes.size());}
  } pending{result.result.scalar_values[0].binary_value};
  pending.bytes=key.bytes;
  if(cancelled())return cancel_result();
  pending.committed=true;
  return result;
}

std::uint64_t Read32LE(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(p[0]) | (static_cast<std::uint64_t>(p[1]) << 8) |
         (static_cast<std::uint64_t>(p[2]) << 16) | (static_cast<std::uint64_t>(p[3]) << 24);
}

std::uint64_t Read64LE(const std::uint8_t* p) {
  return Read32LE(p) | (Read32LE(p + 4) << 32);
}

std::uint64_t Rotl64(std::uint64_t value, int count) {
  return (value << count) | (value >> (64 - count));
}

std::uint64_t Xxh64Round(std::uint64_t acc, std::uint64_t input) {
  constexpr std::uint64_t prime2 = 14029467366897019727ULL;
  constexpr std::uint64_t prime1 = 11400714785074694791ULL;
  acc += input * prime2;
  acc = Rotl64(acc, 31);
  acc *= prime1;
  return acc;
}

std::uint64_t Xxh64MergeRound(std::uint64_t acc, std::uint64_t val) {
  constexpr std::uint64_t prime1 = 11400714785074694791ULL;
  constexpr std::uint64_t prime4 = 9650029242287828579ULL;
  val = Xxh64Round(0, val);
  acc ^= val;
  acc = acc * prime1 + prime4;
  return acc;
}

std::uint64_t Xxh64(const std::vector<std::uint8_t>& input, std::uint64_t seed) {
  constexpr std::uint64_t prime1 = 11400714785074694791ULL;
  constexpr std::uint64_t prime2 = 14029467366897019727ULL;
  constexpr std::uint64_t prime3 = 1609587929392839161ULL;
  constexpr std::uint64_t prime4 = 9650029242287828579ULL;
  constexpr std::uint64_t prime5 = 2870177450012600261ULL;

  const std::uint8_t* p = input.data();
  const std::uint8_t* const end = p + input.size();
  std::uint64_t h64 = 0;
  if (input.size() >= 32) {
    const std::uint8_t* const limit = end - 32;
    std::uint64_t v1 = seed + prime1 + prime2;
    std::uint64_t v2 = seed + prime2;
    std::uint64_t v3 = seed + 0;
    std::uint64_t v4 = seed - prime1;
    do {
      v1 = Xxh64Round(v1, Read64LE(p)); p += 8;
      v2 = Xxh64Round(v2, Read64LE(p)); p += 8;
      v3 = Xxh64Round(v3, Read64LE(p)); p += 8;
      v4 = Xxh64Round(v4, Read64LE(p)); p += 8;
    } while (p <= limit);
    h64 = Rotl64(v1, 1) + Rotl64(v2, 7) + Rotl64(v3, 12) + Rotl64(v4, 18);
    h64 = Xxh64MergeRound(h64, v1);
    h64 = Xxh64MergeRound(h64, v2);
    h64 = Xxh64MergeRound(h64, v3);
    h64 = Xxh64MergeRound(h64, v4);
  } else {
    h64 = seed + prime5;
  }
  h64 += input.size();
  while (p + 8 <= end) {
    const std::uint64_t k1 = Xxh64Round(0, Read64LE(p));
    h64 ^= k1;
    h64 = Rotl64(h64, 27) * prime1 + prime4;
    p += 8;
  }
  if (p + 4 <= end) {
    h64 ^= Read32LE(p) * prime1;
    h64 = Rotl64(h64, 23) * prime2 + prime3;
    p += 4;
  }
  while (p < end) {
    h64 ^= (*p) * prime5;
    h64 = Rotl64(h64, 11) * prime1;
    ++p;
  }
  h64 ^= h64 >> 33;
  h64 *= prime2;
  h64 ^= h64 >> 29;
  h64 *= prime3;
  h64 ^= h64 >> 32;
  return h64;
}

FunctionCallResult Xxh64Function(const FunctionCallRequest& request) {
  if (request.arguments.empty() || request.arguments.size() > 2) return RefuseFunctionInvalidInput(request, "xxhash64 expects value and optional seed");
  if (IsSqlNull(request.arguments[0].value) || (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
    return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
  }
  auto bytes = RawBytesFromValue(request.arguments[0].value);
  if (bytes.size() > kMaxCryptoInputBytes) return RefuseFunctionInvalidInput(request, "xxhash64 input exceeds scalar budget");
  std::uint64_t seed = 0;
  if (request.arguments.size() == 2 && !ParseUint64(request.arguments[1].value, &seed)) {
    return RefuseFunctionInvalidInput(request, "xxhash64 seed must be uint64");
  }
  return MakeFunctionSuccess(request, {MakeUint64Value("uint64", Xxh64(bytes, seed))});
}

FunctionCallResult ArmorFunction(const FunctionCallRequest& request,bool decode,bool binary_result) {
  namespace c=scratchbird::core::crypto;
  using Kind=scratchbird::engine::sblr::SblrValuePayloadKind;
  const auto error=[&](const char* code,const char* detail){return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::execution_failed,code,detail);};
  const auto invalid=[&]{return error("CRYPTO.ARMOR.INVALID_INPUT","armor requires its exact typed input and a complete valid encoding");};
  if(request.arguments.size()!=1)return invalid();
  const auto& value=request.arguments[0].value;
  if(value.descriptor_id!=(decode?"character":"binary")||value.has_int64_value||value.has_uint64_value||value.has_real64_value||
     !value.uuid_value.is_nil()||!value.uuid_array_value.empty())return invalid();
  if(decode) {
    if(!value.binary_value.empty()||(!value.encoded_value.empty()&&value.encoded_value!=value.text_value))return invalid();
  } else if(!value.text_value.empty()||!value.encoded_value.empty()||!value.charset_name.empty()||!value.collation_name.empty())return invalid();
  if(value.is_null) {
    if(value.payload_kind!=Kind::none||!value.binary_value.empty()||!value.text_value.empty()||!value.encoded_value.empty())return invalid();
  } else if(value.payload_kind!=(decode?Kind::text:Kind::binary))return invalid();
  auto cancelled=[&]{const auto* context=request.context.engine_request_context;
    return context&&context->query_cancellation_requested&&context->query_cancellation_requested();};
  const c::PgpCancellation probe{[](void* raw){return (*static_cast<decltype(cancelled)*>(raw))();},&cancelled};
  const auto cancelled_result=[&]{return error("PROCESS.CANCELLED","armor processing was cancelled");};
  if(cancelled())return cancelled_result();
  c::PgpInput input=decode?c::PgpInput{reinterpret_cast<const std::uint8_t*>(value.text_value.data()),value.text_value.size()}:
    c::PgpInput{value.binary_value.data(),value.binary_value.size()};
  // This legacy family capacity guard is NOT an engine-issued memory grant.
  // The extra encoded extent allows round trips at the raw input bound.
  const auto capacity=decode?c::ArmorEncodedSize(kMaxCryptoInputBytes).bytes:kMaxCryptoInputBytes;
  if(input.size>capacity)return error("RESOURCE.BUDGET_EXCEEDED","armor working capacity exceeded");
  c::PgpSize size{c::PgpCode::ok,0};
  if(!value.is_null)size=decode?c::ArmorDecodedSize(input,probe):c::ArmorEncodedSize(input.size);
  if(size.code==c::PgpCode::cancelled)return cancelled_result();
  if(size.code==c::PgpCode::size_overflow)return error("RESOURCE.BUDGET_EXCEEDED","armor size overflow");
  if(size.code!=c::PgpCode::ok)return invalid();
  auto result=MakeFunctionSuccess(request,{});result.result.scalar_values.resize(1);
  auto& output=result.result.scalar_values[0];output.descriptor_id=binary_result?"binary":"character";
  output.is_null=value.is_null;output.payload_kind=value.is_null?Kind::none:binary_result?Kind::binary:Kind::text;
  if(!binary_result)output.charset_name="UTF-8";
  struct Pending {
    scratchbird::engine::sblr::SblrValue& value;bool accepted=false;
    ~Pending(){if(!accepted){
      if(!value.binary_value.empty())OPENSSL_cleanse(value.binary_value.data(),value.binary_value.size());
      if(!value.text_value.empty())OPENSSL_cleanse(value.text_value.data(),value.text_value.size());
      if(!value.encoded_value.empty())OPENSSL_cleanse(value.encoded_value.data(),value.encoded_value.size());
    }}
  } pending{output};
  if(!value.is_null) {
    // Allocate all fallible carriers before populating secret bytes. No growing
    // base64 scratch or initializer-list copy can release an uncleared prefix.
    c::PgpOutput target;
    if(binary_result){output.binary_value.resize(size.bytes);target={output.binary_value.data(),size.bytes};}
    else {output.text_value.resize(size.bytes);output.encoded_value.resize(size.bytes);
      target={reinterpret_cast<std::uint8_t*>(output.text_value.data()),size.bytes};}
    const auto code=decode?c::DecodeArmor(input,target,probe):c::EncodeArmor(input,target,probe);
    if(code==c::PgpCode::cancelled)return cancelled_result();
    if(code!=c::PgpCode::ok)return invalid();
    if(decode&&!binary_result){
      std::size_t at=0,last_probe=0;std::uint32_t scalar=0;
      while(at<target.size){
        if(at-last_probe>=4096){if(cancelled())return cancelled_result();last_probe=at;}
        if(!scratchbird::core::datatypes::DecodeCanonicalUtf8Scalar(target.data,target.size,&at,&scalar))return invalid();
      }
    }
    if(!binary_result)for(std::size_t at=0;at<size.bytes;) {
      if(cancelled())return cancelled_result();const auto n=std::min(std::size_t{4096},size.bytes-at);
      std::memcpy(output.encoded_value.data()+at,output.text_value.data()+at,n);at+=n;
    }
  }
  if(cancelled())return cancelled_result();pending.accepted=true;return result;
}

std::string Envelope(std::string_view kind, const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& data) {
  return "SBPGP1:" + std::string(kind) + ":" + Base64Encode(key) + ":" + Base64Encode(data);
}

bool OpenEnvelope(const std::string& envelope,
                  std::string_view kind,
                  const std::vector<std::uint8_t>& key,
                  std::vector<std::uint8_t>* data) {
  const std::string prefix = "SBPGP1:" + std::string(kind) + ":";
  if (envelope.rfind(prefix, 0) != 0) return false;
  const auto separator = envelope.find(':', prefix.size());
  if (separator == std::string::npos) return false;
  std::vector<std::uint8_t> encoded_key;
  if (!Base64Decode(envelope.substr(prefix.size(), separator - prefix.size()), &encoded_key)) return false;
  if (encoded_key != key) return false;
  return Base64Decode(envelope.substr(separator + 1), data);
}

FunctionCallResult PgpEnvelopeFunction(const FunctionCallRequest& request, std::string_view kind, bool decrypt) {
  if (request.arguments.size() != 2) {
    return RefuseFunctionInvalidInput(request, decrypt ? "pgp decrypt expects envelope and key"
                                                       : "pgp encrypt expects value and key");
  }
  if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue(decrypt ? "character" : "character")});
  const auto key = RawBytesFromValue(request.arguments[1].value);
  if (decrypt) {
    std::vector<std::uint8_t> data;
    if (!OpenEnvelope(ValueAsText(request.arguments[0].value), kind, key, &data)) {
      return RefuseFunctionInvalidInput(request, "SBSFC-057 pgp envelope key or format mismatch");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(data.begin(), data.end()))});
  }
  const auto data = RawBytesFromValue(request.arguments[0].value);
  return MakeFunctionSuccess(request, {MakeTextValue("character", Envelope(kind, key, data))});
}

}  // namespace

bool IsCryptoHashFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("sb.crypto.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.crypto.", 0) == 0;
}

FunctionCallResult DispatchCryptoHashFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;

  if (IdIs(id, {"argon2"})) {
    return DependencyUnavailable(request, "argon2 provider/header is not available in the local core build; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"bcrypt"})) {
    return DependencyUnavailable(request, "bcrypt password-hash provider is not available in the local core build; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"blake3"})) {
    return DigestFunction(request, [](const auto& bytes) {
      const auto digest = scratchbird::core::hash::ComputeBlake3Digest(bytes);
      return std::optional<std::vector<std::uint8_t>>(
          std::in_place, digest.begin(), digest.end());
    });
  }
  if (IdIs(id, {"crypt", "crypt_password_salt"})) {
    return DependencyUnavailable(request, "system crypt password-hash provider is not pinned for ScratchBird core; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"pgcrypto"})) return RefuseFunctionWithDiagnostic(request,
      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
      "CRYPTO.PACKAGE.NOT_CALLABLE", "pgcrypto is a package capability, not a scalar function");
  if (IdIs(id, {"blake2b"})) return DigestFunction(request, [](const auto& bytes) {
    return DigestBytes(EVP_blake2b512(), bytes, 64);
  });
  if (IdIs(id, {"sha3_256"})) return DigestFunction(request, [](const auto& bytes) {
    return DigestBytes(EVP_sha3_256(), bytes, 32);
  });
  if (IdIs(id, {"sha3_512"})) return DigestFunction(request, [](const auto& bytes) {
    return DigestBytes(EVP_sha3_512(), bytes, 64);
  });
  if (IdIs(id, {"hmac", "hmac_value_key_algo"})) return HmacFunction(request);
  if (IdIs(id, {"gen_random_bytes", "gen_random_bytes_n"})) return RandomBytesFunction(request);
  if (IdIs(id, {"gen_random_uuid"})) return RandomUuidFunction(request);
  if (IdIs(id, {"gen_salt", "gen_salt_algo"})) return GenSaltFunction(request);
  if (IdIs(id, {"scrypt"})) return ScryptFunction(request);
  if (IdIs(id, {"xxhash64", "xxhash64_value_seed"})) return Xxh64Function(request);
  if (IdIs(id, {"armor", "armor_binary"})) return ArmorFunction(request,false,IdIs(id,{"armor_binary"}));
  if (IdIs(id, {"dearmor", "dearmor_text"})) return ArmorFunction(request,true,IdIs(id,{"dearmor"}));
  if (IdIs(id, {"pgp_sym_encrypt"})) return PgpEnvelopeFunction(request, "sym", false);
  if (IdIs(id, {"pgp_sym_decrypt"})) return PgpEnvelopeFunction(request, "sym", true);
  if (IdIs(id, {"pgp_pub_encrypt"})) return PgpEnvelopeFunction(request, "pub", false);
  if (IdIs(id, {"pgp_pub_decrypt"})) return PgpEnvelopeFunction(request, "pub", true);

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING",
                                      "crypto.hash function id is not handled by SBSFC-057");
}

}  // namespace scratchbird::engine::functions
