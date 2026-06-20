// SPDX-License-Identifier: MIT
//
// Linker.hpp — links a relocatable object into a loadable shared module.
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

#include "hmr/backend/Backend.hpp"
#include "hmr/diagnostics/Diagnostics.hpp"

namespace hmr::backend {

struct LinkOptions {
    std::string outputPath = "a.so";   // destination shared object
    std::string driver;                // empty → $HMR_CC, else "cc"
    std::vector<std::string> extraArgs;  // appended verbatim to the link line
    bool stripDebug = true;            // pass -Wl,--strip-debug
};

class Linker {
public:
    // Write `obj` to a temporary file and link it into a shared object at
    // opts.outputPath. Returns an error (including captured linker output) on
    // any toolchain failure.
    [[nodiscard]] Result<void> linkSharedObject(const ObjectCode& obj,
                                                const LinkOptions& opts);
};

}  // namespace hmr::backend
