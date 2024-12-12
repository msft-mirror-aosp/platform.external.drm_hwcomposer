/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "drmhwc"

#include "DrmDevice.h"

#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cinttypes>
#include <cstdint>
#include <string>

#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmPlane.h"
#include "drm/ResourceManager.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android {

auto DrmDevice::CreateInstance(std::string const &path,
                               ResourceManager *res_man, uint32_t index)
    -> std::unique_ptr<DrmDevice> {
  if (!IsKMSDev(path.c_str())) {
    return {};
  }

  auto device = std::unique_ptr<DrmDevice>(new DrmDevice(res_man, index));

  if (device->Init(path.c_str()) != 0) {
    return {};
  }

  return device;
}

DrmDevice::DrmDevice(ResourceManager *res_man, uint32_t index)
    : index_in_dev_array_(index), res_man_(res_man) {
  drm_fb_importer_ = std::make_unique<DrmFbImporter>(*this);
}

auto DrmDevice::Init(const char *path) -> int {
  /* TODO: Use drmOpenControl here instead */
  fd_ = MakeSharedFd(open(path, O_RDWR | O_CLOEXEC));
  if (!fd_) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): Fixme
    ALOGE("Failed to open dri %s: %s", path, strerror(errno));
    return -ENODEV;
  }

  int ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret != 0) {
    ALOGE("Failed to set universal plane cap %d", ret);
    return ret;
  }

  ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret != 0) {
    ALOGE("Failed to set atomic cap %d", ret);
    return ret;
  }

#ifdef DRM_CLIENT_CAP_WRITEBACK_CONNECTORS
  ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
  if (ret != 0) {
    ALOGI("Failed to set writeback cap %d", ret);
  }
#endif

  uint64_t cap_value = 0;
  if (drmGetCap(*GetFd(), DRM_CAP_ADDFB2_MODIFIERS, &cap_value) != 0) {
    ALOGW("drmGetCap failed. Fallback to no modifier support.");
    cap_value = 0;
  }
  HasAddFb2ModifiersSupport_ = cap_value != 0;

  uint64_t cursor_width = 0;
  uint64_t cursor_height = 0;
  if (drmGetCap(*GetFd(), DRM_CAP_CURSOR_WIDTH, &cursor_width) == 0 &&
      drmGetCap(*GetFd(), DRM_CAP_CURSOR_HEIGHT, &cursor_height) == 0) {
    cap_cursor_size_ = std::pair<uint64_t, uint64_t>(cursor_width,
                                                     cursor_height);
  }

  drmSetMaster(*GetFd());
  if (drmIsMaster(*GetFd()) == 0) {
    ALOGE("DRM/KMS master access required");
    return -EACCES;
  }

  auto res = MakeDrmModeResUnique(*GetFd());
  if (!res) {
    ALOGE("Failed to get DrmDevice resources");
    return -ENODEV;
  }

  min_resolution_ = std::pair<uint32_t, uint32_t>(res->min_width,
                                                  res->min_height);
  max_resolution_ = std::pair<uint32_t, uint32_t>(res->max_width,
                                                  res->max_height);

  for (int i = 0; i < res->count_crtcs; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto crtc = DrmCrtc::CreateInstance(*this, res->crtcs[i], i);
    if (crtc) {
      crtcs_.emplace_back(std::move(crtc));
    }
  }

  for (int i = 0; i < res->count_encoders; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto enc = DrmEncoder::CreateInstance(*this, res->encoders[i], i);
    if (enc) {
      encoders_.emplace_back(std::move(enc));
    }
  }

  for (int i = 0; i < res->count_connectors; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto conn = DrmConnector::CreateInstance(*this, res->connectors[i], i);

    if (!conn) {
      continue;
    }

    if (conn->IsWriteback()) {
      writeback_connectors_.emplace_back(std::move(conn));
    } else {
      connectors_.emplace_back(std::move(conn));
    }
  }

  auto plane_res = MakeDrmModePlaneResUnique(*GetFd());
  if (!plane_res) {
    ALOGE("Failed to get plane resources");
    return -ENOENT;
  }

  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto plane = DrmPlane::CreateInstance(*this, plane_res->planes[i]);

    if (plane) {
      planes_.emplace_back(std::move(plane));
    }
  }

  return 0;
}

auto DrmDevice::RegisterUserPropertyBlob(void *data, size_t length) const
    -> DrmModeUserPropertyBlobUnique {
  struct drm_mode_create_blob create_blob {};
  create_blob.length = length;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  create_blob.data = (__u64)data;

  auto ret = drmIoctl(*GetFd(), DRM_IOCTL_MODE_CREATEPROPBLOB, &create_blob);
  if (ret != 0) {
    ALOGE("Failed to create mode property blob %d", ret);
    return {};
  }

  return DrmModeUserPropertyBlobUnique(
      new uint32_t(create_blob.blob_id), [this](const uint32_t *it) {
        struct drm_mode_destroy_blob destroy_blob {};
        destroy_blob.blob_id = (__u32)*it;
        auto err = drmIoctl(*GetFd(), DRM_IOCTL_MODE_DESTROYPROPBLOB,
                            &destroy_blob);
        if (err != 0) {
          ALOGE("Failed to destroy mode property blob %" PRIu32 "/%d", *it,
                err);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete it;
      });
}

int DrmDevice::GetProperty(uint32_t obj_id, uint32_t obj_type,
                           const char *prop_name, DrmProperty *property) const {
  drmModeObjectPropertiesPtr props = nullptr;

  props = drmModeObjectGetProperties(*GetFd(), obj_id, obj_type);
  if (props == nullptr) {
    ALOGE("Failed to get properties for %d/%x", obj_id, obj_type);
    return -ENODEV;
  }

  bool found = false;
  for (int i = 0; !found && (size_t)i < props->count_props; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    drmModePropertyPtr p = drmModeGetProperty(*GetFd(), props->props[i]);
    if (strcmp(p->name, prop_name) == 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      property->Init(GetFd(), obj_id, p, props->prop_values[i]);
      found = true;
    }
    drmModeFreeProperty(p);
  }

  drmModeFreeObjectProperties(props);
  return found ? 0 : -ENOENT;
}

std::string DrmDevice::GetName() const {
  auto *ver = drmGetVersion(*GetFd());
  if (ver == nullptr) {
    ALOGW("Failed to get drm version for fd=%d", *GetFd());
    return "generic";
  }

  std::string name(ver->name);
  drmFreeVersion(ver);
  return name;
}

auto DrmDevice::IsKMSDev(const char *path) -> bool {
  auto fd = MakeUniqueFd(open(path, O_RDWR | O_CLOEXEC));
  if (!fd) {
    return false;
  }

  auto res = MakeDrmModeResUnique(*fd);
  if (!res) {
    return false;
  }

  auto is_kms = res->count_crtcs > 0 && res->count_connectors > 0 &&
                res->count_encoders > 0;

  return is_kms;
}

auto DrmDevice::GetConnectors()
    -> const std::vector<std::unique_ptr<DrmConnector>> & {
  return connectors_;
}

auto DrmDevice::GetWritebackConnectors()
    -> const std::vector<std::unique_ptr<DrmConnector>> & {
  return writeback_connectors_;
}

auto DrmDevice::GetPlanes() -> const std::vector<std::unique_ptr<DrmPlane>> & {
  return planes_;
}

auto DrmDevice::GetCrtcs() -> const std::vector<std::unique_ptr<DrmCrtc>> & {
  return crtcs_;
}

auto DrmDevice::GetEncoders()
    -> const std::vector<std::unique_ptr<DrmEncoder>> & {
  return encoders_;
}

class DumbBufferFd : public PrimeFdsSharedBase {
 public:
  SharedFd fd;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-goto)
auto DrmDevice::CreateBufferForModeset(uint32_t width, uint32_t height)
    -> std::optional<BufferInfo> {
  constexpr uint32_t kDumbBufferFormat = DRM_FORMAT_XRGB8888;
  constexpr uint32_t kDumbBufferBpp = 32;

  std::optional<BufferInfo> result;
  void *ptr = MAP_FAILED;
  struct drm_mode_create_dumb create = {
      .height = height,
      .width = width,
      .bpp = kDumbBufferBpp,
      .flags = 0,
  };

  int ret = drmIoctl(*fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create);
  if (ret != 0) {
    ALOGE("Failed to DRM_IOCTL_MODE_CREATE_DUMB %d", errno);
    return {};
  }

  struct drm_mode_map_dumb map = {
      .handle = create.handle,
  };

  auto dumb_buffer_fd = std::make_shared<DumbBufferFd>();

  BufferInfo buffer_info = {
      .width = width,
      .height = height,

      .format = kDumbBufferFormat,
      .pitches = {create.pitch},
      .prime_fds = {-1, -1, -1, -1},
      .modifiers = {DRM_FORMAT_MOD_NONE},

      .color_space = BufferColorSpace::kUndefined,
      .sample_range = BufferSampleRange::kUndefined,
      .blend_mode = BufferBlendMode::kNone,

      .fds_shared = dumb_buffer_fd,
  };

  ret = drmIoctl(*fd_, DRM_IOCTL_MODE_MAP_DUMB, &map);
  if (ret != 0) {
    ALOGE("Failed to DRM_IOCTL_MODE_MAP_DUMB %d", errno);
    goto done;
  }

  ptr = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd_,
             (off_t)map.offset);
  if (ptr == MAP_FAILED) {
    ALOGE("Failed to mmap dumb buffer %d", errno);
    goto done;
  }

  memset(ptr, 0, create.size);

  if (munmap(ptr, create.size) != 0) {
    ALOGE("Failed to unmap dumb buffer: %d", errno);
  }

  ret = drmPrimeHandleToFD(*fd_, create.handle, 0, &buffer_info.prime_fds[0]);
  if (ret != 0) {
    ALOGE("Failed to export dumb buffer as FD: %d", errno);
    goto done;
  }

  dumb_buffer_fd->fd = MakeSharedFd(buffer_info.prime_fds[0]);

  result = buffer_info;

done:
  if (create.handle > 0) {
    struct drm_mode_destroy_dumb destroy = {
        .handle = create.handle,
    };
    drmIoctl(*fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }

  return result;
}
// NOLINTEND(cppcoreguidelines-avoid-goto)

}  // namespace android
