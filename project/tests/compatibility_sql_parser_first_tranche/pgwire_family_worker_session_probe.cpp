// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#if defined(SB_TEST_COCKROACHDB)
#include "cockroachdb_worker_session.hpp"
#elif defined(SB_TEST_YUGABYTEDB)
#include "yugabytedb_worker_session.hpp"
#elif defined(SB_TEST_XTDB)
#include "xtdb_worker_session.hpp"
#else
#error "A parser-family test definition is required"
#endif

#include <array>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#if defined(SB_TEST_COCKROACHDB)
constexpr std::string_view kExpectedVersion = "scratchbird-cockroachdb";
int Serve(int fd) {
  return scratchbird::parser::cockroachdb::ServeCockroachdbWorkerSession(fd);
}
#elif defined(SB_TEST_YUGABYTEDB)
constexpr std::string_view kExpectedVersion = "scratchbird-yugabytedb";
int Serve(int fd) {
  return scratchbird::parser::yugabytedb::ServeYugabytedbWorkerSession(fd);
}
#elif defined(SB_TEST_XTDB)
constexpr std::string_view kExpectedVersion = "scratchbird-xtdb";
int Serve(int fd) {
  return scratchbird::parser::xtdb::ServeXtdbWorkerSession(fd);
}
#endif

std::uint32_t ReadBe32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

void AddBe32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

#ifndef _WIN32
bool WriteAll(int fd, const void* input, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(input);
  std::size_t offset = 0;
  while (offset < size) {
    const auto rc = ::write(fd, bytes + offset, size - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
    } else if (rc < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool ReadExact(int fd, void* output, std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(output);
  std::size_t offset = 0;
  while (offset < size) {
    const auto rc = ::read(fd, bytes + offset, size - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
    } else if (rc < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool SendStartup(int fd) {
  const std::array<std::uint8_t, 8> startup{{0, 0, 0, 8, 0, 3, 0, 0}};
  return WriteAll(fd, startup.data(), startup.size());
}

bool SendTyped(int fd, char type, std::string_view body) {
  std::vector<std::uint8_t> frame;
  frame.push_back(static_cast<std::uint8_t>(type));
  AddBe32(&frame, static_cast<std::uint32_t>(body.size() + 4));
  frame.insert(frame.end(), body.begin(), body.end());
  return WriteAll(fd, frame.data(), frame.size());
}

bool ReadTyped(int fd, char* type, std::vector<std::uint8_t>* body) {
  std::uint8_t type_byte = 0;
  std::uint8_t length[4] = {};
  if (!ReadExact(fd, &type_byte, 1) || !ReadExact(fd, length, sizeof(length))) return false;
  const auto byte_count = ReadBe32(length);
  if (byte_count < 4 || byte_count > 1024 * 1024) return false;
  body->assign(byte_count - 4, 0);
  if (!body->empty() && !ReadExact(fd, body->data(), body->size())) return false;
  *type = static_cast<char>(type_byte);
  return true;
}

bool ReadUntilReady(int fd, std::string* transcript) {
  for (;;) {
    char type = 0;
    std::vector<std::uint8_t> body;
    if (!ReadTyped(fd, &type, &body)) return false;
    transcript->push_back(type);
    transcript->append(reinterpret_cast<const char*>(body.data()), body.size());
    if (type == 'Z') return true;
  }
}
#endif

} // namespace

int main() {
#ifdef _WIN32
  return 0;
#else
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    std::cerr << "socketpair failed\n";
    return 1;
  }

  int worker_status = -1;
  std::thread worker([&] {
    worker_status = Serve(sockets[1]);
    ::close(sockets[1]);
  });

  std::string startup_transcript;
  std::string query_transcript;
  const std::string query_body("select 1\0", 9);
  const bool passed =
      SendStartup(sockets[0]) && ReadUntilReady(sockets[0], &startup_transcript) &&
      startup_transcript.find(kExpectedVersion) != std::string::npos &&
      SendTyped(sockets[0], 'Q', query_body) &&
      ReadUntilReady(sockets[0], &query_transcript) &&
      query_transcript.find('D') != std::string::npos &&
      query_transcript.find("SELECT 1") != std::string::npos &&
      SendTyped(sockets[0], 'X', {});
  ::close(sockets[0]);
  worker.join();

  if (!passed || worker_status != 0) {
    std::cerr << "family-owned wire worker session probe failed\n";
    return 1;
  }
  return 0;
#endif
}
