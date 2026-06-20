// SPDX-License-Identifier: MIT
//
// load_and_apply — manual end-to-end harness for a *generated* HMR module.
//
//   1. dlopen a .so produced by `hmrc compile`
//   2. build a SIP message + a runtime context from the module's descriptor
//   3. call hmr_apply and print the verdict + mutated message
//
// Build (host must export hmr_rt_* for the module to resolve them):
//   g++ -std=c++23 -O2 -rdynamic -Iinclude experiments/load_and_apply.cpp \
//       build/libhmr_core.a -ldl -o /tmp/load_and_apply
//   /tmp/load_and_apply <module.so>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>

#include "hmr/runtime/Runtime.hpp"
#include "hmr/runtime/SipMessage.hpp"
#include "hmr/runtime/hmr_runtime.h"

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

    void* infoSym = ::dlsym(h, "hmr_module_info");
    void* applySym = ::dlsym(h, "hmr_apply");
    if (!infoSym || !applySym) {
        std::fprintf(stderr, "missing exports\n");
        return 1;
    }
    auto* info = static_cast<const HmrModuleInfo*>(infoSym);
    hmr_apply_fn apply = nullptr;
    std::memcpy(&apply, &applySym, sizeof apply);

    std::printf("module: %s  abi=%u slots=%u regexes=%u\n", info->name,
                info->abi_version, info->num_slots, info->num_regexes);

    // A representative outbound request that leaks internal topology.
    const char* raw =
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice@internal.local>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Server: SecretPBX/9.9\r\n"
        "User-Agent: InternalUA\r\n"
        "X-Internal-Route: hop-1\r\n"
        "Contact: <sip:alice@10.0.0.1>\r\n";
    HmrSipMsg msg = HmrSipMsg::parse(raw);

    auto ctx = hmr::runtime::makeContext(*info);
    ctx.setVar(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.setVar(HMR_VAR_TRUNK_GROUP, "tg-42");
    ctx.setVar(HMR_VAR_REALM, "core.example.net");
    ctx.resetForApply();

    std::printf("--- before ---\n%s\n", msg.toString().c_str());
    int verdict = apply(&msg, &ctx);
    std::printf("--- verdict = %d ---\n", verdict);
    std::printf("--- after ---\n%s\n", msg.toString().c_str());

    ::dlclose(h);
    return 0;
}
