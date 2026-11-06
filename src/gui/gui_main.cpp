#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "common/qlhs_shm.h"
#include "common/qlhs_status.h"
#include "daemon/daemon_coordinator.h"
#include "gui/dashboard.h"
#include "gui/launcher.h"

namespace {

static volatile sig_atomic_t g_signal_received = 0;

void SignalHandler(int signum) {
  g_signal_received = signum;
}

void PrintUsage(const char *prog_name) {
  std::cout << "QuestLHSync Desktop Dashboard & Unified Launcher\n"
            << "Usage: " << prog_name << " [options]\n\n"
            << "Options:\n"
            << "  --headless, --daemon     Run synchronization engine without GUI\n"
            << "  --config-dir <dir>       Path to configuration and state directory\n"
            << "  --shm <name>             POSIX shared memory name (default: /questlhsync)\n"
            << "  --host <ip>              Headset host IP\n"
            << "  --preferred <serial>     Preferred headset serial number\n"
            << "  --streamer <name>        Streamer name (default: WiVRn)\n"
            << "  --origin <name>          Monado tracking origin name (default: lighthouse)\n"
            << "  --rate <hz>              Telemetry publish rate in Hz (default: 50)\n"
            << "  --no-link                Disable network discovery/streaming client\n"
            << "  --verbose, -v            Enable verbose diagnostic output\n"
            << "  --help, -h               Show this help message\n";
}

} // namespace

int main(int argc, char *argv[]) {
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);

  bool run_headless = false;
  qlhs::DaemonConfig cfg;
  cfg.config_dir = qlhs::GetDefaultConfigDir();

  std::string error_msg;
  auto parse_res = qlhs::ParseDaemonConfig(argc, argv, cfg, &run_headless, &error_msg);
  if (parse_res == qlhs::CliParseResult::Help) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (parse_res == qlhs::CliParseResult::Error) {
    std::cerr << error_msg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // --- Headless / Daemon Execution Mode ---
  if (run_headless) {
    cfg.headless = true;
    qlhs::DaemonCoordinator coordinator(cfg);

    if (!coordinator.Initialize()) {
      std::cerr << "Failed to initialize QuestLHSync headless daemon coordinator.\n";
      return 1;
    }

    coordinator.Run(&g_signal_received);
    coordinator.Stop();
    return 0;
  }

  // --- GUI Mode: Ensure Daemon is Running ---
  const std::string &shm_name = cfg.shm_name;
  if (!qlhs::IsDaemonActive(shm_name)) {
    std::string daemon_bin = qlhs::FindDaemonBinary(argv[0]);
    std::cout << "No active QuestLHSync daemon found on " << shm_name
              << ". Auto-spawning daemon: " << daemon_bin << "\n";
    std::vector<std::string> daemon_args = {"--headless"};
    if (!cfg.config_dir.empty()) {
      daemon_args.push_back("--config-dir");
      daemon_args.push_back(cfg.config_dir);
    }
    if (cfg.shm_name != QLHS_DEFAULT_SHM_NAME) {
      daemon_args.push_back("--shm");
      daemon_args.push_back(cfg.shm_name);
    }
    if (cfg.verbose) {
      daemon_args.push_back("--verbose");
    }

    if (!qlhs::SpawnDaemon(daemon_bin, daemon_args)) {
      std::cerr << "Warning: Failed to fork/spawn background daemon.\n";
    } else {
      // Wait up to 2 seconds for the daemon shared memory to come alive
      for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (qlhs::IsDaemonActive(shm_name)) {
          std::cout << "Connected to newly spawned background daemon.\n";
          break;
        }
      }
    }
  } else {
    std::cout << "Attached to existing active QuestLHSync daemon on " << shm_name << ".\n";
  }

  // --- Initialize GLFW & OpenGL Window ---
  glfwSetErrorCallback([](int error, const char *description) {
    (void)error;
    (void)description;
  });

  if (!glfwInit()) {
    std::cerr << "Error: Unable to initialize GLFW (no display detected). Use --headless to run in terminal mode.\n";
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  glfwWindowHintString(GLFW_X11_CLASS_NAME, "questlhsync");
  glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "questlhsync");
#ifdef GLFW_WAYLAND_APP_ID
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, "questlhsync");
#endif

  GLFWwindow *window = glfwCreateWindow(1280, 800, "QuestLHSync by CreoleVR", nullptr, nullptr);
  if (!window) {
    std::cerr << "Error: Unable to create GLFW window (display unavailable or OpenGL 3.3 unsupported).\n"
              << "Use --headless to run in terminal mode.\n";
    glfwTerminate();
    return 1;
  }

  // Set modern custom application window icon
  {
    constexpr int W = 32;
    constexpr int H = 32;
    std::vector<unsigned char> icon_pixels(W * H * 4, 0);
    const float cx = 15.5f;
    const float cy = 15.5f;
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        float dx = x - cx;
        float dy = y - cy;
        float r = std::sqrt(dx * dx + dy * dy);
        int idx = (y * W + x) * 4;
        if (r <= 15.0f) {
          icon_pixels[idx + 0] = 0x1B;
          icon_pixels[idx + 1] = 0x1E;
          icon_pixels[idx + 2] = 0x24;
          icon_pixels[idx + 3] = 0xFF;
          if (r >= 11.0f && r <= 14.0f) {
            icon_pixels[idx + 0] = 0x2C;
            icon_pixels[idx + 1] = 0x7B;
            icon_pixels[idx + 2] = 0xE5;
            icon_pixels[idx + 3] = 0xFF;
          } else if (r <= 4.5f) {
            icon_pixels[idx + 0] = 0x00;
            icon_pixels[idx + 1] = 0xD0;
            icon_pixels[idx + 2] = 0x84;
            icon_pixels[idx + 3] = 0xFF;
          }
        }
      }
    }
    GLFWimage icon_img;
    icon_img.width = W;
    icon_img.height = H;
    icon_img.pixels = icon_pixels.data();
    glfwSetWindowIcon(window, 1, &icon_img);
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // 60 FPS vsync

  // --- Initialize Dear ImGui ---
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 130");

  // Load clean system font if available
  const char *candidate_fonts[] = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
  };
  for (const char *fp : candidate_fonts) {
    if (std::filesystem::exists(fp)) {
      io.Fonts->AddFontFromFileTTF(fp, 18.0f);
      break;
    }
  }

  qlhs::ShmReader reader(shm_name);
  reader.Open();
  qlhs::Dashboard dashboard;
  QlhsStatus last_status{};
  double last_reopen_try = 0.0;

  // Main UI event loop
  while (!glfwWindowShouldClose(window)) {
    if (g_signal_received != 0) {
      break;
    }
    glfwPollEvents();

    // Read live telemetry from daemon via POSIX shared memory
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;

    QlhsStatus status{};
    if (!reader.IsConnected() && (now - last_reopen_try >= 0.5)) {
      last_reopen_try = now;
      reader.Open();
    }
    if (reader.IsConnected() && reader.Read(status)) {
      last_status = status;
    }
    bool fresh = (last_status.updated > 0.0 && std::abs(now - last_status.updated) <= 3.0);
    if (!fresh && (now - last_reopen_try >= 1.0)) {
      last_reopen_try = now;
      if (reader.Open() && reader.Read(status)) {
        last_status = status;
        fresh = (last_status.updated > 0.0 && std::abs(now - last_status.updated) <= 3.0);
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    dashboard.Render(last_status, fresh, &reader);

    ImGui::Render();
    int display_w = 0, display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.078f, 0.086f, 0.106f, 1.0f); // col::bg (0x14161b)
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }

  // Cleanup GUI resources (closing GUI leaves daemon running!)
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
