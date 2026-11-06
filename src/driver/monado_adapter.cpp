#include "driver/monado_adapter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>

// -----------------------------------------------------------------------------
// Coordinate & Quaternion Conversions
// -----------------------------------------------------------------------------

Quat ToQuat(const XrQuaternionf &q) {
  return Quat{static_cast<double>(q.w), static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z)};
}

XrQuaternionf ToXrQuat(const Quat &q) {
  return XrQuaternionf{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)};
}

V3 ToV3(const XrVector3f &v) {
  return V3{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

XrVector3f ToXrV3(const V3 &v) {
  return XrVector3f{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Transform ToTransform(const XrPosef &pose) {
  return Transform{true, ToQuat(pose.orientation), ToV3(pose.position)};
}

XrPosef ToXrPose(const Transform &xf) {
  return XrPosef{ToXrQuat(xf.q), ToXrV3(xf.t)};
}

Quat ToQuat(const MndQuaternion &q) {
  return Quat{static_cast<double>(q.w), static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z)};
}

MndQuaternion ToMndQuat(const Quat &q) {
  return MndQuaternion{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)};
}

V3 ToV3(const MndVector3 &v) {
  return V3{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

MndVector3 ToMndV3(const V3 &v) {
  return MndVector3{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Transform ToTransform(const MndPose &pose) {
  return Transform{true, ToQuat(pose.orientation), ToV3(pose.position)};
}

MndPose ToMndPose(const Transform &xf) {
  return MndPose{ToMndQuat(xf.q), ToMndV3(xf.t)};
}

MndPose ToMndPose(const XrPosef &pose) {
  return MndPose{
      MndQuaternion{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
      MndVector3{pose.position.x, pose.position.y, pose.position.z}};
}

XrPosef ToXrPose(const MndPose &pose) {
  return XrPosef{
      XrQuaternionf{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
      XrVector3f{pose.position.x, pose.position.y, pose.position.z}};
}

V3 TransformPoint(const Transform &xf, const V3 &p) {
  return (ToM3(xf.q) * p) + xf.t;
}

Transform Compose(const Transform &parent, const Transform &child) {
  Transform out;
  out.active = parent.active && child.active;
  out.q = parent.q * child.q;
  out.t = parent.t + (ToM3(parent.q) * child.t);
  return out;
}

Transform Inverse(const Transform &xf) {
  Quat inv_q{xf.q.w, -xf.q.x, -xf.q.y, -xf.q.z};
  V3 inv_t = ToM3(inv_q) * (V3{0, 0, 0} - xf.t);
  return Transform{xf.active, inv_q, inv_t};
}

// -----------------------------------------------------------------------------
// MonadoIpcClient
// -----------------------------------------------------------------------------

MonadoIpcClient::MonadoIpcClient(const std::string &custom_lib_path)
    : custom_lib_path_(custom_lib_path) {}

MonadoIpcClient::~MonadoIpcClient() {
  Disconnect();
}

bool MonadoIpcClient::Connect() {
  if (IsConnected()) return true;

  std::vector<std::string> candidate_paths;
  if (!custom_lib_path_.empty()) {
    candidate_paths.push_back(custom_lib_path_);
  } else {
    const char *env_path = std::getenv("MONADO_LIB_PATH");
    if (env_path && *env_path) {
      candidate_paths.push_back(env_path);
    }
    candidate_paths.push_back("libmonado.so");
    candidate_paths.push_back("libmonado_wivrn.so");
    candidate_paths.push_back("/usr/lib/wivrn/libmonado_wivrn.so");
    candidate_paths.push_back("/usr/lib64/wivrn/libmonado_wivrn.so");
    candidate_paths.push_back("/usr/local/lib/libmonado.so");
    candidate_paths.push_back("/usr/lib/libmonado.so");
    candidate_paths.push_back("/usr/lib/x86_64-linux-gnu/libmonado.so");
  }

  void *handle = nullptr;
  for (const auto &path : candidate_paths) {
    handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle) break;
  }

  if (!handle) {
    return false;
  }

  dl_handle_ = handle;

  pfn_create_ = reinterpret_cast<PFN_mnd_root_create>(dlsym(dl_handle_, "mnd_root_create"));
  pfn_destroy_ = reinterpret_cast<PFN_mnd_root_destroy>(dlsym(dl_handle_, "mnd_root_destroy"));
  pfn_get_origin_count_ = reinterpret_cast<PFN_mnd_root_get_tracking_origin_count>(dlsym(dl_handle_, "mnd_root_get_tracking_origin_count"));
  pfn_get_origin_name_ = reinterpret_cast<PFN_mnd_root_get_tracking_origin_name>(dlsym(dl_handle_, "mnd_root_get_tracking_origin_name"));
  pfn_get_origin_offset_ = reinterpret_cast<PFN_mnd_root_get_tracking_origin_offset>(dlsym(dl_handle_, "mnd_root_get_tracking_origin_offset"));
  pfn_set_origin_offset_ = reinterpret_cast<PFN_mnd_root_set_tracking_origin_offset>(dlsym(dl_handle_, "mnd_root_set_tracking_origin_offset"));

  if (!pfn_create_ || !pfn_destroy_ || !pfn_get_origin_count_ ||
      !pfn_get_origin_name_ || !pfn_get_origin_offset_ || !pfn_set_origin_offset_) {
    Disconnect();
    return false;
  }

  int res = pfn_create_(&root_);
  if (res != MND_SUCCESS || !root_) {
    root_ = nullptr;
    Disconnect();
    return false;
  }

  return true;
}

void MonadoIpcClient::Disconnect() {
  if (root_ && pfn_destroy_) {
    pfn_destroy_(&root_);
    root_ = nullptr;
  }
  if (dl_handle_) {
    dlclose(dl_handle_);
    dl_handle_ = nullptr;
  }
  pfn_create_ = nullptr;
  pfn_destroy_ = nullptr;
  pfn_get_origin_count_ = nullptr;
  pfn_get_origin_name_ = nullptr;
  pfn_get_origin_offset_ = nullptr;
  pfn_set_origin_offset_ = nullptr;
}

bool MonadoIpcClient::IsConnected() const {
  return root_ != nullptr;
}

int MonadoIpcClient::GetTrackingOriginCount(uint32_t &out_count) {
  if (!IsConnected() || !pfn_get_origin_count_) return MND_ERROR_OPERATION_FAILED;
  return pfn_get_origin_count_(root_, &out_count);
}

int MonadoIpcClient::GetTrackingOriginName(uint32_t origin_id, std::string &out_name) {
  if (!IsConnected() || !pfn_get_origin_name_) return MND_ERROR_OPERATION_FAILED;
  const char *name_ptr = nullptr;
  int res = pfn_get_origin_name_(root_, origin_id, &name_ptr);
  if (res == MND_SUCCESS && name_ptr) {
    out_name = name_ptr;
  }
  return res;
}

int MonadoIpcClient::GetTrackingOriginOffset(uint32_t origin_id, MndPose &out_offset) {
  if (!IsConnected() || !pfn_get_origin_offset_) return MND_ERROR_OPERATION_FAILED;
  return pfn_get_origin_offset_(root_, origin_id, &out_offset);
}

int MonadoIpcClient::SetTrackingOriginOffset(uint32_t origin_id, const MndPose &offset) {
  if (!IsConnected() || !pfn_set_origin_offset_) return MND_ERROR_OPERATION_FAILED;
  return pfn_set_origin_offset_(root_, origin_id, &offset);
}

static std::string ToLower(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
  return str;
}

int MonadoIpcClient::FindTrackingOriginByName(const std::string &substring, uint32_t &out_id) {
  if (!IsConnected()) return MND_ERROR_OPERATION_FAILED;
  uint32_t count = 0;
  int res = GetTrackingOriginCount(count);
  if (res != MND_SUCCESS) return res;

  std::string lower_sub = ToLower(substring);
  for (uint32_t i = 0; i < count; i++) {
    std::string name;
    if (GetTrackingOriginName(i, name) == MND_SUCCESS) {
      if (ToLower(name).find(lower_sub) != std::string::npos) {
        out_id = i;
        return MND_SUCCESS;
      }
    }
  }
  return MND_ERROR_INVALID_VALUE;
}

// -----------------------------------------------------------------------------
// MockMonadoIpc
// -----------------------------------------------------------------------------

MockMonadoIpc::MockMonadoIpc() = default;

bool MockMonadoIpc::Connect() {
  connected_ = true;
  return true;
}

void MockMonadoIpc::Disconnect() {
  connected_ = false;
}

bool MockMonadoIpc::IsConnected() const {
  return connected_;
}

uint32_t MockMonadoIpc::AddTrackingOrigin(const std::string &name, const MndPose &initial_offset) {
  origins_.push_back({name, initial_offset});
  return static_cast<uint32_t>(origins_.size() - 1);
}

void MockMonadoIpc::ClearOrigins() {
  origins_.clear();
}

int MockMonadoIpc::GetTrackingOriginCount(uint32_t &out_count) {
  if (!connected_) return MND_ERROR_OPERATION_FAILED;
  out_count = static_cast<uint32_t>(origins_.size());
  return MND_SUCCESS;
}

int MockMonadoIpc::GetTrackingOriginName(uint32_t origin_id, std::string &out_name) {
  if (!connected_) return MND_ERROR_OPERATION_FAILED;
  if (origin_id >= origins_.size()) return MND_ERROR_INVALID_VALUE;
  out_name = origins_[origin_id].name;
  return MND_SUCCESS;
}

int MockMonadoIpc::GetTrackingOriginOffset(uint32_t origin_id, MndPose &out_offset) {
  if (!connected_) return MND_ERROR_OPERATION_FAILED;
  if (origin_id >= origins_.size()) return MND_ERROR_INVALID_VALUE;
  out_offset = origins_[origin_id].offset;
  return MND_SUCCESS;
}

int MockMonadoIpc::SetTrackingOriginOffset(uint32_t origin_id, const MndPose &offset) {
  if (!connected_) return MND_ERROR_OPERATION_FAILED;
  if (origin_id >= origins_.size()) return MND_ERROR_INVALID_VALUE;
  origins_[origin_id].offset = offset;
  return MND_SUCCESS;
}

int MockMonadoIpc::FindTrackingOriginByName(const std::string &substring, uint32_t &out_id) {
  if (!connected_) return MND_ERROR_OPERATION_FAILED;
  std::string lower_sub = ToLower(substring);
  for (size_t i = 0; i < origins_.size(); i++) {
    if (ToLower(origins_[i].name).find(lower_sub) != std::string::npos) {
      out_id = static_cast<uint32_t>(i);
      return MND_SUCCESS;
    }
  }
  return MND_ERROR_INVALID_VALUE;
}
