// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Exercise the real private input-owner and bundle implementation, without
// adding a production fault hook or substituting a storage/response mock.
#define main OfflineBundleToolMain
#include "../../tools/release/public_disk_resource_bundle.cpp"
#undef main

bool Receive(const char* expected) {
  std::string line;
  return std::getline(std::cin, line) && line == expected;
}

int main(int argc, char** argv) {
  if (argc != 4) return 64;
  const std::string mode = argv[1];
  const std::filesystem::path root = argv[2];
  if (mode == "probe-file") {
    disk::FileDevice file;
    const auto result = file.Open(root.string(), disk::FileOpenMode::open_existing_read_only);
    if (result.ok()) return file.Close().ok() ? 0 : 3;
    std::cerr << result.diagnostic.diagnostic_code << '\n';
    return result.diagnostic.diagnostic_code.find("OWNER-LOCK-HELD") != std::string::npos ? 2 : 3;
  }
  Args args;
  args.database_path = root / "public-resource.sbdb";
  args.filespace_paths.push_back(root / "public-resource-secondary.sbfs");
  args.out_path = root / "held-bundle";
  if (mode == "hold-aux") {
    args.registry_path = root / "filespace.registry";
    args.dirty_manifest_path = root / "dirty.manifest";
  }
  if (mode == "race") {
    std::cout << "ready" << std::endl;
    if (!Receive("go")) return 64;
  }
  OwnedBundleInputs inputs;
  const auto error = OpenBundleInputs(args, &inputs);
  if (!error.empty()) {
    std::cerr << error << '\n';
    std::cout << "refused" << std::endl;
    if (error.find("OWNER-LOCK-HELD") == std::string::npos) return 3;
    if (mode == "fail-acquire") return Receive("release") ? 0 : 64;
    return 2;
  }
  if (mode == "fail-acquire") return 3;
  std::cout << "held" << std::endl;
  if (!Receive("build")) return 64;
  const auto result = BuildOwnedDiskResourceBundle(args, inputs);
  // The original tool's self-test deliberately leaves a mismatched registry.
  // The auxiliary-input case must report that error, never fabricated success.
  if (mode == "hold-aux") {
    if (result.ok || result.diagnostic_code != "SB-PUBLIC-DISK-BUNDLE-REGISTRY-CAPACITY-MISMATCH") return 3;
  } else if (!result.ok) {
    std::cerr << result.diagnostic_code << '\n';
    return 3;
  }
  std::cout << "checked" << std::endl;
  return Receive("release") ? 0 : 64;
}
