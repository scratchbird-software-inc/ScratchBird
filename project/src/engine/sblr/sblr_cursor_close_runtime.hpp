#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using CursorCloseUuid=std::array<std::uint8_t,16>;using CursorCloseSha=std::array<std::uint8_t,32>;
struct SblrCursorCloseOperandV1{CursorCloseUuid cursor{},plan{},row_shape{},transaction{},session{};CursorCloseSha cursor_evidence{};std::uint64_t cursor_generation=0,plan_generation=0,row_shape_generation=0,position_generation=0,availability_generation=0;std::uint8_t close_reason=0;};
struct SblrCursorCloseResultV1{CursorCloseUuid cursor{};CursorCloseSha cleanup_evidence{},result_evidence{};std::uint64_t cursor_generation=0,final_position_generation=0,availability_generation=0;std::uint8_t final_state=0,close_reason=0;};
std::vector<std::uint8_t> EncodeSblrCursorCloseOperandV1(const SblrCursorCloseOperandV1&);
bool DecodeSblrCursorCloseOperandV1(const std::uint8_t*,std::size_t,SblrCursorCloseOperandV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrCursorCloseResultV1(const SblrCursorCloseResultV1&);
bool DecodeSblrCursorCloseResultV1(const std::uint8_t*,std::size_t,SblrCursorCloseResultV1*,std::string*);
}
