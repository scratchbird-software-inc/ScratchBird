#include "sblr_database_serialize_logical_snapshot_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr {
namespace {
template<class A> bool nz(const A& a) { for (auto x : a) if (x) return true; return false; }
void put(std::vector<std::uint8_t>& o, std::uint64_t v, int n) { for (int i=0;i<n;++i) o.push_back(static_cast<std::uint8_t>(v>>(8*i))); }
std::uint64_t get(const std::uint8_t* p, int n) { std::uint64_t v=0; for (int i=0;i<n;++i) v|=std::uint64_t(p[i])<<(8*i); return v; }
std::vector<std::uint8_t> head(const char* m, std::size_t n) { std::vector<std::uint8_t> o(m,m+4); put(o,1,2); put(o,16,2); put(o,n,4); put(o,0,4); return o; }
bool valid(const std::uint8_t* b,std::size_t n,const char* m,std::size_t z) { return b&&n==z&&!std::memcmp(b,m,4)&&get(b+4,2)==1&&get(b+6,2)==16&&get(b+8,4)==z&&get(b+12,4)==0; }
std::array<std::uint8_t,32> digest(const char* domain,const std::uint8_t* b,std::size_t n) { std::vector<std::uint8_t> material(domain,domain+std::strlen(domain)); material.insert(material.end(),b,b+n); return scratchbird::core::hash::ComputeSha256Digest(material).digest; }
bool eq(const std::uint8_t* a,const std::array<std::uint8_t,32>& b) { return std::equal(b.begin(),b.end(),a); }
}

std::vector<std::uint8_t> EncodeSblrDatabaseSerializeLogicalSnapshotRequestV1(const SblrDatabaseSerializeLogicalSnapshotRequestV1& v) { if(!nz(v.receipt)||!v.occurrence||!v.snapshot_occurrence)return{}; auto o=head("LSRQ",64); o.insert(o.end(),v.receipt.begin(),v.receipt.end()); put(o,v.occurrence,8); put(o,v.snapshot_occurrence,4); o.insert(o.end(),20,0); return o; }
bool DecodeSblrDatabaseSerializeLogicalSnapshotRequestV1(const std::uint8_t* b,std::size_t n,SblrDatabaseSerializeLogicalSnapshotRequestV1* o,std::string* d) { if(!o||!valid(b,n,"LSRQ",64)||std::any_of(b+44,b+64,[](auto x){return x;})){if(d)*d="LSRQ invalid";return false;} std::copy_n(b+16,16,o->receipt.begin()); o->occurrence=get(b+32,8); o->snapshot_occurrence=get(b+40,4); return nz(o->receipt)&&o->occurrence&&o->snapshot_occurrence; }

std::vector<std::uint8_t> EncodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(const SblrDatabaseSerializeLogicalSnapshotDescriptorV1& v,bool operand) { if(!nz(v.body)||!v.availability)return{}; auto o=head(operand?"LSEO":"LSDD",488); o.insert(o.end(),v.body.begin(),v.body.end()); auto e=digest("ScratchBird.SblrDatabaseSerializeLogicalSnapshotDescriptor.V1",o.data()+16,400); if(nz(v.evidence)&&e!=v.evidence)return{}; o.insert(o.end(),e.begin(),e.end()); put(o,v.availability,8); o.insert(o.end(),32,0); return o; }
bool DecodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(const std::uint8_t* b,std::size_t n,SblrDatabaseSerializeLogicalSnapshotDescriptorV1* o,std::string* d,bool operand) { if(!o||!valid(b,n,operand?"LSEO":"LSDD",488)||std::any_of(b+456,b+488,[](auto x){return x;})){if(d)*d="LSEO/LSDD invalid";return false;} std::copy_n(b+16,400,o->body.begin()); std::copy_n(b+416,32,o->evidence.begin()); o->availability=get(b+448,8); auto e=digest("ScratchBird.SblrDatabaseSerializeLogicalSnapshotDescriptor.V1",b+16,400); return o->availability&&nz(o->evidence)&&eq(b+416,e); }

std::vector<std::uint8_t> EncodeSblrDatabaseSerializeLogicalSnapshotResultV1(const SblrDatabaseSerializeLogicalSnapshotResultV1& v) { if(!nz(v.body)||!v.availability||!nz(v.publication_barrier))return{}; auto o=head("LSRS",512); o.insert(o.end(),v.body.begin(),v.body.end()); auto e=digest("ScratchBird.SblrDatabaseSerializeLogicalSnapshotResult.V1",o.data()+16,440); if(nz(v.evidence)&&e!=v.evidence)return{}; o.insert(o.end(),e.begin(),e.end()); put(o,v.availability,8); o.insert(o.end(),v.publication_barrier.begin(),v.publication_barrier.end()); return o; }
bool DecodeSblrDatabaseSerializeLogicalSnapshotResultV1(const std::uint8_t* b,std::size_t n,SblrDatabaseSerializeLogicalSnapshotResultV1* o,std::string* d) { if(!o||!valid(b,n,"LSRS",512)){if(d)*d="LSRS invalid";return false;} std::copy_n(b+16,440,o->body.begin()); std::copy_n(b+456,32,o->evidence.begin()); o->availability=get(b+488,8); std::copy_n(b+496,16,o->publication_barrier.begin()); auto e=digest("ScratchBird.SblrDatabaseSerializeLogicalSnapshotResult.V1",b+16,440); return o->availability&&nz(o->evidence)&&nz(o->publication_barrier)&&eq(b+456,e); }
}
