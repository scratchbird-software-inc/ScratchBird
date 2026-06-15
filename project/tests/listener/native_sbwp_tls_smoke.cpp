// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct Frame {
  std::uint8_t type{0};
  std::vector<std::uint8_t> payload;
};

std::filesystem::path MakeTempDir(const std::filesystem::path& root) {
  std::error_code ec;
  auto base = root;
  if (base.string().size() > 40) {
    base = std::filesystem::temp_directory_path(ec) / "sbntls";
    if (ec) return {};
  }
  std::filesystem::create_directories(base, ec);
  if (ec) return {};
  std::string tmpl = (base / "t.XXXXXX").string();
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

int FindFreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

int ConnectLoopback(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool RunProcess(const std::filesystem::path& executable, const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    const std::string exe = executable.string();
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    if (exe.find('/') == std::string::npos) {
      ::execvp(exe.c_str(), argv.data());
    } else {
      ::execv(exe.c_str(), argv.data());
    }
    _exit(127);
  }
  if (pid <= 0) return false;
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool GenerateServerCertificate(const std::filesystem::path& openssl,
                               const std::filesystem::path& cert,
                               const std::filesystem::path& key) {
  const std::vector<std::string> args{
      "req",
      "-x509",
      "-newkey",
      "rsa:2048",
      "-nodes",
      "-sha256",
      "-days",
      "30",
      "-subj",
      "/CN=localhost",
      "-addext",
      "subjectAltName=DNS:localhost,IP:127.0.0.1",
      "-keyout",
      key.string(),
      "-out",
      cert.string(),
  };
  if (!RunProcess(openssl, args)) return false;
  (void)::chmod(key.c_str(), 0600);
  return std::filesystem::is_regular_file(cert) && std::filesystem::is_regular_file(key);
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    out |= static_cast<std::uint32_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

bool WriteAll(SSL* ssl, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const int rc = SSL_write(ssl, data + written, static_cast<int>(size - written));
    if (rc <= 0) return false;
    written += static_cast<std::size_t>(rc);
  }
  return true;
}

bool ReadExact(SSL* ssl, std::uint8_t* data, std::size_t size) {
  std::size_t read = 0;
  while (read < size) {
    const int rc = SSL_read(ssl, data + read, static_cast<int>(size - read));
    if (rc <= 0) return false;
    read += static_cast<std::size_t>(rc);
  }
  return true;
}

bool SendFrame(SSL* ssl, std::uint8_t type, std::uint32_t sequence, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> out;
  out.push_back('S');
  out.push_back('B');
  out.push_back('W');
  out.push_back('P');
  out.push_back(1);
  out.push_back(1);
  out.push_back(type);
  out.push_back(0);
  PutU32(&out, static_cast<std::uint32_t>(payload.size()));
  PutU32(&out, sequence);
  for (int i = 0; i < 16; ++i) out.push_back(0);
  PutU64(&out, 0);
  out.insert(out.end(), payload.begin(), payload.end());
  return WriteAll(ssl, out.data(), out.size());
}

std::optional<Frame> ReadFrame(SSL* ssl) {
  std::vector<std::uint8_t> header(40);
  if (!ReadExact(ssl, header.data(), header.size())) return std::nullopt;
  if (header[0] != 'S' || header[1] != 'B' || header[2] != 'W' || header[3] != 'P') {
    return std::nullopt;
  }
  const std::uint32_t length = ReadU32(header, 8);
  Frame frame;
  frame.type = header[6];
  frame.payload.assign(length, 0);
  if (!frame.payload.empty() && !ReadExact(ssl, frame.payload.data(), frame.payload.size())) {
    return std::nullopt;
  }
  return frame;
}

std::vector<std::uint8_t> StartupPayload() {
  std::vector<std::uint8_t> out;
  out.push_back(1);
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  PutU64(&out, 0);
  out.push_back(0);
  return out;
}

struct ReadyState {
  bool active{false};
  std::uint64_t txn_id{0};
};

ReadyState DecodeReady(const std::vector<std::uint8_t>& payload) {
  Require(payload.size() >= 20, "READY payload too short");
  ReadyState state;
  state.active = payload[0] != 0;
  state.txn_id = ReadU64(payload, 4);
  return state;
}

bool RunSbwpTlsProbe(int port) {
  int fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) return false;

  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  Require(ctx != nullptr, "could not create TLS client context");
  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  SSL* ssl = SSL_new(ctx);
  Require(ssl != nullptr, "could not create TLS client");
  SSL_set_fd(ssl, fd);
  if (SSL_connect(ssl) != 1) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return false;
  }

  std::uint32_t sequence = 0;
  Require(SendFrame(ssl, 0x01, sequence++, StartupPayload()), "STARTUP write failed");
  auto auth = ReadFrame(ssl);
  Require(auth && auth->type == 0x41, "AUTH_OK not received");
  auto ready = ReadFrame(ssl);
  Require(ready && ready->type == 0x43, "initial READY not received");
  const auto initial_state = DecodeReady(ready->payload);
  Require(initial_state.active, "initial READY did not publish an active MGA transaction");
  Require(initial_state.txn_id != 0, "initial READY did not return an MGA transaction id");

  Require(SendFrame(ssl, 0x15, sequence++, {}), "TXN_BEGIN write failed");
  auto begin_ready = ReadFrame(ssl);
  Require(begin_ready && begin_ready->type == 0x43, "READY not received after TXN_BEGIN");
  const auto begin_state = DecodeReady(begin_ready->payload);
  Require(begin_state.active, "MGA transaction was not marked active");
  Require(begin_state.txn_id != 0, "MGA transaction id was not returned");

  Require(SendFrame(ssl, 0x17, sequence++, {}), "TXN_ROLLBACK write failed");
  auto rollback_ready = ReadFrame(ssl);
  Require(rollback_ready && rollback_ready->type == 0x43, "READY not received after TXN_ROLLBACK");
  const auto rollback_state = DecodeReady(rollback_ready->payload);
  Require(rollback_state.active, "MGA transaction replacement was not active after rollback");
  Require(rollback_state.txn_id != 0, "MGA replacement transaction id was not returned after rollback");
  Require(rollback_state.txn_id != begin_state.txn_id,
          "MGA rollback did not advance to a replacement transaction");

  (void)SendFrame(ssl, 0x0c, sequence++, {});
  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  ::close(fd);
  return true;
}

bool RunConcurrentSbwpTlsProbes(int port, int count) {
  std::atomic<int> passed{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([port, &passed] {
      if (RunSbwpTlsProbe(port)) {
        passed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  return passed.load(std::memory_order_relaxed) == count;
}

bool PlaintextRefused(int port) {
  int fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) return false;
  const char probe[] = "SBWP";
  (void)::send(fd, probe, sizeof(probe) - 1, 0);
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN | POLLHUP | POLLERR;
  const int rc = ::poll(&pfd, 1, 3000);
  if (rc <= 0) {
    ::close(fd);
    return false;
  }
  char buffer[8]{};
  const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
  ::close(fd);
  return got <= 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: sb_listener_native_sbwp_tls_smoke <sb_listener> <sbp_native> <openssl> <fixture-root>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path listener = argv[1];
  const std::filesystem::path parser = argv[2];
  const std::filesystem::path openssl = argv[3];
  const std::filesystem::path fixture_root = argv[4];
  const auto work = MakeTempDir(fixture_root);
  Require(!work.empty(), "could not create temp dir");
  const int port = FindFreePort();
  Require(port > 0, "could not allocate port");

  const auto cert = work / "server.crt";
  const auto key = work / "server.key";
  Require(GenerateServerCertificate(openssl, cert, key), "could not generate server TLS fixture");

  const auto control_dir = work / "control";
  const auto runtime_dir = work / "runtime";
  const auto stdout_path = work / "listener.out";
  const auto stderr_path = work / "listener.err";

  const pid_t pid = ::fork();
  if (pid == 0) {
    int out = ::creat(stdout_path.c_str(), 0600);
    int err = ::creat(stderr_path.c_str(), 0600);
    if (out >= 0) {
      ::dup2(out, STDOUT_FILENO);
      ::close(out);
    }
    if (err >= 0) {
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }
    const std::string parser_arg = "--parser-executable=" + parser.string();
    const std::string control_arg = "--control-dir=" + control_dir.string();
    const std::string runtime_arg = "--runtime-dir=" + runtime_dir.string();
    const std::string port_arg = "--port=" + std::to_string(port);
    const std::string cert_arg = "--tls-cert-file=" + cert.string();
    const std::string key_arg = "--tls-key-file=" + key.string();
    const std::string database_arg =
        "--database-selector=dev_bootstrap_path:" + (work / "native_tls.sbdb").string();
    const std::string endpoint_arg =
        "--server-endpoint=unix:" + (work / "native_tls.sbps.sock").string();
    ::execl(listener.c_str(),
            listener.c_str(),
            "--foreground",
            "--protocol-family=native",
            "--listener-profile=native",
            "--bundle-contract-id=bundle.default@1",
            "--parser-package=sbp_native",
            "--dialect-profile-uuid=sbwp-v1",
            database_arg.c_str(),
            endpoint_arg.c_str(),
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--tls-required=true",
            cert_arg.c_str(),
            key_arg.c_str(),
            "--warm-pool-min=1",
            "--warm-pool-max=2",
            "--dbbt-key-source=test_builtin",
            "--allow-test-dbbt-builtin=true",
            nullptr);
    _exit(127);
  }
  Require(pid > 0, "could not fork listener");

  bool ok = PlaintextRefused(port);
  for (int i = 0; i < 3; ++i) {
    ok = ok && RunSbwpTlsProbe(port);
    if (!ok) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ok = ok && RunConcurrentSbwpTlsProbes(port, 4);
  ::kill(pid, SIGTERM);
  int status = 0;
  for (int i = 0; i < 120; ++i) {
    const auto rc = ::waitpid(pid, &status, WNOHANG);
    if (rc == pid) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!ok) {
    std::cerr << "SBWP/TLS probe failed\n";
    std::cerr << ReadFile(stderr_path) << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "native_sbwp_tls_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
