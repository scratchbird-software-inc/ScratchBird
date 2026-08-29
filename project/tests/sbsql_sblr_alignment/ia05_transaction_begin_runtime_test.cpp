#include "engine/sblr/sblr_transaction_begin_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace s = scratchbird::engine::sblr;

namespace {

s::SblrTxnUuidV1 Uuid(std::uint64_t salt)
{
    const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
        scratchbird::core::platform::UuidKind::object,
        1787000000000ull + salt);
    assert(generated.ok());

    s::SblrTxnUuidV1 value{};
    std::copy(generated.value.value.bytes.begin(),
              generated.value.value.bytes.end(), value.begin());
    return value;
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

void WriteU16(std::vector<std::uint8_t>* bytes, std::size_t offset,
              std::uint16_t value)
{
    (*bytes)[offset] = static_cast<std::uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteU32(std::vector<std::uint8_t>* bytes, std::size_t offset,
              std::uint32_t value)
{
    for (std::size_t i = 0; i < 4; ++i) {
        (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void WriteU64(std::vector<std::uint8_t>* bytes, std::size_t offset,
              std::uint64_t value)
{
    for (std::size_t i = 0; i < 8; ++i) {
        (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

bool NonZero(const s::SblrTxnShaV1& value)
{
    return std::any_of(value.begin(), value.end(),
                       [](std::uint8_t byte) { return byte != 0; });
}

void ExpectOptionsDecodeReject(std::vector<std::uint8_t> bytes)
{
    s::SblrTransactionBeginOptionsV1 decoded;
    std::string detail;
    assert(!s::DecodeSblrTransactionBeginOptionsV1(
        bytes.data(), bytes.size(), &decoded, &detail));
}

void ExpectHandleDecodeReject(std::vector<std::uint8_t> bytes)
{
    s::SblrTransactionHandleV1 decoded;
    std::string detail;
    assert(!s::DecodeSblrTransactionHandleV1(
        bytes.data(), bytes.size(), &decoded, &detail));
}

template <typename Mutator>
void ExpectOptionsEncodeReject(
    const s::SblrTransactionBeginOptionsV1& canonical, Mutator mutate)
{
    auto candidate = canonical;
    mutate(&candidate);
    assert(s::EncodeSblrTransactionBeginOptionsV1(&candidate).empty());
}

template <typename Mutator>
void ExpectHandleEncodeReject(const s::SblrTransactionHandleV1& canonical,
                              Mutator mutate)
{
    auto candidate = canonical;
    mutate(&candidate);
    assert(s::EncodeSblrTransactionHandleV1(candidate).empty());
}

} // namespace

int main()
{
    s::SblrTransactionBeginOptionsV1 options;
    options.isolation_profile_uuid = Uuid(1);
    options.isolation_profile_generation = 7;
    options.read_mode = 1;
    options.authority_scope = 1;
    options.wait_policy = 2;
    options.transaction_policy_snapshot_uuid = Uuid(2);
    options.transaction_policy_generation = 9;
    options.deadline_monotonic_ns = 42;

    assert(s::EncodeSblrTransactionBeginOptionsV1(nullptr).empty());
    const auto options_bytes =
        s::EncodeSblrTransactionBeginOptionsV1(&options);
    assert(options_bytes.size() == 112);
    assert(std::equal(options_bytes.begin(), options_bytes.begin() + 4,
                      "TXBO"));
    assert(ReadU16(options_bytes, 4) == 1);
    assert(ReadU16(options_bytes, 6) == 112);
    assert(ReadU32(options_bytes, 8) == 112);
    assert(ReadU32(options_bytes, 12) == 0);
    assert(std::equal(options.isolation_profile_uuid.begin(),
                      options.isolation_profile_uuid.end(),
                      options_bytes.begin() + 16));
    assert(ReadU64(options_bytes, 32) == 7);
    assert(options_bytes[40] == 1);
    assert(options_bytes[41] == 1);
    assert(options_bytes[42] == 2);
    assert(std::all_of(options_bytes.begin() + 43,
                       options_bytes.begin() + 48,
                       [](std::uint8_t byte) { return byte == 0; }));
    assert(std::equal(options.transaction_policy_snapshot_uuid.begin(),
                      options.transaction_policy_snapshot_uuid.end(),
                      options_bytes.begin() + 48));
    assert(ReadU64(options_bytes, 64) == 9);
    assert(ReadU64(options_bytes, 72) == 42);
    assert(NonZero(options.options_sha256));
    assert(std::equal(options.options_sha256.begin(),
                      options.options_sha256.end(),
                      options_bytes.begin() + 80));

    s::SblrTransactionBeginOptionsV1 decoded_options;
    std::string detail;
    assert(s::DecodeSblrTransactionBeginOptionsV1(
        options_bytes.data(), options_bytes.size(), &decoded_options,
        &detail));
    assert(decoded_options.isolation_profile_uuid ==
           options.isolation_profile_uuid);
    assert(decoded_options.isolation_profile_generation == 7);
    assert(decoded_options.read_mode == 1);
    assert(decoded_options.authority_scope == 1);
    assert(decoded_options.wait_policy == 2);
    assert(decoded_options.transaction_policy_snapshot_uuid ==
           options.transaction_policy_snapshot_uuid);
    assert(decoded_options.transaction_policy_generation == 9);
    assert(decoded_options.deadline_monotonic_ns == 42);
    assert(decoded_options.options_sha256 == options.options_sha256);
    auto reencoded_options = decoded_options;
    assert(s::EncodeSblrTransactionBeginOptionsV1(&reencoded_options) ==
           options_bytes);

    ExpectOptionsEncodeReject(options, [](auto* value) {
        value->isolation_profile_uuid.fill(0);
    });
    ExpectOptionsEncodeReject(options, [](auto* value) {
        value->isolation_profile_generation = 0;
    });
    ExpectOptionsEncodeReject(options, [](auto* value) {
        value->transaction_policy_snapshot_uuid.fill(0);
    });
    ExpectOptionsEncodeReject(options, [](auto* value) {
        value->transaction_policy_generation = 0;
    });
    for (const std::uint8_t invalid : {std::uint8_t{0}, std::uint8_t{3}}) {
        ExpectOptionsEncodeReject(options, [invalid](auto* value) {
            value->read_mode = invalid;
        });
        ExpectOptionsEncodeReject(options, [invalid](auto* value) {
            value->authority_scope = invalid;
        });
        ExpectOptionsEncodeReject(options, [invalid](auto* value) {
            value->wait_policy = invalid;
        });
    }

    auto bad = options_bytes;
    bad[0] = 'X';
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    WriteU16(&bad, 4, 2);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    WriteU16(&bad, 6, 111);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    WriteU32(&bad, 8, 111);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    WriteU32(&bad, 12, 1);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[43] = 1;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[16] ^= 1;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    WriteU64(&bad, 32, 0);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[40] = 3;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[41] = 3;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[42] = 3;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[48] ^= 1;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    WriteU64(&bad, 64, 0);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[72] ^= 1;
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad[80] ^= 1;
    ExpectOptionsDecodeReject(bad);
    bad.assign(options_bytes.begin(), options_bytes.end() - 1);
    ExpectOptionsDecodeReject(bad);
    bad = options_bytes;
    bad.push_back(0);
    ExpectOptionsDecodeReject(bad);
    assert(!s::DecodeSblrTransactionBeginOptionsV1(
        nullptr, options_bytes.size(), &decoded_options, &detail));
    assert(!s::DecodeSblrTransactionBeginOptionsV1(
        options_bytes.data(), options_bytes.size(), nullptr, &detail));

    s::SblrTransactionHandleV1 handle;
    handle.transaction_uuid = Uuid(3);
    handle.local_transaction_id = 1;
    handle.snapshot_uuid = Uuid(4);
    handle.isolation_profile_uuid = options.isolation_profile_uuid;
    handle.isolation_profile_generation = 7;
    handle.transaction_policy_snapshot_uuid =
        options.transaction_policy_snapshot_uuid;
    handle.transaction_policy_generation = 9;
    handle.read_mode = 1;
    handle.lifecycle_state = 1;
    handle.authority_scope = 1;
    handle.executor_availability_generation = 3;

    const auto handle_bytes = s::EncodeSblrTransactionHandleV1(handle);
    assert(handle_bytes.size() == 152);
    assert(std::equal(handle_bytes.begin(), handle_bytes.begin() + 4,
                      "TXBH"));
    assert(ReadU16(handle_bytes, 4) == 1);
    assert(ReadU16(handle_bytes, 6) == 152);
    assert(ReadU32(handle_bytes, 8) == 152);
    assert(ReadU32(handle_bytes, 12) == 0);
    assert(std::equal(handle.transaction_uuid.begin(),
                      handle.transaction_uuid.end(),
                      handle_bytes.begin() + 16));
    assert(ReadU64(handle_bytes, 32) == 1);
    assert(std::equal(handle.snapshot_uuid.begin(), handle.snapshot_uuid.end(),
                      handle_bytes.begin() + 40));
    assert(ReadU64(handle_bytes, 72) == 7);
    assert(ReadU64(handle_bytes, 96) == 9);
    assert(handle_bytes[104] == 1);
    assert(handle_bytes[105] == 1);
    assert(handle_bytes[106] == 1);
    assert(std::all_of(handle_bytes.begin() + 107,
                       handle_bytes.begin() + 112,
                       [](std::uint8_t byte) { return byte == 0; }));
    assert(std::any_of(handle_bytes.begin() + 112,
                       handle_bytes.begin() + 144,
                       [](std::uint8_t byte) { return byte != 0; }));
    assert(ReadU64(handle_bytes, 144) == 3);

    s::SblrTransactionHandleV1 decoded_handle;
    assert(s::DecodeSblrTransactionHandleV1(
        handle_bytes.data(), handle_bytes.size(), &decoded_handle, &detail));
    assert(decoded_handle.transaction_uuid == handle.transaction_uuid);
    assert(decoded_handle.local_transaction_id == 1);
    assert(decoded_handle.snapshot_uuid == handle.snapshot_uuid);
    assert(decoded_handle.isolation_profile_uuid ==
           handle.isolation_profile_uuid);
    assert(decoded_handle.isolation_profile_generation == 7);
    assert(decoded_handle.transaction_policy_snapshot_uuid ==
           handle.transaction_policy_snapshot_uuid);
    assert(decoded_handle.transaction_policy_generation == 9);
    assert(decoded_handle.read_mode == 1);
    assert(decoded_handle.lifecycle_state == 1);
    assert(decoded_handle.authority_scope == 1);
    assert(NonZero(decoded_handle.handle_evidence_sha256));
    assert(decoded_handle.executor_availability_generation == 3);
    assert(s::EncodeSblrTransactionHandleV1(decoded_handle) == handle_bytes);

    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->transaction_uuid.fill(0);
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->local_transaction_id = 0;
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->snapshot_uuid.fill(0);
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->isolation_profile_uuid.fill(0);
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->isolation_profile_generation = 0;
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->transaction_policy_snapshot_uuid.fill(0);
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->transaction_policy_generation = 0;
    });
    for (const std::uint8_t invalid : {std::uint8_t{0}, std::uint8_t{3}}) {
        ExpectHandleEncodeReject(handle, [invalid](auto* value) {
            value->read_mode = invalid;
        });
        ExpectHandleEncodeReject(handle, [invalid](auto* value) {
            value->authority_scope = invalid;
        });
    }
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->lifecycle_state = 0;
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->lifecycle_state = 2;
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->executor_availability_generation = 0;
    });
    ExpectHandleEncodeReject(handle, [](auto* value) {
        value->handle_evidence_sha256.fill(1);
    });

    bad = handle_bytes;
    bad[0] = 'X';
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU16(&bad, 4, 2);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU16(&bad, 6, 151);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU32(&bad, 8, 151);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU32(&bad, 12, 1);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[107] = 1;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[16] ^= 1;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU64(&bad, 32, 0);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[40] ^= 1;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[56] ^= 1;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU64(&bad, 72, 0);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[80] ^= 1;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU64(&bad, 96, 0);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[104] = 3;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[105] = 2;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[106] = 3;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad[112] ^= 1;
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    WriteU64(&bad, 144, 0);
    ExpectHandleDecodeReject(bad);
    bad.assign(handle_bytes.begin(), handle_bytes.end() - 1);
    ExpectHandleDecodeReject(bad);
    bad = handle_bytes;
    bad.push_back(0);
    ExpectHandleDecodeReject(bad);
    assert(!s::DecodeSblrTransactionHandleV1(
        nullptr, handle_bytes.size(), &decoded_handle, &detail));
    assert(!s::DecodeSblrTransactionHandleV1(
        handle_bytes.data(), handle_bytes.size(), nullptr, &detail));

    return 0;
}
