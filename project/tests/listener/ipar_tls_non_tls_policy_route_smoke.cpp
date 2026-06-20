// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
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

std::filesystem::path MakeTempDir(const std::filesystem::path& root) {
  std::error_code ec;
  auto base = root;
  if (base.string().size() > 40) {
    base = std::filesystem::temp_directory_path(ec) / "sbipar";
    if (ec) return {};
  }
  std::filesystem::create_directories(base, ec);
  if (ec) return {};
  std::string tmpl = (base / "ipar-tls-route.XXXXXX").string();
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
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
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

bool ReadLine(int fd, std::string* line) {
  line->clear();
  char ch = 0;
  while (true) {
    const auto rc = ::read(fd, &ch, 1);
    if (rc == 1) {
      if (ch == '\n') return true;
      line->push_back(ch);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
}

bool WriteAll(int fd, const std::string& text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const auto rc = ::write(fd, text.data() + written, text.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool RunProcess(const std::filesystem::path& executable,
                const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
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
      "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "30",
      "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
      "-keyout", key.string(), "-out", cert.string()};
  if (!RunProcess(openssl, args)) return false;
  (void)::chmod(key.c_str(), 0600);
  return std::filesystem::is_regular_file(cert) && std::filesystem::is_regular_file(key);
}

class ListenerProcess {
 public:
  ListenerProcess(std::filesystem::path listener,
                  std::filesystem::path parser,
                  std::filesystem::path work,
                  int port,
                  bool tls_required,
                  std::filesystem::path cert = {},
                  std::filesystem::path key = {})
      : listener_(std::move(listener)),
        parser_(std::move(parser)),
        work_(std::move(work)),
        port_(port),
        tls_required_(tls_required),
        cert_(std::move(cert)),
        key_(std::move(key)) {}

  ~ListenerProcess() { Stop(); }

  void Start() {
    stdout_path_ = work_ / (tls_required_ ? "tls-required.out" : "nontls.out");
    stderr_path_ = work_ / (tls_required_ ? "tls-required.err" : "nontls.err");
    const auto control_dir = work_ / (tls_required_ ? "tls-control" : "nontls-control");
    const auto runtime_dir = work_ / (tls_required_ ? "tls-runtime" : "nontls-runtime");
    pid_ = ::fork();
    if (pid_ == 0) {
      int out = ::creat(stdout_path_.c_str(), 0600);
      int err = ::creat(stderr_path_.c_str(), 0600);
      if (out >= 0) {
        ::dup2(out, STDOUT_FILENO);
        ::close(out);
      }
      if (err >= 0) {
        ::dup2(err, STDERR_FILENO);
        ::close(err);
      }
      const std::string parser_arg = "--parser-executable=" + parser_.string();
      const std::string control_arg = "--control-dir=" + control_dir.string();
      const std::string runtime_arg = "--runtime-dir=" + runtime_dir.string();
      const std::string port_arg = "--port=" + std::to_string(port_);
      const std::string database_arg =
          "--database-selector=dev_bootstrap_path:" +
          (work_ / (tls_required_ ? "tls_required.sbdb" : "nontls_allowed.sbdb")).string();
      const std::string endpoint_arg =
          "--server-endpoint=unix:" +
          (work_ / (tls_required_ ? "tls_required.sbps.sock" : "nontls_allowed.sbps.sock")).string();
      const std::string tls_arg = tls_required_ ? "--tls-required=true" : "--tls-required=false";
      const std::string cert_arg = "--tls-cert-file=" + cert_.string();
      const std::string key_arg = "--tls-key-file=" + key_.string();
      if (tls_required_) {
        ::execl(listener_.c_str(),
                listener_.c_str(),
                "--foreground",
                "--protocol-family=sbsql",
                "--listener-profile=default",
                "--bundle-contract-id=bundle.default@1",
                database_arg.c_str(),
                endpoint_arg.c_str(),
                parser_arg.c_str(),
                control_arg.c_str(),
                runtime_arg.c_str(),
                "--bind-address=127.0.0.1",
                port_arg.c_str(),
                tls_arg.c_str(),
                cert_arg.c_str(),
                key_arg.c_str(),
                "--warm-pool-min=1",
                "--warm-pool-max=2",
                nullptr);
      } else {
        ::execl(listener_.c_str(),
                listener_.c_str(),
                "--foreground",
                "--protocol-family=sbsql",
                "--listener-profile=ipar-test-nontls",
                "--bundle-contract-id=bundle.default@1",
                database_arg.c_str(),
                endpoint_arg.c_str(),
                parser_arg.c_str(),
                control_arg.c_str(),
                runtime_arg.c_str(),
                "--bind-address=127.0.0.1",
                port_arg.c_str(),
                tls_arg.c_str(),
                "--warm-pool-min=1",
                "--warm-pool-max=2",
                nullptr);
      }
      _exit(127);
    }
    Require(pid_ > 0, "could not fork listener");
  }

  void Stop() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);
    int status = 0;
    for (int i = 0; i < 120; ++i) {
      const auto rc = ::waitpid(pid_, &status, WNOHANG);
      if (rc == pid_) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

  const std::filesystem::path& stderr_path() const { return stderr_path_; }

 private:
  std::filesystem::path listener_;
  std::filesystem::path parser_;
  std::filesystem::path work_;
  int port_{0};
  bool tls_required_{false};
  std::filesystem::path cert_;
  std::filesystem::path key_;
  pid_t pid_{-1};
  std::filesystem::path stdout_path_;
  std::filesystem::path stderr_path_;
};

bool PlaintextRefused(int port) {
  int fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) return false;
  (void)WriteAll(fd, "SBWP");
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN | POLLHUP | POLLERR;
  const int rc = ::poll(&pfd, 1, 3000);
  if (rc <= 0) {
    ::close(fd);
    return true;
  }
  char buffer[64]{};
  const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
  ::close(fd);
  if (got <= 0) return true;
  const std::string response(buffer, buffer + got);
  return response.find("ScratchBird dummy parser ready") == std::string::npos;
}

bool PlaintextAcceptedByDummyParser(int port) {
  int fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) return false;
  std::string line;
  const bool greeting_ok =
      ReadLine(fd, &line) && line == "ScratchBird dummy parser ready";
  const bool echo_ok =
      greeting_ok && WriteAll(fd, "ipar-nontls-route-proof\n") &&
      ReadLine(fd, &line) && line == "ipar-nontls-route-proof";
  ::close(fd);
  return echo_ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: sb_listener_ipar_tls_non_tls_policy_route_smoke "
                 "<sb_listener> <sb_parser_dummy> <SBParser> <openssl> <fixture-root>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path listener = argv[1];
  const std::filesystem::path dummy_parser = argv[2];
  const std::filesystem::path sbsql_parser = argv[3];
  const std::filesystem::path openssl = argv[4];
  const std::filesystem::path fixture_root = argv[5];
  const auto work = MakeTempDir(fixture_root);
  Require(!work.empty(), "could not create fixture directory");

  const int tls_port = FindFreePort();
  const int nontls_port = FindFreePort();
  Require(tls_port > 0 && nontls_port > 0 && tls_port != nontls_port,
          "could not allocate listener ports");

  const auto cert = work / "server.crt";
  const auto key = work / "server.key";
  Require(GenerateServerCertificate(openssl, cert, key),
          "could not generate TLS fixture certificate");

  ListenerProcess tls_listener(listener, sbsql_parser, work, tls_port, true, cert, key);
  tls_listener.Start();
  Require(PlaintextRefused(tls_port),
          "TLS-required listener accepted plaintext sslmode=disable probe: " +
              ReadFile(tls_listener.stderr_path()));
  tls_listener.Stop();

  ListenerProcess nontls_listener(listener, dummy_parser, work, nontls_port, false);
  nontls_listener.Start();
  Require(PlaintextAcceptedByDummyParser(nontls_port),
          "explicit test non-TLS listener did not admit plaintext route: " +
              ReadFile(nontls_listener.stderr_path()));
  nontls_listener.Stop();

  std::cout << "ipar_tls_non_tls_policy_route_smoke=passed work=" << work
            << " tls_required_plaintext_refused=true"
            << " explicit_test_nontls_plaintext_accepted=true\n";
  return EXIT_SUCCESS;
}
