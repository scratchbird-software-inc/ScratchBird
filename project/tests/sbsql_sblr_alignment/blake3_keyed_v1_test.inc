// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Computation conformance only, not protected custody or native HASH completion.

namespace {

h::Blake3KeyV1 OfficialKeyedKey() {
    constexpr char key[] = "whats the Elvish word for friend";
    static_assert(sizeof(key) == 33);
    h::Blake3KeyV1 result{};
    std::copy_n(key, result.size(), result.begin());
    return result;
}

h::Blake3Digest128V1 DecodeKeyed128(const char* hex) {
    h::Blake3Digest128V1 result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::uint8_t>(
            (Nibble(hex[2 * i]) << 4U) | Nibble(hex[2 * i + 1]));
    }
    return result;
}

bool KeyedSecretStateErased(const h::Blake3KeyedHasherV1& hasher) {
    // The only retained terminal bytes are the distinct magic, ABI, lifecycle.
    // Observing representation does not fabricate mutable access/authority.
    const auto* bytes = reinterpret_cast<const unsigned char*>(&hasher);
    return std::all_of(bytes + 7, bytes + sizeof(hasher),
                       [](unsigned char b) { return b == 0; });
}

h::Blake3Digest128V1 KeyedFixedStep(const h::Blake3KeyV1& key,
                                  const std::vector<std::uint8_t>& input,
                                  std::size_t step) {
    const auto allocations_before = g_allocations.load();
    h::Blake3Digest128V1 result{};
    {
        h::Blake3KeyedHasherV1 hasher(key);
        Check(hasher.Update(nullptr, 0) == h::Blake3IncrementalStatusV3::kOk,
              "keyed empty update");
        for (std::size_t offset = 0; offset < input.size();) {
            const auto take = std::min(step, input.size() - offset);
            Check(hasher.Update(input.data() + offset, take) ==
                      h::Blake3IncrementalStatusV3::kOk,
                  "keyed segmented update");
            offset += take;
        }
        Check(hasher.Finalize(&result) == h::Blake3IncrementalStatusV3::kOk,
              "keyed finalize");
        Check(KeyedSecretStateErased(hasher), "keyed final secret erasure");
    }
    Check(g_allocations.load() == allocations_before,
          "keyed lifecycle allocated memory");
    return result;
}

void OfficialKeyedAnswersAndSegmentation() {
    struct Known { std::size_t size; const char* hex; };
    // BLAKE3 official test_vectors.json at immutable upstream revision:
    // ae3e8e6b3a5ae3190ca5d62820789b17886a0038 (all 35 keyed cases).
    // https://github.com/BLAKE3-team/BLAKE3/blob/
    // ae3e8e6b3a5ae3190ca5d62820789b17886a0038/test_vectors/test_vectors.json
    // Input byte i = i % 251; expected digest is the first 16 output bytes.
    constexpr Known cases[] = {
        {0, "92b2b75604ed3c761f9d6f62392c8a92"},
        {1, "6d7878dfff2f485635d39013278ae14f"},
        {2, "5392ddae0e0a69d5f40160462cbd9bd8"},
        {3, "39e67b76b5a007d4921969779fe666da"},
        {4, "7671dde590c95d5ac9616651ff5aa0a2"},
        {5, "73ac69eecf286894d8102018a6fc729f"},
        {6, "82d3199d0013035682cc7f2a399d4c21"},
        {7, "af0a7ec382aedc0cfd626e49e7628bc7"},
        {8, "be2f5495c61cba1bb348a34948c00404"},
        {63, "bb1eb5d4afa793c1ebdd9fb08def6c36"},
        {64, "ba8ced36f327700d213f120b1a207a3b"},
        {65, "c0a4edefa2d2accb9277c371ac12fcdb"},
        {127, "c64200ae7dfaf35577ac5a9521c47863"},
        {128, "b04fe15577457267ff3b6f3c947d93be"},
        {129, "d4a64dae6cdccbac1e5287f54f17c5f9"},
        {1023, "c951ecdf03288d0fcc96ee3413563d8a"},
        {1024, "75c46f6f3d9eb4f55ecaaee480db732e"},
        {1025, "357dc55de0c7e382c900fd6e320acc04"},
        {2048, "879cf1fa2ea0e79126cb1063617a05b6"},
        {2049, "9f29700902f7c86e514ddc4df1e3049f"},
        {3072, "044a0e7b172a312dc02a4c9a818c036f"},
        {3073, "68dede9bef00ba89e43f31a6825f4cf4"},
        {4096, "befc660aea2f1718884cd8deb9902811"},
        {4097, "00df940cd36bb9fa7cbbc3556744e0db"},
        {5120, "2c493e48e9b9bf31e0553a22b23503c0"},
        {5121, "6ccf1c34753e7a044db80798ecd0782a"},
        {6144, "3d6b6d21281d0ade5b2b016ae4034c5d"},
        {6145, "9ac301e9e39e45e3250a7e3b3df701aa"},
        {7168, "b42835e40e9d4a7f42ad8cc04f85a963"},
        {7169, "ed9b1a922c046fdb3d423ae34e143b05"},
        {8192, "dc9637c8845a770b4cbf76b8daec0eeb"},
        {8193, "954a2a75420c8d6547e3ba5b98d963e6"},
        {16384, "9e9fc4eb7cf081ea7c47d1807790ed21"},
        {31744, "efa53b389ab67c593dba624d898d0f73"},
        {102400, "1c35d1a5811083fd7119f5d5d1ba027b"},
    };
    const auto key = OfficialKeyedKey();
    for (const auto& known : cases) {
        std::vector<std::uint8_t> input(known.size);
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::uint8_t>(i % 251);
        }
        const auto expected = DecodeKeyed128(known.hex);
        constexpr std::size_t steps[] = {1, 7, 63, 64, 65, 1023, 1024, 1025, 4096};
        for (auto step : steps) {
            Check(KeyedFixedStep(key, input, step) == expected,
                  "official keyed vector / fixed segmentation mismatch");
        }
        // Every split for short messages, then every block and chunk boundary
        // (including adjacent bytes) for longer/tree-shaped messages.
        for (std::size_t split = 0; split <= input.size(); ++split) {
            if (input.size() > 129 && split > 0 && split < input.size() &&
                split % 64 > 1 && split % 64 != 63) continue;
            const auto before = g_allocations.load();
            h::Blake3KeyedHasherV1 hasher(key);
            const auto* first = input.empty() ? nullptr : input.data();
            const auto* second = input.empty() ? nullptr : input.data() + split;
            Check(hasher.Update(first, split) == h::Blake3IncrementalStatusV3::kOk &&
                  hasher.Update(nullptr, 0) == h::Blake3IncrementalStatusV3::kOk &&
                  hasher.Update(second, input.size() - split) ==
                      h::Blake3IncrementalStatusV3::kOk,
                  "keyed two-part split");
            h::Blake3Digest128V1 digest{};
            Check(hasher.Finalize(&digest) == h::Blake3IncrementalStatusV3::kOk &&
                      digest == expected && KeyedSecretStateErased(hasher),
                  "official keyed split vector / erasure mismatch");
            Check(g_allocations.load() == before, "keyed split allocation");
        }
    }
}

void KeyedLifecycleAndPointerContracts() {
    using S = h::Blake3IncrementalStatusV3;
    auto key = OfficialKeyedKey();
    const auto empty = DecodeKeyed128("92b2b75604ed3c761f9d6f62392c8a92");
    struct GuardedOutput {
        std::array<std::uint8_t, 8> before;
        h::Blake3Digest128V1 digest;
        std::array<std::uint8_t, 8> after;
    } output;
    output.before.fill(0x45); output.digest.fill(0xa9); output.after.fill(0xb2);
    {
        h::Blake3KeyedHasherV1 hasher(key);
        key.fill(0);  // Constructor owns the admitted key copy, not caller memory.
        Check(hasher.Finalize(&output.digest) == S::kOk && output.digest == empty,
              "keyed original-key lifetime / exact output bytes");
        const auto completed = output.digest;
        Check(hasher.Finalize(&output.digest) == S::kAlreadyFinalized &&
                  output.digest == completed && KeyedSecretStateErased(hasher),
              "keyed duplicate final / unchanged output / scrub");
        Check(hasher.Update(nullptr, 0) == S::kFailedState,
              "keyed failed state cannot restart");
        Check(std::all_of(output.before.begin(), output.before.end(),
                          [](auto b) { return b == 0x45; }) &&
              std::all_of(output.after.begin(), output.after.end(),
                          [](auto b) { return b == 0xb2; }),
              "keyed output exceeded 16 bytes");
    }
    key = OfficialKeyedKey();
    for (bool null_output : {false, true}) {
        h::Blake3KeyedHasherV1 hasher(key);
        const auto before = output.digest;
        Check((null_output ? hasher.Finalize(nullptr) : hasher.Update(nullptr, 1)) ==
                  (null_output ? S::kNullDigest : S::kNullInput),
              "keyed null admitted failure");
        Check(KeyedSecretStateErased(hasher) &&
                  hasher.Finalize(&output.digest) == S::kFailedState &&
                  output.digest == before,
              "keyed failure key erasure / no output / no fallback");
    }
    {
        h::Blake3KeyedHasherV1 hasher(key);
        const auto digest = empty;
        auto result = digest;
        Check(hasher.Finalize(&result) == S::kOk &&
                  hasher.Update(nullptr, 0) == S::kAlreadyFinalized &&
                  KeyedSecretStateErased(hasher),
              "keyed update after finalize");
    }
    alignas(h::Blake3KeyedHasherV1)
        std::array<unsigned char, sizeof(h::Blake3KeyedHasherV1) + 16> storage{};
    for (bool unfinished : {false, true}) {
        storage.fill(0x7b);
        auto* hasher = ::new (storage.data()) h::Blake3KeyedHasherV1(key);
        std::array<unsigned char, sizeof(*hasher)> snapshot{};
        std::copy_n(storage.begin(), snapshot.size(), snapshot.begin());
        for (std::size_t i = 0; i < sizeof(*hasher); ++i) {
            Check(hasher->Update(storage.data() + i, 1) == S::kStateOverlap &&
                      std::equal(snapshot.begin(), snapshot.end(), storage.begin()),
                  "keyed update overlap did not preserve both ranges");
            Check(hasher->Finalize(reinterpret_cast<h::Blake3Digest128V1*>(
                      storage.data() + i)) == S::kStateOverlap &&
                      std::equal(snapshot.begin(), snapshot.end(), storage.begin()),
                  "keyed output overlap did not preserve both ranges");
        }
        const auto maximum = std::numeric_limits<std::uintptr_t>::max();
        Check(hasher->Update(reinterpret_cast<const std::uint8_t*>(maximum - 1), 3) ==
                  S::kStateOverlap &&
                  hasher->Finalize(reinterpret_cast<h::Blake3Digest128V1*>(maximum - 7)) ==
                  S::kStateOverlap &&
                  std::equal(snapshot.begin(), snapshot.end(), storage.begin()),
              "keyed wrapped extent must refuse before dereference");
        if (unfinished) {
            std::array<std::uint8_t, 1031> payload{};
            payload.fill(0x98);
            Check(hasher->Update(payload.data(), payload.size()) == S::kOk,
                  "keyed unfinished multi-chunk state");
        } else {
            Check(hasher->Finalize(&output.digest) == S::kOk && output.digest == empty,
                  "overlap refusals must not poison keyed state");
        }
        hasher->~Blake3KeyedHasherV1();
        Check(std::all_of(storage.begin(), storage.begin() + sizeof(*hasher),
                          [](auto b) { return b == 0; }),
              "keyed destructor must erase all object bytes");
        Check(std::all_of(storage.begin() + sizeof(*hasher), storage.end(),
                          [](auto b) { return b == 0x7b; }),
              "keyed scrub exceeded object bounds");
    }
    // Every key bit matters; zero is valid for the primitive (not evidence of
    // random generation or a permitted native protected key).
    std::vector<std::uint8_t> payload(1031, 0x4a);
    h::Blake3KeyV1 zero{};
    const auto baseline = KeyedFixedStep(zero, payload, 67);
    for (std::size_t bit = 0; bit < 256; ++bit) {
        auto changed = zero;
        changed[bit / 8] = static_cast<std::uint8_t>(1U << (bit % 8));
        Check(KeyedFixedStep(changed, payload, 67) != baseline,
              "keyed hash omitted a key bit");
    }
}

}  // namespace
