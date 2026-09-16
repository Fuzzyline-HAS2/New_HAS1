#pragma once

#include <stdint.h>
#include <functional>

namespace iotglove {
namespace ota {

enum class CheckResult : uint8_t { Skipped, Failed };

// Worker-only blocking I/O. A successful install commits the verified image,
// invokes onSuccess and restarts. Skipped is returned only after authenticating
// metadata for the requested board/version/current partition. Failure never
// falls back to a fixed release or a different version.
CheckResult checkPinned(const char* board, uint32_t targetVersion,
                        uint32_t currentVersion, uint32_t currentPartition,
                        const char* secret, std::function<void()> onSuccess = {});

}  // namespace ota
}  // namespace iotglove
