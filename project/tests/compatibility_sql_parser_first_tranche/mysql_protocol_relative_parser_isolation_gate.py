#!/usr/bin/env python3
"""Reject direct dependencies on the MySQL-owned parser implementation."""
import argparse,json,pathlib,re
FAMILIES=("mariadb","tidb","vitess","dolt")
FOREIGN=re.compile(r"sbl_mysql_parser_pipeline|src/parsers/compatibility/mysql|mysql_worker_session|parser::mysql|ServeMysql",re.I)
SEMANTIC=re.compile(r"\b(?:ParseStatement|ParseResult|DialectProfile|sblr_envelope|statement_family|Render)\b")
def need(v,m):
 if not v: raise AssertionError(m)
def main():
 p=argparse.ArgumentParser();p.add_argument("--repo-root",required=True,type=pathlib.Path);p.add_argument("--evidence-file",required=True,type=pathlib.Path);a=p.parse_args();r=a.repo_root.resolve();base=r/"project/src/parsers/compatibility";rows=[]
 for f in FAMILIES:
  root=base/f; text="\n".join(x.read_text() for x in root.iterdir() if x.suffix in {".cpp",".hpp",".txt"});m=FOREIGN.search(text);need(not m,f"{f} foreign parser edge: {m.group(0) if m else ''}")
  stem=f"{f}_worker_session";cm=(root/"CMakeLists.txt").read_text();main=(root/"main.cpp").read_text();need((root/f"{stem}.cpp").is_file(),f"{f} worker missing");need(f"{stem}.cpp" in cm,f"{f} worker not built");need(f'#include "{stem}.hpp"' in main,f"{f} main lacks own worker");need(f"parser::{f}::Serve" in main,f"{f} main lacks own namespace");rows.append({"family":f,"mysql_parser_dependency_count":0,"own_worker_session":True})
 codec="\n".join((base/"common"/x).read_text() for x in ("mywire_frame_codec.hpp","mywire_frame_codec.cpp"));m=SEMANTIC.search(codec);need(not m,f"frame codec semantic edge: {m.group(0) if m else ''}")
 fixtures=("sbl_mysql_parser_pipeline",'#include "mysql_worker_session.hpp"',"parser::mysql::ServeMysqlWorkerSession(fd)");need(all(FOREIGN.search(x) for x in fixtures),"negative scanner fixture escaped")
 out={"gate":"mysql_protocol_relative_parser_isolation_gate","status":"passed","families":rows,"mysql_parser_dependency_count":0,"neutral_codec_semantic_edge_count":0,"negative_fixture_count":len(fixtures),"global_shared_semantic_engine_audit_required":True,"parser_transaction_finality_authority":False,"mga_transaction_authority":"scratchbird_engine"};a.evidence_file.parent.mkdir(parents=True,exist_ok=True);a.evidence_file.write_text(json.dumps(out,indent=2,sort_keys=True)+"\n");print("mysql_protocol_relative_parser_isolation_gate=passed")
if __name__=="__main__":
 try: main()
 except AssertionError as e: print(f"mysql_protocol_relative_parser_isolation_gate: {e}");raise SystemExit(1)
