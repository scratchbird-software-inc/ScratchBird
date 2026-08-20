#include "engine/sblr/sblr_security_drop_privilege_template_runtime.hpp"
#include <array>
#include <cstdint>
#include <string>
int main() {
  using namespace scratchbird::engine::sblr;
  SblrSecurityDropPrivilegeTemplateRequestV1 q;
  q.receipt.fill(1); q.occurrence=7; q.template_occurrence=3;
  auto qb=EncodeSblrSecurityDropPrivilegeTemplateRequestV1(q);
  SblrSecurityDropPrivilegeTemplateRequestV1 q2; std::string d;
  if(qb.size()!=64||!DecodeSblrSecurityDropPrivilegeTemplateRequestV1(qb.data(),qb.size(),&q2,&d)) return 1;
  qb[12]=1; if(DecodeSblrSecurityDropPrivilegeTemplateRequestV1(qb.data(),qb.size(),&q2,&d)) return 2;
  SblrSecurityDropPrivilegeTemplateDescriptorV1 x; x.body[0]=1; x.body[17]=2; x.availability=1;
  auto xb=EncodeSblrSecurityDropPrivilegeTemplateDescriptorV1(x,true);
  SblrSecurityDropPrivilegeTemplateDescriptorV1 x2;
  if(xb.size()!=488||!DecodeSblrSecurityDropPrivilegeTemplateDescriptorV1(xb.data(),xb.size(),&x2,&d,true)) return 3;
  xb[416]^=1; if(DecodeSblrSecurityDropPrivilegeTemplateDescriptorV1(xb.data(),xb.size(),&x2,&d,true)) return 4;
  SblrSecurityDropPrivilegeTemplateResultV1 r; r.body[0]=1; r.availability=1; r.publication_barrier.fill(2);
  auto rb=EncodeSblrSecurityDropPrivilegeTemplateResultV1(r);
  SblrSecurityDropPrivilegeTemplateResultV1 r2;
  if(rb.size()!=320||!DecodeSblrSecurityDropPrivilegeTemplateResultV1(rb.data(),rb.size(),&r2,&d)) return 5;
  rb[256]^=1; if(DecodeSblrSecurityDropPrivilegeTemplateResultV1(rb.data(),rb.size(),&r2,&d)) return 6;
  return 0;
}
