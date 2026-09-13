// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/wire/system_variable_registry.hpp"
#include "../../src/engine/sblr/sblr_context_variables.hpp"
#include "../common/single_tu_allocation_fault.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>

namespace {
using namespace scratchbird::wire;
std::size_t checks=0;
void Check(bool condition,const char* message) {
  ++checks;
  if(!condition) {std::fprintf(stderr,"FAIL check=%zu %s\n",checks,message);std::exit(1);}
}
struct Expected {
  std::string_view name, uuid, id, type, source, scope, volatility, right, unavailable;
};
// Independent specification-derived expectations, not a registry iteration used
// as its own oracle. Text UUIDs here are fixture input, never engine identity.
constexpr Expected expected[]{
  {"current_user", "019de5fc-2400-7c0c-9340-e1e7ebf7aaae", "sb.variable.current_user", "text", "security", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"session_user", "019de5fc-2400-729f-8d65-1b693738099c", "sb.variable.session_user", "text", "security", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_role", "019de5fc-2400-70a0-8fc3-43baf020445d", "sb.variable.current_role", "text", "security", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_database", "019de5fc-2400-7b72-83bf-f0164220195c", "sb.variable.current_database", "text", "catalog", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_schema", "019de5fc-2400-71c7-9155-0ae16017357a", "sb.variable.current_schema", "text", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_catalog", "019de5fc-2400-7aba-9bdf-9bba323f4009", "sb.variable.current_catalog", "text", "catalog", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_cluster", "019de5fc-2400-70c7-ab56-f065d70fe231", "sb.variable.current_cluster", "text", "cluster", "session", "STABLE", "public", "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {"current_cluster_uuid", "019de5fc-2400-71fc-905c-e603345e468c", "sb.variable.current_cluster_uuid", "uuid", "cluster", "session", "STABLE", "public", "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {"cluster_epoch", "019de5fc-2400-71d5-9e60-6f2872d8cbf0", "sb.variable.cluster_epoch", "bigint", "cluster", "cluster_authority", "VOLATILE", "CATALOG_READ", "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {"cluster_member_id", "019de5fc-2400-7aaf-9c80-de2b56c48d9d", "sb.variable.cluster_member_id", "text", "cluster", "cluster_authority", "STABLE", "public", "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {"current_session_uuid", "019de5fc-2400-7e97-afa6-3edffb1b8c6d", "sb.variable.current_session_uuid", "uuid", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_session_id", "019de5fc-2400-79f0-afac-8f71720ed8c9", "sb.variable.current_session_id", "bigint", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_request_uuid", "019de5fc-2400-767e-b8cf-ea1ccb6fc838", "sb.variable.current_request_uuid", "uuid", "session", "statement", "STABLE", "public", "SBSQL.NO_REQUEST"},
  {"current_transaction_id", "019de5fc-2400-76b0-9bb6-306675701893", "sb.variable.current_transaction_id", "bigint", "txn", "transaction", "STABLE", "public", "SBSQL.NO_TRANSACTION"},
  {"current_statement_uuid", "019de5fc-2400-70d2-a176-5f0253e20b5a", "sb.variable.current_statement_uuid", "uuid", "session", "statement", "STABLE", "public", "SBSQL.NO_STATEMENT"},
  {"current_isolation_level", "019de5fc-2400-783b-89ac-63b3d376a662", "sb.variable.current_isolation_level", "text", "txn", "transaction", "STABLE", "public", "SBSQL.NO_TRANSACTION"},
  {"current_timezone", "019de5fc-2400-7086-aab7-1ff720591811", "sb.variable.current_timezone", "text", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_locale", "019de5fc-2400-7572-a09a-c01a6f0c0654", "sb.variable.current_locale", "text", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_engine_version", "019de5fc-2400-7825-bcc8-de85fa947f72", "sb.variable.current_engine_version", "text", "engine", "session", "IMMUTABLE", "public", "not_applicable"},
  {"current_dialect_version", "019de5fc-2400-7f6a-aae2-03ce3c4d52a5", "sb.variable.current_dialect_version", "text", "engine", "session", "IMMUTABLE", "public", "not_applicable"},
  {"application_name", "019de5fc-2400-76d8-8b2f-35ce31ea6f51", "sb.variable.application_name", "text", "session", "session", "VOLATILE", "public", "SBSQL.NOT_CONNECTED"},
  {"client_address", "019de5fc-2400-7a41-a3ac-b7ff6652081e", "sb.variable.client_address", "text", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"client_port", "019de5fc-2400-7248-a121-bd6efb02e0be", "sb.variable.client_port", "integer", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"client_protocol", "019de5fc-2400-7343-8f00-a8030135d374", "sb.variable.client_protocol", "text", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"current_capability_set", "019de5fc-2400-7834-9cd1-1a0183fa84dc", "sb.variable.current_capability_set", "text[]", "security", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"now", "019de5fc-2400-724d-8ebd-233df9f1c331", "sb.variable.now", "timestamptz", "engine", "statement", "VOLATILE", "public", "not_applicable"},
  {"statement_timestamp", "019de5fc-2400-7408-815f-36a1ebe334aa", "sb.variable.statement_timestamp", "timestamptz", "engine", "statement", "STABLE", "public", "not_applicable"},
  {"transaction_timestamp", "019de5fc-2400-762d-bfcd-c208ec871377", "sb.variable.transaction_timestamp", "timestamptz", "txn", "transaction", "STABLE", "public", "SBSQL.NO_TRANSACTION"},
  {"clock_timestamp", "019de5fc-2400-7e5b-942f-b192f2ccea92", "sb.variable.clock_timestamp", "timestamptz", "engine", "statement", "VOLATILE", "public", "not_applicable"},
  {"random_seed", "019de5fc-2400-7f4c-ae71-1fff662e4ab5", "sb.variable.random_seed", "bigint", "engine", "session", "VOLATILE", "RANDOM_SEED_CONTROL", "SBSQL.CAPABILITY_REQUIRED"},
  {"read_only_session", "019de5fc-2400-74b0-bfa1-1b7283414709", "sb.variable.read_only_session", "boolean", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"tx_read_only", "019de5fc-2400-7b17-89b6-743b0e196504", "sb.variable.tx_read_only", "boolean", "txn", "transaction", "STABLE", "public", "SBSQL.NO_TRANSACTION"},
  {"lock_timeout_ms", "019de5fc-2400-71ab-850e-f06d072aef4e", "sb.variable.lock_timeout_ms", "integer", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"statement_timeout_ms", "019de5fc-2400-7e29-a350-f124db05af6b", "sb.variable.statement_timeout_ms", "integer", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"idle_in_transaction_session_timeout_ms", "019de5fc-2400-711a-956b-9cc05262ffdf", "sb.variable.idle_in_transaction_session_timeout_ms", "integer", "session", "session", "STABLE", "public", "SBSQL.NOT_CONNECTED"},
  {"private_profile_active", "019de5fc-2400-753c-b5bb-c65db54c4048", "sb.variable.private_profile_active", "boolean", "session", "session", "STABLE", "PRIVATE_PROFILE_READ", "SBSQL.CAPABILITY_REQUIRED"},
  {"evidence_chain_uuid", "019de5fc-2400-7234-ab1c-8f953706dab4", "sb.variable.evidence_chain_uuid", "uuid", "txn", "transaction", "STABLE", "public", "SBSQL.NO_TRANSACTION"},
};
SystemVariableUuid FixtureUuid(std::string_view text) {
  SystemVariableUuid out{};
  std::size_t n=0;
  unsigned char half=0;
  for(char c:text) {
    if(c=='-') continue;
    Check((c>='0'&&c<='9')||(c>='a'&&c<='f'),"fixture UUID hex");
    const auto v=static_cast<unsigned char>(c<='9'?c-'0':c-'a'+10);
    if(n%2==0) half=static_cast<unsigned char>(v<<4);
    else out.bytes[n/2]=static_cast<unsigned char>(half|v);
    ++n;
  }
  Check(n==32,"fixture UUID width");
  return out;
}
constexpr std::string_view type_names[]{"text","uuid","bigint","integer","text[]","timestamptz","boolean"};
constexpr std::string_view source_names[]{"security","catalog","session","cluster","txn","engine"};
constexpr std::string_view scope_names[]{"connection","session","statement","transaction","cluster_authority"};
constexpr std::string_view volatility_names[]{"IMMUTABLE","STABLE","VOLATILE"};
constexpr std::string_view right_names[]{"public","CATALOG_READ","RANDOM_SEED_CONTROL","PRIVATE_PROFILE_READ"};
static_assert(std::is_same_v<decltype(SystemVariableEntry::variable_uuid),SystemVariableUuid>);
static_assert(sizeof(decltype(SystemVariableEntry::variable_uuid))==16);
static_assert(noexcept(FindSystemVariable(SystemVariableUuid{})));
static_assert(FindSystemVariable(SystemVariableUuid{})==nullptr);
}
int main() {
  const auto registry=StandardSystemVariableRegistry();
  Check(registry.size()==std::size(expected),"exact specification seed count");
  Check(scratchbird::engine::sblr::StandardSblrContextVariableRegistry().data()==registry.data(),
        "engine borrows the same binary seed, no divergent context registry");
  allocations_before_failure=0;allocation_attempts=0;
  for(const auto& e:expected) {
    const auto uuid=FixtureUuid(e.uuid);
    const auto* row=FindSystemVariable(uuid);
    Check(row!=nullptr,"every exact canonical UUID resolves");
    Check(row->variable_id==e.id&&row->canonical_name==e.name,"semantic ID and parser name");
    Check(type_names[static_cast<unsigned>(row->result_type)]==e.type,"specified result type");
    Check(source_names[static_cast<unsigned>(row->source)]==e.source,"owning source");
    Check(scope_names[static_cast<unsigned>(row->scope)]==e.scope,"scope lifetime");
    Check(volatility_names[static_cast<unsigned>(row->volatility)]==e.volatility,"optimizer volatility");
    Check(right_names[static_cast<unsigned>(row->required_right)]==e.right,"capability policy");
    Check(row->diagnostic_if_unavailable==e.unavailable,"exact unavailable diagnostic");
    for(unsigned bit=0;bit<128;++bit) {
      auto altered=uuid;altered.bytes[bit/8]^=static_cast<unsigned char>(1u<<(bit%8));
      const auto* found=FindSystemVariable(altered);
      Check(found==nullptr||found->variable_uuid==altered,"lookup never accepts a prefix or forged suffix");
    }
    for(unsigned version=0;version<16;++version) {
      if(version==7) continue;
      auto altered=uuid;altered.bytes[6]=static_cast<unsigned char>((version<<4)|(altered.bytes[6]&15));
      Check(FindSystemVariable(altered)==nullptr,"non-v7 system seed identity refused");
    }
  }
  const auto legacy=FixtureUuid("019b6cf8-b000-740c-add4-4ac7f3cf1904");
  Check(FindSystemVariable(legacy)==nullptr,"unregistered legacy context UUID is not silently reassigned");
  Check(FindSystemVariable(SystemVariableUuid{})==nullptr,"nil not a variable");
  for(std::size_t i=1;i<kSystemVariableUuidOrder.size();++i)
    Check(registry[kSystemVariableUuidOrder[i-1]].variable_uuid<registry[kSystemVariableUuidOrder[i]].variable_uuid,
          "binary search index strictly ordered");
  Check(allocation_attempts==0,"all registry operations are allocation-free");
  allocations_before_failure=-1;
  std::printf("PASS checks=%zu system_variable_seed_only=true\n",checks);
}
