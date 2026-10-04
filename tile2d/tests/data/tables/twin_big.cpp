// The other body of the same function: bigger, and with calls at the end of it, so the copy this
// translation unit emits carries relocations past the end of the small copy above.
int twin_step(int step);

inline int twin_width(int value) {
    int total = value * 3;
    for (int step = 0; step < 16; ++step) total += twin_step(step);
    return twin_step(total);
}

using Twin = int (*)(int);
Twin twin_big_copy() { return &twin_width; }

int twin_big_answer() { return twin_width(41); }
