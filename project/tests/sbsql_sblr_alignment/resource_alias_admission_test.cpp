// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "resource_seed_pack.hpp"
#include "resource_artifact_content_codec.hpp"
#include "unicode_normalization.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <new>
#include <string>
#include <sstream>
#include <type_traits>
#include <vector>

namespace r = scratchbird::core::resources;
namespace { long fail_after = -1; unsigned allocation_faults = 0; }
void* operator new(std::size_t size) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (auto* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
namespace {
unsigned checks = 0, failures = 0;
scratchbird::core::platform::Uuid Id(unsigned tail) {
  return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,static_cast<unsigned char>(tail)}};
}
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::cerr << message << '\n'; }
}
void ArtifactContent(const r::ResourceSeedCatalogImage& image) {
  Check(image.unicode_normalization != nullptr, "seed admission omitted normalization authority");
  Check(image.unicode_collation != nullptr, "seed admission omitted UCA table");
  unsigned executable_profiles=0;
  for(const auto& descriptor:image.collations) {
    if(descriptor.comparison_profile==r::CollationProfile::unbound)continue;
    ++executable_profiles;
    Check(r::ValidCollationProfile(descriptor.comparison_profile,descriptor.case_insensitive,
        descriptor.accent_insensitive),"seed profile contradicts display flags");
  }
  Check(executable_profiles==5,"native seed recipes missing");
  auto invalid_profile=image;
  invalid_profile.collations.front().comparison_profile=static_cast<r::CollationProfile>(6);
  Check(!r::ValidateResourceSeedCatalogImage(invalid_profile,false).ok(),"unknown comparison recipe admitted");
  invalid_profile=image;invalid_profile.collations.front().case_insensitive=true;
  Check(!r::ValidateResourceSeedCatalogImage(invalid_profile,false).ok(),"binary recipe with folding flag admitted");
  static_assert(std::is_const_v<typename decltype(r::ResourceSeedArtifact::content)::element_type>);
  auto snapshot = image;
  Check(snapshot.unicode_normalization == image.unicode_normalization,
        "same-node snapshot lost immutable normalization ownership");
  Check(snapshot.unicode_collation == image.unicode_collation,
        "same-node snapshot lost immutable collation ownership");
  // Re-sign the prototype corruption checks so missing/changed Unicode data
  // must be caught by normalization admission, not an incidental FNV mismatch.
  const auto fnv = [](std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream text; text << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
  };
  const auto reseal = [&](r::ResourceSeedCatalogImage& changed) {
    std::string all, collation;
    for (auto& artifact : changed.artifacts) {
      artifact.content_size_bytes = artifact.content->size(); artifact.content_hash = fnv(*artifact.content);
      all += artifact.content_hash;
      if (artifact.family == r::ResourceSeedFamily::collation || artifact.family == r::ResourceSeedFamily::uca ||
          artifact.family == r::ResourceSeedFamily::uca_manifest) collation += artifact.content_hash;
    }
    changed.resource_artifact_records = changed.artifacts.size();
    changed.content_hash = fnv(all); changed.collation_content_hash = fnv(collation);
  };
  const auto ucd = std::find_if(snapshot.artifacts.begin(), snapshot.artifacts.end(),
      [](const auto& artifact) { return artifact.canonical_path == "resources/collations/uca/UnicodeData.txt"; });
  Check(ucd != snapshot.artifacts.end(), "normalization input not in admitted resource image");
  for (const char* path : {"resources/collations/uca/allkeys.txt", "resources/collations/uca/PropList.txt"}) {
    const auto found = std::find_if(image.artifacts.begin(), image.artifacts.end(),
        [&](const auto& artifact) { return artifact.canonical_path == path; });
    Check(found != image.artifacts.end(), "UCA required artifact missing");
    if (found == image.artifacts.end()) continue;
    const auto index = std::distance(image.artifacts.begin(), found);
    auto changed = image;
    changed.artifacts[index].content = std::make_shared<const std::string>(found->content->substr(1)); reseal(changed);
    const auto corrupted = r::ValidateResourceSeedCatalogImage(changed, false);
    Check(!corrupted.ok() && !corrupted.image.unicode_collation &&
        corrupted.diagnostic.message_key == "resource.seed_pack.unicode_collation_invalid",
        "rehashed truncated UCA artifact admitted");
    changed = image; changed.artifacts.erase(changed.artifacts.begin() + index); reseal(changed);
    const auto missing = r::ValidateResourceSeedCatalogImage(changed, false);
    Check(!missing.ok() && !missing.image.unicode_collation &&
        missing.diagnostic.message_key == "resource.seed_pack.unicode_collation_missing",
        "missing UCA artifact inherited stale compiled authority");
    changed.minimal_bootstrap = true;
    const auto minimal = r::ValidateResourceSeedCatalogImage(changed, true);
    Check(minimal.ok() && minimal.image.unicode_normalization && !minimal.image.unicode_collation,
        "restricted UCA image inherited caller-supplied authority");
  }
  if (ucd != snapshot.artifacts.end()) {
    auto changed = image;
    const auto index = std::distance(snapshot.artifacts.begin(), ucd);
    auto bytes = *changed.artifacts[index].content; bytes[100] ^= 1;
    changed.artifacts[index].content = std::make_shared<const std::string>(std::move(bytes)); reseal(changed);
    const auto corrupt = r::ValidateResourceSeedCatalogImage(changed, false);
    Check(!corrupt.ok() && !corrupt.image.unicode_normalization &&
        corrupt.diagnostic.message_key == "resource.seed_pack.unicode_normalization_invalid",
        "rehashed incomplete/wrong Unicode resource admitted");
    changed = image; changed.artifacts.erase(changed.artifacts.begin() + index); reseal(changed);
    const auto missing = r::ValidateResourceSeedCatalogImage(changed, false);
    Check(!missing.ok() && !missing.image.unicode_normalization &&
        missing.diagnostic.message_key == "resource.seed_pack.unicode_normalization_missing",
        "missing Unicode resource inherited a stale compiled table");
    changed.minimal_bootstrap = true;
    const auto minimal = r::ValidateResourceSeedCatalogImage(changed, true);
    Check(minimal.ok() && !minimal.image.unicode_normalization && !minimal.image.unicode_collation,
        "minimal image inherited another cohort's compiled table");
  }
  Check(snapshot.artifacts.front().content == image.artifacts.front().content,
        "same-node snapshot unnecessarily lost immutable content ownership");
  const auto invalid = [&](r::ResourceSeedCatalogImage changed) {
    const auto refused = r::ValidateResourceSeedCatalogImage(changed, false);
    Check(!refused.ok() && refused.image.artifacts.empty(), "corrupt content image was published");
    changed.minimal_bootstrap = true;
    const auto minimal = r::ValidateResourceSeedCatalogImage(changed, true);
    Check(!minimal.ok() && minimal.image.artifacts.empty(), "minimal flag bypassed retained content integrity");
  };
  snapshot.artifacts.front().content.reset(); invalid(snapshot); snapshot = image;
  snapshot.artifacts.front().content = std::make_shared<const std::string>("changed"); invalid(snapshot); snapshot = image;
  ++snapshot.artifacts.front().content_size_bytes; invalid(snapshot); snapshot = image;
  snapshot.artifacts.front().content_hash[0] ^= 1; invalid(snapshot); snapshot = image;
  snapshot.artifacts.front().status = r::ResourceSeedArtifactStatus::pending; invalid(snapshot); snapshot = image;
  snapshot.artifacts.front().family = r::ResourceSeedFamily::unknown; invalid(snapshot); snapshot = image;
  snapshot.artifacts.front().canonical_path.clear(); invalid(snapshot); snapshot = image;
  snapshot.artifacts.push_back(snapshot.artifacts.front()); invalid(snapshot); snapshot = image;
  ++snapshot.resource_artifact_records; invalid(snapshot); snapshot = image;
  snapshot.content_hash[0] ^= 1; invalid(snapshot); snapshot = image;
  snapshot.collation_content_hash[0] ^= 1;
  Check(!r::ValidateResourceSeedCatalogImage(snapshot, false).ok(), "wrong family checksum accepted");
  // Provenance paths must not be dereferenced while validating retained content.
  snapshot = image; snapshot.seed_pack_root = "/missing/resource-fixture"; snapshot.manifest_path.clear();
  Check(r::ValidateResourceSeedCatalogImage(snapshot, false).ok(), "retained content required a host file");

  std::vector<std::string> encoded;
  std::size_t content_bytes = 0;
  unsigned identity = 1;
  for (auto& artifact : snapshot.artifacts) {
    artifact.artifact_uuid.bytes = {0x01,0x9f,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,0};
    for (unsigned i=0;i<4;++i) artifact.artifact_uuid.bytes[15-i] = (identity >> (8*i)) & 255;
    ++identity;
    Check(artifact.content && artifact.content->size() == artifact.content_size_bytes,
          "actual seed artifact did not retain complete bytes");
    content_bytes += artifact.content->size();
    std::vector<std::string> fragments;
    Check(r::EncodeResourceSeedArtifactContent(artifact, &fragments), "actual artifact encoding failed");
    for (auto& fragment : fragments) encoded.push_back(std::move(fragment));
  }
  std::vector<std::string_view> views;
  {
    auto changed=snapshot; changed.artifacts[1].artifact_uuid=changed.artifacts[0].artifact_uuid; invalid(changed);
    changed=snapshot; changed.artifacts[0].artifact_uuid={}; invalid(changed);
    changed=snapshot; changed.artifacts[0].artifact_uuid.bytes[6]=0x40; invalid(changed);
    changed=snapshot; changed.artifacts[0].artifact_uuid.bytes[8]=0x40; invalid(changed);
  }
  for (const auto& row : encoded) views.push_back(row);
  std::vector<r::ResourceSeedArtifact> decoded;
  Check(r::DecodeResourceSeedArtifactContents(views,&decoded), "actual seed fragment decode failed");
  Check(decoded.size() == snapshot.artifacts.size(), "artifact group count changed");
  if (decoded.size() == snapshot.artifacts.size()) {
    for (std::size_t i=0;i<decoded.size();++i) {
      const auto& expected = snapshot.artifacts[i]; const auto& actual = decoded[i];
      Check(actual.artifact_uuid == expected.artifact_uuid && actual.family == expected.family &&
          actual.canonical_path == expected.canonical_path && actual.source_pattern == expected.source_pattern &&
          actual.required_catalog_rows == expected.required_catalog_rows && actual.create_time_action == expected.create_time_action &&
          actual.content_hash == expected.content_hash && actual.content_size_bytes == expected.content_size_bytes &&
          actual.content && *actual.content == *expected.content, "fragment round trip changed artifact authority");
    }
    snapshot.artifacts = decoded;
    Check(r::ValidateResourceSeedCatalogImage(snapshot, false).ok(), "decoded seed image failed integrity");
  }
  const auto small = std::find_if(snapshot.artifacts.begin(), snapshot.artifacts.end(),
      [](const auto& a) { return a.family == r::ResourceSeedFamily::i18n_version; });
  Check(small != snapshot.artifacts.end(), "version artifact missing");
  if (small != snapshot.artifacts.end()) {
    std::vector<std::string> rows;
    Check(r::EncodeResourceSeedArtifactContent(*small,&rows) && rows.size() == 1, "small artifact is not single-fragment");
    if (rows.size() == 1) {
      const auto reject = [&](const std::vector<std::string>& changed) {
        std::vector<std::string_view> input; for (const auto& row : changed) input.push_back(row);
        std::vector<r::ResourceSeedArtifact> sentinel{*small};
        const auto previous = sentinel.front().content;
        Check(!r::DecodeResourceSeedArtifactContents(input,&sentinel) && sentinel.size() == 1 &&
            sentinel.front().content == previous, "malformed fragments changed published output");
      };
      for (std::size_t size=0;size<rows[0].size();++size) reject({rows[0].substr(0,size)});
      for (std::size_t byte : {0u,4u,6u,14u,16u,24u,32u,36u,40u,44u,48u,50u,52u}) {
        auto changed = rows; changed[0][byte] ^= char(0xff); reject(changed);
      }
      auto changed=rows; changed[0].back() ^= 1; reject(changed);
      changed=rows; changed[0].push_back('\0'); reject(changed);
      changed=rows; changed.push_back(rows[0]); reject(changed);
      auto earlier=*small; earlier.artifact_uuid.bytes[6]=0x40;
      std::vector<std::string> sentinel{"unchanged"};
      Check(!r::EncodeResourceSeedArtifactContent(earlier,&sentinel) && sentinel == std::vector<std::string>{"unchanged"},
            "earlier-version system UUID encoded or changed output");
      auto empty=*small; empty.content = std::make_shared<const std::string>();
      empty.content_size_bytes=0; empty.content_hash="fnv1a64:cbf29ce484222325";
      std::vector<std::string> empty_rows;
      Check(r::EncodeResourceSeedArtifactContent(empty,&empty_rows), "explicit empty artifact refused");
      std::vector<std::string_view> empty_views;
      for (const auto& row:empty_rows) empty_views.push_back(row);
      std::vector<r::ResourceSeedArtifact> empty_decoded;
      Check(r::DecodeResourceSeedArtifactContents(empty_views,&empty_decoded) && empty_decoded.size()==1 &&
          empty_decoded[0].content && empty_decoded[0].content->empty(), "empty artifact became missing content");

      bool encoded_success=false, decoded_success=false;
      std::vector<std::string_view> source{rows[0]};
      for (long attempt=0;attempt<1000 && !encoded_success;++attempt) {
        std::vector<std::string> out{"unchanged"};
        fail_after=attempt; encoded_success=r::EncodeResourceSeedArtifactContent(*small,&out); fail_after=-1;
        if (!encoded_success) { ++allocation_faults; Check(out==std::vector<std::string>{"unchanged"},
            "encode allocation failure changed output"); }
      }
      for (long attempt=0;attempt<1000 && !decoded_success;++attempt) {
        std::vector<r::ResourceSeedArtifact> out{*small}; const auto previous=out[0].content;
        fail_after=attempt; decoded_success=r::DecodeResourceSeedArtifactContents(source,&out); fail_after=-1;
        if (!decoded_success) { ++allocation_faults; Check(out.size()==1 && out[0].content==previous,
            "decode allocation failure changed output"); }
      }
      Check(encoded_success && decoded_success, "allocation fault sweep never reached success");
    }
  }
  const auto large=std::find_if(snapshot.artifacts.begin(),snapshot.artifacts.end(),
      [](const auto& a){return a.family==r::ResourceSeedFamily::uca;});
  if (large!=snapshot.artifacts.end()) {
    std::vector<std::string> rows;
    Check(r::EncodeResourceSeedArtifactContent(*large,&rows) && rows.size()>2,"UCA did not span fragments");
    if (rows.size()>2) {
      std::vector<std::string_view> input; for (const auto& row:rows) input.push_back(row);
      std::vector<r::ResourceSeedArtifact> out{*large}; const auto previous=out[0].content;
      std::swap(input[0],input[1]);
      Check(!r::DecodeResourceSeedArtifactContents(input,&out) && out[0].content==previous,
            "reordered UCA fragments published");
      std::swap(input[0],input[1]); input.pop_back();
      Check(!r::DecodeResourceSeedArtifactContents(input,&out) && out[0].content==previous,
            "missing final UCA fragment published");
    }
  } else Check(false,"actual UCA artifact missing");
  std::cout << "resource_content artifacts=" << snapshot.artifacts.size()
            << " bytes=" << content_bytes << " fragments=" << encoded.size() << '\n';
}
void Ambiguous(const r::ResourceSeedCatalogImage& image, r::ResourceSeedFamily family,
               const std::string& label) {
  const auto result = r::ResolveResourceSeedAlias(image, family, label);
  Check(!result.ok() && result.diagnostic.diagnostic_code == "SB_RESOURCE_ALIAS_AMBIGUOUS",
        "multiple targets were not refused with the registered ambiguity code");
  Check(result.alias.canonical_name.empty() && result.alias.canonical_resource_uuid.is_nil(),
        "ambiguous lookup published a selected target");
  Check(result.diagnostic.message_key == "resource.alias.ambiguous",
        "ambiguity message key did not match the admitted shape");
  Check(result.diagnostic.arguments.size() == 2 &&
        result.diagnostic.arguments[0].key == "resource_family" &&
        result.diagnostic.arguments[0].value == r::ResourceSeedFamilyName(family) &&
        result.diagnostic.arguments[1].key == "alias" &&
        result.diagnostic.arguments[1].value == label,
        "ambiguity diagnostic fields leaked or omitted request-only parameters");
}
}

int main() {
  const auto charset = r::ResourceSeedFamily::charset;
  r::ResourceSeedCatalogImage image;
  image.aliases = {{charset,"shared","first",Id(1),"one"},
                   {charset,"SHARED","second",Id(2),"two"}};
  Ambiguous(image,charset,"Shared");
  std::reverse(image.aliases.begin(),image.aliases.end());
  Ambiguous(image,charset,"Shared");
  image.aliases[1].canonical_name = image.aliases[0].canonical_name;
  Ambiguous(image,charset,"shared"); // Names do not merge different bound identities.
  image.aliases[1].canonical_resource_uuid = image.aliases[0].canonical_resource_uuid;
  image.aliases[1].canonical_name = "presentation-only-difference";
  const auto one = r::ResolveResourceSeedAlias(image,charset,"shared");
  Check(one.ok() && one.alias.canonical_resource_uuid == image.aliases[0].canonical_resource_uuid,
        "repeated evidence for one target became ambiguity");
  image.aliases[1].family = r::ResourceSeedFamily::collation;
  Check(r::ResolveResourceSeedAlias(image,charset,"shared").ok(),
        "another resource family affected charset resolution");
  const auto missing = r::ResolveResourceSeedAlias(image,charset,"absent");
  Check(!missing.ok() && missing.diagnostic.diagnostic_code == "SB_RESOURCE_ALIAS_NOT_FOUND" &&
        missing.alias.canonical_resource_uuid.is_nil(),"absent alias produced a target");
  image.aliases[0].family = r::ResourceSeedFamily::timezone_tables;
  Check(!r::ResolveResourceSeedAlias(image,r::ResourceSeedFamily::timezone_tables,"shared").ok(),
        "timezone aliases unexpectedly folded case");
  Check(r::ResolveResourceSeedAlias(image,r::ResourceSeedFamily::timezone_tables,"SHARED").ok(),
        "exact timezone alias was refused");

  // Bound identities, not alias presentation labels, select descriptors.
  r::ResourceSeedCatalogImage bound;
  r::ResourceSeedCharsetDescriptor charset_descriptor;
  charset_descriptor.resource_uuid=Id(3);
  charset_descriptor.canonical_name="canonical-charset";
  bound.charsets.push_back(charset_descriptor);
  r::ResourceSeedCollationDescriptor collation_descriptor;
  collation_descriptor.resource_uuid=Id(4);
  collation_descriptor.canonical_name="canonical-collation";
  bound.collations.push_back(collation_descriptor);
  bound.aliases={{charset,"cs-alias","stale-presentation",charset_descriptor.resource_uuid,""},
                 {r::ResourceSeedFamily::collation,"co-alias","stale-presentation",
                  collation_descriptor.resource_uuid,""}};
  Check(r::FindResourceSeedCharset(bound,"CS-ALIAS")==&bound.charsets.front(),
        "charset alias selected by presentation instead of bound identity");
  Check(r::FindResourceSeedCollation(bound,"CO-ALIAS")==&bound.collations.front(),
        "collation alias did not select its bound descriptor");
  auto conflict=bound.aliases.back();
  conflict.canonical_resource_uuid=Id(5);
  bound.aliases.push_back(conflict);
  Ambiguous(bound,r::ResourceSeedFamily::collation,"co-alias");
  Check(!r::FindResourceSeedCollation(bound,"co-alias"),
        "collation lookup published an ambiguous descriptor");
  Check(r::FindResourceSeedCollation(bound,"canonical-collation")==&bound.collations.front(),
        "collation primary-name precedence changed");

  // These collisions are independently visible in the actual admitted seed
  // artifact: gb2312 names EUC-CN and GB_2312; binary names OCTETS and binary.
  // No synthetic catalog or generated success fixture substitutes for loading it.
  r::ResourceSeedLoadConfig config;
  config.seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  const auto loaded = r::LoadResourceSeedPack(config);
  if (!loaded.ok()) std::cerr << loaded.diagnostic.diagnostic_code << ':'
                             << loaded.diagnostic.message_key << '\n';
  Check(loaded.ok(),"actual seed pack failed to load");
  if (loaded.ok()) {
    ArtifactContent(loaded.image);
    Ambiguous(loaded.image,charset,"gb2312");
    Ambiguous(loaded.image,charset,"binary");
    const auto* native = r::FindResourceSeedCharset(loaded.image,"binary");
    Check(native && native->canonical_name == "binary", "native primary-name precedence changed");
    Check(!r::FindResourceSeedCharset(loaded.image,"gb2312"),
          "ambiguous alias-only name silently selected a charset");
    const auto unique = r::ResolveResourceSeedAlias(loaded.image,charset,"CP936");
    Check(unique.ok() && unique.alias.canonical_name == "GBK", "unique GBK alias stopped resolving");
    auto reversed = loaded.image;
    std::reverse(reversed.aliases.begin(),reversed.aliases.end());
    Ambiguous(reversed,charset,"Gb2312");
    auto incomplete = loaded.image;
    const auto old_size = incomplete.aliases.size();
    std::erase_if(incomplete.aliases, [&](const auto& row) {
      return row.family == charset && row.canonical_name == "GB_2312" && row.alias == "GB2312";
    });
    Check(incomplete.aliases.size() + 1 == old_size, "missing-relation fixture selected wrong rows");
    const auto refused = r::ValidateResourceSeedCatalogImage(incomplete, false);
    Check(!refused.ok() && refused.diagnostic.diagnostic_code == "SB_RESOURCE_SEED_INCOMPLETE" &&
          refused.diagnostic.message_key == "resource.seed_pack.alias_relationship_missing" &&
          refused.image.aliases.empty(), "incomplete alias catalog became valid activation authority");
    Check(r::ValidateResourceSeedCatalogImage(loaded.image,false).ok(),
          "complete restored alias catalog stopped validating");
  }
  std::cout << "resource_alias_admission checks=" << checks << " failures=" << failures << '\n';
  std::cout << "resource_content allocation_faults=" << allocation_faults << '\n';
  return failures ? 1 : 0;
}
