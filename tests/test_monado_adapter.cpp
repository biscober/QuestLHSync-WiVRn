#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "driver/mathx.h"
#include "driver/sync.h"
#include "driver/monado_adapter.h"
#include "driver/openxr_locator.h"

static bool Near(double a, double b, double eps = 1e-5) {
  return std::fabs(a - b) <= eps;
}

static bool Near(const V3 &a, const V3 &b, double eps = 1e-5) {
  return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

static bool Near(const Quat &a, const Quat &b, double eps = 1e-5) {
  // Check both q and -q since they represent the same SO(3) rotation
  double d1 = std::fabs(a.w - b.w) + std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z);
  double d2 = std::fabs(a.w + b.w) + std::fabs(a.x + b.x) + std::fabs(a.y + b.y) + std::fabs(a.z + b.z);
  return d1 <= eps || d2 <= eps;
}

void TestMathConversions() {
  std::cout << "[TEST] Running TestMathConversions..." << std::endl;

  // 1. Identity conversion
  Transform ident_xf{true, Quat{1, 0, 0, 0}, V3{0, 0, 0}};
  XrPosef xr_ident = ToXrPose(ident_xf);
  assert(Near(xr_ident.orientation.w, 1.0f));
  assert(Near(xr_ident.orientation.x, 0.0f));
  assert(Near(xr_ident.orientation.y, 0.0f));
  assert(Near(xr_ident.orientation.z, 0.0f));
  assert(Near(xr_ident.position.x, 0.0f));
  assert(Near(xr_ident.position.y, 0.0f));
  assert(Near(xr_ident.position.z, 0.0f));

  Transform back_ident = ToTransform(xr_ident);
  assert(back_ident.active);
  assert(Near(back_ident.q, ident_xf.q));
  assert(Near(back_ident.t, ident_xf.t));

  MndPose mnd_ident = ToMndPose(ident_xf);
  assert(Near(mnd_ident.orientation.w, 1.0f));
  assert(Near(mnd_ident.orientation.x, 0.0f));
  assert(Near(mnd_ident.orientation.y, 0.0f));
  assert(Near(mnd_ident.orientation.z, 0.0f));
  assert(Near(mnd_ident.position.x, 0.0f));
  assert(Near(mnd_ident.position.y, 0.0f));
  assert(Near(mnd_ident.position.z, 0.0f));

  Transform back_mnd_ident = ToTransform(mnd_ident);
  assert(back_mnd_ident.active);
  assert(Near(back_mnd_ident.q, ident_xf.q));
  assert(Near(back_mnd_ident.t, ident_xf.t));

  // 2. 90-degree yaw rotation about +Y axis:
  // angle = pi/2, half angle = pi/4
  // q = [cos(pi/4), 0, sin(pi/4), 0] = [sqrt(0.5), 0, sqrt(0.5), 0]
  double half_angle = kPi / 4.0;
  Quat yaw_90{std::cos(half_angle), 0.0, std::sin(half_angle), 0.0};
  V3 translation{1.25, -0.5, 3.75};
  Transform xf{true, yaw_90, translation};

  // Convert to OpenXR and verify
  XrPosef xr_pose = ToXrPose(xf);
  assert(Near(xr_pose.orientation.w, (float)std::cos(half_angle)));
  assert(Near(xr_pose.orientation.x, 0.0f));
  assert(Near(xr_pose.orientation.y, (float)std::sin(half_angle)));
  assert(Near(xr_pose.orientation.z, 0.0f));
  assert(Near(xr_pose.position.x, 1.25f));
  assert(Near(xr_pose.position.y, -0.5f));
  assert(Near(xr_pose.position.z, 3.75f));

  // Convert back from OpenXR
  Transform xf_from_xr = ToTransform(xr_pose);
  assert(Near(xf_from_xr.q, xf.q));
  assert(Near(xf_from_xr.t, xf.t));

  // Convert to Monado and verify
  MndPose mnd_pose = ToMndPose(xf);
  assert(Near(mnd_pose.orientation.w, (float)std::cos(half_angle)));
  assert(Near(mnd_pose.orientation.x, 0.0f));
  assert(Near(mnd_pose.orientation.y, (float)std::sin(half_angle)));
  assert(Near(mnd_pose.orientation.z, 0.0f));
  assert(Near(mnd_pose.position.x, 1.25f));
  assert(Near(mnd_pose.position.y, -0.5f));
  assert(Near(mnd_pose.position.z, 3.75f));

  // Convert back from Monado
  Transform xf_from_mnd = ToTransform(mnd_pose);
  assert(Near(xf_from_mnd.q, xf.q));
  assert(Near(xf_from_mnd.t, xf.t));

  // Convert between XrPosef and MndPose directly
  MndPose mnd_from_xr = ToMndPose(xr_pose);
  assert(Near(mnd_from_xr.orientation.w, xr_pose.orientation.w));
  assert(Near(mnd_from_xr.orientation.y, xr_pose.orientation.y));
  assert(Near(mnd_from_xr.position.x, xr_pose.position.x));

  XrPosef xr_from_mnd = ToXrPose(mnd_from_xr);
  assert(Near(xr_from_mnd.orientation.w, xr_pose.orientation.w));
  assert(Near(xr_from_mnd.orientation.y, xr_pose.orientation.y));
  assert(Near(xr_from_mnd.position.x, xr_pose.position.x));

  // 3. Point transformation consistency
  V3 point{1.0, 0.0, 0.0};
  // Rotated 90 deg about Y, (1, 0, 0) should become (0, 0, -1) in standard right-handed coordinates
  V3 transformed = TransformPoint(xf, point);
  V3 expected_rot{0.0, 0.0, -1.0};
  V3 expected_total = expected_rot + translation;
  assert(Near(transformed, expected_total, 1e-4));

  std::cout << "[TEST] TestMathConversions PASSED." << std::endl;
}

void TestMockMonadoTrackingOriginOffset() {
  std::cout << "[TEST] Running TestMockMonadoTrackingOriginOffset..." << std::endl;

  MockMonadoIpc mock_ipc;
  assert(!mock_ipc.IsConnected());

  // Add mock tracking origins
  mock_ipc.AddTrackingOrigin("WiVRn HMD Origin");
  mock_ipc.AddTrackingOrigin("Lighthouse Tracking Origin");

  bool ok = mock_ipc.Connect();
  assert(ok);
  assert(mock_ipc.IsConnected());

  // Verify origin count
  uint32_t count = 0;
  assert(mock_ipc.GetTrackingOriginCount(count) == MND_SUCCESS);
  assert(count == 2);

  // Verify origin names
  std::string name0, name1;
  assert(mock_ipc.GetTrackingOriginName(0, name0) == MND_SUCCESS);
  assert(name0 == "WiVRn HMD Origin");
  assert(mock_ipc.GetTrackingOriginName(1, name1) == MND_SUCCESS);
  assert(name1 == "Lighthouse Tracking Origin");

  // Find Lighthouse origin by substring
  uint32_t lh_id = 999;
  assert(mock_ipc.FindTrackingOriginByName("Lighthouse", lh_id) == MND_SUCCESS);
  assert(lh_id == 1);

  // Check initial offset
  MndPose initial_offset{};
  assert(mock_ipc.GetTrackingOriginOffset(1, initial_offset) == MND_SUCCESS);
  assert(Near(initial_offset.orientation.w, 1.0f));
  assert(Near(initial_offset.position.x, 0.0f));

  // Set new offset on Lighthouse origin
  double angle = 0.5; // rad
  Quat test_q{std::cos(angle / 2.0), 0.0, std::sin(angle / 2.0), 0.0};
  V3 test_t{0.15, -0.05, 1.82};
  Transform new_xf{true, test_q, test_t};

  MndPose new_pose = ToMndPose(new_xf);
  assert(mock_ipc.SetTrackingOriginOffset(1, new_pose) == MND_SUCCESS);

  // Retrieve and verify
  MndPose fetched_pose{};
  assert(mock_ipc.GetTrackingOriginOffset(1, fetched_pose) == MND_SUCCESS);
  Transform fetched_xf = ToTransform(fetched_pose);
  assert(Near(fetched_xf.q, test_q));
  assert(Near(fetched_xf.t, test_t));

  // Verify invalid origin ID returns error
  MndPose dummy{};
  assert(mock_ipc.GetTrackingOriginOffset(99, dummy) != MND_SUCCESS);
  assert(mock_ipc.SetTrackingOriginOffset(99, dummy) != MND_SUCCESS);

  mock_ipc.Disconnect();
  assert(!mock_ipc.IsConnected());

  std::cout << "[TEST] TestMockMonadoTrackingOriginOffset PASSED." << std::endl;
}

void TestMonadoIpcGracefulFallback() {
  std::cout << "[TEST] Running TestMonadoIpcGracefulFallback..." << std::endl;

  // Attempting to load from an invalid path must fail gracefully without crash
  MonadoIpcClient bad_client("/path/to/nonexistent/libmonado.so");
  assert(!bad_client.Connect());
  assert(!bad_client.IsConnected());

  uint32_t count = 0;
  assert(bad_client.GetTrackingOriginCount(count) != MND_SUCCESS);

  // Default client (tries standard search paths; if Monado daemon is not active, Connect returns false gracefully)
  MonadoIpcClient default_client;
  bool connected = default_client.Connect();
  if (!connected) {
    std::cout << "  (Monado daemon is not running; verified graceful failure)" << std::endl;
    assert(!default_client.IsConnected());
  } else {
    std::cout << "  (Connected to live Monado instance)" << std::endl;
    uint32_t origins = 0;
    assert(default_client.GetTrackingOriginCount(origins) == MND_SUCCESS);
    default_client.Disconnect();
  }

  std::cout << "[TEST] TestMonadoIpcGracefulFallback PASSED." << std::endl;
}

void TestOpenXrLocatorGracefulFallback() {
  std::cout << "[TEST] Running TestOpenXrLocatorGracefulFallback..." << std::endl;

  OpenXrLocator locator;
  assert(!locator.IsInitialized());

  // In test environment without active OpenXR runtime/HMD, Initialize should fail gracefully
  bool init_ok = locator.Initialize();
  if (!init_ok) {
    std::cout << "  (OpenXR runtime not running or headless extensions unavailable; verified graceful handling)" << std::endl;
    assert(!locator.IsInitialized());
  } else {
    std::cout << "  (OpenXR initialized successfully)" << std::endl;
    locator.UpdateDevices();
    locator.Shutdown();
    assert(!locator.IsInitialized());
  }

  std::cout << "[TEST] TestOpenXrLocatorGracefulFallback PASSED." << std::endl;
}

void TestMockOpenXrLocator() {
  std::cout << "[TEST] Running TestMockOpenXrLocator..." << std::endl;

  MockOpenXrLocator mock_loc;
  assert(mock_loc.Initialize());
  assert(mock_loc.IsInitialized());

  // Add mock HMD and Lighthouse trackers
  Transform hmd_pose{true, Quat{1, 0, 0, 0}, V3{0, 1.7, 0}};
  Transform tracker_pose{true, ToQuat(Ry(0.3)), V3{0.5, 0.1, -0.2}};

  mock_loc.AddMockDevice("WMHD001", "WiVRn HMD", true, hmd_pose);
  mock_loc.AddMockDevice("LHR-12345678", "Vive Tracker 3.0", false, tracker_pose);

  assert(mock_loc.GetDeviceCount() == 2);

  Transform out_hmd;
  bool valid_hmd = false;
  assert(mock_loc.LocateHmd(out_hmd, valid_hmd));
  assert(valid_hmd);
  assert(Near(out_hmd.t, hmd_pose.t));

  Transform out_tracker;
  bool valid_tracker = false;
  assert(mock_loc.LocateDevice("LHR-12345678", out_tracker, valid_tracker));
  assert(valid_tracker);
  assert(Near(out_tracker.t, tracker_pose.t));

  Transform out_missing;
  bool valid_missing = false;
  assert(!mock_loc.LocateDevice("NONEXISTENT", out_missing, valid_missing));

  mock_loc.Shutdown();
  assert(!mock_loc.IsInitialized());

  std::cout << "[TEST] TestMockOpenXrLocator PASSED." << std::endl;
}

int main() {
  std::cout << "=== Running Monado Adapter & OpenXR Locator Tests ===" << std::endl;
  TestMathConversions();
  TestMockMonadoTrackingOriginOffset();
  TestMonadoIpcGracefulFallback();
  TestOpenXrLocatorGracefulFallback();
  TestMockOpenXrLocator();
  std::cout << "=== All tests completed successfully! ===" << std::endl;
  return 0;
}
