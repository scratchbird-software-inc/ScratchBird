// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/notification/notification_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <chrono>
namespace api = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1194,n); }
int main() {
  auto identity=Id(1);identity.bytes[10]=0;identity.bytes[15]=255;
  api::EngineRequestContext context;context.database_uuid=Id(2);context.local_transaction_id=7;
  context.database_path=(std::filesystem::temp_directory_path()/
      ("sb_notification_native_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
  api::EventFields fields={{"channel_name","channel"},{"payload_descriptor_uuid",Id(3)},
      {"queue_policy_uuid",api::EngineUuid{}},{"state","active"},{"visibility","normal"},{"redaction_policy","none"}};
  const auto encoded=api::MakeEventLine(context,"CHANNEL",7,identity,fields);
  Check(!encoded.empty());
  Check(!api::AppendEventRecord(context,"CHANNEL",identity,fields).error);
  std::vector<api::EventLogRecord> records;
  Check(api::ReadEventRecords(context,&records) && records.size()==1);
  Check(records[0].object_uuid==identity && api::FieldIdentity(records[0].fields,"payload_descriptor_uuid")==Id(3));
  const auto write=[&](const std::string& bytes) {
    std::ofstream out(api::NotificationEventPath(context),std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),bytes.size());Check(static_cast<bool>(out));
  };
  const auto preserved=records[0].object_uuid;
  auto wrong=context;wrong.database_uuid=Id(99);Check(!api::ReadEventRecords(wrong,&records));
  Check(records[0].object_uuid==preserved);
  write(encoded.substr(0,encoded.size()-1));Check(!api::ReadEventRecords(context,&records));
  write("SBEVN1\tCHANNEL\t7\tlegacy\n");Check(!api::ReadEventRecords(context,&records));
  Check(api::AppendEventRecord(context,"CHANNEL",identity,fields).error);
  write(encoded);
  fields[1].second="019f0000-0000-7000-8000-000000000001";
  Check(api::MakeEventLine(context,"CHANNEL",7,identity,fields).empty());
  fields[1].second=Id(3);fields.push_back({"unknown","value"});
  Check(api::MakeEventLine(context,"CHANNEL",7,identity,fields).empty());
  api::EngineApiResult result;
  api::EventChannelShape channel;channel.channel_uuid=identity;channel.payload_descriptor_uuid=Id(3);
  api::AddChannelRow(&result,channel);
  const auto& value=result.result_shape.rows.front().fields.front().second;
  Check(value.encoded_value.empty() && value.binary_value==std::vector<std::uint8_t>(identity.bytes.begin(),identity.bytes.end()));
  // Tuple keys distinguish roles and preserve all bytes without delimiters.
  api::EventState state;
  state.records={{1,"ACK",0,Id(10),{{"session_uuid",identity},{"subscription_uuid",Id(4)},{"event_uuid",Id(5)},{"state","acknowledged"}}}};
  const auto keys=api::VisibleAcknowledgementKeys(state,context);
  Check(keys.contains({identity,Id(4),Id(5)}) && !keys.contains({Id(4),identity,Id(5)}));
  std::filesystem::remove(api::NotificationEventPath(context));
}
