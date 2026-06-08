// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file odbc_driver.cpp
 * @brief ScratchBird ODBC Driver API Implementation
 *
 * Implements the standard ODBC API functions exported by the driver.
 *
 * Part of Phase 3.8: ODBC Driver
 */

#include "scratchbird/odbc/odbc_driver.h"

#include <algorithm>
#include <atomic>
#include <codecvt>
#include <cstring>
#include <cwchar>
#include <locale>
#include <string>
#include <vector>

using namespace scratchbird::odbc;

namespace {
// Connection-pooling mode set via SQLSetEnvAttr with SQL_NULL_HENV applies to
// subsequent environment allocations.
std::atomic<SQLUINTEGER> g_default_connection_pooling{SQL_CP_OFF};

std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> g_utf16_codec;

std::string wideToUtf8(const ::SQLWCHAR* text, SQLINTEGER length) {
    if (!text) {
        return {};
    }
    std::u16string wide;
    if (length == SQL_NTS) {
        const auto* p = text;
        while (*p) {
            wide.push_back(static_cast<char16_t>(*p));
            ++p;
        }
    } else if (length >= 0) {
        wide.reserve(static_cast<size_t>(length));
        for (SQLINTEGER i = 0; i < length; ++i) {
            wide.push_back(static_cast<char16_t>(text[i]));
        }
    } else {
        return {};
    }
    return g_utf16_codec.to_bytes(wide);
}

std::u16string utf8ToWide(const std::string& text) {
    return g_utf16_codec.from_bytes(text);
}

SQLRETURN copyWideString(const std::u16string& text,
                         ::SQLWCHAR* out_buffer,
                         SQLSMALLINT out_capacity,
                         SQLSMALLINT* out_length) {
    if (out_length) {
        *out_length = static_cast<SQLSMALLINT>(text.size());
    }
    if (!out_buffer) {
        return SQL_SUCCESS;
    }
    if (out_capacity <= 0) {
        return SQL_ERROR;
    }
    size_t copy_len = std::min(static_cast<size_t>(out_capacity - 1), text.size());
    for (size_t i = 0; i < copy_len; ++i) {
        out_buffer[i] = static_cast<::SQLWCHAR>(text[i]);
    }
    out_buffer[copy_len] = 0;
    if (text.size() >= static_cast<size_t>(out_capacity)) {
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

bool isStringInfoType(SQLUSMALLINT info_type) {
    switch (info_type) {
        case SQL_DRIVER_NAME:
        case SQL_DRIVER_VER:
        case SQL_DRIVER_ODBC_VER:
        case SQL_ODBC_VER:
        case SQL_DBMS_NAME:
        case SQL_DBMS_VER:
        case SQL_DATABASE_NAME:
        case SQL_SERVER_NAME:
        case SQL_USER_NAME:
        case SQL_DATA_SOURCE_NAME:
        case SQL_DATA_SOURCE_READ_ONLY:
        case SQL_ACCESSIBLE_TABLES:
        case SQL_ACCESSIBLE_PROCEDURES:
        case SQL_MULT_RESULT_SETS:
        case SQL_MULTIPLE_ACTIVE_TXN:
        case SQL_PROCEDURES:
        case SQL_CATALOG_NAME:
        case SQL_COLUMN_ALIAS:
        case SQL_LIKE_ESCAPE_CLAUSE:
        case SQL_ORDER_BY_COLUMNS_IN_SELECT:
        case SQL_OUTER_JOINS:
        case SQL_ROW_UPDATES:
        case SQL_EXPRESSIONS_IN_ORDERBY:
        case SQL_INTEGRITY:
        case SQL_IDENTIFIER_QUOTE_CHAR:
        case SQL_CATALOG_NAME_SEPARATOR:
        case SQL_CATALOG_TERM:
        case SQL_SCHEMA_TERM:
        case SQL_TABLE_TERM:
        case SQL_PROCEDURE_TERM:
        case SQL_SEARCH_PATTERN_ESCAPE:
        case SQL_SPECIAL_CHARACTERS:
        case SQL_NEED_LONG_DATA_LEN:
        case SQL_DESCRIBE_PARAMETER:
        case SQL_COLLATION_SEQ:
        case SQL_KEYWORDS:
        case SQL_XOPEN_CLI_YEAR:
        case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
        case SQL_DM_VER:
            return true;
        default:
            return false;
    }
}
}  // namespace

// =============================================================================
// Handle Allocation and Freeing
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLAllocHandle(
    SQLSMALLINT HandleType,
    SQLHANDLE InputHandle,
    SQLHANDLE* OutputHandlePtr) {

    if (!OutputHandlePtr) {
        return SQL_ERROR;
    }

    switch (HandleType) {
        case SQL_HANDLE_ENV: {
            auto* env = new OdbcEnvironment();
            env->setConnectionPooling(g_default_connection_pooling.load());
            *OutputHandlePtr = static_cast<SQLHANDLE>(env);
            return SQL_SUCCESS;
        }

        case SQL_HANDLE_DBC: {
            auto* env = asEnvironment(InputHandle);
            if (!env) {
                return SQL_INVALID_HANDLE;
            }
            auto* conn = env->createConnection();
            *OutputHandlePtr = static_cast<SQLHANDLE>(conn);
            return SQL_SUCCESS;
        }

        case SQL_HANDLE_STMT: {
            auto* conn = asConnection(InputHandle);
            if (!conn) {
                return SQL_INVALID_HANDLE;
            }
            auto* stmt = conn->createStatement();
            *OutputHandlePtr = static_cast<SQLHANDLE>(stmt);
            return SQL_SUCCESS;
        }

        case SQL_HANDLE_DESC: {
            auto* conn = asConnection(InputHandle);
            if (!conn) {
                return SQL_INVALID_HANDLE;
            }
            auto* desc = new OdbcDescriptor(conn, OdbcDescriptor::DescriptorType::APD, false);
            *OutputHandlePtr = static_cast<SQLHANDLE>(desc);
            return SQL_SUCCESS;
        }

        default:
            return SQL_ERROR;
    }
}

extern "C" SQLRETURN ODBC_API SQLFreeHandle(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle) {

    if (!Handle) {
        return SQL_INVALID_HANDLE;
    }

    switch (HandleType) {
        case SQL_HANDLE_ENV: {
            auto* env = asEnvironment(Handle);
            if (!env) return SQL_INVALID_HANDLE;
            if (env->getConnectionCount() > 0) {
                env->setError("HY010", 0, "Function sequence error");
                return SQL_ERROR;
            }
            delete env;
            return SQL_SUCCESS;
        }

        case SQL_HANDLE_DBC: {
            auto* conn = asConnection(Handle);
            if (!conn) return SQL_INVALID_HANDLE;
            if (conn->isConnected()) {
                conn->disconnect();
            }
            conn->getEnvironment()->removeConnection(conn);
            return SQL_SUCCESS;
        }

        case SQL_HANDLE_STMT: {
            auto* stmt = asStatement(Handle);
            if (!stmt) return SQL_INVALID_HANDLE;
            stmt->getConnection()->removeStatement(stmt);
            return SQL_SUCCESS;
        }

        case SQL_HANDLE_DESC: {
            auto* desc = asDescriptor(Handle);
            if (!desc) return SQL_INVALID_HANDLE;
            if (!desc->isImplicit()) {
                delete desc;
            }
            return SQL_SUCCESS;
        }

        default:
            return SQL_ERROR;
    }
}

// =============================================================================
// Connection Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLConnect(
    SQLHDBC ConnectionHandle,
    SQLCHAR* ServerName,
    SQLSMALLINT NameLength1,
    SQLCHAR* UserName,
    SQLSMALLINT NameLength2,
    SQLCHAR* Authentication,
    SQLSMALLINT NameLength3) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->connect(ServerName, NameLength1, UserName, NameLength2,
                         Authentication, NameLength3);
}

extern "C" SQLRETURN ODBC_API SQLDriverConnect(
    SQLHDBC ConnectionHandle,
    HWND WindowHandle,
    SQLCHAR* InConnectionString,
    SQLSMALLINT StringLength1,
    SQLCHAR* OutConnectionString,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLength2Ptr,
    SQLUSMALLINT DriverCompletion) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->driverConnect(WindowHandle, InConnectionString, StringLength1,
                               OutConnectionString, BufferLength, StringLength2Ptr,
                               DriverCompletion);
}

extern "C" SQLRETURN ODBC_API SQLBrowseConnect(
    SQLHDBC ConnectionHandle,
    SQLCHAR* InConnectionString,
    SQLSMALLINT StringLength1,
    SQLCHAR* OutConnectionString,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLength2Ptr) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->browseConnect(InConnectionString, StringLength1,
                               OutConnectionString, BufferLength, StringLength2Ptr);
}

extern "C" SQLRETURN ODBC_API SQLDisconnect(
    SQLHDBC ConnectionHandle) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->disconnect();
}

// =============================================================================
// Attribute Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLSetEnvAttr(
    SQLHENV EnvironmentHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER StringLength) {

    // Special case: SQL_ATTR_CONNECTION_POOLING can be set before env allocation
    if (EnvironmentHandle == SQL_NULL_HENV) {
        if (Attribute == SQL_ATTR_CONNECTION_POOLING) {
            g_default_connection_pooling.store(
                static_cast<SQLUINTEGER>(reinterpret_cast<uintptr_t>(ValuePtr)));
            return SQL_SUCCESS;
        }
        return SQL_INVALID_HANDLE;
    }

    auto* env = asEnvironment(EnvironmentHandle);
    if (!env) return SQL_INVALID_HANDLE;

    return env->setAttribute(Attribute, ValuePtr, StringLength);
}

extern "C" SQLRETURN ODBC_API SQLGetEnvAttr(
    SQLHENV EnvironmentHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER BufferLength,
    SQLINTEGER* StringLengthPtr) {

    auto* env = asEnvironment(EnvironmentHandle);
    if (!env) return SQL_INVALID_HANDLE;

    return env->getAttribute(Attribute, ValuePtr, BufferLength, StringLengthPtr);
}

extern "C" SQLRETURN ODBC_API SQLSetConnectAttr(
    SQLHDBC ConnectionHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER StringLength) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->setAttribute(Attribute, ValuePtr, StringLength);
}

extern "C" SQLRETURN ODBC_API SQLGetConnectAttr(
    SQLHDBC ConnectionHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER BufferLength,
    SQLINTEGER* StringLengthPtr) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->getAttribute(Attribute, ValuePtr, BufferLength, StringLengthPtr);
}

extern "C" SQLRETURN ODBC_API SQLSetStmtAttr(
    SQLHSTMT StatementHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER StringLength) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->setAttribute(Attribute, ValuePtr, StringLength);
}

extern "C" SQLRETURN ODBC_API SQLGetStmtAttr(
    SQLHSTMT StatementHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER BufferLength,
    SQLINTEGER* StringLengthPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->getAttribute(Attribute, ValuePtr, BufferLength, StringLengthPtr);
}

// =============================================================================
// Information Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLGetInfo(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT InfoType,
    SQLPOINTER InfoValuePtr,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLengthPtr) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->getInfo(InfoType, InfoValuePtr, BufferLength, StringLengthPtr);
}

extern "C" SQLRETURN ODBC_API SQLGetFunctions(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT FunctionId,
    SQLUSMALLINT* SupportedPtr) {

    auto* conn = asConnection(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;

    return conn->getFunctions(FunctionId, SupportedPtr);
}

extern "C" SQLRETURN ODBC_API SQLGetTypeInfo(
    SQLHSTMT StatementHandle,
    SQLSMALLINT DataType) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->getConnection()->getTypeInfo(DataType, stmt);
}

// =============================================================================
// Statement Execution Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLPrepare(
    SQLHSTMT StatementHandle,
    SQLCHAR* StatementText,
    SQLINTEGER TextLength) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->prepare(StatementText, TextLength);
}

extern "C" SQLRETURN ODBC_API SQLExecute(
    SQLHSTMT StatementHandle) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->execute();
}

extern "C" SQLRETURN ODBC_API SQLExecDirect(
    SQLHSTMT StatementHandle,
    SQLCHAR* StatementText,
    SQLINTEGER TextLength) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->execDirect(StatementText, TextLength);
}

extern "C" SQLRETURN ODBC_API SQLCancel(
    SQLHSTMT StatementHandle) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->cancel();
}

extern "C" SQLRETURN ODBC_API SQLCloseCursor(
    SQLHSTMT StatementHandle) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->closeCursor();
}

extern "C" SQLRETURN ODBC_API SQLFreeStmt(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Option) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    if (Option == SQL_DROP) {
        stmt->getConnection()->removeStatement(stmt);
        return SQL_SUCCESS;
    }

    return stmt->freeStmt(Option);
}

// =============================================================================
// Parameter Binding Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLBindParameter(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ParameterNumber,
    SQLSMALLINT InputOutputType,
    SQLSMALLINT ValueType,
    SQLSMALLINT ParameterType,
    SQLULEN ColumnSize,
    SQLSMALLINT DecimalDigits,
    SQLPOINTER ParameterValuePtr,
    SQLLEN BufferLength,
    SQLLEN* StrLen_or_IndPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->bindParameter(ParameterNumber, InputOutputType, ValueType,
                               ParameterType, ColumnSize, DecimalDigits,
                               ParameterValuePtr, BufferLength, StrLen_or_IndPtr);
}

extern "C" SQLRETURN ODBC_API SQLNumParams(
    SQLHSTMT StatementHandle,
    SQLSMALLINT* ParameterCountPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->numParams(ParameterCountPtr);
}

extern "C" SQLRETURN ODBC_API SQLDescribeParam(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ParameterNumber,
    SQLSMALLINT* DataTypePtr,
    SQLULEN* ParameterSizePtr,
    SQLSMALLINT* DecimalDigitsPtr,
    SQLSMALLINT* NullablePtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->describeParam(ParameterNumber, DataTypePtr, ParameterSizePtr,
                               DecimalDigitsPtr, NullablePtr);
}

// =============================================================================
// Column Binding and Retrieval Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLBindCol(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLSMALLINT TargetType,
    SQLPOINTER TargetValuePtr,
    SQLLEN BufferLength,
    SQLLEN* StrLen_or_IndPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->bindCol(ColumnNumber, TargetType, TargetValuePtr,
                         BufferLength, StrLen_or_IndPtr);
}

extern "C" SQLRETURN ODBC_API SQLNumResultCols(
    SQLHSTMT StatementHandle,
    SQLSMALLINT* ColumnCountPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->numResultCols(ColumnCountPtr);
}

extern "C" SQLRETURN ODBC_API SQLDescribeCol(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLCHAR* ColumnName,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* NameLengthPtr,
    SQLSMALLINT* DataTypePtr,
    SQLULEN* ColumnSizePtr,
    SQLSMALLINT* DecimalDigitsPtr,
    SQLSMALLINT* NullablePtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->describeCol(ColumnNumber, ColumnName, BufferLength, NameLengthPtr,
                             DataTypePtr, ColumnSizePtr, DecimalDigitsPtr, NullablePtr);
}

extern "C" SQLRETURN ODBC_API SQLColAttribute(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLUSMALLINT FieldIdentifier,
    SQLPOINTER CharacterAttributePtr,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLengthPtr,
    SQLLEN* NumericAttributePtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->colAttribute(ColumnNumber, FieldIdentifier, CharacterAttributePtr,
                              BufferLength, StringLengthPtr, NumericAttributePtr);
}

extern "C" SQLRETURN ODBC_API SQLFetch(
    SQLHSTMT StatementHandle) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->fetch();
}

extern "C" SQLRETURN ODBC_API SQLFetchScroll(
    SQLHSTMT StatementHandle,
    SQLSMALLINT FetchOrientation,
    SQLLEN FetchOffset) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->fetchScroll(FetchOrientation, FetchOffset);
}

extern "C" SQLRETURN ODBC_API SQLGetData(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLSMALLINT TargetType,
    SQLPOINTER TargetValuePtr,
    SQLLEN BufferLength,
    SQLLEN* StrLen_or_IndPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->getData(ColumnNumber, TargetType, TargetValuePtr,
                         BufferLength, StrLen_or_IndPtr);
}

extern "C" SQLRETURN ODBC_API SQLParamData(
    SQLHSTMT StatementHandle,
    SQLPOINTER* ValuePtrPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->paramData(ValuePtrPtr);
}

extern "C" SQLRETURN ODBC_API SQLPutData(
    SQLHSTMT StatementHandle,
    SQLPOINTER DataPtr,
    SQLLEN StrLen_or_Ind) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->putData(DataPtr, StrLen_or_Ind);
}

extern "C" SQLRETURN ODBC_API SQLRowCount(
    SQLHSTMT StatementHandle,
    SQLLEN* RowCountPtr) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->rowCount(RowCountPtr);
}

extern "C" SQLRETURN ODBC_API SQLMoreResults(
    SQLHSTMT StatementHandle) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->moreResults();
}

// =============================================================================
// Positioned Operations Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLSetPos(
    SQLHSTMT StatementHandle,
    SQLSETPOSIROW RowNumber,
    SQLUSMALLINT Operation,
    SQLUSMALLINT LockType) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->setPos(RowNumber, Operation, LockType);
}

extern "C" SQLRETURN ODBC_API SQLBulkOperations(
    SQLHSTMT StatementHandle,
    SQLSMALLINT Operation) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->bulkOperations(Operation);
}

// =============================================================================
// Catalog Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLTables(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLCHAR* TableType,
    SQLSMALLINT NameLength4) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->tables(CatalogName, NameLength1, SchemaName, NameLength2,
                        TableName, NameLength3, TableType, NameLength4);
}

extern "C" SQLRETURN ODBC_API SQLColumns(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLCHAR* ColumnName,
    SQLSMALLINT NameLength4) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->columns(CatalogName, NameLength1, SchemaName, NameLength2,
                         TableName, NameLength3, ColumnName, NameLength4);
}

extern "C" SQLRETURN ODBC_API SQLPrimaryKeys(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->primaryKeys(CatalogName, NameLength1, SchemaName, NameLength2,
                             TableName, NameLength3);
}

extern "C" SQLRETURN ODBC_API SQLForeignKeys(
    SQLHSTMT StatementHandle,
    SQLCHAR* PKCatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* PKSchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* PKTableName,
    SQLSMALLINT NameLength3,
    SQLCHAR* FKCatalogName,
    SQLSMALLINT NameLength4,
    SQLCHAR* FKSchemaName,
    SQLSMALLINT NameLength5,
    SQLCHAR* FKTableName,
    SQLSMALLINT NameLength6) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->foreignKeys(PKCatalogName, NameLength1, PKSchemaName, NameLength2,
                             PKTableName, NameLength3, FKCatalogName, NameLength4,
                             FKSchemaName, NameLength5, FKTableName, NameLength6);
}

extern "C" SQLRETURN ODBC_API SQLStatistics(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLUSMALLINT Unique,
    SQLUSMALLINT Reserved) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->statistics(CatalogName, NameLength1, SchemaName, NameLength2,
                            TableName, NameLength3, Unique, Reserved);
}

extern "C" SQLRETURN ODBC_API SQLSpecialColumns(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT IdentifierType,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLUSMALLINT Scope,
    SQLUSMALLINT Nullable) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->specialColumns(IdentifierType, CatalogName, NameLength1,
                                SchemaName, NameLength2, TableName, NameLength3,
                                Scope, Nullable);
}

extern "C" SQLRETURN ODBC_API SQLProcedures(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* ProcName,
    SQLSMALLINT NameLength3) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->procedures(CatalogName, NameLength1, SchemaName, NameLength2,
                            ProcName, NameLength3);
}

extern "C" SQLRETURN ODBC_API SQLProcedureColumns(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* ProcName,
    SQLSMALLINT NameLength3,
    SQLCHAR* ColumnName,
    SQLSMALLINT NameLength4) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->procedureColumns(CatalogName, NameLength1, SchemaName, NameLength2,
                                  ProcName, NameLength3, ColumnName, NameLength4);
}

extern "C" SQLRETURN ODBC_API SQLTablePrivileges(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->tablePrivileges(CatalogName, NameLength1, SchemaName, NameLength2,
                                 TableName, NameLength3);
}

extern "C" SQLRETURN ODBC_API SQLColumnPrivileges(
    SQLHSTMT StatementHandle,
    SQLCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLCHAR* ColumnName,
    SQLSMALLINT NameLength4) {

    auto* stmt = asStatement(StatementHandle);
    if (!stmt) return SQL_INVALID_HANDLE;

    return stmt->columnPrivileges(CatalogName, NameLength1, SchemaName, NameLength2,
                                  TableName, NameLength3, ColumnName, NameLength4);
}

// =============================================================================
// Transaction Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLEndTran(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT CompletionType) {

    if (HandleType == SQL_HANDLE_ENV) {
        auto* env = asEnvironment(Handle);
        if (!env) return SQL_INVALID_HANDLE;
        return env->endTransaction(CompletionType);
    } else if (HandleType == SQL_HANDLE_DBC) {
        auto* conn = asConnection(Handle);
        if (!conn) return SQL_INVALID_HANDLE;

        return conn->endTransaction(CompletionType);
    }

    return SQL_INVALID_HANDLE;
}

// =============================================================================
// Diagnostic Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLGetDiagRec(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT RecNumber,
    SQLCHAR* SQLState,
    SQLINTEGER* NativeErrorPtr,
    SQLCHAR* MessageText,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* TextLengthPtr) {

    if (!Handle) {
        return SQL_INVALID_HANDLE;
    }

    OdbcHandle* handle = nullptr;
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            handle = asEnvironment(Handle);
            break;
        case SQL_HANDLE_DBC:
            handle = asConnection(Handle);
            break;
        case SQL_HANDLE_STMT:
            handle = asStatement(Handle);
            break;
        case SQL_HANDLE_DESC:
            handle = asDescriptor(Handle);
            break;
        default:
            return SQL_INVALID_HANDLE;
    }

    if (!handle) return SQL_INVALID_HANDLE;

    const DiagnosticRecord* rec = handle->getDiagnostic(RecNumber);
    if (!rec) {
        return SQL_NO_DATA;
    }

    // Copy SQLSTATE
    if (SQLState) {
        std::memcpy(SQLState, rec->sqlstate.c_str(), 5);
        SQLState[5] = '\0';
    }

    // Copy native error
    if (NativeErrorPtr) {
        *NativeErrorPtr = rec->native_error;
    }

    // Copy message
    SQLRETURN result = SQL_SUCCESS;
    if (TextLengthPtr) {
        *TextLengthPtr = static_cast<SQLSMALLINT>(rec->message.size());
    }
    if (MessageText && BufferLength > 0) {
        size_t copy_len = std::min(static_cast<size_t>(BufferLength - 1), rec->message.size());
        std::memcpy(MessageText, rec->message.c_str(), copy_len);
        MessageText[copy_len] = '\0';
        if (rec->message.size() >= static_cast<size_t>(BufferLength)) {
            result = SQL_SUCCESS_WITH_INFO;
        }
    }

    return result;
}

extern "C" SQLRETURN ODBC_API SQLGetDiagField(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT DiagIdentifier,
    SQLPOINTER DiagInfoPtr,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLengthPtr) {

    if (!Handle) {
        return SQL_INVALID_HANDLE;
    }

    OdbcHandle* handle = nullptr;
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            handle = asEnvironment(Handle);
            break;
        case SQL_HANDLE_DBC:
            handle = asConnection(Handle);
            break;
        case SQL_HANDLE_STMT:
            handle = asStatement(Handle);
            break;
        case SQL_HANDLE_DESC:
            handle = asDescriptor(Handle);
            break;
        default:
            return SQL_INVALID_HANDLE;
    }

    if (!handle) return SQL_INVALID_HANDLE;

    // Header fields (RecNumber == 0)
    if (RecNumber == 0) {
        switch (DiagIdentifier) {
            case SQL_DIAG_NUMBER:
                if (DiagInfoPtr) {
                    *static_cast<SQLINTEGER*>(DiagInfoPtr) = handle->getDiagnosticCount();
                }
                break;
            case SQL_DIAG_RETURNCODE:
                if (DiagInfoPtr) {
                    *static_cast<SQLRETURN*>(DiagInfoPtr) = handle->getReturnCode();
                }
                break;
            case SQL_DIAG_ROW_COUNT:
                if (HandleType == SQL_HANDLE_STMT) {
                    auto* stmt = asStatement(Handle);
                    if (DiagInfoPtr) {
                        *static_cast<SQLLEN*>(DiagInfoPtr) = stmt ? stmt->getRowCount() : 0;
                    }
                }
                break;
            default:
                return SQL_ERROR;
        }
        return SQL_SUCCESS;
    }

    // Record fields
    const DiagnosticRecord* rec = handle->getDiagnostic(RecNumber);
    if (!rec) {
        return SQL_NO_DATA;
    }

    auto copyString = [&](const std::string& str) -> SQLRETURN {
        if (StringLengthPtr) {
            *StringLengthPtr = static_cast<SQLSMALLINT>(str.size());
        }
        if (DiagInfoPtr && BufferLength > 0) {
            size_t copy_len = std::min(static_cast<size_t>(BufferLength - 1), str.size());
            std::memcpy(DiagInfoPtr, str.c_str(), copy_len);
            static_cast<char*>(DiagInfoPtr)[copy_len] = '\0';
            if (str.size() >= static_cast<size_t>(BufferLength)) {
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    };

    switch (DiagIdentifier) {
        case SQL_DIAG_SQLSTATE:
            return copyString(rec->sqlstate);
        case SQL_DIAG_NATIVE:
            if (DiagInfoPtr) {
                *static_cast<SQLINTEGER*>(DiagInfoPtr) = rec->native_error;
            }
            break;
        case SQL_DIAG_MESSAGE_TEXT:
            return copyString(rec->message);
        case SQL_DIAG_CLASS_ORIGIN:
            return copyString(rec->class_origin);
        case SQL_DIAG_SUBCLASS_ORIGIN:
            return copyString(rec->subclass_origin);
        case SQL_DIAG_CONNECTION_NAME:
            return copyString(rec->connection_name);
        case SQL_DIAG_SERVER_NAME:
            return copyString(rec->server_name);
        case SQL_DIAG_ROW_NUMBER:
            if (DiagInfoPtr) {
                *static_cast<SQLINTEGER*>(DiagInfoPtr) = rec->row_number;
            }
            break;
        case SQL_DIAG_COLUMN_NUMBER:
            if (DiagInfoPtr) {
                *static_cast<SQLINTEGER*>(DiagInfoPtr) = rec->column_number;
            }
            break;
        default:
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

// =============================================================================
// Descriptor Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLSetDescField(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT FieldIdentifier,
    SQLPOINTER ValuePtr,
    SQLINTEGER BufferLength) {

    auto* desc = asDescriptor(DescriptorHandle);
    if (!desc) return SQL_INVALID_HANDLE;

    return desc->setField(RecNumber, FieldIdentifier, ValuePtr, BufferLength);
}

extern "C" SQLRETURN ODBC_API SQLGetDescField(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT FieldIdentifier,
    SQLPOINTER ValuePtr,
    SQLINTEGER BufferLength,
    SQLINTEGER* StringLengthPtr) {

    auto* desc = asDescriptor(DescriptorHandle);
    if (!desc) return SQL_INVALID_HANDLE;

    return desc->getField(RecNumber, FieldIdentifier, ValuePtr, BufferLength, StringLengthPtr);
}

extern "C" SQLRETURN ODBC_API SQLSetDescRec(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT Type,
    SQLSMALLINT SubType,
    SQLLEN Length,
    SQLSMALLINT Precision,
    SQLSMALLINT Scale,
    SQLPOINTER DataPtr,
    SQLLEN* StringLengthPtr,
    SQLLEN* IndicatorPtr) {

    auto* desc = asDescriptor(DescriptorHandle);
    if (!desc) return SQL_INVALID_HANDLE;

    return desc->setRec(RecNumber, Type, SubType, Length, Precision, Scale,
                        DataPtr, StringLengthPtr, IndicatorPtr);
}

extern "C" SQLRETURN ODBC_API SQLGetDescRec(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLCHAR* Name,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLengthPtr,
    SQLSMALLINT* TypePtr,
    SQLSMALLINT* SubTypePtr,
    SQLLEN* LengthPtr,
    SQLSMALLINT* PrecisionPtr,
    SQLSMALLINT* ScalePtr,
    SQLSMALLINT* NullablePtr) {

    auto* desc = asDescriptor(DescriptorHandle);
    if (!desc) return SQL_INVALID_HANDLE;

    return desc->getRec(RecNumber, Name, BufferLength, StringLengthPtr, TypePtr,
                        SubTypePtr, LengthPtr, PrecisionPtr, ScalePtr, NullablePtr);
}

extern "C" SQLRETURN ODBC_API SQLCopyDesc(
    SQLHDESC SourceDescHandle,
    SQLHDESC TargetDescHandle) {

    auto* source = asDescriptor(SourceDescHandle);
    auto* target = asDescriptor(TargetDescHandle);

    if (!source || !target) return SQL_INVALID_HANDLE;

    return source->copyDesc(target);
}

// =============================================================================
// ODBC 2.x Compatibility Functions
// =============================================================================

extern "C" SQLRETURN ODBC_API SQLError(
    SQLHENV EnvironmentHandle,
    SQLHDBC ConnectionHandle,
    SQLHSTMT StatementHandle,
    SQLCHAR* SQLState,
    SQLINTEGER* NativeErrorPtr,
    SQLCHAR* MessageText,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* TextLengthPtr) {

    // Use the most specific handle provided
    SQLSMALLINT handle_type;
    SQLHANDLE handle;

    if (StatementHandle) {
        handle_type = SQL_HANDLE_STMT;
        handle = StatementHandle;
    } else if (ConnectionHandle) {
        handle_type = SQL_HANDLE_DBC;
        handle = ConnectionHandle;
    } else if (EnvironmentHandle) {
        handle_type = SQL_HANDLE_ENV;
        handle = EnvironmentHandle;
    } else {
        return SQL_INVALID_HANDLE;
    }

    // SQLError retrieves records one at a time and removes them
    static int rec_number = 1;  // Not thread-safe, but this is deprecated anyway
    return SQLGetDiagRec(handle_type, handle, rec_number++, SQLState,
                         NativeErrorPtr, MessageText, BufferLength, TextLengthPtr);
}

extern "C" SQLRETURN ODBC_API SQLAllocEnv(
    SQLHENV* EnvironmentHandlePtr) {

    return SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, EnvironmentHandlePtr);
}

extern "C" SQLRETURN ODBC_API SQLAllocConnect(
    SQLHENV EnvironmentHandle,
    SQLHDBC* ConnectionHandlePtr) {

    return SQLAllocHandle(SQL_HANDLE_DBC, EnvironmentHandle, ConnectionHandlePtr);
}

extern "C" SQLRETURN ODBC_API SQLAllocStmt(
    SQLHDBC ConnectionHandle,
    SQLHSTMT* StatementHandlePtr) {

    return SQLAllocHandle(SQL_HANDLE_STMT, ConnectionHandle, StatementHandlePtr);
}

extern "C" SQLRETURN ODBC_API SQLFreeEnv(
    SQLHENV EnvironmentHandle) {

    return SQLFreeHandle(SQL_HANDLE_ENV, EnvironmentHandle);
}

extern "C" SQLRETURN ODBC_API SQLFreeConnect(
    SQLHDBC ConnectionHandle) {

    return SQLFreeHandle(SQL_HANDLE_DBC, ConnectionHandle);
}

extern "C" SQLRETURN ODBC_API SQLSetConnectOption(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT Option,
    SQLULEN Value) {
    return SQLSetConnectAttr(ConnectionHandle,
                             static_cast<SQLINTEGER>(Option),
                             reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(Value)),
                             0);
}

extern "C" SQLRETURN ODBC_API SQLGetConnectOption(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT Option,
    SQLPOINTER ValuePtr) {
    return SQLGetConnectAttr(ConnectionHandle,
                             static_cast<SQLINTEGER>(Option),
                             ValuePtr,
                             0,
                             nullptr);
}

extern "C" SQLRETURN ODBC_API SQLSetStmtOption(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Option,
    SQLULEN Value) {
    return SQLSetStmtAttr(StatementHandle,
                          static_cast<SQLINTEGER>(Option),
                          reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(Value)),
                          0);
}

extern "C" SQLRETURN ODBC_API SQLGetStmtOption(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Option,
    SQLPOINTER ValuePtr) {
    return SQLGetStmtAttr(StatementHandle,
                          static_cast<SQLINTEGER>(Option),
                          ValuePtr,
                          0,
                          nullptr);
}

extern "C" SQLRETURN ODBC_API SQLNativeSql(
    SQLHDBC ConnectionHandle,
    SQLCHAR* InStatementText,
    SQLINTEGER TextLength1,
    SQLCHAR* OutStatementText,
    SQLINTEGER BufferLength,
    SQLINTEGER* TextLength2Ptr) {
    auto* conn = asConnection(ConnectionHandle);
    if (!conn) {
        return SQL_INVALID_HANDLE;
    }
    if (!InStatementText) {
        conn->setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    std::string sql = (TextLength1 == SQL_NTS)
        ? std::string(reinterpret_cast<const char*>(InStatementText))
        : std::string(reinterpret_cast<const char*>(InStatementText),
                      static_cast<size_t>(TextLength1));
    if (TextLength2Ptr) {
        *TextLength2Ptr = static_cast<SQLINTEGER>(sql.size());
    }
    if (!OutStatementText) {
        return SQL_SUCCESS;
    }
    if (BufferLength <= 0) {
        conn->setError("HY090", 0, "Invalid string or buffer length");
        return SQL_ERROR;
    }

    size_t copy_len = std::min(static_cast<size_t>(BufferLength - 1), sql.size());
    std::memcpy(OutStatementText, sql.data(), copy_len);
    OutStatementText[copy_len] = '\0';
    if (sql.size() >= static_cast<size_t>(BufferLength)) {
        conn->setError("01004", 0, "String data, right truncated");
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

extern "C" SQLRETURN ODBC_API SQLSetCursorName(
    SQLHSTMT StatementHandle,
    SQLCHAR* CursorName,
    SQLSMALLINT NameLength) {
    auto* stmt = asStatement(StatementHandle);
    if (!stmt) {
        return SQL_INVALID_HANDLE;
    }
    return stmt->setCursorName(CursorName, NameLength);
}

extern "C" SQLRETURN ODBC_API SQLGetCursorName(
    SQLHSTMT StatementHandle,
    SQLCHAR* CursorName,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* NameLengthPtr) {
    auto* stmt = asStatement(StatementHandle);
    if (!stmt) {
        return SQL_INVALID_HANDLE;
    }
    return stmt->getCursorName(CursorName, BufferLength, NameLengthPtr);
}

extern "C" SQLRETURN ODBC_API SQLCancelHandle(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle) {
    if (HandleType == SQL_HANDLE_STMT) {
        return SQLCancel(static_cast<SQLHSTMT>(Handle));
    }
    if (HandleType == SQL_HANDLE_DBC) {
        auto* conn = asConnection(static_cast<SQLHDBC>(Handle));
        if (!conn) {
            return SQL_INVALID_HANDLE;
        }
        return conn->cancel();
    }
    return SQL_INVALID_HANDLE;
}

extern "C" SQLRETURN ODBC_API SQLConnectW(
    SQLHDBC ConnectionHandle,
    ::SQLWCHAR* ServerName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* UserName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* Authentication,
    SQLSMALLINT NameLength3) {
    std::string server = wideToUtf8(ServerName, NameLength1);
    std::string user = wideToUtf8(UserName, NameLength2);
    std::string auth = wideToUtf8(Authentication, NameLength3);

    SQLCHAR* server_ptr = ServerName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(server.c_str())) : nullptr;
    SQLCHAR* user_ptr = UserName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(user.c_str())) : nullptr;
    SQLCHAR* auth_ptr = Authentication ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(auth.c_str())) : nullptr;
    return SQLConnect(ConnectionHandle,
                      server_ptr, ServerName ? SQL_NTS : 0,
                      user_ptr, UserName ? SQL_NTS : 0,
                      auth_ptr, Authentication ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLDriverConnectW(
    SQLHDBC ConnectionHandle,
    HWND WindowHandle,
    ::SQLWCHAR* InConnectionString,
    SQLSMALLINT StringLength1,
    ::SQLWCHAR* OutConnectionString,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLength2Ptr,
    SQLUSMALLINT DriverCompletion) {
    std::string in = wideToUtf8(InConnectionString, StringLength1);
    std::vector<SQLCHAR> out_ansi(std::max(1, static_cast<int>(BufferLength) * 4 + 1), 0);
    SQLSMALLINT out_ansi_len = 0;
    SQLRETURN rc = SQLDriverConnect(
        ConnectionHandle,
        WindowHandle,
        InConnectionString ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(in.c_str())) : nullptr,
        InConnectionString ? SQL_NTS : 0,
        OutConnectionString ? out_ansi.data() : nullptr,
        OutConnectionString ? static_cast<SQLSMALLINT>(out_ansi.size()) : 0,
        OutConnectionString ? &out_ansi_len : nullptr,
        DriverCompletion);
    if (rc == SQL_ERROR || rc == SQL_INVALID_HANDLE || rc == SQL_NO_DATA) {
        return rc;
    }
    if (OutConnectionString) {
        std::string out_text(reinterpret_cast<char*>(out_ansi.data()),
                             out_ansi_len >= 0 ? static_cast<size_t>(out_ansi_len)
                                               : std::strlen(reinterpret_cast<char*>(out_ansi.data())));
        SQLSMALLINT wide_len = 0;
        SQLRETURN wide_rc = copyWideString(utf8ToWide(out_text), OutConnectionString, BufferLength, &wide_len);
        if (StringLength2Ptr) {
            *StringLength2Ptr = wide_len;
        }
        if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
            return SQL_SUCCESS_WITH_INFO;
        }
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLBrowseConnectW(
    SQLHDBC ConnectionHandle,
    ::SQLWCHAR* InConnectionString,
    SQLSMALLINT StringLength1,
    ::SQLWCHAR* OutConnectionString,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLength2Ptr) {
    std::string in = wideToUtf8(InConnectionString, StringLength1);
    std::vector<SQLCHAR> out_ansi(std::max(1, static_cast<int>(BufferLength) * 4 + 1), 0);
    SQLSMALLINT out_ansi_len = 0;
    SQLRETURN rc = SQLBrowseConnect(
        ConnectionHandle,
        InConnectionString ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(in.c_str())) : nullptr,
        InConnectionString ? SQL_NTS : 0,
        OutConnectionString ? out_ansi.data() : nullptr,
        OutConnectionString ? static_cast<SQLSMALLINT>(out_ansi.size()) : 0,
        OutConnectionString ? &out_ansi_len : nullptr);
    if (rc == SQL_ERROR || rc == SQL_INVALID_HANDLE || rc == SQL_NO_DATA) {
        return rc;
    }
    if (OutConnectionString) {
        std::string out_text(reinterpret_cast<char*>(out_ansi.data()),
                             out_ansi_len >= 0 ? static_cast<size_t>(out_ansi_len)
                                               : std::strlen(reinterpret_cast<char*>(out_ansi.data())));
        SQLSMALLINT wide_len = 0;
        SQLRETURN wide_rc = copyWideString(utf8ToWide(out_text), OutConnectionString, BufferLength, &wide_len);
        if (StringLength2Ptr) {
            *StringLength2Ptr = wide_len;
        }
        if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
            return SQL_SUCCESS_WITH_INFO;
        }
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLPrepareW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* StatementText,
    SQLINTEGER TextLength) {
    std::string sql = wideToUtf8(StatementText, TextLength);
    return SQLPrepare(StatementHandle,
                      StatementText ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())) : nullptr,
                      StatementText ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLExecDirectW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* StatementText,
    SQLINTEGER TextLength) {
    std::string sql = wideToUtf8(StatementText, TextLength);
    return SQLExecDirect(StatementHandle,
                         StatementText ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())) : nullptr,
                         StatementText ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLGetInfoW(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT InfoType,
    SQLPOINTER InfoValuePtr,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLengthPtr) {
    if (!isStringInfoType(InfoType)) {
        return SQLGetInfo(ConnectionHandle, InfoType, InfoValuePtr, BufferLength, StringLengthPtr);
    }

    std::vector<SQLCHAR> ansi_value(2048, 0);
    SQLSMALLINT ansi_len = 0;
    SQLRETURN rc = SQLGetInfo(ConnectionHandle, InfoType, ansi_value.data(),
                              static_cast<SQLSMALLINT>(ansi_value.size()), &ansi_len);
    if (rc == SQL_ERROR || rc == SQL_INVALID_HANDLE) {
        return rc;
    }

    std::string text(reinterpret_cast<char*>(ansi_value.data()),
                     ansi_len >= 0 ? static_cast<size_t>(ansi_len)
                                   : std::strlen(reinterpret_cast<char*>(ansi_value.data())));
    SQLSMALLINT wide_len = 0;
    SQLRETURN wide_rc = copyWideString(utf8ToWide(text),
                                       static_cast<::SQLWCHAR*>(InfoValuePtr),
                                       BufferLength,
                                       &wide_len);
    if (StringLengthPtr) {
        *StringLengthPtr = wide_len;
    }
    if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
        return SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLGetDiagRecW(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT RecNumber,
    ::SQLWCHAR* SQLState,
    SQLINTEGER* NativeErrorPtr,
    ::SQLWCHAR* MessageText,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* TextLengthPtr) {
    SQLCHAR ansi_state[6] = {};
    std::vector<SQLCHAR> ansi_message(2048, 0);
    SQLSMALLINT ansi_msg_len = 0;
    SQLRETURN rc = SQLGetDiagRec(HandleType, Handle, RecNumber, ansi_state,
                                 NativeErrorPtr, ansi_message.data(),
                                 static_cast<SQLSMALLINT>(ansi_message.size()),
                                 &ansi_msg_len);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        return rc;
    }

    if (SQLState) {
        std::u16string wide_state = utf8ToWide(reinterpret_cast<char*>(ansi_state));
        for (size_t i = 0; i < 5; ++i) {
            SQLState[i] = (i < wide_state.size()) ? static_cast<::SQLWCHAR>(wide_state[i]) : 0;
        }
        SQLState[5] = 0;
    }

    std::string msg_text(reinterpret_cast<char*>(ansi_message.data()),
                         ansi_msg_len >= 0 ? static_cast<size_t>(ansi_msg_len)
                                           : std::strlen(reinterpret_cast<char*>(ansi_message.data())));
    SQLSMALLINT wide_len = 0;
    SQLRETURN wide_rc = copyWideString(utf8ToWide(msg_text), MessageText, BufferLength, &wide_len);
    if (TextLengthPtr) {
        *TextLengthPtr = wide_len;
    }
    if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
        return SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLDescribeColW(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    ::SQLWCHAR* ColumnName,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* NameLengthPtr,
    SQLSMALLINT* DataTypePtr,
    SQLULEN* ColumnSizePtr,
    SQLSMALLINT* DecimalDigitsPtr,
    SQLSMALLINT* NullablePtr) {
    SQLCHAR ansi_name[1024] = {};
    SQLSMALLINT ansi_len = 0;
    SQLRETURN rc = SQLDescribeCol(StatementHandle, ColumnNumber, ansi_name,
                                  static_cast<SQLSMALLINT>(sizeof(ansi_name)),
                                  &ansi_len, DataTypePtr, ColumnSizePtr,
                                  DecimalDigitsPtr, NullablePtr);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        return rc;
    }
    std::string name_text(reinterpret_cast<char*>(ansi_name),
                          ansi_len >= 0 ? static_cast<size_t>(ansi_len)
                                        : std::strlen(reinterpret_cast<char*>(ansi_name)));
    SQLSMALLINT wide_len = 0;
    SQLRETURN wide_rc = copyWideString(utf8ToWide(name_text), ColumnName, BufferLength, &wide_len);
    if (NameLengthPtr) {
        *NameLengthPtr = wide_len;
    }
    if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
        return SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLTablesW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3,
    ::SQLWCHAR* TableType,
    SQLSMALLINT NameLength4) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    std::string type = wideToUtf8(TableType, NameLength4);
    return SQLTables(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0,
        TableType ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(type.c_str())) : nullptr,
        TableType ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLColumnsW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3,
    ::SQLWCHAR* ColumnName,
    SQLSMALLINT NameLength4) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    std::string column = wideToUtf8(ColumnName, NameLength4);
    return SQLColumns(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0,
        ColumnName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(column.c_str())) : nullptr,
        ColumnName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLNativeSqlW(
    SQLHDBC ConnectionHandle,
    ::SQLWCHAR* InStatementText,
    SQLINTEGER TextLength1,
    ::SQLWCHAR* OutStatementText,
    SQLINTEGER BufferLength,
    SQLINTEGER* TextLength2Ptr) {
    std::string in = wideToUtf8(InStatementText, TextLength1);
    std::vector<SQLCHAR> out_ansi(std::max(1, static_cast<int>(BufferLength) * 4 + 1), 0);
    SQLINTEGER out_ansi_len = 0;
    SQLRETURN rc = SQLNativeSql(
        ConnectionHandle,
        InStatementText ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(in.c_str())) : nullptr,
        InStatementText ? SQL_NTS : 0,
        OutStatementText ? out_ansi.data() : nullptr,
        OutStatementText ? static_cast<SQLINTEGER>(out_ansi.size()) : 0,
        OutStatementText ? &out_ansi_len : nullptr);
    if (rc == SQL_ERROR || rc == SQL_INVALID_HANDLE) {
        return rc;
    }
    if (OutStatementText) {
        std::string out_text(reinterpret_cast<char*>(out_ansi.data()),
                             out_ansi_len >= 0 ? static_cast<size_t>(out_ansi_len)
                                               : std::strlen(reinterpret_cast<char*>(out_ansi.data())));
        SQLSMALLINT wide_len = 0;
        SQLRETURN wide_rc = copyWideString(utf8ToWide(out_text),
                                           OutStatementText,
                                           static_cast<SQLSMALLINT>(BufferLength),
                                           &wide_len);
        if (TextLength2Ptr) {
            *TextLength2Ptr = wide_len;
        }
        if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
            return SQL_SUCCESS_WITH_INFO;
        }
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLSetCursorNameW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CursorName,
    SQLSMALLINT NameLength) {
    std::string name = wideToUtf8(CursorName, NameLength);
    return SQLSetCursorName(
        StatementHandle,
        CursorName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(name.c_str())) : nullptr,
        CursorName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLGetCursorNameW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CursorName,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* NameLengthPtr) {
    SQLCHAR ansi_name[1024] = {};
    SQLSMALLINT ansi_len = 0;
    SQLRETURN rc = SQLGetCursorName(StatementHandle,
                                    ansi_name,
                                    static_cast<SQLSMALLINT>(sizeof(ansi_name)),
                                    &ansi_len);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        return rc;
    }
    std::string name_text(reinterpret_cast<char*>(ansi_name),
                          ansi_len >= 0 ? static_cast<size_t>(ansi_len)
                                        : std::strlen(reinterpret_cast<char*>(ansi_name)));
    SQLSMALLINT wide_len = 0;
    SQLRETURN wide_rc = copyWideString(utf8ToWide(name_text), CursorName, BufferLength, &wide_len);
    if (NameLengthPtr) {
        *NameLengthPtr = wide_len;
    }
    if (wide_rc == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
        return SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

extern "C" SQLRETURN ODBC_API SQLPrimaryKeysW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    return SQLPrimaryKeys(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLForeignKeysW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* PKCatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* PKSchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* PKTableName,
    SQLSMALLINT NameLength3,
    ::SQLWCHAR* FKCatalogName,
    SQLSMALLINT NameLength4,
    ::SQLWCHAR* FKSchemaName,
    SQLSMALLINT NameLength5,
    ::SQLWCHAR* FKTableName,
    SQLSMALLINT NameLength6) {
    std::string pk_catalog = wideToUtf8(PKCatalogName, NameLength1);
    std::string pk_schema = wideToUtf8(PKSchemaName, NameLength2);
    std::string pk_table = wideToUtf8(PKTableName, NameLength3);
    std::string fk_catalog = wideToUtf8(FKCatalogName, NameLength4);
    std::string fk_schema = wideToUtf8(FKSchemaName, NameLength5);
    std::string fk_table = wideToUtf8(FKTableName, NameLength6);
    return SQLForeignKeys(
        StatementHandle,
        PKCatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(pk_catalog.c_str())) : nullptr,
        PKCatalogName ? SQL_NTS : 0,
        PKSchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(pk_schema.c_str())) : nullptr,
        PKSchemaName ? SQL_NTS : 0,
        PKTableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(pk_table.c_str())) : nullptr,
        PKTableName ? SQL_NTS : 0,
        FKCatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(fk_catalog.c_str())) : nullptr,
        FKCatalogName ? SQL_NTS : 0,
        FKSchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(fk_schema.c_str())) : nullptr,
        FKSchemaName ? SQL_NTS : 0,
        FKTableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(fk_table.c_str())) : nullptr,
        FKTableName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLStatisticsW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLUSMALLINT Unique,
    SQLUSMALLINT Reserved) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    return SQLStatistics(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0,
        Unique,
        Reserved);
}

extern "C" SQLRETURN ODBC_API SQLSpecialColumnsW(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT IdentifierType,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3,
    SQLUSMALLINT Scope,
    SQLUSMALLINT Nullable) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    return SQLSpecialColumns(
        StatementHandle,
        IdentifierType,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0,
        Scope,
        Nullable);
}

extern "C" SQLRETURN ODBC_API SQLProceduresW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* ProcName,
    SQLSMALLINT NameLength3) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string procedure = wideToUtf8(ProcName, NameLength3);
    return SQLProcedures(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        ProcName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(procedure.c_str())) : nullptr,
        ProcName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLProcedureColumnsW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* ProcName,
    SQLSMALLINT NameLength3,
    ::SQLWCHAR* ColumnName,
    SQLSMALLINT NameLength4) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string procedure = wideToUtf8(ProcName, NameLength3);
    std::string column = wideToUtf8(ColumnName, NameLength4);
    return SQLProcedureColumns(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        ProcName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(procedure.c_str())) : nullptr,
        ProcName ? SQL_NTS : 0,
        ColumnName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(column.c_str())) : nullptr,
        ColumnName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLTablePrivilegesW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    return SQLTablePrivileges(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0);
}

extern "C" SQLRETURN ODBC_API SQLColumnPrivilegesW(
    SQLHSTMT StatementHandle,
    ::SQLWCHAR* CatalogName,
    SQLSMALLINT NameLength1,
    ::SQLWCHAR* SchemaName,
    SQLSMALLINT NameLength2,
    ::SQLWCHAR* TableName,
    SQLSMALLINT NameLength3,
    ::SQLWCHAR* ColumnName,
    SQLSMALLINT NameLength4) {
    std::string catalog = wideToUtf8(CatalogName, NameLength1);
    std::string schema = wideToUtf8(SchemaName, NameLength2);
    std::string table = wideToUtf8(TableName, NameLength3);
    std::string column = wideToUtf8(ColumnName, NameLength4);
    return SQLColumnPrivileges(
        StatementHandle,
        CatalogName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(catalog.c_str())) : nullptr,
        CatalogName ? SQL_NTS : 0,
        SchemaName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema.c_str())) : nullptr,
        SchemaName ? SQL_NTS : 0,
        TableName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())) : nullptr,
        TableName ? SQL_NTS : 0,
        ColumnName ? reinterpret_cast<SQLCHAR*>(const_cast<char*>(column.c_str())) : nullptr,
        ColumnName ? SQL_NTS : 0);
}
