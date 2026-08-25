#include "sblr_ddl_alter_publication_coordinator.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <map>
#include <mutex>
namespace scratchbird::engine::internal_api { namespace {
std::mutex mutex; using Descriptor=scratchbird::engine::sblr::SblrDdlAlterPublicationDescriptorV1; std::map<std::string,Descriptor> live,used;
std::string key(const Descriptor&d){return{reinterpret_cast<const char*>(d.body.data()),d.body.size()};}
bool tag(const EngineRequestContext&c,const char*t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}
EngineApiDiagnostic diag(std::string c,std::string m){return MakeEngineApiDiagnostic(std::move(c),std::move(m),{});}
void u64(std::array<std::uint8_t,312>&b,std::size_t o,std::uint64_t v){for(std::size_t i=0;i<8;++i)b[o+i]=static_cast<std::uint8_t>(v>>(8*i));}
}
SblrDdlAlterPublicationCoordinationResult CompileSblrDdlAlterPublicationDescriptor(const EngineRequestContext&c,const std::string&publication,std::uint64_t expected,std::uint8_t change,std::uint64_t availability){std::lock_guard l(mutex);SblrDdlAlterPublicationCoordinationResult o;if(!tag(c,"private_ddl_alter_publication_binder")||!c.statement_metadata_snapshot_engine_owned||publication!=c.statement_uuid.canonical||!expected||change>4||!availability){o.diagnostic=diag("SBLR.OPERAND_INVALID","sblr.ddl_alter_publication.coordination_invalid");return o;}o.descriptor.body[0]=1;o.descriptor.body[16]=1;u64(o.descriptor.body,48,expected);o.descriptor.body[144]=change;u64(o.descriptor.body,272,availability);auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterPublicationDescriptorV1(o.descriptor);if(bytes.empty()||!scratchbird::engine::sblr::DecodeSblrDdlAlterPublicationDescriptorV1(bytes.data(),bytes.size(),&o.descriptor,nullptr)){o.diagnostic=diag("SBLR.OPERAND_INVALID","sblr.ddl_alter_publication.descriptor_invalid");return o;}live[key(o.descriptor)]=o.descriptor;o.ok=true;o.diagnostic=diag("OK","ok");return o;}
SblrDdlAlterPublicationCoordinationResult ConsumeSblrDdlAlterPublicationDescriptor(const EngineRequestContext&c,const Descriptor&d){std::lock_guard l(mutex);SblrDdlAlterPublicationCoordinationResult o;auto k=key(d);if(!tag(c,"private_ddl_alter_publication")){o.diagnostic=diag("SECURITY.ACCESS_DENIED","sblr.ddl_alter_publication.hidden");return o;}auto i=live.find(k);if(i==live.end()){o.diagnostic=used.count(k)?diag("MGA.TRANSACTION.STALE","sblr.ddl_alter_publication.stale"):diag("SECURITY.ACCESS_DENIED","sblr.ddl_alter_publication.hidden");return o;}if(c.query_cancellation_requested&&c.query_cancellation_requested()){o.diagnostic=diag("PROCESS.CANCELLED","sblr.ddl_alter_publication.cancelled");return o;}used[k]=d;live.erase(i);o.ok=true;o.descriptor=d;o.diagnostic=diag("OK","ok");return o;}
}
