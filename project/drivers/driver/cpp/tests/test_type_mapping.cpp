// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#define SCRATCHBIRD_TEST_HOOKS
#include "scratchbird/client/scratchbird_client.h"
#include "scratchbird/protocol/sbwp_protocol.h"

TEST(TypeMappingConformance, MapsWireOidsToSbTypes) {
    using namespace scratchbird::protocol;

    EXPECT_EQ(sb_test_map_type_oid(kOidBool), SB_TYPE_BOOLEAN);
    EXPECT_EQ(sb_test_map_type_oid(kOidInt2), SB_TYPE_SMALLINT);
    EXPECT_EQ(sb_test_map_type_oid(kOidInt4), SB_TYPE_INTEGER);
    EXPECT_EQ(sb_test_map_type_oid(kOidInt8), SB_TYPE_BIGINT);
    EXPECT_EQ(sb_test_map_type_oid(kOidFloat4), SB_TYPE_REAL);
    EXPECT_EQ(sb_test_map_type_oid(kOidFloat8), SB_TYPE_DOUBLE);
    EXPECT_EQ(sb_test_map_type_oid(kOidNumeric), SB_TYPE_DECIMAL);
    EXPECT_EQ(sb_test_map_type_oid(kOidMoney), SB_TYPE_MONEY);
    EXPECT_EQ(sb_test_map_type_oid(kOidChar), SB_TYPE_CHAR);
    EXPECT_EQ(sb_test_map_type_oid(kOidBpChar), SB_TYPE_CHAR);
    EXPECT_EQ(sb_test_map_type_oid(kOidVarchar), SB_TYPE_VARCHAR);
    EXPECT_EQ(sb_test_map_type_oid(kOidText), SB_TYPE_TEXT);
    EXPECT_EQ(sb_test_map_type_oid(kOidJson), SB_TYPE_JSON);
    EXPECT_EQ(sb_test_map_type_oid(kOidJsonb), SB_TYPE_JSONB);
    EXPECT_EQ(sb_test_map_type_oid(kOidXml), SB_TYPE_XML);
    EXPECT_EQ(sb_test_map_type_oid(kOidTsVector), SB_TYPE_TSVECTOR);
    EXPECT_EQ(sb_test_map_type_oid(kOidTsQuery), SB_TYPE_TSQUERY);
    EXPECT_EQ(sb_test_map_type_oid(kOidBytea), SB_TYPE_BLOB);
    EXPECT_EQ(sb_test_map_type_oid(kOidDate), SB_TYPE_DATE);
    EXPECT_EQ(sb_test_map_type_oid(kOidTime), SB_TYPE_TIME);
    EXPECT_EQ(sb_test_map_type_oid(kOidTimetz), SB_TYPE_TIME_TZ);
    EXPECT_EQ(sb_test_map_type_oid(kOidTimestamp), SB_TYPE_TIMESTAMP);
    EXPECT_EQ(sb_test_map_type_oid(kOidTimestamptz), SB_TYPE_TIMESTAMP_TZ);
    EXPECT_EQ(sb_test_map_type_oid(kOidInterval), SB_TYPE_INTERVAL);
    EXPECT_EQ(sb_test_map_type_oid(kOidUuid), SB_TYPE_UUID);
    EXPECT_EQ(sb_test_map_type_oid(kOidInet), SB_TYPE_INET);
    EXPECT_EQ(sb_test_map_type_oid(kOidCidr), SB_TYPE_CIDR);
    EXPECT_EQ(sb_test_map_type_oid(kOidMacaddr), SB_TYPE_MACADDR);
    EXPECT_EQ(sb_test_map_type_oid(kOidMacaddr8), SB_TYPE_MACADDR);
    EXPECT_EQ(sb_test_map_type_oid(kOidSbVector), SB_TYPE_VECTOR);
    EXPECT_EQ(sb_test_map_type_oid(kOidRecord), SB_TYPE_COMPOSITE);
    EXPECT_EQ(sb_test_map_type_oid(kOidInt4Array), SB_TYPE_ARRAY);
    EXPECT_EQ(sb_test_map_type_oid(kOidTextArray), SB_TYPE_ARRAY);
    EXPECT_EQ(sb_test_map_type_oid(kOidInt4Range), SB_TYPE_RANGE);
    EXPECT_EQ(sb_test_map_type_oid(kOidInt8Range), SB_TYPE_RANGE);
    EXPECT_EQ(sb_test_map_type_oid(kOidNumRange), SB_TYPE_RANGE);
    EXPECT_EQ(sb_test_map_type_oid(kOidTsRange), SB_TYPE_RANGE);
    EXPECT_EQ(sb_test_map_type_oid(kOidTstzRange), SB_TYPE_RANGE);
    EXPECT_EQ(sb_test_map_type_oid(kOidDateRange), SB_TYPE_RANGE);
}

TEST(TypeMappingConformance, MapsSbTypesToWireOids) {
    using namespace scratchbird::protocol;

    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_BOOLEAN), kOidBool);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_SMALLINT), kOidInt2);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_INTEGER), kOidInt4);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_BIGINT), kOidInt8);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_REAL), kOidFloat4);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_DOUBLE), kOidFloat8);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_DECIMAL), kOidNumeric);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_MONEY), kOidMoney);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_CHAR), kOidBpChar);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_VARCHAR), kOidText);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TEXT), kOidText);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_JSON), kOidJson);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_JSONB), kOidJsonb);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_XML), kOidXml);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TSVECTOR), kOidTsVector);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TSQUERY), kOidTsQuery);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_BLOB), kOidBytea);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_DATE), kOidDate);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TIME), kOidTime);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TIME_TZ), kOidTimetz);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TIMESTAMP), kOidTimestamp);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_TIMESTAMP_TZ), kOidTimestamptz);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_INTERVAL), kOidInterval);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_UUID), kOidUuid);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_GEOMETRY), kOidPoint);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_ARRAY), kOidTextArray);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_VECTOR), kOidSbVector);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_COMPOSITE), kOidRecord);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_RANGE), kOidInt4Range);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_INET), kOidInet);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_CIDR), kOidCidr);
    EXPECT_EQ(sb_test_map_sb_type_to_oid(SB_TYPE_MACADDR), kOidMacaddr);
}

TEST(TypeMappingConformance, MetadataQueryRequiresConnectionHandle) {
    sb_error err{};
    sb_result* result = sb_metadata_query(nullptr, "tables", &err);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(err.code, SB_ERR_NULL_POINTER);
}
