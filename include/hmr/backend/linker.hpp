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

// linker.hpp — links a relocatable object into a loadable shared module.
//
// A generated HMR module references the host's `hmr_rt_*` runtime entry points
// as *undefined* symbols: the host process (the SBC) provides the runtime, and
// each rule module is a hot-swappable plug-in resolved against it at dlopen
// time. That mirrors how a real SBC ships a stable runtime and reloads rules
// without restarting. Producing such a .so only needs `-shared` (undefined
// symbols are permitted in shared objects by default on ELF).
//
// We drive the system toolchain's `cc`/`ld` via posix_spawn rather than calling
// into LLD's library API: it keeps the public surface free of LLVM/LLD headers,
// avoids a hard dependency on LLD being built with the installed LLVM, and uses
// exactly the linker the platform already trusts. The driver is overridable
// (LinkOptions::driver or the $HMR_CC environment variable) for cross builds.

#pragma once

#include <string>
#include <vector>

#include "hmr/backend/backend.hpp"
#include "hmr/diagnostics/diagnostics.hpp"

namespace hmr::backend {

struct LinkOptions {
    std::string output_path = "a.so";    // destination shared object
    std::string driver;                  // empty → $HMR_CC, else "cc"
    std::vector<std::string> extra_args;  // appended verbatim to the link line
    bool strip_debug = true;             // pass -Wl,--strip-debug
};

class Linker {
public:
    // Write `obj` to a temporary file and link it into a shared object at
    // opts.output_path. Returns an error (including captured linker output) on
    // any toolchain failure.
    [[nodiscard]] Result<void> link_shared_object(const ObjectCode& obj,
                                                  const LinkOptions& opts);
};

}  // namespace hmr::backend
