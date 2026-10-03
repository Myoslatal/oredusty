// The framework's dynamic loader: loading a library at runtime, finding symbols in it, and giving it
// back. The library under test is built from tests/fixtures/module_fixture.cpp and its path is baked
// in at configure time, so this runs wherever the suite runs.
#include <support/test_support.h>

#include <t2d/core/module.h>

#include <string>

using namespace t2d;

namespace {

[[nodiscard]] const char* fixture_path() { return T2D_TEST_MODULE_PATH; }

} // namespace

T2D_TEST(loading_a_module_and_calling_its_functions) {
    std::string error;
    Module module = Module::load(fixture_path(), &error);
    if (!module.valid()) {
        T2D_CHECK_MSG(false, "could not load '{}': {}", fixture_path(), error);
        return;
    }
    T2D_CHECK(module.error().empty());
    T2D_CHECK_EQ(module.path(), std::string(fixture_path()));

    const auto answer = module.function<int()>("t2d_fixture_answer");
    const auto greeting = module.function<const char*()>("t2d_fixture_greeting");
    const auto add = module.function<int(int, int)>("t2d_fixture_add");
    T2D_REQUIRE(answer != nullptr);
    T2D_REQUIRE(greeting != nullptr);
    T2D_REQUIRE(add != nullptr);
    T2D_CHECK_EQ(answer(), 42);
    T2D_CHECK_EQ(std::string(greeting()), std::string("fixture"));
    T2D_CHECK_EQ(add(2, 3), 5);
    T2D_CHECK_EQ(add(-1, 1), 0);

    // The state lives inside the library: two calls in a row see the same counter, which a fresh copy
    // of the code would not.
    const auto counter = module.function<int()>("t2d_fixture_counter");
    T2D_REQUIRE(counter != nullptr);
    const int first = counter();
    T2D_CHECK_EQ(counter(), first + 1);
}

T2D_TEST(a_missing_symbol_is_reported_and_the_module_stays_usable) {
    std::string error;
    Module module = Module::load(fixture_path(), &error);
    T2D_REQUIRE(module.valid());

    T2D_CHECK(module.symbol("t2d_fixture_not_here") == nullptr);
    T2D_CHECK(module.error().find("t2d_fixture_not_here") != std::string::npos);
    T2D_CHECK(module.error().find(fixture_path()) != std::string::npos);
    // A failed lookup must not poison the module: the next one still works.
    T2D_CHECK(module.symbol("t2d_fixture_answer") != nullptr);
    T2D_CHECK(module.error().empty());

    // An empty name is refused with a message rather than handed to the loader.
    T2D_CHECK(module.symbol("") == nullptr);
    T2D_CHECK_FALSE(module.error().empty());
    T2D_CHECK(module.symbol(nullptr) == nullptr);
}

T2D_TEST(loading_something_that_is_not_a_library_fails_with_a_reason) {
    std::string error;
    Module missing = Module::load("/nonexistent/tile2d/libnope.so", &error);
    T2D_CHECK_FALSE(missing.valid());
    T2D_CHECK_FALSE(error.empty());
    T2D_CHECK_FALSE(missing.error().empty());
    // A handle that was never loaded cannot resolve anything, and says so instead of crashing.
    T2D_CHECK(missing.symbol("anything") == nullptr);
    T2D_CHECK_FALSE(missing.error().empty());
    missing.unload();   // unloading nothing is a no-op

    // A real file that is not a library is refused by the loader, not by us.
    std::string text_error;
    Module not_a_library = Module::load(std::string(T2D_SOURCE_DIR) + "/CMakeLists.txt", &text_error);
    T2D_CHECK_FALSE(not_a_library.valid());
    T2D_CHECK_FALSE(text_error.empty());
}

T2D_TEST(a_module_can_be_unloaded_and_loaded_again) {
    Module first = Module::load(fixture_path());
    T2D_REQUIRE(first.valid());
    const auto answer = first.function<int()>("t2d_fixture_answer");
    T2D_REQUIRE(answer != nullptr);
    T2D_CHECK_EQ(answer(), 42);
    first.unload();
    T2D_CHECK_FALSE(first.valid());
    T2D_CHECK(first.symbol("t2d_fixture_answer") == nullptr);

    // Loading it again gives a working module: the counter starts over, because the library was
    // really unloaded and mapped again.
    Module second = Module::load(fixture_path());
    T2D_REQUIRE(second.valid());
    const auto counter = second.function<int()>("t2d_fixture_counter");
    T2D_REQUIRE(counter != nullptr);
    T2D_CHECK_EQ(counter(), 1);
    T2D_CHECK_EQ(counter(), 2);
}

T2D_TEST(moving_a_module_moves_the_handle) {
    Module source = Module::load(fixture_path());
    T2D_REQUIRE(source.valid());
    Module moved = std::move(source);
    T2D_CHECK_FALSE(source.valid());          // the moved-from module owns nothing
    T2D_CHECK(moved.valid());
    const auto answer = moved.function<int()>("t2d_fixture_answer");
    T2D_REQUIRE(answer != nullptr);
    T2D_CHECK_EQ(answer(), 42);

    Module other = Module::load(fixture_path());
    T2D_REQUIRE(other.valid());
    other = std::move(moved);                 // assigning over a loaded module unloads it first
    T2D_CHECK(other.valid());
    T2D_CHECK_FALSE(moved.valid());
    T2D_CHECK_EQ(other.function<int()>("t2d_fixture_answer")(), 42);
    // Both destructors run here; a double unload would show up as a crash or a sanitizer report.
}

T2D_TEST_MAIN
