#pragma once

#include <google/protobuf/message.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "support/source_file.h"

namespace sun::serialization {

/** Remap source identities throughout a serialized tree. */
template <typename Remap>
void remapSourceFiles(google::protobuf::Message& message, Remap&& remap) {
  const auto* reflection = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->name() == "source_file_id" &&
        field->cpp_type() ==
            google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
      auto id = reflection->GetUInt64(message, field);
      if (id) reflection->SetUInt64(&message, field, remap(id));
    } else if (field->cpp_type() ==
               google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (field->is_repeated()) {
        for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
          remapSourceFiles(
              *reflection->MutableRepeatedMessage(&message, field, i), remap);
        }
      } else {
        remapSourceFiles(*reflection->MutableMessage(&message, field), remap);
      }
    }
  }
}

/** Give a bundle-local file a stable, distinct identity in this process. */
inline SourceFileId loadedSourceFileId(const std::string& bundle,
                                       SourceFileId local) {
  static std::mutex mutex;
  static std::map<std::pair<std::string, SourceFileId>, SourceFileId> files;
  std::lock_guard<std::mutex> lock(mutex);
  auto [it, inserted] = files.try_emplace({bundle, local}, 0);
  if (inserted) it->second = nextSourceFileId();
  return it->second;
}

}  // namespace sun::serialization
