// SPDX-License-Identifier: MIT
//
// module_manager.hpp — loads generated HMR modules and hot-swaps them safely.
//
// A compiled ruleset is a shared object exporting `hmr_apply` + `hmr_module_info`
// (see hmr_runtime.h). The manager dlopen's such a file, resolves and validates
// those two exports, and publishes the result as the *current* module.
//
// Hot replacement is RCU-style. The current module is held in a
// std::atomic<std::shared_ptr<const LoadedModule>>:
//   * Readers call current() to take a shared_ptr snapshot. They keep applying
//     against it with no lock; the module cannot be unloaded while any snapshot
//     is alive.
//   * A writer calls load(), which builds the replacement and atomically stores
//     it. The previous module's shared_ptr refcount drops to zero only once the
//     last in-flight reader releases its snapshot — that reference-count decay
//     is the grace period, and the destructor's dlclose is the reclamation.
//
// Observers (GoF Observer) are notified of load / reload / failure events so a
// host can log or re-prepare per-thread contexts on a swap.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hmr/diagnostics/diagnostics.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace hmr::module {

// A live, dlopen'd HMR module: an OS handle plus its two resolved exports.
// Destroying it dlcloses the handle, so a module's lifetime is governed purely
// by shared_ptr reference counting — the basis of the RCU-style hot swap.
class LoadedModule {
public:
    LoadedModule(void* handle, hmr_apply_fn apply_fn, const HmrModuleInfo* info,
                 std::string path);
    ~LoadedModule();

    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    // Invoke the module's entry point against a message + per-thread context.
    [[nodiscard]] int apply(HmrSipMsg* msg, HmrContext* ctx) const {
        return apply_fn_(msg, ctx);
    }

    [[nodiscard]] const HmrModuleInfo& info() const noexcept { return *info_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string name() const {
        return info_ && info_->name ? std::string(info_->name) : std::string{};
    }

private:
    void* handle_;
    hmr_apply_fn apply_fn_;
    const HmrModuleInfo* info_;
    std::string path_;
};

// What happened to a module — delivered to observers.
enum class ModuleEventKind { Loaded, Reloaded, LoadFailed };

struct ModuleEvent {
    ModuleEventKind kind;
    std::string path;
    std::string module_name;  // from info->name (empty on failure)
    std::string error;        // populated for LoadFailed
};

// GoF Observer: implement to react to module lifecycle events.
class ModuleObserver {
public:
    virtual ~ModuleObserver() = default;
    virtual void on_module_event(const ModuleEvent& event) = 0;
};

class ModuleManager {
public:
    ModuleManager() = default;
    ~ModuleManager() = default;

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    // Load (or hot-swap) the module at `path`. On success the new module
    // atomically becomes current(); the previous one is unloaded once the last
    // reader releases its snapshot. The first successful load reports `Loaded`,
    // subsequent ones `Reloaded`. Failures leave the current module unchanged.
    [[nodiscard]] Result<std::shared_ptr<const LoadedModule>> load(
        const std::string& path);

    // RCU acquire: a snapshot of the active module (nullptr if none), safe to
    // use without locking for as long as the returned shared_ptr is held.
    [[nodiscard]] std::shared_ptr<const LoadedModule> current() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    // Observer registration. Call before going multi-threaded; registration
    // itself is mutex-guarded but is not meant to race with hot loads.
    void subscribe(ModuleObserver* observer);
    void unsubscribe(ModuleObserver* observer);

private:
    void notify(const ModuleEvent& event);

    std::atomic<std::shared_ptr<const LoadedModule>> current_{nullptr};
    std::mutex observer_mtx_;
    std::vector<ModuleObserver*> observers_;
};

}  // namespace hmr::module
