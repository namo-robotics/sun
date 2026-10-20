/** Provides position-independent native code with a global offset table
 * reference. */

/** Remains externally visible so position-independent reads use the GOT. */
int sun_abi_relocation_value = 42;

/** Reads a global through a relocation handled by the native linker. */
int sun_abi_read_relocation_value() { return sun_abi_relocation_value; }
