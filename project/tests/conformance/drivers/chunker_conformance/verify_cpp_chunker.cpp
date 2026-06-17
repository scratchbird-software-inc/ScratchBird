// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// C++ verifier for the cross-driver statement-chunker fixture (cases.json).
// Exercises the SHARED canonical chunker that the C++ driver tools include
// (drivers/driver/cpp/tools/sb_statement_chunker.hpp), so this test proves the
// real implementation — not a copy — reproduces the fixture.
//
// Build (from repo root):
//   g++ -std=c++17 -I project/drivers/driver/cpp/include \
//       -I project/drivers/driver/cpp/tools \
//       project/tests/conformance/drivers/chunker_conformance/verify_cpp_chunker.cpp \
//       -o /tmp/verify_cpp_chunker
//   /tmp/verify_cpp_chunker project/tests/conformance/drivers/chunker_conformance/cases.json
//
// Exit 0 = all cases pass.
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "sb_statement_chunker.hpp"

int main(int argc, char** argv) {
    const std::string casesPath =
        argc > 1 ? argv[1]
                 : "tests/conformance/drivers/chunker_conformance/cases.json";
    std::ifstream in(casesPath);
    if (!in) {
        std::cerr << "cannot open " << casesPath << "\n";
        return 2;
    }
    nlohmann::json doc;
    in >> doc;

    int failures = 0;
    for (const auto& c : doc.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string input = c.at("input").get<std::string>();
        std::vector<std::string> expected =
            c.at("expected").get<std::vector<std::string>>();
        const std::vector<std::string> got = sbchunk::splitStatements(input);
        if (got == expected) {
            std::cout << "[PASS] " << name << "\n";
        } else {
            ++failures;
            std::cout << "[FAIL] " << name << "\n";
            std::cout << "   expected (" << expected.size() << "):\n";
            for (const auto& s : expected) std::cout << "     | " << s << "\n";
            std::cout << "   got (" << got.size() << "):\n";
            for (const auto& s : got) std::cout << "     | " << s << "\n";
        }
    }
    const std::size_t total = doc.at("cases").size();
    std::cout << "\n" << (total - failures) << "/" << total
              << " chunker conformance cases passed\n";
    return failures ? 1 : 0;
}
