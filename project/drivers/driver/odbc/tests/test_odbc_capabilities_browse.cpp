// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>

#define private public
#include "scratchbird/odbc/metadata_helpers.h"
#include "scratchbird/odbc/odbc_handles.h"
#include "scratchbird/odbc/odbc_client_bridge.h"
#include "scratchbird/odbc/odbc_driver.h"
#undef private

namespace {

class FakeBrowseClientBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        (void)columns;
        results.clear();
        rows_affected = 0;

        if (sql == "SHOW DATABASES") {
            results = {{"db_main"}, {"db_reporting"}};
            return SQL_SUCCESS;
        }
        if (sql == scratchbird::odbc::metadata::kSchemasQuery) {
            results = {
                {"public"},
                {"analytics"},
                {"database.default.users"},
                {"database.default.audit"},
                {"users.alice.dev"},
                {"users.bob.dev"},
                {"users.bob.dev"}
            };
            return SQL_SUCCESS;
        }
        if (sql == scratchbird::odbc::metadata::kTablesQuery) {
            results = {
                {"users", "public", "TABLE"},
                {"orders", "public", "TABLE"},
                {"events", "analytics", "TABLE"},
                {"session_log", "database.default.users", "TABLE"},
                {"audit_entry", "database.default.audit", "TABLE"},
                {"profile", "users.alice.dev", "TABLE"},
                {"profile", "users.bob.dev", "TABLE"}
            };
            return SQL_SUCCESS;
        }
        if (sql == scratchbird::odbc::metadata::kColumnsQuery) {
            results = {
                {"id", "users", "public", "INTEGER", "1", "NO", "PRI"},
                {"name", "users", "public", "VARCHAR", "2", "YES", ""},
                {"created_at", "users", "public", "TIMESTAMP", "3", "YES", ""},
                {"event_id", "events", "analytics", "INTEGER", "1", "NO", "PRI"},
                {"payload", "events", "analytics", "JSON", "2", "YES", ""},
                {"id", "profile", "users.alice.dev", "INTEGER", "1", "NO", "PRI"},
                {"id", "profile", "users.bob.dev", "INTEGER", "1", "NO", "PRI"}
            };
            return SQL_SUCCESS;
        }
        return SQL_ERROR;
    }
};

class ScopedOdbcIni {
public:
    explicit ScopedOdbcIni(const std::string& path) : path_(path) {
        const char* existing = std::getenv("ODBCINI");
        if (existing) {
            had_existing_ = true;
            old_value_ = existing;
        }
#if defined(_WIN32)
        _putenv_s("ODBCINI", path_.c_str());
#else
        setenv("ODBCINI", path_.c_str(), 1);
#endif
    }

    ~ScopedOdbcIni() {
#if defined(_WIN32)
        if (had_existing_) {
            _putenv_s("ODBCINI", old_value_.c_str());
        } else {
            _putenv_s("ODBCINI", "");
        }
#else
        if (had_existing_) {
            setenv("ODBCINI", old_value_.c_str(), 1);
        } else {
            unsetenv("ODBCINI");
        }
#endif
    }

private:
    std::string path_;
    bool had_existing_{false};
    std::string old_value_;
};

class OdbcCapabilityBrowseTest : public ::testing::Test {
protected:
    scratchbird::odbc::OdbcEnvironment env_{};
    scratchbird::odbc::OdbcConnection conn_{&env_};

    void SetUp() override {
        conn_.connected_ = true;
        conn_.current_database_ = "db_main";
        conn_.current_schema_ = "public";
        conn_.params_.dsn = "MainDSN";
        conn_.client_bridge_ = std::make_unique<FakeBrowseClientBridge>();
    }

    static std::string writeIniFile() {
        char path_template[] = "/tmp/sb_odbc_ini_XXXXXX";
        int fd = mkstemp(path_template);
        if (fd < 0) {
            return {};
        }
        std::ofstream out(path_template);
        out << "[odbc data sources]\n";
        out << "AlphaDSN=ScratchBird\n";
        out << "BetaDSN=ScratchBird\n";
        out << "\n[AlphaDSN]\nDriver=ScratchBird\n";
        out.close();
        ::close(fd);
        return path_template;
    }
};


static std::vector<SQLUSMALLINT> expectedSupportedFunctions() {
    return {
        SQL_API_SQLALLOCCONNECT,
        SQL_API_SQLALLOCENV,
        SQL_API_SQLALLOCSTMT,
        SQL_API_SQLALLOCHANDLE,
        SQL_API_SQLFREECONNECT,
        SQL_API_SQLFREEENV,
        SQL_API_SQLFREESTMT,
        SQL_API_SQLFREEHANDLE,
        SQL_API_SQLENDTRAN,
        SQL_API_SQLCONNECT,
        SQL_API_SQLDRIVERCONNECT,
        SQL_API_SQLBROWSECONNECT,
        SQL_API_SQLDISCONNECT,
        SQL_API_SQLSETCONNECTATTR,
        SQL_API_SQLGETCONNECTATTR,
        SQL_API_SQLSETCONNECTOPTION,
        SQL_API_SQLGETCONNECTOPTION,
        SQL_API_SQLSETENVATTR,
        SQL_API_SQLGETENVATTR,
        SQL_API_SQLSETSTMTATTR,
        SQL_API_SQLGETSTMTATTR,
        SQL_API_SQLSETSTMTOPTION,
        SQL_API_SQLGETSTMTOPTION,
        SQL_API_SQLPREPARE,
        SQL_API_SQLEXECUTE,
        SQL_API_SQLEXECDIRECT,
        SQL_API_SQLCANCEL,
        SQL_API_SQLCANCELHANDLE,
        SQL_API_SQLCLOSECURSOR,
        SQL_API_SQLGETCURSORNAME,
        SQL_API_SQLSETCURSORNAME,
        SQL_API_SQLNATIVESQL,
        SQL_API_SQLBULKOPERATIONS,
        SQL_API_SQLSETPOS,
        SQL_API_SQLFETCH,
        SQL_API_SQLFETCHSCROLL,
        SQL_API_SQLMORERESULTS,
        SQL_API_SQLBINDCOL,
        SQL_API_SQLBINDPARAM,
        SQL_API_SQLBINDPARAMETER,
        SQL_API_SQLNUMPARAMS,
        SQL_API_SQLDESCRIBEPARAM,
        SQL_API_SQLDESCRIBECOL,
        SQL_API_SQLNUMRESULTCOLS,
        SQL_API_SQLCOLATTRIBUTE,
        SQL_API_SQLSETDESCREC,
        SQL_API_SQLGETDESCREC,
        SQL_API_SQLSETDESCFIELD,
        SQL_API_SQLGETDESCFIELD,
        SQL_API_SQLCOPYDESC,
        SQL_API_SQLROWCOUNT,
        SQL_API_SQLGETDATA,
        SQL_API_SQLPARAMDATA,
        SQL_API_SQLPUTDATA,
        SQL_API_SQLGETDIAGFIELD,
        SQL_API_SQLGETDIAGREC,
        SQL_API_SQLERROR,
        SQL_API_SQLTABLES,
        SQL_API_SQLCOLUMNS,
        SQL_API_SQLPRIMARYKEYS,
        SQL_API_SQLFOREIGNKEYS,
        SQL_API_SQLSTATISTICS,
        SQL_API_SQLSPECIALCOLUMNS,
        SQL_API_SQLPROCEDURES,
        SQL_API_SQLPROCEDURECOLUMNS,
        SQL_API_SQLTABLEPRIVILEGES,
        SQL_API_SQLCOLUMNPRIVILEGES,
        SQL_API_SQLGETFUNCTIONS,
        SQL_API_SQLGETINFO,
        SQL_API_SQLGETTYPEINFO,
    };
}

static bool isFunctionAdvertised(const SQLUSMALLINT* function_map, SQLUSMALLINT function_id) {
    if (!function_map) {
        return false;
    }
    if (function_id >= SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * 16) {
        return false;
    }
    std::size_t word = static_cast<std::size_t>(function_id >> 4);
    std::size_t bit = static_cast<std::size_t>(function_id & 0x0F);
    return ((function_map[word] >> bit) & 1u) != 0;
}

static std::size_t countOccurrences(const std::string& value, const std::string& token) {
    if (token.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = value.find(token, pos)) != std::string::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

static std::string trim(const std::string& value) {
    std::size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

struct FunctionMatrixRow {
    SQLUSMALLINT function_id{0};
    SQLUSMALLINT advertised{0};
};

struct InfoMatrixRow {
    SQLUSMALLINT info_type{0};
    std::string value_kind;
    std::string expected;
};

static bool parseMatrixUint(const std::string& text, SQLUSMALLINT* out) {
    if (!out) {
        return false;
    }
    try {
        auto value = std::stoul(trim(text));
        if (value > 0xFFFFu) {
            return false;
        }
        *out = static_cast<SQLUSMALLINT>(value);
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<FunctionMatrixRow> loadFunctionMatrixCsv(const std::string& path,
                                                            std::string* error) {
    std::vector<FunctionMatrixRow> rows;
    std::ifstream in(path);
    if (!in.good()) {
        if (error) {
            *error = "Failed to open function matrix: " + path;
        }
        return rows;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        std::string row = trim(line);
        if (row.empty() || row[0] == '#') {
            continue;
        }
        if (line_number == 1 && row.find("function_id") != std::string::npos) {
            continue;
        }

        std::stringstream ss(row);
        std::string function_id_text;
        std::string advertised_text;
        if (!std::getline(ss, function_id_text, ',') ||
            !std::getline(ss, advertised_text)) {
            if (error) {
                *error = "Malformed function matrix row at line " + std::to_string(line_number);
            }
            rows.clear();
            return rows;
        }

        FunctionMatrixRow parsed;
        if (!parseMatrixUint(function_id_text, &parsed.function_id) ||
            !parseMatrixUint(advertised_text, &parsed.advertised)) {
            if (error) {
                *error = "Invalid numeric value in function matrix row at line " +
                         std::to_string(line_number);
            }
            rows.clear();
            return rows;
        }
        rows.push_back(parsed);
    }

    return rows;
}

static std::vector<InfoMatrixRow> loadInfoMatrixCsv(const std::string& path,
                                                    std::string* error) {
    std::vector<InfoMatrixRow> rows;
    std::ifstream in(path);
    if (!in.good()) {
        if (error) {
            *error = "Failed to open info matrix: " + path;
        }
        return rows;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        std::string row = trim(line);
        if (row.empty() || row[0] == '#') {
            continue;
        }
        if (line_number == 1 && row.find("info_type") != std::string::npos) {
            continue;
        }

        auto first_comma = row.find(',');
        auto second_comma = (first_comma == std::string::npos)
                                ? std::string::npos
                                : row.find(',', first_comma + 1);
        if (first_comma == std::string::npos || second_comma == std::string::npos) {
            if (error) {
                *error = "Malformed info matrix row at line " + std::to_string(line_number);
            }
            rows.clear();
            return rows;
        }

        std::string info_type_text = row.substr(0, first_comma);
        std::string value_kind = row.substr(first_comma + 1, second_comma - first_comma - 1);
        std::string expected = row.substr(second_comma + 1);

        InfoMatrixRow parsed;
        if (!parseMatrixUint(info_type_text, &parsed.info_type)) {
            if (error) {
                *error = "Invalid info_type in info matrix row at line " +
                         std::to_string(line_number);
            }
            rows.clear();
            return rows;
        }
        parsed.value_kind = trim(value_kind);
        parsed.expected = trim(expected);
        rows.push_back(parsed);
    }

    return rows;
}

TEST(OdbcMetadataShapingTest, DatabaseDefaultRowsExposeDefaultBranchPaths) {
    auto rows = scratchbird::odbc::metadata::buildDatabaseDefaultMetadataRows(
        {"database.default.users", "database.default.audit"},
        "db_main",
        true);

    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows[0].kind, scratchbird::odbc::metadata::MetadataTreeRowKind::DATABASE);
    EXPECT_EQ(rows[0].path, "db_main");

    std::vector<std::string> schema_paths;
    std::vector<std::string> top_branches;
    for (const auto& row : rows) {
        if (row.kind != scratchbird::odbc::metadata::MetadataTreeRowKind::SCHEMA) {
            continue;
        }
        schema_paths.push_back(row.path);
        if (row.top_level_branch) {
            top_branches.push_back(row.path);
        }
    }

    EXPECT_EQ(schema_paths,
              (std::vector<std::string>{
                  "database",
                  "database.default",
                  "database.default.users",
                  "database.default.audit"}));
    EXPECT_EQ(top_branches, (std::vector<std::string>{"database"}));
}

TEST(OdbcMetadataShapingTest, ParentExpansionAndPerParentUniquenessAreStable) {
    auto expanded = scratchbird::odbc::metadata::metadataSchemaPathsForNavigation(
        {"users.alice.dev", "users.bob.dev", "users.bob.dev"},
        true);

    EXPECT_EQ(expanded,
              (std::vector<std::string>{
                  "users",
                  "users.alice",
                  "users.alice.dev",
                  "users.bob",
                  "users.bob.dev"}));

    auto tree = scratchbird::odbc::metadata::buildMetadataSchemaTree(
        {"users.bob.dev", "users.bob.dev", "users.bob.prod"},
        "db_main",
        true);
    const auto* bob = scratchbird::odbc::metadata::findMetadataSchemaNodeByPath(
        tree.schemas,
        "users.bob");
    ASSERT_NE(bob, nullptr);
    ASSERT_EQ(bob->children.size(), 2u);
    EXPECT_EQ(bob->children[0]->full_path, "users.bob.dev");
    EXPECT_EQ(bob->children[1]->full_path, "users.bob.prod");
}

TEST(OdbcMetadataShapingTest, SameLeafNameUnderDifferentParentsIsDistinct) {
    auto tree = scratchbird::odbc::metadata::buildMetadataSchemaTree(
        {"users.alice.dev", "users.bob.dev"},
        "db_main",
        true);

    const auto* alice_leaf = scratchbird::odbc::metadata::findMetadataSchemaNodeByPath(
        tree.schemas,
        "users.alice.dev");
    const auto* bob_leaf = scratchbird::odbc::metadata::findMetadataSchemaNodeByPath(
        tree.schemas,
        "users.bob.dev");

    ASSERT_NE(alice_leaf, nullptr);
    ASSERT_NE(bob_leaf, nullptr);
    EXPECT_EQ(alice_leaf->name, "dev");
    EXPECT_EQ(bob_leaf->name, "dev");
    EXPECT_NE(alice_leaf->full_path, bob_leaf->full_path);
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectListsAvailableDsnsWhenNotYetConnected) {
    auto ini_path = writeIniFile();
    ASSERT_FALSE(ini_path.empty());
    ScopedOdbcIni scoped(ini_path);

    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;

    auto rc = conn_.browseConnect(nullptr, SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_GT(out_len, 0);
    std::string out = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(out.find("DSN=AlphaDSN"), std::string::npos);
    EXPECT_NE(out.find("DSN=BetaDSN"), std::string::npos);
    EXPECT_TRUE(std::remove(ini_path.c_str()) == 0);
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectTraversesCatalogSchemaTableColumns) {
    std::string input;
    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    SQLRETURN rc;

    input = "DSN=MainDSN;CATALOG=db_main;";
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string schema_level = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(schema_level.find("SCHEMA=public"), std::string::npos);
    EXPECT_NE(schema_level.find("SCHEMA=analytics"), std::string::npos);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=public;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string table_level = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(table_level.find("TABLE=users"), std::string::npos);
    EXPECT_NE(table_level.find("TABLE=orders"), std::string::npos);
    EXPECT_EQ(table_level.find("TABLE=events"), std::string::npos);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=public;TABLE=users;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string column_level = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(column_level.find("COLUMN=id"), std::string::npos);
    EXPECT_NE(column_level.find("COLUMN=name"), std::string::npos);
    EXPECT_NE(column_level.find("COLUMN=created_at"), std::string::npos);
    EXPECT_EQ(column_level.find("COLUMN=payload"), std::string::npos);
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectExpandsRecursiveSchemaBranchesBeforeTables) {
    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    SQLRETURN rc = SQL_SUCCESS;

    std::string input = "DSN=MainDSN;CATALOG=db_main;";
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string root_schemas = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(root_schemas.find("SCHEMA=database"), std::string::npos);
    EXPECT_NE(root_schemas.find("SCHEMA=users"), std::string::npos);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=database;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string database_branch = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(database_branch.find("SCHEMA=database.default"), std::string::npos);
    EXPECT_EQ(database_branch.find("TABLE="), std::string::npos);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=database.default;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string leaf_branches = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(leaf_branches.find("SCHEMA=database.default.users"), std::string::npos);
    EXPECT_NE(leaf_branches.find("SCHEMA=database.default.audit"), std::string::npos);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=database.default.users;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string leaf_tables = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(leaf_tables.find("TABLE=session_log"), std::string::npos);
    EXPECT_EQ(leaf_tables.find("TABLE=audit_entry"), std::string::npos);
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectDeduplicatesSiblingLeavesAndKeepsParentIdentity) {
    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    SQLRETURN rc = SQL_SUCCESS;

    std::string input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=users;";
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string users_branch = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(users_branch.find("SCHEMA=users.alice"), std::string::npos);
    EXPECT_NE(users_branch.find("SCHEMA=users.bob"), std::string::npos);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=users.bob;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string bob_branch = reinterpret_cast<const char*>(out_conn);
    EXPECT_EQ(countOccurrences(bob_branch, "SCHEMA=users.bob.dev"), 1u);

    input = "DSN=MainDSN;CATALOG=db_main;SCHEMA=users.alice;";
    std::fill(std::begin(out_conn), std::end(out_conn), 0);
    rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                             SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string alice_branch = reinterpret_cast<const char*>(out_conn);
    EXPECT_EQ(countOccurrences(alice_branch, "SCHEMA=users.alice.dev"), 1u);
    EXPECT_NE(alice_branch.find("SCHEMA=users.alice.dev"), std::string::npos);
    EXPECT_NE(bob_branch.find("SCHEMA=users.bob.dev"), std::string::npos);
}

TEST_F(OdbcCapabilityBrowseTest, GetInfoAndGetFunctionsReportNoFalsePositives) {
    char value[8] = {};
    SQLSMALLINT len = 0;
    EXPECT_EQ(conn_.getInfo(SQL_MULT_RESULT_SETS, value, sizeof(value), &len), SQL_SUCCESS);
    EXPECT_STREQ(value, "N");
    EXPECT_EQ(conn_.getInfo(SQL_MULTIPLE_ACTIVE_TXN, value, sizeof(value), &len), SQL_SUCCESS);
    EXPECT_STREQ(value, "N");

    SQLUSMALLINT function_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
    EXPECT_EQ(conn_.getFunctions(SQL_API_ODBC3_ALL_FUNCTIONS, function_map), SQL_SUCCESS);
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLGETCURSORNAME));
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLNATIVESQL));
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLPARAMDATA));
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLPUTDATA));
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLSETCURSORNAME));
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLCONNECT));
    EXPECT_TRUE(isFunctionAdvertised(function_map, SQL_API_SQLTABLES));

    SQLUSMALLINT unsupported = 0;
    EXPECT_EQ(conn_.getFunctions(SQL_API_SQLGETCURSORNAME, &unsupported), SQL_SUCCESS);
    EXPECT_EQ(unsupported, 1);
    EXPECT_EQ(conn_.getFunctions(SQL_API_SQLGETFUNCTIONS, &unsupported), SQL_SUCCESS);
    EXPECT_EQ(unsupported, 1);
}

TEST_F(OdbcCapabilityBrowseTest, GetFunctionsAdvertisesOnlyImplementedFunctions) {
    SQLUSMALLINT function_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
    ASSERT_EQ(conn_.getFunctions(SQL_API_ODBC3_ALL_FUNCTIONS, function_map), SQL_SUCCESS);

    auto expected = expectedSupportedFunctions();
    std::sort(expected.begin(), expected.end());

    for (auto func_id : expected) {
        EXPECT_TRUE(isFunctionAdvertised(function_map, func_id))
            << "Expected function is not advertised: " << func_id;
    }

    for (std::size_t word = 0; word < SQL_API_ODBC3_ALL_FUNCTIONS_SIZE; ++word) {
        SQLUSMALLINT bits = function_map[word];
        for (std::size_t bit = 0; bit < 16; ++bit) {
            if ((bits >> bit) & 1u) {
                SQLUSMALLINT func_id = static_cast<SQLUSMALLINT>((word << 4) + bit);
                EXPECT_TRUE(std::binary_search(expected.begin(), expected.end(), func_id))
                    << "Unexpected advertised function id: " << func_id;
            }
        }
    }

    const char* matrix_path = std::getenv("ODBC_008_CAPABILITY_MATRIX_PATH");
    if (matrix_path && std::strlen(matrix_path) > 0) {
        std::ofstream matrix_file(matrix_path);
        ASSERT_TRUE(matrix_file.good()) << "Failed to open capability matrix path: " << matrix_path;
        matrix_file << "function_id,advertised\n";
        const SQLUSMALLINT max_function_id =
            static_cast<SQLUSMALLINT>(SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * 16);
        for (SQLUSMALLINT func_id = 0; func_id < max_function_id; ++func_id) {
            matrix_file << func_id << ',' << (isFunctionAdvertised(function_map, func_id) ? 1 : 0) << '\n';
        }
    }
}

TEST_F(OdbcCapabilityBrowseTest, GetFunctionsSupportsAllFunctionsBitmapAlias) {
    SQLUSMALLINT all_functions_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
    SQLUSMALLINT legacy_function_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
    ASSERT_EQ(conn_.getFunctions(0, all_functions_map), SQL_SUCCESS);
    ASSERT_EQ(conn_.getFunctions(SQL_API_ODBC3_ALL_FUNCTIONS, legacy_function_map), SQL_SUCCESS);
    EXPECT_TRUE(std::equal(std::begin(all_functions_map),
                           std::end(all_functions_map),
                           std::begin(legacy_function_map)));
}

TEST_F(OdbcCapabilityBrowseTest, DriverEntryGetFunctionsMatchesConnectionGetter) {
    SQLUSMALLINT handle_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
    SQLUSMALLINT driver_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};

    ASSERT_EQ(conn_.getFunctions(SQL_API_ODBC3_ALL_FUNCTIONS, handle_map), SQL_SUCCESS);
    ASSERT_EQ(SQLGetFunctions(reinterpret_cast<SQLHDBC>(&conn_),
                              SQL_API_ODBC3_ALL_FUNCTIONS,
                              driver_map),
              SQL_SUCCESS);
    EXPECT_TRUE(std::equal(std::begin(handle_map), std::end(handle_map), std::begin(driver_map)));
}

TEST_F(OdbcCapabilityBrowseTest, DriverEntryGetInfoMatchesConnectionGetter) {
    std::array<SQLUSMALLINT, 4> string_info = {
        SQL_DBMS_NAME,
        SQL_MULT_RESULT_SETS,
        SQL_XOPEN_CLI_YEAR,
        SQL_MAX_ROW_SIZE_INCLUDES_LONG,
    };

    for (auto info_type : string_info) {
        char handle_value[128] = {};
        char driver_value[128] = {};
        SQLSMALLINT handle_len = 0;
        SQLSMALLINT driver_len = 0;
        ASSERT_EQ(conn_.getInfo(info_type, handle_value, sizeof(handle_value), &handle_len), SQL_SUCCESS);
        ASSERT_EQ(SQLGetInfo(reinterpret_cast<SQLHDBC>(&conn_),
                             info_type,
                             driver_value,
                             sizeof(driver_value),
                             &driver_len),
                  SQL_SUCCESS);
        EXPECT_STREQ(handle_value, driver_value);
        EXPECT_EQ(handle_len, driver_len);
    }

    SQLUSMALLINT u16_handle = 0;
    SQLUSMALLINT u16_driver = 0;
    SQLSMALLINT len_handle = 0;
    SQLSMALLINT len_driver = 0;
    ASSERT_EQ(conn_.getInfo(SQL_CATALOG_LOCATION, &u16_handle, sizeof(u16_handle), &len_handle), SQL_SUCCESS);
    ASSERT_EQ(SQLGetInfo(reinterpret_cast<SQLHDBC>(&conn_),
                         SQL_CATALOG_LOCATION,
                         &u16_driver,
                         sizeof(u16_driver),
                         &len_driver),
              SQL_SUCCESS);
    EXPECT_EQ(u16_handle, u16_driver);
    EXPECT_EQ(len_handle, len_driver);

    SQLUINTEGER u32_handle = 0;
    SQLUINTEGER u32_driver = 0;
    ASSERT_EQ(conn_.getInfo(SQL_OJ_CAPABILITIES, &u32_handle, sizeof(u32_handle), &len_handle), SQL_SUCCESS);
    ASSERT_EQ(SQLGetInfo(reinterpret_cast<SQLHDBC>(&conn_),
                         SQL_OJ_CAPABILITIES,
                         &u32_driver,
                         sizeof(u32_driver),
                         &len_driver),
              SQL_SUCCESS);
    EXPECT_EQ(u32_handle, u32_driver);
    EXPECT_EQ(len_handle, len_driver);
}

TEST_F(OdbcCapabilityBrowseTest, GetFunctionsCanCompareAgainstExpectedCsvMatrix) {
    const char* matrix_path = std::getenv("ODBC_008_EXPECTED_FUNCTION_MATRIX_PATH");
    if (!matrix_path || std::strlen(matrix_path) == 0) {
        GTEST_SKIP() << "ODBC_008_EXPECTED_FUNCTION_MATRIX_PATH is not set";
    }

    std::string parse_error;
    auto expected_rows = loadFunctionMatrixCsv(matrix_path, &parse_error);
    ASSERT_TRUE(parse_error.empty()) << parse_error;
    ASSERT_FALSE(expected_rows.empty()) << "Expected function matrix is empty: " << matrix_path;

    SQLUSMALLINT function_map[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
    ASSERT_EQ(conn_.getFunctions(SQL_API_ODBC3_ALL_FUNCTIONS, function_map), SQL_SUCCESS);

    for (const auto& row : expected_rows) {
        EXPECT_EQ(isFunctionAdvertised(function_map, row.function_id), row.advertised != 0)
            << "Capability matrix mismatch for function id " << row.function_id;
    }
}

TEST_F(OdbcCapabilityBrowseTest, GetInfoCanCompareAgainstExpectedCsvMatrix) {
    const char* matrix_path = std::getenv("ODBC_008_EXPECTED_INFO_MATRIX_PATH");
    if (!matrix_path || std::strlen(matrix_path) == 0) {
        GTEST_SKIP() << "ODBC_008_EXPECTED_INFO_MATRIX_PATH is not set";
    }

    std::string parse_error;
    auto expected_rows = loadInfoMatrixCsv(matrix_path, &parse_error);
    ASSERT_TRUE(parse_error.empty()) << parse_error;
    ASSERT_FALSE(expected_rows.empty()) << "Expected info matrix is empty: " << matrix_path;

    for (const auto& row : expected_rows) {
        if (row.value_kind == "string") {
            char value[256] = {};
            SQLSMALLINT len = 0;
            ASSERT_EQ(conn_.getInfo(row.info_type, value, sizeof(value), &len), SQL_SUCCESS)
                << "SQLGetInfo failed for info_type " << row.info_type;
            EXPECT_EQ(std::string(value), row.expected)
                << "Info matrix string mismatch for info_type " << row.info_type;
            continue;
        }
        if (row.value_kind == "u16") {
            SQLUSMALLINT value = 0;
            SQLSMALLINT len = 0;
            ASSERT_EQ(conn_.getInfo(row.info_type, &value, sizeof(value), &len), SQL_SUCCESS)
                << "SQLGetInfo failed for info_type " << row.info_type;
            SQLUSMALLINT expected = 0;
            ASSERT_TRUE(parseMatrixUint(row.expected, &expected))
                << "Invalid expected u16 value in info matrix for info_type " << row.info_type;
            EXPECT_EQ(value, expected)
                << "Info matrix u16 mismatch for info_type " << row.info_type;
            continue;
        }
        if (row.value_kind == "u32") {
            SQLUINTEGER value = 0;
            SQLSMALLINT len = 0;
            ASSERT_EQ(conn_.getInfo(row.info_type, &value, sizeof(value), &len), SQL_SUCCESS)
                << "SQLGetInfo failed for info_type " << row.info_type;
            SQLUSMALLINT expected_u16 = 0;
            ASSERT_TRUE(parseMatrixUint(row.expected, &expected_u16))
                << "Invalid expected u32 value in info matrix for info_type " << row.info_type;
            EXPECT_EQ(value, static_cast<SQLUINTEGER>(expected_u16))
                << "Info matrix u32 mismatch for info_type " << row.info_type;
            continue;
        }
        FAIL() << "Unsupported value_kind in info matrix for info_type "
               << row.info_type << ": " << row.value_kind;
    }
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectPathFallbackParsesHierarchicalPath) {
    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    auto input = std::string("PATH=MainDSN/db_main/public/users;");
    auto rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                                 SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string row_columns = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(row_columns.find("COLUMN=id"), std::string::npos);
    EXPECT_NE(row_columns.find("COLUMN=name"), std::string::npos);
    EXPECT_NE(row_columns.find("COLUMN=created_at"), std::string::npos);
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectPathFallbackPreservesDottedSchemaSegments) {
    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    auto input = std::string("PATH=MainDSN/db_main/users.alice.dev;");
    auto rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                                  SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string table_level = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(table_level.find("TABLE=profile"), std::string::npos);
}

TEST_F(OdbcCapabilityBrowseTest, BrowseConnectRawPathWithoutKeyFallsBackToPath) {
    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    auto input = std::string("MainDSN/db_main/public/users;");
    auto rc = conn_.browseConnect(reinterpret_cast<SQLCHAR*>(input.data()),
                                 SQL_NTS, out_conn, sizeof(out_conn), &out_len);
    ASSERT_EQ(rc, SQL_NEED_DATA);
    std::string row_columns = reinterpret_cast<const char*>(out_conn);
    EXPECT_NE(row_columns.find("COLUMN=id"), std::string::npos);
    EXPECT_NE(row_columns.find("COLUMN=name"), std::string::npos);
    EXPECT_NE(row_columns.find("COLUMN=created_at"), std::string::npos);
}

TEST_F(OdbcCapabilityBrowseTest, NullEnvConnectionPoolingDefaultsPropagateToNewEnvironments) {
    SQLHENV env = SQL_NULL_HENV;
    constexpr SQLUINTEGER desired_pooling = SQL_CP_ONE_PER_DRIVER;
    SQLUINTEGER pooling = SQL_CP_OFF;
    SQLINTEGER len = 0;

    ASSERT_EQ(SQLSetEnvAttr(SQL_NULL_HENV, SQL_ATTR_CONNECTION_POOLING,
                           reinterpret_cast<SQLPOINTER>(desired_pooling), 0),
              SQL_SUCCESS);
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_SUCCESS);
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(SQLGetEnvAttr(env, SQL_ATTR_CONNECTION_POOLING, &pooling, sizeof(pooling), &len),
              SQL_SUCCESS);
    EXPECT_EQ(pooling, desired_pooling);
    EXPECT_EQ(len, sizeof(pooling));

    ASSERT_EQ(SQLFreeHandle(SQL_HANDLE_ENV, env), SQL_SUCCESS);

    // Restore default to avoid leaking state to other tests.
    ASSERT_EQ(SQLSetEnvAttr(SQL_NULL_HENV, SQL_ATTR_CONNECTION_POOLING,
                           reinterpret_cast<SQLPOINTER>(SQL_CP_OFF), 0),
              SQL_SUCCESS);
}

TEST_F(OdbcCapabilityBrowseTest, GetInfoSupportsCommonCapabilityProbeSet) {
    SQLUINTEGER u32 = 1234;
    SQLSMALLINT len = 0;

    EXPECT_EQ(conn_.getInfo(SQL_ASYNC_MODE, &u32, sizeof(u32), &len), SQL_SUCCESS);
    EXPECT_EQ(u32, 0u);
    EXPECT_EQ(len, static_cast<SQLSMALLINT>(sizeof(u32)));

    u32 = 1234;
    EXPECT_EQ(conn_.getInfo(SQL_BATCH_SUPPORT, &u32, sizeof(u32), &len), SQL_SUCCESS);
    EXPECT_EQ(u32, 0u);

    u32 = 1234;
    EXPECT_EQ(conn_.getInfo(SQL_BATCH_ROW_COUNT, &u32, sizeof(u32), &len), SQL_SUCCESS);
    EXPECT_EQ(u32, 0u);

    u32 = 1234;
    EXPECT_EQ(conn_.getInfo(SQL_OJ_CAPABILITIES, &u32, sizeof(u32), &len), SQL_SUCCESS);
    EXPECT_EQ(u32, 0u);

    u32 = 1234;
    EXPECT_EQ(conn_.getInfo(SQL_SQL92_PREDICATES, &u32, sizeof(u32), &len), SQL_SUCCESS);
    EXPECT_EQ(u32, 0u);

    SQLUSMALLINT u16 = 1234;
    EXPECT_EQ(conn_.getInfo(SQL_CATALOG_LOCATION, &u16, sizeof(u16), &len), SQL_SUCCESS);
    EXPECT_EQ(u16, 1u);
    EXPECT_EQ(len, static_cast<SQLSMALLINT>(sizeof(u16)));

    u16 = 1234;
    EXPECT_EQ(conn_.getInfo(SQL_MAX_COLUMNS_IN_SELECT, &u16, sizeof(u16), &len), SQL_SUCCESS);
    EXPECT_EQ(u16, 0u);

    char text[64] = {};
    EXPECT_EQ(conn_.getInfo(SQL_XOPEN_CLI_YEAR, text, sizeof(text), &len), SQL_SUCCESS);
    EXPECT_STREQ(text, "1995");

    std::memset(text, 0, sizeof(text));
    EXPECT_EQ(conn_.getInfo(SQL_MAX_ROW_SIZE_INCLUDES_LONG, text, sizeof(text), &len), SQL_SUCCESS);
    EXPECT_STREQ(text, "Y");
}

}  // namespace
