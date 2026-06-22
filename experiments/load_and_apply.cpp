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

// load_and_apply — manual end-to-end harness for a *generated* HMR module.
//
//   1. dlopen a .so produced by `hmrc compile`
//   2. build a SIP message + a runtime context from the module's descriptor
//   3. call hmr_apply and print the verdict + mutated message
//
// Build (host must export hmr_rt_* for the module to resolve them):
//   g++ -std=c++23 -O2 -rdynamic -Iinclude experiments/load_and_apply.cpp \
//       builddir/libhmr_core.a -ldl -o /tmp/load_and_apply
//   /tmp/load_and_apply <module.so>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "hmr/runtime/arena.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/hmr_runtime.h"
#include "hmr/runtime/sip_message.hpp"

namespace {

// Serialize `msg` into `scratch` and write it to stdout.
void print_msg(const HmrSipMsg& msg, HmrArena& scratch) {
    scratch.reset();
    const HmrStr s = msg.serialize(scratch);
    std::fwrite(s.data, 1, s.len, stdout);
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <module.so>\n", argv[0]);
        return 2;
    }

    void* h = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "dlopen failed: %s\n", ::dlerror());
        return 1;
    }

    void* info_sym = ::dlsym(h, "hmr_module_info");
    void* apply_sym = ::dlsym(h, "hmr_apply");
    if (!info_sym || !apply_sym) {
        std::fprintf(stderr, "missing exports\n");
        return 1;
    }
    auto* info = static_cast<const HmrModuleInfo*>(info_sym);
    hmr_apply_fn apply = nullptr;
    std::memcpy(&apply, &apply_sym, sizeof apply);

    std::printf("module: %s  abi=%u slots=%u regexes=%u\n", info->name,
                info->abi_version, info->num_slots, info->num_regexes);

    // A representative outbound request that leaks internal topology. The raw
    // bytes have static storage, so the slices the message holds stay valid.
    const char* raw =
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice@internal.local>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Server: SecretPBX/9.9\r\n"
        "User-Agent: InternalUA\r\n"
        "X-Internal-Route: hop-1\r\n"
        "Contact: <sip:alice@10.0.0.1>\r\n";
    HmrSipMsg msg = HmrSipMsg::parse(raw);

    auto ctx = hmr::runtime::make_context(*info);
    ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.set_var(HMR_VAR_TRUNK_GROUP, "tg-42");
    ctx.set_var(HMR_VAR_REALM, "core.example.net");
    ctx.reset_for_apply();

    // A dedicated scratch arena to render snapshots, independent of the context.
    auto scratch = std::make_unique<HmrArena>();

    std::printf("--- before ---\n");
    print_msg(msg, *scratch);
    int verdict = apply(&msg, &ctx);
    std::printf("--- verdict = %d ---\n", verdict);
    std::printf("--- after ---\n");
    print_msg(msg, *scratch);

    ::dlclose(h);
    return 0;
}
