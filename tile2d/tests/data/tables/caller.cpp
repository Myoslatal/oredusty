// The second translation unit of the same "game": it calls what base.cpp defines, and it calls the C
// library, which no table defines - so the running program has to answer for that one.
extern "C" int base_value();
extern "C" unsigned long strlen(const char* text);

extern "C" int use_base() { return base_value() + 1; }

extern "C" unsigned long length_of(const char* text) { return strlen(text); }
