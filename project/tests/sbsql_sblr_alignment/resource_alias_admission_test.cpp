// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "resource_seed_pack.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace r = scratchbird::core::resources;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::cerr << message << '\n'; }
}
void Ambiguous(const r::ResourceSeedCatalogImage& image, r::ResourceSeedFamily family,
               const std::string& label) {
  const auto result = r::ResolveResourceSeedAlias(image, family, label);
  Check(!result.ok() && result.diagnostic.diagnostic_code == "SB_RESOURCE_ALIAS_AMBIGUOUS",
        "multiple targets were not refused with the registered ambiguity code");
  Check(result.alias.canonical_name.empty() && result.alias.canonical_resource_uuid.empty(),
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
  image.aliases = {{charset,"shared","first","019d0000-0000-7000-8000-000000000001","one"},
                   {charset,"SHARED","second","019d0000-0000-7000-8000-000000000002","two"}};
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
        missing.alias.canonical_resource_uuid.empty(),"absent alias produced a target");
  image.aliases[0].family = r::ResourceSeedFamily::timezone_tables;
  Check(!r::ResolveResourceSeedAlias(image,r::ResourceSeedFamily::timezone_tables,"shared").ok(),
        "timezone aliases unexpectedly folded case");
  Check(r::ResolveResourceSeedAlias(image,r::ResourceSeedFamily::timezone_tables,"SHARED").ok(),
        "exact timezone alias was refused");

  // Bound identities, not alias presentation labels, select descriptors.
  r::ResourceSeedCatalogImage bound;
  r::ResourceSeedCharsetDescriptor charset_descriptor;
  charset_descriptor.resource_uuid="019d0000-0000-7000-8000-000000000003";
  charset_descriptor.canonical_name="canonical-charset";
  bound.charsets.push_back(charset_descriptor);
  r::ResourceSeedCollationDescriptor collation_descriptor;
  collation_descriptor.resource_uuid="019d0000-0000-7000-8000-000000000004";
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
  conflict.canonical_resource_uuid="019d0000-0000-7000-8000-000000000005";
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
  Check(loaded.ok(),"actual seed pack failed to load");
  if (loaded.ok()) {
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
  return failures ? 1 : 0;
}
