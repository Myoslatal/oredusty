// An absolute symbol: a number the object carries rather than something to place. A linker folds it
// in, and so does the runtime - it used to be dropped and reported as "nobody defines this"
// (docs/ABI.md H11).
asm(".globl abs_constant\n.set abs_constant, 0x1234\n");

extern "C" unsigned long long abs_slot;
asm(".globl abs_slot\n.section .data\n.align 8\nabs_slot:\n.quad abs_constant\n.previous\n");

extern "C" unsigned long long abs_probe() { return abs_slot; }
