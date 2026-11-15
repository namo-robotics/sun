/** Resolves captured addresses using indexes prepared before program execution.
 */
#include "runtime/exception_sources.h"

#include <pthread.h>

/** Protects module lifetime during lookup without allocating bookkeeping. */
namespace {
pthread_mutex_t sourceMutex = PTHREAD_MUTEX_INITIALIZER;
sun::runtime::ExceptionSourceMap* sourceMaps = nullptr;
}  // namespace

void sun_eh_register_source_map(void* storage) noexcept {
  auto* map = static_cast<sun::runtime::ExceptionSourceMap*>(storage);
  pthread_mutex_lock(&sourceMutex);
  for (auto* existing = sourceMaps; existing; existing = existing->next) {
    if (existing == map) {
      pthread_mutex_unlock(&sourceMutex);
      return;
    }
  }
  map->next = sourceMaps;
  sourceMaps = map;
  pthread_mutex_unlock(&sourceMutex);
}

void sun_eh_unregister_source_map(void* storage) noexcept {
  pthread_mutex_lock(&sourceMutex);
  auto** link = &sourceMaps;
  while (*link && *link != storage) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = (*link)->next;
  }
  pthread_mutex_unlock(&sourceMutex);
}

bool sun::runtime::visitExceptionSource(std::uintptr_t address,
                                        ExceptionFrameVisitor visitor,
                                        void* context) noexcept {
  pthread_mutex_lock(&sourceMutex);
  bool found = false;
  for (auto* map = sourceMaps; map && !found; map = map->next) {
    const auto* bytes = reinterpret_cast<const char*>(map);
    const auto* functions =
        reinterpret_cast<const ExceptionSourceFunction*>(map + 1);
    for (std::uint32_t index = 0; index < map->functionCount; ++index) {
      const auto& function = functions[index];
      if (address < function.address ||
          address - function.address >= function.size) {
        continue;
      }
      const auto offset = address - function.address;
      const auto* rows =
          reinterpret_cast<const ExceptionSourceRow*>(bytes + function.rows);
      std::uint32_t first = 0, last = function.rowCount;
      while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (rows[middle].end <= offset) {
          first = middle + 1;
        } else {
          last = middle;
        }
      }
      if (first == function.rowCount || rows[first].begin > offset) {
        continue;
      }
      const auto& row = rows[first];
      const auto* frames =
          reinterpret_cast<const ExceptionSourceFrame*>(bytes + row.frames);
      for (std::uint32_t i = 0; i < row.frameCount; ++i) {
        const auto& frame = frames[i];
        if (visitor) {
          visitor({bytes + frame.name, bytes + frame.file,
                   bytes + frame.throwLabel, frame.line, frame.column},
                  context);
        }
      }
      found = row.frameCount != 0;
      break;
    }
  }
  pthread_mutex_unlock(&sourceMutex);
  return found;
}
