// Tile2D - the library test_module loads. Built as a MODULE library (a plain shared object, nothing
// links against it at build time) so the test can prove that loading code at runtime works.
//
// The symbols are extern "C" on purpose: C++ name mangling is not a stable interface between a host
// and a library that were compiled separately, which is exactly what this file is about.
extern "C" {

int t2d_fixture_answer() { return 42; }

const char* t2d_fixture_greeting() { return "fixture"; }

int t2d_fixture_add(int a, int b) { return a + b; }

/// Counts calls: the state lives inside the library, so a test can tell "the same instance" from
/// "some other copy of the code".
int t2d_fixture_counter() {
    static int calls = 0;
    return ++calls;
}

} // extern "C"
