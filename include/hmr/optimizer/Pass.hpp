// SPDX-License-Identifier: MIT
//
// Pass.hpp — CRTP base for optimizer passes (zero-cost static polymorphism).
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

#include "hmr/ast/Ast.hpp"

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
