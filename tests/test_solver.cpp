#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "../src/driver/mathx.h"
#include "../src/driver/net.h"
#include "../src/driver/sync.h"

namespace {

inline M3 Rx(double a) {
  double c = std::cos(a), s = std::sin(a);
  M3 r;
  r.m[0][0] = 1; r.m[0][1] = 0; r.m[0][2] = 0;
  r.m[1][0] = 0; r.m[1][1] = c; r.m[1][2] = -s;
  r.m[2][0] = 0; r.m[2][1] = s; r.m[2][2] = c;
  return r;
}

// Synthetic Camera calibration JSON with 2 forward/side cameras on Quest Pro
std::string MakeCalibrationJson() {
  // Camera 0: Left side/front, yaw +20 deg, pitch +10 deg, tx -0.05m
  // Camera 1: Right side/front, yaw -20 deg, pitch +10 deg, tx +0.05m
  // Focal length 450px on 1280x1024 sensor gives wide ~110 deg FOV.
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

  std::string json = "{\n"
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
  return json;
}

struct CamInfo {
  int id;
  M3 R_dfc;  // device <- camera
  V3 t_dfc;
  int w = 1280, h = 1024;
  double f = 450.0, fy = 450.0, cx = 640.0, cy = 512.0;

  // Projects direction in camera frame to pixel coordinates
  bool Project(V3 d_cam, double &px, double &py) const {
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

}  // namespace

int main() {
  std::cout << "[test_solver] Starting synthetic 4-DOF solver convergence test..." << std::endl;

  // Temporary directory for state/stations
  std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "qlhs_test_solver";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  SyncConfig cfg;
  cfg.dir = test_dir.string();
  cfg.arrival_clock = true;
  cfg.learn_timing = false;
  cfg.per_headset = false;

  std::vector<std::string> logs;
  auto logfn = [&](const std::string &s) {
    std::cout << "  [Sync] " << s << std::endl;
    logs.push_back(s);
  };

  Sync sync(cfg, logfn);
  sync.SetStreamer("synthetic_streamer");

  std::string calib_json = MakeCalibrationJson();
  std::string err;
  if (!sync.SetCalibration(calib_json, &err)) {
    std::cerr << "Failed to set calibration: " << err << std::endl;
    return 1;
  }

  // Camera parameters for projecting rays
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

  // Ground-truth 4-DOF alignment (yaw, tx, ty, tz)
  // p_quest = Ry(gt_yaw) * p_ref + gt_t
  const double gt_yaw = 0.38;  // ~21.77 deg
  const V3 gt_t = { 0.45, 0.12, -0.35 };

  // Define 3 synthetic base stations in Lighthouse space
  struct BaseStation {
    std::string serial;
    V3 pos_lh;
    M3 rot_lh;
  };
  std::vector<BaseStation> bs_list = {
    { "LHB-00000001", { 0.0, 2.0, 0.0 }, M3() },     // closest to origin => anchor
    { "LHB-00000002", { 2.4, 2.2, 2.6 }, M3() },
    { "LHB-00000003", { -2.2, 2.1, 2.4 }, M3() }
  };

  std::map<std::string, std::pair<V3, M3>> raw_stations;
  for (const auto &bs : bs_list) {
    raw_stations[bs.serial] = { bs.pos_lh, bs.rot_lh };
  }
  sync.SetStationsRaw(raw_stations);

  // Simulation loop: 18 seconds duration
  const double t_end = 18.0;
  const double dt_pose = 1.0 / 90.0;  // 90 Hz HMD poses
  const double dt_cam = 1.0 / 30.0;   // 30 Hz camera frames
  const double dt_tick = 0.05;        // 20 Hz sync tick

  double next_cam = 0.0;
  double next_tick = 0.0;
  unsigned frame_seq = 0;

  // Head motion trajectory generator
  auto get_head_pose = [](double t_val) {
    double yaw = 0.22 * std::sin(0.6 * t_val);
    M3 R = Ry(yaw);
    V3 p = { 0.35 * std::sin(0.45 * t_val), 1.60 + 0.04 * std::sin(0.9 * t_val), 0.35 * std::cos(0.45 * t_val) };
    return std::make_pair(R, p);
  };

  // Pre-seed pose history for the initial window so interpolation is always available
  for (double t = -1.0; t < 0.0; t += dt_pose) {
    auto [R, p] = get_head_pose(t);
    sync.OnHmdPose(t, ToQuat(R), p);
  }

  for (double t = 0.0; t <= t_end; t += dt_pose) {
    auto [R_hmd, p_hmd] = get_head_pose(t);
    sync.OnHmdPose(t, ToQuat(R_hmd), p_hmd);

    // Camera frames: exposure time is t - kExpoArrival (20 ms before arrival)
    if (t >= next_cam) {
      next_cam += dt_cam;
      long long tus = (long long)(t * 1e6);
      double t_expo = t - kExpoArrival;
      auto [R_cam_hmd, p_cam_hmd] = get_head_pose(t_expo);

      for (int c = 0; c < 2; c++) {
        const CamInfo &cam = cams[c];
        // Camera pose in Quest space at optical exposure time
        V3 p_cam_world = p_cam_hmd + R_cam_hmd * cam.t_dfc;
        M3 R_cam_world = R_cam_hmd * cam.R_dfc;

        std::vector<std::pair<double, double>> visible_pixels;

        for (const auto &bs : bs_list) {
          // Base station position in Quest space
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
          sync.OnLine(t, line);
        }
      }
    }

    // Solver tick
    if (t >= next_tick) {
      next_tick += dt_tick;
      sync.Tick(t);
    }
  }

  // Retrieve solver convergence status
  Sync::Status status = sync.GetStatus(t_end);
  Transform xf = sync.Tick(t_end);

  std::cout << "\n--- Final Solver Status ---" << std::endl;
  std::cout << "has_x: " << (status.has_x ? "true" : "false")
            << " | locked: " << (status.locked ? "true" : "false")
            << " | cond: " << (status.cond ? "true" : "false") << std::endl;
  std::cout << "Estimated x: yaw=" << status.x[0] * 180.0 / M_PI << " deg"
            << ", t=[" << status.x[1] << ", " << status.x[2] << ", " << status.x[3] << "]" << std::endl;
  std::cout << "Ground truth: yaw=" << gt_yaw * 180.0 / M_PI << " deg"
            << ", t=[" << gt_t.x << ", " << gt_t.y << ", " << gt_t.z << "]" << std::endl;

  // Assertions
  assert(status.has_x && "Solver must have acquired alignment transform");

  double yaw_err_deg = std::fabs(Wrap(status.x[0] - gt_yaw)) * 180.0 / M_PI;
  V3 t_est = { status.x[1], status.x[2], status.x[3] };
  double pos_err_m = norm(t_est - gt_t);

  std::cout << "Angular error: " << yaw_err_deg << " deg (tolerance < 0.1 deg)" << std::endl;
  std::cout << "Positional error: " << pos_err_m * 100.0 << " cm (tolerance < 1.0 cm)" << std::endl;

  assert(yaw_err_deg < 0.1 && "Angular error must be < 0.1 deg");
  assert(pos_err_m < 0.01 && "Positional error must be < 1.0 cm (< 0.01 m)");

  assert(xf.active && "Applied transform must be active");
  std::cout << "[test_solver] SUCCESS: Solver converged accurately within sub-centimeter tolerances!" << std::endl;

  std::filesystem::remove_all(test_dir);
  return 0;
}
