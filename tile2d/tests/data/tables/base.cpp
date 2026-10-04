// The "game" half of the code table tests. It is compiled on its own, so the function another file
// calls is reached through a relocation - which is the only reason a table merged later can replace
// it. A single translation unit that inlined the call would leave nothing to replace.
extern "C" int base_value() { return 10; }

extern "C" {
int counter = 3;
}

extern "C" int read_counter() { return counter; }
