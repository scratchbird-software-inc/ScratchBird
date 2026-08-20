#include "engine/sblr/sblr_autonomous_frame_runtime.hpp"
#include <string>
namespace s = scratchbird::engine::sblr;
template <class A> void Seed(A& a, unsigned n) { for (auto& v : a) v = n++; }
int main() {
  s::SblrAutonomousFrameRequestV1 q;
  Seed(q.receipt,1); Seed(q.parent_transaction,20); Seed(q.parent_frame,40);
  Seed(q.database,60); Seed(q.attachment,80); Seed(q.session,100); Seed(q.body,120);
  q.occurrence=1; q.intent=2; q.depth=1; q.effect_count=1;
  Seed(q.effect_sha,10); Seed(q.body_sha,50);
  auto b=s::EncodeSblrAutonomousFrameRequestV1(q); s::SblrAutonomousFrameRequestV1 qd; std::string e;
  if(b.size()!=224||!s::DecodeSblrAutonomousFrameRequestV1(b.data(),b.size(),&qd,&e)) return 1;
  auto bad=b; bad[220]=1; if(s::DecodeSblrAutonomousFrameRequestV1(bad.data(),bad.size(),&qd,&e)) return 2;
  s::SblrAutonomousFrameDescriptorV1 d;
  d.receipt=q.receipt; Seed(d.frame,5); d.frame_generation=1; Seed(d.child_transaction,25);
  d.child_transaction_number=2; d.parent_transaction=q.parent_transaction; d.parent_frame=q.parent_frame;
  d.database=q.database; d.attachment=q.attachment; d.session=q.session; Seed(d.principal,45);
  Seed(d.security,65); Seed(d.policy,85); d.catalog_generation=3; d.capability_generation=4;
  d.body=q.body; d.intent=2; d.depth=1; d.effect_count=1; d.effect_sha=q.effect_sha;
  auto db=s::EncodeSblrAutonomousFrameDescriptorV1(d,true); s::SblrAutonomousFrameDescriptorV1 dd;
  if(db.size()!=324||!s::DecodeSblrAutonomousFrameDescriptorV1(db.data(),db.size(),&dd,&e,true)) return 3;
  bad=db; bad[292]^=1; if(s::DecodeSblrAutonomousFrameDescriptorV1(bad.data(),bad.size(),&dd,&e,true)) return 4;
  auto afcd=s::EncodeSblrAutonomousFrameDescriptorV1(d,false);
  if(afcd.size()!=324||!s::DecodeSblrAutonomousFrameDescriptorV1(afcd.data(),afcd.size(),&dd,&e,false))return 7;
  auto afdo=s::EncodeSblrAutonomousFrameDescriptorV1(dd,true);
  if(afdo.size()!=324||!std::equal(afcd.begin()+16,afcd.end(),afdo.begin()+16))return 8;
  s::SblrAutonomousFrameResultV1 r;
  r.frame=d.frame; r.frame_generation=1; r.child_transaction=d.child_transaction;
  r.child_transaction_number=2; r.parent_transaction=d.parent_transaction; r.final_state=1;
  r.intent=2; r.depth=1; r.commit_sequence=5; Seed(r.finality_sha,110); Seed(r.recovery_token,140);
  r.recovery_generation=6; r.availability_generation=7;
  auto rb=s::EncodeSblrAutonomousFrameResultV1(r); s::SblrAutonomousFrameResultV1 rd;
  if(rb.size()!=200||!s::DecodeSblrAutonomousFrameResultV1(rb.data(),rb.size(),&rd,&e)) return 5;
  bad=rb; bad[168]^=1; if(s::DecodeSblrAutonomousFrameResultV1(bad.data(),bad.size(),&rd,&e)) return 6;
  return 0;
}
