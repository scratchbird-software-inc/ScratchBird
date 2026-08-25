#include "sblr_ddl_alter_sequence_runtime.hpp"
namespace scratchbird::engine::sblr {
static std::vector<std::uint8_t> R(std::vector<std::uint8_t>b,const char*m){if(b.size()>=4)for(int i=0;i<4;++i)b[i]=m[i];return b;} static std::vector<std::uint8_t>C(const std::uint8_t*p,std::size_t n,const char*m){return R(std::vector<std::uint8_t>(p,p+n),m);}
std::vector<std::uint8_t> EncodeSblrDdlAlterSequenceRequestV1(const SblrDdlAlterSequenceRequestV1&v){return R(EncodeSblrDdlCreatePackageRequestV1(v),"SAQX");}
bool DecodeSblrDdlAlterSequenceRequestV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterSequenceRequestV1*o,std::string*d){if(o&&b&&n==64){std::copy(b,b+16,o->receipt.begin());o->occurrence=b[32];o->procedure_occurrence=b[40];if(o->occurrence&&o->procedure_occurrence)return true;}auto q=C(b,n,"PCQX");return DecodeSblrDdlCreatePackageRequestV1(q.data(),q.size(),o,d);}
std::vector<std::uint8_t> EncodeSblrDdlAlterSequenceDescriptorV1(const SblrDdlAlterSequenceDescriptorV1&v,bool op){return R(EncodeSblrDdlCreatePackageDescriptorV1(v,op),op?"SADO":"SADX");}
bool DecodeSblrDdlAlterSequenceDescriptorV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterSequenceDescriptorV1*o,std::string*d,bool op){auto q=C(b,n,op?"PGDO":"PGDX");return DecodeSblrDdlCreatePackageDescriptorV1(q.data(),q.size(),o,d,op);}
std::vector<std::uint8_t> EncodeSblrDdlAlterSequenceResultV1(const SblrDdlAlterSequenceResultV1&v){return R(EncodeSblrDdlCreatePackageResultV1(v),"SARS");}
bool DecodeSblrDdlAlterSequenceResultV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterSequenceResultV1*o,std::string*d){auto q=C(b,n,"PGRS");return DecodeSblrDdlCreatePackageResultV1(q.data(),q.size(),o,d);}
}
