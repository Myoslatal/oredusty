// A section that asks for more alignment than a page. A module's halves start on a page, so it cannot
// be placed where it asked to be, and saying so beats placing it somewhere it did not ask for
// (docs/ABI.md H10).
alignas(8192) char aligned_buffer[16] = {1};

extern "C" int aligned_probe() { return aligned_buffer[0]; }
