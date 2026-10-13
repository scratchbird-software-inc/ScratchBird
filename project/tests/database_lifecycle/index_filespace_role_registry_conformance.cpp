// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "page_registry.hpp"
#include <cstdlib>
#include <iostream>
#include <set>

// Registry eligibility only: this does not authorize physical placement,
// change an index's lifetime, or waive actual lifecycle/MGA/security admission.
int main() {
  namespace page=scratchbird::storage::page;
  using Role=scratchbird::storage::filespace::FilespaceRole;
  unsigned families=0,checks=0;
  for(const auto& descriptor:page::BuiltinPageFamilyRegistry()) {
    if(descriptor.family!=page::PageFamily::index)continue;
    ++families;
    const std::set<Role> roles(descriptor.legal_filespace_roles.begin(),descriptor.legal_filespace_roles.end());
    if(roles.size()!=14||descriptor.legal_filespace_roles.size()!=14)return EXIT_FAILURE;
    for(unsigned value=0;value<=17;++value) {
      const auto result=page::ValidatePageTypeFilespaceRole(descriptor.page_type,
          static_cast<Role>(value),"architect_selected_index_placement");
      const bool registered=value>=1&&value<=14;
      ++checks;
      if(result.ok()!=registered||(!registered&&
          result.diagnostic.diagnostic_code!="SB_DIAG_PAGE_FILESPACE_ROLE_FORBIDDEN")) {
        std::cerr<<descriptor.stable_name<<" role="<<value<<" eligibility mismatch\n";
        return EXIT_FAILURE;
      }
    }
  }
  if(!families)return EXIT_FAILURE;
  // A related management/dependency page is not an index placement family.
  // Its independent six-role contract must not widen through a shared helper.
  const auto dependency=page::LookupPageFamily(scratchbird::storage::disk::PageType::index_resource_state);
  const std::set<Role> dependency_roles{Role::active_primary,Role::primary_shadow,
      Role::primary_snapshot,Role::primary_candidate,Role::archive_history,Role::import_candidate};
  if(!dependency.ok()||dependency.descriptor.family!=page::PageFamily::typed_dependency||
      dependency.descriptor.registry_status!=page::PageRegistryStatus::reserved||
      dependency.descriptor.supported_local_read||dependency.descriptor.supported_local_write||
      std::set<Role>(dependency.descriptor.legal_filespace_roles.begin(),
          dependency.descriptor.legal_filespace_roles.end())!=dependency_roles||
      dependency.descriptor.legal_filespace_roles.size()!=dependency_roles.size())return EXIT_FAILURE;
  for(unsigned value=0;value<=17;++value) {
    const auto role=static_cast<Role>(value);
    const auto result=page::ValidatePageTypeFilespaceRole(dependency.descriptor.page_type,role,"inspect_dependency");
    ++checks;
    if(result.ok()!=dependency_roles.contains(role)||(!result.ok()&&
        result.diagnostic.diagnostic_code!="SB_DIAG_PAGE_FILESPACE_ROLE_FORBIDDEN"))return EXIT_FAILURE;
  }
  std::cout<<"PASS index filespace registry families="<<families<<" checks="<<checks<<'\n';
  return EXIT_SUCCESS;
}
