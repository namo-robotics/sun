/** Provides position-independent native code with a global offset table
 * reference. */

/** Defined in a separate object so reads use the GOT on ELF and Mach-O. */
extern int sun_abi_relocation_value;

/** Reads a global through a relocation handled by the native linker. */
int sun_abi_read_relocation_value() { return sun_abi_relocation_value; }
