#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <type_traits>
#include "qlhs_status.h"

#ifndef QLHS_POSIX_SHM_NAME
#define QLHS_POSIX_SHM_NAME "/questlhsync"
#endif

#define QLHS_DEFAULT_SHM_NAME QLHS_POSIX_SHM_NAME

namespace qlhs {

// Normalizes shm name: ensures leading slash, strips "/dev/shm" if present, and removes interior slashes.
std::string NormalizeShmName(std::string_view name);

// Publisher (Server/Daemon role):
// Creates and owns the shared memory segment, publishing QlhsStatus telemetry under seqlock
// and polling commands sent by clients.
class ShmPublisher {
public:
  explicit ShmPublisher(std::string_view name = QLHS_DEFAULT_SHM_NAME);
  ~ShmPublisher();

  ShmPublisher(const ShmPublisher&) = delete;
  ShmPublisher& operator=(const ShmPublisher&) = delete;
  ShmPublisher(ShmPublisher&& other) noexcept;
  ShmPublisher& operator=(ShmPublisher&& other) noexcept;

  // Initializes shared memory (shm_open + ftruncate + mmap).
  bool Init(bool create_exclusive = false);

  // Status checks
  bool IsValid() const noexcept { return st_ != nullptr; }
  bool IsOpen() const noexcept { return st_ != nullptr; }

  // Cleanly unmap, close fd, and unlink from system.
  void Close();
  void Unlink();

  // Publish updated status to shared memory.
  // Updates seqlock (seq odd while writing, even when finished).
  // Safely preserves cmd and cmd_seq channels without overwriting client inputs.
  void Publish(const QlhsStatus &status);

  // In-place atomic update helper: executes fn(QlhsStatus &st) while holding the seqlock.
  template <typename F>
  void Update(F &&fn) {
    if (!st_) return;
    int32_t s = __atomic_load_n(&st_->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&st_->seq, static_cast<int32_t>(static_cast<uint32_t>(s) + 1), __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    fn(*st_);

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&st_->seq, static_cast<int32_t>(static_cast<uint32_t>(s) + 2), __ATOMIC_RELEASE);
  }

  // Poll for commands sent by client (e.g. QLHS_CMD_PAUSE, QLHS_CMD_RESUME).
  bool PollCommand(QlhsCmd &out_cmd);
  std::optional<QlhsCmd> PollCommand();

  // Raw pointer access (advanced use only)
  QlhsStatus* Raw() noexcept { return st_; }
  const QlhsStatus* Raw() const noexcept { return st_; }

  const std::string& Name() const noexcept { return name_; }

private:
  std::string name_;
  int fd_ = -1;
  QlhsStatus *st_ = nullptr;
  int32_t last_cmd_seq_ = 0;
  bool owns_unlink_ = true;
};

// Reader (Client/Dashboard role):
// Attaches to existing shared memory, reads QlhsStatus with seqlock verification
// (retry loop protecting against torn reads/writes), and dispatches commands to the publisher.
class ShmReader {
public:
  explicit ShmReader(std::string_view name = QLHS_DEFAULT_SHM_NAME);
  ~ShmReader();

  ShmReader(const ShmReader&) = delete;
  ShmReader& operator=(const ShmReader&) = delete;
  ShmReader(ShmReader&& other) noexcept;
  ShmReader& operator=(ShmReader&& other) noexcept;

  // Attaches to existing shared memory (shm_open + mmap).
  bool Open();

  // Connection check
  bool IsConnected() const noexcept { return st_ != nullptr; }

  // Cleanly unmap and close fd (does NOT unlink).
  void Close();

  // Reads status with seqlock validation.
  // Returns true if a consistent, non-torn read with matching magic and version succeeded.
  bool Read(QlhsStatus &out, int max_retries = 1000) const;

  // Sends a command to the publisher (e.g. QLHS_CMD_PAUSE).
  bool SendCommand(QlhsCmd cmd);

  // Check if server is running
  static bool IsServerRunning(std::string_view name = QLHS_DEFAULT_SHM_NAME);

  // Raw pointer access
  QlhsStatus* Raw() noexcept { return st_; }
  const QlhsStatus* Raw() const noexcept { return st_; }

  const std::string& Name() const noexcept { return name_; }

private:
  std::string name_;
  int fd_ = -1;
  QlhsStatus *st_ = nullptr;
};

} // namespace qlhs

// Aliases for convenience matching Qlhs naming conventions
using QlhsShmPublisher = qlhs::ShmPublisher;
using QlhsShmReader = qlhs::ShmReader;
using QlhsShmServer = qlhs::ShmPublisher;
using QlhsShmClient = qlhs::ShmReader;
