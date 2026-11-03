// The user clock offset the game saves in its system clock context (Player.sav). It stays constant from one run to the next so the game
// does not report "your system time has changed"; the guest counter carries the difference to the host clock instead (CntpctBaseTicks).
#pragma once

#include <cstdint>

namespace nxrt {
inline constexpr std::int64_t kUserClockOffset = 1793199229;
}  // namespace nxrt
