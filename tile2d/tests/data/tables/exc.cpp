// A fixture compiled *with* exceptions, which the toolchain does not do by default. The runtime
// registers the frame descriptions a table brings, so a throw inside one is walked back out to its
// handler - inside the table, or in the program that loaded it (docs/ABI.md H1).
extern "C" int exc_thrower() { throw 7; }

extern "C" int exc_catcher() {
    try {
        return exc_thrower();
    } catch (int value) {
        return value;
    }
}
