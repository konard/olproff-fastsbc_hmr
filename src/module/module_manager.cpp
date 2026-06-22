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

// module_manager.cpp — dlopen/dlsym loading with RCU-style hot replacement.

#include "hmr/module/module_manager.hpp"

#include <dlfcn.h>

#include <cstring>
#include <string>
#include <utility>

namespace hmr::module {

LoadedModule::LoadedModule(void* handle, hmr_apply_fn apply_fn,
                           const HmrModuleInfo* info, std::string path)
    : handle_(handle),
      apply_fn_(apply_fn),
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
T symbol_as(void* handle, const char* name) {
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

    auto apply_fn = symbol_as<hmr_apply_fn>(handle, "hmr_apply");
    if (!apply_fn) return fail("module: '" + path + "' is missing 'hmr_apply'");

    auto info = symbol_as<const HmrModuleInfo*>(handle, "hmr_module_info");
    if (!info) return fail("module: '" + path + "' is missing 'hmr_module_info'");

    if (info->abi_version != HMR_ABI_VERSION)
        return fail("module: ABI mismatch in '" + path + "' (module v" +
                    std::to_string(info->abi_version) + ", host v" +
                    std::to_string(HMR_ABI_VERSION) + ")");

    const bool is_reload = current_.load(std::memory_order_acquire) != nullptr;

    auto module = std::make_shared<LoadedModule>(handle, apply_fn, info, path);
    // Publish: release-store so a reader's acquire-load sees a fully built node.
    current_.store(module, std::memory_order_release);

    notify({is_reload ? ModuleEventKind::Reloaded : ModuleEventKind::Loaded, path,
            module->name(), {}});
    return std::shared_ptr<const LoadedModule>(std::move(module));
}

void ModuleManager::subscribe(ModuleObserver* observer) {
    if (!observer) return;
    std::lock_guard lock(observer_mtx_);
    for (auto* o : observers_)
        if (o == observer) return;  // idempotent
    observers_.push_back(observer);
}

void ModuleManager::unsubscribe(ModuleObserver* observer) {
    std::lock_guard lock(observer_mtx_);
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
        std::lock_guard lock(observer_mtx_);
        snapshot = observers_;
    }
    for (auto* o : snapshot) o->on_module_event(event);
}

}  // namespace hmr::module
