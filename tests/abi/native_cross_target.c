/** Provides SDK-free Mach-O fixtures that are inspected, never executed. */

/** Exercises native thread-local section detection. */
_Thread_local int sun_abi_cross_value;

/** Exercises native constructor section detection. */
__attribute__((constructor)) static void initialize(void) {
  sun_abi_cross_value = 42;
}

/** Exports a definition whose namespace must be isolated by the packager. */
int sun_abi_cross_read(void) { return sun_abi_cross_value; }
