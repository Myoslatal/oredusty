// The small body of a function that two translation units define. There is no header to share, and that
// is the point of the fixture: the two bodies disagree. A compiler is allowed to do that with a vague
// linkage function - it inlines a different amount of it into each translation unit, and both bodies
// stand for the one symbol (GCC does exactly this with libstdc++'s std::__format sinks at -O3).
inline int twin_width(int value) { return value + 1; }

// Handing the address out is what makes the compiler emit the out-of-line copy: a copy that is only
// ever inlined is not in the object at all, and this fixture is about two of them being there.
using Twin = int (*)(int);
Twin twin_small_copy() { return &twin_width; }

int twin_small_answer() { return twin_width(41); }
