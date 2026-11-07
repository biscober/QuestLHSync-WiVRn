#pragma once

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/qlhs_shm.h"
#include "common/qlhs_status.h"
#include "driver/monado_adapter.h"
#include "driver/net.h"
#include "driver/openxr_locator.h"
#include "driver/relations.h"
#include "driver/sync.h"

namespace qlhs {

struct DaemonConfig {
  std::string config_dir;
  std::string shm_name = QLHS_DEFAULT_SHM_NAME;
  std::vector<std::string> hosts;
  std::string preferred_headset;
  std::string streamer = "WiVRn";
  std::string lighthouse_origin_name = "lighthouse";
  uint32_t default_origin_id = 0;
  bool enable_headset_link = true;
  bool enable_openxr = true;
  bool enable_monado = true;
  bool enable_lighthousedb_poll = true;
  bool steady_stations = true;
  std::string lighthousedb_path;
  bool per_headset = true;
  bool arrival_clock = false;
  bool learn_timing = true;
  bool verbose = false;
  bool headless = true;
  double publish_rate_hz = 50.0;
};

class DaemonCoordinator {
public:
  DaemonCoordinator(
      DaemonConfig config,
      std::shared_ptr<IOpenXrLocator> openxr_locator = nullptr,
      std::shared_ptr<IMonadoIpc> monado_ipc = nullptr);
  ~DaemonCoordinator();

  DaemonCoordinator(const DaemonCoordinator &) = delete;
  DaemonCoordinator &operator=(const DaemonCoordinator &) = delete;
  DaemonCoordinator(DaemonCoordinator &&) = delete;
  DaemonCoordinator &operator=(DaemonCoordinator &&) = delete;

  // Lifecycle
  bool Initialize();
  void Start();
  void Stop();
  void Run(const volatile sig_atomic_t *signal_flag = nullptr); // Blocks until Stop(), RequestStop(), or signal
  void RequestStop() noexcept { running_.store(false, std::memory_order_relaxed); }
  void SetSignalFlag(const volatile sig_atomic_t *flag) noexcept { signal_flag_ = flag; }

  // Deterministic single step (for tests or manual pacing)
  void Step(double now = -1.0);

  // Status accessors
  bool IsInitialized() const noexcept { return initialized_; }
  bool IsRunning() const noexcept { return running_; }
  bool IsRecording() const noexcept { return recording_.load(std::memory_order_relaxed); }
  bool IsHmdLocated() const noexcept { return hmd_located_; }
  bool IsMonadoConnected() const;
  uint32_t GetMonadoOriginId() const noexcept { return lighthouse_origin_id_; }

  Sync *GetSync() noexcept { return sync_.get(); }
  const Sync *GetSync() const noexcept { return sync_.get(); }
  Relations *GetRelations() noexcept { return relations_.get(); }
  const Relations *GetRelations() const noexcept { return relations_.get(); }
  ShmPublisher &GetShmPublisher() noexcept { return shm_pub_; }
  const DaemonConfig &GetConfig() const noexcept { return config_; }

  // External / test input feeds
  bool SetCalibration(const std::string &json, std::string *err = nullptr);
  void FeedHeadsetLine(const char *line, double pc_now = -1.0);
  void FeedHeadsetPing(double pc_send, double pc_recv, double hs);
  void SetHeadsetInfo(const std::string &serial, const std::string &model);

  // Command handlers
  void HandleCommand(QlhsCmd cmd);
  void SetRecording(bool on);
  void ReloadConfig();

  // Logging
  void Log(const std::string &msg);

private:
  void WorkerLoop();
  void PollOpenXr(double now);
  void PollLighthouseDb(double now);
  void PollConfigFile(double now);
  void StepSolver(double now);
  bool EnsureMonadoConnected(double now);
  void ApplyMonadoOffset(const Transform &xf);
  void PollCommands();
  void PublishTelemetry(double now);

  DaemonConfig config_;
  std::shared_ptr<IOpenXrLocator> openxr_;
  std::shared_ptr<IMonadoIpc> monado_;
  ShmPublisher shm_pub_;

  std::unique_ptr<Sync> sync_;
  std::unique_ptr<HeadsetLink> link_;
  std::unique_ptr<Relations> relations_;
  bool rel_done_ = true;
  double started_ = 0.0;
  std::map<std::pair<uint32_t, uint32_t>, std::array<float, 7>> last_raw_rel_;
  std::map<std::pair<uint32_t, uint32_t>, std::array<float, 7>> last_avg_rel_;

  std::atomic<bool> initialized_{false};
  std::atomic<bool> running_{false};
  std::thread worker_thread_;
  std::thread pose_thread_;
  std::atomic<bool> pose_thread_running_{false};
  void PoseLoop();

  std::atomic<bool> hmd_located_{false};
  std::string hmd_model_ = "Quest Pro";
  std::string hmd_system_ = "WiVRn";
  std::string headset_serial_;
  std::string headset_model_ = "Quest Pro";

  uint32_t lighthouse_origin_id_ = 0;
  bool origin_id_resolved_ = false;
  mutable std::mutex lh_offset_mutex_;
  Transform current_lh_offset_{};
  Transform current_stage_offset_{};
  double last_monado_connect_try_ = -1e9;
  double last_openxr_retry_ = -1e9;
  double last_lighthousedb_poll_ = -1e9;
  double last_lighthousedb_full_check_ = -1e9;
  std::filesystem::file_time_type last_lighthousedb_mtime_{};

  double last_step_ = -1e9;
  double last_publish_ = -1e9;
  double last_config_check_ = -1e9;
  std::filesystem::file_time_type config_file_mtime_{};
  std::atomic<double> last_pose_heartbeat_{-1e9};

  std::atomic<bool> recording_{false};
  Sync::Spots rec_spots_{};

  const volatile sig_atomic_t *signal_flag_ = nullptr;

  std::mutex log_mutex_;
  std::string log_history_[8];
  uint32_t log_counter_ = 0;
  FILE *log_file_ = nullptr;
};

// Resolves XDG config directory ($XDG_CONFIG_HOME/questlhsync or ~/.config/questlhsync)
std::string GetDefaultConfigDir();

// Loads config from ${config_dir}/config.json into cfg.
// Returns true if config was read and parsed successfully.
bool LoadConfigFile(const std::string &config_dir, DaemonConfig &cfg, std::string *err = nullptr);

// Saves host and preferred_headset to ${config_dir}/config.json.
// Returns true on success.
bool SaveConfigFile(const std::string &config_dir, const std::string &host, const std::string &preferred = "", std::string *err = nullptr);

enum class CliParseResult {
  Success,
  Help,
  Error
};

// Parses common CLI options (--config-dir, --shm, --host, --preferred, --streamer, --origin, --rate, --no-link, --verbose, --headless, --daemon, --help).
CliParseResult ParseDaemonConfig(
    int argc,
    char *argv[],
    DaemonConfig &cfg,
    bool *run_headless = nullptr,
    std::string *error_msg = nullptr);

} // namespace qlhs
