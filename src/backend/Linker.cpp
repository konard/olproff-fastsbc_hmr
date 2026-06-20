// SPDX-License-Identifier: MIT
//
// Linker.cpp — drive the system toolchain to produce a shared module.

#include "hmr/backend/Linker.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern char** environ;

namespace hmr::backend {

namespace {

// RAII temp file: owns an fd from mkstemps and unlinks the path on destruction.
struct TempFile {
    std::string path;
    int fd = -1;

    TempFile() = default;
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    void reset() {
        if (fd >= 0) ::close(fd);
        if (!path.empty()) ::unlink(path.c_str());
        fd = -1;
        path.clear();
    }
    ~TempFile() { reset(); }
};

std::string tmpDir() {
    if (const char* t = std::getenv("TMPDIR"); t && *t) return t;
    return "/tmp";
}

bool createTemp(TempFile& tf, const char* suffix, std::string& err) {
    const std::string tmpl = tmpDir() + "/hmrlinkXXXXXX" + suffix;
    std::vector<char> buf(tmpl.c_str(), tmpl.c_str() + tmpl.size() + 1);
    const int fd = ::mkstemps(buf.data(), static_cast<int>(std::strlen(suffix)));
    if (fd < 0) {
        err = std::string("linker: mkstemps failed: ") + std::strerror(errno);
        return false;
    }
    tf.reset();
    tf.fd = fd;
    tf.path.assign(buf.data());
    return true;
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

Result<void> Linker::linkSharedObject(const ObjectCode& obj,
                                      const LinkOptions& opts) {
    std::string err;

    TempFile objFile;
    if (!createTemp(objFile, ".o", err)) return make_error(err);
    {
        std::ofstream out(objFile.path, std::ios::binary | std::ios::trunc);
        if (!out) return make_error("linker: cannot open temp object for writing");
        out.write(reinterpret_cast<const char*>(obj.bytes.data()),
                  static_cast<std::streamsize>(obj.bytes.size()));
        if (!out) return make_error("linker: failed writing temp object");
    }

    TempFile logFile;
    if (!createTemp(logFile, ".log", err)) return make_error(err);

    std::string driver = opts.driver;
    if (driver.empty())
        if (const char* e = std::getenv("HMR_CC"); e && *e) driver = e;
    if (driver.empty()) driver = "cc";

    std::vector<std::string> args = {driver, "-shared", "-o", opts.outputPath,
                                     objFile.path};
    if (opts.stripDebug) args.insert(args.begin() + 2, "-Wl,--strip-debug");
    for (const auto& a : opts.extraArgs) args.push_back(a);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // Route the child's stdout+stderr into the log temp file for diagnostics.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, logFile.path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, driver.c_str(), &fa, nullptr, argv.data(),
                                environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0)
        return make_error("linker: cannot spawn '" + driver +
                          "': " + std::strerror(rc));

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
        return make_error(std::string("linker: waitpid failed: ") +
                          std::strerror(errno));

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return make_error("linker: '" + driver + "' failed (exit " +
                          std::to_string(code) + ")\n" + readAll(logFile.path));
    }
    return {};
}

}  // namespace hmr::backend
