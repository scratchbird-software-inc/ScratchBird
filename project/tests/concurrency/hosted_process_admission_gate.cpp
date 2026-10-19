// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "hosted_process_admission.hpp"
#include <barrier>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace s=scratchbird::server;
using Error=s::HostedProcessAdmissionError;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class Work>void Child(int expected,Work&& work) {
  const auto child=::fork();Check(child>=0,"fork custody probe");
  if(!child){std::set_terminate([]{::_exit(73);});work();::_exit(74);}
  int status=0;pid_t waited;
  do {waited=::waitpid(child,&status,0);} while(waited<0&&errno==EINTR);
  Check(waited==child&&WIFEXITED(status)&&WEXITSTATUS(status)==expected,"custody probe exit");
}
void RetryAndPermanentBinding() {
  s::HostedProcessAdmission owner;
  Check(owner.InOwningProcess(),"real process custody");
  struct OpenFailure {};
  try {
    auto opening=owner.TryReserve();Check(opening.ok(),"first reservation");
    Check(owner.TryReserve().error==Error::opening,"reentrant callback cannot reserve another hosted open");
    throw OpenFailure{};
  } catch(const OpenFailure&) {}
  auto retry=owner.TryReserve();Check(retry.ok(),"exception unwinding releases failed open reservation");
  auto moved=std::move(retry.reservation);
  Check(!retry.reservation&&moved&&owner.TryReserve().error==Error::opening,"move retains same exclusive reservation");
  retry.reservation.Reset();
  Check(owner.TryReserve().error==Error::opening,"erasing moved-from handle cannot release live reservation");
  moved.Reset();Check(!moved&&!moved.Commit(),"reset invalidates reservation without committing");
  auto final=owner.TryReserve();Check(final.ok()&&final.reservation.Commit(),"successful hosted open binds process");
  final.reservation.Reset();
  Check(owner.TryReserve().error==Error::bound,"snapshot/custody retirement cannot retarget a bound host");
}
void ConcurrentOpening() {
  s::HostedProcessAdmission owner;
  constexpr unsigned count=16;
  std::barrier start(count),attempted(count);
  std::atomic<unsigned> winners=0,wrong=0;
  std::vector<std::thread> callers;
  for(unsigned n=0;n<count;++n)callers.emplace_back([&]{
    start.arrive_and_wait();auto result=owner.TryReserve();
    if(result.ok())++winners;else if(result.error!=Error::opening)++wrong;
    // Every caller finishes its attempt while the winning reservation is
    // still alive. No scheduler-dependent assumption about overlap or sleep.
    attempted.arrive_and_wait();if(result.ok()&&!result.reservation.Commit())++wrong;
  });
  for(auto& caller:callers)caller.join();
  Check(winners==1&&wrong==0&&owner.TryReserve().error==Error::bound,"exactly one concurrent open binds the process");
}
void InheritedProcess() {
  s::HostedProcessAdmission owner;auto held=owner.TryReserve();Check(held.ok(),"parent reservation");
  Child(0,[&]{
    auto inherited=owner.TryReserve();
    ::_exit(!owner.InOwningProcess()&&inherited.error==Error::wrong_process&&!inherited.reservation?0:74);
  });
  Child(73,[&]{(void)held.reservation.Commit();});
  Child(73,[&]{auto moved=std::move(held.reservation);(void)moved;});
  held.reservation.Reset();
  Child(73,[&]{(void)bool(held.reservation);});
  auto retry=owner.TryReserve();Check(retry.ok(),"child refusal preserves parent's retryable owner");
}
void ForeignThread() {
  for(unsigned mode=0;mode<7;++mode)Child(73,[&]{
    // This owner is created in this child, so the refusal being exercised is
    // foreign-thread use, independently of the inherited-process tests.
    s::HostedProcessAdmission owner;auto held=owner.TryReserve();
    if(mode>=4)held.reservation.Reset();
    std::thread foreign([&]{
      switch(mode){
        case 0:case 4:(void)bool(held.reservation);break;
        case 1:case 5:{auto moved=std::move(held.reservation);(void)moved;break;}
        case 2:case 6:held.reservation.Reset();break;
        case 3:(void)held.reservation.Commit();break;
      }
    });foreign.join();
  });
}
int main() try {
  RetryAndPermanentBinding();ConcurrentOpening();InheritedProcess();ForeignThread();
  std::cout<<"hosted_process_admission=PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
