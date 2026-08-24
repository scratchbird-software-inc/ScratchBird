#include "sblr_ddl_drop_synonym_runtime.hpp"
namespace scratchbird::engine::sblr {
static std::vector<uint8_t> Magic(std::vector<uint8_t> b,const char* m){if(b.size()>=4)for(int i=0;i<4;++i)b[i]=m[i];return b;}
static std::vector<uint8_t> Copy(const uint8_t* p,size_t n,const char* m){return Magic(std::vector<uint8_t>(p,p+n),m);}
std::vector<uint8_t> EncodeSblrDdlDropSynonymRequestV1(const SblrDdlDropSynonymRequestV1& v){return Magic(EncodeSblrDdlDropPackageRequestV1(v),"DSYQ");}
bool DecodeSblrDdlDropSynonymRequestV1(const uint8_t* p,size_t n,SblrDdlDropSynonymRequestV1* o,std::string* d){if(n==64&&p&&p[0]=='D'&&p[1]=='S'&&p[2]=='Y'&&p[3]=='Q'){auto b=Copy(p,n,"PCQX");return DecodeSblrDdlCreatePackageRequestV1(b.data(),b.size(),o,d);}return false;}
std::vector<uint8_t> EncodeSblrDdlDropSynonymDescriptorV1(const SblrDdlDropSynonymDescriptorV1& v,bool q){return Magic(EncodeSblrDdlDropPackageDescriptorV1(v,q),q?"DSYO":"DSYD");}
bool DecodeSblrDdlDropSynonymDescriptorV1(const uint8_t* p,size_t n,SblrDdlDropSynonymDescriptorV1* o,std::string* d,bool q){auto b=Copy(p,n,q?("DSYO"):("DSYD"));if(n<4)return false;for(int i=0;i<4;++i)if(p[i]!=(q?"DSYO":"DSYD")[i])return false;for(int i=0;i<4;++i)b[i]=(q?"PCDO":"PCDX")[i];return DecodeSblrDdlCreatePackageDescriptorV1(b.data(),b.size(),o,d,q);}
std::vector<uint8_t> EncodeSblrDdlDropSynonymResultV1(const SblrDdlDropSynonymResultV1& v){return Magic(EncodeSblrDdlDropPackageResultV1(v),"DYRS");}
bool DecodeSblrDdlDropSynonymResultV1(const uint8_t* p,size_t n,SblrDdlDropSynonymResultV1* o,std::string* d){if(n<4||p[0]!='D'||p[1]!='Y'||p[2]!='R'||p[3]!='S')return false;auto b=Copy(p,n,"PCRS");return DecodeSblrDdlCreatePackageResultV1(b.data(),b.size(),o,d);}
}
