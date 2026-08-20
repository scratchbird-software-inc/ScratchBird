#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using CursorFetchUuid=std::array<std::uint8_t,16>;using CursorFetchSha=std::array<std::uint8_t,32>;
struct SblrCursorFetchOperandV1{CursorFetchUuid cursor{},plan{},row_shape{},transaction{},session{};CursorFetchSha cursor_evidence{};std::uint64_t cursor_generation=0,plan_generation=0,row_shape_generation=0,position_generation=0,availability_generation=0;std::uint32_t maximum_rows=0;};
struct SblrCursorFetchResultV1{CursorFetchUuid cursor{};CursorFetchSha row_batch_sha{},refreshed_cursor_evidence{},result_evidence{};std::uint64_t cursor_generation=0,prior_position_generation=0,resulting_position_generation=0,availability_generation=0;std::uint32_t returned_rows=0;std::uint8_t eof=0;};
std::vector<std::uint8_t> EncodeSblrCursorFetchOperandV1(const SblrCursorFetchOperandV1&);
bool DecodeSblrCursorFetchOperandV1(const std::uint8_t*,std::size_t,SblrCursorFetchOperandV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrCursorFetchResultV1(const SblrCursorFetchResultV1&);
bool DecodeSblrCursorFetchResultV1(const std::uint8_t*,std::size_t,SblrCursorFetchResultV1*,std::string*);
}
