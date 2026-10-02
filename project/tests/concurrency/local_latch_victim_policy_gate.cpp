// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "local_latch_victim_policy.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
namespace c = scratchbird::core::concurrency;
using P = c::LocalLatchVictimPreference;
using S = c::LocalLatchVictimSelectionStatus;
using Category = c::LocalLatchVictimSafetyClass;
unsigned checks=0;
bool forbid_heap=false;
unsigned heap_attempts=0;
void Check(bool ok,const char* reason) {
  ++checks;
  if (!ok) { std::fprintf(stderr,"FAIL %s\n",reason); std::abort(); }
}
auto Id(unsigned n) {
  c::LocalLatchVictimTaskId id{};
  id[6]=0x70; id[8]=0x80; id[15]=static_cast<std::uint8_t>(n);
  return id;
}
auto Candidate(unsigned id,unsigned category) {
  return c::LocalLatchVictimCandidate{Id(id),static_cast<Category>(category)};
}
void Compare() {
  for (unsigned left=1;left<=5;++left) for (unsigned right=1;right<=5;++right) {
    const auto a=Candidate(1,left), b=Candidate(2,right);
    const auto expected=left<right?P::left: P::right;
    Check(c::CompareLocalLatchVictimPreference(a,b)==expected,"safety category precedes UUID tie");
    Check(c::CompareLocalLatchVictimPreference(b,a)==(expected==P::left?P::right:P::left),
          "reverse pair preserves category priority");
    Check(c::CompareLocalLatchVictimPreference(a,a)==P::equivalent,"identical task and category equivalent");
  }
  for (unsigned category=1;category<=5;++category) for (unsigned byte=0;byte<16;++byte) {
    for (unsigned value=0;value<256;++value) {
      auto a=Candidate(0,category), b=a;
      // Independently enumerate only legal values of the constrained UUID bytes.
      if (byte==6 && (value<0x70 || value>0x7f)) continue;
      if (byte==8 && (value<0x80 || value>0xbf)) continue;
      b.task_id[byte]=static_cast<std::uint8_t>(value);
      const auto expected=value>a.task_id[byte]?P::right:value<a.task_id[byte]?P::left:P::equivalent;
      Check(c::CompareLocalLatchVictimPreference(a,b)==expected,"unsigned canonical binary byte comparison");
      Check(c::CompareLocalLatchVictimPreference(b,a)==(expected==P::left?P::right:expected==P::right?P::left:P::equivalent),
            "binary tie reverse comparison");
    }
  }
  // Highest-order differing byte wins even when host-endian integer ordering
  // or a later byte would select the other task.
  auto a=Candidate(255,1), b=Candidate(0,1); b.task_id[0]=1;
  Check(c::CompareLocalLatchVictimPreference(a,b)==P::right,"first canonical byte dominates trailing byte");
  for (unsigned category=0;category<256;++category) {
    if (category>=1 && category<=5) continue;
    Check(c::CompareLocalLatchVictimPreference(Candidate(1,category),Candidate(2,1))==P::invalid,
          "every invalid category byte rejected");
  }
  for (unsigned version=0;version<16;++version) {
    if (version==7) continue;
    auto bad=Candidate(1,1); bad.task_id[6]=static_cast<std::uint8_t>(version<<4);
    Check(c::CompareLocalLatchVictimPreference(bad,Candidate(2,1))==P::invalid,"non-system UUID version rejected");
  }
  for (unsigned variant=0;variant<4;++variant) {
    if (variant==2) continue;
    auto bad=Candidate(1,1); bad.task_id[8]=static_cast<std::uint8_t>(variant<<6);
    Check(c::CompareLocalLatchVictimPreference(Candidate(2,1),bad)==P::invalid,"malformed UUID variant rejected");
  }
  auto nil=Candidate(1,1); nil.task_id={};
  Check(c::CompareLocalLatchVictimPreference(nil,Candidate(2,1))==P::invalid,"nil task cannot be candidate");
  Check(c::CompareLocalLatchVictimPreference(Candidate(1,1),Candidate(1,2))==P::invalid,
        "conflicting phase categories for one task invalidate snapshot");
}
void Select() {
  std::array candidates{Candidate(7,2),Candidate(3,1),Candidate(5,1),Candidate(255,5)};
  std::array<unsigned,4> order{0,1,2,3};
  do {
    std::array<c::LocalLatchVictimCandidate,4> input;
    for (unsigned i=0;i<4;++i) input[i]=candidates[order[i]];
    const auto saved=input;
    const auto selected=c::SelectLocalLatchVictimPreference(input,4);
    Check(selected.status==S::selected && selected.candidate==candidates[2],
          "all permutations select highest task within first eligible category");
    Check(input==saved,"selection does not mutate borrowed graph observations");
  } while (std::next_permutation(order.begin(),order.end()));
  for (unsigned category=1;category<=5;++category) {
    std::array one{Candidate(9,category)};
    Check(c::SelectLocalLatchVictimPreference(one,1).candidate==one[0],"each already-eligible category selectable");
  }
  std::array duplicates{Candidate(3,1),Candidate(5,1),Candidate(3,1)};
  Check(c::SelectLocalLatchVictimPreference(duplicates,3).candidate==duplicates[1],
        "equivalent duplicates do not gain preference");
  for (unsigned position=0;position<3;++position) {
    auto bad=duplicates; bad[position].task_id={};
    const auto invalid=c::SelectLocalLatchVictimPreference(bad,3);
    Check(invalid.status==S::invalid && !invalid.candidate,"invalid record never silently omitted");
  }
  std::array conflict{Candidate(255,1),Candidate(3,3),Candidate(3,4)};
  const auto invalid=c::SelectLocalLatchVictimPreference(conflict,3);
  Check(invalid.status==S::invalid && !invalid.candidate,"nonwinning conflicting duplicate still invalidates selection");
  const auto empty=c::SelectLocalLatchVictimPreference({},1);
  Check(empty.status==S::empty && !empty.candidate,"empty eligible set has no fabricated victim");
  const auto bound=c::SelectLocalLatchVictimPreference(candidates,3);
  Check(bound.status==S::exhausted && !bound.candidate,"candidate bound before partial selection");
  Check(c::SelectLocalLatchVictimPreference({},0).status==S::invalid,"absent candidate bound invalid");
}
}
void* operator new(std::size_t bytes) {
  if (forbid_heap) { ++heap_attempts; throw std::bad_alloc(); }
  if (auto* p=std::malloc(bytes?bytes:1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
int main() {
  forbid_heap=true;
  Compare(); Select();
  forbid_heap=false;
  Check(heap_attempts==0,"policy comparison and selection perform no heap allocation");
  std::printf("PASS %u local latch victim preference checks; not graph or resolution authority\n",checks);
}

