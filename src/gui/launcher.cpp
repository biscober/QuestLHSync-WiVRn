#include "gui/launcher.h"

#include <cmath>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace qlhs {

bool CheckAndCleanStaleDaemon(std::string_view shm_name) {
  ShmReader reader(shm_name);
  if (!reader.Open()) {
    return false;
  }

  QlhsStatus st{};
  if (!reader.Read(st, 20)) {
    return false;
  }

  if (st.magic != QLHS_MAGIC || st.version != QLHS_VERSION) {
    return false;
  }

  if (st.daemon_pid <= 0) {
    return false;
  }

  // Check if PID is alive
  if (kill(st.daemon_pid, 0) != 0) {
    // Process is dead, clean up stale shm
    shm_unlink(std::string(shm_name).c_str());
    return true;
  }

  // Check if daemon process is running an unlinked / deleted binary
  std::string exe_path = "/proc/" + std::to_string(st.daemon_pid) + "/exe";
  char buf[1024];
  ssize_t len = readlink(exe_path.c_str(), buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    if (std::strstr(buf, " (deleted)") != nullptr) {
      // Outdated binary detected! Terminate stale daemon so the fresh binary runs.
      kill(st.daemon_pid, SIGTERM);
      for (int i = 0; i < 10; ++i) {
        if (kill(st.daemon_pid, 0) != 0) break;
        usleep(50000);
      }
      if (kill(st.daemon_pid, 0) == 0) {
        kill(st.daemon_pid, SIGKILL);
      }
      shm_unlink(std::string(shm_name).c_str());
      return true;
    }
  }

  return false;
}

bool IsDaemonActive(std::string_view shm_name, double max_staleness_sec) {
  if (CheckAndCleanStaleDaemon(shm_name)) {
    return false;
  }

  ShmReader reader(shm_name);
  if (!reader.Open()) {
    return false;
  }

  QlhsStatus st{};
  if (!reader.Read(st, 20)) {
    return false;
  }

  if (st.magic != QLHS_MAGIC || st.version != QLHS_VERSION) {
    return false;
  }

  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
  double diff = std::abs(now - st.updated);

  return (diff <= max_staleness_sec);
}

bool SpawnDaemon(const std::string &daemon_bin, const std::vector<std::string> &extra_args) {
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }

  if (pid == 0) {
    // Child 1: start a new session detached from controlling terminal
    if (setsid() < 0) {
      _exit(1);
    }

    // Double-fork: spawn grandchild (child 2)
    pid_t pid2 = fork();
    if (pid2 < 0) {
      _exit(1);
    }
    if (pid2 > 0) {
      // Child 1 exits immediately so grandchild is reparented to init / systemd
      _exit(0);
    }

    // Grandchild process (child 2): fully detached daemon
    // Redirect standard file descriptors to /dev/null
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > 2) {
        close(devnull);
      }
    }

    std::vector<char*> c_args;
    c_args.push_back(const_cast<char*>(daemon_bin.c_str()));
    for (const auto &arg : extra_args) {
      c_args.push_back(const_cast<char*>(arg.c_str()));
    }
    c_args.push_back(nullptr);

    execvp(daemon_bin.c_str(), c_args.data());
    _exit(127);
  }

  // Parent process: reap child 1 immediately to prevent zombie leak
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return false;
  }
  return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

std::string FindDaemonBinary(const char *argv0) {
  // 1. Check directory next to argv0
  if (argv0) {
    std::filesystem::path p(argv0);
    std::filesystem::path dir = p.parent_path();
    if (!dir.empty()) {
      std::filesystem::path candidate = dir / "questlhsyncd";
      if (std::filesystem::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
        return candidate.string();
      }
    }
  }

  // 2. Check /proc/self/exe directory
  char proc_path[4096];
  ssize_t len = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
  if (len > 0) {
    proc_path[len] = '\0';
    std::filesystem::path p(proc_path);
    std::filesystem::path candidate = p.parent_path() / "questlhsyncd";
    if (std::filesystem::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }

  // 3. Check current working directory / common build directories
  const char *local_dirs[] = {".", "./build", "../build"};
  for (const char *d : local_dirs) {
    std::filesystem::path candidate = std::filesystem::path(d) / "questlhsyncd";
    if (std::filesystem::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }

  // 4. Check system paths
  const char *sys_dirs[] = {"/usr/local/bin", "/usr/bin", "/opt/questlhsync/bin"};
  for (const char *d : sys_dirs) {
    std::filesystem::path candidate = std::filesystem::path(d) / "questlhsyncd";
    if (std::filesystem::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }

  // 5. Check PATH environment variable
  const char *env_path = std::getenv("PATH");
  if (env_path) {
    std::string path_str(env_path);
    size_t start = 0;
    while (start < path_str.size()) {
      size_t end = path_str.find(':', start);
      if (end == std::string::npos) end = path_str.size();
      std::string segment = path_str.substr(start, end - start);
      if (!segment.empty()) {
        std::filesystem::path candidate = std::filesystem::path(segment) / "questlhsyncd";
        if (std::filesystem::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
          return candidate.string();
        }
      }
      start = end + 1;
    }
  }

  // Fallback to name in PATH
  return "questlhsyncd";
}

} // namespace qlhs
