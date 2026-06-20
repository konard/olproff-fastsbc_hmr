// SPDX-License-Identifier: MIT
//
// ModuleManager.cpp — dlopen/dlsym loading with RCU-style hot replacement.

#include "hmr/module/ModuleManager.hpp"

#include <dlfcn.h>

#include <cstring>
#include <string>
#include <utility>

namespace hmr::module {

LoadedModule::LoadedModule(void* handle, hmr_apply_fn applyFn,
                           const HmrModuleInfo* info, std::string path)
    : handle_(handle),
      applyFn_(applyFn),
      info_(info),
      path_(std::move(path)) {}

LoadedModule::~LoadedModule() {
    if (handle_) ::dlclose(handle_);
}

namespace {

// Resolve a symbol, bit-copying the void* into a typed pointer. The explicit
// memcpy sidesteps the ISO-C++ object↔function pointer cast diagnostic while
// relying only on POSIX's guarantee that dlsym yields a usable address.
template <class T>
T symbolAs(void* handle, const char* name) {
    void* sym = ::dlsym(handle, name);
    if (!sym) return nullptr;
    T out{};
    std::memcpy(&out, &sym, sizeof out);
    return out;
}

}  // namespace

Result<std::shared_ptr<const LoadedModule>> ModuleManager::load(
    const std::string& path) {
    ::dlerror();  // clear any stale error
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* e = ::dlerror();
        std::string msg =
            "module: dlopen('" + path + "') failed: " + (e ? e : "unknown error");
        notify({ModuleEventKind::LoadFailed, path, {}, msg});
        return make_error(std::move(msg));
    }

    auto fail = [&](std::string msg) -> std::unexpected<Error> {
        ::dlclose(handle);
        notify({ModuleEventKind::LoadFailed, path, {}, msg});
        return make_error(std::move(msg));
    };

    auto applyFn = symbolAs<hmr_apply_fn>(handle, "hmr_apply");
    if (!applyFn) return fail("module: '" + path + "' is missing 'hmr_apply'");

    auto info = symbolAs<const HmrModuleInfo*>(handle, "hmr_module_info");
    if (!info) return fail("module: '" + path + "' is missing 'hmr_module_info'");

    if (info->abi_version != HMR_ABI_VERSION)
        return fail("module: ABI mismatch in '" + path + "' (module v" +
                    std::to_string(info->abi_version) + ", host v" +
                    std::to_string(HMR_ABI_VERSION) + ")");

    const bool isReload = current_.load(std::memory_order_acquire) != nullptr;

    auto module = std::make_shared<LoadedModule>(handle, applyFn, info, path);
    // Publish: release-store so a reader's acquire-load sees a fully built node.
    current_.store(module, std::memory_order_release);

    notify({isReload ? ModuleEventKind::Reloaded : ModuleEventKind::Loaded, path,
            module->name(), {}});
    return std::shared_ptr<const LoadedModule>(std::move(module));
}

void ModuleManager::subscribe(ModuleObserver* observer) {
    if (!observer) return;
    std::lock_guard lock(observerMtx_);
    for (auto* o : observers_)
        if (o == observer) return;  // idempotent
    observers_.push_back(observer);
}

void ModuleManager::unsubscribe(ModuleObserver* observer) {
    std::lock_guard lock(observerMtx_);
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
        if (*it == observer) {
            observers_.erase(it);
            return;
        }
    }
}

void ModuleManager::notify(const ModuleEvent& event) {
    std::vector<ModuleObserver*> snapshot;
    {
        std::lock_guard lock(observerMtx_);
        snapshot = observers_;
    }
    for (auto* o : snapshot) o->onModuleEvent(event);
}

}  // namespace hmr::module
