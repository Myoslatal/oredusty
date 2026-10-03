// Tile2D - loading code that was not compiled into this program.
//
// The framework ships no game, so it also ships no mod API: this is only the part that is the same for
// everybody - ask the operating system for a library, find a symbol in it, and give it back when it is
// no longer needed. What the symbol means, and whether calling it is safe, is the caller's contract.
//
// A loaded module is a *move only* value: copying it would mean two owners and a double unload.
#pragma once

#include <t2d/core/types.h>

#include <string>
#include <type_traits>

namespace t2d {

/// A dynamically loaded library.
class Module {
public:
    Module() = default;
    Module(Module&& other) noexcept;
    Module& operator=(Module&& other) noexcept;
    ~Module();
    T2D_NON_COPYABLE(Module);

    /// Loads \p path. On failure the result is empty and \p error (when given) says what the loader
    /// said. Loading the same path twice gives two independent handles.
    [[nodiscard]] static Module load(const std::string& path, std::string* error = nullptr);

    [[nodiscard]] bool valid() const { return handle_ != nullptr; }
    [[nodiscard]] const std::string& path() const { return path_; }
    /// What went wrong with the last symbol() call (empty when it succeeded).
    [[nodiscard]] const std::string& error() const { return error_; }

    /// Resolves one symbol. Returns nullptr and fills error() when the library does not export it -
    /// which is a normal thing to happen, not a crash.
    [[nodiscard]] void* symbol(const char* name);
    /// The typed form, taking the *function type*: function<int(int)>() returns an int(*)(int), and
    /// nullptr when the symbol is missing. Whether the signature matches what the library exports
    /// cannot be checked here - the ABI version is how a module says which shape its symbols have.
    template <class Signature>
    [[nodiscard]] Signature* function(const char* name) {
        static_assert(std::is_function_v<Signature>, "pass a function type, e.g. function<int(int)>()");
        return reinterpret_cast<Signature*>(symbol(name));
    }

    /// Unloads the library now; the destructor does the same. A module that is still running code -
    /// a callback the host is inside of - must not be unloaded, and only the caller can know that.
    void unload();

private:
    void* handle_ = nullptr;
    std::string path_;
    std::string error_;
};

} // namespace t2d
