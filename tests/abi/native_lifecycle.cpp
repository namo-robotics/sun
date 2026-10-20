/** Exercises native initialization and thread-local storage under the system
 * linker. */

/** Stores the value installed by the native initializer. */
int sun_abi_initialized_value;
/** Gives each thread its own trivially initialized native value. */
thread_local int sun_abi_thread_value = 2;

/** Initializes native state before the executable enters main. */
__attribute__((constructor)) void sun_abi_initialize_value() {
  sun_abi_initialized_value = 40;
}

/** Reads initialized process and thread state to verify native execution. */
int sun_abi_read_lifecycle_value() {
  return sun_abi_initialized_value + sun_abi_thread_value;
}
