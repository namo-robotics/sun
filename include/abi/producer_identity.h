#pragma once

#include "abi/compatibility.h"

/** Provides diagnostic provenance independently of compatibility decisions. */
namespace sun::abi {
/** Describes the producer relative to the consumer without implying compatibility. */
enum class ProducerOrdering { SameBuild, Older, Newer, Unknown };

/** Returns the generated identity and optional official release ordering. */
ProducerIdentity currentProducerIdentity();
/** Compares official releases within one series; incomplete provenance is unknown. */
ProducerOrdering compareProducerIdentity(const ProducerIdentity& producer,
                                        const ProducerIdentity& consumer);
}  // namespace sun::abi
