// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "grpc_unary_worker_session.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::compatibility {
namespace {

constexpr std::string_view kHttp2Preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::uint8_t kFrameData = 0x00;
constexpr std::uint8_t kFrameHeaders = 0x01;
constexpr std::uint8_t kFrameSettings = 0x04;
constexpr std::uint8_t kFramePing = 0x06;
constexpr std::uint8_t kFrameGoaway = 0x07;
constexpr std::uint8_t kFrameWindowUpdate = 0x08;
constexpr std::uint8_t kFlagEndStream = 0x01;
constexpr std::uint8_t kFlagAck = 0x01;
constexpr std::uint8_t kFlagEndHeaders = 0x04;
constexpr std::size_t kMaxFramePayload = 16 * 1024 * 1024;

struct Frame {
  std::uint8_t type{0};
  std::uint8_t flags{0};
  std::uint32_t stream_id{0};
  std::vector<std::uint8_t> payload;
};

#ifndef _WIN32
bool ReadExact(int fd, void* out, std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(out);
  std::size_t read_total = 0;
  while (read_total < size) {
    const auto rc = ::read(fd, bytes + read_total, size - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WriteAll(int fd, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const auto rc = ::write(fd, bytes + written, size - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}
#endif

std::uint32_t ReadU24BE(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 16) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         static_cast<std::uint32_t>(data[2]);
}

std::uint32_t ReadU31BE(const std::uint8_t* data) {
  return ((static_cast<std::uint32_t>(data[0]) & 0x7f) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

void AppendU24BE(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void AppendU32BE(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

bool ReadFrame(int fd, Frame* frame) {
  std::uint8_t header[9] = {};
  if (!ReadExact(fd, header, sizeof(header))) return false;
  const auto length = ReadU24BE(header);
  if (length > kMaxFramePayload) return false;
  frame->type = header[3];
  frame->flags = header[4];
  frame->stream_id = ReadU31BE(header + 5);
  frame->payload.assign(length, 0);
  return length == 0 || ReadExact(fd, frame->payload.data(), frame->payload.size());
}

bool WriteFrame(int fd,
                std::uint8_t type,
                std::uint8_t flags,
                std::uint32_t stream_id,
                const std::vector<std::uint8_t>& payload) {
  if (payload.size() > 0xffffff) return false;
  std::vector<std::uint8_t> out;
  out.reserve(9 + payload.size());
  AppendU24BE(&out, static_cast<std::uint32_t>(payload.size()));
  out.push_back(type);
  out.push_back(flags);
  AppendU32BE(&out, stream_id & 0x7fffffffU);
  out.insert(out.end(), payload.begin(), payload.end());
  return WriteAll(fd, out.data(), out.size());
}

void AppendHpackString(std::vector<std::uint8_t>* out, std::string_view value) {
  if (value.size() < 0x80) {
    out->push_back(static_cast<std::uint8_t>(value.size()));
  } else {
    std::size_t remaining = value.size();
    out->push_back(static_cast<std::uint8_t>(0x80 | (remaining & 0x7f)));
    remaining >>= 7;
    while (remaining > 0) {
      out->push_back(static_cast<std::uint8_t>(remaining & 0x7f));
      remaining >>= 7;
    }
  }
  out->insert(out->end(), value.begin(), value.end());
}

void AppendLiteralHeader(std::vector<std::uint8_t>* out,
                         std::string_view name,
                         std::string_view value) {
  out->push_back(0x00);
  AppendHpackString(out, name);
  AppendHpackString(out, value);
}

std::vector<std::uint8_t> ResponseHeaders() {
  std::vector<std::uint8_t> out;
  out.push_back(0x88);
  AppendLiteralHeader(&out, "content-type", "application/grpc");
  return out;
}

std::vector<std::uint8_t> ResponseTrailers(std::string_view grpc_status,
                                           std::string_view grpc_message) {
  std::vector<std::uint8_t> out;
  AppendLiteralHeader(&out, "grpc-status", grpc_status);
  if (!grpc_message.empty()) AppendLiteralHeader(&out, "grpc-message", grpc_message);
  return out;
}

std::vector<std::uint8_t> GrpcMessage(std::string_view protobuf_payload) {
  std::vector<std::uint8_t> out;
  out.reserve(5 + protobuf_payload.size());
  out.push_back(0x00);
  AppendU32BE(&out, static_cast<std::uint32_t>(protobuf_payload.size()));
  out.insert(out.end(), protobuf_payload.begin(), protobuf_payload.end());
  return out;
}

std::string ExtractGrpcPayload(const std::vector<std::uint8_t>& data_frames) {
  if (data_frames.size() < 5 || data_frames[0] != 0) return {};
  const auto len = (static_cast<std::uint32_t>(data_frames[1]) << 24) |
                   (static_cast<std::uint32_t>(data_frames[2]) << 16) |
                   (static_cast<std::uint32_t>(data_frames[3]) << 8) |
                   static_cast<std::uint32_t>(data_frames[4]);
  if (data_frames.size() < 5 + len) return {};
  return std::string(reinterpret_cast<const char*>(data_frames.data() + 5), len);
}

bool SendGrpcResponse(int fd,
                      std::uint32_t stream_id,
                      const std::vector<std::uint8_t>& header_block,
                      const std::vector<std::uint8_t>& data_frames,
                      GrpcUnaryResponder responder) {
  const std::string headers(reinterpret_cast<const char*>(header_block.data()), header_block.size());
  const auto request_payload = ExtractGrpcPayload(data_frames);
  GrpcUnaryResponse response;
  if (responder != nullptr) response = responder(headers, request_payload);
  if (response.grpc_status.empty()) response.grpc_status = "12";
  if (response.grpc_status != "0") {
    if (!WriteFrame(fd, kFrameHeaders, kFlagEndHeaders, stream_id, ResponseHeaders())) return false;
    return WriteFrame(fd, kFrameHeaders, kFlagEndHeaders | kFlagEndStream, stream_id,
                      ResponseTrailers(response.grpc_status, response.grpc_message));
  }
  if (!WriteFrame(fd, kFrameHeaders, kFlagEndHeaders, stream_id, ResponseHeaders())) return false;
  if (!WriteFrame(fd, kFrameData, 0, stream_id, GrpcMessage(response.payload))) return false;
  return WriteFrame(fd, kFrameHeaders, kFlagEndHeaders | kFlagEndStream, stream_id,
                    ResponseTrailers("0", ""));
}

} // namespace

int ServeGrpcUnaryWorkerSession(int fd, GrpcUnaryResponder responder) {
#ifdef _WIN32
  (void)fd;
  (void)responder;
  return 1;
#else
  std::vector<std::uint8_t> preface(kHttp2Preface.size());
  if (!ReadExact(fd, preface.data(), preface.size())) return 1;
  if (std::string_view(reinterpret_cast<const char*>(preface.data()), preface.size()) !=
      kHttp2Preface) {
    return 1;
  }
  if (!WriteFrame(fd, kFrameSettings, 0, 0, {})) return 1;

  std::uint32_t pending_stream = 0;
  std::vector<std::uint8_t> pending_headers;
  std::vector<std::uint8_t> pending_data;
  for (;;) {
    Frame frame;
    if (!ReadFrame(fd, &frame)) return 0;
    if (frame.type == kFrameSettings) {
      if ((frame.flags & kFlagAck) == 0 &&
          !WriteFrame(fd, kFrameSettings, kFlagAck, 0, {})) {
        return 1;
      }
      continue;
    }
    if (frame.type == kFramePing && frame.payload.size() == 8) {
      if (!WriteFrame(fd, kFramePing, kFlagAck, 0, frame.payload)) return 1;
      continue;
    }
    if (frame.type == kFrameWindowUpdate) continue;
    if (frame.type == kFrameGoaway) return 0;
    if (frame.type == kFrameHeaders && frame.stream_id != 0) {
      pending_stream = frame.stream_id;
      pending_headers = frame.payload;
      pending_data.clear();
      if ((frame.flags & kFlagEndStream) != 0) {
        return SendGrpcResponse(fd, pending_stream, pending_headers, pending_data, responder) ? 0 : 1;
      }
      continue;
    }
    if (frame.type == kFrameData && frame.stream_id != 0) {
      if (pending_stream == 0) pending_stream = frame.stream_id;
      pending_data.insert(pending_data.end(), frame.payload.begin(), frame.payload.end());
      if ((frame.flags & kFlagEndStream) != 0) {
        const auto stream_id = pending_stream == frame.stream_id ? pending_stream : frame.stream_id;
        if (!SendGrpcResponse(fd, stream_id, pending_headers, pending_data, responder)) return 1;
        pending_stream = 0;
        pending_headers.clear();
        pending_data.clear();
      }
      continue;
    }
  }
#endif
}

} // namespace scratchbird::parser::compatibility
