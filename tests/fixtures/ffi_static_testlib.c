// tests/fixtures/ffi_static_testlib.c
//
// Built as a static archive (libsun_ffi_static_testlib.a) for the tests that
// JIT-load a .a named with -l (issue #133). dlopen cannot open an archive,
// so these symbols only become reachable through the JIT's own linker.
// Symbols are distinct from the shared sun_ffi_testlib so nothing resident
// in the process can satisfy them by accident.

static int slot;

void sun_ffi_slot_set(int v) { slot = v; }

int sun_ffi_slot_get(void) { return slot; }
