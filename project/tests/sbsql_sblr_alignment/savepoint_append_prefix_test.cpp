// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_savepoint_append_recovery.hpp"
#include "../support/owned_temp_directory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

// Pure physical hash-lifecycle regression, not a fabricated recovery issuer.
// The live KV gate separately admits actual receipts and exercises real append,
// synchronization, partial-write, identity, rollback and retained-owner effects.
int main() {
#if !defined(_WIN32)
  namespace recovery = scratchbird::engine::internal_api::savepoint_append_recovery;
  namespace hash = scratchbird::core::hash;
  const auto check = [](bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
  };
  try {
    scratchbird::tests::OwnedTempDirectory directory;
    const auto path = (directory.path()/"prefix.bin").string();
    std::vector<std::uint8_t> bytes(8197);
    for (std::size_t i=0; i<bytes.size(); ++i) bytes[i]=static_cast<std::uint8_t>(i*131+17);
    {
      std::ofstream output(path,std::ios::binary);
      output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
      output.close(); check(!output.fail(),"write actual prefix fixture");
    }
    {
      recovery::Owner owner;
      owner.recovery_fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
      check(owner.recovery_fd>=0,"open actual prefix source");
      for (const std::size_t length : {0u,1u,4095u,4096u,4097u,8197u}) {
        owner.original_end=length;
        const auto expected=hash::ComputeBlake3Digest(std::vector<std::uint8_t>(bytes.begin(),bytes.begin()+length));
        for (unsigned pass=0; pass<4; ++pass) {
          hash::Blake3Digest digest{};
          check(recovery::PrefixDigest(owner,&digest)&&digest==expected,
              "independent prefix reread reused a terminal hasher or changed exact bytes");
          check(hash::InitializeBlake3IncrementalV3(&owner.hash_state)==hash::Blake3IncrementalStatusV3::kOk,
              "successful prefix reread did not scrub its charged state");
          hash::ScrubBlake3IncrementalV3(&owner.hash_state);
        }
      }
      owner.original_end=bytes.size()+1;
      hash::Blake3Digest sentinel{};sentinel.fill(0xa5);auto digest=sentinel;
      check(!recovery::PrefixDigest(owner,&digest)&&digest==sentinel&&owner.native_error==EIO,
          "short actual source must refuse without publishing a prefix digest");
      check(hash::InitializeBlake3IncrementalV3(&owner.hash_state)==hash::Blake3IncrementalStatusV3::kOk,
          "failed prefix reread did not scrub its charged state before retry");
      hash::ScrubBlake3IncrementalV3(&owner.hash_state);
      owner.original_end=bytes.size();
      check(recovery::PrefixDigest(owner,&digest)&&digest==hash::ComputeBlake3Digest(bytes),
          "failed prefix read poisoned a fresh valid independent reread");
    } // The real source descriptor closes before fixture removal.
    directory.Cleanup();
    std::cout<<"independent prefix hash lifecycle PASS\n";
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
#endif
}
