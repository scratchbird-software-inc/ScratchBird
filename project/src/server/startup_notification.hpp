// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_startup_notification.hpp"
#include "runtime_platform.hpp"
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scratchbird::server {
// A private launch notification, never database or client authority. The
// sender owns this endpoint and closes it on every startup failure. Only the
// actual IPC-ready callback may send the opened database's binary identity.
class StartupNotification final {
 public:
  StartupNotification()=default;
  StartupNotification(const StartupNotification&)=delete;
  StartupNotification& operator=(const StartupNotification&)=delete;
  ~StartupNotification(){Close();}
  bool Adopt(int descriptor) noexcept {
#ifndef _WIN32
    if(fd_>=0||descriptor<3)return false;
    int kind=0;socklen_t length=sizeof(kind);sockaddr_storage peer{};socklen_t peer_length=sizeof(peer);
    if(::getsockopt(descriptor,SOL_SOCKET,SO_TYPE,&kind,&length)!=0||kind!=SOCK_STREAM||
       ::getpeername(descriptor,reinterpret_cast<sockaddr*>(&peer),&peer_length)!=0||peer.ss_family!=AF_UNIX)
      return false;
    const auto flags=::fcntl(descriptor,F_GETFD);
    if(flags<0||::fcntl(descriptor,F_SETFD,flags|FD_CLOEXEC)<0)return false;
#if defined(SO_NOSIGPIPE)
    int enabled=1;if(::setsockopt(descriptor,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled))!=0)return false;
#endif
    fd_=descriptor;return true;
#else
    (void)descriptor;return false;
#endif
  }
  bool Send(const core::platform::Uuid& database) noexcept {
#ifndef _WIN32
    if(fd_<0||database.is_nil())return false;
    const auto packet=core::platform::EncodeDatabaseStartupNotification(database.bytes);
    std::size_t sent=0;
    while(sent<packet.size()){
#ifdef MSG_NOSIGNAL
      constexpr int flags=MSG_NOSIGNAL;
#else
      constexpr int flags=0;
#endif
      const auto count=::send(fd_,packet.data()+sent,packet.size()-sent,flags);
      if(count<0&&errno==EINTR)continue;
      if(count<=0){Close();return false;}
      sent+=static_cast<std::size_t>(count);
    }
    Close();return true;
#else
    (void)database;return false;
#endif
  }
 private:
  void Close() noexcept {
#ifndef _WIN32
    if(fd_>=0){::close(fd_);fd_=-1;}
#endif
  }
  int fd_=-1;
};
} // namespace scratchbird::server
