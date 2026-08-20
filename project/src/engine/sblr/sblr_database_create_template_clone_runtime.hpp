#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { using TemplateCloneUuid=std::array<std::uint8_t,16>; using TemplateCloneSha=std::array<std::uint8_t,32>;
struct SblrDatabaseCreateTemplateCloneRequestV1{TemplateCloneUuid receipt{};std::uint64_t occurrence=0;std::uint32_t template_clone_occurrence=0;};
struct SblrDatabaseCreateTemplateCloneDescriptorV1{std::array<std::uint8_t,400> body{};TemplateCloneSha evidence{};std::uint64_t availability=0;};
struct SblrDatabaseCreateTemplateCloneResultV1{std::array<std::uint8_t,240> body{};TemplateCloneSha evidence{};std::uint64_t availability=0;TemplateCloneUuid publication_barrier{};};
std::vector<std::uint8_t> EncodeSblrDatabaseCreateTemplateCloneRequestV1(const SblrDatabaseCreateTemplateCloneRequestV1&); bool DecodeSblrDatabaseCreateTemplateCloneRequestV1(const std::uint8_t*,std::size_t,SblrDatabaseCreateTemplateCloneRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDatabaseCreateTemplateCloneDescriptorV1(const SblrDatabaseCreateTemplateCloneDescriptorV1&,bool); bool DecodeSblrDatabaseCreateTemplateCloneDescriptorV1(const std::uint8_t*,std::size_t,SblrDatabaseCreateTemplateCloneDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDatabaseCreateTemplateCloneResultV1(const SblrDatabaseCreateTemplateCloneResultV1&); bool DecodeSblrDatabaseCreateTemplateCloneResultV1(const std::uint8_t*,std::size_t,SblrDatabaseCreateTemplateCloneResultV1*,std::string*);
}
