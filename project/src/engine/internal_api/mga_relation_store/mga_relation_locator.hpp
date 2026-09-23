// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_metadata_record_codec.hpp"
#include "mga_update_durable_frame_store_internal.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
namespace scratchbird::engine::internal_api {
// Filesystem locators are allocated ordinals. The UUID-to-ordinal directory is
// binary companion metadata, never transaction finality or visibility authority.
using MgaRelationLocators = std::map<EngineUuid, std::uint64_t>;
inline MgaRelationLocators LoadMgaRelationLocators(const std::string& root) {
  MgaRelationLocators result;
  const auto path = root + "/locators.v2";
  if (!std::filesystem::exists(root)) return result;
  if (!std::filesystem::exists(path)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      const auto name = entry.path().filename().string();
      if (name != "locators.v2.lock" && name != "locators.v2.pending")
        throw std::runtime_error("mga_relation_locator_missing_or_legacy");
    }
    return result;
  }
  const auto size = std::filesystem::file_size(path);
  if (size > kMgaMetadataMaximumBytes + 48) throw std::runtime_error("mga_relation_locator_extent_invalid");
  std::ifstream file(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!file || !file.read(bytes.data(), static_cast<std::streamsize>(size)) ||
      file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("mga_relation_locator_read_failed");
  std::vector<std::string> fields;
  if (!DecodeMgaMetadataFields(bytes, &fields) || fields[0] != "relation.locators.v2" || fields.size() % 2 != 1)
    throw std::runtime_error("mga_relation_locator_frame_invalid");
  std::uint64_t expected = 1;
  for (std::size_t n=1;n<fields.size();n+=2,++expected) {
    EngineUuid id;
    std::size_t cursor=0;
    std::uint64_t ordinal=0;
    const std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(fields[n+1].data()),fields[n+1].size());
    if (!ReadMetadataUuid(fields[n],&id) || input.size()!=8 || !ReadBinaryU64(input,&cursor,&ordinal) ||
        ordinal!=expected || !result.emplace(id,ordinal).second)
      throw std::runtime_error("mga_relation_locator_identity_or_ordinal_invalid");
  }
  return result;
}
inline void StoreMgaRelationLocators(const std::string& root, const MgaRelationLocators& locators) {
  std::map<std::uint64_t,EngineUuid> ordered;
  for (const auto& [id,ordinal]:locators) {
    if (!core::uuid::IsEngineIdentityUuid(id) || !ordered.emplace(ordinal,id).second)
      throw std::runtime_error("mga_relation_locator_assignment_invalid");
  }
  std::vector<std::string> fields{"relation.locators.v2"};
  std::uint64_t expected=1;
  for (const auto& [ordinal,id]:ordered) {
    if (ordinal!=expected++) throw std::runtime_error("mga_relation_locator_sequence_invalid");
    fields.push_back(MetadataUuidBytes(id));
    std::string bytes;AppendBinaryU64(&bytes,ordinal);fields.push_back(std::move(bytes));
  }
  const auto bytes=EncodeMgaMetadataFields(fields);
  if (bytes.empty()) throw std::runtime_error("mga_relation_locator_capacity_exceeded");
  const auto path=root+"/locators.v2", pending=path+".pending";
#if defined(_WIN32)
  HANDLE file=CreateFileA(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("mga_relation_locator_write_failed");
  DWORD written=0;
  const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=0 &&
      written==bytes.size() && FlushFileBuffers(file)!=0;
  CloseHandle(file);
  if(!ok || !MoveFileExA(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("mga_relation_locator_publish_failed");
#else
  const int fd=::open(pending.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
  if(fd<0) throw std::runtime_error("mga_relation_locator_write_failed");
  std::size_t offset=0;
  while(offset<bytes.size()) {
    const auto written=::write(fd,bytes.data()+offset,bytes.size()-offset);
    if(written<0 && errno==EINTR) continue;
    if(written<=0) {::close(fd);throw std::runtime_error("mga_relation_locator_write_failed");}
    offset+=static_cast<std::size_t>(written);
  }
  const bool synced=::fsync(fd)==0;::close(fd);
  if(!synced || ::rename(pending.c_str(),path.c_str())!=0)
    throw std::runtime_error("mga_relation_locator_publish_failed");
  const int directory=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(directory<0) throw std::runtime_error("mga_relation_locator_directory_open_failed");
  const bool durable=::fsync(directory)==0;::close(directory);
  if(!durable) throw std::runtime_error("mga_relation_locator_directory_sync_failed");
#endif
}
inline std::string MgaScopedRelationBasePath(const EngineRequestContext& context,
                                            const EngineUuid& id, bool allocate=false) {
  if (!core::uuid::IsEngineIdentityUuid(id)) throw std::invalid_argument("mga_relation_identity_invalid");
  const auto root=context.database_path+".sb.mga_relation_scope";
  if (!allocate) {
    const auto locators=LoadMgaRelationLocators(root);
    const auto found=locators.find(id);
    return found==locators.end()?std::string{}:root+"/relation-"+std::to_string(found->second);
  }
  namespace detail=mga_update_durable_detail;
  if (!detail::DmlUpdateDurableEnsureDirectory(root)) throw std::runtime_error("mga_relation_locator_directory_failed");
  detail::DmlUpdateDurableFileLock lock(root+"/locators.v2");
  if(!lock.ok()) throw std::runtime_error("mga_relation_locator_lock_failed");
  auto locators=LoadMgaRelationLocators(root);
  auto found=locators.find(id);
  if(found==locators.end()) {
    const auto ordinal=static_cast<std::uint64_t>(locators.size())+1;
    found=locators.emplace(id,ordinal).first;
    StoreMgaRelationLocators(root,locators);
  }
  return root+"/relation-"+std::to_string(found->second);
}
inline std::string MgaScopedRelationPath(const EngineRequestContext& context,
    const EngineUuid& id, std::string_view suffix, bool allocate=false) {
  auto base=MgaScopedRelationBasePath(context,id,allocate);
  return base.empty()?std::string{}:base+std::string(suffix);
}
} // namespace scratchbird::engine::internal_api
