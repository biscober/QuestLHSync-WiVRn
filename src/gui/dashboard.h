#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include "common/qlhs_status.h"

namespace col {
constexpr uint32_t bg     = 0x14161b;
constexpr uint32_t card   = 0x1d2027;
constexpr uint32_t card2  = 0x252932;
constexpr uint32_t line   = 0x30343e;
constexpr uint32_t text   = 0xeceef2;
constexpr uint32_t dim    = 0x9aa1ad;
constexpr uint32_t faint  = 0x6b7280;
constexpr uint32_t green  = 0x3ccf6e;
constexpr uint32_t amber  = 0xffb224;
constexpr uint32_t red    = 0xff5f57;
constexpr uint32_t blue   = 0x5b8cff;
constexpr uint32_t grey   = 0x8e93a0;
} // namespace col

namespace qlhs {

class ShmReader;

// Formats state title, subtitle, and color based on status and staleness.
void StateText(const QlhsStatus &s, bool stale, std::string &title, std::string &sub, uint32_t &color);

// Time ago helper (e.g. "now", "12 s ago", "5 min ago", "1.2 h ago", "not yet").
std::string Ago(double s);

// Duration helper (e.g. "45 s", "12 min", "1.5 h").
std::string Dur(double s);

// Dashboard renderer class for Dear ImGui.
class Dashboard {
public:
  Dashboard() = default;
  ~Dashboard() = default;

  void SetConfigDir(std::string dir) { config_dir_ = std::move(dir); }
  const std::string &GetConfigDir() const { return config_dir_; }

  // Renders the full 1280x800 dashboard interface using Dear ImGui.
  // Sends commands via `reader` if provided and interactive buttons are pressed.
  void Render(const QlhsStatus &status, bool is_connected_and_fresh, ShmReader *reader = nullptr);

private:
  std::string config_dir_;
  char host_buf_[64] = "";
  bool config_loaded_ = false;
  std::string feedback_msg_;
  double feedback_until_ = 0.0;
};

} // namespace qlhs
