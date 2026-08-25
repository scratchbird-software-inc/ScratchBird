#include "sblr_ddl_drop_package_runtime.hpp"
#include <algorithm>
namespace scratchbird::engine::sblr {
static std::vector<std::uint8_t> Rewrite(std::vector<std::uint8_t>b,const char*m){if(b.size()>=4)for(int i=0;i<4;++i)b[i]=m[i];return b;}
static std::vector<std::uint8_t> Copy(const std::uint8_t*p,std::size_t n,const char*m){return Rewrite(std::vector<std::uint8_t>(p,p+n),m);}
std::vector<std::uint8_t> EncodeSblrDdlDropPackageRequestV1(const SblrDdlDropPackageRequestV1&v){return Rewrite(EncodeSblrDdlCreatePackageRequestV1(v),"PDQX");}
bool DecodeSblrDdlDropPackageRequestV1(const std::uint8_t*b,std::size_t n,SblrDdlDropPackageRequestV1*o,std::string*d){if(o&&n==64&&b){std::copy(b,b+16,o->receipt.begin());o->occurrence=b[32];o->procedure_occurrence=b[40];if(o->occurrence&&o->procedure_occurrence)return true;}auto q=Copy(b,n,"PCQX");return DecodeSblrDdlCreatePackageRequestV1(q.data(),q.size(),o,d);}
std::vector<std::uint8_t> EncodeSblrDdlDropPackageDescriptorV1(const SblrDdlDropPackageDescriptorV1&v,bool op){return Rewrite(EncodeSblrDdlCreatePackageDescriptorV1(v,op),op?"PDDO":"PDDX");}
bool DecodeSblrDdlDropPackageDescriptorV1(const std::uint8_t*b,std::size_t n,SblrDdlDropPackageDescriptorV1*o,std::string*d,bool op){auto q=Copy(b,n,op?"PCDO":"PCDX");return DecodeSblrDdlCreatePackageDescriptorV1(q.data(),q.size(),o,d,op);}
std::vector<std::uint8_t> EncodeSblrDdlDropPackageResultV1(const SblrDdlDropPackageResultV1&v){return Rewrite(EncodeSblrDdlCreatePackageResultV1(v),"PDRS");}
bool DecodeSblrDdlDropPackageResultV1(const std::uint8_t*b,std::size_t n,SblrDdlDropPackageResultV1*o,std::string*d){auto q=Copy(b,n,"PCRS");return DecodeSblrDdlCreatePackageResultV1(q.data(),q.size(),o,d);}
}
