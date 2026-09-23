#include "sblr_transaction_begin_authority.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"
#include "mga_relation_store/mga_binary_identity_codec.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex authority_mutex;
constexpr std::string_view kMagic = "SBTBEG02";
constexpr std::size_t kPayloadSize = 72;
constexpr std::size_t kRecordSize = kPayloadSize + 32;
EngineUuid New(std::uint64_t salt) {
  const auto millis = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto value = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::object, millis + salt);
  return value.ok() ? value.value.value : EngineUuid{};
}
EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  const bool error = code != "OK";
  return MakeEngineApiDiagnostic(std::move(code),std::move(key),{},error);
}
auto Bytes(const std::string& value) {
  return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()),value.size());
}
bool Encode(const EngineUuid& database, const SblrTransactionBeginAuthorityV1& authority,
            std::string* output) {
  std::string bytes(kMagic);
  if (!AppendBinaryEngineUuid(&bytes,database) ||
      !AppendBinaryEngineUuid(&bytes,authority.isolation_profile_uuid)) return false;
  AppendBinaryU64(&bytes,authority.isolation_profile_generation);
  if (!AppendBinaryEngineUuid(&bytes,authority.transaction_policy_snapshot_uuid)) return false;
  AppendBinaryU64(&bytes,authority.transaction_policy_generation);
  const auto digest=core::hash::ComputeSha256Digest(Bytes(bytes).data(), bytes.size());
  if (!digest.ok()) return false;
  bytes.append(reinterpret_cast<const char*>(digest.digest.data()),digest.digest.size());
  output->swap(bytes);return true;
}
bool Decode(const std::string& bytes,const EngineUuid& database,
            SblrTransactionBeginAuthorityV1* output) {
  if (bytes.size()!=kRecordSize || !bytes.starts_with(kMagic)) return false;
  const auto input=Bytes(bytes);
  const auto digest=core::hash::ComputeSha256Digest(input.data(), kPayloadSize);
  if (!digest.ok() || !std::equal(digest.digest.begin(),digest.digest.end(),input.begin()+kPayloadSize)) return false;
  SblrTransactionBeginAuthorityV1 candidate;
  EngineUuid stored_database;
  std::size_t cursor=kMagic.size();
  if (!ReadBinaryEngineUuid(input,&cursor,&stored_database) || stored_database!=database ||
      !ReadBinaryEngineUuid(input,&cursor,&candidate.isolation_profile_uuid) ||
      !ReadBinaryU64(input,&cursor,&candidate.isolation_profile_generation) ||
      !ReadBinaryEngineUuid(input,&cursor,&candidate.transaction_policy_snapshot_uuid) ||
      !ReadBinaryU64(input,&cursor,&candidate.transaction_policy_generation) ||
      !candidate.isolation_profile_generation || !candidate.transaction_policy_generation || cursor!=kPayloadSize) return false;
  *output=candidate;return true;
}
}

// Policy identity storage only. This record cannot publish transaction finality;
// durable MGA inventory remains the transaction authority.
SblrTransactionBeginAuthorityResultV1 LoadSblrTransactionBeginAuthorityV1(const EngineRequestContext& context) {
  SblrTransactionBeginAuthorityResultV1 result;
  if (!context.security_context_present || !context.statement_metadata_snapshot_engine_owned ||
      context.database_path.empty() || !core::uuid::IsEngineIdentityUuid(context.database_uuid)) {
    result.diagnostic=Diagnostic("SECURITY.ACCESS_DENIED","sblr.txn_begin.authority_hidden");return result;
  }
  std::lock_guard lock(authority_mutex);
  const auto path=context.database_path+".sb.sblr_txn_begin_authority.v2";
  std::error_code error;
  const bool exists=std::filesystem::exists(path,error);
  if (error) {
    result.diagnostic=Diagnostic("SBLR.EXECUTION_FAILED","sblr.txn_begin.authority_read_failed");return result;
  }
  SblrTransactionBeginAuthorityV1 candidate;
  if (exists) {
    std::ifstream input(path,std::ios::binary);
    std::string bytes(kRecordSize,'\0');
    if (!input.read(bytes.data(),static_cast<std::streamsize>(bytes.size())) ||
        input.peek()!=std::char_traits<char>::eof() || input.bad() ||
        !Decode(bytes,context.database_uuid,&candidate)) {
      result.diagnostic=Diagnostic("MGA.TRANSACTION.STALE","sblr.txn_begin.authority_corrupt");return result;
    }
  } else {
    const bool legacy=std::filesystem::exists(context.database_path+".sb.sblr_txn_begin_authority.v1",error);
    if (legacy || error) {
      result.diagnostic=Diagnostic("MGA.TRANSACTION.STALE","sblr.txn_begin.authority_format_unsupported");return result;
    }
    candidate.isolation_profile_uuid=New(1);candidate.isolation_profile_generation=1;
    candidate.transaction_policy_snapshot_uuid=New(2);candidate.transaction_policy_generation=1;
    std::string bytes;
    if (!Encode(context.database_uuid,candidate,&bytes)) {
      result.diagnostic=Diagnostic("SBLR.EXECUTION_FAILED","sblr.txn_begin.authority_identity_failed");return result;
    }
    // Serialize local publication and expose the complete checked record only.
    // The engine's database ownership lock remains responsible for process exclusion.
    const auto temporary=path+".tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.flush();output.close();
    if (!output) {
      result.diagnostic=Diagnostic("SBLR.EXECUTION_FAILED","sblr.txn_begin.authority_publish_failed");return result;
    }
    std::filesystem::rename(temporary,path,error);
    if (error) {
      result.diagnostic=Diagnostic("SBLR.EXECUTION_FAILED","sblr.txn_begin.authority_publish_failed");return result;
    }
  }
  result.authority=candidate;result.ok=true;result.diagnostic=Diagnostic("OK","ok");return result;
}
}
