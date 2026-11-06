#include "gui/dashboard.h"
#include "common/qlhs_shm.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace qlhs {

static inline ImU32 ToImU32(uint32_t rgb, uint8_t a = 255) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

static inline ImVec4 ToImVec4(uint32_t rgb, float a = 1.0f) {
  return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                ((rgb >> 8) & 0xFF) / 255.0f,
                (rgb & 0xFF) / 255.0f,
                a);
}

template <typename... Args>
static std::string Formatted(const char *fmt, Args... args) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  return std::string(buf);
}

void StateText(const QlhsStatus &s, bool stale, std::string &title, std::string &sub, uint32_t &color) {
  if (stale) {
    title = "Daemon not running";
    sub = "The QuestLHSync daemon isn't updating.";
    color = col::red;
    return;
  }
  switch (s.state) {
    case QLHS_NO_HMD:
      title = "Waiting for a Quest or Steam Frame";
      sub = s.hmd[0] ? "WiVRn's headset isn't a Quest Pro, 3, 3S or Steam Frame, so the lighthouse space is left alone."
                     : "Start your streaming app (WiVRn, ALVR, ...).";
      color = col::grey;
      break;
    case QLHS_SEARCHING:
      title = "Looking for the headset";
      sub = "No headset with QuestLHSync's lhsyncd answers on the network. Is it awake and on the same Wi-Fi?";
      color = col::amber;
      break;
    case QLHS_CONNECTING:
      title = "Connecting";
      sub = "Found the headset, opening the camera stream.";
      color = col::amber;
      break;
    case QLHS_NO_CAMERAS:
      title = "No camera frames";
      sub = "Connected, but the tracking cameras send nothing. Put the headset on (or wake it) and check lhsyncd.";
      color = col::red;
      break;
    case QLHS_NO_STATIONS:
      title = "Waiting for base stations";
      sub = "WiVRn shows base stations only while a lighthouse device (tracker, controller) is on. Turn one on.";
      color = col::amber;
      break;
    case QLHS_ACQUIRING:
      title = "Finding the base stations";
      if (s.head_still == 2) {
        sub = "WiVRn isn't getting your head's motion. Is WiVRn's view in the headset, not a desktop view?";
      } else if (s.head_still) {
        sub = "The headset isn't moving, so its camera frames wait. Put it on and look around the room.";
      } else if (s.nst > 2) {
        sub = Formatted("Look around the room so the cameras catch all %d base stations' flashes.", s.nst);
      } else {
        sub = "Look around the room so the cameras catch the base stations' flashes.";
      }
      color = col::amber;
      break;
    case QLHS_LOCKED:
      title = s.paused ? "Locked, corrections paused" : "Locked";
      sub = s.paused ? "Lighthouse devices stay where they are until you resume."
                     : "Lighthouse devices follow the headset's own tracking.";
      color = s.paused ? col::amber : col::green;
      break;
    default:
      title = "Starting";
      sub = "";
      color = col::grey;
      break;
  }
}

std::string Ago(double s) {
  if (s < 0) return "not yet";
  if (s < 1.5) return "now";
  char b[64];
  if (s < 90) {
    std::snprintf(b, sizeof(b), "%.0f s ago", s);
    return b;
  }
  if (s < 5400) {
    std::snprintf(b, sizeof(b), "%.0f min ago", s / 60.0);
    return b;
  }
  std::snprintf(b, sizeof(b), "%.1f h ago", s / 3600.0);
  return b;
}

std::string Dur(double s) {
  char b[64];
  if (s < 90) {
    std::snprintf(b, sizeof(b), "%.0f s", s);
    return b;
  }
  if (s < 5400) {
    std::snprintf(b, sizeof(b), "%.0f min", s / 60.0);
    return b;
  }
  std::snprintf(b, sizeof(b), "%.1f h", s / 3600.0);
  return b;
}

void Dashboard::Render(const QlhsStatus &s, bool is_connected_and_fresh, ShmReader *reader) {
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ToImVec4(col::bg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

  ImGui::Begin("QuestLHSync Dashboard", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
               ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
               ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetWindowPos();
  const float ox = origin.x;
  const float oy = origin.y;
  ImFont *font = ImGui::GetFont();

  // 1. Header
  const char *title_text = "QuestLHSync";
  draw->AddText(font, 36.0f, ImVec2(ox + 48.0f, oy + 32.0f), ToImU32(col::text), title_text);
  ImVec2 title_sz = font->CalcTextSizeA(36.0f, FLT_MAX, 0.0f, title_text);

  draw->AddText(font, 22.0f, ImVec2(ox + 48.0f + title_sz.x + 14.0f, oy + 46.0f), ToImU32(col::faint), "by CreoleVR");
  draw->AddText(font, 17.0f, ImVec2(ox + 50.0f, oy + 86.0f), ToImU32(col::dim),
                "Lighthouse tracking aligned to your headset, from its own cameras");

  // State Pill (top right)
  std::string pill_title, status_sub;
  uint32_t state_col = col::grey;
  StateText(s, !is_connected_and_fresh, pill_title, status_sub, state_col);

  ImVec2 pill_sz = font->CalcTextSizeA(20.0f, FLT_MAX, 0.0f, pill_title.c_str());
  float pw = pill_sz.x + 64.0f;
  float ph = 46.0f;
  float px = ox + 1280.0f - 48.0f - pw;
  float py = oy + 38.0f;

  draw->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), ToImU32(col::card2), 23.0f);
  draw->AddCircleFilled(ImVec2(px + 24.0f, py + ph / 2.0f), 7.0f, ToImU32(state_col));
  draw->AddText(font, 20.0f, ImVec2(px + 42.0f, py + (ph - pill_sz.y) / 2.0f), ToImU32(col::text), pill_title.c_str());

  // 2. Status summary line
  float bar_y = oy + 124.0f;
  float bar_w = 1280.0f - 96.0f;
  float bar_h = 52.0f;
  draw->AddRectFilled(ImVec2(ox + 48.0f, bar_y), ImVec2(ox + 48.0f + bar_w, bar_y + bar_h), ToImU32(col::card), 12.0f);
  draw->AddRectFilled(ImVec2(ox + 48.0f, bar_y), ImVec2(ox + 54.0f, bar_y + bar_h), ToImU32(state_col), 3.0f);
  draw->AddText(font, 20.0f, ImVec2(ox + 70.0f, bar_y + 16.0f), ToImU32(col::text), status_sub.c_str());

  // 3. Three Cards (Headset, Base Stations, Alignment)
  float top = oy + 196.0f;
  float ch = 300.0f;
  float gap = 20.0f;
  float cw = (1280.0f - 96.0f - 2.0f * gap) / 3.0f; // 381.33px
  float x1 = ox + 48.0f;
  float x2 = x1 + cw + gap;
  float x3 = x2 + cw + gap;

  draw->AddRectFilled(ImVec2(x1, top), ImVec2(x1 + cw, top + ch), ToImU32(col::card), 14.0f);
  draw->AddRectFilled(ImVec2(x2, top), ImVec2(x2 + cw, top + ch), ToImU32(col::card), 14.0f);
  draw->AddRectFilled(ImVec2(x3, top), ImVec2(x3 + cw, top + ch), ToImU32(col::card), 14.0f);

  auto draw_row = [&](float x, float y, const char *k, const std::string &v, uint32_t val_color = col::text) {
    draw->AddText(font, 17.0f, ImVec2(x + 22.0f, y), ToImU32(col::dim), k);
    ImVec2 val_sz = font->CalcTextSizeA(17.0f, FLT_MAX, 0.0f, v.c_str());
    draw->AddText(font, 17.0f, ImVec2(x + cw - 22.0f - val_sz.x, y), ToImU32(val_color), v.c_str());
  };

  // --- Headset Card (x1) ---
  draw->AddText(font, 16.0f, ImVec2(x1 + 22.0f, top + 18.0f), ToImU32(col::faint), "HEADSET");
  const char *headset_name = s.headset[0] ? s.headset : "—";
  draw->AddText(font, 26.0f, ImVec2(x1 + 22.0f, top + 44.0f), ToImU32(col::text), headset_name);

  draw_row(x1, top + 100.0f, "Address", s.headset_addr[0] ? s.headset_addr : "—");
  draw_row(x1, top + 134.0f, "Cameras",
           s.cam_fps > 0.5 ? Formatted("%.0f frames/s", s.cam_fps) : "—",
           (s.cam_fps > 0.5 || s.state < QLHS_NO_CAMERAS) ? col::text : col::red);
  draw_row(x1, top + 168.0f, "Round trip",
           s.rtt_ms > 0 ? Formatted("%.1f ms", s.rtt_ms) : "—");
  draw_row(x1, top + 202.0f, "Streamer", s.hmd_system[0] ? s.hmd_system : "—");
  draw_row(x1, top + 236.0f, "Pose timing",
           s.headset_addr[0] ? Formatted("%.1f ms %s", s.expo_ms, s.expo_learned ? "(learned)" : "(learning)") : "—",
           (s.expo_learned || !s.headset_addr[0]) ? col::text : col::amber);
  draw_row(x1, top + 270.0f, "Bright spots",
           s.cam_fps > 0.5 ? Formatted("%.0f/s, %.0f/s used", s.spot_rate, s.sight_rate) : "—",
           (s.state == QLHS_ACQUIRING && s.sight_rate < 0.5) ? col::amber : col::text);

  // --- Base Stations Card (x2) ---
  bool has_alignment = s.locked || s.nfit > 0 || s.yaw_deg != 0;
  draw->AddText(font, 16.0f, ImVec2(x2 + 22.0f, top + 18.0f), ToImU32(col::faint), "BASE STATIONS");

  if (s.nst == 0) {
    draw->AddText(font, 20.0f, ImVec2(x2 + 22.0f, top + 52.0f), ToImU32(col::dim), "None detected yet");
    draw->AddText(font, 17.0f, ImVec2(x2 + 22.0f, top + 84.0f), ToImU32(col::faint), "They show up while a tracker or");
    draw->AddText(font, 17.0f, ImVec2(x2 + 22.0f, top + 108.0f), ToImU32(col::faint), "controller is switched on.");
  } else {
    for (int i = 0; i < std::min(s.nst, 4); ++i) {
      const QlhsStation &st = s.st[i];
      float y = top + 50.0f + i * 60.0f;
      bool fresh = st.last_seen >= 0 && st.last_seen < 15.0;
      uint32_t dot_color = fresh ? col::green : (st.last_seen >= 0 ? col::amber : col::faint);

      draw->AddCircleFilled(ImVec2(x2 + 30.0f, y + 13.0f), 6.0f, ToImU32(dot_color));
      draw->AddText(font, 19.0f, ImVec2(x2 + 46.0f, y), ToImU32(col::text), st.serial);

      std::string role = st.anchor ? "anchor" : (st.measured ? "measured" : "");
      if (!role.empty()) {
        ImVec2 role_sz = font->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, role.c_str());
        draw->AddText(font, 16.0f, ImVec2(x2 + cw - 22.0f - role_sz.x, y + 3.0f), ToImU32(col::blue), role.c_str());
      }

      std::string info = "seen " + Ago(st.last_seen);
      if (has_alignment) info += Formatted("  ·  %d sightings", st.support);
      if (st.dist > 0) info += Formatted("  ·  %.1f m", st.dist);
      draw->AddText(font, 16.0f, ImVec2(x2 + 46.0f, y + 26.0f), ToImU32(col::dim), info.c_str());
    }
  }

  // --- Alignment Card (x3) ---
  draw->AddText(font, 16.0f, ImVec2(x3 + 22.0f, top + 18.0f), ToImU32(col::faint), "ALIGNMENT");

  std::string med_err_str = has_alignment ? Formatted("%.2f°", s.med_deg >= 0 ? s.med_deg : 0.0) : "—";
  uint32_t med_col = (has_alignment && s.med_deg >= 0 && s.med_deg < 0.3) ? col::green : col::text;
  draw->AddText(font, 26.0f, ImVec2(x3 + 22.0f, top + 44.0f), ToImU32(med_col), med_err_str.c_str());

  if (has_alignment) {
    draw->AddText(font, 17.0f, ImVec2(x3 + 22.0f + 110.0f, top + 54.0f), ToImU32(col::dim), "median sighting error");
  }

  draw_row(x3, top + 100.0f, "Yaw", has_alignment ? Formatted("%+.2f°", s.yaw_deg) : "—");
  draw_row(x3, top + 134.0f, "Offset",
           has_alignment ? Formatted("%.2f, %.2f, %.2f m", s.t[0], s.t[1], s.t[2]) : "—");
  draw_row(x3, top + 168.0f, "Sightings used", has_alignment ? std::to_string(s.nfit) : "—");
  draw_row(x3, top + 202.0f, "Since acquired", s.locked_for >= 0 ? Dur(s.locked_for) : "—");
  draw_row(x3, top + 236.0f, "Settling",
           s.lag_cm > 0.05 ? Formatted("%.1f cm to go", s.lag_cm) : (has_alignment ? "done" : "—"));

  // 4. Log Panel
  float ly = top + ch + 20.0f; // 516.0f
  float lh = 150.0f;
  draw->AddRectFilled(ImVec2(ox + 48.0f, ly), ImVec2(ox + 48.0f + bar_w, ly + lh), ToImU32(col::card), 14.0f);
  draw->AddText(font, 16.0f, ImVec2(ox + 70.0f, ly + 16.0f), ToImU32(col::faint), "LOG");

  uint32_t n = s.nlog;
  int shown = 0;
  for (uint32_t i = 0; i < 5 && i < n; ++i) {
    uint32_t k = n - 1 - i;
    const char *entry = s.log[k % 8];
    draw->AddText(font, 16.0f, ImVec2(ox + 70.0f, ly + 42.0f + 20.0f * i),
                  ToImU32(i == 0 ? col::text : col::dim), entry);
    shown++;
  }
  if (shown == 0) {
    draw->AddText(font, 16.0f, ImVec2(ox + 70.0f, ly + 42.0f), ToImU32(col::dim), "—");
  }

  // 5. Interactive Buttons & Footer
  float by = oy + 800.0f - 66.0f;
  float bh = 46.0f;

  auto render_button = [&](const char *btn_id, float bx, float bw,
                           const char *label, bool primary, bool red_dot, QlhsCmd cmd) {
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    ImGui::InvisibleButton(btn_id, ImVec2(bw, bh));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();

    uint32_t fill = primary ? col::blue : (hovered ? col::line : col::card2);
    draw->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), ToImU32(fill), 10.0f);

    if (red_dot) {
      draw->AddCircleFilled(ImVec2(bx + 26.0f, by + bh / 2.0f), 6.0f, ToImU32(col::red));
    }

    ImVec2 txt_sz = font->CalcTextSizeA(19.0f, FLT_MAX, 0.0f, label);
    float tx = bx + (bw - txt_sz.x) / 2.0f + (red_dot ? 10.0f : 0.0f);
    float ty = by + (bh - txt_sz.y) / 2.0f;
    draw->AddText(font, 19.0f, ImVec2(tx, ty), ToImU32(col::text), label);

    if (clicked && reader) {
      reader->SendCommand(cmd);
    }
  };

  // Button 1: Pause / Resume corrections
  const char *pause_label = s.paused ? "Resume corrections" : "Pause corrections";
  QlhsCmd pause_cmd = s.paused ? QLHS_CMD_RESUME : QLHS_CMD_PAUSE;
  render_button("btn_pause", ox + 48.0f, 240.0f, pause_label, s.paused != 0, false, pause_cmd);

  // Button 2: Record session / Stop recording
  const char *rec_label = s.recording ? "Stop recording" : "Record session";
  QlhsCmd rec_cmd = s.recording ? QLHS_CMD_RECORD_OFF : QLHS_CMD_RECORD_ON;
  render_button("btn_rec", ox + 304.0f, 210.0f, rec_label, false, s.recording != 0, rec_cmd);

  // Button 3: Force recalibrate
  render_button("btn_recal", ox + 530.0f, 240.0f, "Force recalibrate", false, false, QLHS_CMD_RECALIBRATE);

  // Settings note on the right
  const char *settings_note = "Settings: $XDG_CONFIG_HOME/questlhsync";
  ImVec2 set_sz = font->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, settings_note);
  draw->AddText(font, 16.0f, ImVec2(ox + 1280.0f - 48.0f - set_sz.x, by + 14.0f),
                ToImU32(col::faint), settings_note);

  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(1);
}

} // namespace qlhs
