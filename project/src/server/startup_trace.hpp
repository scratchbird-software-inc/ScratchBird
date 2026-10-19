// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "config.hpp"
#include "api_types.hpp"
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::server {
namespace startup_trace_detail {
struct Record {
  std::array<char,1024> bytes{};
  std::size_t used=0;
  bool complete=true;
  void Append(std::string_view text) noexcept {
    if(text.size()>bytes.size()-used){complete=false;return;}
    for(char c:text)bytes[used++]=c;
  }
  void Quoted(std::string_view text) noexcept {
    constexpr char hex[]="0123456789abcdef";
    Append("\"");
    for(unsigned char c:text){
      if(c=='"'||c=='\\'){const char escaped[]{'\\',static_cast<char>(c)};Append({escaped,2});}
      else if(c<32||c>126){const char escaped[]{'\\','u','0','0',hex[c>>4],hex[c&15]};Append({escaped,6});}
      else {const char value=static_cast<char>(c);Append({&value,1});}
      if(!complete)return;
    }
    Append("\"");
  }
  void Number(std::uint64_t value) noexcept {
    std::array<char,32> text;const auto converted=std::to_chars(text.data(),text.data()+text.size(),value);
    if(converted.ec!=std::errc{}){complete=false;return;}
    Append({text.data(),static_cast<std::size_t>(converted.ptr-text.data())});
  }
};
inline std::atomic<std::uint64_t> sequence{0};
}  // namespace startup_trace_detail

// Explicit startup logging, independent of hot-path/profile instrumentation.
// Only bounded phase/code labels and non-secret numeric observations are
// emitted. Never serialize a context, credential, verifier, path or grant list.
// These observations do not authenticate, authorize or establish readiness.
inline void TraceServerStartupPhase(const ServerBootstrapConfig& config,
    std::string_view phase,std::string_view outcome,
    const engine::internal_api::EngineRequestContext* context=nullptr,
    const engine::internal_api::EngineApiDiagnostic* diagnostic=nullptr) noexcept {
  if(config.log_level!="trace")return;
  startup_trace_detail::Record record;
  record.Append("{\"server_startup_trace\":{\"sequence\":");
  record.Number(startup_trace_detail::sequence.fetch_add(1,std::memory_order_relaxed)+1);
  record.Append(",\"monotonic_ns\":");
  record.Number(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()));
  record.Append(",\"pid\":");
#if defined(_WIN32)
  record.Number(static_cast<std::uint64_t>(::_getpid()));
#else
  record.Number(static_cast<std::uint64_t>(::getpid()));
#endif
  record.Append(",\"phase\":");record.Quoted(phase);
  record.Append(",\"outcome\":");record.Quoted(outcome);
  if(context){
    record.Append(",\"local_transaction_id\":");record.Number(context->local_transaction_id);
    record.Append(",\"authorization_context_generation\":");record.Number(context->authorization_context.security_context_generation);
    record.Append(",\"security_epoch\":");record.Number(context->security_epoch);
    record.Append(",\"catalog_generation\":");record.Number(context->catalog_generation_id);
  }
  if(diagnostic){
    record.Append(",\"diagnostic_code\":");record.Quoted(diagnostic->code);
    for(const auto& field:diagnostic->fields)if(field.key=="authorization_context_failure"){
      record.Append(",\"authorization_context_failure\":");record.Quoted(field.value);break;
    }
  }
  record.Append("}}\n");
  if(record.complete)(void)std::fwrite(record.bytes.data(),1,record.used,stderr);
}
}  // namespace scratchbird::server
