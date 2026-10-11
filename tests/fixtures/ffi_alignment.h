/** Native layouts used to validate generated and handwritten Sun bindings. */
#ifndef SUN_FFI_ALIGNMENT_H
#define SUN_FFI_ALIGNMENT_H

/** A record with ordinary interior padding. */
struct SunFfiReading {
  unsigned char status;
  int value;
};

/** A record whose aggregate alignment exceeds its fields' natural alignment. */
struct __attribute__((aligned(64))) SunFfiAligned {
  int value;
};

/** A nested aligned record with a nonzero field offset and padded array stride.
 */
struct SunFfiAlignedOuter {
  unsigned char prefix;
  struct SunFfiAligned items[2];
  unsigned char suffix;
};

/** Returns a record through the target C aggregate return convention. */
struct SunFfiAligned sun_ffi_aligned_make(int value);
/** Reads a record passed by value through the target C argument convention. */
int sun_ffi_aligned_take(struct SunFfiAligned value);
/** Checks the alignment of a pointer received from Sun. */
int sun_ffi_aligned_address(struct SunFfiAligned* value);
/** Returns a native layout measurement selected by index. */
long long sun_ffi_aligned_layout(int index);
#endif
