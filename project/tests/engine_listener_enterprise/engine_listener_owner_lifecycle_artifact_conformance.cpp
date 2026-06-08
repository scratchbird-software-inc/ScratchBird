// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_diagnostics.hpp"
#include "listener_socket_identity.hpp"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_eler067.XXXXXX";
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

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc | std::ios::binary);
  out << text;
}

std::filesystem::path ArtifactLockPath(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".lock");
}

int HoldArtifactLock(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  const auto lock_path = ArtifactLockPath(path);
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  Require(fd >= 0, "could not open external artifact lock");
  Require(::flock(fd, LOCK_EX | LOCK_NB) == 0, "could not hold external artifact lock");
  return fd;
}

void ReleaseArtifactLock(int fd) {
  if (fd >= 0) {
    ::flock(fd, LOCK_UN);
    ::close(fd);
  }
}

std::string HexDigestPrefix(const scratchbird::listener::proto::Sha256Digest& digest,
                            std::size_t bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes && i < digest.size(); ++i) {
    out.push_back(hex[(digest[i] >> 4u) & 0x0fu]);
    out.push_back(hex[digest[i] & 0x0fu]);
  }
  return out;
}

bool SignedArtifactValid(const std::filesystem::path& path, std::string* body_out) {
  const auto text = ReadFile(path);
  std::istringstream in(text);
  std::ostringstream body;
  std::string signature;
  std::string line;
  while (std::getline(in, line)) {
    constexpr std::string_view prefix = "signature_sha256_128=";
    if (line.rfind(prefix, 0) == 0) {
      signature = line.substr(prefix.size());
      continue;
    }
    body << line << '\n';
  }
  const auto body_text = body.str();
  scratchbird::listener::proto::Bytes bytes(body_text.begin(), body_text.end());
  const auto expected = HexDigestPrefix(scratchbird::listener::proto::Sha256(bytes), 16);
  if (body_out != nullptr) *body_out = body_text;
  return !signature.empty() && signature == expected;
}

void RequireMode0600(const std::filesystem::path& path, const std::string& label) {
  struct stat st {};
  Require(::stat(path.c_str(), &st) == 0, label + " stat failed");
  Require((st.st_mode & 0777) == 0600, label + " mode is not 0600");
}

void RequireNoTempFiles(const std::filesystem::path& dir) {
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    Require(name.find(".tmp.") == std::string::npos,
            "durable artifact temp file was left behind: " + name);
  }
}

}  // namespace

int main() {
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");

  scratchbird::listener::ListenerConfig config;
  config.protocol_family = "sbsql";
  config.listener_uuid = "listener-eler067";
  config.listener_profile = "enterprise";
  config.database_selector = "dev_bootstrap_path:/tmp/sb_eler067.sbdb";
  config.server_endpoint = "unix:/tmp/sb_eler067.sbps.sock";
  config.control_dir = (work / "control").string();
  config.runtime_dir = (work / "runtime").string();
  config.lifecycle_generation = 67;
  const auto identity = scratchbird::listener::BuildSocketIdentity(config);

  std::string error;
  Require(scratchbird::listener::WriteOwnerToken(identity, &error),
          "initial owner token write failed: " + error);
  Require(std::filesystem::exists(identity.owner_file), "owner token was not created");
  RequireMode0600(identity.owner_file, "owner token");
  std::string owner_body;
  Require(SignedArtifactValid(identity.owner_file, &owner_body),
          "owner token signature was not valid");
  Require(owner_body.find("format=SB_LISTENER_OWNER_V1\n") != std::string::npos,
          "owner token format marker missing");
  Require(owner_body.find("atomic_write=true\n") != std::string::npos &&
              owner_body.find("file_fsync=true\n") != std::string::npos &&
              owner_body.find("parent_fsync=true\n") != std::string::npos &&
              owner_body.find("advisory_lock=true\n") != std::string::npos &&
              owner_body.find("lock_file_mode=0600\n") != std::string::npos,
          "owner token durable-write evidence missing");
  RequireMode0600(ArtifactLockPath(identity.owner_file), "owner artifact lock");
  RequireNoTempFiles(identity.owner_file.parent_path());

  const int held_owner_lock = HoldArtifactLock(identity.owner_file);
  error.clear();
  Require(!scratchbird::listener::WriteOwnerToken(identity, &error),
          "owner token write ignored held artifact lock");
  Require(error.find("artifact lock busy") != std::string::npos,
          "owner lock refusal diagnostic was not specific");
  ReleaseArtifactLock(held_owner_lock);

  error.clear();
  Require(!scratchbird::listener::WriteOwnerToken(identity, &error),
          "valid live owner token did not block a second owner");
  Require(error.find("live owner token exists") != std::string::npos,
          "live owner refusal diagnostic was not specific");

  auto tampered = ReadFile(identity.owner_file);
  const auto sig_pos = tampered.find("signature_sha256_128=");
  Require(sig_pos != std::string::npos, "owner signature not found for tamper test");
  tampered.replace(sig_pos, std::string::npos, "signature_sha256_128=00000000000000000000000000000000\n");
  WriteFile(identity.owner_file, tampered);
  Require(!SignedArtifactValid(identity.owner_file, nullptr),
          "tampered owner token still validated");
  const auto stale_owner_tmp = std::filesystem::path(identity.owner_file.string() + ".tmp.stale");
  WriteFile(stale_owner_tmp, "stale temp must be removed\n");
  error.clear();
  Require(scratchbird::listener::WriteOwnerToken(identity, &error),
          "tampered owner token did not permit stale/corrupt recovery: " + error);
  Require(SignedArtifactValid(identity.owner_file, nullptr),
          "owner token was not re-signed after stale/corrupt recovery");
  Require(!std::filesystem::exists(stale_owner_tmp), "stale owner temp file was not removed");

  const std::string identity_json = scratchbird::listener::SocketIdentityJson(identity);
  const std::string pool_json = "{\"running\":true,\"draining\":true,\"busy_worker_count\":1}";
  const int held_lifecycle_lock = HoldArtifactLock(identity.lifecycle_file);
  error.clear();
  Require(!scratchbird::listener::WriteLifecycleStateToken(identity,
                                                           "draining",
                                                           "draining",
                                                           identity_json,
                                                           pool_json,
                                                           &error),
          "lifecycle write ignored held artifact lock");
  Require(error.find("artifact lock busy") != std::string::npos,
          "lifecycle lock refusal diagnostic was not specific");
  ReleaseArtifactLock(held_lifecycle_lock);

  const auto stale_lifecycle_tmp = std::filesystem::path(identity.lifecycle_file.string() + ".tmp.stale");
  WriteFile(stale_lifecycle_tmp, "stale temp must be removed\n");
  error.clear();
  Require(scratchbird::listener::WriteLifecycleStateToken(identity,
                                                          "draining",
                                                          "draining",
                                                          identity_json,
                                                          pool_json,
                                                          &error),
          "lifecycle state write failed: " + error);
  Require(std::filesystem::exists(identity.lifecycle_file), "lifecycle token was not created");
  RequireMode0600(identity.lifecycle_file, "lifecycle token");
  std::string lifecycle_body;
  Require(SignedArtifactValid(identity.lifecycle_file, &lifecycle_body),
          "lifecycle token signature was not valid");
  Require(lifecycle_body.find("format=SB_LISTENER_LIFECYCLE_V1\n") != std::string::npos,
          "lifecycle format marker missing");
  Require(lifecycle_body.find("state=draining\n") != std::string::npos &&
              lifecycle_body.find("requested_state=draining\n") != std::string::npos,
          "lifecycle state evidence missing");
  Require(lifecycle_body.find("atomic_write=true\n") != std::string::npos &&
              lifecycle_body.find("file_fsync=true\n") != std::string::npos &&
              lifecycle_body.find("parent_fsync=true\n") != std::string::npos &&
              lifecycle_body.find("advisory_lock=true\n") != std::string::npos &&
              lifecycle_body.find("lock_file_mode=0600\n") != std::string::npos,
          "lifecycle durable-write evidence missing");
  RequireMode0600(ArtifactLockPath(identity.lifecycle_file), "lifecycle artifact lock");
  Require(!std::filesystem::exists(stale_lifecycle_tmp), "stale lifecycle temp file was not removed");
  RequireNoTempFiles(identity.lifecycle_file.parent_path());

  auto tampered_lifecycle = ReadFile(identity.lifecycle_file);
  const auto state_pos = tampered_lifecycle.find("state=draining");
  Require(state_pos != std::string::npos, "lifecycle state not found for tamper test");
  tampered_lifecycle.replace(state_pos, std::string("state=draining").size(), "state=running");
  WriteFile(identity.lifecycle_file, tampered_lifecycle);
  Require(!SignedArtifactValid(identity.lifecycle_file, nullptr),
          "tampered lifecycle token still validated");

  std::error_code ec;
  std::filesystem::remove_all(work, ec);
  std::cout << "engine_listener_owner_lifecycle_artifact_conformance=passed\n";
  return EXIT_SUCCESS;
}
