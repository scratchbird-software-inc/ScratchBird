// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Include in exactly ONE translation unit of a test executable (register C9).
//
// Counts every open of one watched database file and of its "<db>.sb.*"
// sidecars, from any library: the executable's open/open64/openat/openat64
// and fopen/fopen64 interpose libc's and forward to the real functions.
// libstdc++'s std::ifstream opens through fopen, whose own internal open is
// not interposable, so fopen must be counted separately. SB_C9_TRACE_OPENS=1 prints each path opened
// while a file is watched.
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

#include <dlfcn.h>
#include <fcntl.h>

namespace scratchbird::tests::path_open_counter {

struct State {
  std::mutex mutex;
  std::string watched;
  bool tracing = false;
  std::atomic<std::uint64_t> file_opens{0};
  std::atomic<std::uint64_t> sidecar_opens{0};
};

inline State& Get() {
  static State state;
  return state;
}

inline void Watch(const std::string& path) {
  auto& state = Get();
  std::lock_guard<std::mutex> guard(state.mutex);
  state.watched = path;
  state.tracing = std::getenv("SB_C9_TRACE_OPENS") != nullptr;
}

inline std::uint64_t FileOpens() { return Get().file_opens.load(); }
inline std::uint64_t SidecarOpens() { return Get().sidecar_opens.load(); }

inline void Observe(const char* path) {
  if (path == nullptr) return;
  auto& state = Get();
  std::lock_guard<std::mutex> guard(state.mutex);
  if (state.watched.empty()) return;
  if (state.tracing) std::cerr << "c9_open " << path << '\n';
  if (state.watched == path) {
    state.file_opens.fetch_add(1);
  } else if (std::strncmp(path, state.watched.c_str(), state.watched.size()) == 0 &&
             std::strncmp(path + state.watched.size(), ".sb.", 4) == 0) {
    state.sidecar_opens.fetch_add(1);
  }
}

template <class Fn>
Fn Next(const char* name) {
  return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

inline mode_t ModeArgument(int flags, va_list args) {
  return (flags & (O_CREAT | O_TMPFILE)) ? static_cast<mode_t>(va_arg(args, mode_t)) : 0;
}

}  // namespace scratchbird::tests::path_open_counter

extern "C" int open(const char* path, int flags, ...) {
  va_list args; va_start(args, flags);
  const mode_t mode = scratchbird::tests::path_open_counter::ModeArgument(flags, args);
  va_end(args);
  scratchbird::tests::path_open_counter::Observe(path);
  static const auto real =
      scratchbird::tests::path_open_counter::Next<int (*)(const char*, int, ...)>("open");
  return real(path, flags, mode);
}
extern "C" int open64(const char* path, int flags, ...) {
  va_list args; va_start(args, flags);
  const mode_t mode = scratchbird::tests::path_open_counter::ModeArgument(flags, args);
  va_end(args);
  scratchbird::tests::path_open_counter::Observe(path);
  static const auto real =
      scratchbird::tests::path_open_counter::Next<int (*)(const char*, int, ...)>("open64");
  return real(path, flags, mode);
}
extern "C" int openat(int dir, const char* path, int flags, ...) {
  va_list args; va_start(args, flags);
  const mode_t mode = scratchbird::tests::path_open_counter::ModeArgument(flags, args);
  va_end(args);
  scratchbird::tests::path_open_counter::Observe(path);
  static const auto real =
      scratchbird::tests::path_open_counter::Next<int (*)(int, const char*, int, ...)>("openat");
  return real(dir, path, flags, mode);
}
extern "C" int openat64(int dir, const char* path, int flags, ...) {
  va_list args; va_start(args, flags);
  const mode_t mode = scratchbird::tests::path_open_counter::ModeArgument(flags, args);
  va_end(args);
  scratchbird::tests::path_open_counter::Observe(path);
  static const auto real =
      scratchbird::tests::path_open_counter::Next<int (*)(int, const char*, int, ...)>("openat64");
  return real(dir, path, flags, mode);
}
extern "C" FILE* fopen(const char* path, const char* mode) {
  scratchbird::tests::path_open_counter::Observe(path);
  static const auto real =
      scratchbird::tests::path_open_counter::Next<FILE* (*)(const char*, const char*)>("fopen");
  return real(path, mode);
}
extern "C" FILE* fopen64(const char* path, const char* mode) {
  scratchbird::tests::path_open_counter::Observe(path);
  static const auto real =
      scratchbird::tests::path_open_counter::Next<FILE* (*)(const char*, const char*)>("fopen64");
  return real(path, mode);
}
