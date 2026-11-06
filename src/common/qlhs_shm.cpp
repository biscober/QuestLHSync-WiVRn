#include "qlhs_shm.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <utility>

namespace qlhs {

static inline void CpuPause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

std::string NormalizeShmName(std::string_view name) {
  std::string s(name);
  if (s.rfind("/dev/shm", 0) == 0) {
    s = s.substr(8);
  }
  if (s.empty()) {
    return QLHS_DEFAULT_SHM_NAME;
  }
  size_t first_non_slash = s.find_first_not_of('/');
  if (first_non_slash == std::string::npos) {
    return QLHS_DEFAULT_SHM_NAME;
  }
  s = "/" + s.substr(first_non_slash);
  for (size_t i = 1; i < s.size(); ++i) {
    if (s[i] == '/') {
      s[i] = '_';
    }
  }
  return s;
}

// ============================================================================
// ShmPublisher
// ============================================================================

ShmPublisher::ShmPublisher(std::string_view name)
    : name_(NormalizeShmName(name)) {}

ShmPublisher::~ShmPublisher() {
  Close();
  Unlink();
}

ShmPublisher::ShmPublisher(ShmPublisher&& other) noexcept
    : name_(std::move(other.name_)),
      fd_(other.fd_),
      st_(other.st_),
      last_cmd_seq_(other.last_cmd_seq_),
      owns_unlink_(other.owns_unlink_) {
  other.fd_ = -1;
  other.st_ = nullptr;
  other.owns_unlink_ = false;
}

ShmPublisher& ShmPublisher::operator=(ShmPublisher&& other) noexcept {
  if (this != &other) {
    Close();
    Unlink();
    name_ = std::move(other.name_);
    fd_ = other.fd_;
    st_ = other.st_;
    last_cmd_seq_ = other.last_cmd_seq_;
    owns_unlink_ = other.owns_unlink_;
    other.fd_ = -1;
    other.st_ = nullptr;
    other.owns_unlink_ = false;
  }
  return *this;
}

bool ShmPublisher::Init(bool create_exclusive) {
  Close();

  int flags = O_RDWR | O_CREAT;
  if (create_exclusive) {
    flags |= O_EXCL;
  }

  // 0660: Read/write for owner and group
  fd_ = shm_open(name_.c_str(), flags, 0660);
  if (fd_ < 0) {
    return false;
  }

  if (ftruncate(fd_, sizeof(QlhsStatus)) != 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  void *ptr = mmap(nullptr, sizeof(QlhsStatus), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (ptr == MAP_FAILED) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  st_ = static_cast<QlhsStatus*>(ptr);
  std::memset(st_, 0, sizeof(QlhsStatus));
  st_->magic = QLHS_MAGIC;
  st_->version = QLHS_VERSION;
  st_->seq = 0;
  st_->cmd_seq = 0;
  last_cmd_seq_ = 0;
  owns_unlink_ = true;
  return true;
}

void ShmPublisher::Close() {
  if (st_) {
    munmap(st_, sizeof(QlhsStatus));
    st_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void ShmPublisher::Unlink() {
  if (owns_unlink_ && !name_.empty()) {
    shm_unlink(name_.c_str());
    owns_unlink_ = false;
  }
}

void ShmPublisher::Publish(const QlhsStatus &status) {
  if (!st_) return;

  int32_t s = __atomic_load_n(&st_->seq, __ATOMIC_RELAXED);
  // Increment seq to odd (write in progress)
  __atomic_store_n(&st_->seq, static_cast<int32_t>(static_cast<uint32_t>(s) + 1), __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_RELEASE);

  st_->magic = status.magic ? status.magic : QLHS_MAGIC;
  st_->version = status.version ? status.version : QLHS_VERSION;

  // Copy status telemetry fields up to cmd_seq, protecting the cmd channel from overwrite
  constexpr size_t payload_offset = offsetof(QlhsStatus, state);
  constexpr size_t payload_size = offsetof(QlhsStatus, cmd_seq) - payload_offset;
  std::memcpy(reinterpret_cast<char*>(st_) + payload_offset,
              reinterpret_cast<const char*>(&status) + payload_offset,
              payload_size);

  __atomic_thread_fence(__ATOMIC_RELEASE);
  // Increment seq to even (write complete)
  __atomic_store_n(&st_->seq, static_cast<int32_t>(static_cast<uint32_t>(s) + 2), __ATOMIC_RELEASE);
}

bool ShmPublisher::PollCommand(QlhsCmd &out_cmd) {
  if (!st_) return false;
  int32_t s = __atomic_load_n(&st_->cmd_seq, __ATOMIC_ACQUIRE);
  if (s == last_cmd_seq_) return false;

  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  out_cmd = static_cast<QlhsCmd>(st_->cmd);
  last_cmd_seq_ = s;
  return true;
}

std::optional<QlhsCmd> ShmPublisher::PollCommand() {
  QlhsCmd cmd = QLHS_CMD_NONE;
  if (PollCommand(cmd)) {
    return cmd;
  }
  return std::nullopt;
}

// ============================================================================
// ShmReader
// ============================================================================

ShmReader::ShmReader(std::string_view name)
    : name_(NormalizeShmName(name)) {}

ShmReader::~ShmReader() {
  Close();
}

ShmReader::ShmReader(ShmReader&& other) noexcept
    : name_(std::move(other.name_)),
      fd_(other.fd_),
      st_(other.st_) {
  other.fd_ = -1;
  other.st_ = nullptr;
}

ShmReader& ShmReader::operator=(ShmReader&& other) noexcept {
  if (this != &other) {
    Close();
    name_ = std::move(other.name_);
    fd_ = other.fd_;
    st_ = other.st_;
    other.fd_ = -1;
    other.st_ = nullptr;
  }
  return *this;
}

bool ShmReader::Open() {
  Close();

  fd_ = shm_open(name_.c_str(), O_RDWR, 0);
  if (fd_ < 0) {
    return false;
  }

  void *ptr = mmap(nullptr, sizeof(QlhsStatus), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (ptr == MAP_FAILED) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  st_ = static_cast<QlhsStatus*>(ptr);
  return true;
}

void ShmReader::Close() {
  if (st_) {
    munmap(st_, sizeof(QlhsStatus));
    st_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

bool ShmReader::Read(QlhsStatus &out, int max_retries) const {
  if (!st_) return false;

  for (int retry = 0; retry < max_retries; ++retry) {
    int32_t s1 = __atomic_load_n(&st_->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) {
      if ((retry & 63) == 63) {
        std::this_thread::yield();
      } else {
        CpuPause();
      }
      continue;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    std::memcpy(&out, const_cast<const QlhsStatus*>(st_), sizeof(QlhsStatus));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    int32_t s2 = __atomic_load_n(&st_->seq, __ATOMIC_ACQUIRE);
    if (s1 == s2) {
      if (out.magic == QLHS_MAGIC && out.version == QLHS_VERSION) {
        return true;
      }
    }
    CpuPause();
  }
  return false;
}

bool ShmReader::SendCommand(QlhsCmd cmd) {
  if (!st_) return false;

  st_->cmd = static_cast<int32_t>(cmd);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_add_fetch(&st_->cmd_seq, 1, __ATOMIC_RELEASE);
  return true;
}

bool ShmReader::IsServerRunning(std::string_view name) {
  ShmReader reader(name);
  if (!reader.Open()) {
    return false;
  }
  QlhsStatus st;
  return reader.Read(st, 10);
}

} // namespace qlhs
