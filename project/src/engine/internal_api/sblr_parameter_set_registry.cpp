// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_parameter_set_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::string_view kMagic = "SBPSR1";
constexpr std::string_view kDomain = "ScratchBird.SblrParameterSetRegistry.V1";
std::mutex& RegistryMutex() { static std::mutex value; return value; }
std::unordered_map<std::string, SblrParameterSetSnapshot>& LiveSets() {
  static std::unordered_map<std::string, SblrParameterSetSnapshot> value;
  return value;
}

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}
bool HasAdmin(const EngineRequestContext& context) {
  return context.security_context_present &&
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "right:SBLR_PARAMETER_SET_ADMIN") != context.trace_tags.end();
}
bool HasPrivateReceiptAuthority(const EngineRequestContext& context) {
  return context.security_context_present &&
      context.statement_metadata_snapshot_engine_owned &&
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "private_statement_context_receipt") != context.trace_tags.end();
}
bool SafeReason(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
           c == '-';
  });
}
bool ValidUuid(std::string_view text, scratchbird::core::platform::UuidKind kind) {
  if (text.empty()) return false;
  if (kind == scratchbird::core::platform::UuidKind::session) {
    return scratchbird::core::uuid::ParseTypedUuid(kind, std::string(text)).ok();
  }
  return scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
      kind, std::string(text)).ok();
}
bool ValidOptionalPair(std::string_view uuid, std::uint64_t generation) {
  return (uuid.empty() && generation == 0) ||
      (generation != 0 &&
       ValidUuid(uuid, scratchbird::core::platform::UuidKind::object));
}
std::string GenerateUuid(scratchbird::core::platform::UuidKind kind,
                         std::uint64_t salt) {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      kind, now + salt);
  return generated.ok()
      ? scratchbird::core::uuid::UuidToString(generated.value.value)
      : std::string{};
}
std::string Sha256(std::string_view bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data()),
      bytes.size());
  return digest.ok()
      ? "sha256:" + scratchbird::core::hash::HexLower(digest.digest)
      : std::string{};
}
void Field(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size())); out->push_back(':'); out->append(value);
}
std::string SlotsText(const std::vector<SblrParameterSlotDescriptor>& slots) {
  std::ostringstream out;
  for (const auto& slot : slots) {
    out << slot.slot_ordinal << ',' << slot.slot_uuid << ','
        << slot.datatype_descriptor_uuid << ','
        << slot.datatype_descriptor_generation << ','
        << static_cast<unsigned>(slot.direction) << ','
        << (slot.nullable ? 1 : 0) << ';';
  }
  return out.str();
}
std::vector<std::string> Split(std::string_view value, char separator) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(separator, start);
    result.emplace_back(value.substr(start, end == std::string_view::npos
                                               ? value.size() - start
                                               : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}
std::uint64_t U64(std::string_view value) {
  if (value.empty()) return 0;
  std::uint64_t result = 0;
  for (char c : value) {
    if (c < '0' || c > '9' || result > (UINT64_MAX - (c - '0')) / 10) return 0;
    result = result * 10 + static_cast<unsigned>(c - '0');
  }
  return result;
}
bool ValidateSlots(const std::vector<SblrParameterSlotDescriptor>& slots) {
  if (slots.empty() || slots.size() > 4096) return false;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    const auto direction = static_cast<unsigned>(slot.direction);
    if (slot.slot_ordinal != i ||
        !ValidUuid(slot.slot_uuid, scratchbird::core::platform::UuidKind::object) ||
        !ValidUuid(slot.datatype_descriptor_uuid,
                   scratchbird::core::platform::UuidKind::object) ||
        slot.datatype_descriptor_generation == 0 || direction < 1 || direction > 3) {
      return false;
    }
  }
  return true;
}
bool ParseSlots(std::string_view text,
                std::vector<SblrParameterSlotDescriptor>* slots) {
  if (slots == nullptr || text.empty() || text.back() != ';') return false;
  slots->clear();
  for (const auto& record : Split(text.substr(0, text.size() - 1), ';')) {
    const auto fields = Split(record, ',');
    if (fields.size() != 6) return false;
    SblrParameterSlotDescriptor slot;
    const auto ordinal = U64(fields[0]);
    if (ordinal > UINT32_MAX) return false;
    slot.slot_ordinal = static_cast<std::uint32_t>(ordinal);
    slot.slot_uuid = fields[1];
    slot.datatype_descriptor_uuid = fields[2];
    slot.datatype_descriptor_generation = U64(fields[3]);
    const auto direction = U64(fields[4]);
    if (direction < 1 || direction > 3 || (fields[5] != "0" && fields[5] != "1"))
      return false;
    slot.direction = static_cast<SblrParameterDirection>(direction);
    slot.nullable = fields[5] == "1";
    slots->push_back(std::move(slot));
  }
  return ValidateSlots(*slots);
}
bool SameSlots(const std::vector<SblrParameterSlotDescriptor>& left,
               const std::vector<SblrParameterSlotDescriptor>& right) {
  if (left.size()!=right.size()) return false;
  for (std::size_t i=0;i<left.size();++i) {
    if (left[i].slot_ordinal!=right[i].slot_ordinal ||
        left[i].slot_uuid!=right[i].slot_uuid ||
        left[i].datatype_descriptor_uuid!=right[i].datatype_descriptor_uuid ||
        left[i].datatype_descriptor_generation!=right[i].datatype_descriptor_generation ||
        left[i].direction!=right[i].direction || left[i].nullable!=right[i].nullable)
      return false;
  }
  return true;
}
bool SameImmutableBinding(const SblrParameterSetSnapshot& left,
                          const SblrParameterSetSnapshot& right) {
  return left.database_uuid==right.database_uuid &&
      left.session_uuid==right.session_uuid &&
      left.statement_receipt_uuid==right.statement_receipt_uuid &&
      left.execution_uuid==right.execution_uuid &&
      left.parameter_set_descriptor_uuid==right.parameter_set_descriptor_uuid &&
      left.prepared_statement_uuid==right.prepared_statement_uuid &&
      left.prepared_generation==right.prepared_generation &&
      left.batch_uuid==right.batch_uuid && left.batch_generation==right.batch_generation &&
      left.dynamic_package_uuid==right.dynamic_package_uuid &&
      left.dynamic_generation==right.dynamic_generation &&
      left.catalog_generation==right.catalog_generation &&
      left.security_epoch==right.security_epoch && left.resource_epoch==right.resource_epoch &&
      left.slots_sha256==right.slots_sha256 && SameSlots(left.slots,right.slots);
}
std::string SnapshotMaterial(const SblrParameterSetSnapshot& value,
                             std::string_view prior_uuid,
                             std::uint64_t prior_generation,
                             std::string_view reason) {
  std::string out(kDomain);
  for (const auto& field : {value.snapshot_uuid, value.database_uuid,
                            value.session_uuid, value.statement_receipt_uuid,
                            value.execution_uuid,
                            value.parameter_set_descriptor_uuid,
                            value.prepared_statement_uuid, value.batch_uuid,
                            value.dynamic_package_uuid, value.slots_sha256,
                            std::string(prior_uuid), std::string(reason)}) Field(&out, field);
  for (auto number : {value.snapshot_generation, value.descriptor_generation,
                      value.prepared_generation, value.batch_generation,
                      value.dynamic_generation, value.catalog_generation,
                      value.security_epoch, value.resource_epoch,
                      prior_generation,
                      static_cast<std::uint64_t>(value.state)}) {
    Field(&out, std::to_string(number));
  }
  Field(&out, SlotsText(value.slots));
  return out;
}
std::string Record(std::string_view kind, const SblrParameterSetSnapshot& value,
                   std::string_view prior_uuid, std::uint64_t prior_generation,
                   std::string_view reason) {
  std::ostringstream out;
  out << kMagic << '\t' << kind << '\t' << value.snapshot_uuid << '\t'
      << value.snapshot_generation << '\t' << value.database_uuid << '\t'
      << value.session_uuid << '\t' << value.statement_receipt_uuid << '\t'
      << value.execution_uuid << '\t' << value.parameter_set_descriptor_uuid << '\t'
      << value.descriptor_generation << '\t' << value.prepared_statement_uuid << '\t'
      << value.prepared_generation << '\t' << value.batch_uuid << '\t'
      << value.batch_generation << '\t' << value.dynamic_package_uuid << '\t'
      << value.dynamic_generation << '\t' << value.catalog_generation << '\t'
      << value.security_epoch << '\t' << value.resource_epoch << '\t'
      << static_cast<unsigned>(value.state) << '\t' << value.slots_sha256 << '\t'
      << value.decision_evidence_sha256 << '\t' << prior_uuid << '\t'
      << prior_generation << '\t' << reason << '\t' << SlotsText(value.slots);
  return out.str();
}
bool DurableAppend(const std::string& path, const std::string& line) {
  { std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false; out << line << '\n'; out.flush(); if (!out) return false; }
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const bool ok = FlushFileBuffers(handle) != 0; CloseHandle(handle); return ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false; const bool ok = ::fsync(fd) == 0; ::close(fd); return ok;
#endif
}
std::string StorePath(const EngineRequestContext& context, std::string_view uuid) {
  return context.database_path + ".sb.sblr_parameter_set." + std::string(uuid) + ".v1";
}
bool DecodeRecord(const std::string& line, std::string_view kind,
                  SblrParameterSetSnapshot* value, std::string* prior_uuid,
                  std::uint64_t* prior_generation, std::string* reason) {
  const auto f = Split(line, '\t');
  if (f.size() != 26 || f[0] != kMagic || f[1] != kind) return false;
  value->snapshot_uuid=f[2]; value->snapshot_generation=U64(f[3]);
  value->database_uuid=f[4]; value->session_uuid=f[5];
  value->statement_receipt_uuid=f[6]; value->execution_uuid=f[7];
  value->parameter_set_descriptor_uuid=f[8]; value->descriptor_generation=U64(f[9]);
  value->prepared_statement_uuid=f[10]; value->prepared_generation=U64(f[11]);
  value->batch_uuid=f[12]; value->batch_generation=U64(f[13]);
  value->dynamic_package_uuid=f[14]; value->dynamic_generation=U64(f[15]);
  value->catalog_generation=U64(f[16]); value->security_epoch=U64(f[17]);
  value->resource_epoch=U64(f[18]);
  const auto state=U64(f[19]); value->slots_sha256=f[20];
  value->decision_evidence_sha256=f[21]; *prior_uuid=f[22];
  *prior_generation=U64(f[23]); *reason=f[24];
  if (state < 1 || state > 2 || !ParseSlots(f[25], &value->slots)) return false;
  value->state=static_cast<SblrParameterSetState>(state);
  return ValidUuid(value->snapshot_uuid, scratchbird::core::platform::UuidKind::object) &&
      value->snapshot_generation != 0 &&
      ValidUuid(value->database_uuid, scratchbird::core::platform::UuidKind::database) &&
      ValidUuid(value->session_uuid, scratchbird::core::platform::UuidKind::session) &&
      ValidUuid(value->statement_receipt_uuid, scratchbird::core::platform::UuidKind::object) &&
      ValidUuid(value->execution_uuid, scratchbird::core::platform::UuidKind::object) &&
      ValidUuid(value->parameter_set_descriptor_uuid,
                scratchbird::core::platform::UuidKind::object) &&
      value->descriptor_generation != 0 && value->catalog_generation != 0 &&
      value->security_epoch != 0 && value->resource_epoch != 0 &&
      ValidOptionalPair(value->prepared_statement_uuid,value->prepared_generation) &&
      ValidOptionalPair(value->batch_uuid,value->batch_generation) &&
      ValidOptionalPair(value->dynamic_package_uuid,value->dynamic_generation) &&
      value->slots_sha256 == Sha256(SlotsText(value->slots)) && SafeReason(*reason) &&
      value->decision_evidence_sha256 == Sha256(SnapshotMaterial(
          *value, *prior_uuid, *prior_generation, *reason));
}
bool Publish(const EngineRequestContext& context, const SblrParameterSetSnapshot& value,
             std::string_view prior_uuid, std::uint64_t prior_generation,
             std::string_view reason) {
  const auto path=StorePath(context,value.parameter_set_descriptor_uuid);
  return DurableAppend(path,Record("EVIDENCE",value,prior_uuid,prior_generation,reason)) &&
      DurableAppend(path,Record("SNAPSHOT",value,prior_uuid,prior_generation,reason));
}
SblrParameterSetLoadResult LoadLocked(const EngineRequestContext& context,
                                     const std::string& descriptor_uuid) {
  SblrParameterSetLoadResult result;
  if (context.database_path.empty() ||
      !ValidUuid(context.database_uuid.canonical,
                 scratchbird::core::platform::UuidKind::database) ||
      !ValidUuid(descriptor_uuid,scratchbird::core::platform::UuidKind::object)) {
    result.diagnostic=Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.identity_invalid","fail closed");
    return result;
  }
  std::ifstream in(StorePath(context,descriptor_uuid),std::ios::binary);
  if (!in) { result.diagnostic=Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.absent","parameter set absent"); return result; }
  std::vector<std::string> lines; std::string line;
  while (std::getline(in,line)) lines.push_back(line);
  if (!in.eof() || lines.empty() || lines.size()%2) {
    result.diagnostic=Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.torn","torn registry publication"); return result;
  }
  SblrParameterSetSnapshot prior;
  for (std::size_t i=0;i<lines.size();i+=2) {
    SblrParameterSetSnapshot evidence,snapshot; std::string ep,sp,er,sr; std::uint64_t eg=0,sg=0;
    if (!DecodeRecord(lines[i],"EVIDENCE",&evidence,&ep,&eg,&er) ||
        !DecodeRecord(lines[i+1],"SNAPSHOT",&snapshot,&sp,&sg,&sr) ||
        lines[i].substr(lines[i].find('\t',lines[i].find('\t')+1)) !=
            lines[i+1].substr(lines[i+1].find('\t',lines[i+1].find('\t')+1)) ||
        ep!=sp || eg!=sg || er!=sr || snapshot.database_uuid!=context.database_uuid.canonical ||
        snapshot.parameter_set_descriptor_uuid!=descriptor_uuid ||
        ((i==0) ? (!ep.empty()||eg!=0||snapshot.snapshot_generation!=1||
                    snapshot.descriptor_generation!=1||
                    snapshot.state!=SblrParameterSetState::active)
                : (ep!=prior.snapshot_uuid||eg!=prior.snapshot_generation||
                   snapshot.snapshot_generation!=prior.snapshot_generation+1||
                   snapshot.descriptor_generation!=prior.descriptor_generation+1||
                   prior.state!=SblrParameterSetState::active||
                   snapshot.state!=SblrParameterSetState::revoked||
                   !SameImmutableBinding(prior,snapshot)))) {
      result.diagnostic=Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.corrupt","contradictory registry evidence"); return result;
    }
    prior=std::move(snapshot);
  }
  result.ok=true; result.snapshot=std::move(prior);
  result.diagnostic=MakeEngineApiDiagnostic("OK","ok",{},false); return result;
}
}  // namespace

SblrParameterSetMutationResult IssueSblrParameterSet(
    const EngineRequestContext& context,const SblrParameterSetIssueRequest& request) {
  std::lock_guard lock(RegistryMutex()); SblrParameterSetMutationResult result;
  if (!HasPrivateReceiptAuthority(context)) { result.diagnostic=Diagnostic("SECURITY.ACCESS_DENIED","sblr.parameter_set.issue_denied","engine-owned private statement receipt required"); return result; }
  if (!ValidUuid(context.database_uuid.canonical,scratchbird::core::platform::UuidKind::database) ||
      !ValidUuid(context.session_uuid.canonical,scratchbird::core::platform::UuidKind::session) ||
      !ValidUuid(request.statement_receipt_uuid,scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(request.execution_uuid,scratchbird::core::platform::UuidKind::object) ||
      context.catalog_generation_id==0||context.security_epoch==0||context.resource_epoch==0||
      !ValidOptionalPair(request.prepared_statement_uuid,request.prepared_generation)||
      !ValidOptionalPair(request.batch_uuid,request.batch_generation)||
      !ValidOptionalPair(request.dynamic_package_uuid,request.dynamic_generation)||
      request.slots.empty()||request.slots.size()>4096||!SafeReason(request.reason_code)) {
    result.diagnostic=Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.issue_invalid","exact engine context and demands required"); return result;
  }
  if (!request.prepared_statement_uuid.empty() &&
      !request.dynamic_package_uuid.empty()) {
    result.diagnostic=Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.identity_matrix_invalid","prepared and dynamic identities are mutually exclusive"); return result;
  }
  SblrParameterSetSnapshot value; value.snapshot_uuid=GenerateUuid(scratchbird::core::platform::UuidKind::object,1);
  value.snapshot_generation=1; value.database_uuid=context.database_uuid.canonical;
  value.session_uuid=context.session_uuid.canonical; value.statement_receipt_uuid=request.statement_receipt_uuid;
  value.execution_uuid=request.execution_uuid; value.parameter_set_descriptor_uuid=GenerateUuid(scratchbird::core::platform::UuidKind::object,2);
  value.descriptor_generation=1; value.prepared_statement_uuid=request.prepared_statement_uuid;
  value.prepared_generation=request.prepared_generation; value.batch_uuid=request.batch_uuid;
  value.batch_generation=request.batch_generation; value.dynamic_package_uuid=request.dynamic_package_uuid;
  value.dynamic_generation=request.dynamic_generation; value.catalog_generation=context.catalog_generation_id;
  value.security_epoch=context.security_epoch; value.resource_epoch=context.resource_epoch;
  value.state=SblrParameterSetState::active;
  for (std::size_t i=0;i<request.slots.size();++i) {
    const auto& demand=request.slots[i]; const auto direction=static_cast<unsigned>(demand.direction);
    if (!ValidUuid(demand.datatype_descriptor_uuid,scratchbird::core::platform::UuidKind::object)||
        demand.datatype_descriptor_generation==0||direction<1||direction>3) {
      result.diagnostic=Diagnostic("DATATYPE.DESCRIPTOR_INVALID","sblr.parameter_set.slot_descriptor_invalid","exact datatype identity required"); return result;
    }
    value.slots.push_back({static_cast<std::uint32_t>(i),GenerateUuid(scratchbird::core::platform::UuidKind::object,3+i),demand.datatype_descriptor_uuid,demand.datatype_descriptor_generation,demand.direction,demand.nullable});
  }
  value.slots_sha256=Sha256(SlotsText(value.slots));
  value.decision_evidence_sha256=Sha256(SnapshotMaterial(value,{},0,request.reason_code));
  std::error_code error;
  if (value.snapshot_uuid.empty()||value.parameter_set_descriptor_uuid.empty()||
      !ValidateSlots(value.slots)||value.slots_sha256.empty()||value.decision_evidence_sha256.empty()||
      std::filesystem::exists(StorePath(context,value.parameter_set_descriptor_uuid),error)||error||
      !Publish(context,value,{},0,request.reason_code)) {
    result.diagnostic=Diagnostic("SBLR.EXECUTION_FAILED","sblr.parameter_set.publish_failed","durable issue failed"); return result;
  }
  LiveSets()[value.parameter_set_descriptor_uuid]=value;
  result.ok=true; result.snapshot=value; result.diagnostic=MakeEngineApiDiagnostic("OK","ok",{},false);
  result.evidence.push_back({"sblr.parameter_set.issue",value.decision_evidence_sha256}); return result;
}

SblrParameterSetLoadResult LoadSblrParameterSet(const EngineRequestContext& context,
                                                const std::string& uuid) {
  std::lock_guard lock(RegistryMutex()); return LoadLocked(context,uuid);
}

EngineApiDiagnostic BeginSblrParameterSetRegistryRecovery(
    const EngineRequestContext& context) {
  std::lock_guard lock(RegistryMutex());
  if (!HasAdmin(context)) {
    return Diagnostic("SECURITY.ACCESS_DENIED","sblr.parameter_set.recovery_denied","startup recovery authority required");
  }
  for (auto it=LiveSets().begin();it!=LiveSets().end();) {
    if (it->second.database_uuid==context.database_uuid.canonical) it=LiveSets().erase(it);
    else ++it;
  }
  return MakeEngineApiDiagnostic("OK","ok",{},false);
}

SblrParameterSetMutationResult InvalidateSblrParameterSet(
    const EngineRequestContext& context,const std::string& uuid,
    const std::string& expected_uuid,std::uint64_t expected_generation,
    const std::string& reason) {
  std::lock_guard lock(RegistryMutex()); SblrParameterSetMutationResult result;
  if (!HasAdmin(context) && !HasPrivateReceiptAuthority(context)) { result.diagnostic=Diagnostic("SECURITY.ACCESS_DENIED","sblr.parameter_set.invalidate_denied","admin or engine-owned receipt authority required"); return result; }
  auto loaded=LoadLocked(context,uuid); if(!loaded.ok){result.diagnostic=loaded.diagnostic;return result;}
  if(loaded.snapshot.snapshot_uuid!=expected_uuid||loaded.snapshot.snapshot_generation!=expected_generation){result.diagnostic=Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.compare_stale","snapshot compare failed");return result;}
  if(!SafeReason(reason)){result.diagnostic=Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.reason_invalid","canonical reason required");return result;}
  auto next=loaded.snapshot; next.snapshot_uuid=GenerateUuid(scratchbird::core::platform::UuidKind::object,next.snapshot_generation+11);
  ++next.snapshot_generation; ++next.descriptor_generation; next.state=SblrParameterSetState::revoked;
  next.decision_evidence_sha256=Sha256(SnapshotMaterial(next,loaded.snapshot.snapshot_uuid,loaded.snapshot.snapshot_generation,reason));
  if(next.snapshot_uuid.empty()||next.decision_evidence_sha256.empty()||!Publish(context,next,loaded.snapshot.snapshot_uuid,loaded.snapshot.snapshot_generation,reason)){result.diagnostic=Diagnostic("SBLR.EXECUTION_FAILED","sblr.parameter_set.invalidate_failed","durable invalidation failed");return result;}
  LiveSets().erase(uuid);
  result.ok=true;result.snapshot=next;result.diagnostic=MakeEngineApiDiagnostic("OK","ok",{},false);result.evidence.push_back({"sblr.parameter_set.invalidate",next.decision_evidence_sha256});return result;
}

EngineApiDiagnostic RevalidateSblrParameterSet(
    const EngineRequestContext& context,const SblrParameterSetSnapshot& admitted,
    const std::string& receipt,const std::string& execution,
    const std::string& prepared,std::uint64_t prepared_generation,
    const std::string& batch,std::uint64_t batch_generation,
    const std::string& dynamic,std::uint64_t dynamic_generation,
    SblrParameterSetSnapshot* current) {
  {
    std::lock_guard lock(RegistryMutex());
    const auto live=LiveSets().find(admitted.parameter_set_descriptor_uuid);
    if(live==LiveSets().end()||live->second.snapshot_uuid!=admitted.snapshot_uuid)
      return Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.execution_not_live","receipt execution authority does not recover");
  }
  const auto loaded=LoadSblrParameterSet(context,admitted.parameter_set_descriptor_uuid);
  if(!loaded.ok)return loaded.diagnostic; if(current)*current=loaded.snapshot;
  if(loaded.snapshot.state!=SblrParameterSetState::active)return Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.revoked","parameter set revoked");
  if(loaded.snapshot.snapshot_uuid!=admitted.snapshot_uuid||loaded.snapshot.snapshot_generation!=admitted.snapshot_generation||loaded.snapshot.descriptor_generation!=admitted.descriptor_generation||
     loaded.snapshot.session_uuid!=context.session_uuid.canonical||loaded.snapshot.statement_receipt_uuid!=receipt||loaded.snapshot.execution_uuid!=execution||
     loaded.snapshot.prepared_statement_uuid!=prepared||loaded.snapshot.prepared_generation!=prepared_generation||loaded.snapshot.batch_uuid!=batch||loaded.snapshot.batch_generation!=batch_generation||loaded.snapshot.dynamic_package_uuid!=dynamic||loaded.snapshot.dynamic_generation!=dynamic_generation||
     loaded.snapshot.catalog_generation!=context.catalog_generation_id||loaded.snapshot.security_epoch!=context.security_epoch||loaded.snapshot.resource_epoch!=context.resource_epoch)
    return Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.binding_stale","immutable binding changed");
  return MakeEngineApiDiagnostic("OK","ok",{},false);
}
}  // namespace scratchbird::engine::internal_api
