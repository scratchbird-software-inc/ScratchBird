// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

#include "scratchbird/core/status.h"
#include "scratchbird/odbc/odbc_types.h"
#include "scratchbird/client/network_client.h"

namespace scratchbird {
namespace odbc {

class OdbcClientBridge {
public:
    OdbcClientBridge();
    virtual ~OdbcClientBridge();

    virtual SQLRETURN connect(const ConnectionParams& params, std::string& error);
    virtual SQLRETURN probeAuthSurface(const ConnectionParams& params,
                                       client::AuthProbeResult& result,
                                       std::string& error);
    virtual void disconnect();
    virtual bool isConnected() const;
    virtual client::ResolvedAuthContext getResolvedAuthContext() const;
    core::Status lastStatus() const { return last_status_; }
    const std::string& lastError() const { return last_error_; }

    virtual SQLRETURN executeSQL(const std::string& sql,
                                 std::vector<std::vector<std::string>>& results,
                                 std::vector<ColumnMetadata>& columns,
                                 SQLLEN& rows_affected);
    virtual SQLRETURN cancel();

    virtual SQLRETURN beginTransaction();
    virtual SQLRETURN commit();
    virtual SQLRETURN rollback();

private:
    static client::NetworkClientConfig buildConfig(const ConnectionParams& params);
    static ColumnMetadata mapColumn(const client::NetworkColumn& col);
    static SQLSMALLINT mapTypeOid(uint32_t type_oid);
    static std::string typeOidToString(uint32_t type_oid);
    static std::string stringifyValue(const protocol::ColumnValue& val,
                                      uint32_t type_oid,
                                      uint8_t format);

    client::NetworkClient client_;
    ConnectionParams params_{};
    core::Status last_status_{core::Status::OK};
    std::string last_error_;
    client::ResolvedAuthContext resolved_auth_context_{};
};

} // namespace odbc
} // namespace scratchbird
