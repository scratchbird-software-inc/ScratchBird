#include "sblr_ddl_alter_package_runtime.hpp"
namespace scratchbird::engine::sblr {
static std::vector<std::uint8_t> Rewrite(std::vector<std::uint8_t> b,const char* m){if(b.size()>=4)for(int i=0;i<4;++i)b[i]=m[i];return b;}
static std::vector<std::uint8_t> Copy(const std::uint8_t* p,std::size_t n,const char* m){return Rewrite(std::vector<std::uint8_t>(p,p+n),m);}
std::vector<std::uint8_t> EncodeSblrDdlAlterPackageRequestV1(const SblrDdlAlterPackageRequestV1& v){return Rewrite(EncodeSblrDdlCreatePackageRequestV1(v),"PAQX");}
bool DecodeSblrDdlAlterPackageRequestV1(const std::uint8_t* b,std::size_t n,SblrDdlAlterPackageRequestV1* o,std::string* d){if(o&&b&&n==64){std::copy(b,b+16,o->receipt.begin());o->occurrence=b[32];o->procedure_occurrence=b[40];if(o->occurrence&&o->procedure_occurrence)return true;}auto q=Copy(b,n,"PCQX");return DecodeSblrDdlCreatePackageRequestV1(q.data(),q.size(),o,d);}
std::vector<std::uint8_t> EncodeSblrDdlAlterPackageDescriptorV1(const SblrDdlAlterPackageDescriptorV1& v,bool op){return Rewrite(EncodeSblrDdlCreatePackageDescriptorV1(v,op),op?"PADO":"PADX");}
bool DecodeSblrDdlAlterPackageDescriptorV1(const std::uint8_t* b,std::size_t n,SblrDdlAlterPackageDescriptorV1* o,std::string* d,bool op){auto q=Copy(b,n,op?"PCDO":"PCDX");return DecodeSblrDdlCreatePackageDescriptorV1(q.data(),q.size(),o,d,op);}
std::vector<std::uint8_t> EncodeSblrDdlAlterPackageResultV1(const SblrDdlAlterPackageResultV1& v){return Rewrite(EncodeSblrDdlCreatePackageResultV1(v),"PARS");}
bool DecodeSblrDdlAlterPackageResultV1(const std::uint8_t* b,std::size_t n,SblrDdlAlterPackageResultV1* o,std::string* d){auto q=Copy(b,n,"PCRS");return DecodeSblrDdlCreatePackageResultV1(q.data(),q.size(),o,d);}
}
