// What calls the function both copies define, and where the step they call back is defined. The
// declaration here is what makes the call go through the symbol rather than into one of the copies.
int twin_step(int step) { return step + 1; }

int twin_width(int value);

extern "C" int twin_answer() { return twin_width(41); }
