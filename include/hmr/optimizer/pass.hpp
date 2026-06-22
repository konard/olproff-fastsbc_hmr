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

// pass.hpp — CRTP base for optimizer passes (zero-cost static polymorphism).
//
// Each optimization pass derives from PassBase<Derived> and provides:
//   * `static constexpr std::string_view kName;`
//   * `std::size_t apply(ast::Ruleset&);`   // returns the number of changes
//
// PassBase::run() wraps apply() and labels the result. There is no virtual
// dispatch inside a pass — the call to apply() is statically resolved through
// the CRTP, so a release build inlines the whole pass. The Optimizer composes
// passes into a pipeline (Strategy + chain-of-responsibility).

#pragma once

#include <cstddef>
#include <string_view>

#include "hmr/ast/ast.hpp"

namespace hmr::opt {

struct PassResult {
    std::string_view name;
    std::size_t changes = 0;  // transformations applied (0 == no change)
};

template <class Derived>
class PassBase {
public:
    [[nodiscard]] PassResult run(ast::Ruleset& rs) {
        return PassResult{Derived::kName,
                          static_cast<Derived&>(*this).apply(rs)};
    }

    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return Derived::kName;
    }
};

}  // namespace hmr::opt
