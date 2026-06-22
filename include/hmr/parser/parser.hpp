// fastsbc_hmr - The HMR DSL was inspired by Oracle's HMR language. It is a compiler based on the LLVM backend that generates a library for dynamic loading.
// Copyright (C) 2026  fastsbc_hmr contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// parser.hpp — front-end entry point building an AST from HMR source text.
//
// The grammar lives in grammar/Hmr.g4 and is compiled to a C++ lexer/parser by
// ANTLR4 (`antlr4 -Dlanguage=Cpp -visitor -no-listener`). This class is a thin
// visitor over the generated parse tree: it maps Oracle keyword spellings to
// the strongly-typed AST via ast::AstFactory, folds match-value / new-value
// expressions through ast::Value, and turns recognized-but-unsupported blocks
// and unknown attributes into warnings (error recovery) instead of aborting.
//
// Error recovery: syntax errors are collected from ANTLR's lexer and parser
// error listeners and semantic errors (unknown enum values) are accumulated
// while walking the tree; parse() returns an aggregate Error listing them all
// rather than stopping at the first.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "hmr/ast/ast.hpp"
#include "hmr/diagnostics/diagnostics.hpp"

namespace hmr::parser {

class Parser {
public:
    explicit Parser(std::string_view source) : source_(source) {}

    // Parse the whole source into a Ruleset. Non-fatal issues accumulate in
    // warnings(); on failure the Error aggregates every collected diagnostic.
    [[nodiscard]] Result<ast::Ruleset> parse();

    [[nodiscard]] const std::vector<Diagnostic>& warnings() const {
        return warnings_;
    }

    // Convenience: parse `source` in one call, discarding warnings.
    [[nodiscard]] static Result<ast::Ruleset> parse_string(
        std::string_view source);

private:
    std::string source_;
    std::vector<Diagnostic> warnings_;
};

}  // namespace hmr::parser
