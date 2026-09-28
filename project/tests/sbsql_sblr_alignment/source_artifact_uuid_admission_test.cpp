// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_source_artifact_runtime.hpp"
#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace s = scratchbird::engine::sblr;
using Id = s::SblrSourceArtifactUuidV1;
using Artifact = s::SblrSourceArtifactMapV1;
using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0;
void Require(bool ok, const char* reason) {
  ++checks;
  if (!ok) {
    std::cerr << reason << " check=" << checks << '\n';
    std::exit(1);
  }
}
Id Identity(unsigned suffix) {
  return {1, 0xa0, 0x7c, 0x0a, 0x0d, 0x5c, 0x70, 0, 0x80, 0,
          0xff, 0, 0, 0, 0, static_cast<std::uint8_t>(suffix)};
}
std::uint32_t Read32(const Bytes& bytes, std::size_t at) {
  std::uint32_t result = 0;
  for (unsigned i = 0; i != 4; ++i)
    result |= std::uint32_t(bytes.at(at + i)) << (8 * i);
  return result;
}

// Rebuild the specified hashes independently of the source-artifact codec.
// A valid hash must not authenticate malformed system identity bytes.
void Hash(Bytes& bytes, std::string_view domain, std::size_t fixed,
          std::size_t fixed_size, std::size_t variable,
          std::size_t variable_size, std::size_t destination) {
  Bytes input(domain.begin(), domain.end());
  input.insert(input.end(), bytes.begin() + fixed,
               bytes.begin() + fixed + fixed_size);
  input.insert(input.end(), bytes.begin() + variable,
               bytes.begin() + variable + variable_size);
  auto digest = scratchbird::core::hash::ComputeSha256Digest(input).digest;
  std::copy(digest.begin(), digest.end(), bytes.begin() + destination);
}
void Rehash(Bytes& bytes) {
  const std::size_t language_size = bytes.at(100) | (bytes.at(101) << 8);
  const auto symbol = 224 + language_size;
  const auto symbol_size = Read32(bytes, 176);
  const auto hint = symbol + symbol_size + Read32(bytes, 180);
  const auto hint_size = Read32(bytes, 184);
  if (symbol_size != 0)
    Hash(bytes, "ScratchBird.SblrSourceArtifactSymbol.V1", symbol + 4, 76,
         symbol + 112, symbol_size - 112, symbol + 80);
  if (hint_size != 0)
    Hash(bytes, "ScratchBird.SblrSourceArtifactRenderHint.V1", hint + 4, 52,
         hint + 88, hint_size - 88, hint + 56);
  Hash(bytes, "ScratchBird.SblrSourceArtifactMap.V1", 16, 176,
       224, bytes.size() - 224, 192);
}
Bytes Encode(const Artifact& artifact) {
  return s::EncodeSblrSourceArtifactMapV1(artifact);
}
bool Decode(const Bytes& bytes) {
  const auto result = s::DecodeSblrSourceArtifactMapV1(bytes.data(), bytes.size());
  const bool ok = result.status == s::SblrSourceArtifactDecodeStatusV1::ok;
  if (ok) {
    Require(result.canonical_bytes == bytes && Encode(result.artifact) == bytes,
            "accepted artifact did not preserve exact bytes");
  } else {
    Require(result.artifact.artifact_uuid == Id{} &&
                result.artifact.symbols.empty() &&
                result.artifact.render_hints.empty() &&
                result.canonical_bytes.empty(),
            "refused artifact published partial state");
  }
  return ok;
}
template<class Field>
void Required(const Artifact& source, Field field, std::size_t wire_offset) {
  const auto canonical = Encode(source);
  Require(!canonical.empty() && Decode(canonical), "positive fixture refused");
  auto reconstructed = canonical;
  Rehash(reconstructed);
  Require(reconstructed == canonical, "independent digest reconstruction differs");
  for (unsigned offset : {6u, 8u}) {
    for (unsigned byte = 0; byte != 256; ++byte) {
      const bool valid = offset == 6 ? byte >> 4 == 7 : byte >> 6 == 2;
      auto value = source;
      field(value)[offset] = static_cast<std::uint8_t>(byte);
      Require(!Encode(value).empty() == valid,
              "encoder system UUID version or variant admission");
      auto wire = canonical;
      wire.at(wire_offset + offset) = static_cast<std::uint8_t>(byte);
      Rehash(wire);
      Require(Decode(wire) == valid,
              "decoder admitted malformed UUID with authentic hashes");
    }
  }
  auto value = source;
  field(value) = {};
  Require(Encode(value).empty(), "required nil identity encoded");
  auto wire = canonical;
  std::fill_n(wire.begin() + wire_offset, 16, 0);
  Rehash(wire);
  Require(!Decode(wire), "required nil identity decoded");
}
Artifact Fixture() {
  Artifact a;
  a.artifact_uuid = Identity(1);
  a.sblr_envelope_uuid = Identity(2);
  a.container_request_uuid = Identity(3);
  a.dialect_family_uuid = Identity(4);
  a.parser_package_uuid = Identity(5);
  a.source_text_ref.present = true;
  a.source_text_ref.uuid = Identity(6);
  a.source_text_ref.declared_size = 4;
  a.source_text_ref.crc32c = 1;
  a.source_text_ref.sha256[0] = 1;
  s::SblrSourceArtifactSymbolV1 symbol;
  symbol.symbol_id = 1;
  symbol.symbol_key = "object.1";
  symbol.symbol_kind = s::SblrSourceArtifactSymbolKindV1::object_display_name;
  symbol.related_object_uuid = Identity(7);
  symbol.raw_name_utf8 = "customer";
  a.symbols = {symbol};
  s::SblrSourceArtifactRenderHintV1 hint;
  hint.render_hint_id = 1;
  hint.symbol_id = 1;
  hint.dialect_family_uuid = Identity(4);
  a.render_hints = {hint};
  return a;
}
int main() {
  auto source = Fixture();
  Required(source, [](Artifact& a) -> Id& { return a.dialect_family_uuid; }, 64);
  Required(source, [](Artifact& a) -> Id& { return a.parser_package_uuid; }, 80);
  Required(source, [](Artifact& a) -> Id& { return a.artifact_uuid; }, 16);
  Required(source, [](Artifact& a) -> Id& { return a.source_text_ref.uuid; }, 116);
  const auto canonical = Encode(source);
  const auto symbol = 224 + source.language_tag.size();
  const auto hint = symbol + Read32(canonical, 176);
  Required(source, [](Artifact& a) -> Id& { return a.symbols[0].related_object_uuid; },
           symbol + 36);
  Required(source, [](Artifact& a) -> Id& { return a.render_hints[0].dialect_family_uuid; },
           hint + 28);
  auto envelope_only = source;
  envelope_only.container_request_uuid = {};
  Required(envelope_only, [](Artifact& a) -> Id& { return a.sblr_envelope_uuid; }, 32);
  auto container_only = source;
  container_only.sblr_envelope_uuid = {};
  Required(container_only, [](Artifact& a) -> Id& { return a.container_request_uuid; }, 48);
  // Neither an alternative valid binding nor a cleared presence flag may
  // turn a malformed non-nil optional UUID into an absent one.
  for (unsigned offset : {32u, 48u}) {
    for (unsigned version = 0; version != 16; ++version) {
      if (version == 7) continue;
      auto value = source;
      auto& id = offset == 32 ? value.sblr_envelope_uuid : value.container_request_uuid;
      id[6] = version << 4;
      Require(Encode(value).empty(), "malformed optional binding encoded as absent");
      for (bool flag : {false, true}) {
        auto wire = canonical;
        wire[offset + 6] = version << 4;
        if (!flag) wire[12] &= ~(offset == 32 ? 1 : 2);
        Rehash(wire);
        Require(!Decode(wire), "malformed optional binding decoded as absent");
      }
    }
  }
  auto value = source;
  value.source_text_ref = {};
  Require(Decode(Encode(value)), "absent source text reference refused");
  value.source_text_ref.uuid = Identity(6);
  Require(Encode(value).empty(), "absent source text reference carried UUID");
  value = source;
  value.source_text_ref.present = false;
  Require(Encode(value).empty(), "absent source text reference carried metadata");
  value = source;
  value.symbols[0].symbol_kind = s::SblrSourceArtifactSymbolKindV1::variable;
  value.symbols[0].declaration_node_id = 1;
  Require(Encode(value).empty(), "non-object symbol carried object identity");
  value.symbols[0].related_object_uuid = {};
  Require(Decode(Encode(value)), "non-object symbol with absent identity refused");
  std::cout << "source artifact UUID admission checks=" << checks << '\n';
}
