// The same constructor with a priority, and the probe: which of the two ran first is the whole point.
extern "C" char ctor_order[4] = {0, 0, 0, 0};
extern "C" int ctor_at = 0;

__attribute__((constructor(101))) static void ctor_early() { ctor_order[ctor_at++] = 'A'; }

extern "C" int ctor_probe() {
    int packed = 0;
    for (int index = 0; index < ctor_at; ++index) packed = packed * 10 + (ctor_order[index] - 'A' + 1);
    return packed;   // 14 = "A,D", what a linker does; 41 = "D,A", the order the sections were in
}
