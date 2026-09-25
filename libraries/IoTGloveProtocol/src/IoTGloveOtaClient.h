#pragma once

#include <stdint.h>
#include <functional>

namespace iotglove {
namespace ota {

enum class CheckResult : uint8_t { Skipped, Failed };

// Authenticate the fixed channel's signed manifest and return its target.
// Callers must then verify/install the immutable version archive with
// checkPinned; a replayed older channel target can be rejected by policy.
bool fetchLatestTarget(const char* board, uint32_t currentPartition,
                       const char* secret, uint32_t& targetVersion);

// Worker-only blocking I/O. A successful install commits the verified image,
// invokes onSuccess and restarts. Skipped is returned only after authenticating
// metadata for the requested board/version/current partition. Failure never
// falls back to a fixed release or a different version.
CheckResult checkPinned(const char* board, uint32_t targetVersion,
                        uint32_t currentVersion, uint32_t currentPartition,
                        const char* secret, std::function<void()> onSuccess = {});

}  // namespace ota
}  // namespace iotglove
