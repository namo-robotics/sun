/** Selects Sun handlers and cleanup landing pads from native unwind tables. */
#include <cstring>

#include "runtime/exceptions.h"

/** Names the fixed headers used by generated exception storage. */
namespace {
/** Refers to compiler-emitted nominal type metadata. */
using sun::runtime::ExceptionTypeDescriptor;
/** Refers to the native unwind header and its owned exception value. */
using sun::runtime::PropagationRecordHeader;

/** Reads a potentially unaligned fixed-width field from trusted unwind
 * metadata. */
template <typename T>
T readUnaligned(const unsigned char*& cursor) noexcept {
  T result;
  std::memcpy(&result, cursor, sizeof(T));
  cursor += sizeof(T);
  return result;
}

/** Decodes an unsigned LEB128 field, rejecting integer overflow. */
std::uintptr_t readUnsignedLEB128(const unsigned char*& cursor) noexcept {
  std::uintptr_t result = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    const auto byte = *cursor++;
    if (shift == 63 && (byte & 0x7e)) {
      sun_eh_terminate();
    }
    result |= std::uintptr_t(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      return result;
    }
  }
  sun_eh_terminate();
}

/** Decodes a signed LEB128 action field without signed-shift overflow. */
std::intptr_t readSignedLEB128(const unsigned char*& cursor) noexcept {
  std::uintptr_t result = 0;
  unsigned shift = 0;
  unsigned char byte;
  do {
    if (shift >= 64) {
      sun_eh_terminate();
    }
    byte = *cursor++;
    if (shift == 63 && (byte & 0x7f) != 0 && (byte & 0x7f) != 0x7f) {
      sun_eh_terminate();
    }
    result |= std::uintptr_t(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if (shift < 64 && (byte & 0x40)) {
    result |= ~std::uintptr_t(0) << shift;
  }
  return static_cast<std::intptr_t>(result);
}

/** Returns the fixed width required for reverse-indexed exception type entries.
 */
std::size_t encodedPointerSize(unsigned encoding) noexcept {
  switch (encoding & 0x0f) {
    case 0:
      return sizeof(void*);
    case 2:
    case 0xa:
      return 2;
    case 3:
    case 0xb:
      return 4;
    case 4:
    case 0xc:
      return 8;
    default:
      sun_eh_terminate();
  }
}

/** Decodes the pointer encodings emitted by LLVM on the supported native
 * targets. */
std::uintptr_t readEncodedPointer(const unsigned char*& cursor,
                                  unsigned encoding,
                                  _Unwind_Context* context) noexcept {
  if (encoding == 0xff) {
    return 0;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(cursor);
  std::uintptr_t result;
  switch (encoding & 0x0f) {
    case 0:
      result = readUnaligned<std::uintptr_t>(cursor);
      break;
    case 1:
      result = readUnsignedLEB128(cursor);
      break;
    case 2:
      result = readUnaligned<std::uint16_t>(cursor);
      break;
    case 3:
      result = readUnaligned<std::uint32_t>(cursor);
      break;
    case 4:
      result = readUnaligned<std::uint64_t>(cursor);
      break;
    case 9:
      result = readSignedLEB128(cursor);
      break;
    case 0xa:
      result = readUnaligned<std::int16_t>(cursor);
      break;
    case 0xb:
      result = readUnaligned<std::int32_t>(cursor);
      break;
    case 0xc:
      result = readUnaligned<std::int64_t>(cursor);
      break;
    default:
      sun_eh_terminate();
  }
  if (!result) {
    return 0;
  }
  switch (encoding & 0x70) {
    case 0:
      break;
    case 0x10:
      result += address;
      break;
    case 0x30:
#if defined(__APPLE__)
      // Darwin does not provide a data-relative base through the unwind API.
      sun_eh_terminate();
#else
      result += _Unwind_GetDataRelBase(context);
      break;
#endif
    case 0x40:
      result += _Unwind_GetRegionStart(context);
      break;
    default:
      sun_eh_terminate();
  }
  if (encoding & 0x80) {
    auto* pointer = reinterpret_cast<const unsigned char*>(result);
    result = readUnaligned<std::uintptr_t>(pointer);
  }
  return result;
}
}  // namespace

_Unwind_Reason_Code sun_eh_personality(int version, _Unwind_Action actions,
                                       std::uint64_t exceptionClass,
                                       _Unwind_Exception* exception,
                                       _Unwind_Context* context) {
  if (version != 1) {
    return _URC_FATAL_PHASE1_ERROR;
  }
  if (exceptionClass != sun::runtime::exceptionClass ||
      (actions & _UA_FORCE_UNWIND)) {
    sun_eh_terminate();
  }
  // Darwin exposes the LSDA address as uintptr_t; other unwinders use void*.
  auto* cursor = reinterpret_cast<const unsigned char*>(
      _Unwind_GetLanguageSpecificData(context));
  if (!cursor) {
    return _URC_CONTINUE_UNWIND;
  }
  const auto region = _Unwind_GetRegionStart(context);
  const auto landingEncoding = *cursor++;
  const auto landingBase =
      landingEncoding == 0xff
          ? region
          : readEncodedPointer(cursor, landingEncoding, context);
  const auto typeEncoding = *cursor++;
  const unsigned char* typeTable = nullptr;
  if (typeEncoding != 0xff) {
    const auto offset = readUnsignedLEB128(cursor);
    typeTable = cursor + offset;
  }
  const auto callEncoding = *cursor++;
  const auto callLength = readUnsignedLEB128(cursor);
  const auto* actionTable = cursor + callLength;
  int beforeInstruction = 0;
  auto ip = _Unwind_GetIPInfo(context, &beforeInstruction);
  if (!beforeInstruction) {
    --ip;
  }
  const auto position = ip - region;
  while (cursor < actionTable) {
    const auto start = readEncodedPointer(cursor, callEncoding, context);
    const auto length = readEncodedPointer(cursor, callEncoding, context);
    const auto landing = readEncodedPointer(cursor, callEncoding, context);
    const auto action = readUnsignedLEB128(cursor);
    if (position < start || position - start >= length) {
      continue;
    }
    if (!landing) {
      return _URC_CONTINUE_UNWIND;
    }
    bool cleanup = !action;
    std::intptr_t selector = 0;
    if (action) {
      auto* entry = actionTable + action - 1;
      while (true) {
        const auto filter = readSignedLEB128(entry);
        const auto* nextBase = entry;
        const auto next = readSignedLEB128(entry);
        if (!filter) {
          cleanup = true;
        } else if (filter > 0) {
          if (!typeTable) {
            sun_eh_terminate();
          }
          auto* typeEntry =
              typeTable - filter * encodedPointerSize(typeEncoding);
          auto* handler = reinterpret_cast<const ExceptionTypeDescriptor*>(
              readEncodedPointer(typeEntry, typeEncoding, context));
          if (!selector &&
              sun_eh_matches(
                  reinterpret_cast<PropagationRecordHeader*>(exception)
                      ->value.type,
                  handler)) {
            selector = filter;
          }
        } else {
          sun_eh_terminate();
        }
        if (!next) {
          break;
        }
        entry = nextBase + next;
      }
    }
    if (actions & _UA_SEARCH_PHASE) {
      return selector ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
    }
    if (!(actions & _UA_HANDLER_FRAME)) {
      selector = 0;
    }
    if (!selector && !cleanup) {
      return _URC_CONTINUE_UNWIND;
    }
    _Unwind_SetGR(context, __builtin_eh_return_data_regno(0),
                  reinterpret_cast<std::uintptr_t>(exception));
    _Unwind_SetGR(context, __builtin_eh_return_data_regno(1), selector);
    _Unwind_SetIP(context, landingBase + landing);
    return _URC_INSTALL_CONTEXT;
  }
  return _URC_CONTINUE_UNWIND;
}
