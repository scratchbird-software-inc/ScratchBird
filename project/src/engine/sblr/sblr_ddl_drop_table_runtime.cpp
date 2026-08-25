#include "sblr_ddl_drop_table_runtime.hpp"
namespace scratchbird::engine::sblr {
static std::vector<std::uint8_t> Magic(std::vector<std::uint8_t> b,const char* m){if(b.size()>=4)for(int i=0;i<4;++i)b[i]=static_cast<std::uint8_t>(m[i]);return b;}
std::vector<std::uint8_t> EncodeSblrDdlDropTableRequestV1(const SblrDdlDropTableRequestV1& v){return Magic(EncodeSblrDdlCreateTableRequestV1(v),"DTBX");}
bool DecodeSblrDdlDropTableRequestV1(const std::uint8_t* p,std::size_t n,SblrDdlDropTableRequestV1* out,std::string* d){if(!p||n<4||std::string(reinterpret_cast<const char*>(p),4)!="DTBX"){if(d)*d="DTBX invalid";return false;} std::vector<std::uint8_t> q(p,p+n); q[0]='C';q[1]='T';q[2]='Q';q[3]='X'; return DecodeSblrDdlCreateTableRequestV1(q.data(),q.size(),out,d);}
std::vector<std::uint8_t> EncodeSblrDdlDropTableDescriptorV1(const SblrDdlDropTableDescriptorV1& v,bool operand){return Magic(EncodeSblrDdlCreateTableDescriptorV1(v,operand),operand?"TBDO":"TBDX");}
bool DecodeSblrDdlDropTableDescriptorV1(const std::uint8_t* p,std::size_t n,SblrDdlDropTableDescriptorV1* out,std::string* d,bool operand){if(!p||n<4||std::string(reinterpret_cast<const char*>(p),4)!=(operand?"TBDO":"TBDX")){if(d)*d="TBD header invalid";return false;} std::vector<std::uint8_t> q(p,p+n); q[0]='C';q[1]='T';q[2]=operand?'D':'D';q[3]=operand?'O':'X'; return DecodeSblrDdlCreateTableDescriptorV1(q.data(),q.size(),out,d,operand);}
std::vector<std::uint8_t> EncodeSblrDdlDropTableResultV1(const SblrDdlDropTableResultV1& v){return Magic(EncodeSblrDdlCreateTableResultV1(v),"DTRS");}
bool DecodeSblrDdlDropTableResultV1(const std::uint8_t* p,std::size_t n,SblrDdlDropTableResultV1* out,std::string* d){if(!p||n<4||std::string(reinterpret_cast<const char*>(p),4)!="DTRS"){if(d)*d="DTRS invalid";return false;} std::vector<std::uint8_t> q(p,p+n); q[0]='C';q[1]='T';q[2]='R';q[3]='S'; return DecodeSblrDdlCreateTableResultV1(q.data(),q.size(),out,d);}
}
