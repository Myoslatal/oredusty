#include <t2d/core/module.h>

#include <format>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace t2d {
namespace {

#if defined(_WIN32)
using NativeHandle = HMODULE;

[[nodiscard]] NativeHandle open_library(const char* path, std::string& error) {
    // LOAD_WITH_ALTERED_SEARCH_PATH so a module's own dependencies next to it are found.
    HMODULE handle = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (handle == nullptr) error = std::format("LoadLibraryEx failed ({})", GetLastError());
    return handle;
}

void close_library(NativeHandle handle) { FreeLibrary(handle); }

[[nodiscard]] void* find_symbol(NativeHandle handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}

[[nodiscard]] std::string last_error() { return "the loader reported no detail"; }
#else
using NativeHandle = void*;

[[nodiscard]] NativeHandle open_library(const char* path, std::string& error) {
    // RTLD_NOW: a module with an unresolved symbol fails here, where the message names it, instead of
    // somewhere later in the middle of a call.
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* message = dlerror();
        error = message != nullptr ? message : "dlopen failed";
    }
    return handle;
}

void close_library(NativeHandle handle) { dlclose(handle); }

[[nodiscard]] void* find_symbol(NativeHandle handle, const char* name) {
    return dlsym(handle, name);
}

[[nodiscard]] std::string last_error() {
    const char* message = dlerror();
    return message != nullptr ? message : "dlsym failed";
}
#endif

} // namespace

Module::Module(Module&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)), error_(std::move(other.error_)) {
    other.handle_ = nullptr;
}

Module& Module::operator=(Module&& other) noexcept {
    if (this == &other) return *this;
    unload();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    error_ = std::move(other.error_);
    other.handle_ = nullptr;
    return *this;
}

Module::~Module() { unload(); }

Module Module::load(const std::string& path, std::string* error) {
    Module module;
    module.path_ = path;
    std::string message;
    module.handle_ = open_library(path.c_str(), message);
    if (module.handle_ == nullptr) {
        module.error_ = message;
        module.path_.clear();
        if (error != nullptr) *error = message;
    }
    return module;
}

void* Module::symbol(const char* name) {
    if (handle_ == nullptr) {
        error_ = "the module is not loaded";
        return nullptr;
    }
    if (name == nullptr || name[0] == '\0') {
        error_ = "an empty symbol name was asked for";
        return nullptr;
    }
    // dlerror() must be cleared first: it reports the *last* error, which may be an old one.
#if !defined(_WIN32)
    dlerror();
#endif
    void* address = find_symbol(handle_, name);
    error_ = address != nullptr ? std::string{} : std::format("'{}' is not exported by {}", name, path_);
    if (address == nullptr && !error_.empty()) {
#if !defined(_WIN32)
        const std::string detail = last_error();
        if (!detail.empty()) error_ = std::format("{} ({})", error_, detail);
#endif
    }
    return address;
}

void Module::unload() {
    if (handle_ == nullptr) return;
    close_library(handle_);
    handle_ = nullptr;
}

} // namespace t2d
