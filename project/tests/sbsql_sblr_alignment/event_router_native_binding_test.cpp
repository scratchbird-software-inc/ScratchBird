// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/event_notification_router.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace server = scratchbird::server;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1198,n); }
int main() {
  server::ParserEventNotificationRouter router;
  auto channel = Id(1); channel.bytes[10]=0; channel.bytes[15]=255;
  for (unsigned n=2;n<4;++n) {
    server::ParserEventSubscription sub;
    sub.subscription_uuid=Id(n);sub.parser_channel_uuid=Id(n+10);
    sub.session_uuid=Id(n+20);sub.principal_uuid=Id(n+30);
    sub.event_channel_uuid=channel;
    Check(router.RegisterSubscription(sub).ok);
  }
  Check(router.ActiveSubscriptionCount()==2);
  Check(router.EnqueueCommittedEvent(channel,Id(50),Id(51),"payload","redacted").affected_count==2);
  Check(router.EnqueueCommittedEvent(channel,Id(50),Id(51),"payload","redacted").affected_count==0);
  auto first=router.DrainParserChannel(Id(12),1),second=router.DrainParserChannel(Id(13),1);
  Check(first.size()==1 && second.size()==1);
  Check(first[0].event_channel_uuid==channel && first[0].event_uuid==Id(50));
  Check(first[0].payload_descriptor_uuid==Id(51));
  Check(first[0].redaction_state=="redacted" && second[0].redaction_state=="redacted");
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(first[0].message_vector_uuid));
  Check(first[0].message_vector_uuid!=second[0].message_vector_uuid);
  Check(router.FindSubscription(Id(12),Id(2))!=nullptr);
  Check(router.FindSubscription(Id(13),Id(2))==nullptr);
  Check(router.UnregisterSession(Id(12),Id(22)).affected_count==1);
  Check(router.ActiveSubscriptionCount()==1);
  Check(router.EnqueueCommittedEvent(channel,Id(52),Id(51),"next").affected_count==1);
  Check(router.QueuedEventCount(Id(12))==0 && router.QueuedEventCount(Id(13))==1);
  Check(router.UnregisterSubscription(Id(13),Id(3),channel).affected_count==1);
}
