// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#if defined(SB_TEST_MARIADB)
#include "mariadb_worker_session.hpp"
constexpr const char* kVersion="ScratchBird-MariaDB";
int Serve(int fd){return scratchbird::parser::mariadb::ServeMariadbWorkerSession(fd);}
#elif defined(SB_TEST_TIDB)
#include "tidb_worker_session.hpp"
constexpr const char* kVersion="TiDB-ScratchBird";
int Serve(int fd){return scratchbird::parser::tidb::ServeTidbWorkerSession(fd);}
#elif defined(SB_TEST_VITESS)
#include "vitess_worker_session.hpp"
constexpr const char* kVersion="Vitess-ScratchBird";
int Serve(int fd){return scratchbird::parser::vitess::ServeVitessWorkerSession(fd);}
#elif defined(SB_TEST_DOLT)
#include "dolt_worker_session.hpp"
constexpr const char* kVersion="Dolt-ScratchBird";
int Serve(int fd){return scratchbird::parser::dolt::ServeDoltWorkerSession(fd);}
#else
#error family definition required
#endif
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace {
#ifndef _WIN32
bool Io(int fd,void* data,std::size_t n,bool writing){auto* p=static_cast<std::uint8_t*>(data);std::size_t o=0;while(o<n){const auto r=writing ? ::write(fd,p+o,n-o) : ::read(fd,p+o,n-o);if(r>0)o+=r;else if(r<0&&errno==EINTR)continue;else return false;}return true;}
bool Send(int fd,std::uint8_t seq,std::vector<std::uint8_t> p){std::vector<std::uint8_t> f{static_cast<std::uint8_t>(p.size()),static_cast<std::uint8_t>(p.size()>>8),static_cast<std::uint8_t>(p.size()>>16),seq};f.insert(f.end(),p.begin(),p.end());return Io(fd,f.data(),f.size(),true);}
bool Read(int fd,std::uint8_t* seq,std::vector<std::uint8_t>* p){std::uint8_t h[4];if(!Io(fd,h,4,false))return false;auto n=std::uint32_t(h[0])|(std::uint32_t(h[1])<<8)|(std::uint32_t(h[2])<<16);*seq=h[3];p->assign(n,0);return !n||Io(fd,p->data(),n,false);}
#endif
}
int main(){
#ifdef _WIN32
return 0;
#else
int s[2];if(::socketpair(AF_UNIX,SOCK_STREAM,0,s))return 1;int status=-1;
std::thread worker([&]{status=Serve(s[1]);::close(s[1]);});std::uint8_t seq=0;std::vector<std::uint8_t> p;
bool ok=Read(s[0],&seq,&p)&&std::string(p.begin(),p.end()).find(kVersion)!=std::string::npos;
ok=ok&&Send(s[0],1,{0,0,0,0})&&Read(s[0],&seq,&p)&&!p.empty()&&p[0]==0;
std::vector<std::uint8_t> q{3};const std::string sql="select 1";q.insert(q.end(),sql.begin(),sql.end());ok=ok&&Send(s[0],0,q);
bool row=false;for(int i=0;ok&&i<5;++i){ok=Read(s[0],&seq,&p);if(ok&&seq==4&&p.size()>=2&&p.back()=='1')row=true;}
ok=ok&&row&&Send(s[0],0,{1});::close(s[0]);worker.join();if(!ok||status!=0){std::cerr<<"family-owned packet worker probe failed\n";return 1;}return 0;
#endif
}
