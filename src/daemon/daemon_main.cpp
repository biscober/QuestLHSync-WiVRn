#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "daemon/daemon_coordinator.h"

namespace {

static volatile sig_atomic_t g_signal_received = 0;

void SignalHandler(int signum) {
  g_signal_received = signum;
}

void PrintUsage(const char *prog_name) {
  std::cout << "QuestLHSync Linux Synchronization Daemon (questlhsyncd)\n"
            << "Usage: " << prog_name << " [options]\n\n"
            << "Options:\n"
            << "  --headless               Run headless without desktop UI (default)\n"
            << "  --verbose, -v            Enable verbose diagnostic stdout output\n"
            << "  --config-dir <dir>       Path to configuration and state directory\n"
            << "                           (default: $XDG_CONFIG_HOME/questlhsync)\n"
            << "  --shm <name>             POSIX shared memory name (default: /questlhsync)\n"
            << "  --host <ip>              Headset host IP (can be repeated or comma-separated)\n"
            << "  --preferred <serial>     Preferred headset serial number\n"
            << "  --streamer <name>        Streamer name (default: WiVRn)\n"
            << "  --origin <name>          Monado tracking origin name (default: lighthouse)\n"
            << "  --rate <hz>              Telemetry publish rate in Hz (default: 50)\n"
            << "  --no-link                Disable network discovery/streaming client\n"
            << "  --help, -h               Show this help message\n";
}

} // namespace

int main(int argc, char *argv[]) {
  // Install signal handlers early for graceful shutdown
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);

  qlhs::DaemonConfig cfg;
  cfg.config_dir = qlhs::GetDefaultConfigDir();
  cfg.headless = true;

  std::string error_msg;
  auto parse_res = qlhs::ParseDaemonConfig(argc, argv, cfg, nullptr, &error_msg);
  if (parse_res == qlhs::CliParseResult::Help) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (parse_res == qlhs::CliParseResult::Error) {
    std::cerr << error_msg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  std::cout << "[questlhsyncd] Starting QuestLHSync daemon on Linux (WiVRn / Monado)..." << std::endl;
  qlhs::DaemonCoordinator coordinator(cfg);

  if (!coordinator.Initialize()) {
    std::cerr << "[questlhsyncd] Error: Failed to initialize daemon coordinator." << std::endl;
    return 1;
  }

  coordinator.Run(&g_signal_received);
  coordinator.Stop();

  std::cout << "[questlhsyncd] Daemon shut down cleanly." << std::endl;
  return 0;
}
