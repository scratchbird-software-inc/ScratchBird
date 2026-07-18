// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_execution_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fb = scratchbird::parser::firebird;
namespace ipc = scratchbird::parser::ipc;

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
  // Twenty-two bounded route-shape representatives cover the 14 legitimate
  // baseline and eight native-violation execution classes. Each statement
  // below must classify into the same
  // standalone Firebird ALTER route; runtime outcome is engine data/MGA
  // dependent, never a parser overlay decision.
  const std::vector<std::string> d1_cases = {
      "ALTER TABLE detail_table ADD CONSTRAINT integ_1 FOREIGN KEY (fkey) REFERENCES master_table (pkey)",
      "alter table detail_table add constraint integ_2 foreign key (fkey) references master_table (pkey)",
      "ALTER TABLE D1_CHILD_03 ADD FOREIGN KEY (FK) REFERENCES D1_PARENT_03 (PK)",
      "ALTER TABLE D1_CHILD_04 ADD CONSTRAINT FK_04 FOREIGN KEY(FK) REFERENCES D1_PARENT_04(PK)",
      "ALTER TABLE \"D1_CHILD_05\" ADD CONSTRAINT \"FK_05\" FOREIGN KEY (\"FK\") REFERENCES \"D1_PARENT_05\" (\"PK\")",
      "ALTER TABLE D1_CHILD_06 ADD CONSTRAINT FK_06 FOREIGN KEY (FKEY) REFERENCES D1_PARENT_06 (PKEY);",
      "ALTER\nTABLE D1_CHILD_07 ADD CONSTRAINT FK_07 FOREIGN KEY (FKEY) REFERENCES D1_PARENT_07 (PKEY)",
      "ALTER TABLE D1_CHILD_08 ADD CONSTRAINT FK_08 FOREIGN\nKEY (FKEY)\nREFERENCES D1_PARENT_08 (PKEY)",
      "ALTER TABLE D1_CHILD_09 ADD CONSTRAINT FK_09 FOREIGN KEY (FKEY) REFERENCES D1_PARENT_09(PKEY)",
      "ALTER TABLE D1_CHILD_10 ADD CONSTRAINT FK_10 FOREIGN KEY(FKEY) REFERENCES D1_PARENT_10 (PKEY)",
      "ALTER TABLE D1_CHILD_11 ADD CONSTRAINT FK_11 FOREIGN KEY(FKEY)REFERENCES D1_PARENT_11(PKEY)",
      "ALTER TABLE D1_CHILD_12 ADD CONSTRAINT FK_12 FOREIGN\tKEY (FKEY) REFERENCES D1_PARENT_12 (PKEY)",
      "ALTER TABLE D1_CHILD_13 ADD CONSTRAINT FK_13 FOREIGN KEY (FKEY) REFERENCES D1_PARENT_13 (PKEY)",
      "ALTER TABLE D1_CHILD_14 ADD CONSTRAINT FK_14 FOREIGN KEY (FKEY) REFERENCES D1_PARENT_14 (PKEY)",
      "ALTER TABLE PK14_DETAIL ADD CONSTRAINT INTEG_14 FOREIGN KEY (FKEY) REFERENCES PK14_MASTER (PKEY)",
      "ALTER TABLE PK18_DETAIL ADD CONSTRAINT INTEG_18 FOREIGN KEY (FKEY) REFERENCES PK18_MASTER (PKEY)",
      "ALTER TABLE UNIQUE_INSERT02_DETAIL ADD CONSTRAINT INTEG_I02 FOREIGN KEY (FKEY) REFERENCES UNIQUE_INSERT02_MASTER (PKEY)",
      "ALTER TABLE UNIQUE_INSERT03_DETAIL ADD CONSTRAINT INTEG_I03 FOREIGN KEY (FKEY) REFERENCES UNIQUE_INSERT03_MASTER (PKEY)",
      "ALTER TABLE UNIQUE_INSERT04_DETAIL ADD CONSTRAINT INTEG_I04 FOREIGN KEY (FKEY) REFERENCES UNIQUE_INSERT04_MASTER (PKEY)",
      "ALTER TABLE UNIQUE_INSERT08_DETAIL ADD CONSTRAINT INTEG_I08 FOREIGN KEY (FKEY) REFERENCES UNIQUE_INSERT08_MASTER (PKEY)",
      "ALTER TABLE UNIQUE_INSERT12_DETAIL ADD CONSTRAINT INTEG_I12 FOREIGN KEY (FKEY) REFERENCES UNIQUE_INSERT12_MASTER (PKEY)",
      "ALTER TABLE UNIQUE_INSERT13_DETAIL ADD CONSTRAINT INTEG_I13 FOREIGN KEY (FKEY) REFERENCES UNIQUE_INSERT13_MASTER (PKEY)"};

  fb::FirebirdExecutionSession session({});
  const ipc::ParserTransactionSelector no_transaction{};
  for (const auto& sql : d1_cases) {
    const auto route = fb::ParseFirebirdForeignKeyAlterRoute(sql);
    Require(route.attempted, "D1 ALTER did not preempt metadata classification");
    Require(route.supported && route.recognized(),
            "D1 ALTER did not classify as exact single-column FK");
    Require(!route.child_table_name.empty() &&
                !route.child_column_name.empty() &&
                !route.parent_table_name.empty() &&
                !route.parent_column_name.empty(),
            "D1 ALTER lost a required presented identifier");
    const auto lowered =
        session.RunStatement(sql, no_transaction, false, false, 0, false);
    Require(lowered.accepted,
            "parse-only D1 ALTER was not lowered by sbp_firebird");
    Require(lowered.sblr_payload.find("ddl.constraint.alter") !=
                std::string::npos &&
                lowered.sblr_payload.find(
                    "descriptor_version=neutral_fk_single_column_v1") !=
                    std::string::npos,
            "D1 ALTER did not emit the neutral FK descriptor");
    Require(lowered.sblr_payload.find("constraint_mutation_batch_state") ==
                std::string::npos &&
                lowered.sblr_payload.find("support_uuid=") ==
                    std::string::npos &&
                lowered.sblr_payload.find("key_descriptor_uuid=") ==
                    std::string::npos,
            "standalone parser attempted to supply an engine identity/seal");
  }

  const std::vector<std::string> refused = {
      "ALTER TABLE C ADD CONSTRAINT F FOREIGN KEY (A, B) REFERENCES P (A, B)",
      "ALTER TABLE C ADD CONSTRAINT F FOREIGN KEY (A) REFERENCES P (A) ON DELETE CASCADE",
      "ALTER TABLE C ADD CONSTRAINT F FOREIGN KEY (A) REFERENCES P (A) ON UPDATE SET NULL",
      "ALTER TABLE C ADD CONSTRAINT F FOREIGN\nKEY (A) REFERENCES P (A) ON\nDELETE CASCADE",
      "ALTER TABLE C ADD CONSTRAINT F FOREIGN KEY (A) REFERENCES P (A) DEFERRABLE",
      "ALTER TABLE C ADD CONSTRAINT F FOREIGN KEY (A) REFERENCES P (A) INITIALLY DEFERRED"};
  for (const auto& sql : refused) {
    const auto route = fb::ParseFirebirdForeignKeyAlterRoute(sql);
    Require(route.attempted && !route.supported && !route.recognized(),
            "out-of-scope FK shape did not fail closed");
  }

  const std::vector<std::string> not_foreign_keys = {
      "ALTER TABLE C ADD \"FOREIGN KEY\" INTEGER",
      "ALTER TABLE C ADD NOTE VARCHAR(32) DEFAULT 'REFERENCES'"};
  for (const auto& sql : not_foreign_keys) {
    const auto route = fb::ParseFirebirdForeignKeyAlterRoute(sql);
    Require(!route.attempted && !route.supported && !route.recognized(),
            "quoted/literal referential text was claimed as a foreign key");
  }

  std::cout << "firebird foreign-key D1 route-shape gate passed: 22 cases\n";
  return EXIT_SUCCESS;
}
