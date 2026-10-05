// A 32 bit absolute relocation. Ordinary C++ does not emit one; a hand written one does, and the
// runtime used to write it without a range check - so the value it produced was silently truncated,
// and a table is placed by mmap, which never hands back an address below 4 GiB (docs/ABI.md H5).
extern "C" int abs32_target() { return 7; }

extern "C" unsigned abs32_slot;
asm(".globl abs32_slot\n.section .data\n.align 4\nabs32_slot:\n.long abs32_target\n.previous\n");

extern "C" int abs32_probe() { return static_cast<int>(abs32_slot); }
