#include "driver/openxr_locator.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

// -----------------------------------------------------------------------------
// OpenXrLocator Implementation
// -----------------------------------------------------------------------------

OpenXrLocator::OpenXrLocator() = default;

OpenXrLocator::~OpenXrLocator() {
  Shutdown();
}

bool OpenXrLocator::CheckExtensionSupported(const char *extension_name) {
  uint32_t extension_count = 0;
  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr)) || extension_count == 0) {
    return false;
  }

  std::vector<XrExtensionProperties> extensions(extension_count);
  for (auto &ext : extensions) {
    ext = {};
    ext.type = XR_TYPE_EXTENSION_PROPERTIES;
  }

  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data()))) {
    return false;
  }

  for (const auto &ext : extensions) {
    if (std::strcmp(ext.extensionName, extension_name) == 0) {
      return true;
    }
  }
  return false;
}

void OpenXrLocator::CleanupSpaces() {
  for (auto &dev : devices_) {
    if (dev.space != XR_NULL_HANDLE) {
      xrDestroySpace(dev.space);
      dev.space = XR_NULL_HANDLE;
    }
  }
  devices_.clear();
}

bool OpenXrLocator::Initialize() {
  if (initialized_) return true;

  if (!CheckExtensionSupported(XR_MND_HEADLESS_EXTENSION_NAME) ||
      !CheckExtensionSupported(XR_MNDX_XDEV_SPACE_EXTENSION_NAME)) {
    return false;
  }

  std::vector<const char *> enabled_extensions = {
      XR_MND_HEADLESS_EXTENSION_NAME,
      XR_MNDX_XDEV_SPACE_EXTENSION_NAME,
  };
  if (CheckExtensionSupported(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) {
    enabled_extensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
  }

  XrInstanceCreateInfo instance_info{};
  instance_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
  instance_info.next = nullptr;
  instance_info.createFlags = 0;
  std::strncpy(instance_info.applicationInfo.applicationName, "QuestLHSync", XR_MAX_APPLICATION_NAME_SIZE);
  instance_info.applicationInfo.applicationVersion = 1;
  std::strncpy(instance_info.applicationInfo.engineName, "QuestLHSync", XR_MAX_ENGINE_NAME_SIZE);
  instance_info.applicationInfo.engineVersion = 1;
  instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
  instance_info.enabledApiLayerCount = 0;
  instance_info.enabledApiLayerNames = nullptr;
  instance_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
  instance_info.enabledExtensionNames = enabled_extensions.data();

  if (XR_FAILED(xrCreateInstance(&instance_info, &instance_)) || instance_ == XR_NULL_HANDLE) {
    instance_ = XR_NULL_HANDLE;
    return false;
  }

  // Load extension function pointers
  auto load_proc = [this](const char *name) -> PFN_xrVoidFunction {
    PFN_xrVoidFunction pfn = nullptr;
    xrGetInstanceProcAddr(instance_, name, &pfn);
    return pfn;
  };

  pfn_create_xdev_list_ = reinterpret_cast<PFN_xrCreateXDevListMNDX>(load_proc("xrCreateXDevListMNDX"));
  pfn_get_gen_number_ = reinterpret_cast<PFN_xrGetXDevListGenerationNumberMNDX>(load_proc("xrGetXDevListGenerationNumberMNDX"));
  pfn_enumerate_xdevs_ = reinterpret_cast<PFN_xrEnumerateXDevsMNDX>(load_proc("xrEnumerateXDevsMNDX"));
  pfn_get_xdev_properties_ = reinterpret_cast<PFN_xrGetXDevPropertiesMNDX>(load_proc("xrGetXDevPropertiesMNDX"));
  pfn_destroy_xdev_list_ = reinterpret_cast<PFN_xrDestroyXDevListMNDX>(load_proc("xrDestroyXDevListMNDX"));
  pfn_create_xdev_space_ = reinterpret_cast<PFN_xrCreateXDevSpaceMNDX>(load_proc("xrCreateXDevSpaceMNDX"));
  pfn_convert_timespec_time_ = reinterpret_cast<PFN_xrConvertTimespecTimeToTimeKHR>(load_proc("xrConvertTimespecTimeToTimeKHR"));

  if (!pfn_create_xdev_list_ || !pfn_get_gen_number_ || !pfn_enumerate_xdevs_ ||
      !pfn_get_xdev_properties_ || !pfn_destroy_xdev_list_ || !pfn_create_xdev_space_) {
    Shutdown();
    return false;
  }

  // Query HMD system
  XrSystemGetInfo system_info{};
  system_info.type = XR_TYPE_SYSTEM_GET_INFO;
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (XR_FAILED(xrGetSystem(instance_, &system_info, &system_id_)) || system_id_ == XR_NULL_SYSTEM_ID) {
    Shutdown();
    return false;
  }

  // Headless OpenXR session
  XrSessionCreateInfo session_info{};
  session_info.type = XR_TYPE_SESSION_CREATE_INFO;
  session_info.systemId = system_id_;
  if (XR_FAILED(xrCreateSession(instance_, &session_info, &session_)) || session_ == XR_NULL_HANDLE) {
    Shutdown();
    return false;
  }

  // Create Stage reference space
  XrReferenceSpaceCreateInfo stage_info{};
  stage_info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
  stage_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  stage_info.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  if (XR_FAILED(xrCreateReferenceSpace(session_, &stage_info, &stage_space_)) || stage_space_ == XR_NULL_HANDLE) {
    Shutdown();
    return false;
  }

  // Create standard View reference space (fallback for HMD tracking)
  XrReferenceSpaceCreateInfo view_info{};
  view_info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
  view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  view_info.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  xrCreateReferenceSpace(session_, &view_info, &view_space_);

  initialized_ = true;
  return true;
}

void OpenXrLocator::Shutdown() {
  CleanupSpaces();

  if (xdev_list_ != XR_NULL_HANDLE && pfn_destroy_xdev_list_) {
    pfn_destroy_xdev_list_(xdev_list_);
    xdev_list_ = XR_NULL_HANDLE;
  }
  if (view_space_ != XR_NULL_HANDLE) {
    xrDestroySpace(view_space_);
    view_space_ = XR_NULL_HANDLE;
  }
  if (stage_space_ != XR_NULL_HANDLE) {
    xrDestroySpace(stage_space_);
    stage_space_ = XR_NULL_HANDLE;
  }
  if (session_ != XR_NULL_HANDLE) {
    if (session_running_) {
      xrEndSession(session_);
      session_running_ = false;
    }
    xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
  }
  if (instance_ != XR_NULL_HANDLE) {
    xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
  }

  pfn_create_xdev_list_ = nullptr;
  pfn_get_gen_number_ = nullptr;
  pfn_enumerate_xdevs_ = nullptr;
  pfn_get_xdev_properties_ = nullptr;
  pfn_destroy_xdev_list_ = nullptr;
  pfn_create_xdev_space_ = nullptr;
  pfn_convert_timespec_time_ = nullptr;

  system_id_ = XR_NULL_SYSTEM_ID;
  current_generation_ = 0;
  initialized_ = false;
}

bool OpenXrLocator::IsInitialized() const {
  return initialized_;
}

void OpenXrLocator::PollEvents() {
  if (!initialized_ || instance_ == XR_NULL_HANDLE) return;

  XrEventDataBuffer event_buffer{};
  event_buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
  XrResult res = XR_SUCCESS;
  while ((res = xrPollEvent(instance_, &event_buffer)) == XR_SUCCESS) {
    if (event_buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      auto *sc = reinterpret_cast<XrEventDataSessionStateChanged *>(&event_buffer);
      if (sc->state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo begin_info{};
        begin_info.type = XR_TYPE_SESSION_BEGIN_INFO;
        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(session_, &begin_info))) {
          session_running_ = true;
        }
      } else if (sc->state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(session_);
        session_running_ = false;
      } else if (sc->state == XR_SESSION_STATE_LOSS_PENDING || sc->state == XR_SESSION_STATE_EXITING) {
        Shutdown();
        return;
      }
    }
    event_buffer = {};
    event_buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
  }
  if (res == XR_ERROR_INSTANCE_LOST || res == XR_ERROR_SESSION_LOST) {
    Shutdown();
    return;
  }
}

static bool CheckIsHmd(const std::string &name, const std::string &serial) {
  std::string lower_name = name;
  std::string lower_serial = serial;
  for (char &c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char &c : lower_serial) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (lower_name.find("hmd") != std::string::npos ||
          lower_name.find("headset") != std::string::npos ||
          lower_serial.find("hmd") != std::string::npos);
}

bool OpenXrLocator::UpdateDevices() {
  if (!initialized_ || session_ == XR_NULL_HANDLE) return false;

  PollEvents();

  if (xdev_list_ != XR_NULL_HANDLE) {
    uint64_t gen = 0;
    if (XR_SUCCEEDED(pfn_get_gen_number_(xdev_list_, &gen)) && gen == current_generation_) {
      return true; // No change in connected devices
    }
    CleanupSpaces();
    pfn_destroy_xdev_list_(xdev_list_);
    xdev_list_ = XR_NULL_HANDLE;
  }

  XrCreateXDevListInfoMNDX list_info{};
  list_info.type = XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX;
  if (XR_FAILED(pfn_create_xdev_list_(session_, &list_info, &xdev_list_)) || xdev_list_ == XR_NULL_HANDLE) {
    return false;
  }

  pfn_get_gen_number_(xdev_list_, &current_generation_);

  uint32_t capacity_output = 0;
  if (XR_FAILED(pfn_enumerate_xdevs_(xdev_list_, 0, &capacity_output, nullptr)) || capacity_output == 0) {
    return true; // 0 devices currently active
  }

  std::vector<XrXDevIdMNDX> xdev_ids(capacity_output);
  if (XR_FAILED(pfn_enumerate_xdevs_(xdev_list_, capacity_output, &capacity_output, xdev_ids.data()))) {
    return false;
  }
  xdev_ids.resize(capacity_output);

  for (const auto &id : xdev_ids) {
    XrGetXDevInfoMNDX get_info{};
    get_info.type = XR_TYPE_GET_XDEV_INFO_MNDX;
    get_info.xdevId = id;

    XrXDevPropertiesMNDX props{};
    props.type = XR_TYPE_XDEV_PROPERTIES_MNDX;
    if (XR_FAILED(pfn_get_xdev_properties_(xdev_list_, &get_info, &props))) {
      continue;
    }

    LocatedDevice dev;
    dev.dev_id = id;
    dev.serial = props.serial;
    dev.name = props.name;
    dev.can_create_space = (props.canCreateSpace == XR_TRUE);
    dev.is_hmd = CheckIsHmd(dev.name, dev.serial);

    if (dev.can_create_space) {
      XrCreateXDevSpaceInfoMNDX space_info{};
      space_info.type = XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX;
      space_info.xdevList = xdev_list_;
      space_info.xdevId = id;
      space_info.offset = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

      if (XR_SUCCEEDED(pfn_create_xdev_space_(session_, &space_info, &dev.space))) {
        devices_.push_back(dev);
      }
    } else {
      devices_.push_back(dev);
    }
  }

  return true;
}

size_t OpenXrLocator::GetDeviceCount() const {
  return devices_.size();
}

XrTime OpenXrLocator::ConvertTimeToXrTime(double monotonic_sec) {
  if (pfn_convert_timespec_time_ && instance_ != XR_NULL_HANDLE) {
    struct timespec ts;
    if (monotonic_sec > 0.0) {
      ts.tv_sec = static_cast<time_t>(monotonic_sec);
      ts.tv_nsec = static_cast<long>((monotonic_sec - static_cast<double>(ts.tv_sec)) * 1e9);
    } else {
      clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    XrTime xr_time = 0;
    if (XR_SUCCEEDED(pfn_convert_timespec_time_(instance_, &ts, &xr_time))) {
      return xr_time;
    }
  }
  return 0;
}

bool OpenXrLocator::LocateDevice(const std::string &serial, Transform &out_transform, bool &out_valid, XrTime time) {
  out_valid = false;
  if (!initialized_ || stage_space_ == XR_NULL_HANDLE) return false;

  if (time == 0) {
    time = ConvertTimeToXrTime(0.0);
    if (time == 0) return false;
  }

  for (const auto &dev : devices_) {
    if (dev.serial == serial) {
      if (dev.space == XR_NULL_HANDLE) return false;

      XrSpaceLocation loc{};
      loc.type = XR_TYPE_SPACE_LOCATION;
      XrResult res = xrLocateSpace(dev.space, stage_space_, time, &loc);
      if (res == XR_ERROR_INSTANCE_LOST || res == XR_ERROR_SESSION_LOST) {
        Shutdown();
        return false;
      }
      if (XR_FAILED(res)) {
        return false;
      }

      constexpr XrSpaceLocationFlags kValidFlags =
          XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
      out_valid = ((loc.locationFlags & kValidFlags) == kValidFlags);
      out_transform = ToTransform(loc.pose);
      return true;
    }
  }

  return false;
}

bool OpenXrLocator::LocateHmd(Transform &out_transform, bool &out_valid, XrTime time) {
  out_valid = false;
  if (!initialized_ || stage_space_ == XR_NULL_HANDLE) return false;

  if (time == 0) {
    time = ConvertTimeToXrTime(0.0);
    if (time == 0) return false;
  }

  // 1. Try MNDX xdev space for HMD
  for (const auto &dev : devices_) {
    if (dev.is_hmd && dev.space != XR_NULL_HANDLE) {
      if (LocateDevice(dev.serial, out_transform, out_valid, time) && out_valid) {
        return true;
      }
    }
  }

  // 2. Fallback to standard OpenXR VIEW space
  if (view_space_ != XR_NULL_HANDLE) {
    XrSpaceLocation loc{};
    loc.type = XR_TYPE_SPACE_LOCATION;
    XrResult res = xrLocateSpace(view_space_, stage_space_, time, &loc);
    if (res == XR_ERROR_INSTANCE_LOST || res == XR_ERROR_SESSION_LOST) {
      Shutdown();
      return false;
    }
    if (XR_SUCCEEDED(res)) {
      constexpr XrSpaceLocationFlags kValidFlags =
          XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
      out_valid = ((loc.locationFlags & kValidFlags) == kValidFlags);
      if (out_valid) {
        out_transform = ToTransform(loc.pose);
        return true;
      }
    }
  }

  return false;
}

std::vector<LocatedDevice> OpenXrLocator::GetDevices() const {
  return devices_;
}

// -----------------------------------------------------------------------------
// MockOpenXrLocator Implementation
// -----------------------------------------------------------------------------

MockOpenXrLocator::MockOpenXrLocator() = default;

bool MockOpenXrLocator::Initialize() {
  initialized_ = true;
  return true;
}

void MockOpenXrLocator::Shutdown() {
  initialized_ = false;
  mock_devices_.clear();
}

bool MockOpenXrLocator::IsInitialized() const {
  return initialized_;
}

void MockOpenXrLocator::PollEvents() {}

bool MockOpenXrLocator::UpdateDevices() {
  return true;
}

size_t MockOpenXrLocator::GetDeviceCount() const {
  return mock_devices_.size();
}

bool MockOpenXrLocator::LocateDevice(const std::string &serial, Transform &out_transform, bool &out_valid, XrTime /*time*/) {
  out_valid = false;
  if (!initialized_) return false;
  auto it = mock_devices_.find(serial);
  if (it == mock_devices_.end()) return false;
  out_transform = it->second.pose;
  out_valid = it->second.valid;
  return true;
}

bool MockOpenXrLocator::LocateHmd(Transform &out_transform, bool &out_valid, XrTime time) {
  for (const auto &pair : mock_devices_) {
    if (pair.second.dev.is_hmd) {
      return LocateDevice(pair.first, out_transform, out_valid, time);
    }
  }
  return false;
}

std::vector<LocatedDevice> MockOpenXrLocator::GetDevices() const {
  std::vector<LocatedDevice> list;
  for (const auto &pair : mock_devices_) {
    list.push_back(pair.second.dev);
  }
  return list;
}

void MockOpenXrLocator::AddMockDevice(const std::string &serial, const std::string &name, bool is_hmd, const Transform &pose) {
  LocatedDevice dev;
  dev.dev_id = mock_devices_.size() + 1;
  dev.serial = serial;
  dev.name = name;
  dev.can_create_space = true;
  dev.is_hmd = is_hmd;
  mock_devices_[serial] = {dev, pose, true};
}

void MockOpenXrLocator::SetMockDevicePose(const std::string &serial, const Transform &pose, bool valid) {
  auto it = mock_devices_.find(serial);
  if (it != mock_devices_.end()) {
    it->second.pose = pose;
    it->second.valid = valid;
  }
}

void MockOpenXrLocator::ClearMockDevices() {
  mock_devices_.clear();
}
