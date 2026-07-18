// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "tidb_worker_session.hpp"
#include "mywire_frame_codec.hpp"
#include "tidb_dialect.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::tidb {
namespace {
namespace wire = scratchbird::parser::compatibility::mywire;
constexpr std::uint32_t kProtocol41=0x200U,kTransactions=0x2000U,
 kSecure=0x8000U,kMulti=0x20000U,kPlugin=0x80000U,kAttrs=0x100000U,
 kDeprecateEof=0x1000000U;
constexpr std::uint32_t kCapabilities=1U|4U|8U|kProtocol41|kTransactions|kSecure|
 0x10000U|kMulti|kPlugin|kAttrs|0x200000U;

std::vector<std::uint8_t> Handshake(){std::vector<std::uint8_t> p{10};
 wire::AppendNullString(&p,"8.5.3-TiDB-ScratchBird");
#ifdef _WIN32
 wire::AppendU32(&p,0);
#else
 wire::AppendU32(&p,static_cast<std::uint32_t>(::getpid()));
#endif
 const std::string a="sbtidb01";p.insert(p.end(),a.begin(),a.end());p.push_back(0);
 wire::AppendU16(&p,static_cast<std::uint16_t>(kCapabilities));p.push_back(45);
 wire::AppendU16(&p,2);wire::AppendU16(&p,static_cast<std::uint16_t>(kCapabilities>>16));
 p.push_back(21);p.insert(p.end(),10,0);const std::string b="sbtidb02auth";
 p.insert(p.end(),b.begin(),b.end());p.push_back(0);
 wire::AppendNullString(&p,"caching_sha2_password");return p;}
std::vector<std::uint8_t> Ok(){std::vector<std::uint8_t> p{0};
 wire::AppendLengthEncodedInteger(&p,0);wire::AppendLengthEncodedInteger(&p,0);
 wire::AppendU16(&p,2);wire::AppendU16(&p,0);return p;}
std::vector<std::uint8_t> Eof(){std::vector<std::uint8_t> p{0xfe};
 wire::AppendU16(&p,0);wire::AppendU16(&p,2);return p;}
std::vector<std::uint8_t> Error(std::string_view m){std::vector<std::uint8_t> p{0xff};
 wire::AppendU16(&p,1064);p.push_back('#');p.insert(p.end(),{'4','2','0','0','0'});
 p.insert(p.end(),m.begin(),m.end());return p;}
std::string Message(std::string_view j){constexpr std::string_view k="\"message\":\"";
 auto a=j.find(k);if(a==j.npos)return "TiDB parser refused the statement";a+=k.size();
 auto b=j.find('"',a);return std::string(j.substr(a,b==j.npos?j.npos:b-a));}
bool Probe(std::string_view s){auto n=ToUpperAscii(NormalizeWhitespace(TrimAscii(s)));
 while(!n.empty()&&n.back()==';')n.pop_back();return n=="SELECT 1"||
 n=="SELECT 1 AS SB_REFERENCE_PROBE"||n=="SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE";}
std::vector<std::uint8_t> Column(){std::vector<std::uint8_t> p;
 for(auto v:{"def","","","","sb_reference_probe",""})wire::AppendLengthEncodedString(&p,v);
 p.push_back(12);wire::AppendU16(&p,45);wire::AppendU32(&p,1024);p.push_back(0xfd);
 wire::AppendU16(&p,0);p.push_back(0);p.push_back(0);return p;}
bool Render(int fd,std::string_view sql,bool deprecate){auto r=ParseStatement(sql);
 if(!r.ok||r.fail_closed_refusal)return wire::WritePacket(fd,1,Error(Message(r.message_vector_json)));
 if(!Probe(sql))return wire::WritePacket(fd,1,Ok());auto end=deprecate?Ok():Eof();
 std::vector<std::uint8_t> row;wire::AppendLengthEncodedString(&row,"1");
 return wire::WritePacket(fd,1,{1})&&wire::WritePacket(fd,2,Column())&&
 wire::WritePacket(fd,3,end)&&wire::WritePacket(fd,4,row)&&wire::WritePacket(fd,5,end);}
}
int ServeTidbWorkerSession(int fd){
#ifdef _WIN32
 (void)fd;return 1;
#else
 if(!wire::WritePacket(fd,0,Handshake()))return 1;wire::Packet p;
 if(!wire::ReadPacket(fd,&p))return 1;bool deprecate=(wire::DecodeU32(p.payload.data(),p.payload.size())&kDeprecateEof)!=0;
 if(!wire::WritePacket(fd,2,Ok()))return 1;for(;;){if(!wire::ReadPacket(fd,&p))return 0;
 if(p.payload.empty())continue;auto c=p.payload[0];if(c==1)return 0;
 if(c==14){if(!wire::WritePacket(fd,1,Ok()))return 1;continue;}
 if(c==3){std::string sql(p.payload.begin()+1,p.payload.end());if(!Render(fd,sql,deprecate))return 1;continue;}
 if(!wire::WritePacket(fd,1,Error("unsupported TiDB frontend command")))return 1;}
#endif
}
}
