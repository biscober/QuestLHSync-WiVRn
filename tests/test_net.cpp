#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/driver/net.h"
#include "../src/driver/sync.h"

namespace {

// Test QuestFamily identification logic
void TestQuestFamily() {
  std::cout << "[test_net] Testing QuestFamily parser..." << std::endl;
  assert(QuestFamily("Meta Quest Pro") == "Quest Pro");
  assert(QuestFamily("Oculus Quest Pro") == "Quest Pro");
  assert(QuestFamily("seacliff") == "Quest Pro");
  assert(QuestFamily("Quest 3") == "Quest 3");
  assert(QuestFamily("eureka") == "Quest 3");
  assert(QuestFamily("CREOLECAST-QUEST3S") == "Quest 3");
  assert(QuestFamily("Steam Frame") == "Steam Frame");
  assert(QuestFamily("deckard") == "Steam Frame");
  assert(QuestFamily("Unknown HMD") == "");
}

// Test POSIX monotonic clock
void TestMonotonicClock() {
  std::cout << "[test_net] Testing QpcNow monotonic clock..." << std::endl;
  double t1 = QpcNow();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  double t2 = QpcNow();
  double dt = t2 - t1;
  assert(dt >= 0.045 && dt <= 0.150);
}

// Minimal valid CameraCalibration JSON for test
std::string SimpleCalibJson() {
  return "{\n"
         "  \"CameraCalibration\": [\n"
         "    {\n"
         "      \"Id\": \"0\",\n"
         "      \"ImageSize\": [1280, 1024],\n"
         "      \"DeviceFromCamera\": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],\n"
         "      \"Projection\": { \"Coefficients\": [450.0, 640.0, 512.0] },\n"
         "      \"Distortion\": { \"Coefficients\": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0] }\n"
         "    }\n"
         "  ],\n"
         "  \"Device\": { \"DeviceType\": \"Quest Pro\" }\n"
         "}\n";
}

// Mock lhsyncd daemon: listens for UDP broadcast discovery on 47281 and accepts TCP on 47280
void TestUdpDiscoveryAndTcpStreaming() {
  std::cout << "[test_net] Testing POSIX UDP discovery (47281) and TCP streaming (47280)..." << std::endl;

  std::atomic<bool> stop_mock{false};
  std::atomic<bool> mock_ready{false};
  std::atomic<bool> mock_failed{false};

  std::thread mock_server([&]() {
    // 1. Setup UDP listener
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);
    int opt = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    sockaddr_in udp_addr{};
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons(47281);
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp_fd, (sockaddr *)&udp_addr, sizeof(udp_addr)) != 0) {
      std::cerr << "Mock UDP bind failed: " << strerror(errno) << std::endl;
      close(udp_fd);
      mock_failed = true;
      return;
    }

    // 2. Setup TCP listener
    int tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(tcp_listen_fd >= 0);
    setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    sockaddr_in tcp_addr{};
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(47280);
    tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(tcp_listen_fd, (sockaddr *)&tcp_addr, sizeof(tcp_addr)) != 0) {
      std::cerr << "Mock TCP bind failed: " << strerror(errno) << std::endl;
      close(udp_fd);
      close(tcp_listen_fd);
      mock_failed = true;
      return;
    }
    listen(tcp_listen_fd, 1);

    mock_ready = true;

    // Wait for UDP "QLHS?"
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    char buf[256];

    // Set 2s timeout on UDP recv
    timeval tv{2, 0};
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!stop_mock) {
      int n = recvfrom(udp_fd, buf, sizeof(buf) - 1, 0, (sockaddr *)&client_addr, &client_len);
      if (n > 0) {
        buf[n] = 0;
        if (strstr(buf, "QLHS?") != nullptr) {
          const char resp[] = "QLHS 1 TEST0001 47280 Quest_Pro";
          sendto(udp_fd, resp, (int)strlen(resp), 0, (sockaddr *)&client_addr, client_len);
          break;
        }
      }
    }
    close(udp_fd);

    if (stop_mock) {
      close(tcp_listen_fd);
      return;
    }

    // Accept TCP connection
    setsockopt(tcp_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int client_tcp = accept(tcp_listen_fd, (sockaddr *)&client_addr, &client_len);
    close(tcp_listen_fd);
    if (client_tcp < 0) return;

    // Send Hello line
    std::string hello = "H QuestLHSync 1 serial=TEST0001 model=Quest_Pro fw=1.0 module=test\n";
    send(client_tcp, hello.data(), hello.size(), 0);

    // Send Calibration line + JSON
    std::string calib = SimpleCalibJson();
    std::string calib_hdr = "C " + std::to_string(calib.size()) + " calib/test\n";
    send(client_tcp, calib_hdr.data(), calib_hdr.size(), 0);
    send(client_tcp, calib.data(), calib.size(), 0);

    // Send one camera frame line
    std::string frame = "F 0 1 1000000 10 0\n";
    send(client_tcp, frame.data(), frame.size(), 0);

    // Handle pings
    while (!stop_mock) {
      fd_set rf;
      FD_ZERO(&rf);
      FD_SET(client_tcp, &rf);
      timeval t_sel{0, 50000};
      if (select(client_tcp + 1, &rf, nullptr, nullptr, &t_sel) > 0) {
        int n = recv(client_tcp, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        unsigned seq = 0;
        long long pc_ns = 0;
        if (sscanf(buf, "P %u %lld", &seq, &pc_ns) == 2) {
          char q[128];
          long long mono_ns = 1000000000ULL;
          int qlen = snprintf(q, sizeof(q), "Q %u %lld %lld\n", seq, pc_ns, mono_ns);
          send(client_tcp, q, qlen, 0);
        }
      }
    }
    close(client_tcp);
  });

  // Wait for mock server to be listening (with timeout)
  for (int i = 0; i < 400; ++i) {
    if (mock_ready || mock_failed) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (mock_failed || !mock_ready) {
    std::cerr << "[test_net] Warning: Mock server failed to bind (port in use on host). Skipping network loop test." << std::endl;
    stop_mock = true;
    mock_server.join();
    return;
  }

  // Set up Sync and HeadsetLink
  SyncConfig cfg;
  cfg.dir = "/tmp";
  Sync sync(cfg, [](const std::string &) {});

  HeadsetLink link(&sync,
                   [](const std::string &msg) { std::cout << "    [HeadsetLink log] " << msg << std::endl; },
                   [](double, const std::string &) {});

  link.SetHosts({ "127.0.0.1" });
  link.SetFamily("Quest Pro");
  link.SetWanted(true);
  link.Start();

  // Wait for connection to establish and Hello handshake to complete
  bool connected = false;
  for (int i = 0; i < 60; i++) {
    if (link.state() == HeadsetLink::kConnected && !link.serial().empty()) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  assert(connected && "HeadsetLink failed to discover and connect over POSIX sockets!");
  assert(link.serial() == "TEST0001");
  assert(link.model() == "Quest Pro");
  assert(link.fw() == "1.0");

  std::cout << "    Connected to " << link.model() << " (" << link.serial() << ") at " << link.addr() << std::endl;

  // Let it receive the frame
  for (int i = 0; i < 20; i++) {
    if (link.last_frame() > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  assert(link.last_frame() > 0 && "Expected to receive at least one camera frame");

  // Clean shutdown
  link.Stop();
  stop_mock = true;
  mock_server.join();

  std::cout << "[test_net] UDP discovery and TCP streaming completed successfully!" << std::endl;
}

}  // namespace

int main() {
  TestQuestFamily();
  TestMonotonicClock();
  TestUdpDiscoveryAndTcpStreaming();
  std::cout << "[test_net] SUCCESS: All networking tests passed!" << std::endl;
  return 0;
}
