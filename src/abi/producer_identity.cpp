#include "abi/producer_identity.h"

#include "generated/sun_version.h"

/** Provides diagnostic provenance independently of compatibility decisions. */
namespace sun::abi {
ProducerIdentity currentProducerIdentity() {
  ProducerIdentity identity;
  if (!std::string(SUN_VERSION_NAME).empty()) {
    identity.set_version_name(SUN_VERSION_NAME);
  }
  identity.set_build(std::string(SUN_GIT_HASH) + ":" +
                     compatibilityImplementation());
  identity.set_release_order(SUN_RELEASE_ORDER);
  identity.set_release_series(SUN_RELEASE_SERIES);
  identity.set_runtime_fingerprint(runtimeFingerprint());
  return identity;
}

ProducerOrdering compareProducerIdentity(const ProducerIdentity& producer,
                                         const ProducerIdentity& consumer) {
  if (!producer.build().empty() && producer.build() == consumer.build()) {
    return ProducerOrdering::SameBuild;
  }
  if (producer.build().empty() || consumer.build().empty() ||
      producer.release_order() == 0 || consumer.release_order() == 0 ||
      producer.release_series().empty() ||
      producer.release_series() != consumer.release_series()) {
    return ProducerOrdering::Unknown;
  }
  if (producer.release_order() < consumer.release_order()) {
    return ProducerOrdering::Older;
  }
  if (producer.release_order() > consumer.release_order()) {
    return ProducerOrdering::Newer;
  }
  return ProducerOrdering::Unknown;
}
}  // namespace sun::abi
