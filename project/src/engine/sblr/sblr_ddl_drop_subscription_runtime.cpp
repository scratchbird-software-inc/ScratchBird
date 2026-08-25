#include "engine/sblr/sblr_ddl_drop_subscription_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr {
namespace {
template <class A> bool nonzero(const A& a) { return std::any_of(a.begin(), a.end(), [](auto b) { return b != 0; }); }
void put(std::vector<std::uint8_t>& o, std::uint64_t v, std::size_t n) { for (std::size_t i = 0; i < n; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
std::uint64_t get(const std::uint8_t* b, std::size_t n) { std::uint64_t v = 0; for (std::size_t i = 0; i < n; ++i) v |= std::uint64_t(b[i]) << (8 * i); return v; }
std::array<std::uint8_t,32> digest(const char* domain, const std::vector<std::uint8_t>& body) { std::vector<std::uint8_t> x(domain, domain + std::strlen(domain)); x.insert(x.end(), body.begin(), body.end()); return scratchbird::core::hash::ComputeSha256Digest(x).digest; }
std::vector<std::uint8_t> drop_subscription_encode(const char* magic, const std::array<std::uint8_t,376>& body, const char* domain) { std::vector<std::uint8_t> o{static_cast<std::uint8_t>(magic[0]),static_cast<std::uint8_t>(magic[1]),static_cast<std::uint8_t>(magic[2]),static_cast<std::uint8_t>(magic[3]),1,0,0,0}; o.insert(o.end(), body.begin(), body.end()); std::fill(o.begin()+352,o.begin()+384,0); auto h=digest(domain,o); std::copy(h.begin(),h.end(),o.begin()+352); return o; }
bool drop_subscription_decode(const std::uint8_t* b, std::size_t n, const char* magic, std::array<std::uint8_t,376>* out, const char* domain, std::string* error) { if (!out || n != 384 || std::memcmp(b,magic,4) != 0 || b[4] != 1) { if(error)*error="descriptor invalid"; return false; } std::copy_n(b+8,376,out->begin()); auto encoded=drop_subscription_encode(magic,*out,domain); if (!std::equal(encoded.begin(),encoded.end(),b)) { if(error)*error="descriptor hash invalid"; return false; } return true; }
}

std::vector<std::uint8_t> EncodeSblrDdlDropSubscriptionRequestV1(const SblrDdlDropSubscriptionRequestV1& v) { if (!nonzero(v.operation)||!nonzero(v.receipt)||v.descriptor_length!=384) return {}; std::vector<std::uint8_t> o{'S','B','D','Q'}; put(o,1,2);put(o,8,2);put(o,7561,4);put(o,548,4);o.insert(o.end(),v.operation.begin(),v.operation.end());o.insert(o.end(),v.receipt.begin(),v.receipt.end());put(o,384,4);o.insert(o.end(),12,0);return o; }
bool DecodeSblrDdlDropSubscriptionRequestV1(const std::uint8_t* b,std::size_t n,SblrDdlDropSubscriptionRequestV1* out,std::string* error) { if(!out||n!=64||std::memcmp(b,"SBDQ",4)!=0||get(b+4,2)!=1||get(b+6,2)!=8||get(b+8,4)!=7561||get(b+12,4)!=548) { if(error)*error="SBDQ invalid";return false; } SblrDdlDropSubscriptionRequestV1 v;std::copy_n(b+16,16,v.operation.begin());std::copy_n(b+32,16,v.receipt.begin());v.descriptor_length=get(b+48,4);if(EncodeSblrDdlDropSubscriptionRequestV1(v).empty())return false;*out=v;return true; }
std::vector<std::uint8_t> EncodeSblrDdlDropSubscriptionDescriptorV1(const SblrDdlDropSubscriptionDescriptorV1& v) { return nonzero(v.body)?drop_subscription_encode("SBDD",v.body,"SBOD|SBDD|v1"):std::vector<std::uint8_t>{}; }
bool DecodeSblrDdlDropSubscriptionDescriptorV1(const std::uint8_t* b,std::size_t n,SblrDdlDropSubscriptionDescriptorV1* out,std::string* error) { return out&&drop_subscription_decode(b,n,"SBDD",&out->body,"SBOD|SBDD|v1",error); }
std::vector<std::uint8_t> EncodeSblrDdlDropSubscriptionResultV1(const SblrDdlDropSubscriptionResultV1& v) { return nonzero(v.body)?drop_subscription_encode("SLRR",v.body,"ScratchBird.SblrDdlDropSubscription.Result.V1"):std::vector<std::uint8_t>{}; }
bool DecodeSblrDdlDropSubscriptionResultV1(const std::uint8_t* b,std::size_t n,SblrDdlDropSubscriptionResultV1* out,std::string* error) { return out&&drop_subscription_decode(b,n,"SLRR",&out->body,"ScratchBird.SblrDdlDropSubscription.Result.V1",error); }
}
