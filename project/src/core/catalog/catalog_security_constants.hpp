// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_CATALOG_SECURITY_CONSTANTS (owner Q62 = 1, 2026-11-12). The
// engine's security rights are coding-time constants: a code fixed forever
// (append-only) and a permanent UUID (generated once 2026-11-12, recorded
// before use), its identity in the names table. A runtime GRANT is a catalog
// row naming a right by this constant and its target by UUID. GENERATED from
// the recorded list; never edit by hand. `tag` is the boundary spelling only
// (the policy-pack import adapter maps text once); engine code compares the
// constant.
//
// Also the small security classifications (hand-written below the table).
#include "runtime_platform.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {

enum class CatalogSecurityRight : scratchbird::core::platform::u16 {
  none = 0,  // not a right; never stored
  CONNECT = 1,  // 01a1cfef-2065-7ec8-a0e5-d5b187302a54
  VISIBLE = 2,  // 01a1cfef-2066-788a-8886-be6ec9a5a82c
  DISCOVER = 3,  // 01a1cfef-2067-7bc0-a546-c1297218a168
  LIST_CHILD = 4,  // 01a1cfef-2068-70aa-b567-1e6c6fb28557
  READ_DIAGNOSTIC_DETAIL = 5,  // 01a1cfef-2069-7c60-957e-e1fa41ad2c16
  SELECT = 6,  // 01a1cfef-206a-7256-87ba-598408a73d5d
  INSERT = 7,  // 01a1cfef-206b-78e2-b2d8-458374cb5605
  UPDATE = 8,  // 01a1cfef-206c-79c4-8e5a-7a26836acc75
  DELETE = 9,  // 01a1cfef-206d-7ae4-b27d-785edda1caba
  EXECUTE = 10,  // 01a1cfef-206e-76e3-a654-7116f029161d
  CREATE = 11,  // 01a1cfef-206f-7dde-b224-6614602714fd
  ALTER = 12,  // 01a1cfef-2070-7a46-a5ab-212c2c97407b
  DROP = 13,  // 01a1cfef-2071-7317-b362-2724cead97c7
  CATALOG_MUTATE = 14,  // 01a1cfef-2072-7134-8dba-07a7efbe8e5a
  USAGE = 15,  // 01a1cfef-2073-77cd-bad1-90286102fe73
  TYPE_DDL = 16,  // 01a1cfef-2074-76e5-a786-15bedd469082
  CONTEXT_READ = 17,  // 01a1cfef-2075-7522-83f4-8116c2482197
  CONTEXT_WRITE = 18,  // 01a1cfef-2076-76c1-a27b-3e27dccdbec2
  DOMAIN_USE = 19,  // 01a1cfef-2077-7543-a89f-09efc10dadcb
  DOMAIN_CAST = 20,  // 01a1cfef-2078-78ed-84d6-758518d666c7
  DOMAIN_METHOD = 21,  // 01a1cfef-2079-7ac5-8bda-3219cd9c8df8
  DOMAIN_POLICY_ADMIN = 22,  // 01a1cfef-207a-7a24-bcda-75dd345207ef
  DOMAIN_UNMASK = 23,  // 01a1cfef-207b-7172-aa57-9e22097ed493
  UNMASK = 24,  // 01a1cfef-207c-70ac-b7cf-656512f4089d
  POLICY_ADMIN = 25,  // 01a1cfef-207d-7b4e-a4c9-37ce7f33d0a2
  OBS_METRICS_READ_SELF = 26,  // 01a1cfef-207e-7ee0-be47-12f81e52ab64
  OBS_METRICS_READ_ALL = 27,  // 01a1cfef-207f-7d99-9b50-468998ffb789
  OBS_METRICS_READ_FAMILY = 28,  // 01a1cfef-2080-7c3f-97af-95d23abbf2df
  OBS_METRICS_READ_DATABASE = 29,  // 01a1cfef-2081-7c61-966e-94f4e69dad59
  OBS_METRICS_READ_NODE = 30,  // 01a1cfef-2082-7b88-869a-71207896d349
  OBS_METRICS_READ_CLUSTER = 31,  // 01a1cfef-2083-7b81-8faa-a6c43f0fb6e6
  OBS_METRICS_EXPORT = 32,  // 01a1cfef-2084-79e6-8606-7b6c50db1978
  OBS_METRICS_EXPORT_READ = 33,  // 01a1cfef-2085-77c2-b219-165efcaa4f3a
  SEC_AUTH_METRICS_READ = 34,  // 01a1cfef-2086-76d3-a38e-331f49669803
  OBS_POLICY_READ = 35,  // 01a1cfef-2087-7a7d-b825-240917a38cc7
  OBS_AGENT_STATE_READ = 36,  // 01a1cfef-2088-7963-bea9-b0214acffd99
  OBS_AGENT_EVIDENCE_READ = 37,  // 01a1cfef-2089-776a-b02e-3a7155cc697e
  OBS_AGENT_CONTROL = 38,  // 01a1cfef-208a-7d97-8a32-4a33b7ef51cb
  OBS_AGENT_POLICY_CONTROL = 39,  // 01a1cfef-208b-75b2-95a5-2a1bc7c4b963
  OBS_AGENT_OVERRIDE = 40,  // 01a1cfef-208c-79b6-953e-d27d0ad3ed7d
  OBS_AGENT_RECOMMENDATION_READ = 41,  // 01a1cfef-208d-7dfe-9aae-ce3f3e65caae
  OBS_AGENT_ACTION_APPROVE = 42,  // 01a1cfef-208e-7ada-b906-18b7521f0d01
  OBS_AGENT_ACTION_CANCEL = 43,  // 01a1cfef-208f-72b1-8070-a2ed0cf95543
  OBS_SUPPORT_BUNDLE_READ = 44,  // 01a1cfef-2090-7809-b197-748d33bbb0d7
  OBS_POLICY_SIMULATE = 45,  // 01a1cfef-2091-7248-afbc-3a8dd17251cb
  OBS_POLICY_EDIT_DRAFT = 46,  // 01a1cfef-2092-7c6d-a1c3-40185a660cea
  OBS_POLICY_APPROVE = 47,  // 01a1cfef-2093-7eb2-a328-84e0c66ec07f
  OBS_POLICY_APPLY = 48,  // 01a1cfef-2094-7959-8dc4-722704e55de4
  OBS_POLICY_ROLLBACK = 49,  // 01a1cfef-2095-75c7-ab5f-1499acf93fd9
  OBS_POLICY_DELETE = 50,  // 01a1cfef-2096-752f-95a3-78d8d1aa5ba7
  OBS_CLUSTER_TOPOLOGY_INSPECT = 51,  // 01a1cfef-2097-7b99-a4c6-1f72f90c932c
  SEC_REDACTION_POLICY_EDIT = 52,  // 01a1cfef-2098-7124-ad3b-46b8a7ee7209
  SEC_EXPORT_POLICY_APPROVE = 53,  // 01a1cfef-2099-7dad-a77f-997ff7607d6e
  OBS_POLICY_VALIDATE = 54,  // 01a1cfef-209a-7cca-a209-4739c72104fc
  OBS_METRICS_POLICY_INSPECT = 55,  // 01a1cfef-209b-7f82-83f8-ceb141399540
  OBS_METRICS_POLICY_CONTROL = 56,  // 01a1cfef-209c-7701-b92e-b1ef39f01622
  OBS_METRICS_EXPORT_CONTROL = 57,  // 01a1cfef-209d-7628-b9db-2450d70a17b6
  OBS_METRICS_RETENTION_CONTROL = 58,  // 01a1cfef-209e-72a8-8dfd-06b3c54a0338
  OBS_RUNTIME_SELF = 59,  // 01a1cfef-209f-7c69-86bb-6cba815ee6f9
  OBS_RUNTIME_ALL = 60,  // 01a1cfef-20a0-7e96-9d33-2be28d999b76
  OBS_INDEX_PROFILE_READ = 61,  // 01a1cfef-20a1-7ebb-b407-b35d3ddcc6ab
  OBS_MANAGEMENT_INSPECT = 62,  // 01a1cfef-20a2-7ed9-ac48-fb7877638b0f
  OBS_MANAGEMENT_CONTROL = 63,  // 01a1cfef-20a3-791f-bb1d-9958ebf55365
  OBS_CONFIG_INSPECT = 64,  // 01a1cfef-20a4-7f35-a376-3ca45c3b7573
  OBS_CONFIG_CONTROL = 65,  // 01a1cfef-20a5-7134-9873-98a6ec842d43
  OBS_CLUSTER_HEALTH_INSPECT = 66,  // 01a1cfef-20a6-7c17-8da0-56be3b9fe2d2
  OBS_CLUSTER_CONTROL = 67,  // 01a1cfef-20a7-7140-be4c-cf634585f4a7
  OBS_DATA_MOVEMENT_INSPECT = 68,  // 01a1cfef-20a8-7890-b83c-945ebab22d9b
  SEC_IDENTITY_ADMIN = 69,  // 01a1cfef-20a9-77af-99ce-9a73ec6ac4fb
  SEC_MEMBERSHIP_ADMIN = 70,  // 01a1cfef-20aa-70fa-ae13-f64799800b38
  SEC_GRANT_ADMIN = 71,  // 01a1cfef-20ab-7179-a641-7e0403508499
  MGA_TRANSACTION_INSPECT = 72,  // 01a1cfef-20ac-7e7d-83b7-6b494f3d4a2d
  MGA_RECOVERY_INSPECT = 73,  // 01a1cfef-20ad-7366-a40e-8ee681e3675d
  MGA_CLEANUP_INSPECT = 74,  // 01a1cfef-20ae-7457-86af-3a0f037a4908
  MGA_CLEANUP_CONTROL = 75,  // 01a1cfef-20af-7500-831b-f732c1c0a2c6
  MGA_HORIZON_INSPECT = 76,  // 01a1cfef-20b0-78a7-9be7-3d48656a9eff
  MGA_LINEAGE_INSPECT = 77,  // 01a1cfef-20b1-79b9-b7c9-a3cfc2bb97e8
  MGA_FORENSIC_INSPECT = 78,  // 01a1cfef-20b2-7572-934b-bac7fef38dc7
  MGA_METRICS_READ = 79,  // 01a1cfef-20b3-7d29-91fb-887d7014cd56
  AUTH_PROVIDER_ADMIN = 80,  // 01a1cfef-20b4-7f71-8880-e4b250774e0e
  AUDIT_READ = 81,  // 01a1cfef-20b5-789a-ac4a-8aa6c8c6b0fc
  AUDIT_ADMIN = 82,  // 01a1cfef-20b6-79ba-8aea-4ae9156a2c5d
  PROTECTED_MATERIAL_RELEASE = 83,  // 01a1cfef-20b7-771c-a67b-4a0474ac17b7
  KEY_RELEASE_APPROVE = 84,  // 01a1cfef-20b8-7f10-ad14-eb01d0b5b32c
  SUPPORT_EXPORT = 85,  // 01a1cfef-20b9-7281-8c71-31421b082e40
  UDR_TRUST_ADMIN = 86,  // 01a1cfef-20ba-7d85-ab3b-9b83c727fa4b
  UDR_MANAGE = 87,  // 01a1cfef-20bb-72fa-9d93-31218d929a9c
  UDR_INSPECT = 88,  // 01a1cfef-20bc-7855-b2ea-ce10fcd24799
  UDR_INVOKE = 89,  // 01a1cfef-20bd-77da-a9ee-2403cdac416e
  FILESPACE_LIFECYCLE_CONTROL = 90,  // 01a1cfef-20be-72f3-97b6-433f6192158e
  MIGRATE_DATABASE = 91,  // 01a1cfef-20bf-7e7c-893e-410db20621a0
  BACKUP_CREATE = 92,  // 01a1cfef-20c0-725f-b0a6-578ae9896d25
  BACKUP_RESTORE = 93,  // 01a1cfef-20c1-7746-a47a-cb3b7e01a634
  BACKUP_CONTROL = 94,  // 01a1cfef-20c2-7619-8e4a-72899fe0b4e2
  BACKUP_INSPECT = 95,  // 01a1cfef-20c3-79bc-8dc0-2d7f62dcda10
  SYS_BACKUP = 96,  // 01a1cfef-20c4-7d62-ac49-3c2dbe1dbf22
  EVENT_ADMIN = 97,  // 01a1cfef-20c5-7697-a5d6-5a9d30f87314
  EVENT_CREATE = 98,  // 01a1cfef-20c6-7203-a661-599c580c9fb0
  EVENT_ALTER = 99,  // 01a1cfef-20c7-7f98-ac91-acc7e88c0ecd
  EVENT_DROP = 100,  // 01a1cfef-20c8-7d0e-a834-3e730ed1a009
  EVENT_SUBSCRIBE = 101,  // 01a1cfef-20c9-7fbc-9bf4-f7c88aebecb6
  EVENT_PUBLISH = 102,  // 01a1cfef-20ca-7d24-b4f8-6a998c852438
  EVENT_DELIVERY_READ = 103,  // 01a1cfef-20cb-797e-891a-0c29ed3f772e
  EVENT_DELIVERY_ACK = 104,  // 01a1cfef-20cc-7f89-acb6-83909018cffc
  MANAGER_ADMISSION_ADMIN = 105,  // 01a1cfef-20cd-7279-bdef-909e06c9b284
  OBS_AGENT_CONTROL_INTERNAL = 106,  // 01a1cfef-20ce-7479-907a-5c5a7ea1e95b
  OBS_ALERT_INTERNAL = 107,  // 01a1cfef-20cf-785e-99e7-4baad556dc0c
  OBS_AGENT_EVIDENCE_APPEND = 108,  // 01a1cfef-20d0-7248-a84c-93c3b1bd5539
  OBS_RESTORE_DRILL_CONTROL = 109,  // 01a1cfef-20d1-7814-a3e3-0b7357aefbb1
  OBS_RESTORE_PLANNING = 110,  // 01a1cfef-20d2-7550-9d3a-94bea97eb1a1
  OBS_EXPORT_CONTROL = 111,  // 01a1cfef-20d3-7b55-98fd-1564377b51c3
  OBS_FILESPACE_TRUNCATE = 112,  // 01a1cfef-20d4-70ce-98a7-b8c749c306dd
  SEC_SESSION_KILL = 113,  // 01a1cfef-20d5-728e-afb5-4876ed2271c8
  SEC_SESSION_CONTROL = 114,  // 01a1cfef-20d6-72b1-990b-2fdb20de8bf0
  OBS_JOB_ADMIN = 115,  // 01a1cfef-20d7-7309-ad2e-56ba8b5c8bfb
  EXTERNAL_RANGE_MATERIALIZATION_ADMIN = 116,  // 01a1cfef-20d8-7dd6-afd3-3dd44d56b9fd
  OBS_AGENT_INTERNAL = 117,  // 01a1cfef-20d9-7c30-b786-28b0c5e8b7ab
  OBS_LISTENER_CONTROL = 118,  // 01a1cfef-20da-7e90-97f3-9d0b88429fb6
  OBS_PARSER_CONTROL = 119,  // 01a1cfef-20db-713f-8f4a-68c119e0c98f
  INDEX_MAINTENANCE_CONTROL = 120,  // 01a1cfef-20dc-7220-ac71-3526113c95e5
  ARCHIVE_CONTROL = 121,  // 01a1cfef-20dd-7ac4-a6fc-e30c721f0984
  ARCHIVE_EXPIRY_REMOVE = 122,  // 01a1cfef-20de-7993-b300-7b468f2cdc35
};
struct CatalogSecurityRightIdentity {
  CatalogSecurityRight right;
  scratchbird::core::platform::Uuid uuid;
  std::string_view tag;  // boundary spelling only
};
inline constexpr std::array<CatalogSecurityRightIdentity, 122> kCatalogSecurityRights{{
    CatalogSecurityRightIdentity{CatalogSecurityRight::CONNECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x65,0x7e,0xc8,0xa0,0xe5,0xd5,0xb1,0x87,0x30,0x2a,0x54}}, "CONNECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::VISIBLE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x66,0x78,0x8a,0x88,0x86,0xbe,0x6e,0xc9,0xa5,0xa8,0x2c}}, "VISIBLE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DISCOVER, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x67,0x7b,0xc0,0xa5,0x46,0xc1,0x29,0x72,0x18,0xa1,0x68}}, "DISCOVER"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::LIST_CHILD, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x68,0x70,0xaa,0xb5,0x67,0x1e,0x6c,0x6f,0xb2,0x85,0x57}}, "LIST_CHILD"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::READ_DIAGNOSTIC_DETAIL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x69,0x7c,0x60,0x95,0x7e,0xe1,0xfa,0x41,0xad,0x2c,0x16}}, "READ_DIAGNOSTIC_DETAIL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SELECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x6a,0x72,0x56,0x87,0xba,0x59,0x84,0x08,0xa7,0x3d,0x5d}}, "SELECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::INSERT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x6b,0x78,0xe2,0xb2,0xd8,0x45,0x83,0x74,0xcb,0x56,0x05}}, "INSERT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::UPDATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x6c,0x79,0xc4,0x8e,0x5a,0x7a,0x26,0x83,0x6a,0xcc,0x75}}, "UPDATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DELETE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x6d,0x7a,0xe4,0xb2,0x7d,0x78,0x5e,0xdd,0xa1,0xca,0xba}}, "DELETE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EXECUTE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x6e,0x76,0xe3,0xa6,0x54,0x71,0x16,0xf0,0x29,0x16,0x1d}}, "EXECUTE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::CREATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x6f,0x7d,0xde,0xb2,0x24,0x66,0x14,0x60,0x27,0x14,0xfd}}, "CREATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::ALTER, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x70,0x7a,0x46,0xa5,0xab,0x21,0x2c,0x2c,0x97,0x40,0x7b}}, "ALTER"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DROP, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x71,0x73,0x17,0xb3,0x62,0x27,0x24,0xce,0xad,0x97,0xc7}}, "DROP"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::CATALOG_MUTATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x72,0x71,0x34,0x8d,0xba,0x07,0xa7,0xef,0xbe,0x8e,0x5a}}, "CATALOG_MUTATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::USAGE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x73,0x77,0xcd,0xba,0xd1,0x90,0x28,0x61,0x02,0xfe,0x73}}, "USAGE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::TYPE_DDL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x74,0x76,0xe5,0xa7,0x86,0x15,0xbe,0xdd,0x46,0x90,0x82}}, "TYPE_DDL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::CONTEXT_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x75,0x75,0x22,0x83,0xf4,0x81,0x16,0xc2,0x48,0x21,0x97}}, "CONTEXT_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::CONTEXT_WRITE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x76,0x76,0xc1,0xa2,0x7b,0x3e,0x27,0xdc,0xcd,0xbe,0xc2}}, "CONTEXT_WRITE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DOMAIN_USE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x77,0x75,0x43,0xa8,0x9f,0x09,0xef,0xc1,0x0d,0xad,0xcb}}, "DOMAIN_USE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DOMAIN_CAST, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x78,0x78,0xed,0x84,0xd6,0x75,0x85,0x18,0xd6,0x66,0xc7}}, "DOMAIN_CAST"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DOMAIN_METHOD, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x79,0x7a,0xc5,0x8b,0xda,0x32,0x19,0xcd,0x9c,0x8d,0xf8}}, "DOMAIN_METHOD"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DOMAIN_POLICY_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x7a,0x7a,0x24,0xbc,0xda,0x75,0xdd,0x34,0x52,0x07,0xef}}, "DOMAIN_POLICY_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::DOMAIN_UNMASK, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x7b,0x71,0x72,0xaa,0x57,0x9e,0x22,0x09,0x7e,0xd4,0x93}}, "DOMAIN_UNMASK"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::UNMASK, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x7c,0x70,0xac,0xb7,0xcf,0x65,0x65,0x12,0xf4,0x08,0x9d}}, "UNMASK"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::POLICY_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x7d,0x7b,0x4e,0xa4,0xc9,0x37,0xce,0x7f,0x33,0xd0,0xa2}}, "POLICY_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_READ_SELF, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x7e,0x7e,0xe0,0xbe,0x47,0x12,0xf8,0x1e,0x52,0xab,0x64}}, "OBS_METRICS_READ_SELF"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_READ_ALL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x7f,0x7d,0x99,0x9b,0x50,0x46,0x89,0x98,0xff,0xb7,0x89}}, "OBS_METRICS_READ_ALL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_READ_FAMILY, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x80,0x7c,0x3f,0x97,0xaf,0x95,0xd2,0x3a,0xbb,0xf2,0xdf}}, "OBS_METRICS_READ_FAMILY"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_READ_DATABASE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x81,0x7c,0x61,0x96,0x6e,0x94,0xf4,0xe6,0x9d,0xad,0x59}}, "OBS_METRICS_READ_DATABASE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_READ_NODE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x82,0x7b,0x88,0x86,0x9a,0x71,0x20,0x78,0x96,0xd3,0x49}}, "OBS_METRICS_READ_NODE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_READ_CLUSTER, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x83,0x7b,0x81,0x8f,0xaa,0xa6,0xc4,0x3f,0x0f,0xb6,0xe6}}, "OBS_METRICS_READ_CLUSTER"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_EXPORT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x84,0x79,0xe6,0x86,0x06,0x7b,0x6c,0x50,0xdb,0x19,0x78}}, "OBS_METRICS_EXPORT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_EXPORT_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x85,0x77,0xc2,0xb2,0x19,0x16,0x5e,0xfc,0xaa,0x4f,0x3a}}, "OBS_METRICS_EXPORT_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_AUTH_METRICS_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x86,0x76,0xd3,0xa3,0x8e,0x33,0x1f,0x49,0x66,0x98,0x03}}, "SEC_AUTH_METRICS_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x87,0x7a,0x7d,0xb8,0x25,0x24,0x09,0x17,0xa3,0x8c,0xc7}}, "OBS_POLICY_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_STATE_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x88,0x79,0x63,0xbe,0xa9,0xb0,0x21,0x4a,0xcf,0xfd,0x99}}, "OBS_AGENT_STATE_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_EVIDENCE_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x89,0x77,0x6a,0xb0,0x2e,0x3a,0x71,0x55,0xcc,0x69,0x7e}}, "OBS_AGENT_EVIDENCE_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x8a,0x7d,0x97,0x8a,0x32,0x4a,0x33,0xb7,0xef,0x51,0xcb}}, "OBS_AGENT_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_POLICY_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x8b,0x75,0xb2,0x95,0xa5,0x2a,0x1b,0xc7,0xc4,0xb9,0x63}}, "OBS_AGENT_POLICY_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_OVERRIDE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x8c,0x79,0xb6,0x95,0x3e,0xd2,0x7d,0x0a,0xd3,0xed,0x7d}}, "OBS_AGENT_OVERRIDE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_RECOMMENDATION_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x8d,0x7d,0xfe,0x9a,0xae,0xce,0x3f,0x3e,0x65,0xca,0xae}}, "OBS_AGENT_RECOMMENDATION_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_ACTION_APPROVE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x8e,0x7a,0xda,0xb9,0x06,0x18,0xb7,0x52,0x1f,0x0d,0x01}}, "OBS_AGENT_ACTION_APPROVE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_ACTION_CANCEL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x8f,0x72,0xb1,0x80,0x70,0xa2,0xed,0x0c,0xf9,0x55,0x43}}, "OBS_AGENT_ACTION_CANCEL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_SUPPORT_BUNDLE_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x90,0x78,0x09,0xb1,0x97,0x74,0x8d,0x33,0xbb,0xb0,0xd7}}, "OBS_SUPPORT_BUNDLE_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_SIMULATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x91,0x72,0x48,0xaf,0xbc,0x3a,0x8d,0xd1,0x72,0x51,0xcb}}, "OBS_POLICY_SIMULATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_EDIT_DRAFT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x92,0x7c,0x6d,0xa1,0xc3,0x40,0x18,0x5a,0x66,0x0c,0xea}}, "OBS_POLICY_EDIT_DRAFT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_APPROVE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x93,0x7e,0xb2,0xa3,0x28,0x84,0xe0,0xc6,0x6e,0xc0,0x7f}}, "OBS_POLICY_APPROVE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_APPLY, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x94,0x79,0x59,0x8d,0xc4,0x72,0x27,0x04,0xe5,0x5d,0xe4}}, "OBS_POLICY_APPLY"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_ROLLBACK, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x95,0x75,0xc7,0xab,0x5f,0x14,0x99,0xac,0xf9,0x3f,0xd9}}, "OBS_POLICY_ROLLBACK"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_DELETE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x96,0x75,0x2f,0x95,0xa3,0x78,0xd8,0xd1,0xaa,0x5b,0xa7}}, "OBS_POLICY_DELETE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_CLUSTER_TOPOLOGY_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x97,0x7b,0x99,0xa4,0xc6,0x1f,0x72,0xf9,0x0c,0x93,0x2c}}, "OBS_CLUSTER_TOPOLOGY_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_REDACTION_POLICY_EDIT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x98,0x71,0x24,0xad,0x3b,0x46,0xb8,0xa7,0xee,0x72,0x09}}, "SEC_REDACTION_POLICY_EDIT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_EXPORT_POLICY_APPROVE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x99,0x7d,0xad,0xa7,0x7f,0x99,0x7f,0xf7,0x60,0x7d,0x6e}}, "SEC_EXPORT_POLICY_APPROVE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_POLICY_VALIDATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x9a,0x7c,0xca,0xa2,0x09,0x47,0x39,0xc7,0x21,0x04,0xfc}}, "OBS_POLICY_VALIDATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_POLICY_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x9b,0x7f,0x82,0x83,0xf8,0xce,0xb1,0x41,0x39,0x95,0x40}}, "OBS_METRICS_POLICY_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_POLICY_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x9c,0x77,0x01,0xb9,0x2e,0xb1,0xef,0x39,0xf0,0x16,0x22}}, "OBS_METRICS_POLICY_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_EXPORT_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x9d,0x76,0x28,0xb9,0xdb,0x24,0x50,0xd7,0x0a,0x17,0xb6}}, "OBS_METRICS_EXPORT_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_METRICS_RETENTION_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x9e,0x72,0xa8,0x8d,0xfd,0x06,0xb3,0xc5,0x4a,0x03,0x38}}, "OBS_METRICS_RETENTION_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_RUNTIME_SELF, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0x9f,0x7c,0x69,0x86,0xbb,0x6c,0xba,0x81,0x5e,0xe6,0xf9}}, "OBS_RUNTIME_SELF"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_RUNTIME_ALL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa0,0x7e,0x96,0x9d,0x33,0x2b,0xe2,0x8d,0x99,0x9b,0x76}}, "OBS_RUNTIME_ALL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_INDEX_PROFILE_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa1,0x7e,0xbb,0xb4,0x07,0xb3,0x5d,0x3d,0xdc,0xc6,0xab}}, "OBS_INDEX_PROFILE_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_MANAGEMENT_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa2,0x7e,0xd9,0xac,0x48,0xfb,0x78,0x77,0x63,0x8b,0x0f}}, "OBS_MANAGEMENT_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_MANAGEMENT_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa3,0x79,0x1f,0xbb,0x1d,0x99,0x58,0xeb,0xf5,0x53,0x65}}, "OBS_MANAGEMENT_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_CONFIG_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa4,0x7f,0x35,0xa3,0x76,0x3c,0xa4,0x5c,0x3b,0x75,0x73}}, "OBS_CONFIG_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_CONFIG_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa5,0x71,0x34,0x98,0x73,0x98,0xa6,0xec,0x84,0x2d,0x43}}, "OBS_CONFIG_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_CLUSTER_HEALTH_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa6,0x7c,0x17,0x8d,0xa0,0x56,0xbe,0x3b,0x9f,0xe2,0xd2}}, "OBS_CLUSTER_HEALTH_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_CLUSTER_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa7,0x71,0x40,0xbe,0x4c,0xcf,0x63,0x45,0x85,0xf4,0xa7}}, "OBS_CLUSTER_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_DATA_MOVEMENT_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa8,0x78,0x90,0xb8,0x3c,0x94,0x5e,0xba,0xb2,0x2d,0x9b}}, "OBS_DATA_MOVEMENT_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_IDENTITY_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xa9,0x77,0xaf,0x99,0xce,0x9a,0x73,0xec,0x6a,0xc4,0xfb}}, "SEC_IDENTITY_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_MEMBERSHIP_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xaa,0x70,0xfa,0xae,0x13,0xf6,0x47,0x99,0x80,0x0b,0x38}}, "SEC_MEMBERSHIP_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_GRANT_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xab,0x71,0x79,0xa6,0x41,0x7e,0x04,0x03,0x50,0x84,0x99}}, "SEC_GRANT_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_TRANSACTION_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xac,0x7e,0x7d,0x83,0xb7,0x6b,0x49,0x4f,0x3d,0x4a,0x2d}}, "MGA_TRANSACTION_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_RECOVERY_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xad,0x73,0x66,0xa4,0x0e,0x8e,0xe6,0x81,0xe3,0x67,0x5d}}, "MGA_RECOVERY_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_CLEANUP_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xae,0x74,0x57,0x86,0xaf,0x3a,0x0f,0x03,0x7a,0x49,0x08}}, "MGA_CLEANUP_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_CLEANUP_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xaf,0x75,0x00,0x83,0x1b,0xf7,0x32,0xc1,0xc0,0xa2,0xc6}}, "MGA_CLEANUP_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_HORIZON_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb0,0x78,0xa7,0x9b,0xe7,0x3d,0x48,0x65,0x6a,0x9e,0xff}}, "MGA_HORIZON_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_LINEAGE_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb1,0x79,0xb9,0xb7,0xc9,0xa3,0xcf,0xc2,0xbb,0x97,0xe8}}, "MGA_LINEAGE_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_FORENSIC_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb2,0x75,0x72,0x93,0x4b,0xba,0xc7,0xfe,0xf3,0x8d,0xc7}}, "MGA_FORENSIC_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MGA_METRICS_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb3,0x7d,0x29,0x91,0xfb,0x88,0x7d,0x70,0x14,0xcd,0x56}}, "MGA_METRICS_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::AUTH_PROVIDER_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb4,0x7f,0x71,0x88,0x80,0xe4,0xb2,0x50,0x77,0x4e,0x0e}}, "AUTH_PROVIDER_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::AUDIT_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb5,0x78,0x9a,0xac,0x4a,0x8a,0xa6,0xc8,0xc6,0xb0,0xfc}}, "AUDIT_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::AUDIT_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb6,0x79,0xba,0x8a,0xea,0x4a,0xe9,0x15,0x6a,0x2c,0x5d}}, "AUDIT_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::PROTECTED_MATERIAL_RELEASE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb7,0x77,0x1c,0xa6,0x7b,0x4a,0x04,0x74,0xac,0x17,0xb7}}, "PROTECTED_MATERIAL_RELEASE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::KEY_RELEASE_APPROVE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb8,0x7f,0x10,0xad,0x14,0xeb,0x01,0xd0,0xb5,0xb3,0x2c}}, "KEY_RELEASE_APPROVE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SUPPORT_EXPORT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xb9,0x72,0x81,0x8c,0x71,0x31,0x42,0x1b,0x08,0x2e,0x40}}, "SUPPORT_EXPORT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::UDR_TRUST_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xba,0x7d,0x85,0xab,0x3b,0x9b,0x83,0xc7,0x27,0xfa,0x4b}}, "UDR_TRUST_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::UDR_MANAGE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xbb,0x72,0xfa,0x9d,0x93,0x31,0x21,0x8d,0x92,0x9a,0x9c}}, "UDR_MANAGE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::UDR_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xbc,0x78,0x55,0xb2,0xea,0xce,0x10,0xfc,0xd2,0x47,0x99}}, "UDR_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::UDR_INVOKE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xbd,0x77,0xda,0xa9,0xee,0x24,0x03,0xcd,0xac,0x41,0x6e}}, "UDR_INVOKE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::FILESPACE_LIFECYCLE_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xbe,0x72,0xf3,0x97,0xb6,0x43,0x3f,0x61,0x92,0x15,0x8e}}, "FILESPACE_LIFECYCLE_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MIGRATE_DATABASE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xbf,0x7e,0x7c,0x89,0x3e,0x41,0x0d,0xb2,0x06,0x21,0xa0}}, "MIGRATE_DATABASE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::BACKUP_CREATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc0,0x72,0x5f,0xb0,0xa6,0x57,0x8a,0xe9,0x89,0x6d,0x25}}, "BACKUP_CREATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::BACKUP_RESTORE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc1,0x77,0x46,0xa4,0x7a,0xcb,0x3b,0x7e,0x01,0xa6,0x34}}, "BACKUP_RESTORE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::BACKUP_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc2,0x76,0x19,0x8e,0x4a,0x72,0x89,0x9f,0xe0,0xb4,0xe2}}, "BACKUP_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::BACKUP_INSPECT, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc3,0x79,0xbc,0x8d,0xc0,0x2d,0x7f,0x62,0xdc,0xda,0x10}}, "BACKUP_INSPECT"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SYS_BACKUP, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc4,0x7d,0x62,0xac,0x49,0x3c,0x2d,0xbe,0x1d,0xbf,0x22}}, "SYS_BACKUP"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc5,0x76,0x97,0xa5,0xd6,0x5a,0x9d,0x30,0xf8,0x73,0x14}}, "EVENT_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_CREATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc6,0x72,0x03,0xa6,0x61,0x59,0x9c,0x58,0x0c,0x9f,0xb0}}, "EVENT_CREATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_ALTER, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc7,0x7f,0x98,0xac,0x91,0xac,0xc7,0xe8,0x8c,0x0e,0xcd}}, "EVENT_ALTER"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_DROP, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc8,0x7d,0x0e,0xa8,0x34,0x3e,0x73,0x0e,0xd1,0xa0,0x09}}, "EVENT_DROP"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_SUBSCRIBE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xc9,0x7f,0xbc,0x9b,0xf4,0xf7,0xc8,0x8a,0xeb,0xec,0xb6}}, "EVENT_SUBSCRIBE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_PUBLISH, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xca,0x7d,0x24,0xb4,0xf8,0x6a,0x99,0x8c,0x85,0x24,0x38}}, "EVENT_PUBLISH"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_DELIVERY_READ, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xcb,0x79,0x7e,0x89,0x1a,0x0c,0x29,0xed,0x3f,0x77,0x2e}}, "EVENT_DELIVERY_READ"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EVENT_DELIVERY_ACK, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xcc,0x7f,0x89,0xac,0xb6,0x83,0x90,0x90,0x18,0xcf,0xfc}}, "EVENT_DELIVERY_ACK"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::MANAGER_ADMISSION_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xcd,0x72,0x79,0xbd,0xef,0x90,0x9e,0x06,0xc9,0xb2,0x84}}, "MANAGER_ADMISSION_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_CONTROL_INTERNAL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xce,0x74,0x79,0x90,0x7a,0x5c,0x5a,0x7e,0xa1,0xe9,0x5b}}, "OBS_AGENT_CONTROL_INTERNAL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_ALERT_INTERNAL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xcf,0x78,0x5e,0x99,0xe7,0x4b,0xaa,0xd5,0x56,0xdc,0x0c}}, "OBS_ALERT_INTERNAL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_EVIDENCE_APPEND, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd0,0x72,0x48,0xa8,0x4c,0x93,0xc3,0xb1,0xbd,0x55,0x39}}, "OBS_AGENT_EVIDENCE_APPEND"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_RESTORE_DRILL_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd1,0x78,0x14,0xa3,0xe3,0x0b,0x73,0x57,0xae,0xfb,0xb1}}, "OBS_RESTORE_DRILL_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_RESTORE_PLANNING, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd2,0x75,0x50,0x9d,0x3a,0x94,0xbe,0xa9,0x7e,0xb1,0xa1}}, "OBS_RESTORE_PLANNING"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_EXPORT_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd3,0x7b,0x55,0x98,0xfd,0x15,0x64,0x37,0x7b,0x51,0xc3}}, "OBS_EXPORT_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_FILESPACE_TRUNCATE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd4,0x70,0xce,0x98,0xa7,0xb8,0xc7,0x49,0xc3,0x06,0xdd}}, "OBS_FILESPACE_TRUNCATE"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_SESSION_KILL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd5,0x72,0x8e,0xaf,0xb5,0x48,0x76,0xed,0x22,0x71,0xc8}}, "SEC_SESSION_KILL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::SEC_SESSION_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd6,0x72,0xb1,0x99,0x0b,0x2f,0xdb,0x20,0xde,0x8b,0xf0}}, "SEC_SESSION_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_JOB_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd7,0x73,0x09,0xad,0x2e,0x56,0xba,0x8b,0x5c,0x8b,0xfb}}, "OBS_JOB_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::EXTERNAL_RANGE_MATERIALIZATION_ADMIN, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd8,0x7d,0xd6,0xaf,0xd3,0x3d,0xd4,0x4d,0x56,0xb9,0xfd}}, "EXTERNAL_RANGE_MATERIALIZATION_ADMIN"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_AGENT_INTERNAL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xd9,0x7c,0x30,0xb7,0x86,0x28,0xb0,0xc5,0xe8,0xb7,0xab}}, "OBS_AGENT_INTERNAL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_LISTENER_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xda,0x7e,0x90,0x97,0xf3,0x9d,0x0b,0x88,0x42,0x9f,0xb6}}, "OBS_LISTENER_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::OBS_PARSER_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xdb,0x71,0x3f,0x8f,0x4a,0x68,0xc1,0x19,0xe0,0xc9,0x8f}}, "OBS_PARSER_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::INDEX_MAINTENANCE_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xdc,0x72,0x20,0xac,0x71,0x35,0x26,0x11,0x3c,0x95,0xe5}}, "INDEX_MAINTENANCE_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::ARCHIVE_CONTROL, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xdd,0x7a,0xc4,0xa6,0xfc,0xe3,0x0c,0x72,0x1f,0x09,0x84}}, "ARCHIVE_CONTROL"},
    CatalogSecurityRightIdentity{CatalogSecurityRight::ARCHIVE_EXPIRY_REMOVE, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xcf,0xef,0x20,0xde,0x79,0x93,0xb3,0x00,0x7b,0x46,0x8f,0x2c,0xdc,0x35}}, "ARCHIVE_EXPIRY_REMOVE"},
}};
constexpr bool IsKnownCatalogSecurityRight(CatalogSecurityRight right) noexcept {
  for (const auto& r : kCatalogSecurityRights)
    if (r.right == right) return true;
  return false;
}
constexpr std::optional<scratchbird::core::platform::Uuid> CatalogSecurityRightIdentityOf(CatalogSecurityRight right) noexcept {
  for (const auto& r : kCatalogSecurityRights)
    if (r.right == right) return r.uuid;
  return std::nullopt;
}
// Boundary adapter (policy-pack import): the right a text tag names; nullopt
// for one with no constant (refused by the caller, never defaulted).
constexpr std::optional<CatalogSecurityRight> CatalogSecurityRightForTag(std::string_view tag) noexcept {
  for (const auto& r : kCatalogSecurityRights)
    if (r.tag == tag) return r.right;
  return std::nullopt;
}

// ---- classifications (coding-time constants, hand-written) ----
enum class CatalogSecurityPrincipalKind : scratchbird::core::platform::u16 { none = 0, user = 1, service = 2 };
enum class CatalogSecurityGrantClass : scratchbird::core::platform::u16 {
  none = 0, privilege = 1, role_membership = 2, group_membership = 3, agent_principal_binding = 4 };
enum class CatalogSecuritySubjectKind : scratchbird::core::platform::u16 { none = 0, principal = 1, role = 2, group = 3 };
enum class CatalogSecurityEffect : scratchbird::core::platform::u16 { none = 0, allow = 1, deny = 2 };
enum class CatalogSecurityAuthorityClass : scratchbird::core::platform::u16 {
  none = 0, engine_owned_sysarch = 1, l5_database = 2, direct_bounded_action = 3, observe_only = 4,
  recommend_only = 5, request_action = 6 };
enum class CatalogSecurityCredentialScheme : scratchbird::core::platform::u16 { none = 0, pbkdf2_sha256_v1 = 1 };
}  // namespace scratchbird::core::catalog
