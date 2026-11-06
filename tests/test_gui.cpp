#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "common/qlhs_status.h"
#include "common/qlhs_shm.h"
#include "gui/dashboard.h"
#include "gui/launcher.h"

// Monotonic clock in seconds
static double GetNowSeconds() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

void TestStateText() {
  std::cout << "[TEST] Running TestStateText..." << std::endl;
  QlhsStatus s{};
  s.magic = QLHS_MAGIC;
  s.version = QLHS_VERSION;
  std::string title, sub;
  uint32_t color = 0;

  // Stale
  qlhs::StateText(s, true, title, sub, color);
  assert(color == col::red);
  assert(title.find("not running") != std::string::npos);

  // QLHS_NO_HMD (empty hmd)
  s.state = QLHS_NO_HMD;
  s.hmd[0] = '\0';
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::grey);
  assert(title == "Waiting for a Quest or Steam Frame");
  assert(sub.find("streaming app") != std::string::npos);

  // QLHS_NO_HMD (other hmd)
  std::strcpy(s.hmd, "Valve Index");
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::grey);
  assert(title == "Waiting for a Quest or Steam Frame");
  assert(sub.find("isn't a Quest Pro") != std::string::npos);
  assert(sub.find("WiVRn") != std::string::npos);
  assert(sub.find("SteamVR") == std::string::npos);

  // QLHS_SEARCHING
  s.state = QLHS_SEARCHING;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(title == "Looking for the headset");
  assert(sub.find("lhsyncd") != std::string::npos);

  // QLHS_CONNECTING
  s.state = QLHS_CONNECTING;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(title == "Connecting");

  // QLHS_NO_CAMERAS
  s.state = QLHS_NO_CAMERAS;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::red);
  assert(title == "No camera frames");

  // QLHS_NO_STATIONS
  s.state = QLHS_NO_STATIONS;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(title == "Waiting for base stations");
  assert(sub.find("WiVRn") != std::string::npos);
  assert(sub.find("SteamVR") == std::string::npos);

  // QLHS_ACQUIRING (moving)
  s.state = QLHS_ACQUIRING;
  s.head_still = 0;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(title == "Finding the base stations");
  assert(sub.find("Look around the room") != std::string::npos);

  // QLHS_ACQUIRING (head still 1)
  s.head_still = 1;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(sub.find("headset isn't moving") != std::string::npos);

  // QLHS_ACQUIRING (head still 2)
  s.head_still = 2;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(sub.find("getting your head's motion") != std::string::npos);
  assert(sub.find("WiVRn") != std::string::npos);
  assert(sub.find("SteamVR") == std::string::npos);

  // QLHS_LOCKED (unpaused)
  s.state = QLHS_LOCKED;
  s.paused = 0;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::green);
  assert(title == "Locked");
  assert(sub.find("follow") != std::string::npos);

  // QLHS_LOCKED (paused)
  s.paused = 1;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::amber);
  assert(title == "Locked, corrections paused");
  assert(sub.find("stay where they are") != std::string::npos);

  // QLHS_STARTING / default
  s.state = QLHS_STARTING;
  qlhs::StateText(s, false, title, sub, color);
  assert(color == col::grey);
  assert(title == "Starting");

  std::cout << "[PASS] TestStateText passed successfully." << std::endl;
}

void TestTimeFormatters() {
  std::cout << "[TEST] Running TestTimeFormatters..." << std::endl;

  // Ago tests
  assert(qlhs::Ago(-1.0) == "not yet");
  assert(qlhs::Ago(0.5) == "now");
  assert(qlhs::Ago(1.4) == "now");
  assert(qlhs::Ago(10.0) == "10 s ago");
  assert(qlhs::Ago(120.0) == "2 min ago");
  assert(qlhs::Ago(7200.0) == "2.0 h ago");

  // Dur tests
  assert(qlhs::Dur(45.0) == "45 s");
  assert(qlhs::Dur(300.0) == "5 min");
  assert(qlhs::Dur(7200.0) == "2.0 h");

  std::cout << "[PASS] TestTimeFormatters passed successfully." << std::endl;
}

void TestShmCommandsAndTelemetry() {
  std::cout << "[TEST] Running TestShmCommandsAndTelemetry..." << std::endl;
  const std::string shm_name = "/qlhs_gui_test_ipc";

  qlhs::ShmPublisher publisher(shm_name);
  assert(publisher.Init(true));

  QlhsStatus st{};
  st.magic = QLHS_MAGIC;
  st.version = QLHS_VERSION;
  st.state = QLHS_LOCKED;
  st.cam_fps = 90.0;
  st.yaw_deg = -5.76;
  st.updated = GetNowSeconds();
  std::strcpy(st.headset, "Quest Pro");
  publisher.Publish(st);

  qlhs::ShmReader reader(shm_name);
  assert(reader.Open());
  assert(reader.IsConnected());

  QlhsStatus read_st{};
  assert(reader.Read(read_st));
  assert(read_st.state == QLHS_LOCKED);
  assert(read_st.cam_fps == 90.0);
  assert(std::string(read_st.headset) == "Quest Pro");

  // Test command passing across all commands
  assert(reader.SendCommand(QLHS_CMD_PAUSE));
  QlhsCmd cmd = QLHS_CMD_NONE;
  assert(publisher.PollCommand(cmd));
  assert(cmd == QLHS_CMD_PAUSE);

  assert(reader.SendCommand(QLHS_CMD_RESUME));
  assert(publisher.PollCommand(cmd));
  assert(cmd == QLHS_CMD_RESUME);

  assert(reader.SendCommand(QLHS_CMD_RECORD_ON));
  assert(publisher.PollCommand(cmd));
  assert(cmd == QLHS_CMD_RECORD_ON);

  assert(reader.SendCommand(QLHS_CMD_RECORD_OFF));
  assert(publisher.PollCommand(cmd));
  assert(cmd == QLHS_CMD_RECORD_OFF);

  assert(reader.SendCommand(QLHS_CMD_RECALIBRATE));
  assert(publisher.PollCommand(cmd));
  assert(cmd == QLHS_CMD_RECALIBRATE);

  // Poll with nothing pending
  assert(!publisher.PollCommand(cmd));

  publisher.Unlink();
  std::cout << "[PASS] TestShmCommandsAndTelemetry passed successfully." << std::endl;
}

void TestDaemonDetection() {
  std::cout << "[TEST] Running TestDaemonDetection..." << std::endl;
  const std::string shm_name = "/qlhs_gui_test_detect";

  // Non-existent SHM
  assert(!qlhs::IsDaemonActive(shm_name, 2.0));

  // Initialize SHM with stale timestamp
  {
    qlhs::ShmPublisher pub(shm_name);
    assert(pub.Init(true));
    QlhsStatus st{};
    st.magic = QLHS_MAGIC;
    st.version = QLHS_VERSION;
    st.updated = 100.0; // very stale compared to steady_clock
    pub.Publish(st);

    // Stale check
    assert(!qlhs::IsDaemonActive(shm_name, 2.0));

    // Update with fresh timestamp
    pub.Update([](QlhsStatus &s) {
      s.updated = GetNowSeconds();
    });

    assert(qlhs::IsDaemonActive(shm_name, 2.0));
    pub.Unlink();
  }

  // After unlink, should no longer be active
  assert(!qlhs::IsDaemonActive(shm_name, 2.0));
  std::cout << "[PASS] TestDaemonDetection passed successfully." << std::endl;
}

void TestBinaryFinder() {
  std::cout << "[TEST] Running TestBinaryFinder..." << std::endl;
  std::string bin = qlhs::FindDaemonBinary("./build/questlhsync");
  assert(!bin.empty());
  std::cout << "  Found daemon binary candidate: " << bin << std::endl;
  std::cout << "[PASS] TestBinaryFinder passed successfully." << std::endl;
}

void TestSpawnDaemonDoubleFork() {
  std::cout << "[TEST] Running TestSpawnDaemonDoubleFork..." << std::endl;
  assert(qlhs::SpawnDaemon("/bin/true"));
  assert(qlhs::SpawnDaemon("true", {"--version"}));
  std::cout << "[PASS] TestSpawnDaemonDoubleFork passed successfully." << std::endl;
}

void TestLogRingBuffer() {
  std::cout << "[TEST] Running TestLogRingBuffer..." << std::endl;
  const std::string shm_name = "/qlhs_gui_test_log";

  qlhs::ShmPublisher publisher(shm_name);
  assert(publisher.Init(true));

  QlhsStatus st{};
  st.magic = QLHS_MAGIC;
  st.version = QLHS_VERSION;
  st.nlog = 3;
  std::strcpy(st.log[0], "Line 1");
  std::strcpy(st.log[1], "Line 2");
  std::strcpy(st.log[2], "Line 3");
  st.updated = GetNowSeconds();
  publisher.Publish(st);

  qlhs::ShmReader reader(shm_name);
  assert(reader.Open());

  QlhsStatus read_st{};
  assert(reader.Read(read_st));
  assert(read_st.nlog == 3);
  assert(std::string(read_st.log[0]) == "Line 1");
  assert(std::string(read_st.log[1]) == "Line 2");
  assert(std::string(read_st.log[2]) == "Line 3");

  publisher.Unlink();
  std::cout << "[PASS] TestLogRingBuffer passed successfully." << std::endl;
}

int main() {
  std::cout << "=== Running QuestLHSync GUI & Launcher Unit Tests ===" << std::endl;
  TestStateText();
  TestTimeFormatters();
  TestShmCommandsAndTelemetry();
  TestDaemonDetection();
  TestBinaryFinder();
  TestSpawnDaemonDoubleFork();
  TestLogRingBuffer();
  std::cout << "=== All GUI & Launcher Tests Passed! ===" << std::endl;
  return 0;
}
