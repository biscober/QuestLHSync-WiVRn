#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "common/qlhs_shm.h"

namespace qlhs {

// Checks whether the daemon is actively running and updating the shared memory segment.
bool IsDaemonActive(std::string_view shm_name = QLHS_DEFAULT_SHM_NAME, double max_staleness_sec = 3.0);

// Checks if an active daemon is running a stale/deleted binary or dead PID, and cleans it up.
bool CheckAndCleanStaleDaemon(std::string_view shm_name = QLHS_DEFAULT_SHM_NAME);

// Spawns questlhsyncd in the background as a detached process (survives GUI close).
bool SpawnDaemon(const std::string &daemon_bin, const std::vector<std::string> &extra_args = {});

// Locates the questlhsyncd binary (checking near argv[0], in standard paths, and PATH).
std::string FindDaemonBinary(const char *argv0 = nullptr);

} // namespace qlhs
