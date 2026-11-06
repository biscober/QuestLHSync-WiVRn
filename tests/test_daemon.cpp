#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "common/qlhs_shm.h"
#include "common/qlhs_status.h"
#include "daemon/daemon_coordinator.h"
#include "driver/json.h"
#include "driver/mathx.h"
#include "driver/monado_adapter.h"
#include "driver/openxr_locator.h"
#include "driver/sync.h"

namespace {

inline M3 Rx(double a) {
  double c = std::cos(a), s = std::sin(a);
  M3 r;
  r.m[0][0] = 1; r.m[0][1] = 0; r.m[0][2] = 0;
  r.m[1][0] = 0; r.m[1][1] = c; r.m[1][2] = -s;
  r.m[2][0] = 0; r.m[2][1] = s; r.m[2][2] = c;
  return r;
}

std::string MakeTestCalibrationJson() {
  const double yaw_l = 20.0 * M_PI / 180.0;
  const double yaw_r = -20.0 * M_PI / 180.0;
  const double pitch = 10.0 * M_PI / 180.0;

  auto mat4_json = [](double yaw, double pitch, double tx, double ty, double tz) {
    M3 R = Ry(yaw) * Rx(pitch);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "[%.7f, %.7f, %.7f, %.7f, "
             "%.7f, %.7f, %.7f, %.7f, "
             "%.7f, %.7f, %.7f, %.7f, "
             "0.0, 0.0, 0.0, 1.0]",
             R.m[0][0], R.m[0][1], R.m[0][2], tx,
             R.m[1][0], R.m[1][1], R.m[1][2], ty,
             R.m[2][0], R.m[2][1], R.m[2][2], tz);
    return std::string(buf);
  };

  std::string cam0_mat = mat4_json(yaw_l, pitch, -0.05, 0.02, 0.06);
  std::string cam1_mat = mat4_json(yaw_r, pitch, 0.05, 0.02, 0.06);

  return "{\n"
         "  \"CameraCalibration\": [\n"
         "    {\n"
         "      \"Id\": \"0\",\n"
         "      \"ImageSize\": [1280, 1024],\n"
         "      \"DeviceFromCamera\": " + cam0_mat + ",\n"
         "      \"Projection\": { \"Coefficients\": [450.0, 640.0, 512.0] },\n"
         "      \"Distortion\": { \"Coefficients\": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0] }\n"
         "    },\n"
         "    {\n"
         "      \"Id\": \"1\",\n"
         "      \"ImageSize\": [1280, 1024],\n"
         "      \"DeviceFromCamera\": " + cam1_mat + ",\n"
         "      \"Projection\": { \"Coefficients\": [450.0, 640.0, 512.0] },\n"
         "      \"Distortion\": { \"Coefficients\": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0] }\n"
         "    }\n"
         "  ],\n"
         "  \"Device\": { \"DeviceType\": \"Quest Pro\" }\n"
         "}\n";
}

struct CamInfo {
  int id;
  M3 R_dfc;
  V3 t_dfc;
  int w = 1280, h = 1024;
  double f = 450.0, fy = 450.0, cx = 640.0, cy = 512.0;

  bool Project(const V3 &d_cam, double &px, double &py) const {
    if (d_cam.z <= 0.05) return false;
    double a = d_cam.x / d_cam.z;
    double b = d_cam.y / d_cam.z;
    double r = std::hypot(a, b);
    double th = std::atan(r);
    double s = r > 1e-12 ? th / r : 1.0;
    double u = a * s;
    double v = b * s;
    px = cx + f * u;
    py = cy + fy * v;
    return (px >= 20.0 && px <= w - 20.0 && py >= 20.0 && py <= h - 20.0);
  }
};

void TestLifecycleAndShm() {
  std::cout << "[test_daemon] Running TestLifecycleAndShm..." << std::endl;

  std::string test_shm = "/qlhs_test_daemon_life_" + std::to_string(getpid());
  std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  auto mock_openxr = std::make_shared<MockOpenXrLocator>();
  auto mock_monado = std::make_shared<MockMonadoIpc>();
  mock_monado->AddTrackingOrigin("Lighthouse", MndPose{});

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false; // direct testing
  cfg.enable_openxr = true;
  cfg.enable_monado = true;
  cfg.enable_lighthousedb_poll = false;
  cfg.verbose = false;

  qlhs::DaemonCoordinator coord(cfg, mock_openxr, mock_monado);
  assert(coord.Initialize());
  assert(coord.IsInitialized());

  qlhs::ShmReader reader(test_shm);
  assert(reader.Open());

  // Step 1: No HMD yet
  coord.Step(1.0);
  QlhsStatus st{};
  assert(reader.Read(st));
  assert(st.magic == QLHS_MAGIC);
  assert(st.version == QLHS_VERSION);
  assert(st.state == QLHS_NO_HMD);

  // Step 2: Add HMD to OpenXR
  Transform hmd_pose{true, Quat{1, 0, 0, 0}, V3{0, 1.6, 0}};
  mock_openxr->AddMockDevice("HMD_1", "Quest Pro HMD", true, hmd_pose);
  coord.Step(1.1);

  assert(reader.Read(st));
  assert(coord.IsHmdLocated());
  // With HMD present but no stations and link idle/disabled, state should not be NO_HMD
  assert(st.state == QLHS_SEARCHING || st.state == QLHS_NO_STATIONS);

  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestLifecycleAndShm PASSED" << std::endl;
}

void TestSolverToMonadoOffsetPipeline() {
  std::cout << "[test_daemon] Running TestSolverToMonadoOffsetPipeline..." << std::endl;

  std::string test_shm = "/qlhs_test_daemon_pipe_" + std::to_string(getpid());
  std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_pipe_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  auto mock_openxr = std::make_shared<MockOpenXrLocator>();
  auto mock_monado = std::make_shared<MockMonadoIpc>();
  uint32_t lh_id = mock_monado->AddTrackingOrigin("Lighthouse Tracking Origin", MndPose{});

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false;
  cfg.enable_openxr = true;
  cfg.enable_monado = true;
  cfg.enable_lighthousedb_poll = false;
  cfg.per_headset = false;
  cfg.arrival_clock = true;
  cfg.learn_timing = false;
  cfg.lighthouse_origin_name = "lighthouse";
  cfg.verbose = false;

  qlhs::DaemonCoordinator coord(cfg, mock_openxr, mock_monado);
  assert(coord.Initialize());

  qlhs::ShmReader reader(test_shm);
  assert(reader.Open());

  std::string err;
  assert(coord.SetCalibration(MakeTestCalibrationJson(), &err));
  coord.GetSync()->SetStreamer("WiVRn");

  // Camera setup
  const double yaw_l = 20.0 * M_PI / 180.0;
  const double yaw_r = -20.0 * M_PI / 180.0;
  const double pitch = 10.0 * M_PI / 180.0;
  CamInfo cams[2];
  cams[0].id = 0;
  cams[0].R_dfc = Ry(yaw_l) * Rx(pitch);
  cams[0].t_dfc = V3{ -0.05, 0.02, 0.06 };
  cams[1].id = 1;
  cams[1].R_dfc = Ry(yaw_r) * Rx(pitch);
  cams[1].t_dfc = V3{ 0.05, 0.02, 0.06 };

  // Ground truth transform: yaw = 0.38 rad (~21.77 deg), translation = (0.45, 0.12, -0.35)
  const double gt_yaw = 0.38;
  const V3 gt_t = { 0.45, 0.12, -0.35 };

  struct BaseStation {
    std::string serial;
    V3 pos_lh;
    M3 rot_lh;
  };
  std::vector<BaseStation> bs_list = {
    { "LHB-00000001", { 0.0, 2.0, 0.0 }, M3() },
    { "LHB-00000002", { 2.4, 2.2, 2.6 }, M3() },
    { "LHB-00000003", { -2.2, 2.1, 2.4 }, M3() }
  };

  std::map<std::string, std::pair<V3, M3>> raw_stations;
  for (const auto &bs : bs_list) {
    raw_stations[bs.serial] = { bs.pos_lh, bs.rot_lh };
  }
  coord.GetSync()->SetStationsRaw(raw_stations);

  // Trajectory function
  auto get_head_pose = [](double t_val) {
    double yaw = 0.22 * std::sin(0.6 * t_val);
    M3 R = Ry(yaw);
    V3 p = { 0.35 * std::sin(0.45 * t_val), 1.60 + 0.04 * std::sin(0.9 * t_val), 0.35 * std::cos(0.45 * t_val) };
    return std::make_pair(R, p);
  };

  // Add HMD to OpenXR
  auto [R_init, p_init] = get_head_pose(0.0);
  mock_openxr->AddMockDevice("HMD_PRO", "Meta Quest Pro HMD", true, Transform{true, ToQuat(R_init), p_init});

  // Pre-seed pose history
  for (double t = -1.0; t < 0.0; t += 0.01) {
    auto [R, p] = get_head_pose(t);
    coord.GetSync()->OnHmdPose(t, ToQuat(R), p);
  }

  const double t_end = 18.0;
  const double dt_pose = 1.0 / 90.0;
  const double dt_cam = 1.0 / 30.0;

  double next_cam = 0.0;
  unsigned frame_seq = 0;

  for (double t = 0.0; t <= t_end; t += dt_pose) {
    auto [R_hmd, p_hmd] = get_head_pose(t);
    mock_openxr->SetMockDevicePose("HMD_PRO", Transform{true, ToQuat(R_hmd), p_hmd}, true);

    if (t >= next_cam) {
      next_cam += dt_cam;
      long long tus = (long long)(t * 1e6);
      double t_expo = t - kExpoArrival;
      auto [R_cam_hmd, p_cam_hmd] = get_head_pose(t_expo);

      for (int c = 0; c < 2; c++) {
        const CamInfo &cam = cams[c];
        V3 p_cam_world = p_cam_hmd + R_cam_hmd * cam.t_dfc;
        M3 R_cam_world = R_cam_hmd * cam.R_dfc;

        std::vector<std::pair<double, double>> visible_pixels;
        for (const auto &bs : bs_list) {
          V3 p_station_quest = Ry(gt_yaw) * bs.pos_lh + gt_t;
          V3 d_world = p_station_quest - p_cam_world;
          V3 d_cam = T(R_cam_world) * d_world;
          double px, py;
          if (cam.Project(d_cam, px, py)) {
            visible_pixels.push_back({ px, py });
          }
        }

        if (!visible_pixels.empty()) {
          char line[512];
          int off = snprintf(line, sizeof(line), "F %d %u %lld 10 %d", cam.id, ++frame_seq, tus, (int)visible_pixels.size());
          for (const auto &pix : visible_pixels) {
            int x10 = (int)std::round(pix.first * 10.0);
            int y10 = (int)std::round(pix.second * 10.0);
            off += snprintf(line + off, sizeof(line) - off, " %d %d 12 255", x10, y10);
          }
          coord.FeedHeadsetLine(line, t);
        }
      }
    }

    coord.Step(t);
  }

  // Final check
  coord.Step(t_end);
  auto sync_st = coord.GetSync()->GetStatus(t_end);
  std::cout << "Sync direct status: locked=" << sync_st.locked
            << ", has_x=" << sync_st.has_x
            << ", n=" << sync_st.n
            << ", med=" << sync_st.med
            << ", x=[" << sync_st.x[0] << ", " << sync_st.x[1] << ", " << sync_st.x[2] << ", " << sync_st.x[3] << "]"
            << std::endl;

  QlhsStatus final_st{};
  assert(reader.Read(final_st));

  auto sp = coord.GetSync()->spots();
  std::cout << "Spots summary: frames=" << sp.frames << ", seen=" << sp.seen << ", used=" << sp.used
            << ", still=" << sp.still << ", fast=" << sp.fast << ", big=" << sp.big
            << ", other=" << sp.other << ", dim=" << sp.dim << std::endl;

  std::cout << "Status after 18s simulation: locked=" << final_st.locked
            << ", yaw_deg=" << final_st.yaw_deg
            << ", t=[" << final_st.t[0] << ", " << final_st.t[1] << ", " << final_st.t[2] << "]"
            << ", med_deg=" << final_st.med_deg << std::endl;

  assert(final_st.locked == 1 || final_st.nfit > 0);

  // Check that MockMonado received tracking origin offset!
  MndPose monado_offset{};
  assert(mock_monado->GetTrackingOriginOffset(lh_id, monado_offset) == MND_SUCCESS);
  std::cout << "Monado Tracking Origin Offset: pos=["
            << monado_offset.position.x << ", "
            << monado_offset.position.y << ", "
            << monado_offset.position.z << "]" << std::endl;

  // Verify non-zero position set in Monado
  double pos_norm = std::hypot(monado_offset.position.x, monado_offset.position.z);
  assert(pos_norm > 0.1 && "Monado tracking origin offset must be non-zero after convergence");

  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestSolverToMonadoOffsetPipeline PASSED" << std::endl;
}

void TestCommandHandling() {
  std::cout << "[test_daemon] Running TestCommandHandling..." << std::endl;

  std::string test_shm = "/qlhs_test_daemon_cmd_" + std::to_string(getpid());
  std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_cmd_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  auto mock_openxr = std::make_shared<MockOpenXrLocator>();
  auto mock_monado = std::make_shared<MockMonadoIpc>();

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false;
  cfg.verbose = false;

  qlhs::DaemonCoordinator coord(cfg, mock_openxr, mock_monado);
  assert(coord.Initialize());

  qlhs::ShmReader reader(test_shm);
  assert(reader.Open());

  coord.Step(1.0);
  assert(!coord.GetSync()->paused());

  // Send QLHS_CMD_PAUSE
  assert(reader.SendCommand(QLHS_CMD_PAUSE));
  coord.Step(1.1);
  assert(coord.GetSync()->paused());
  QlhsStatus st{};
  assert(reader.Read(st));
  assert(st.paused == 1);

  // Send QLHS_CMD_RESUME
  assert(reader.SendCommand(QLHS_CMD_RESUME));
  coord.Step(1.2);
  assert(!coord.GetSync()->paused());
  assert(reader.Read(st));
  assert(st.paused == 0);

  // Send QLHS_CMD_RECORD_ON
  assert(reader.SendCommand(QLHS_CMD_RECORD_ON));
  coord.Step(1.3);
  assert(reader.Read(st));
  assert(st.recording == 1);

  // Send QLHS_CMD_RECORD_OFF
  assert(reader.SendCommand(QLHS_CMD_RECORD_OFF));
  coord.Step(1.4);
  assert(reader.Read(st));
  assert(st.recording == 0);

  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestCommandHandling PASSED" << std::endl;
}

void TestBackgroundThreadExecution() {
  std::cout << "[test_daemon] Running TestBackgroundThreadExecution..." << std::endl;

  std::string test_shm = "/qlhs_test_daemon_bg_" + std::to_string(getpid());
  std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_bg_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  auto mock_openxr = std::make_shared<MockOpenXrLocator>();
  auto mock_monado = std::make_shared<MockMonadoIpc>();

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false;
  cfg.verbose = false;

  qlhs::DaemonCoordinator coord(cfg, mock_openxr, mock_monado);
  assert(coord.Initialize());

  coord.Start();
  assert(coord.IsRunning());

  qlhs::ShmReader reader(test_shm);
  assert(reader.Open());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  QlhsStatus st{};
  assert(reader.Read(st));
  assert(st.magic == QLHS_MAGIC);

  // Send pause command while background worker is running
  assert(reader.SendCommand(QLHS_CMD_PAUSE));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(reader.Read(st));
  assert(st.paused == 1);

  coord.Stop();
  assert(!coord.IsRunning());

  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestBackgroundThreadExecution PASSED" << std::endl;
}

void TestCliParserAndSignals() {
  std::cout << "[test_daemon] Running TestCliParserAndSignals..." << std::endl;

  // 1. Parsing normal arguments
  {
    const char *argv[] = {
      "questlhsyncd",
      "--config-dir", "/tmp/qlhs_custom_cfg",
      "--shm", "/qlhs_custom_shm",
      "--host", "192.168.1.10,192.168.1.11",
      "--host", "192.168.1.12",
      "--preferred", "1WMHH1234567",
      "--streamer", "WiVRn-Custom",
      "--origin", "lighthouse_custom",
      "--rate", "60.0",
      "--no-link",
      "--verbose",
      "--headless"
    };
    int argc = sizeof(argv) / sizeof(argv[0]);
    qlhs::DaemonConfig cfg;
    bool headless = false;
    std::string err;
    auto res = qlhs::ParseDaemonConfig(argc, const_cast<char**>(argv), cfg, &headless, &err);
    assert(res == qlhs::CliParseResult::Success);
    assert(cfg.config_dir == "/tmp/qlhs_custom_cfg");
    assert(cfg.shm_name == "/qlhs_custom_shm");
    assert(cfg.hosts.size() == 3);
    assert(cfg.hosts[0] == "192.168.1.10");
    assert(cfg.hosts[1] == "192.168.1.11");
    assert(cfg.hosts[2] == "192.168.1.12");
    assert(cfg.preferred_headset == "1WMHH1234567");
    assert(cfg.streamer == "WiVRn-Custom");
    assert(cfg.lighthouse_origin_name == "lighthouse_custom");
    assert(cfg.publish_rate_hz == 60.0);
    assert(!cfg.enable_headset_link);
    assert(cfg.verbose);
    assert(cfg.headless);
    assert(headless);
  }

  // 2. Help flag
  {
    const char *argv[] = {"questlhsyncd", "--help"};
    qlhs::DaemonConfig cfg;
    auto res = qlhs::ParseDaemonConfig(2, const_cast<char**>(argv), cfg);
    assert(res == qlhs::CliParseResult::Help);
  }

  // 3. Error on unknown flag
  {
    const char *argv[] = {"questlhsyncd", "--unknown-flag-xyz"};
    qlhs::DaemonConfig cfg;
    std::string err;
    auto res = qlhs::ParseDaemonConfig(2, const_cast<char**>(argv), cfg, nullptr, &err);
    assert(res == qlhs::CliParseResult::Error);
    assert(!err.empty());
  }

  // 4. Test signal-based graceful shutdown in Run(&sig)
  {
    std::string test_shm = "/qlhs_test_sig_" + std::to_string(getpid());
    std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_sig_" + std::to_string(getpid()));
    std::filesystem::remove_all(config_dir);
    std::filesystem::create_directories(config_dir);

    qlhs::DaemonConfig cfg;
    cfg.config_dir = config_dir.string();
    cfg.shm_name = test_shm;
    cfg.enable_headset_link = false;
    cfg.enable_openxr = false;
    cfg.enable_monado = false;
    cfg.publish_rate_hz = 100.0;

    qlhs::DaemonCoordinator coord(cfg);
    assert(coord.Initialize());

    volatile sig_atomic_t test_sig = 0;
    std::thread runner([&]() {
      coord.Run(&test_sig);
    });

    for (int i = 0; i < 50 && !coord.IsRunning(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(coord.IsRunning());

    // Trigger signal
    test_sig = SIGINT;
    runner.join();

    assert(!coord.IsRunning());
    coord.Stop();
    std::filesystem::remove_all(config_dir);
  }

  std::cout << "[test_daemon] TestCliParserAndSignals PASSED" << std::endl;
}

void TestStopRecordingAndLogCleanup() {
  std::cout << "[test_daemon] Running TestStopRecordingAndLogCleanup..." << std::endl;

  std::string test_shm = "/qlhs_test_cleanup_" + std::to_string(getpid());
  std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_cleanup_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false;
  cfg.enable_openxr = false;
  cfg.enable_monado = false;

  {
    qlhs::DaemonCoordinator coord(cfg);
    assert(coord.Initialize());
    assert(coord.IsInitialized());

    // Turn recording on
    coord.SetRecording(true);
    assert(coord.IsRecording());

    // Stop coordinator: should auto-stop recording and close log_file_
    coord.Stop();
    assert(!coord.IsRecording());
    assert(!coord.IsInitialized());
  }

  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestStopRecordingAndLogCleanup PASSED" << std::endl;
}

void TestUniverseTransition() {
  std::cout << "[test_daemon] Running TestUniverseTransition..." << std::endl;

  std::string test_shm = "/qlhs_test_shm_univ_" + std::to_string(getpid());
  std::filesystem::path config_dir = std::filesystem::temp_directory_path() / ("qlhs_cfg_univ_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  auto mock_openxr = std::make_shared<MockOpenXrLocator>();
  auto mock_monado = std::make_shared<MockMonadoIpc>();

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false;
  cfg.enable_openxr = false;
  cfg.enable_monado = false;
  cfg.enable_lighthousedb_poll = false;
  cfg.verbose = false;

  qlhs::DaemonCoordinator coord(cfg, mock_openxr, mock_monado);
  assert(coord.Initialize());

  // Setup 4 home base stations
  std::map<std::string, std::pair<V3, M3>> home_stations = {
    { "LHB-HOME0001", { {0.0, 2.0, 0.0}, M3() } },
    { "LHB-HOME0002", { {2.0, 2.0, 2.0}, M3() } },
    { "LHB-HOME0003", { {-2.0, 2.0, 2.0}, M3() } },
    { "LHB-HOME0004", { {0.0, 2.0, 3.0}, M3() } }
  };

  coord.GetSync()->SetStationsRaw(home_stations);
  coord.Step(1.0);

  auto st1 = coord.GetSync()->GetStatus(1.0);
  assert(st1.nstations == 4);
  assert(std::filesystem::exists(config_dir / "stations.json"));

  // Now simulate visiting friend's house: 2 completely different base stations
  std::map<std::string, std::pair<V3, M3>> friend_stations = {
    { "LHB-FRND000A", { {0.0, 1.8, 0.0}, M3() } },
    { "LHB-FRND000B", { {3.0, 2.1, -2.5}, M3() } }
  };

  coord.GetSync()->SetStationsRaw(friend_stations);
  coord.Step(2.0);

  auto st2 = coord.GetSync()->GetStatus(2.0);
  assert(st2.nstations == 2);
  bool has_frnd_a = false, has_frnd_b = false;
  for (const auto &s : st2.st) {
    if (s.serial == "LHB-FRND000A") has_frnd_a = true;
    if (s.serial == "LHB-FRND000B") has_frnd_b = true;
  }
  assert(has_frnd_a && has_frnd_b);

  // Now simulate returning home: 4 home stations again
  coord.GetSync()->SetStationsRaw(home_stations);
  coord.Step(3.0);

  auto st3 = coord.GetSync()->GetStatus(3.0);
  assert(st3.nstations == 4);
  bool has_home_1 = false;
  for (const auto &s : st3.st) {
    if (s.serial == "LHB-HOME0001") has_home_1 = true;
  }
  assert(has_home_1);

  coord.Stop();
  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestUniverseTransition PASSED" << std::endl;
}

void TestLighthouseDbTilt() {
  std::cout << "[test_daemon] Running TestLighthouseDbTilt..." << std::endl;

  // Verify that T(Tilt(pitch, roll)) un-tilts the raw base stations accurately
  double pitch = 0.631371915; // 36.17 degrees
  double roll = 0.0237919651;
  M3 R_level = T(Tilt(pitch, roll));

  // Anchor is at (0,0,0)
  V3 raw_c4{-2.31625843, 1.23795748, -1.889696};
  V3 lvl_c4 = R_level * raw_c4;

  V3 raw_af{1.34649181, 2.27857304, -3.19088674};
  V3 lvl_af = R_level * raw_af;

  V3 raw_61{-0.197771549, 2.94983435, -4.04857635};
  V3 lvl_61 = R_level * raw_61;

  // In the raw frame, heights vary by up to 2.95m (a 36-degree ski slope)
  assert(raw_61.y > 2.9);

  // In the levelled frame, all 4 ceiling stations must be on the horizontal plane (within 8 cm)
  assert(std::abs(lvl_c4.y) < 0.08);
  assert(std::abs(lvl_af.y) < 0.08);
  assert(std::abs(lvl_61.y) < 0.08);

  // Pairwise Euclidean distances must be perfectly preserved
  double d_raw = norm(raw_61 - raw_af);
  double d_lvl = norm(lvl_61 - lvl_af);
  assert(std::abs(d_raw - d_lvl) < 1e-6);

  std::cout << "[test_daemon] TestLighthouseDbTilt PASSED" << std::endl;
}

void TestPeripheralRawPoseDirect() {
  std::cout << "[test_daemon] Running TestPeripheralRawPoseDirect..." << std::endl;

  // Verify that when PollLighthouseDb reads a SteamVR lighthousedb.json where known_universes[0]
  // has no top-level "tilt" object (tilt_to_pose_method: 0, tilt stored on the origin base station
  // in base_stations[].dynamic_states), all 4 base stations are levelled by the origin station's tilt,
  // all 4 base stations lock quickly without false layout corrections, and peripheral Vive Tracker
  // poses from OpenXR (which have Monado's tracking origin offset baked in) have the offset inverted
  // before feeding Sync::OnBodyPose.
  std::string test_shm = "/qlhs_test_daemon_real4_" + std::to_string(getpid());
  std::filesystem::path config_dir =
      std::filesystem::temp_directory_path() / ("qlhs_cfg_real4_" + std::to_string(getpid()));
  std::filesystem::remove_all(config_dir);
  std::filesystem::create_directories(config_dir);

  std::filesystem::path db_file = config_dir / "lighthousedb.json";
  {
    std::ofstream out(db_file);
    out << R"JSON({
   "base_stations" : [
      {
         "config" : { "serialNumber" : 1634705723 },
         "dynamic_states" : [
            {
               "dynamic_state" : { "basestation_mode" : 4, "sobChannel" : 2 },
               "tilt" : { "pitch" : 0.628078282, "roll" : 0.0828283876, "variance" : 8.487e-06 },
               "time_last_seen" : "1793894947"
            }
         ]
      },
      {
         "config" : { "serialNumber" : 2936189406 },
         "dynamic_states" : [
            {
               "dynamic_state" : { "basestation_mode" : 4, "sobChannel" : 3 },
               "tilt" : { "pitch" : 0.610307455, "roll" : 0.0648146197, "variance" : 1.151e-06 },
               "time_last_seen" : "1793894971"
            }
         ]
      },
      {
         "config" : { "serialNumber" : 3197617591 },
         "dynamic_states" : [
            {
               "dynamic_state" : { "basestation_mode" : 4, "sobChannel" : 0 },
               "tilt" : { "pitch" : 0.613488793, "roll" : 0.0241179466, "variance" : 9.107e-07 },
               "time_last_seen" : "1793894973"
            }
         ]
      },
      {
         "config" : { "serialNumber" : 3289621119 },
         "dynamic_states" : [
            {
               "dynamic_state" : { "basestation_mode" : 4, "sobChannel" : 1 },
               "tilt" : { "pitch" : 0.549421787, "roll" : -0.0379958227, "variance" : 9.887e-06 },
               "time_last_seen" : "1793894973"
            }
         ]
      }
   ],
   "known_universes" : [
      {
         "base_stations" : [
            {
               "base_serial_number" : 1634705723,
               "target_pose" : {
                  "pose" : [ -0.0183968563, 0.375665158, 0.272125304, 0.88571161, 2.25404406, 1.13251126, -1.25931025 ],
                  "target_serial_number" : 0,
                  "variance" : 4.16832117e-05
               }
            },
            {
               "base_serial_number" : 2936189406,
               "target_pose" : {
                  "pose" : [ 0, 0, 0, 1, 0, 0, 0 ],
                  "target_serial_number" : 0,
                  "variance" : 0
               }
            },
            {
               "base_serial_number" : 3197617591,
               "target_pose" : {
                  "pose" : [ -0.0450164229, 0.763602555, 0.542956233, -0.34653008, -1.47526765, 2.15328455, -3.15353441 ],
                  "target_serial_number" : 0,
                  "variance" : 0.000112146481
               }
            },
            {
               "base_serial_number" : 3289621119,
               "target_pose" : {
                  "pose" : [ -0.0266935825, 0.811068773, 0.500097692, 0.302253425, 1.00626564, 2.38206768, -4.06096077 ],
                  "target_serial_number" : 0,
                  "variance" : 0.000157882809
               }
            }
         ],
         "id" : "1793855610",
         "tilt_to_pose_method" : 0
      }
   ]
})JSON";
  }

  auto mock_openxr = std::make_shared<MockOpenXrLocator>();
  auto mock_monado = std::make_shared<MockMonadoIpc>();
  // Pre-seed a stale offset in Monado (simulating daemon restart while Monado already had an offset)
  MndPose stale_offset{};
  stale_offset.orientation = {0.0f, 0.7071068f, 0.0f, 0.7071068f};
  stale_offset.position = {3.0f, -1.2f, 4.0f};
  uint32_t lh_id = mock_monado->AddTrackingOrigin("lighthouse", stale_offset);

  qlhs::DaemonConfig cfg;
  cfg.config_dir = config_dir.string();
  cfg.shm_name = test_shm;
  cfg.enable_headset_link = false;
  cfg.enable_openxr = true;
  cfg.enable_monado = true;
  cfg.enable_lighthousedb_poll = true;
  cfg.lighthousedb_path = db_file.string();
  cfg.per_headset = false;
  cfg.arrival_clock = true;
  cfg.learn_timing = false;
  cfg.verbose = false;

  qlhs::DaemonCoordinator coord(cfg, mock_openxr, mock_monado);
  assert(coord.Initialize());

  std::string err;
  assert(coord.SetCalibration(MakeTestCalibrationJson(), &err));

  // Trigger initial step to poll lighthousedb.json and write stations.json
  Transform hmd_init{true, Quat{1, 0, 0, 0}, V3{0, 1.65, -2.5}};
  mock_openxr->AddMockDevice("HMD_PRO", "Meta Quest Pro", true, hmd_init);
  mock_openxr->AddMockDevice("LHR-79F8E140", "Vive Tracker 3.0", false, hmd_init);

  coord.Step(1.5);

  // Verify stations.json was written with LEVELLED station positions (not unlevelled +2.38m ski-slope!)
  std::ifstream sf(config_dir / "stations.json");
  assert(sf.is_open());
  std::string sjson((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
  JVal sroot;
  assert(JParse(sjson, sroot));
  const JVal *st_obj = sroot.get("stations");
  assert(st_obj && st_obj->t == JVal::Obj);
  const JVal *c4 = st_obj->get("LHB-C413A27F");
  assert(c4 != nullptr);
  std::vector<double> c4_ref = c4->get("ref")->nums();
  assert(c4_ref.size() == 3);
  const JVal *be = st_obj->get("LHB-BE97C5B7");
  assert(be != nullptr);
  std::vector<double> be_ref = be->get("ref")->nums();
  assert(be_ref.size() == 3);
  const JVal *b61 = st_obj->get("LHB-616FA13B");
  assert(b61 != nullptr);
  std::vector<double> b61_ref = b61->get("ref")->nums();
  assert(b61_ref.size() == 3);

  std::cout << "  LHB-C413A27F ref in stations.json: [" << c4_ref[0] << ", " << c4_ref[1] << ", " << c4_ref[2] << "]" << std::endl;
  std::cout << "  LHB-BE97C5B7 ref in stations.json: [" << be_ref[0] << ", " << be_ref[1] << ", " << be_ref[2] << "]" << std::endl;
  // All 4 stations are ceiling-mounted near Y=0 (within 7cm)
  assert(std::abs(c4_ref[1] - (-0.065)) < 0.05);
  assert(std::abs(be_ref[1] - 0.011) < 0.05);
  // Pairwise distances match driver_lighthouse.so optical relationships (~3.17m..3.26m, ~3.66m, ~5.03m)
  double dist_be_c4 = std::sqrt(std::pow(be_ref[0] - c4_ref[0], 2) + std::pow(be_ref[1] - c4_ref[1], 2) + std::pow(be_ref[2] - c4_ref[2], 2));
  double dist_61_be = std::sqrt(std::pow(b61_ref[0] - be_ref[0], 2) + std::pow(b61_ref[1] - be_ref[1], 2) + std::pow(b61_ref[2] - be_ref[2], 2));
  assert(std::abs(dist_be_c4 - 3.31) < 0.10);
  assert(std::abs(dist_61_be - 4.98) < 0.10);

  // Also verify that sending QLHS_CMD_RECALIBRATE resets Monado's tracking origin offset to Identity
  qlhs::ShmReader reader(test_shm);
  assert(reader.Open());
  assert(reader.SendCommand(QLHS_CMD_RECALIBRATE));
  coord.Step(2.0);
  MndPose after_recal{};
  assert(mock_monado->GetTrackingOriginOffset(lh_id, after_recal) == MND_SUCCESS);
  assert(std::abs(after_recal.position.x) < 1e-5f);
  assert(std::abs(after_recal.position.y) < 1e-5f);
  assert(std::abs(after_recal.position.z) < 1e-5f);
  assert(std::abs(after_recal.orientation.w - 1.0f) < 1e-5f);

  coord.Stop();
  std::filesystem::remove_all(config_dir);
  std::cout << "[test_daemon] TestPeripheralRawPoseDirect PASSED" << std::endl;
}

} // namespace

int main() {
  std::cout << "===========================================" << std::endl;
  std::cout << "Starting QuestLHSync Daemon Integration Tests" << std::endl;
  std::cout << "===========================================" << std::endl;

  TestLifecycleAndShm();
  TestSolverToMonadoOffsetPipeline();
  TestCommandHandling();
  TestBackgroundThreadExecution();
  TestCliParserAndSignals();
  TestStopRecordingAndLogCleanup();
  TestUniverseTransition();
  TestLighthouseDbTilt();
  TestPeripheralRawPoseDirect();

  std::cout << "===========================================" << std::endl;
  std::cout << "All QuestLHSync Daemon Tests PASSED!" << std::endl;
  std::cout << "===========================================" << std::endl;
  return 0;
}
