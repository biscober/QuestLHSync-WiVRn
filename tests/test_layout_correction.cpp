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

std::string MakeCalibrationJson() {
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
  M3 R_dfc;
  V3 t_dfc;
  int w = 1280, h = 1024;
  double f = 450.0, fy = 450.0, cx = 640.0, cy = 512.0;

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

struct StationDef {
  std::string serial;
  V3 physical_pos;
  V3 stale_db_pos;
  int channel;
};

}  // namespace

int main() {
  std::cout << "[test_layout_correction] Starting 4-station moved-station repro test..." << std::endl;

  std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "qlhs_test_layout_corr";
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
  assert(sync.SetCalibration(calib_json, &err));

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

  const double gt_yaw = 0.20;
  const V3 gt_t = { 0.10, 0.05, -0.20 };

  // Real 4-station constellation from the user's room:
  // Levelled physical positions vs unlevelled 35-deg tilted positions previously cached in stations.json.
  std::vector<StationDef> stations = {
    { "LHB-616FA13B", {  2.256455,  0.060973, -1.682035 }, {  2.254044, 1.132511, -1.259310 }, 3 },
    { "LHB-AF02B1DE", {  0.000000,  0.000000,  0.000000 }, {  0.000000, 0.000000,  0.000000 }, 4 },
    { "LHB-BE97C5B7", { -1.498118, -0.043095, -3.816288 }, { -1.475268, 2.153285, -3.153534 }, 1 },
    { "LHB-C413A27F", {  1.005012, -0.440262, -4.749210 }, {  1.006266, 2.382068, -4.060961 }, 2 }
  };

  M3 R_616, R_AF, R_BE, R_C4;
  R_616.m[0][0] = 0.568906; R_616.m[0][1] = -0.441048; R_616.m[0][2] = 0.694135;
  R_616.m[1][0] = -0.030152; R_616.m[1][1] = 0.832279; R_616.m[1][2] = 0.553536;
  R_616.m[2][0] = -0.821850; R_616.m[2][1] = -0.335839; R_616.m[2][2] = 0.460190;

  R_AF.m[0][0] = 0.997749; R_AF.m[0][1] = 0.055489; R_AF.m[0][2] = 0.037663;
  R_AF.m[1][0] = -0.067064; R_AF.m[1][1] = 0.825547; R_AF.m[1][2] = 0.560335;
  R_AF.m[2][0] = 0.000000; R_AF.m[2][1] = -0.561599; R_AF.m[2][2] = 0.827409;

  R_BE.m[0][0] = -0.767630; R_BE.m[0][1] = 0.352699; R_BE.m[0][2] = -0.535114;
  R_BE.m[1][0] = -0.051621; R_BE.m[1][1] = 0.798212; R_BE.m[1][2] = 0.600161;
  R_BE.m[2][0] = 0.638810; R_BE.m[2][1] = 0.488325; R_BE.m[2][2] = -0.594525;

  R_C4.m[0][0] = -0.818760; R_C4.m[0][1] = -0.279463; R_C4.m[0][2] = 0.501531;
  R_C4.m[1][0] = -0.007491; R_C4.m[1][1] = 0.878665; R_C4.m[1][2] = 0.477381;
  R_C4.m[2][0] = -0.574087; R_C4.m[2][1] = 0.387103; R_C4.m[2][2] = -0.721509;

  // Seed stations.json with the stale unlevelled coordinates first
  std::map<std::string, std::pair<V3, M3>> stale_stations = {
    { "LHB-616FA13B", { stations[0].stale_db_pos, R_616 } },
    { "LHB-AF02B1DE", { stations[1].stale_db_pos, R_AF } },
    { "LHB-BE97C5B7", { stations[2].stale_db_pos, R_BE } },
    { "LHB-C413A27F", { stations[3].stale_db_pos, R_C4 } }
  };
  std::map<std::string, int> channels;
  for (const auto &s : stations) {
    channels[s.serial] = s.channel;
  }

  sync.SetStationsRaw(stale_stations);
  sync.SetChannels(channels);
  sync.Tick(1.0);

  // Now feed the properly levelled raw stations from PollLighthouseDb
  std::map<std::string, std::pair<V3, M3>> raw_stations = {
    { "LHB-616FA13B", { stations[0].physical_pos, R_616 } },
    { "LHB-AF02B1DE", { stations[1].physical_pos, R_AF } },
    { "LHB-BE97C5B7", { stations[2].physical_pos, R_BE } },
    { "LHB-C413A27F", { stations[3].physical_pos, R_C4 } }
  };
  sync.SetStationsRaw(raw_stations);

  const double t_end = 15.0;
  const double dt_pose = 1.0 / 90.0;
  const double dt_cam = 1.0 / 30.0;
  const double dt_tick = 0.05;

  double next_cam = 0.0;
  double next_tick = 0.0;
  unsigned frame_seq = 0;

  auto get_head_pose = [](double t_val) {
    // Head rotates around Y while moving slightly in the room center (y = -1.1m below ceiling stations)
    double yaw = 0.8 * t_val;
    M3 R = Ry(yaw);
    V3 p = { 0.3 * std::sin(0.5 * t_val), -1.10 + 0.05 * std::sin(0.7 * t_val), 0.3 * std::cos(0.5 * t_val) - 2.2 };
    return std::make_pair(R, p);
  };

  for (double t = -1.0; t < 0.0; t += dt_pose) {
    auto [R, p] = get_head_pose(t);
    sync.OnHmdPose(t, ToQuat(R), p);
  }

  for (double t = 0.0; t <= t_end; t += dt_pose) {
    auto [R_hmd, p_hmd] = get_head_pose(t);
    sync.OnHmdPose(t, ToQuat(R_hmd), p_hmd);

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

        for (const auto &bs : stations) {
          // Camera sees physical_pos!
          V3 p_station_quest = Ry(gt_yaw) * bs.physical_pos + gt_t;
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

    if (t >= next_tick) {
      next_tick += dt_tick;
      sync.Tick(t);
    }
  }

  Sync::Status status = sync.GetStatus(t_end);
  std::cout << "\n=== Results after 25s simulation ===" << std::endl;
  std::cout << "has_x: " << status.has_x << " locked: " << status.locked << std::endl;
  std::cout << "Stations seen:" << std::endl;
  assert(status.has_x == 1);
  assert(status.locked == 1);
  assert(status.st.size() == 4);
  for (const auto &s : status.st) {
    std::cout << "  " << s.serial << ": support=" << s.support << " dist=" << s.dist
              << " measured=" << s.measured << " last_seen=" << s.last_seen << std::endl;
    assert(s.support > 50);
    assert(s.last_seen >= 0.0 && s.last_seen < 10.0);
  }
  assert(std::abs(status.x[0] - gt_yaw) < 0.01);
  assert(norm(V3{status.x[1], status.x[2], status.x[3]} - gt_t) < 0.02);

  std::cout << "\n[PASS] All 4 base stations acquired and sighted with high support!" << std::endl;
  std::filesystem::remove_all(test_dir);
  return 0;
}
