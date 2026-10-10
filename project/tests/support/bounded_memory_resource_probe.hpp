// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <memory_resource>
#include <string>

namespace scratchbird::test_support {
class BoundedMemoryProbe final : public std::pmr::memory_resource {
 public:
  std::size_t limit=16*1024*1024, live=0, peak=0, calls=0, releases=0;
  long fail_after=-1;
 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (fail_after==0 || bytes>limit-live) throw std::bad_alloc();
    if (fail_after>0) --fail_after;
    auto* result=std::pmr::new_delete_resource()->allocate(bytes,alignment);
    ++calls;live+=bytes;peak=std::max(peak,live);return result;
  }
  void do_deallocate(void* p,std::size_t bytes,std::size_t alignment) override {
    ++releases;live-=bytes;std::pmr::new_delete_resource()->deallocate(p,bytes,alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& r)const noexcept override{return this==&r;}
};
template<class Invoke,class Check,class Status>
void QualifyBoundedUnicodeMemory(Invoke invoke,Check check,const std::string& expected,
                                Status ok,Status allocation_failure) {
  struct RestoreDefault {
    std::pmr::memory_resource* saved=std::pmr::set_default_resource(std::pmr::null_memory_resource());
    ~RestoreDefault(){std::pmr::set_default_resource(saved);}
  } restore;
  BoundedMemoryProbe baseline;
  {
    std::pmr::string result("sentinel",&baseline);
    check(invoke(&result)==ok && std::string_view(result)==expected,"bounded Unicode positive/default-resource bypass");
  }
  check(baseline.peak>expected.size()&&baseline.live==0&&baseline.calls==baseline.releases,
        "Unicode scratch unmeasured or leaked");
  for(std::size_t fail=0;fail<baseline.calls;++fail){
    BoundedMemoryProbe memory;memory.fail_after=static_cast<long>(fail);
    {
      std::pmr::string result("sentinel",&memory);
      check(invoke(&result)==allocation_failure&&result=="sentinel","Unicode allocation failure not atomic");
    }
    check(memory.live==0&&memory.calls==memory.releases,"Unicode failed path leaked admitted memory");
  }
  for(const auto limit:{std::size_t(0),baseline.peak-1,baseline.peak}){
    BoundedMemoryProbe memory;memory.limit=limit;
    {
      std::pmr::string result("sentinel",&memory);
      auto status=invoke(&result);
      check(limit==baseline.peak ? status==ok&&std::string_view(result)==expected
          : status==allocation_failure&&result=="sentinel","Unicode exact peak boundary/atomicity");
    }
    check(memory.peak<=limit&&memory.live==0&&memory.calls==memory.releases,
          "Unicode allocation exceeded admission or leaked");
  }
}
} // namespace scratchbird::test_support
