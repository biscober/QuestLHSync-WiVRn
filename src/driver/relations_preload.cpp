#include "relations.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::string DefaultConfigDir() {
  const char *cfg_env = std::getenv("QLHS_CONFIG_DIR");
  if (cfg_env && *cfg_env) return cfg_env;
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/questlhsync";
  const char *home = std::getenv("HOME");
  if (home && *home) return std::string(home) + "/.config/questlhsync";
  return "/tmp/questlhsync";
}

class PreloadRunner {
 public:
  PreloadRunner() {
    dir_ = DefaultConfigDir();
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    rel_ = std::make_unique<Relations>(dir_, [this](const std::string &s) { Log(s); });
    run_ = true;
    worker_ = std::thread([this]() { Loop(); });
  }

  ~PreloadRunner() {
    run_ = false;
    if (worker_.joinable()) worker_.join();
    if (rel_) {
      rel_->Unhook();
      rel_->Flush(true);
    }
  }

 private:
  void Log(const std::string &msg) {
    std::lock_guard<std::mutex> g(log_m_);
    std::string path = dir_ + "/questlhsync.log";
    FILE *f = std::fopen(path.c_str(), "a");
    if (!f) return;
    std::time_t now = std::time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char tbuf[16];
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_buf);
    std::fprintf(f, "%s  %s\n", tbuf, msg.c_str());
    std::fclose(f);
  }

  void Loop() {
    bool hooked = false;
    for (int i = 0; run_; ++i) {
      if (!hooked && i < 1200) {  // retry for up to 60s while driver_lighthouse.so loads
        hooked = rel_->Hook();
      }
      rel_->Flush(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  std::string dir_;
  std::mutex log_m_;
  std::unique_ptr<Relations> rel_;
  std::atomic<bool> run_{false};
  std::thread worker_;
};

#if defined(__linux__) && defined(__x86_64__)
__attribute__((constructor)) void InitRelationsPreload() {
  static PreloadRunner runner;
}
#endif

}  // namespace
