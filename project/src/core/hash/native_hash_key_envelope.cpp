// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hash_key_envelope.hpp"
#include "crypto_memory_adapter.hpp"
#include "../common/crypto_random.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace scratchbird::core::crypto {
namespace {
using E = NativeHashKeyEnvelopeErrorV1;
using namespace platform;
constexpr std::size_t kHeader = 240, kSalt = 168, kNonce = 200, kTag = 272;
constexpr char kDomain[] = "ScratchBird/native-hash-key-wrap/v1";
static_assert(sizeof(kDomain) == 36);
bool V7(const Uuid& id) noexcept {
    return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
}
bool Extent(const void* p, std::size_t n) noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    return p && n && n - 1 <= std::numeric_limits<std::uintptr_t>::max() - at;
}
bool Overlap(const void* a, std::size_t an, const void* b, std::size_t bn) noexcept {
    const auto x = reinterpret_cast<std::uintptr_t>(a), y = reinterpret_cast<std::uintptr_t>(b);
    return x <= y + bn - 1 && y <= x + an - 1;
}
bool OutputsDisjoint(const NativeHashKeyBindingV1& binding, NativeHashKeyOutputV1 out,
                     NativeHashKeyInputV1 a, NativeHashKeyInputV1 b) noexcept {
    return !Overlap(out.data, out.size, &binding, sizeof(binding)) &&
           !Overlap(out.data, out.size, a.data, a.size) &&
           !Overlap(out.data, out.size, b.data, b.size);
}
struct OutputOwner {
    NativeHashKeyOutputV1 out;
    bool success = false;
    ~OutputOwner() { if (!success) OPENSSL_cleanse(out.data, out.size); }
};
template<std::size_t N> struct Secret {
    std::array<std::uint8_t, N> bytes{};
    ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};
bool Cancelled(NativeHashKeyCancellationV1 c) {
    return c.requested && c.requested(c.context);
}
void Header(const NativeHashKeyBindingV1& b, std::uint8_t* out) noexcept {
    std::memset(out, 0, kHeader);
    std::memcpy(out, "SBHKEY01", 8);
    StoreLittle16(out + 8, 1); StoreLittle16(out + 10, kHeader);
    StoreLittle16(out + 12, 1);
    const auto put = [&](std::size_t at, const Uuid& id) {
        std::copy(id.bytes.begin(), id.bytes.end(), out + at);
    };
    put(16, b.node); put(32, b.index); put(48, b.key);
    StoreLittle64(out + 64, b.key_generation);
    put(72, b.hash_profile); StoreLittle64(out + 88, b.hash_profile_generation);
    put(96, b.seed_policy); StoreLittle64(out + 112, b.seed_policy_generation);
    put(120, b.wrapping_root); StoreLittle64(out + 136, b.wrapping_root_generation);
    put(144, b.operation); StoreLittle32(out + 160, 32); StoreLittle32(out + 164, 32);
}
bool Derive(NativeHashKeyInputV1 root, const std::uint8_t* header,
            std::array<std::uint8_t, 32>& result) {
    // Submit one exact info extent: avoid repeated provider-parameter updates
    // and their intermediate allocations/concatenation behavior.
    std::array<unsigned char, sizeof(kDomain) + kHeader> info{};
    std::memcpy(info.data(), kDomain, sizeof(kDomain));
    std::copy_n(header, kHeader, info.begin() + sizeof(kDomain));
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
    std::size_t count = result.size();
    return context && EVP_PKEY_derive_init(context.get()) > 0 &&
        EVP_PKEY_CTX_set_hkdf_mode(context.get(), EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND) > 0 &&
        EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) > 0 &&
        EVP_PKEY_CTX_set1_hkdf_salt(context.get(), header + kSalt, 32) > 0 &&
        EVP_PKEY_CTX_set1_hkdf_key(context.get(), root.data, 32) > 0 &&
        EVP_PKEY_CTX_add1_hkdf_info(context.get(), info.data(), info.size()) > 0 &&
        EVP_PKEY_derive(context.get(), result.data(), &count) > 0 && count == result.size();
}
E Cipher(bool seal, NativeHashKeyInputV1 root, const std::uint8_t* header,
         const std::uint8_t* in, std::uint8_t* out, const std::uint8_t* input_tag,
         std::uint8_t* output_tag, NativeHashKeyCancellationV1 cancellation) {
    Secret<32> derived;
    Secret<16> final, tag;
    if (Cancelled(cancellation)) return E::cancelled;
    if (!Derive(root, header, derived.bytes)) return E::provider_failure;
    if (Cancelled(cancellation)) return E::cancelled;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_CipherInit_ex(context.get(), EVP_aes_256_gcm(), nullptr,
                                     nullptr, nullptr, seal ? 1 : 0) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_CipherInit_ex(context.get(), nullptr, nullptr, derived.bytes.data(),
                         header + kNonce, seal ? 1 : 0) != 1) return E::provider_failure;
    int count = 0;
    if (EVP_CipherUpdate(context.get(), nullptr, &count, header, kHeader) != 1 ||
        count != static_cast<int>(kHeader)) return E::provider_failure;
    if (Cancelled(cancellation)) return E::cancelled;
    if (EVP_CipherUpdate(context.get(), out, &count, in, 32) != 1 || count != 32)
        return E::provider_failure;
    if (!seal) {
        std::copy_n(input_tag, tag.bytes.size(), tag.bytes.begin());
        if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                               tag.bytes.data()) != 1) return E::provider_failure;
    }
    if (Cancelled(cancellation)) return E::cancelled;
    const int completed = EVP_CipherFinal_ex(context.get(), final.bytes.data(), &count);
    if (completed != 1) return seal ? E::provider_failure : E::authentication_failed;
    if (count != 0) return E::provider_failure;
    if (seal && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, 16,
                                   output_tag) != 1) return E::provider_failure;
    return Cancelled(cancellation) ? E::cancelled : E::none;
}
}  // namespace

bool NativeHashKeyBindingV1::operator==(const NativeHashKeyBindingV1& b) const noexcept {
    return node == b.node && index == b.index && key == b.key &&
        hash_profile == b.hash_profile && seed_policy == b.seed_policy &&
        wrapping_root == b.wrapping_root && operation == b.operation &&
        key_generation == b.key_generation && hash_profile_generation == b.hash_profile_generation &&
        seed_policy_generation == b.seed_policy_generation && wrapping_root_generation == b.wrapping_root_generation;
}
bool ValidNativeHashKeyBindingV1(const NativeHashKeyBindingV1& b) noexcept {
    return V7(b.node) && V7(b.index) && V7(b.key) && V7(b.hash_profile) &&
        V7(b.seed_policy) && V7(b.wrapping_root) && V7(b.operation) &&
        b.key_generation && b.hash_profile_generation && b.seed_policy_generation && b.wrapping_root_generation;
}
E InspectNativeHashKeyEnvelopeV1(const NativeHashKeyBindingV1& expected,
                               NativeHashKeyInputV1 record) noexcept {
    if (!ValidNativeHashKeyBindingV1(expected)) return E::invalid_binding;
    if (record.size != kNativeHashKeyEnvelopeBytesV1 || !Extent(record.data, record.size))
        return E::invalid_extent;
    const auto* p = record.data;
    if (std::memcmp(p, "SBHKEY01", 8) || LoadLittle16(p + 8) != 1 ||
        LoadLittle16(p + 10) != kHeader || LoadLittle16(p + 12) != 1 ||
        LoadLittle16(p + 14) || LoadLittle32(p + 160) != 32 || LoadLittle32(p + 164) != 32 ||
        !std::all_of(p + 212, p + kHeader, [](auto byte) { return byte == 0; })) return E::malformed;
    std::array<std::uint8_t, kHeader> header{};
    Header(expected, header.data());
    return std::equal(header.begin(), header.begin() + kSalt, p)
        ? E::none : E::binding_mismatch;
}
E SealNativeHashKeyEnvelopeV1(const NativeHashKeyBindingV1& binding,
    NativeHashKeyInputV1 root, NativeHashKeyInputV1 key, NativeHashKeyOutputV1 record,
    NativeHashKeyCancellationV1 cancellation) {
    if (!ValidNativeHashKeyBindingV1(binding)) return E::invalid_binding;
    if (root.size != 32 || key.size != 32 || record.size != kNativeHashKeyEnvelopeBytesV1 ||
        !Extent(root.data, root.size) || !Extent(key.data, key.size) ||
        !Extent(record.data, record.size)) return E::invalid_extent;
    if (!OutputsDisjoint(binding, record, root, key)) return E::overlap;
    OutputOwner owner{record};
    if (hash::CheckExplicitCryptoMemoryScope(binding.node) != hash::CryptoMemoryError::none)
        return E::memory_scope_unavailable;
    if (Cancelled(cancellation)) return E::cancelled;
    Header(binding, record.data);
    if (!core::FillCryptographicRandomBytes(record.data + kSalt, 44)) return E::entropy_failure;
    const auto result = Cipher(true, root, record.data, key.data, record.data + kHeader,
                               nullptr, record.data + kTag, cancellation);
    if (result == E::none &&
        hash::CheckExplicitCryptoMemoryScope(binding.node) != hash::CryptoMemoryError::none)
        return E::memory_scope_unavailable;
    owner.success = result == E::none;
    return result;
}
E OpenNativeHashKeyEnvelopeV1(const NativeHashKeyBindingV1& expected,
    NativeHashKeyInputV1 root, NativeHashKeyInputV1 record, NativeHashKeyOutputV1 key,
    NativeHashKeyCancellationV1 cancellation) {
    if (!ValidNativeHashKeyBindingV1(expected)) return E::invalid_binding;
    if (root.size != 32 || record.size != kNativeHashKeyEnvelopeBytesV1 || key.size != 32 ||
        !Extent(root.data, root.size) || !Extent(record.data, record.size) ||
        !Extent(key.data, key.size)) return E::invalid_extent;
    if (!OutputsDisjoint(expected, key, root, record)) return E::overlap;
    OutputOwner owner{key};
    if (Cancelled(cancellation)) return E::cancelled;
    const auto inspected = InspectNativeHashKeyEnvelopeV1(expected, record);
    if (inspected != E::none) return inspected;
    if (hash::CheckExplicitCryptoMemoryScope(expected.node) != hash::CryptoMemoryError::none)
        return E::memory_scope_unavailable;
    const auto result = Cipher(false, root, record.data, record.data + kHeader, key.data,
                               record.data + kTag, nullptr, cancellation);
    if (result == E::none &&
        hash::CheckExplicitCryptoMemoryScope(expected.node) != hash::CryptoMemoryError::none)
        return E::memory_scope_unavailable;
    owner.success = result == E::none;
    return result;
}
}  // namespace scratchbird::core::crypto
