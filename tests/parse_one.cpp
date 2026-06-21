// SPDX-License-Identifier: MIT
//
// parse_one — minimal front-end driver used by the Meson sample tests.
//
//   parse_one [--expect-fail] <file.hmr>
//
// Without --expect-fail it parses the file and exits 0 on success, 1 on parse
// or semantic errors. With --expect-fail the meaning is inverted: exit 0 only
// when the file is *correctly* rejected, 1 if it unexpectedly parses. Crashes
// (non-zero, non-1 status) therefore always fail the test — unlike Meson's
// should_fail, this proves the parser recovers from bad input rather than
// aborting on it. Exit 2 is reserved for usage / I/O errors.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "hmr/parser/parser.hpp"

int main(int argc, char** argv) {
    bool expect_fail = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect-fail") == 0) {
            expect_fail = true;
        } else if (path == nullptr) {
            path = argv[i];
        } else {
            std::fprintf(stderr, "usage: parse_one [--expect-fail] <file.hmr>\n");
            return 2;
        }
    }
    if (path == nullptr) {
        std::fprintf(stderr, "usage: parse_one [--expect-fail] <file.hmr>\n");
        return 2;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "parse_one: cannot open %s\n", path);
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    hmr::parser::Parser parser(ss.str());
    auto result = parser.parse();
    const bool ok = result.has_value();
    if (ok) {
        for (const auto& w : parser.warnings())
            std::fprintf(stderr, "%s\n", w.format().c_str());
        std::printf("OK: %s — %zu header-rule(s)\n", path,
                    result->header_rules.size());
    } else {
        std::fprintf(stderr, "%s\n", result.error().format().c_str());
    }

    if (expect_fail) return ok ? 1 : 0;
    return ok ? 0 : 1;
}
