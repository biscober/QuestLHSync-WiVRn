#include "daemon/daemon_coordinator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "driver/json.h"
#include "driver/mathx.h"

namespace qlhs {

namespace {

void SafeCopy(char *dst, size_t dst_size, const std::string &src) {
  if (dst_size == 0) return;
  std::strncpy(dst, src.c_str(), dst_size - 1);
  dst[dst_size - 1] = '\0';
}

bool IsTrackingReference(const std::string &name, const std::string &serial) {
  std::string lower_name = name;
  std::string lower_serial = serial;
  for (char &c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char &c : lower_serial) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (lower_name.find("basestation") != std::string::npos ||
          lower_name.find("base station") != std::string::npos ||
          lower_name.find("trackingreference") != std::string::npos ||
          lower_name.find("tracking reference") != std::string::npos ||
          lower_serial.find("lhb-") != std::string::npos ||
          lower_serial.find("basestation") != std::string::npos);
}

bool IsLighthousePeripheral(const std::string &name, const std::string &serial) {
  std::string lower_name = name;
  std::string lower_serial = serial;
  for (char &c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char &c : lower_serial) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower_name.find("wivrn") != std::string::npos ||
      lower_serial.find("wivrn") != std::string::npos ||
      lower_name.find("quest") != std::string::npos ||
      lower_serial.find("quest") != std::string::npos ||
      lower_name.find("eye tracker") != std::string::npos ||
      lower_name.find("face tracker") != std::string::npos ||
      lower_name.find("hand interaction") != std::string::npos ||
      lower_name.find("gamepad") != std::string::npos) {
    return false;
  }
  return true;
}

} // namespace

std::string GetDefaultConfigDir() {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return std::string(xdg) + "/questlhsync";
  }
  const char *home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.config/questlhsync";
  }
  return "/tmp/questlhsync";
}

DaemonCoordinator::DaemonCoordinator(
    DaemonConfig config,
    std::shared_ptr<IOpenXrLocator> openxr_locator,
    std::shared_ptr<IMonadoIpc> monado_ipc)
    : config_(std::move(config)),
      openxr_(std::move(openxr_locator)),
      monado_(std::move(monado_ipc)),
      shm_pub_(config_.shm_name) {
  if (!openxr_ && config_.enable_openxr) {
    openxr_ = std::make_shared<OpenXrLocator>();
  }
  if (!monado_ && config_.enable_monado) {
    monado_ = std::make_shared<MonadoIpcClient>();
  }
}

DaemonCoordinator::~DaemonCoordinator() {
  Stop();
}

bool DaemonCoordinator::Initialize() {
  if (initialized_) return true;

  if (config_.config_dir.empty()) {
    config_.config_dir = GetDefaultConfigDir();
  }

  std::error_code ec;
  std::filesystem::create_directories(config_.config_dir, ec);

  // Load config.json if hosts or preferred headset not supplied via CLI
  if (config_.hosts.empty() || config_.preferred_headset.empty()) {
    DaemonConfig disk_cfg;
    if (LoadConfigFile(config_.config_dir, disk_cfg)) {
      if (config_.hosts.empty()) config_.hosts = disk_cfg.hosts;
      if (config_.preferred_headset.empty()) config_.preferred_headset = disk_cfg.preferred_headset;
    }
  }

  std::filesystem::path cfg_file = std::filesystem::path(config_.config_dir) / "config.json";
  std::error_code ec_mtime;
  auto mtime = std::filesystem::last_write_time(cfg_file, ec_mtime);
  if (!ec_mtime) {
    config_file_mtime_ = mtime;
  }

  std::string log_path = (std::filesystem::path(config_.config_dir) / "questlhsync.log").string();
  log_file_ = std::fopen(log_path.c_str(), "a");

  Log("QuestLHSync daemon initializing, config dir: " + config_.config_dir);

  // Initialize Sync core
  SyncConfig sc;
  sc.dir = config_.config_dir;
  sc.per_headset = config_.per_headset;
  sc.arrival_clock = config_.arrival_clock;
  sc.learn_timing = config_.learn_timing;
  sync_ = std::make_unique<Sync>(sc, [this](const std::string &s) { Log(s); });
  sync_->SetStreamer(config_.streamer);

  relations_ = std::make_unique<Relations>(config_.config_dir, [this](const std::string &s) { Log(s); });
  relations_->SetEnabled(config_.steady_stations);
  started_ = QpcNow();
  rel_done_ = !config_.steady_stations || relations_->Hook();

  // Initialize HeadsetLink if enabled
  if (config_.enable_headset_link) {
    link_ = std::make_unique<HeadsetLink>(
        sync_.get(),
        [this](const std::string &s) { Log(s); },
        [this](double t, const std::string &s) {
          if (sync_) sync_->Rec(t, "%s", s.c_str());
        });
    link_->SetMemory((std::filesystem::path(config_.config_dir) / "headset.txt").string());
    if (!config_.hosts.empty()) {
      link_->SetHosts(config_.hosts);
    }
    if (!config_.preferred_headset.empty()) {
      link_->SetPreferred(config_.preferred_headset);
    }
    link_->SetStreamer(config_.streamer);
    link_->Start();
  }

  // Initialize POSIX shared memory publisher
  if (!shm_pub_.Init(false)) {
    Log("Error: Failed to initialize POSIX shared memory publisher on " + config_.shm_name);
    return false;
  }

  // Connect OpenXR if available
  if (openxr_) {
    if (openxr_->Initialize()) {
      Log("OpenXR headless session initialized");
    } else {
      Log("OpenXR initialization deferred (runtime may not be running yet)");
    }
  }

  // Connect Monado IPC if available
  if (monado_) {
    if (monado_->Connect()) {
      Log("Monado IPC connected");
      if (monado_->FindTrackingOriginByName(config_.lighthouse_origin_name, lighthouse_origin_id_) == MND_SUCCESS) {
        origin_id_resolved_ = true;
        Log("Found tracking origin '" + config_.lighthouse_origin_name + "' with id " + std::to_string(lighthouse_origin_id_));
      } else {
        lighthouse_origin_id_ = config_.default_origin_id;
        origin_id_resolved_ = true;
        Log("Defaulting tracking origin id to " + std::to_string(lighthouse_origin_id_));
      }
      MndPose cur_pose{};
      if (monado_->GetTrackingOriginOffset(lighthouse_origin_id_, cur_pose) == MND_SUCCESS) {
        std::lock_guard<std::mutex> g(lh_offset_mutex_);
        current_lh_offset_ = ToTransform(cur_pose);
      }
      MndPose stage_pose{};
      if (monado_->GetReferenceSpaceOffset(MND_REFERENCE_SPACE_TYPE_STAGE, stage_pose) == MND_SUCCESS) {
        std::lock_guard<std::mutex> g(lh_offset_mutex_);
        current_stage_offset_ = ToTransform(stage_pose);
      }
    } else {
      Log("Monado IPC connection deferred (libmonado not loaded or runtime not ready)");
    }
  }

  initialized_ = true;
  PublishTelemetry(QpcNow());
  return true;
}

void DaemonCoordinator::Start() {
  if (running_) return;
  if (!initialized_) {
    if (!Initialize()) return;
  }
  running_ = true;
  if (config_.enable_openxr && openxr_) {
    pose_thread_running_ = true;
    pose_thread_ = std::thread(&DaemonCoordinator::PoseLoop, this);
  }
  worker_thread_ = std::thread(&DaemonCoordinator::WorkerLoop, this);
}

void DaemonCoordinator::Stop() {
  if (!running_ && !initialized_ && !log_file_) return;
  running_ = false;
  pose_thread_running_ = false;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  if (pose_thread_.joinable()) {
    pose_thread_.join();
  }
  if (link_) {
    link_->Stop();
    link_.reset();
  }
  if (openxr_) {
    openxr_->Shutdown();
  }
  if (monado_) {
    monado_->Disconnect();
  }
  if (relations_) {
    relations_->Unhook();
    relations_->Flush(true);
    relations_.reset();
  }
  shm_pub_.Close();
  shm_pub_.Unlink();
  SetRecording(false);
  if (initialized_) {
    Log("QuestLHSync daemon stopped");
  }
  if (log_file_) {
    std::fclose(log_file_);
    log_file_ = nullptr;
  }
  initialized_ = false;
}

void DaemonCoordinator::Run(const volatile sig_atomic_t *signal_flag) {
  if (signal_flag) {
    signal_flag_ = signal_flag;
  }
  if (!initialized_) {
    if (!Initialize()) return;
  }
  running_ = true;
  if (config_.enable_openxr && openxr_) {
    pose_thread_running_ = true;
    pose_thread_ = std::thread(&DaemonCoordinator::PoseLoop, this);
  }
  WorkerLoop();
}

void DaemonCoordinator::PoseLoop() {
  const auto sleep_dur = std::chrono::microseconds(4000); // 250 Hz (covers 72 Hz, 90 Hz, and 120 Hz)
  while (running_ && pose_thread_running_) {
    if (signal_flag_ && *signal_flag_ != 0) break;
    double now = QpcNow();
    PollOpenXr(now);
    double prev_hb = last_pose_heartbeat_.load(std::memory_order_relaxed);
    if (shm_pub_.IsOpen() && now - prev_hb >= 0.2) {
      last_pose_heartbeat_.store(now, std::memory_order_relaxed);
      shm_pub_.Update([now](QlhsStatus &st) {
        if (st.updated > 0.0 && now > st.updated) {
          st.updated = now;
        }
      });
    }
    std::this_thread::sleep_for(sleep_dur);
  }
}

void DaemonCoordinator::WorkerLoop() {
  const double sleep_sec = 1.0 / std::max(10.0, config_.publish_rate_hz);
  const auto sleep_dur = std::chrono::microseconds(static_cast<int64_t>(sleep_sec * 1e6));

  while (running_) {
    if (signal_flag_ && *signal_flag_ != 0) {
      RequestStop();
      break;
    }
    double now = QpcNow();
    Step(now);
    std::this_thread::sleep_for(sleep_dur);
  }
}

void DaemonCoordinator::Step(double now) {
  bool live_clock = false;
  if (now < 0.0) {
    now = QpcNow();
    live_clock = true;
  }

  if (config_.enable_monado && monado_) {
    EnsureMonadoConnected(now);
  }

  // If pose_thread_ is not active (e.g. synchronous tests), poll OpenXR synchronously
  if (!pose_thread_.joinable()) {
    PollOpenXr(now);
  }

  PollLighthouseDb(now);
  PollConfigFile(now);

  if (link_) {
    link_->SetWanted(hmd_located_.load(std::memory_order_relaxed));
  }

  PollCommands();
  StepSolver(now);

  if (relations_) {
    if (!rel_done_ && now - started_ < 60.0) {
      rel_done_ = relations_->Hook();
    }
    relations_->Flush();
  }

  double pub_now = (live_clock || running_.load(std::memory_order_relaxed)) ? QpcNow() : now;
  double min_interval = 1.0 / std::max(10.0, config_.publish_rate_hz);
  if (pub_now - last_publish_ >= min_interval) {
    PublishTelemetry(pub_now);
    last_publish_ = pub_now;
  }
}

void DaemonCoordinator::PollOpenXr(double now) {
  if (!openxr_) return;

  if (!openxr_->IsInitialized()) {
    if (now - last_openxr_retry_ >= 2.0) {
      last_openxr_retry_ = now;
      if (openxr_->Initialize()) {
        Log("OpenXR headless session initialized");
      }
    }
    if (!openxr_->IsInitialized()) return;
  }

  openxr_->PollEvents();
  openxr_->UpdateDevices();

  if (config_.enable_monado && monado_ && monado_->IsConnected()) {
    MndPose stage_pose{};
    if (monado_->GetReferenceSpaceOffset(MND_REFERENCE_SPACE_TYPE_STAGE, stage_pose) == MND_SUCCESS) {
      std::lock_guard<std::mutex> g(lh_offset_mutex_);
      current_stage_offset_ = ToTransform(stage_pose);
    }
  }

  Transform stage_offset;
  Transform lh_offset;
  {
    std::lock_guard<std::mutex> g(lh_offset_mutex_);
    stage_offset = current_stage_offset_;
    lh_offset = current_lh_offset_;
  }

  XrTime xr_now = openxr_->ConvertTimeToXrTime(now);

  // Locate HMD
  Transform hmd_xf;
  bool hmd_valid = false;
  if (openxr_->LocateHmd(hmd_xf, hmd_valid, xr_now) && hmd_valid) {
    if (stage_offset.active) {
      // OpenXrLocator locates spaces relative to XR_REFERENCE_SPACE_TYPE_STAGE,
      // which includes any playspace mover offset (e.g. WayVR Space Drag) as T_stage^-1.
      // Left-multiply by T_stage to recover the un-dragged root coordinate.
      hmd_xf = Compose(stage_offset, hmd_xf);
    }
    hmd_located_.store(true, std::memory_order_relaxed);
    if (sync_) {
      sync_->OnHmdPose(now, hmd_xf.q, hmd_xf.t);
    }
  } else {
    hmd_located_.store(false, std::memory_order_relaxed);
  }

  // Locate peripherals and tracking references
  auto devices = openxr_->GetDevices();
  std::map<std::string, std::pair<V3, M3>> raw_stations;

  for (const auto &dev : devices) {
    if (dev.is_hmd) continue;

    Transform dev_xf;
    bool dev_valid = false;
    if (!openxr_->LocateDevice(dev.serial, dev_xf, dev_valid, xr_now) || !dev_valid) {
      continue;
    }

    if (stage_offset.active) {
      dev_xf = Compose(stage_offset, dev_xf);
    }

    if (IsTrackingReference(dev.name, dev.serial)) {
      V3 p_raw = dev_xf.t;
      M3 R_raw = ToM3(dev_xf.q);
      if (lh_offset.active) {
        M3 R_inv = T(ToM3(lh_offset.q));
        p_raw = R_inv * (dev_xf.t - lh_offset.t);
        R_raw = R_inv * R_raw;
      }
      raw_stations[dev.serial] = {p_raw, R_raw};
    } else if (IsLighthousePeripheral(dev.name, dev.serial)) {
      if (sync_) {
        // OpenXR MNDX xdev spaces are children of their Monado tracking origin space.
        // Invert the active lighthouse tracking origin offset to recover raw lighthouse coordinates.
        V3 p_raw = dev_xf.t;
        if (lh_offset.active) {
          p_raw = TransformPoint(Inverse(lh_offset), dev_xf.t);
        }
        sync_->OnBodyPose(static_cast<int>(dev.dev_id), now, p_raw);
      }
    }
  }

  if (!raw_stations.empty()) {
    if (sync_) {
      sync_->SetStationsRaw(raw_stations);
    }
  }
}

void DaemonCoordinator::PollLighthouseDb(double now) {
  if (!config_.enable_lighthousedb_poll) return;
  if (now - last_lighthousedb_poll_ < 1.0) return;
  last_lighthousedb_poll_ = now;

  namespace fs = std::filesystem;
  fs::path db_path;
  if (!config_.lighthousedb_path.empty()) {
    std::error_code ec;
    if (fs::exists(config_.lighthousedb_path, ec)) {
      db_path = config_.lighthousedb_path;
    }
  } else {
    const char *home = std::getenv("HOME");
    if (!home) return;

    const std::vector<fs::path> candidates = {
        fs::path(home) / ".local/share/Steam/config/lighthouse/lighthousedb.json",
        fs::path(home) / ".steam/root/config/lighthouse/lighthousedb.json",
        fs::path(home) / ".steam/steam/config/lighthouse/lighthousedb.json",
    };

    for (const auto &p : candidates) {
      std::error_code ec;
      if (fs::exists(p, ec)) {
        db_path = p;
        break;
      }
    }
  }
  if (db_path.empty()) return;

  std::error_code ec;
  auto mtime = fs::last_write_time(db_path, ec);
  bool mtime_ok = !ec;
  if (mtime_ok && mtime == last_lighthousedb_mtime_ && (now - last_lighthousedb_full_check_ < 5.0)) {
    return;
  }
  last_lighthousedb_full_check_ = now;
  if (mtime_ok) {
    last_lighthousedb_mtime_ = mtime;
  }

  std::ifstream f(db_path, std::ios::binary);
  if (!f.is_open()) return;
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();

  JVal root;
  JParser parser(content.data(), content.size());
  if (!parser.Parse(root)) return;

  std::map<uint32_t, int> ch_map;
  std::map<uint32_t, uint64_t> last_seen_map;
  std::map<uint32_t, std::pair<double, double>> bs_tilt_map;

  const JVal *bs_list = root.get("base_stations");
  if (bs_list && bs_list->t == JVal::Arr) {
    for (const auto &b : bs_list->a) {
      const JVal *cfg = b.get("config");
      if (!cfg) continue;
      const JVal *sn = cfg->get("serialNumber");
      if (!sn || sn->t != JVal::Num) continue;
      uint32_t s_num = static_cast<uint32_t>(sn->n);

      const JVal *dyn = b.get("dynamic_states");
      if (dyn && dyn->t == JVal::Arr && !dyn->a.empty()) {
        double best_var = 1e30;
        for (const auto &st_entry : dyn->a) {
          const JVal *tv = st_entry.get("tilt");
          if (tv && tv->t == JVal::Obj) {
            const JVal *pv = tv->get("pitch");
            const JVal *rv = tv->get("roll");
            if (pv && rv && pv->t == JVal::Num && rv->t == JVal::Num &&
                std::isfinite(pv->n) && std::isfinite(rv->n)) {
              const JVal *vv = tv->get("variance");
              double var = (vv && vv->t == JVal::Num && std::isfinite(vv->n)) ? vv->n : 1.0;
              if (var <= best_var || !bs_tilt_map.count(s_num)) {
                best_var = var;
                bs_tilt_map[s_num] = {pv->n, rv->n};
              }
            }
          }
        }
        const auto &latest = dyn->a.back();
        const JVal *ds = latest.get("dynamic_state");
        if (ds) {
          const JVal *ch = ds->get("sobChannel");
          if (ch && ch->t == JVal::Num) {
            ch_map[s_num] = static_cast<int>(ch->n) + 1; // SteamVR sobChannel is 0-indexed; QuestLHSync uses 1..16
          }
        }
        const JVal *tls = latest.get("time_last_seen");
        if (tls) {
          uint64_t t = 0;
          if (tls->t == JVal::Num) {
            t = static_cast<uint64_t>(tls->n);
          } else if (tls->t == JVal::Str) {
            t = std::strtoull(tls->s.c_str(), nullptr, 10);
          }
          last_seen_map[s_num] = t;
        }
      }
    }
  }

  const JVal *univ_list = root.get("known_universes");
  if (!univ_list || univ_list->t != JVal::Arr || univ_list->a.empty()) return;

  const JVal *best_u = nullptr;
  uint64_t best_time = 0;
  size_t max_count = 0;

  for (const auto &u : univ_list->a) {
    const JVal *u_bs = u.get("base_stations");
    if (!u_bs || u_bs->t != JVal::Arr) continue;

    uint64_t u_time = 0;
    for (const auto &ub : u_bs->a) {
      const JVal *bsn = ub.get("base_serial_number");
      if (!bsn || bsn->t != JVal::Num) continue;
      uint32_t num = static_cast<uint32_t>(bsn->n);
      auto it = last_seen_map.find(num);
      if (it != last_seen_map.end() && it->second > u_time) {
        u_time = it->second;
      }
    }
    if (u_time > best_time || (u_time == best_time && u_bs->a.size() > max_count)) {
      best_time = u_time;
      best_u = &u;
      max_count = u_bs->a.size();
    }
  }

  if (!best_u) return;

  const JVal *u_bs = best_u->get("base_stations");
  if (!u_bs || u_bs->t != JVal::Arr) return;

  // In SteamVR lighthousedb.json, target_pose is T_base_from_origin (the pose of the
  // target/origin 0 in the base station's unlevelled coordinate frame).
  // Compute the universe level rotation R_level matching driver_lighthouse.so (253ab0):
  int tilt_method = 0;
  const JVal *tm_val = best_u->get("tilt_to_pose_method");
  if (tm_val && tm_val->t == JVal::Num && std::isfinite(tm_val->n)) {
    tilt_method = static_cast<int>(tm_val->n);
  }
  auto make_level_matrix = [](double pitch, double roll, int method) -> M3 {
    if (method == 1) {
      return T(Tilt(pitch, roll));
    }
    V3 y_row = Tilt(pitch, roll).col(1);
    double h = std::hypot(y_row.x, y_row.y);
    if (h <= 1e-12) {
      return T(Tilt(pitch, roll));
    }
    V3 x_row{y_row.y / h, -y_row.x / h, 0.0};
    V3 z_row = cross(x_row, y_row);
    M3 R;
    R.m[0][0] = x_row.x; R.m[0][1] = x_row.y; R.m[0][2] = x_row.z;
    R.m[1][0] = y_row.x; R.m[1][1] = y_row.y; R.m[1][2] = y_row.z;
    R.m[2][0] = z_row.x; R.m[2][1] = z_row.y; R.m[2][2] = z_row.z;
    return R;
  };

  M3 R_level;
  bool have_level = false;
  const JVal *tilt_val = best_u->get("tilt");
  if (tilt_val && tilt_val->t == JVal::Obj) {
    const JVal *pv = tilt_val->get("pitch");
    const JVal *rv = tilt_val->get("roll");
    if (pv && rv && pv->t == JVal::Num && rv->t == JVal::Num &&
        std::isfinite(pv->n) && std::isfinite(rv->n)) {
      R_level = make_level_matrix(pv->n, rv->n, tilt_method);
      have_level = true;
    }
  }
  if (!have_level) {
    double min_dist2 = 1e30;
    for (const auto &ub : u_bs->a) {
      const JVal *bsn = ub.get("base_serial_number");
      if (!bsn || bsn->t != JVal::Num) continue;
      uint32_t num = static_cast<uint32_t>(bsn->n);
      auto it_tilt = bs_tilt_map.find(num);
      if (it_tilt == bs_tilt_map.end()) continue;

      const JVal *tp = ub.get("target_pose");
      if (!tp) continue;
      const JVal *pv = tp->get("pose");
      if (!pv || pv->t != JVal::Arr || pv->a.size() < 7) continue;
      std::vector<double> p = pv->nums();
      if (p.size() < 7) continue;

      double d2 = p[4] * p[4] + p[5] * p[5] + p[6] * p[6];
      if (d2 < min_dist2) {
        min_dist2 = d2;
        Quat q_raw{p[3], p[0], p[1], p[2]};
        M3 R_inv = T(ToM3(q_raw));
        V3 u_local = Tilt(it_tilt->second.first, it_tilt->second.second).col(1);
        V3 u_global = R_inv * u_local;
        double gn = norm(u_global);
        if (gn > 1e-12) {
          u_global = u_global * (1.0 / gn);
          double th = std::acos(std::max(-1.0, std::min(1.0, u_global.y)));
          double h = std::hypot(u_global.x, u_global.z);
          double g_pitch = (h > 1e-15) ? (th * u_global.z / h) : 0.0;
          double g_roll = (h > 1e-15) ? (-th * u_global.x / h) : 0.0;
          R_level = make_level_matrix(g_pitch, g_roll, tilt_method);
          have_level = true;
        }
      }
    }
  }

  uint32_t origin_serial = 0;
  for (const auto &ub : u_bs->a) {
    const JVal *bsn = ub.get("base_serial_number");
    if (!bsn || bsn->t != JVal::Num) continue;
    const JVal *tp = ub.get("target_pose");
    if (!tp) continue;
    const JVal *pv = tp->get("pose");
    if (!pv || pv->t != JVal::Arr || pv->a.size() < 7) continue;
    std::vector<double> p = pv->nums();
    if (p.size() < 7) continue;
    double d2 = p[4] * p[4] + p[5] * p[5] + p[6] * p[6];
    if (d2 < 1e-6) {
      origin_serial = static_cast<uint32_t>(bsn->n);
      break;
    }
  }

  std::map<std::string, std::pair<V3, M3>> l_stations;
  std::map<std::string, int> channels;

  for (const auto &ub : u_bs->a) {
    const JVal *bsn = ub.get("base_serial_number");
    if (!bsn || bsn->t != JVal::Num) continue;
    uint32_t num = static_cast<uint32_t>(bsn->n);

    // Exclude stale base stations from prior setups that were not seen in the current session
    auto it_seen = last_seen_map.find(num);
    uint64_t t_seen = (it_seen != last_seen_map.end()) ? it_seen->second : 0;
    if (best_time > 86400 && t_seen + 86400 < best_time) {
      continue;
    }

    char sbuf[32];
    std::snprintf(sbuf, sizeof(sbuf), "LHB-%08X", num);
    std::string s_name(sbuf);

    if (ch_map.count(num)) {
      channels[s_name] = ch_map[num];
    }

    const JVal *tp = ub.get("target_pose");
    if (!tp) continue;
    const JVal *pv = tp->get("pose");
    if (!pv || pv->t != JVal::Arr || pv->a.size() < 7) continue;

    std::vector<double> p = pv->nums();
    if (p.size() < 7) continue;

    if (relations_ && config_.steady_stations && origin_serial != 0 && num != origin_serial) {
      std::array<float, 7> rel = {
          static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]),
          static_cast<float>(p[3]), static_cast<float>(p[4]), static_cast<float>(p[5]),
          static_cast<float>(p[6])};
      auto key = std::make_pair(origin_serial, num);
      bool changed = true;
      auto it_raw = last_raw_rel_.find(key);
      if (it_raw != last_raw_rel_.end()) {
        changed = false;
        for (int i = 0; i < 7; ++i) {
          if (std::fabs(rel[i] - it_raw->second[i]) > 1e-6f) {
            changed = true;
            break;
          }
        }
      }
      if (changed) {
        last_raw_rel_[key] = rel;
        float avg[7];
        if (relations_->Average(origin_serial, num, rel.data(), avg)) {
          std::array<float, 7> avg_arr = {avg[0], avg[1], avg[2], avg[3], avg[4], avg[5], avg[6]};
          last_avg_rel_[key] = avg_arr;
          for (int i = 0; i < 7; ++i) p[i] = avg[i];
        } else {
          last_avg_rel_.erase(key);
        }
      } else {
        auto it_avg = last_avg_rel_.find(key);
        if (it_avg != last_avg_rel_.end()) {
          for (int i = 0; i < 7; ++i) p[i] = it_avg->second[i];
        }
      }
    }

    // p is [qx, qy, qz, qw, x, y, z] representing T_base_from_origin.
    // Invert T_base_from_origin to get T_origin_from_base = (R_inv, -R_inv * raw_pos),
    // then left-multiply by R_level to obtain the levelled universe pose matching driver_lighthouse.so.
    Quat q{p[3], p[0], p[1], p[2]};
    M3 R_inv = T(ToM3(q));
    V3 raw_pos{p[4], p[5], p[6]};
    V3 pos = R_level * (R_inv * (raw_pos * -1.0));
    M3 rot = R_level * R_inv;
    l_stations[s_name] = {pos, rot};
  }

  if (relations_) {
    relations_->Flush();
  }

  if (!l_stations.empty() && sync_) {
    sync_->SetStationsRaw(l_stations);
    if (!channels.empty()) {
      sync_->SetChannels(channels);
    }
  }
}

void DaemonCoordinator::StepSolver(double now) {
  if (!sync_) return;

  Transform xf = sync_->Tick(now);
  ApplyMonadoOffset(xf);
}

bool DaemonCoordinator::EnsureMonadoConnected(double now) {
  if (!monado_) return false;

  auto query_initial_offset = [this]() {
    MndPose cur_pose{};
    if (monado_->GetTrackingOriginOffset(lighthouse_origin_id_, cur_pose) == MND_SUCCESS) {
      std::lock_guard<std::mutex> g(lh_offset_mutex_);
      current_lh_offset_ = ToTransform(cur_pose);
    }
    MndPose stage_pose{};
    if (monado_->GetReferenceSpaceOffset(MND_REFERENCE_SPACE_TYPE_STAGE, stage_pose) == MND_SUCCESS) {
      std::lock_guard<std::mutex> g(lh_offset_mutex_);
      current_stage_offset_ = ToTransform(stage_pose);
    }
  };

  if (!monado_->IsConnected()) {
    if (now - last_monado_connect_try_ >= 2.0) {
      last_monado_connect_try_ = now;
      if (monado_->Connect()) {
        Log("Monado IPC connected");
        if (monado_->FindTrackingOriginByName(config_.lighthouse_origin_name, lighthouse_origin_id_) == MND_SUCCESS) {
          origin_id_resolved_ = true;
          Log("Found tracking origin '" + config_.lighthouse_origin_name + "' with id " + std::to_string(lighthouse_origin_id_));
        } else {
          lighthouse_origin_id_ = config_.default_origin_id;
          origin_id_resolved_ = true;
        }
        query_initial_offset();
      }
    }
    if (!monado_->IsConnected()) return false;
  }

  if (!origin_id_resolved_) {
    if (monado_->FindTrackingOriginByName(config_.lighthouse_origin_name, lighthouse_origin_id_) == MND_SUCCESS) {
      origin_id_resolved_ = true;
    } else {
      lighthouse_origin_id_ = config_.default_origin_id;
      origin_id_resolved_ = true;
    }
    query_initial_offset();
  }
  return true;
}

void DaemonCoordinator::ApplyMonadoOffset(const Transform &xf) {
  if (!monado_) return;

  double now = QpcNow();
  if (!EnsureMonadoConnected(now)) return;

  if (xf.active) {
    MndPose pose = ToMndPose(xf);
    int res = monado_->SetTrackingOriginOffset(lighthouse_origin_id_, pose);
    if (res == MND_SUCCESS) {
      std::lock_guard<std::mutex> g(lh_offset_mutex_);
      current_lh_offset_ = xf;
    } else {
      Log("Warning: Monado SetTrackingOriginOffset returned error " + std::to_string(res));
      if (res == MND_ERROR_CONNECTING_FAILED) {
        monado_->Disconnect();
        origin_id_resolved_ = false;
      }
    }
  }
}

void DaemonCoordinator::PollCommands() {
  QlhsCmd cmd = QLHS_CMD_NONE;
  while (shm_pub_.PollCommand(cmd)) {
    HandleCommand(cmd);
  }
}

void DaemonCoordinator::HandleCommand(QlhsCmd cmd) {
  switch (cmd) {
    case QLHS_CMD_PAUSE:
      if (sync_) sync_->SetPaused(true);
      Log("Corrections paused");
      break;
    case QLHS_CMD_RESUME:
      if (sync_) sync_->SetPaused(false);
      Log("Corrections resumed");
      break;
    case QLHS_CMD_RECALIBRATE:
      if (sync_) sync_->ForceRecalibrate();
      if (config_.enable_monado && monado_ && EnsureMonadoConnected(QpcNow())) {
        MndPose identity_pose{};
        if (monado_->SetTrackingOriginOffset(lighthouse_origin_id_, identity_pose) == MND_SUCCESS) {
          std::lock_guard<std::mutex> g(lh_offset_mutex_);
          current_lh_offset_ = ToTransform(identity_pose);
        }
      }
      Log("Force recalibration triggered");
      break;
    case QLHS_CMD_RECORD_ON:
      SetRecording(true);
      break;
    case QLHS_CMD_RECORD_OFF:
      SetRecording(false);
      break;
    case QLHS_CMD_RELOAD_CONFIG:
      ReloadConfig();
      break;
    case QLHS_CMD_REACQUIRE:
    case QLHS_CMD_NONE:
    default:
      break;
  }
}

void DaemonCoordinator::SetRecording(bool on) {
  if (on == recording_.load(std::memory_order_relaxed)) return;
  recording_.store(on, std::memory_order_relaxed);
  if (!sync_) return;

  if (!on) {
    sync_->SetRecord(nullptr);
    Log("Recording stopped: " + Sync::Describe(rec_spots_, sync_->spots()));
    return;
  }

  std::filesystem::path rec_dir = std::filesystem::path(config_.config_dir) / "recordings";
  std::error_code ec;
  std::filesystem::create_directories(rec_dir, ec);

  // Keep newest 9 recordings
  std::vector<std::filesystem::path> old_recs;
  for (const auto &entry : std::filesystem::directory_iterator(rec_dir, ec)) {
    if (entry.path().extension() == ".txt") {
      old_recs.push_back(entry.path());
    }
  }
  std::sort(old_recs.begin(), old_recs.end());
  while (old_recs.size() > 9) {
    std::filesystem::remove(old_recs.front(), ec);
    old_recs.erase(old_recs.begin());
  }

  std::time_t t = std::time(nullptr);
  struct std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char file_name[64];
  std::strftime(file_name, sizeof(file_name), "qlhs-%Y%m%d-%H%M%S.txt", &tm_buf);

  std::string file_path = (rec_dir / file_name).string();
  FILE *f = std::fopen(file_path.c_str(), "w");
  if (!f) {
    Log("Error: Unable to open recording file " + file_path);
    recording_.store(false, std::memory_order_relaxed);
    return;
  }

  std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
  sync_->SetRecord(f);
  rec_spots_ = sync_->spots();
  sync_->Rec(QpcNow(), "I QuestLHSync Linux recording (qlhs_replay reads it)");
  Log("Recording session to " + file_path);
}

void DaemonCoordinator::PublishTelemetry(double now) {
  if (!shm_pub_.IsOpen()) return;

  Sync::Status s{};
  if (sync_) {
    s = sync_->GetStatus(now);
  }

  int state = QLHS_STARTING;
  HeadsetLink::State ls = link_ ? link_->state() : HeadsetLink::kIdle;

  if (!hmd_located_.load(std::memory_order_relaxed)) {
    state = QLHS_NO_HMD;
  } else if (ls == HeadsetLink::kSearching || ls == HeadsetLink::kIdle) {
    state = QLHS_SEARCHING;
  } else if (ls == HeadsetLink::kConnecting) {
    state = QLHS_CONNECTING;
  } else if (link_ && (now - link_->last_frame() > 3.0 && now - link_->connected_at() > 5.0)) {
    state = QLHS_NO_CAMERAS;
  } else if (s.nstations < 2) {
    state = QLHS_NO_STATIONS;
  } else if (s.locked) {
    state = QLHS_LOCKED;
  } else {
    state = QLHS_ACQUIRING;
  }

  QlhsStatus st{};
  st.magic = QLHS_MAGIC;
  st.version = QLHS_VERSION;
  st.state = state;
  st.updated = now;

  if (state == QLHS_NO_HMD) {
    st.hmd[0] = '\0';
  } else {
    SafeCopy(st.hmd, sizeof(st.hmd), (link_ && !link_->model().empty()) ? link_->model() : hmd_model_);
  }
  SafeCopy(st.hmd_system, sizeof(st.hmd_system), hmd_system_);
  SafeCopy(st.headset, sizeof(st.headset), (link_ && !link_->model().empty()) ? link_->model() : headset_model_);
  std::string display_addr;
  if (link_ && ls == HeadsetLink::kConnected) {
    display_addr = link_->addr();
  } else if (!config_.hosts.empty()) {
    display_addr = config_.hosts[0];
  }
  SafeCopy(st.headset_addr, sizeof(st.headset_addr), display_addr);
  SafeCopy(st.headset_fw, sizeof(st.headset_fw), link_ ? link_->fw() : "");

  st.cam_fps = s.cam_fps;
  st.sight_rate = s.sight_rate;
  st.spot_rate = s.spot_rate;
  st.head_still = s.head_still;
  st.daemon_pid = static_cast<int32_t>(getpid());
  st.rtt_ms = s.rtt * 1000.0;
  st.expo_ms = s.expo * 1000.0;
  st.expo_learned = s.timing_learned ? 1 : 0;

  st.nst = static_cast<int32_t>(std::min<size_t>(s.st.size(), 8));
  for (int i = 0; i < st.nst; i++) {
    const auto &e = s.st[i];
    SafeCopy(st.st[i].serial, sizeof(st.st[i].serial), e.serial);
    st.st[i].support = e.support;
    st.st[i].anchor = e.anchor ? 1 : 0;
    st.st[i].measured = e.measured ? 1 : 0;
    st.st[i].last_seen = e.last_seen;
    st.st[i].dist = e.dist;
  }

  st.locked = s.locked ? 1 : 0;
  st.cond = s.cond ? 1 : 0;
  st.yaw_deg = s.has_x ? s.x[0] * kDeg : 0.0;
  for (int i = 0; i < 3; i++) {
    st.t[i] = s.has_x ? s.x[i + 1] : 0.0;
  }
  st.med_deg = s.med;
  st.nfit = s.n;
  st.paused = (sync_ && sync_->paused()) ? 1 : 0;
  st.locked_for = s.locked_for;
  st.lag_cm = s.lag_cm;
  st.recording = recording_.load(std::memory_order_relaxed) ? 1 : 0;

  {
    std::lock_guard<std::mutex> g(log_mutex_);
    for (int i = 0; i < 8; i++) {
      SafeCopy(st.log[i], sizeof(st.log[0]), log_history_[i]);
    }
    st.nlog = log_counter_;
  }

  shm_pub_.Publish(st);
}

void DaemonCoordinator::Log(const std::string &msg) {
  if (config_.verbose) {
    std::cout << "[questlhsyncd] " << msg << std::endl;
  }

  std::time_t t = std::time(nullptr);
  struct std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char time_str[32];
  std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

  std::string formatted = std::string(time_str) + "  " + msg;

  if (log_file_) {
    std::fprintf(log_file_, "%s\n", formatted.c_str());
    std::fflush(log_file_);
  }

  std::lock_guard<std::mutex> g(log_mutex_);
  log_history_[log_counter_ % 8] = formatted;
  log_counter_++;
}

bool DaemonCoordinator::IsMonadoConnected() const {
  return monado_ && monado_->IsConnected();
}

bool DaemonCoordinator::SetCalibration(const std::string &json, std::string *err) {
  if (!sync_) return false;
  return sync_->SetCalibration(json, err);
}

void DaemonCoordinator::FeedHeadsetLine(const char *line, double pc_now) {
  if (!sync_) return;
  if (pc_now < 0.0) pc_now = QpcNow();
  sync_->OnLine(pc_now, line);
}

void DaemonCoordinator::FeedHeadsetPing(double pc_send, double pc_recv, double hs) {
  if (!sync_) return;
  sync_->OnPing(pc_send, pc_recv, hs);
}

void DaemonCoordinator::SetHeadsetInfo(const std::string &serial, const std::string &model) {
  headset_serial_ = serial;
  headset_model_ = model;
  if (sync_) {
    sync_->SetHeadset(serial, model);
  }
}

void DaemonCoordinator::ReloadConfig() {
  DaemonConfig new_cfg = config_;
  new_cfg.hosts.clear();
  if (LoadConfigFile(config_.config_dir, new_cfg)) {
    bool hosts_changed = (new_cfg.hosts != config_.hosts);
    bool pref_changed = (new_cfg.preferred_headset != config_.preferred_headset);
    config_.hosts = new_cfg.hosts;
    config_.preferred_headset = new_cfg.preferred_headset;
    config_.steady_stations = new_cfg.steady_stations;
    if (relations_) {
      relations_->SetEnabled(config_.steady_stations);
    }
    if (link_) {
      link_->SetHosts(config_.hosts);
      link_->SetPreferred(config_.preferred_headset);
    }
    if (hosts_changed || pref_changed) {
      std::string h_str = config_.hosts.empty() ? "auto-discover" : config_.hosts[0];
      Log("Configuration reloaded from config.json: target host=" + h_str +
          (config_.preferred_headset.empty() ? "" : ", preferred=" + config_.preferred_headset));
    }
  }
}

void DaemonCoordinator::PollConfigFile(double now) {
  if (now - last_config_check_ < 1.0) return;
  last_config_check_ = now;

  std::filesystem::path p = std::filesystem::path(config_.config_dir) / "config.json";
  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(p, ec);
  if (!ec) {
    if (config_file_mtime_ == std::filesystem::file_time_type{} || mtime != config_file_mtime_) {
      config_file_mtime_ = mtime;
      ReloadConfig();
    }
  }
}

bool LoadConfigFile(const std::string &config_dir, DaemonConfig &cfg, std::string *err) {
  if (config_dir.empty()) return false;
  std::filesystem::path p = std::filesystem::path(config_dir) / "config.json";
  std::ifstream f(p);
  if (!f.is_open()) return false;
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();

  JVal root;
  if (!JParse(text, root) || root.t != JVal::Obj) {
    if (err) *err = "Failed to parse JSON in " + p.string();
    return false;
  }

  if (const JVal *h = root.get("host")) {
    if (h->t == JVal::Str) {
      if (!h->s.empty()) {
        cfg.hosts = { h->s };
      } else {
        cfg.hosts.clear();
      }
    }
  } else if (const JVal *hv = root.get("hosts")) {
    if (hv->t == JVal::Arr) {
      cfg.hosts.clear();
      for (const auto &item : hv->a) {
        if (item.t == JVal::Str && !item.s.empty()) {
          cfg.hosts.push_back(item.s);
        }
      }
    }
  }
  if (const JVal *pref = root.get("preferred")) {
    if (pref->t == JVal::Str) cfg.preferred_headset = pref->s;
  }
  if (const JVal *st = root.get("streamer")) {
    if (st->t == JVal::Str && !st->s.empty()) cfg.streamer = st->s;
  }
  if (const JVal *ss = root.get("steadyStations")) {
    if (ss->t == JVal::Bool) cfg.steady_stations = ss->b;
  }
  return true;
}

bool SaveConfigFile(const std::string &config_dir, const std::string &host, const std::string &preferred, std::string *err) {
  if (config_dir.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(config_dir, ec);

  std::filesystem::path p = std::filesystem::path(config_dir) / "config.json";
  std::filesystem::path tmp_p = std::filesystem::path(config_dir) / "config.json.tmp";

  std::ofstream f(tmp_p);
  if (!f.is_open()) {
    if (err) *err = "Failed to open temporary file for writing: " + tmp_p.string();
    return false;
  }

  std::string streamer = "WiVRn";
  DaemonConfig cur_cfg;
  if (LoadConfigFile(config_dir, cur_cfg) && !cur_cfg.streamer.empty()) {
    streamer = cur_cfg.streamer;
  }

  f << "{\n";
  f << "  \"host\": \"" << host << "\",\n";
  f << "  \"preferred\": \"" << preferred << "\",\n";
  f << "  \"streamer\": \"" << streamer << "\"\n";
  f << "}\n";
  f.close();

  std::filesystem::rename(tmp_p, p, ec);
  if (ec) {
    if (err) *err = "Failed to rename temp file: " + ec.message();
    return false;
  }
  return true;
}

CliParseResult ParseDaemonConfig(
    int argc,
    char *argv[],
    DaemonConfig &cfg,
    bool *run_headless,
    std::string *error_msg) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--headless") {
      cfg.headless = true;
      if (run_headless) *run_headless = true;
    } else if (arg == "--daemon") {
      cfg.headless = true;
      if (run_headless) *run_headless = true;
    } else if (arg == "--verbose" || arg == "-v") {
      cfg.verbose = true;
    } else if (arg == "--config-dir") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --config-dir requires an argument";
        return CliParseResult::Error;
      }
      cfg.config_dir = argv[++i];
    } else if (arg == "--shm") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --shm requires an argument";
        return CliParseResult::Error;
      }
      cfg.shm_name = argv[++i];
    } else if (arg == "--host") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --host requires an argument";
        return CliParseResult::Error;
      }
      std::string host_arg = argv[++i];
      std::string cur;
      for (char c : host_arg + ",") {
        if (c == ',' || c == ' ' || c == ';') {
          if (!cur.empty()) {
            cfg.hosts.push_back(cur);
            cur.clear();
          }
        } else {
          cur += c;
        }
      }
    } else if (arg == "--preferred") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --preferred requires an argument";
        return CliParseResult::Error;
      }
      cfg.preferred_headset = argv[++i];
    } else if (arg == "--streamer") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --streamer requires an argument";
        return CliParseResult::Error;
      }
      cfg.streamer = argv[++i];
    } else if (arg == "--origin") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --origin requires an argument";
        return CliParseResult::Error;
      }
      cfg.lighthouse_origin_name = argv[++i];
    } else if (arg == "--rate") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Error: --rate requires an argument";
        return CliParseResult::Error;
      }
      cfg.publish_rate_hz = std::atof(argv[++i]);
    } else if (arg == "--no-link") {
      cfg.enable_headset_link = false;
    } else if (arg == "--help" || arg == "-h") {
      return CliParseResult::Help;
    } else {
      if (error_msg) *error_msg = "Unknown argument: " + arg;
      return CliParseResult::Error;
    }
  }
  return CliParseResult::Success;
}

} // namespace qlhs
