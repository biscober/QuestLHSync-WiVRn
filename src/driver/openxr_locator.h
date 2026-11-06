#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include <openxr/openxr.h>

#include "driver/mathx.h"
#include "driver/sync.h"
#include "driver/monado_adapter.h"

// -----------------------------------------------------------------------------
// OpenXR Extension Definitions: XR_MNDX_xdev_space & XR_MND_headless
// -----------------------------------------------------------------------------

#ifndef XR_MND_HEADLESS_EXTENSION_NAME
#define XR_MND_HEADLESS_EXTENSION_NAME "XR_MND_headless"
#endif

#ifndef XR_MNDX_xdev_space
#define XR_MNDX_xdev_space 1
#define XR_MNDX_xdev_space_SPEC_VERSION 1
#define XR_MNDX_XDEV_SPACE_EXTENSION_NAME "XR_MNDX_xdev_space"

#define XR_TYPE_SYSTEM_XDEV_SPACE_PROPERTIES_MNDX ((XrStructureType)1000444001)
#define XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX ((XrStructureType)1000444002)
#define XR_TYPE_GET_XDEV_INFO_MNDX ((XrStructureType)1000444003)
#define XR_TYPE_XDEV_PROPERTIES_MNDX ((XrStructureType)1000444004)
#define XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX ((XrStructureType)1000444005)

typedef uint64_t XrXDevIdMNDX;
XR_DEFINE_HANDLE(XrXDevListMNDX);

typedef struct XrSystemXDevSpacePropertiesMNDX {
  XrStructureType type;
  void *next;
  XrBool32 supportsXDevSpace;
} XrSystemXDevSpacePropertiesMNDX;

typedef struct XrCreateXDevListInfoMNDX {
  XrStructureType type;
  const void *next;
} XrCreateXDevListInfoMNDX;

typedef struct XrGetXDevInfoMNDX {
  XrStructureType type;
  const void *next;
  XrXDevIdMNDX xdevId;
} XrGetXDevInfoMNDX;

typedef struct XrXDevPropertiesMNDX {
  XrStructureType type;
  void *next;
  char name[XR_MAX_SYSTEM_NAME_SIZE];
  char serial[XR_MAX_SYSTEM_NAME_SIZE];
  XrBool32 canCreateSpace;
} XrXDevPropertiesMNDX;

typedef struct XrCreateXDevSpaceInfoMNDX {
  XrStructureType type;
  const void *next;
  XrXDevListMNDX xdevList;
  XrXDevIdMNDX xdevId;
  XrPosef offset;
} XrCreateXDevSpaceInfoMNDX;

typedef XrResult (XRAPI_PTR *PFN_xrCreateXDevListMNDX)(
    XrSession session,
    const XrCreateXDevListInfoMNDX *createInfo,
    XrXDevListMNDX *outXDevList);

typedef XrResult (XRAPI_PTR *PFN_xrGetXDevListGenerationNumberMNDX)(
    XrXDevListMNDX xdevList,
    uint64_t *outGeneration);

typedef XrResult (XRAPI_PTR *PFN_xrEnumerateXDevsMNDX)(
    XrXDevListMNDX xdevList,
    uint32_t xdevCapacityInput,
    uint32_t *xdevCapacityOutput,
    XrXDevIdMNDX *xdevIds);

typedef XrResult (XRAPI_PTR *PFN_xrGetXDevPropertiesMNDX)(
    XrXDevListMNDX xdevList,
    const XrGetXDevInfoMNDX *getInfo,
    XrXDevPropertiesMNDX *outProperties);

typedef XrResult (XRAPI_PTR *PFN_xrDestroyXDevListMNDX)(
    XrXDevListMNDX xdevList);

typedef XrResult (XRAPI_PTR *PFN_xrCreateXDevSpaceMNDX)(
    XrSession session,
    const XrCreateXDevSpaceInfoMNDX *createInfo,
    XrSpace *outSpace);
#endif

#ifndef XR_KHR_convert_timespec_time
#define XR_KHR_convert_timespec_time 1
#define XR_KHR_convert_timespec_time_SPEC_VERSION 1
#define XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME "XR_KHR_convert_timespec_time"
typedef XrResult (XRAPI_PTR *PFN_xrConvertTimespecTimeToTimeKHR)(
    XrInstance instance,
    const struct timespec *unixTime,
    XrTime *time);
#endif

// -----------------------------------------------------------------------------
// OpenXR Device Representation & Locator Interface
// -----------------------------------------------------------------------------

struct LocatedDevice {
  uint64_t dev_id = 0;
  std::string serial;
  std::string name;
  XrSpace space = XR_NULL_HANDLE;
  bool can_create_space = false;
  bool is_hmd = false;
};

class IOpenXrLocator {
public:
  virtual ~IOpenXrLocator() = default;
  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;
  virtual bool IsInitialized() const = 0;
  virtual void PollEvents() = 0;
  virtual bool UpdateDevices() = 0;
  virtual size_t GetDeviceCount() const = 0;
  virtual bool LocateDevice(const std::string &serial, Transform &out_transform, bool &out_valid, XrTime time = 0) = 0;
  virtual bool LocateHmd(Transform &out_transform, bool &out_valid, XrTime time = 0) = 0;
  virtual XrTime ConvertTimeToXrTime(double monotonic_sec) = 0;
  virtual std::vector<LocatedDevice> GetDevices() const = 0;
};

class OpenXrLocator : public IOpenXrLocator {
public:
  OpenXrLocator();
  ~OpenXrLocator() override;

  bool Initialize() override;
  void Shutdown() override;
  bool IsInitialized() const override;

  void PollEvents() override;
  bool UpdateDevices() override;
  size_t GetDeviceCount() const override;
  bool LocateDevice(const std::string &serial, Transform &out_transform, bool &out_valid, XrTime time = 0) override;
  bool LocateHmd(Transform &out_transform, bool &out_valid, XrTime time = 0) override;
  XrTime ConvertTimeToXrTime(double monotonic_sec) override;
  std::vector<LocatedDevice> GetDevices() const override;

  XrInstance GetInstance() const { return instance_; }
  XrSession GetSession() const { return session_; }
  XrSpace GetStageSpace() const { return stage_space_; }
  XrSpace GetViewSpace() const { return view_space_; }

private:
  bool CheckExtensionSupported(const char *extension_name);
  void CleanupSpaces();

  bool initialized_ = false;
  bool session_running_ = false;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace stage_space_ = XR_NULL_HANDLE;
  XrSpace view_space_ = XR_NULL_HANDLE;
  XrXDevListMNDX xdev_list_ = XR_NULL_HANDLE;
  uint64_t current_generation_ = 0;

  // Extension function pointers
  PFN_xrCreateXDevListMNDX pfn_create_xdev_list_ = nullptr;
  PFN_xrGetXDevListGenerationNumberMNDX pfn_get_gen_number_ = nullptr;
  PFN_xrEnumerateXDevsMNDX pfn_enumerate_xdevs_ = nullptr;
  PFN_xrGetXDevPropertiesMNDX pfn_get_xdev_properties_ = nullptr;
  PFN_xrDestroyXDevListMNDX pfn_destroy_xdev_list_ = nullptr;
  PFN_xrCreateXDevSpaceMNDX pfn_create_xdev_space_ = nullptr;
  PFN_xrConvertTimespecTimeToTimeKHR pfn_convert_timespec_time_ = nullptr;

  std::vector<LocatedDevice> devices_;
};

class MockOpenXrLocator : public IOpenXrLocator {
public:
  MockOpenXrLocator();
  ~MockOpenXrLocator() override = default;

  bool Initialize() override;
  void Shutdown() override;
  bool IsInitialized() const override;

  void PollEvents() override;
  bool UpdateDevices() override;
  size_t GetDeviceCount() const override;
  bool LocateDevice(const std::string &serial, Transform &out_transform, bool &out_valid, XrTime time = 0) override;
  bool LocateHmd(Transform &out_transform, bool &out_valid, XrTime time = 0) override;
  XrTime ConvertTimeToXrTime(double monotonic_sec) override {
    return static_cast<XrTime>(monotonic_sec * 1e9);
  }
  std::vector<LocatedDevice> GetDevices() const override;

  // Mock control methods
  void AddMockDevice(const std::string &serial, const std::string &name, bool is_hmd, const Transform &pose);
  void SetMockDevicePose(const std::string &serial, const Transform &pose, bool valid = true);
  void ClearMockDevices();

private:
  struct MockEntry {
    LocatedDevice dev;
    Transform pose;
    bool valid = true;
  };
  bool initialized_ = false;
  std::unordered_map<std::string, MockEntry> mock_devices_;
};
