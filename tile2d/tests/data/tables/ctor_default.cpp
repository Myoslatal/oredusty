// A namespace scope object whose constructor has no priority: the plain .init_array section, which a
// linker runs *after* every .init_array.NNNNN - and which the runtime used to run wherever the section
// happened to sit (docs/ABI.md H7).
extern "C" char ctor_order[4];
extern "C" int ctor_at;

namespace {

struct Default {
    Default() { ctor_order[ctor_at++] = 'D'; }
};

Default default_ctor;

} // namespace
