// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml_delete_io_fault_fixture.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using delete_io_fixture::Fault;
Fault selected = Fault::none;
std::string database;
std::string directory;
bool hit = false, publication_renamed = false;
bool Named(int fd, std::string_view* path, char (&buffer)[4096]) {
  char link[64]; std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  const auto size = ::readlink(link, buffer, sizeof(buffer));
  if (size < 0 || static_cast<std::size_t>(size) >= sizeof(buffer)) return false;
  buffer[size] = '\0';
  *path = std::string_view(buffer, size); return true;
}
bool NativeKind(int fd, const char* path, unsigned char expected) {
  struct stat state{};
  struct stat inspected{};
  unsigned char frame[16]{};
  if (::fstat(fd, &state) != 0 || state.st_size < 124) return false;
  const int reader = ::open(path, O_RDONLY | O_CLOEXEC);
  if (reader < 0) return false;
  const bool read = ::fstat(reader, &inspected) == 0 && inspected.st_dev == state.st_dev &&
      inspected.st_ino == state.st_ino &&
      ::pread(reader, frame, sizeof(frame), state.st_size - 124) == sizeof(frame);
  ::close(reader);
  if (!read) return false;
  return std::memcmp(frame, "\x89SBSP2\r\n", 8) == 0 &&
      frame[8] == 124 && frame[9] == 0 && frame[10] == 0 && frame[11] == 0 &&
      frame[12] == expected && frame[13] == 1;
}
int Fail() { selected = Fault::none; hit = true; errno = EIO; return -1; }
}
namespace delete_io_fixture {
void Arm(Fault value, std::string_view path) {
  database = path; directory = database + ".sb.mga_delete_operations.v1";
  hit = publication_renamed = false; selected = value;
}
bool Disarm() { selected = Fault::none; return hit; }
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  char buffer[4096]; std::string_view path;
  if (selected != Fault::none && Named(fd, &path, buffer)) {
    const bool marker = path.starts_with(database) && path.substr(database.size()) == ".sb.mga_savepoints";
    if ((marker && selected == Fault::native_create_sync && NativeKind(fd, buffer, 1)) ||
        (marker && selected == Fault::native_release_sync && NativeKind(fd, buffer, 2)) ||
        (selected == Fault::publication_directory_sync && publication_renamed && path == directory)) {
      // Lose the acknowledgement after the real fence: recovery must inspect
      // durable authority, not assume the failed call means nothing happened.
      if (__real_fsync(fd) != 0) return -1;
      return Fail();
    }
  }
  return __real_fsync(fd);
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
  const std::string_view source(from);
  const bool publication = source.starts_with(database) && source.ends_with(".publication");
  if (publication && selected == Fault::publication_rename) return Fail();
  const auto result = __real_rename(from, to);
  if (publication && result == 0 && selected == Fault::publication_directory_sync) publication_renamed = true;
  return result;
}
