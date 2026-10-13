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
  std::cout<<"PASS index filespace registry families="<<families<<" checks="<<checks<<'\n';
  return EXIT_SUCCESS;
}
