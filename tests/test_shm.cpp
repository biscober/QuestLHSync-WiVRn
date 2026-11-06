#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "common/qlhs_status.h"
#include "common/qlhs_shm.h"

// Helper to generate a correlated test status struct for iteration `idx`
static QlhsStatus MakeCorrelatedStatus(int idx) {
  QlhsStatus st;
  std::memset(&st, 0, sizeof(st));
  st.magic = QLHS_MAGIC;
  st.version = QLHS_VERSION;
  st.seq = 0; // seqlock manages this
  st.state = static_cast<QlhsState>(idx % 8);
  st.updated = 10000.0 + idx;
  
  std::snprintf(st.hmd, sizeof(st.hmd), "TestHMD-%d", idx);
  std::snprintf(st.headset, sizeof(st.headset), "Quest Pro-%d", idx);
  std::snprintf(st.headset_addr, sizeof(st.headset_addr), "192.168.1.%d", (idx % 250) + 1);
  
  st.cam_fps = 90.0 + (idx % 30);
  st.sight_rate = 120.0 + idx;
  st.spot_rate = 240.0 + idx;
  st.yaw_deg = static_cast<double>(idx);
  st.t[0] = static_cast<double>(idx * 2);
  st.t[1] = static_cast<double>(idx * 3);
  st.t[2] = static_cast<double>(idx * 4);
  st.med_deg = 0.05 + (idx % 100) * 0.001;
  st.nst = 4;
  for (int j = 0; j < 4; ++j) {
    std::snprintf(st.st[j].serial, sizeof(st.st[j].serial), "LHB-%d-%d", idx, j);
    st.st[j].support = idx + j;
    st.st[j].dist = static_cast<double>(idx * 10 + j);
    st.st[j].anchor = (j == 0) ? 1 : 0;
  }
  return st;
}

// Verify that a read QlhsStatus is completely internally consistent (not torn)
static bool VerifyCorrelatedStatus(const QlhsStatus &st, std::string &err) {
  if (st.magic != QLHS_MAGIC) {
    err = "magic mismatch: got " + std::to_string(st.magic);
    return false;
  }
  if (st.version != QLHS_VERSION) {
    err = "version mismatch: got " + std::to_string(st.version);
    return false;
  }
  
  int idx = static_cast<int>(st.yaw_deg);
  if (st.t[0] != static_cast<double>(idx * 2)) {
    err = "torn t[0]: expected " + std::to_string(idx * 2) + ", got " + std::to_string(st.t[0]);
    return false;
  }
  if (st.t[1] != static_cast<double>(idx * 3)) {
    err = "torn t[1]: expected " + std::to_string(idx * 3) + ", got " + std::to_string(st.t[1]);
    return false;
  }
  if (st.t[2] != static_cast<double>(idx * 4)) {
    err = "torn t[2]: expected " + std::to_string(idx * 4) + ", got " + std::to_string(st.t[2]);
    return false;
  }
  if (st.updated != 10000.0 + idx) {
    err = "torn updated timestamp";
    return false;
  }
  if (st.state != static_cast<QlhsState>(idx % 8)) {
    err = "torn state field";
    return false;
  }
  char expected_hmd[64];
  std::snprintf(expected_hmd, sizeof(expected_hmd), "TestHMD-%d", idx);
  if (std::strcmp(st.hmd, expected_hmd) != 0) {
    err = "torn hmd string: expected " + std::string(expected_hmd) + ", got " + std::string(st.hmd);
    return false;
  }
  if (st.nst != 4) {
    err = "torn nst field: got " + std::to_string(st.nst);
    return false;
  }
  for (int j = 0; j < 4; ++j) {
    if (st.st[j].support != idx + j) {
      err = "torn station support";
      return false;
    }
    if (st.st[j].dist != static_cast<double>(idx * 10 + j)) {
      err = "torn station dist";
      return false;
    }
  }
  return true;
}

int main() {
  std::cout << "[TEST] Starting POSIX Shared Memory IPC Unit Test..." << std::endl;
  
  // Test 1: Name Normalization
  std::cout << "[TEST] Checking shm name normalization..." << std::endl;
  assert(qlhs::NormalizeShmName("/questlhsync") == "/questlhsync");
  assert(qlhs::NormalizeShmName("questlhsync") == "/questlhsync");
  assert(qlhs::NormalizeShmName("/dev/shm/questlhsync") == "/questlhsync");
  assert(qlhs::NormalizeShmName("/dev/shm/test_shm") == "/test_shm");
  std::cout << "[TEST] Name normalization passed." << std::endl;

  // Test 2: Client fails to open non-existent shm
  {
    qlhs::ShmReader nonexistent("/nonexistent_shm_12345678");
    assert(!nonexistent.Open());
    assert(!nonexistent.IsConnected());
    assert(!qlhs::ShmReader::IsServerRunning("/nonexistent_shm_12345678"));
  }

  // Test 3: In-place Update lambda & Move semantics
  {
    std::string unit_shm = "/dev/shm/qlhs_unit_test_" + std::to_string(getpid());
    qlhs::ShmPublisher pub(unit_shm);
    assert(pub.Init());
    assert(pub.IsValid());
    assert(qlhs::ShmReader::IsServerRunning(unit_shm));

    pub.Update([](QlhsStatus &st) {
      st.yaw_deg = 42.0;
      st.t[0] = 1.0;
      st.t[1] = 2.0;
      st.t[2] = 3.0;
    });

    qlhs::ShmReader rdr(unit_shm);
    assert(rdr.Open());
    assert(rdr.IsConnected());

    QlhsStatus out;
    assert(rdr.Read(out));
    assert(out.yaw_deg == 42.0);
    assert(out.t[0] == 1.0);

    // Test move semantics
    qlhs::ShmPublisher pub_moved = std::move(pub);
    assert(pub_moved.IsValid());
    assert(!pub.IsValid());

    qlhs::ShmReader rdr_moved = std::move(rdr);
    assert(rdr_moved.IsConnected());
    assert(!rdr.IsConnected());

    pub_moved.Unlink();
  }

  std::string test_shm_name = "/dev/shm/questlhsync_test_" + std::to_string(getpid());

  // Setup pipes for inter-process synchronization
  // pipe_start: parent signals child that publisher is initialized
  // pipe_done: child signals parent that reading is complete
  int pipe_start[2];
  int pipe_done[2];
  if (pipe(pipe_start) != 0 || pipe(pipe_done) != 0) {
    std::cerr << "Failed to create pipes" << std::endl;
    return 1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "Fork failed" << std::endl;
    return 1;
  }

  if (pid == 0) {
    // -------------------------------------------------------------
    // CHILD PROCESS: Reader / Dashboard Client
    // -------------------------------------------------------------
    close(pipe_start[1]);
    close(pipe_done[0]);

    // Wait for parent signal that publisher is ready
    char signal_byte = 0;
    if (read(pipe_start[0], &signal_byte, 1) != 1) {
      std::cerr << "[CHILD] Failed to receive start signal from parent" << std::endl;
      _exit(1);
    }
    close(pipe_start[0]);

    std::cout << "[CHILD] Reader attaching to shared memory " << test_shm_name << "..." << std::endl;
    qlhs::ShmReader reader(test_shm_name);
    if (!reader.Open()) {
      std::cerr << "[CHILD] Failed to open shared memory" << std::endl;
      _exit(2);
    }

    int successful_reads = 0;
    int torn_reads = 0;
    bool sent_pause_cmd = false;
    bool sent_record_on_cmd = false;
    bool sent_record_off_cmd = false;
    bool sent_recalibrate_cmd = false;
    bool sent_resume_cmd = false;

    QlhsStatus status_read;
    std::string err;

    // Read concurrently while parent writes
    // We expect to read many status updates
    int last_idx = -1;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
      if (reader.Read(status_read, 50)) {
        if (!VerifyCorrelatedStatus(status_read, err)) {
          std::cerr << "[CHILD] TORN READ DETECTED: " << err << std::endl;
          torn_reads++;
        } else {
          successful_reads++;
          int current_idx = static_cast<int>(status_read.yaw_deg);
          
          // Send commands at intermediate iterations across all commands
          if (current_idx > 1000 && !sent_pause_cmd) {
            std::cout << "[CHILD] Sending QLHS_CMD_PAUSE command at idx " << current_idx << "..." << std::endl;
            assert(reader.SendCommand(QLHS_CMD_PAUSE));
            sent_pause_cmd = true;
          }
          if (current_idx > 3000 && !sent_record_on_cmd) {
            std::cout << "[CHILD] Sending QLHS_CMD_RECORD_ON command at idx " << current_idx << "..." << std::endl;
            assert(reader.SendCommand(QLHS_CMD_RECORD_ON));
            sent_record_on_cmd = true;
          }
          if (current_idx > 5000 && !sent_record_off_cmd) {
            std::cout << "[CHILD] Sending QLHS_CMD_RECORD_OFF command at idx " << current_idx << "..." << std::endl;
            assert(reader.SendCommand(QLHS_CMD_RECORD_OFF));
            sent_record_off_cmd = true;
          }
          if (current_idx > 7000 && !sent_recalibrate_cmd) {
            std::cout << "[CHILD] Sending QLHS_CMD_RECALIBRATE command at idx " << current_idx << "..." << std::endl;
            assert(reader.SendCommand(QLHS_CMD_RECALIBRATE));
            sent_recalibrate_cmd = true;
          }
          if (current_idx > 9000 && !sent_resume_cmd) {
            std::cout << "[CHILD] Sending QLHS_CMD_RESUME command at idx " << current_idx << "..." << std::endl;
            assert(reader.SendCommand(QLHS_CMD_RESUME));
            sent_resume_cmd = true;
          }

          if (current_idx >= 9999) {
            last_idx = current_idx;
            break;
          }
        }
      }

      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 10) {
        std::cerr << "[CHILD] Timeout waiting for 9999 updates" << std::endl;
        break;
      }
    }

    std::cout << "[CHILD] Finished: successful_reads=" << successful_reads
              << ", torn_reads=" << torn_reads
              << ", last_idx=" << last_idx << std::endl;

    if (torn_reads > 0 || successful_reads < 500 ||
        !sent_pause_cmd || !sent_record_on_cmd || !sent_record_off_cmd ||
        !sent_recalibrate_cmd || !sent_resume_cmd) {
      std::cerr << "[CHILD] Test validation failed!" << std::endl;
      _exit(5);
    }

    // Signal parent that child finished successfully
    signal_byte = 1;
    if (write(pipe_done[1], &signal_byte, 1) != 1) {
      _exit(6);
    }
    close(pipe_done[1]);
    reader.Close();
    _exit(0);

  } else {
    // -------------------------------------------------------------
    // PARENT PROCESS: Writer / Daemon Publisher
    // -------------------------------------------------------------
    close(pipe_start[0]);
    close(pipe_done[1]);

    std::cout << "[PARENT] Creating publisher for " << test_shm_name << "..." << std::endl;
    qlhs::ShmPublisher publisher(test_shm_name);
    if (!publisher.Init()) {
      std::cerr << "[PARENT] Failed to initialize ShmPublisher" << std::endl;
      kill(pid, SIGKILL);
      return 1;
    }

    // Signal child to start reading
    char signal_byte = 1;
    if (write(pipe_start[1], &signal_byte, 1) != 1) {
      std::cerr << "[PARENT] Failed to signal child" << std::endl;
      kill(pid, SIGKILL);
      return 1;
    }
    close(pipe_start[1]);

    bool saw_pause_cmd = false;
    bool saw_record_on_cmd = false;
    bool saw_record_off_cmd = false;
    bool saw_recalibrate_cmd = false;
    bool saw_resume_cmd = false;

    constexpr int kTotalUpdates = 10000;
    std::cout << "[PARENT] Emitting " << kTotalUpdates << " rapid status updates..." << std::endl;

    for (int i = 0; i < kTotalUpdates; ++i) {
      QlhsStatus st = MakeCorrelatedStatus(i);
      publisher.Publish(st);

      // Poll commands sent by child
      QlhsCmd cmd;
      while (publisher.PollCommand(cmd)) {
        if (cmd == QLHS_CMD_PAUSE) {
          std::cout << "[PARENT] Received command QLHS_CMD_PAUSE at iteration " << i << std::endl;
          saw_pause_cmd = true;
        } else if (cmd == QLHS_CMD_RECORD_ON) {
          std::cout << "[PARENT] Received command QLHS_CMD_RECORD_ON at iteration " << i << std::endl;
          saw_record_on_cmd = true;
        } else if (cmd == QLHS_CMD_RECORD_OFF) {
          std::cout << "[PARENT] Received command QLHS_CMD_RECORD_OFF at iteration " << i << std::endl;
          saw_record_off_cmd = true;
        } else if (cmd == QLHS_CMD_RECALIBRATE) {
          std::cout << "[PARENT] Received command QLHS_CMD_RECALIBRATE at iteration " << i << std::endl;
          saw_recalibrate_cmd = true;
        } else if (cmd == QLHS_CMD_RESUME) {
          std::cout << "[PARENT] Received command QLHS_CMD_RESUME at iteration " << i << std::endl;
          saw_resume_cmd = true;
        }
      }

      // Small nano-pause every 100 iterations to allow OS scheduler context switch
      if (i % 100 == 0) {
        std::this_thread::yield();
      }
    }

    // Give child a moment to receive final updates and check commands
    for (int retry = 0; retry < 100 && (!saw_pause_cmd || !saw_record_on_cmd || !saw_record_off_cmd ||
                                        !saw_recalibrate_cmd || !saw_resume_cmd); ++retry) {
      QlhsCmd cmd;
      while (publisher.PollCommand(cmd)) {
        if (cmd == QLHS_CMD_PAUSE) saw_pause_cmd = true;
        if (cmd == QLHS_CMD_RECORD_ON) saw_record_on_cmd = true;
        if (cmd == QLHS_CMD_RECORD_OFF) saw_record_off_cmd = true;
        if (cmd == QLHS_CMD_RECALIBRATE) saw_recalibrate_cmd = true;
        if (cmd == QLHS_CMD_RESUME) saw_resume_cmd = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for child signal
    char done_signal = 0;
    read(pipe_done[0], &done_signal, 1);
    close(pipe_done[0]);

    int child_status = 0;
    waitpid(pid, &child_status, 0);

    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(saw_pause_cmd && "Parent must receive QLHS_CMD_PAUSE from child");
    assert(saw_record_on_cmd && "Parent must receive QLHS_CMD_RECORD_ON from child");
    assert(saw_record_off_cmd && "Parent must receive QLHS_CMD_RECORD_OFF from child");
    assert(saw_recalibrate_cmd && "Parent must receive QLHS_CMD_RECALIBRATE from child");
    assert(saw_resume_cmd && "Parent must receive QLHS_CMD_RESUME from child");
    std::cout << "[PARENT] Multi-process verification succeeded." << std::endl;

    // Verify unlinking
    publisher.Unlink();
    struct stat sb;
    int stat_res = stat(("/dev/shm" + qlhs::NormalizeShmName(test_shm_name)).c_str(), &sb);
    // Should be unlinked (or stat fails / ENOENT)
    assert(stat_res != 0);
    std::cout << "[PARENT] Shared memory cleanly unlinked." << std::endl;
  }

  std::cout << "[TEST] ALL TESTS PASSED SUCCESSFULLY!" << std::endl;
  return 0;
}
