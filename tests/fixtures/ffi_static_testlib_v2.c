// tests/fixtures/ffi_static_testlib_v2.c
//
// A second "version" of ffi_static_testlib.c: the same two symbols, built
// into an archive of the same file name (tests/v2/libsun_ffi_static_testlib.a),
// with behaviour that tells the two apart. Two bundles each carrying one
// version must link into one program with each bound to its own copy.
// The atexit call stands in for what real libraries do (OpenSSL registers
// its cleanup this way). Under the JIT the driver has to provide that
// symbol itself and run the handler when the program's main returns: the
// real atexit would call into JIT memory long after it is gone.

#include <stdlib.h>

static int slot;

/** Clears the archive-local slot when the registered exit handler runs. */
static void sun_ffi_slot_cleanup(void) { slot = 0; }

/** Stores a value in the archive-local slot used to check symbol isolation. */
void sun_ffi_slot_set(int v) {
  slot = v;
  atexit(sun_ffi_slot_cleanup);
}

/** Returns the archive-local slot value used to distinguish library versions. */
int sun_ffi_slot_get(void) { return slot + 1000; }
