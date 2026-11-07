#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <openxr/openxr.h>

#include "driver/mathx.h"
#include "driver/sync.h"

// -----------------------------------------------------------------------------
// Monado IPC Types and Constants
// -----------------------------------------------------------------------------

enum MndResult {
  MND_SUCCESS = 0,
  MND_ERROR_INVALID_VERSION = -1,
  MND_ERROR_INVALID_VALUE = -2,
  MND_ERROR_CONNECTING_FAILED = -3,
  MND_ERROR_OPERATION_FAILED = -4,
  MND_ERROR_RECENTERING_NOT_SUPPORTED = -5,
  MND_ERROR_INVALID_PROPERTY = -6,
  MND_ERROR_INVALID_OPERATION = -7,
  MND_ERROR_UNSUPPORTED_OPERATION = -8,
};

enum MndProperty {
  MND_PROPERTY_NAME_STRING = 0,
  MND_PROPERTY_SERIAL_STRING = 1,
  MND_PROPERTY_TRACKING_ORIGIN_U32 = 2,
  MND_PROPERTY_SUPPORTS_POSITION_BOOL = 3,
  MND_PROPERTY_SUPPORTS_ORIENTATION_BOOL = 4,
  MND_PROPERTY_SUPPORTS_BRIGHTNESS_BOOL = 5,
};

enum MndReferenceSpaceType {
  MND_REFERENCE_SPACE_TYPE_VIEW = 0,
  MND_REFERENCE_SPACE_TYPE_LOCAL = 1,
  MND_REFERENCE_SPACE_TYPE_LOCAL_FLOOR = 2,
  MND_REFERENCE_SPACE_TYPE_STAGE = 3,
  MND_REFERENCE_SPACE_TYPE_UNBOUNDED = 4,
};

struct MndQuaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct MndVector3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct MndPose {
  MndQuaternion orientation;
  MndVector3 position;
};

typedef void *mnd_root_ptr;

// Function pointer signatures for libmonado C API
typedef void (*PFN_mnd_api_get_version)(uint32_t *major, uint32_t *minor, uint32_t *patch);
typedef int (*PFN_mnd_root_create)(mnd_root_ptr *out_root);
typedef void (*PFN_mnd_root_destroy)(mnd_root_ptr *root);
typedef int (*PFN_mnd_root_get_tracking_origin_count)(mnd_root_ptr root, uint32_t *out_count);
typedef int (*PFN_mnd_root_get_tracking_origin_name)(mnd_root_ptr root, uint32_t origin_id, const char **out_name);
typedef int (*PFN_mnd_root_get_tracking_origin_offset)(mnd_root_ptr root, uint32_t origin_id, MndPose *out_pose);
typedef int (*PFN_mnd_root_set_tracking_origin_offset)(mnd_root_ptr root, uint32_t origin_id, const MndPose *pose);
typedef int (*PFN_mnd_root_get_reference_space_offset)(mnd_root_ptr root, int type, MndPose *out_pose);
typedef int (*PFN_mnd_root_set_reference_space_offset)(mnd_root_ptr root, int type, const MndPose *pose);

// -----------------------------------------------------------------------------
// Coordinate & Quaternion Conversions
// -----------------------------------------------------------------------------

// OpenXR <-> QuestLHSync (Quat, V3, Transform)
Quat ToQuat(const XrQuaternionf &q);
XrQuaternionf ToXrQuat(const Quat &q);
V3 ToV3(const XrVector3f &v);
XrVector3f ToXrV3(const V3 &v);
Transform ToTransform(const XrPosef &pose);
XrPosef ToXrPose(const Transform &xf);

// Monado <-> QuestLHSync (Quat, V3, Transform)
Quat ToQuat(const MndQuaternion &q);
MndQuaternion ToMndQuat(const Quat &q);
V3 ToV3(const MndVector3 &v);
MndVector3 ToMndV3(const V3 &v);
Transform ToTransform(const MndPose &pose);
MndPose ToMndPose(const Transform &xf);

// Cross Conversions (OpenXR <-> Monado)
MndPose ToMndPose(const XrPosef &pose);
XrPosef ToXrPose(const MndPose &pose);

// Transform rigid helpers
V3 TransformPoint(const Transform &xf, const V3 &p);
Transform Compose(const Transform &parent, const Transform &child);
Transform Inverse(const Transform &xf);

// -----------------------------------------------------------------------------
// Monado IPC Interface & Implementations
// -----------------------------------------------------------------------------

class IMonadoIpc {
public:
  virtual ~IMonadoIpc() = default;
  virtual bool Connect() = 0;
  virtual void Disconnect() = 0;
  virtual bool IsConnected() const = 0;
  virtual int GetTrackingOriginCount(uint32_t &out_count) = 0;
  virtual int GetTrackingOriginName(uint32_t origin_id, std::string &out_name) = 0;
  virtual int GetTrackingOriginOffset(uint32_t origin_id, MndPose &out_offset) = 0;
  virtual int SetTrackingOriginOffset(uint32_t origin_id, const MndPose &offset) = 0;
  virtual int FindTrackingOriginByName(const std::string &substring, uint32_t &out_id) = 0;
  virtual int GetReferenceSpaceOffset(MndReferenceSpaceType type, MndPose &out_offset) = 0;
  virtual int SetReferenceSpaceOffset(MndReferenceSpaceType type, const MndPose &offset) = 0;
};

class MonadoIpcClient : public IMonadoIpc {
public:
  explicit MonadoIpcClient(const std::string &custom_lib_path = "");
  ~MonadoIpcClient() override;

  bool Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;

  int GetTrackingOriginCount(uint32_t &out_count) override;
  int GetTrackingOriginName(uint32_t origin_id, std::string &out_name) override;
  int GetTrackingOriginOffset(uint32_t origin_id, MndPose &out_offset) override;
  int SetTrackingOriginOffset(uint32_t origin_id, const MndPose &offset) override;
  int FindTrackingOriginByName(const std::string &substring, uint32_t &out_id) override;
  int GetReferenceSpaceOffset(MndReferenceSpaceType type, MndPose &out_offset) override;
  int SetReferenceSpaceOffset(MndReferenceSpaceType type, const MndPose &offset) override;

private:
  mutable std::mutex ipc_mutex_;
  std::string custom_lib_path_;
  void *dl_handle_ = nullptr;
  mnd_root_ptr root_ = nullptr;

  PFN_mnd_api_get_version pfn_get_version_ = nullptr;
  PFN_mnd_root_create pfn_create_ = nullptr;
  PFN_mnd_root_destroy pfn_destroy_ = nullptr;
  PFN_mnd_root_get_tracking_origin_count pfn_get_origin_count_ = nullptr;
  PFN_mnd_root_get_tracking_origin_name pfn_get_origin_name_ = nullptr;
  PFN_mnd_root_get_tracking_origin_offset pfn_get_origin_offset_ = nullptr;
  PFN_mnd_root_set_tracking_origin_offset pfn_set_origin_offset_ = nullptr;
  PFN_mnd_root_get_reference_space_offset pfn_get_ref_space_offset_ = nullptr;
  PFN_mnd_root_set_reference_space_offset pfn_set_ref_space_offset_ = nullptr;
};

class MockMonadoIpc : public IMonadoIpc {
public:
  MockMonadoIpc();
  ~MockMonadoIpc() override = default;

  bool Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;

  int GetTrackingOriginCount(uint32_t &out_count) override;
  int GetTrackingOriginName(uint32_t origin_id, std::string &out_name) override;
  int GetTrackingOriginOffset(uint32_t origin_id, MndPose &out_offset) override;
  int SetTrackingOriginOffset(uint32_t origin_id, const MndPose &offset) override;
  int FindTrackingOriginByName(const std::string &substring, uint32_t &out_id) override;
  int GetReferenceSpaceOffset(MndReferenceSpaceType type, MndPose &out_offset) override;
  int SetReferenceSpaceOffset(MndReferenceSpaceType type, const MndPose &offset) override;

  // Mock-specific helpers
  uint32_t AddTrackingOrigin(const std::string &name, const MndPose &initial_offset = MndPose{});
  void ClearOrigins();

private:
  struct OriginEntry {
    std::string name;
    MndPose offset;
  };
  mutable std::mutex mutex_;
  bool connected_ = false;
  std::vector<OriginEntry> origins_;
  std::map<MndReferenceSpaceType, MndPose> ref_spaces_;
};
