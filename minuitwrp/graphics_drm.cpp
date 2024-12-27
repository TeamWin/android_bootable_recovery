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

/*
 * DRM based mode setting test program
 * Copyright 2008 Tungsten Graphics
 *   Jakob Bornecrantz <jakob@tungstengraphics.com>
 * Copyright 2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/properties.h>

#include <array>
#include <map>
#include <string>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sstream>

#include "minuitwrp/minui.h"
#include "graphics.h"
#include <pixelflinger/pixelflinger.h>

#define ARRAY_SIZE(A) (sizeof(A)/sizeof(*(A)))

struct drm_surface {
    GRSurface base;
    uint32_t fb_id;
    uint32_t handle;
};

#define NUM_MAIN 1
#define NUM_PLANES 4
#define DEFAULT_NUM_LMS 2

#define SPR_INIT_PARAM_SIZE_1 4
#define SPR_INIT_PARAM_SIZE_2 5
#define SPR_INIT_PARAM_SIZE_3 16
#define SPR_INIT_PARAM_SIZE_4 24
#define SPR_INIT_PARAM_SIZE_5 32
#define SPR_INIT_PARAM_SIZE_6 7
enum class SPRPackType {
  kPentile,
  kRGBW,
  kYYGW,
  kYYGM,
  kDelta3,
  kMax = 0xFF,
};
enum class SPRFilterType {
  kPixelDrop,
  kBilinear,
  kFourTap,
  kAdaptive,
  k2DAvg,
  kMax = 0xFF,
};
enum class SPRAdaptiveModeType {
  kYYGM,
  kYYGW,
  kMax = 0xFF,
};
static const std::map<SPRPackType, uint32_t> kDefaultColorPhaseIncrement = {
  { { { SPRPackType::kPentile }, { 8 } },
    { { SPRPackType::kYYGM }, { 6 } },
    { { SPRPackType::kYYGW }, { 6 } },
    { { SPRPackType::kDelta3 }, { 6 } },
    { { SPRPackType::kRGBW }, { 8 } } }
};
static const std::map<SPRPackType, uint32_t> kDefaultColorPhaseRepeat = {
  { { { SPRPackType::kPentile }, { 2 } },
    { { SPRPackType::kYYGM }, { 2 } },
    { { SPRPackType::kYYGW }, { 2 } },
    { { SPRPackType::kDelta3 }, { 2 } },
    { { SPRPackType::kRGBW }, { 2 } } }
};
static const std::map<SPRPackType, std::array<uint16_t, SPR_INIT_PARAM_SIZE_1>> kDecimationRatioMap{
  {
      { { SPRPackType::kPentile }, { 1, 0, 1, 0 } },
      { { SPRPackType::kYYGM }, { 2, 2, 2, 0 } },
      { { SPRPackType::kYYGW }, { 2, 2, 2, 0 } },
      { { SPRPackType::kRGBW }, { 1, 1, 1, 1 } },
  }
};
static const std::map<SPRFilterType, std::array<int16_t, SPR_INIT_PARAM_SIZE_3>>
    kDefaultFilterCoeffsMap{
      { { { SPRFilterType::kPixelDrop }, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { { SPRFilterType::kBilinear },
          { 0, 512, 0, 0, -33, 443, 110, -8, -23, 279, 279, -23, -8, 110, 443, -33 } },
        { { SPRFilterType::kFourTap },
          { 128, 256, 128, 0, 86, 241, 164, 21, 52, 204, 204, 52, 21, 164, 241, 86 } },
        { { SPRFilterType::kAdaptive },
          { 0, 256, 256, 0, 0, 256, 256, 0, 0, 256, 256, 0, 0, 256, 256, 0 } } }
    };
static const std::map<SPRPackType, std::array<int16_t, SPR_INIT_PARAM_SIZE_4>>
    kDefaultColorPhaseMap{
      { { { SPRPackType::kPentile },
          { -2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { { SPRPackType::kYYGM },
          { -3, 0, 0, 0, 0, 0, -1, 2, 1, 1, 0, 0, 1, -2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { { SPRPackType::kYYGW },
          { -4, 2, 0, 0, 0, -1, 2, 2, 0, -1, -1, -1, 2, 2, -1, -1, -1, 2, 0, 0, 0, 0, 0, 0 } },
        { { SPRPackType::kDelta3 },
          { -3, 0, 0, 0, 0, 0, 0, -3, 0, 0, 0, 0, -3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { { SPRPackType::kRGBW },
          { -4, 0, 0, 0, 0, 0, -2, 2, 0, 0, 0, 0, 0, -4, 0, 0, 0, 0, 2, -2, 0, 0, 0, 0 } } }
    };
static const std::array<uint16_t, SPR_INIT_PARAM_SIZE_1> kDefaultRGBWGains = { 1024, 1024, 1024,
                                                                               341 };
static const std::array<uint16_t, SPR_INIT_PARAM_SIZE_1> kDefaultOPRGains = { 341, 341, 341, 0 };
static const std::array<uint16_t, SPR_INIT_PARAM_SIZE_2> kDefaultAdaptiveStrengths = { 0, 4, 8, 12,
                                                                                       16 };
static const std::array<uint16_t, SPR_INIT_PARAM_SIZE_5> kDefaultOPROffsets = {
  0,    132,  264,  396,  529,  661,  793,  925,  1057, 1189, 1321, 1453, 1586, 1718, 1850, 1982,
  2114, 2246, 2378, 2510, 2643, 2775, 2907, 3039, 3171, 3303, 3435, 3567, 3700, 3832, 3964, 4095
};

struct Crtc {
  drmModeObjectProperties *props;
  drmModePropertyRes **props_info;
  uint32_t mode_blob_id;
  uint32_t spr_blob_id;
};

struct Connector {
  drmModeObjectProperties *props;
  drmModePropertyRes **props_info;
};

struct Plane {
  drmModePlane *plane;
  drmModeObjectProperties *props;
  drmModePropertyRes ** props_info;
};

struct drm_msm_spr_init_cfg {
  __u64 flags;
  __u16 cfg0;
  __u16 cfg1;
  __u16 cfg2;
  __u16 cfg3;
  __u16 cfg4;
  __u16 cfg5;
  __u16 cfg6;
  __u16 cfg7;
  __u16 cfg8;
  __u16 cfg9;
  __u32 cfg10;
  __u16 cfg11[SPR_INIT_PARAM_SIZE_1];
  __u16 cfg12[SPR_INIT_PARAM_SIZE_1];
  __u16 cfg13[SPR_INIT_PARAM_SIZE_1];
  __u16 cfg14[SPR_INIT_PARAM_SIZE_2];
  __u16 cfg15[SPR_INIT_PARAM_SIZE_5];
  int cfg16[SPR_INIT_PARAM_SIZE_3];
  int cfg17[SPR_INIT_PARAM_SIZE_4];
};
struct drm_msm_spr_init_cfg_v2 {
  __u64 flags;
  __u16 cfg0;
  __u16 cfg1;
  __u16 cfg2;
  __u16 cfg3;
  __u16 cfg4;
  __u16 cfg5;
  __u16 cfg6;
  __u16 cfg7;
  __u16 cfg8;
  __u16 cfg9;
  __u32 cfg10;
  __u16 cfg11[SPR_INIT_PARAM_SIZE_1];
  __u16 cfg12[SPR_INIT_PARAM_SIZE_1];
  __u16 cfg13[SPR_INIT_PARAM_SIZE_1];
  __u16 cfg14[SPR_INIT_PARAM_SIZE_2];
  __u16 cfg15[SPR_INIT_PARAM_SIZE_5];
  int cfg16[SPR_INIT_PARAM_SIZE_3];
  int cfg17[SPR_INIT_PARAM_SIZE_4];
  __u16 cfg18_en;
  __u8 cfg18[SPR_INIT_PARAM_SIZE_6];
};

static drm_surface *drm_surfaces[2];
static int current_buffer;
static GRSurface *draw_buf = nullptr;

static drmModeCrtc *main_monitor_crtc;
static drmModeConnector *main_monitor_connector;

static int drm_fd = -1;

static bool current_blank_state = true;
static int fb_prop_id;
static struct Crtc crtc_res;
static struct Connector conn_res;
static struct Plane plane_res[NUM_PLANES];
static uint32_t number_of_lms = DEFAULT_NUM_LMS;
static uint32_t spr_enabled;
static std::string spr_prop_name;

#define find_prop_id(_res, type, Type, obj_id, prop_name, prop_id)    \
  do {                                                                \
    int j = 0;                                                        \
    int prop_count = 0;                                               \
    struct Type *obj = NULL;                                          \
    obj = (_res);                                                     \
    if (!obj || main_monitor_##type->type##_id != (obj_id)){          \
      prop_id = 0;                                                    \
      break;                                                          \
    }                                                                 \
    prop_count = (int)obj->props->count_props;                        \
    for (j = 0; j < prop_count; ++j)                                  \
      if (!strcmp(obj->props_info[j]->name, (prop_name)))             \
        break;                                                        \
    (prop_id) = (j == prop_count)?                                    \
      0 : obj->props_info[j]->prop_id;                                \
  } while (0)

#define add_prop(res, type, Type, id, id_name, id_val) \
  find_prop_id(res, type, Type, id, id_name, prop_id); \
  if (prop_id)                                         \
    drmModeAtomicAddProperty(atomic_req, id, prop_id, id_val);

/**
 * enum sde_rm_topology_name - HW resource use case in use by connector
 * @SDE_RM_TOPOLOGY_NONE:                 No topology in use currently
 * @SDE_RM_TOPOLOGY_SINGLEPIPE:           1 LM, 1 PP, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_SINGLEPIPE_DSC:       1 LM, 1 DSC, 1 PP, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_SINGLEPIPE_VDC:       1 LM, 1 VDC, 1 PP, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_DUALPIPE:             2 LM, 2 PP, 2 INTF/WB
 * @SDE_RM_TOPOLOGY_DUALPIPE_DSC:         2 LM, 2 DSC, 2 PP, 2 INTF/WB
 * @SDE_RM_TOPOLOGY_DUALPIPE_3DMERGE:     2 LM, 2 PP, 3DMux, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_DUALPIPE_3DMERGE_DSC: 2 LM, 2 PP, 3DMux, 1 DSC, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_DUALPIPE_3DMERGE_VDC: 2 LM, 2 PP, 3DMux, 1 VDC, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_DUALPIPE_DSCMERGE:    2 LM, 2 PP, 2 DSC Merge, 1 INTF/WB
 * @SDE_RM_TOPOLOGY_PPSPLIT:              1 LM, 2 PPs, 2 INTF/WB
 * @SDE_RM_TOPOLOGY_QUADPIPE_3DMERGE      4 LM, 4 PP, 3DMux, 2 INTF
 * @SDE_RM_TOPOLOGY_QUADPIPE_3DMERGE_DSC  4 LM, 4 PP, 3DMux, 3 DSC, 2 INTF
 * @SDE_RM_TOPOLOGY_QUADPIPE_DSCMERE      4 LM, 4 PP, 4 DSC Merge, 2 INTF
 * @SDE_RM_TOPOLOGY_QUADPIPE_DSC4HSMERGE  4 LM, 4 PP, 4 DSC Merge, 1 INTF
 */

static uint32_t get_lm_number(const std::string &topology) {
  if (topology == "sde_singlepipe") return 1;
  if (topology == "sde_singlepipe_dsc") return 1;
  if (topology == "sde_singlepipe_vdc") return 1;
  if (topology == "sde_dualpipe") return 2;
  if (topology == "sde_dualpipe_dsc") return 2;
  if (topology == "sde_dualpipe_vdc") return 2;
  if (topology == "sde_dualpipemerge") return 2;
  if (topology == "sde_dualpipemerge_dsc") return 2;
  if (topology == "sde_dualpipemerge_vdc") return 2;
  if (topology == "sde_dualpipe_dscmerge") return 2;
  if (topology == "sde_ppsplit") return 1;
  if (topology == "sde_quadpipemerge") return 4;
  if (topology == "sde_quadpipe_3dmerge_dsc") return 4;
  if (topology == "sde_quadpipe_dscmerge") return 4;
  if (topology == "sde_quadpipe_dsc4hsmerge") return 4;
  return DEFAULT_NUM_LMS;
}

static uint32_t get_topology_lm_number(int fd, uint32_t blob_id) {
  uint32_t num_lm = DEFAULT_NUM_LMS;

  drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(fd, blob_id);
  if (!blob) {
    return num_lm;
  }

  const char *fmt_str = (const char *)(blob->data);
  std::stringstream stream(fmt_str);
  std::string line = {};
  const std::string topology = "topology=";

  while (std::getline(stream, line)) {
    if (line.find(topology) != std::string::npos) {
        num_lm = get_lm_number(std::string(line, topology.length()));
        break;
    }
  }

  drmModeFreePropertyBlob(blob);
  return num_lm;
}

static int find_plane_prop_id(uint32_t obj_id, const char *prop_name,
                              Plane *plane_res) {
  int i, j = 0;
  int prop_count = 0;
  struct Plane *obj = NULL;

  for (i = 0; i < NUM_PLANES; ++i) {
    obj = &plane_res[i];
    if (!obj || obj->plane->plane_id != obj_id)
      continue;
    prop_count = (int)obj->props->count_props;
    for (j = 0; j < prop_count; ++j)
      if (!strcmp(obj->props_info[j]->name, prop_name))
       return obj->props_info[j]->prop_id;
    break;
  }

  return 0;
}

static int atomic_add_prop_to_plane(Plane *plane_res, drmModeAtomicReq *req,
                                    uint32_t obj_id, const char *prop_name,
                                    uint64_t value) {
  uint32_t prop_id;

  prop_id = find_plane_prop_id(obj_id, prop_name, plane_res);
  if (prop_id == 0) {
    printf("Could not find obj_id = %d\n", obj_id);
    return -EINVAL;
  }

  if (drmModeAtomicAddProperty(req, obj_id, prop_id, value) < 0) {
    printf("Could not add prop_id = %d for obj_id %d\n",
            prop_id, obj_id);
    return -EINVAL;
  }

  return 0;
}

static int SetupSprBlobV1(int fd, uint32_t* blob_id) {
  SPRPackType pack_type = SPRPackType::kPentile;
  SPRFilterType filter_type = SPRFilterType::kFourTap;
  SPRAdaptiveModeType adpative_mode = SPRAdaptiveModeType::kYYGM;

  drm_msm_spr_init_cfg spr_init_cfg;
  spr_init_cfg.cfg0 = 1;
  spr_init_cfg.cfg1 = 1;
  spr_init_cfg.cfg2 = 1;
  spr_init_cfg.cfg3 = 0;
  spr_init_cfg.flags = 0;
  spr_init_cfg.cfg4 = (pack_type == SPRPackType::kRGBW);
  spr_init_cfg.cfg5 = kDefaultColorPhaseIncrement.at(pack_type);
  spr_init_cfg.cfg6 = kDefaultColorPhaseRepeat.at(pack_type);
  spr_init_cfg.cfg7 = static_cast<uint16_t>(filter_type);
  spr_init_cfg.cfg8 = static_cast<uint16_t>(adpative_mode);
  if (pack_type == SPRPackType::kRGBW) {
    spr_init_cfg.cfg9 = 512;
    std::copy(kDefaultRGBWGains.begin(), kDefaultRGBWGains.end(), spr_init_cfg.cfg11);
  }
  spr_init_cfg.cfg10 = 0;
  std::copy(kDecimationRatioMap.at(pack_type).begin(), kDecimationRatioMap.at(pack_type).end(),
            spr_init_cfg.cfg11);
  std::copy(kDefaultOPRGains.begin(), kDefaultOPRGains.end(), spr_init_cfg.cfg13);
  std::copy(kDefaultAdaptiveStrengths.begin(), kDefaultAdaptiveStrengths.end(), spr_init_cfg.cfg14);
  std::copy(kDefaultOPROffsets.begin(), kDefaultOPROffsets.end(), spr_init_cfg.cfg15);
  std::copy(kDefaultFilterCoeffsMap.at(filter_type).begin(),
            kDefaultFilterCoeffsMap.at(filter_type).end(), spr_init_cfg.cfg16);
  std::copy(kDefaultColorPhaseMap.at(pack_type).begin(), kDefaultColorPhaseMap.at(pack_type).end(),
            spr_init_cfg.cfg17);

  if (drmModeCreatePropertyBlob(fd, &spr_init_cfg, sizeof(drm_msm_spr_init_cfg), blob_id)) {
    printf("failed to create spr blob\n");
    return -EINVAL;
  }

  return 0;
}

static int SetupSprBlobV2(int fd, uint32_t* blob_id) {
  SPRPackType pack_type = SPRPackType::kPentile;
  SPRFilterType filter_type = SPRFilterType::kFourTap;
  SPRAdaptiveModeType adpative_mode = SPRAdaptiveModeType::kYYGM;

  drm_msm_spr_init_cfg_v2 spr_init_cfg_v2;
  spr_init_cfg_v2.cfg0 = 1;
  spr_init_cfg_v2.cfg1 = 1;
  spr_init_cfg_v2.cfg2 = 1;
  spr_init_cfg_v2.cfg3 = 0;
  spr_init_cfg_v2.flags = 0;
  spr_init_cfg_v2.cfg4 = (pack_type == SPRPackType::kRGBW);
  spr_init_cfg_v2.cfg5 = kDefaultColorPhaseIncrement.at(pack_type);
  spr_init_cfg_v2.cfg6 = kDefaultColorPhaseRepeat.at(pack_type);
  spr_init_cfg_v2.cfg7 = static_cast<uint16_t>(filter_type);
  spr_init_cfg_v2.cfg8 = static_cast<uint16_t>(adpative_mode);
  if (pack_type == SPRPackType::kRGBW) {
    spr_init_cfg_v2.cfg9 = 512;
    std::copy(kDefaultRGBWGains.begin(), kDefaultRGBWGains.end(), spr_init_cfg_v2.cfg11);
  }
  spr_init_cfg_v2.cfg10 = 0;
  std::copy(kDecimationRatioMap.at(pack_type).begin(), kDecimationRatioMap.at(pack_type).end(),
            spr_init_cfg_v2.cfg11);
  std::copy(kDefaultOPRGains.begin(), kDefaultOPRGains.end(), spr_init_cfg_v2.cfg13);
  std::copy(kDefaultAdaptiveStrengths.begin(), kDefaultAdaptiveStrengths.end(),
            spr_init_cfg_v2.cfg14);
  std::copy(kDefaultOPROffsets.begin(), kDefaultOPROffsets.end(), spr_init_cfg_v2.cfg15);
  std::copy(kDefaultFilterCoeffsMap.at(filter_type).begin(),
            kDefaultFilterCoeffsMap.at(filter_type).end(), spr_init_cfg_v2.cfg16);
  std::copy(kDefaultColorPhaseMap.at(pack_type).begin(), kDefaultColorPhaseMap.at(pack_type).end(),
            spr_init_cfg_v2.cfg17);

  if (drmModeCreatePropertyBlob(fd, &spr_init_cfg_v2, sizeof(drm_msm_spr_init_cfg_v2), blob_id)) {
    printf("failed to create spr blob\n");
    return -EINVAL;
  }

  return 0;
}

static int SetupSprBlob(int fd, const std::string& prop_name, uint32_t* blob_id) {
  int ret = 0;
  if (prop_name == "SDE_SPR_INIT_CFG_V1") {
    ret = SetupSprBlobV1(fd, blob_id);
  } else if (prop_name == "SDE_SPR_INIT_CFG_V2") {
    ret = SetupSprBlobV2(fd, blob_id);
  } else {
    ret = -ENOENT;
  }

  return ret;
}

static int atomic_populate_plane(int plane, drmModeAtomicReqPtr atomic_req) {
  uint32_t src_x, src_y, src_w, src_h;
  uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
  int width = main_monitor_crtc->mode.hdisplay;
  int height = main_monitor_crtc->mode.vdisplay;
  int zpos = 0;
  src_y = 0;
  src_w = width/number_of_lms;
  src_h =  height;
  crtc_y = 0;
  crtc_w = width/number_of_lms;
  crtc_h = height;

  src_x = (width/number_of_lms) * plane;
  crtc_x = (width/number_of_lms) * plane;

  /* populate z-order property required for 4 layer mixer */
  if (number_of_lms == 4)
    zpos = plane >> 1;

  atomic_add_prop_to_plane(plane_res, atomic_req,
                           plane_res[plane].plane->plane_id, "zpos", zpos);

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "FB_ID",
                               drm_surfaces[current_buffer]->fb_id))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "SRC_X", src_x << 16))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "SRC_Y", src_y << 16))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "SRC_W", src_w << 16))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "SRC_H", src_h << 16))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "CRTC_X", crtc_x))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "CRTC_Y", crtc_y))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "CRTC_W", crtc_w))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "CRTC_H", crtc_h))
    return -EINVAL;

  if (atomic_add_prop_to_plane(plane_res, atomic_req,
                               plane_res[plane].plane->plane_id, "CRTC_ID",
                               main_monitor_crtc->crtc_id))
    return -EINVAL;

  return 0;
}

static int teardown_pipeline(drmModeAtomicReqPtr atomic_req) {
  uint32_t i, prop_id;
  int ret;

  /* During suspend, tear down pipeline */
  add_prop(&conn_res, connector, Connector, main_monitor_connector->connector_id, "CRTC_ID", 0);
  add_prop(&crtc_res, crtc, Crtc, main_monitor_crtc->crtc_id, "MODE_ID", 0);
  add_prop(&crtc_res, crtc, Crtc, main_monitor_crtc->crtc_id, "ACTIVE", 0);
  if (spr_enabled) {
    add_prop(&crtc_res, crtc, Crtc, main_monitor_crtc->crtc_id, spr_prop_name.c_str(), 0);
  }

  for(i = 0; i < number_of_lms; i++) {
    ret = atomic_add_prop_to_plane(plane_res, atomic_req,
                                   plane_res[i].plane->plane_id, "CRTC_ID", 0);
    if (ret < 0) {
      printf("Failed to tear down plane %d\n", i);
      return ret;
    }

    if (drmModeAtomicAddProperty(atomic_req, plane_res[i].plane->plane_id, fb_prop_id, 0) < 0) {
      printf("Failed to add property for plane_id=%d\n", plane_res[i].plane->plane_id);
      return -EINVAL;
    }
  }

  return 0;
}

static int drm_disable_crtc(drmModeAtomicReqPtr atomic_req) {
  return teardown_pipeline(atomic_req);
}

static int setup_pipeline(drmModeAtomicReqPtr atomic_req) {
  uint32_t i, prop_id;
  int ret;

  for(i = 0; i < number_of_lms; i++) {
    add_prop(&conn_res, connector, Connector, main_monitor_connector->connector_id,
         "CRTC_ID", main_monitor_crtc->crtc_id);
    add_prop(&crtc_res, crtc, Crtc, main_monitor_crtc->crtc_id, "MODE_ID", crtc_res.mode_blob_id);
    add_prop(&crtc_res, crtc, Crtc, main_monitor_crtc->crtc_id, "ACTIVE", 1);
    if (spr_enabled) {
      add_prop(&crtc_res, crtc, Crtc, main_monitor_crtc->crtc_id, spr_prop_name.c_str(),
           crtc_res.spr_blob_id);
    }
  }

  /* Setup planes */
  for(i = 0; i < number_of_lms; i++) {
    ret = atomic_populate_plane(i, atomic_req);
    if (ret < 0) {
      printf("Error populating plane_id = %d\n", plane_res[i].plane->plane_id);
      return ret;
    }
  }

  return 0;

}
static int drm_enable_crtc(drmModeAtomicReqPtr atomic_req) {
  return setup_pipeline(atomic_req);
}

static void drm_blank(minui_backend* backend __unused, bool blank) {
  int ret = 0;

  if (blank == current_blank_state)
    return;

  drmModeAtomicReqPtr atomic_req = drmModeAtomicAlloc();
  if (!atomic_req) {
     printf("Atomic Alloc failed\n");
     return;
  }

  if (blank)
    ret = drm_disable_crtc(atomic_req);
  else
    ret = drm_enable_crtc(atomic_req);

  if (!ret)
    ret = drmModeAtomicCommit(drm_fd, atomic_req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

  if (!ret) {
    printf("Atomic Commit succeed");
    current_blank_state = blank;
  } else {
    printf("Atomic Commit failed, rc = %d\n", ret);
  }

  drmModeAtomicFree(atomic_req);

}

static void drm_destroy_surface(struct drm_surface *surface) {
    if(!surface) return;

    if (surface->base.data) {
        munmap(surface->base.data, surface->base.row_bytes * surface->base.height);
    }

    if (surface->fb_id) {
        int ret = drmModeRmFB(drm_fd, surface->fb_id);
        if (ret) {
            printf("drmModeRmFB failed ret=%d\n", ret);
        }
    }

    if (surface->handle) {
        drm_gem_close gem_close = {};
        gem_close.handle = surface->handle;

        int ret = drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
        if (ret) {
            printf("DRM_IOCTL_GEM_CLOSE failed ret=%d\n", ret);
        }
    }

    free(surface);
}

static int drm_format_to_bpp(uint32_t format) {
    switch(format) {
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
            return 32;
        case DRM_FORMAT_RGB565:
            return 16;
        default:
            printf("Unknown format %d\n", format);
            return 32;
    }
}

static drm_surface *drm_create_surface(int width, int height) {
    uint32_t format;
    __u32 base_format;
    int ret;

    drm_surface* surface = static_cast<drm_surface*>(calloc(1, sizeof(*surface)));
    if (!surface) {
        printf("Can't allocate memory\n");
        return nullptr;
    }

#if defined(RECOVERY_ABGR)
    format = DRM_FORMAT_RGBA8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
    printf("setting DRM_FORMAT_RGBA8888 and GGL_PIXEL_FORMAT_RGBA_8888\n");
#elif defined(RECOVERY_BGRA)
    format = DRM_FORMAT_ARGB8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
    printf("setting DRM_FORMAT_ARGB8888 and GGL_PIXEL_FORMAT_RGBA_8888\n");
#elif defined(RECOVERY_RGBA)
    format = DRM_FORMAT_ABGR8888;
    base_format = GGL_PIXEL_FORMAT_BGRA_8888;
    printf("setting DRM_FORMAT_ABGR8888 and GGL_PIXEL_FORMAT_BGRA_8888, GGL_PIXEL_FORMAT may not match!\n");
#elif defined(RECOVERY_RGBX)
    format = DRM_FORMAT_XBGR8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
    printf("setting DRM_FORMAT_XBGR8888 and GGL_PIXEL_FORMAT_RGBA_8888\n");
#else
    format = DRM_FORMAT_RGB565;
    base_format = GGL_PIXEL_FORMAT_BGRA_8888;
    printf("setting DRM_FORMAT_RGB565 and GGL_PIXEL_FORMAT_RGB_565\n");
#endif

    drm_mode_create_dumb create_dumb = {};
    create_dumb.height = height;
    create_dumb.width = width;
    create_dumb.bpp = drm_format_to_bpp(format);
    create_dumb.flags = 0;

    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
    if (ret) {
        printf("DRM_IOCTL_MODE_CREATE_DUMB failed ret=%d\n",ret);
        drm_destroy_surface(surface);
        return nullptr;
    }
    surface->handle = create_dumb.handle;

    uint32_t handles[4], pitches[4], offsets[4];

    handles[0] = surface->handle;
    pitches[0] = create_dumb.pitch;
    offsets[0] = 0;

    ret = drmModeAddFB2(drm_fd, width, height,
            format, handles, pitches, offsets,
            &(surface->fb_id), 0);
    if (ret) {
        printf("drmModeAddFB2 failed ret=%d\n", ret);
        drm_destroy_surface(surface);
        return nullptr;
    }

    struct drm_mode_map_dumb map_dumb = {};
    map_dumb.handle = create_dumb.handle;
    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
    if (ret) {
        printf("DRM_IOCTL_MODE_MAP_DUMB failed ret=%d\n",ret);
        drm_destroy_surface(surface);
        return nullptr;;
    }

    surface->base.height = height;
    surface->base.width = width;
    surface->base.row_bytes = create_dumb.pitch;
    surface->base.pixel_bytes = create_dumb.bpp / 8;
    surface->base.format = base_format;
    surface->base.data = (unsigned char*)
                         mmap(nullptr,
                              surface->base.height * surface->base.row_bytes,
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              drm_fd, map_dumb.offset);
    if (surface->base.data == MAP_FAILED) {
        printf("mmap() failed");
        drm_destroy_surface(surface);
        return nullptr;
    }

    return surface;
}

static drmModeCrtc *find_crtc_for_connector(int fd,
                            drmModeRes *resources,
                            drmModeConnector *connector) {
    drmModeEncoder *encoder;
    /*
     * Find the encoder. If we already have one, just use it.
     */
    if (connector->encoder_id) {
        encoder = drmModeGetEncoder(fd, connector->encoder_id);
    }
    else {
        encoder = nullptr;
    }

    int32_t crtc;
    if (encoder && encoder->crtc_id) {
        crtc = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
        return drmModeGetCrtc(fd, crtc);
    }

    /*
     * Didn't find anything, try to find a crtc and encoder combo.
     */
    crtc = -1;
    for (int i = 0; i < connector->count_encoders; i++) {
        encoder = drmModeGetEncoder(fd, connector->encoders[i]);

        if (encoder) {
            for (int j = 0; j < resources->count_crtcs; j++) {
                if (!(encoder->possible_crtcs & (1 << j)))
                    continue;
                crtc = resources->crtcs[j];
                break;
            }
            if (crtc >= 0) {
                drmModeFreeEncoder(encoder);
                return drmModeGetCrtc(fd, crtc);
            }
        }
    }
    return nullptr;
}

static drmModeConnector *find_used_connector_by_type(int fd,
                                 drmModeRes *resources,
                                 unsigned type) {
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector) {
            if ((connector->connector_type == type) &&
                    (connector->connection == DRM_MODE_CONNECTED) &&
                    (connector->count_modes > 0))
                return connector;

            drmModeFreeConnector(connector);
        }
    }
    return nullptr;
}

static drmModeConnector *find_first_connected_connector(int fd,
                             drmModeRes *resources) {
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector) {
            if ((connector->count_modes > 0) &&
                    (connector->connection == DRM_MODE_CONNECTED))
                return connector;

            drmModeFreeConnector(connector);
        }
    }
    return nullptr;
}

static drmModeConnector *find_main_monitor(int fd, drmModeRes *resources,
        uint32_t *mode_index) {
    /* Look for LVDS/eDP/DSI connectors. Those are the main screens. */
    unsigned kConnectorPriority[] = {
        DRM_MODE_CONNECTOR_LVDS,
        DRM_MODE_CONNECTOR_eDP,
        DRM_MODE_CONNECTOR_DSI,
    };

    drmModeConnector *main_monitor_connector = nullptr;
    unsigned i = 0;
    do {
        main_monitor_connector = find_used_connector_by_type(fd,
                                         resources,
                                         kConnectorPriority[i]);
        i++;
    } while (!main_monitor_connector && i < ARRAY_SIZE(kConnectorPriority));

    /* If we didn't find a connector, grab the first one that is connected. */
    if (!main_monitor_connector)
        main_monitor_connector =
                find_first_connected_connector(fd, resources);

    /* If we still didn't find a connector, give up and return. */
    if (!main_monitor_connector)
        return nullptr;

    *mode_index = 0;
    for (int modes = 0; modes < main_monitor_connector->count_modes; modes++) {
        if (main_monitor_connector->modes[modes].type &
                DRM_MODE_TYPE_PREFERRED) {
            *mode_index = modes;
            break;
        }
    }

    return main_monitor_connector;
}

static void disable_non_main_crtcs(int fd,
                    drmModeRes *resources,
                    drmModeCrtc* main_crtc) {
  uint32_t prop_id;
  drmModeAtomicReqPtr atomic_req = drmModeAtomicAlloc();
  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
    drmModeCrtc* crtc = find_crtc_for_connector(fd, resources, connector);
    if (crtc->crtc_id != main_crtc->crtc_id) {
      // Switching to atomic commit. Given only crtc, we can only set ACTIVE = 0
      // to disable any Nonmain CRTCs
      find_prop_id(&crtc_res, crtc, Crtc, crtc->crtc_id, "ACTIVE", prop_id);
      if (prop_id == 0)
        return;

      if (drmModeAtomicAddProperty(atomic_req, main_monitor_crtc->crtc_id, prop_id, 0) < 0)
        return;

    }
    drmModeFreeCrtc(crtc);
  }
  if (drmModeAtomicCommit(drm_fd, atomic_req,DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
    printf("Atomic Commit failed in DisableNonMainCrtcs\n");

  drmModeAtomicFree(atomic_req);
}

static void update_plane_fb() {
  uint32_t i, prop_id;

  /* Set atomic req */
  drmModeAtomicReqPtr atomic_req = drmModeAtomicAlloc();
  if (!atomic_req) {
     printf("Atomic Alloc failed. Could not update fb_id\n");
     return;
  }

  /* Add conn-crtc association property required
   * for driver to recognize quadpipe topology.
   */
  add_prop(&conn_res, connector, Connector, main_monitor_connector->connector_id,
           "CRTC_ID", main_monitor_crtc->crtc_id);

  /* Add property */
  for(i = 0; i < number_of_lms; i++)
    drmModeAtomicAddProperty(atomic_req, plane_res[i].plane->plane_id,
                             fb_prop_id, drm_surfaces[current_buffer]->fb_id);

  /* Commit changes */
  int32_t ret;
  ret = drmModeAtomicCommit(drm_fd, atomic_req,
                 DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

  drmModeAtomicFree(atomic_req);

  if (ret)
    printf("Atomic commit failed ret=%d\n", ret);

}

static GRSurface* drm_init(minui_backend* backend __unused) {
  drmModeRes* res = nullptr;

  spr_enabled = android::base::GetIntProperty("vendor.display.enable_spr", 0);
  /* Consider DRM devices in order. */
  for (int i = 0; i < DRM_MAX_MINOR; i++) {
    char* dev_name;
    int ret = asprintf(&dev_name, DRM_DEV_NAME, DRM_DIR_NAME, i);
    if (ret < 0) continue;

    drm_fd = open(dev_name, O_RDWR, 0);
    free(dev_name);
    if (drm_fd < 0) continue;

    uint64_t cap = 0;
    /* We need dumb buffers. */
    ret = drmGetCap(drm_fd, DRM_CAP_DUMB_BUFFER, &cap);
    if (ret || cap == 0) {
      close(drm_fd);
      continue;

    }
    res = drmModeGetResources(drm_fd);
    if (!res) {
      close(drm_fd);
      continue;
    }

    /* Use this device if it has at least one connected monitor. */
    if (res->count_crtcs > 0 && res->count_connectors > 0) {
      if (find_first_connected_connector(drm_fd, res)) break;
    }

    drmModeFreeResources(res);
    close(drm_fd);
    res = nullptr;
  }

  if (drm_fd < 0 || res == nullptr) {
    perror("cannot find/open a drm device");
    return nullptr;
  }

  uint32_t selected_mode;
  main_monitor_connector = find_main_monitor(drm_fd, res, &selected_mode);

  if (!main_monitor_connector) {
    printf("main_monitor_connector not found\n");
    drmModeFreeResources(res);
    close(drm_fd);
    return nullptr;
  }

  main_monitor_crtc = find_crtc_for_connector(drm_fd, res, main_monitor_connector);

  if (!main_monitor_crtc) {
    printf("main_monitor_crtc not found\n");
    drmModeFreeResources(res);
    close(drm_fd);
    return nullptr;
  }

  disable_non_main_crtcs(drm_fd, res, main_monitor_crtc);

  main_monitor_crtc->mode = main_monitor_connector->modes[selected_mode];

  int width = main_monitor_crtc->mode.hdisplay;
  int height = main_monitor_crtc->mode.vdisplay;

  printf("width: %d, height: %d\n", width, height);

  drmModeFreeResources(res);

  drm_surfaces[0] = drm_create_surface(width, height);
  drm_surfaces[1] = drm_create_surface(width, height);
  if (!drm_surfaces[0] || !drm_surfaces[1]) {
    drm_destroy_surface(drm_surfaces[0]);
    drm_destroy_surface(drm_surfaces[1]);
    drmModeFreeResources(res);
    close(drm_fd);
    return nullptr;
  }

  draw_buf = (GRSurface *)malloc(sizeof(GRSurface));
  if (!draw_buf) {
    printf("failed to alloc draw_buf\n");
    drm_destroy_surface(drm_surfaces[0]);
    drm_destroy_surface(drm_surfaces[1]);
    drmModeFreeResources(res);
    close(drm_fd);
    return nullptr;
  }

  memcpy(draw_buf, &drm_surfaces[0]->base, sizeof(GRSurface));
  draw_buf->data = (unsigned char *)calloc(draw_buf->height * draw_buf->row_bytes, 1);
  if (!draw_buf->data) {
    printf("failed to alloc draw_buf surface\n");
    free(draw_buf);
    drm_destroy_surface(drm_surfaces[0]);
    drm_destroy_surface(drm_surfaces[1]);
    drmModeFreeResources(res);
    close(drm_fd);
    return nullptr;
  }

  current_buffer = 0;

  drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

 /* Get possible plane_ids */
  drmModePlaneRes *plane_options = drmModeGetPlaneResources(drm_fd);
  if (!plane_options || !plane_options->planes || (plane_options->count_planes < number_of_lms))
    return NULL;

  /* Set crtc resources */
  crtc_res.props = drmModeObjectGetProperties(drm_fd,
                      main_monitor_crtc->crtc_id,
                      DRM_MODE_OBJECT_CRTC);

  if (!crtc_res.props)
    return NULL;

  crtc_res.props_info = static_cast<drmModePropertyRes **>
                           (calloc(crtc_res.props->count_props,
                           sizeof(crtc_res.props_info)));
  if (!crtc_res.props_info)
    return NULL;
  else
    for (int j = 0; j < (int)crtc_res.props->count_props; ++j) {
      crtc_res.props_info[j] = drmModeGetProperty(drm_fd,
                                   crtc_res.props->props[j]);
      /* Get spr property name */
      if (!strcmp(crtc_res.props_info[j]->name, "SDE_SPR_INIT_CFG_V1") ||
          !strcmp(crtc_res.props_info[j]->name, "SDE_SPR_INIT_CFG_V2")) {
        spr_prop_name = crtc_res.props_info[j]->name;
      }
    }

  /* Set connector resources */
  conn_res.props = drmModeObjectGetProperties(drm_fd,
                     main_monitor_connector->connector_id,
                     DRM_MODE_OBJECT_CONNECTOR);
  if (!conn_res.props)
    return NULL;

  conn_res.props_info = static_cast<drmModePropertyRes **>
                         (calloc(conn_res.props->count_props,
                         sizeof(conn_res.props_info)));
  if (!conn_res.props_info)
    return NULL;
  else {
    for (int j = 0; j < (int)conn_res.props->count_props; ++j) {
      conn_res.props_info[j] = drmModeGetProperty(drm_fd,
                                 conn_res.props->props[j]);

      /* Get preferred mode information and extract the
       * number of layer mixers needed from the topology name.
       */
      if (!strcmp(conn_res.props_info[j]->name, "mode_properties")) {
        number_of_lms = get_topology_lm_number(drm_fd, conn_res.props->prop_values[j]);
        printf("number of lms in topology %d\n", number_of_lms);
      }
    }
  }

  /* Set plane resources */
  for(uint32_t i = 0; i < number_of_lms; ++i) {
    plane_res[i].plane = drmModeGetPlane(drm_fd, plane_options->planes[i]);
    if (!plane_res[i].plane)
      return NULL;
  }

  for (uint32_t i = 0; i < number_of_lms; ++i) {
    struct Plane *obj = &plane_res[i];
    unsigned int j;
    obj->props = drmModeObjectGetProperties(drm_fd, obj->plane->plane_id,
                    DRM_MODE_OBJECT_PLANE);
    if (!obj->props)
      continue;
    obj->props_info = static_cast<drmModePropertyRes **>
                         (calloc(obj->props->count_props, sizeof(*obj->props_info)));
    if (!obj->props_info)
      continue;
    for (j = 0; j < obj->props->count_props; ++j)
      obj->props_info[j] = drmModeGetProperty(drm_fd, obj->props->props[j]);
  }

  drmModeFreePlaneResources(plane_options);
  plane_options = NULL;

  /* Setup spr blob id if enabled */
  if (spr_enabled) {
    if (SetupSprBlob(drm_fd, spr_prop_name, &crtc_res.spr_blob_id)) {
      return NULL;
    }
  }

  /* Setup pipe and blob_id */
  if (drmModeCreatePropertyBlob(drm_fd, &main_monitor_crtc->mode, sizeof(drmModeModeInfo),
      &crtc_res.mode_blob_id)) {
    printf("failed to create mode blob\n");
    return NULL;
  }

  /* Save fb_prop_id*/
  uint32_t prop_id;
  prop_id = find_plane_prop_id(plane_res[0].plane->plane_id, "FB_ID", plane_res);
  fb_prop_id = prop_id;

  drm_blank(nullptr, false);

  return draw_buf;
}

static GRSurface* drm_flip(minui_backend* backend __unused) {
    memcpy(drm_surfaces[current_buffer]->base.data,
            draw_buf->data, draw_buf->height * draw_buf->row_bytes);
    update_plane_fb();
    current_buffer = 1 - current_buffer;
    return draw_buf;
}

static void drm_exit(minui_backend* backend __unused) {
    drm_blank(nullptr, true);
    drmModeDestroyPropertyBlob(drm_fd, crtc_res.mode_blob_id);
    drm_destroy_surface(drm_surfaces[0]);
    drm_destroy_surface(drm_surfaces[1]);
    close(drm_fd);
    drm_fd = -1;
}

static minui_backend drm_backend = {
    .init = drm_init,
    .flip = drm_flip,
    .blank = drm_blank,
    .exit = drm_exit,
};

minui_backend* open_drm() {
    return &drm_backend;
}
