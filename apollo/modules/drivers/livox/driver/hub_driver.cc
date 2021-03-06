/******************************************************************************
 * Copyright 2020 Livox. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "livox_def.h"
#include "livox_sdk.h"

#include "cyber/cyber.h"
#include "modules/drivers/livox/driver/hub_driver.h"

#include "modules/drivers/livox/proto/config.pb.h"
#include "modules/drivers/proto/pointcloud.pb.h"

namespace apollo {
namespace drivers {
namespace livox {

using apollo::drivers::PointXYZIT;

/** @brief For callback use only. */
static LivoxHubDriver *g_hub_driver = nullptr;

template <class T>
void global_callback(livox_status status, uint8_t handle, T t,
                     void *client_data) {
  auto f =
      static_cast<std::function<void(livox_status, uint8_t, T)> *>(client_data);
  (*f)(status, handle, t);
  delete f;
}

bool LivoxHubDriver::DriverInit() {
  if (!Init()) {
    Uninit();
    AERROR << "Livox-SDK init fail!";
    return false;
  }

  LivoxSdkVersion sdkversion;
  GetLivoxSdkVersion(&sdkversion);
  AINFO << "Livox SDK version" << sdkversion.major << "." << sdkversion.minor
        << "." << sdkversion.patch;

  SetBroadcastCallback(LivoxHubDriver::OnDeviceBroadcast);
  SetDeviceStateUpdateCallback(LivoxHubDriver::OnDeviceInfoChange);

  // Start livox sdk to receive lidar data
  if (!Start()) {
    Uninit();
    AERROR << "Livox-SDK init fail!";
    return false;
  }

  // Add here, only for callback use.
  if (!g_hub_driver) {
    g_hub_driver = this;
  }

  return true;
}

void LivoxHubDriver::DriverUninit() {
  Uninit();
  devices_.clear();
  AINFO << "Livox SDK Deinit completely!";
}

/** @brief Static function in LivoxHubDriver for Point cloud callback. */
void LivoxHubDriver::HubDataCb(uint8_t hub_handle, LivoxEthPacket *data,
                               uint32_t data_num, void *client_data) {
  LivoxHubDriver *hub_driver = static_cast<LivoxHubDriver *>(client_data);
  if (!data || data_num == 0) {
    return;
  }

  // Caculate which lidar this eth packet data belong to
  uint8_t handle = HubGetLidarHandle(data->slot, data->id);
  if (handle >= kMaxLidarCount) {
    return;
  }

  if (hub_driver) {
    hub_driver->PointCloudProcessCallback(data, data_num);
  } else {
    AERROR << "lds_hub does not exits";
  }
}

void LivoxHubDriver::PointCloudProcessCallback(LivoxEthPacket *data,
                                               uint32_t data_num) {
  if (!data) {
    return;
  }

  uint8_t hub_port = data->slot;

  if (data_cb_) {
    data_cb_(hub_port, data, data_num);
  } else {
    AERROR << "Data cb does not exits";
  }
}

void LivoxHubDriver::OnDeviceBroadcast(const BroadcastDeviceInfo *info) {
  if (!info) {
    return;
  }

  if (info->dev_type != kDeviceTypeHub) {
    AWARN << "It's Not Livox Hub";
    return;
  }

  uint8_t hub_handle = kMaxLidarCount - 1;
  AddHubToConnect(info->broadcast_code, &hub_handle);
  SetDataCallback(hub_handle, LivoxHubDriver::HubDataCb, (void *)g_hub_driver);
  g_hub_driver->SetHubHandle(hub_handle);
}

/** @brief Callback function of changing of device state. */
void LivoxHubDriver::OnDeviceInfoChange(const DeviceInfo *info,
                                        DeviceEvent type) {
  if (!info) {
    return;
  }

  if (type == kEventDisconnect) {
    AERROR << "Hub: " << info->broadcast_code << " Disconnect!";
  } else if (type == kEventStateChange) {
    AWARN << "Hub: " << info->broadcast_code << " StateChange!";
  } else if (type == kEventHubConnectionChange) {
    AINFO << "Hub: " << info->broadcast_code << " Connect!";
  }

  AINFO << "info->state: " << info->state;
  if (info->state != kLidarStateNormal) {
    AINFO << "Hub State Not Normal";
    return;
  }

  if (g_hub_driver) {
    g_hub_driver->HubStartSample();
  }
}

void LivoxHubDriver::HubStartSample() {
  HubQueryLidarInfo([=](bool status) {
    if (!status) {
      return;
    }
    HubConfig([=](bool status) {
      if (!status) {
        return;
      }
      HubSampling();
    });
  });
}

void LivoxHubDriver::HubQueryLidarInfo(std::function<void(bool)> callback) {
  auto func_callback = new std::function<void(
      livox_status, uint8_t, HubQueryLidarInformationResponse *)>(
      [&, callback](livox_status status, uint8_t handle,
                    HubQueryLidarInformationResponse *response) {
        if ((handle >= kMaxLidarCount) || status != kStatusSuccess ||
            !response || response->ret_code != 0) {
          AERROR << "Hub query lidar information fail!";
          callback(false);
          return;
        }
        AINFO << "Hub query lidar information success!";

        if (response->count != config_.lidars_conf().lidar_conf().size()) {
          AERROR << "Hub connect: " << response->count << "lidars"
                 << "not same with lidar_conf size: "
                 << config_.lidars_conf().lidar_conf().size();
          callback(false);
          return;
        }

        for (int i = 0; i < response->count; i++) {
          ConnectedLidarInfo lidar_info = response->device_info_list[i];
          uint8_t hub_port = lidar_info.slot;
          devices_[hub_port].push_back(lidar_info);
        }
        callback(true);
      });
  HubQueryLidarInformation(global_callback<HubQueryLidarInformationResponse *>,
                           func_callback);
}

/** @brief Config Hub's paramters. */
void LivoxHubDriver::HubConfig(std::function<void(bool)> callback) {
  HubConfigPointCloudReturnMode([=](bool status) {
    if (!status) {
      callback(false);
      return;
    }
    HubConfigFan([=](bool status) {
      if (!status) {
        callback(false);
        return;
      }
      HubConfigCoordinate([=](bool status) {
        if (!status) {
          callback(false);
          return;
        }
        callback(true);
      });
    });
  });
}

void LivoxHubDriver::HubConfigPointCloudReturnMode(
    std::function<void(bool)> callback) {
  uint8_t req_buf[1024] = {0};
  HubSetPointCloudReturnModeRequest *req =
      (HubSetPointCloudReturnModeRequest *)req_buf;

  for (auto config : config_.lidars_conf().lidar_conf()) {
    uint8_t hub_port = (uint8_t)config.hub_port();

    if (!config.has_return_mode()) {
      AWARN << "hub_port: "<< hub_port << " not config return mode";
      continue;
    }

    if (IsMid40(hub_port)) {
      AWARN << "hub_port: " << hub_port << " not support config return mode";
      continue;
    }

    if (devices_.find(hub_port) == devices_.end()) {
      continue;
    }

    for (const auto &device : devices_[hub_port]) {
      strncpy(req->lidar_cfg_list[req->count].broadcast_code, device.broadcast_code,
              sizeof(req->lidar_cfg_list[req->count].broadcast_code));
      req->lidar_cfg_list[req->count].mode = config.return_mode();
      req->count++;
    }
  }

  if (req->count == 0) {
    callback(true);
    return;
  }

  uint32_t length =
      sizeof(HubSetPointCloudReturnModeRequest) +
      sizeof(SetPointCloudReturnModeRequestItem) * (req->count - 1);

  auto func_callback = new std::function<void(
      livox_status, uint8_t, HubSetPointCloudReturnModeResponse *)>(
      [=](livox_status status, uint8_t handle,
          HubSetPointCloudReturnModeResponse *response) {
        if ((handle >= kMaxLidarCount) || status != kStatusSuccess ||
            !response || response->ret_code != 0) {
          AERROR << "Hub set return mode fail!";
          callback(false);
          return;
        }
        AINFO << "Hub set return mode success!";
        callback(true);
      });
  HubSetPointCloudReturnMode(
      req, length, global_callback<HubSetPointCloudReturnModeResponse *>,
      func_callback);
}

void LivoxHubDriver::HubConfigFan(std::function<void(bool)> callback) {
  uint8_t req_buf[1024] = {0};
  HubFanControlRequest *req = (HubFanControlRequest *)req_buf;

  for (auto config : config_.lidars_conf().lidar_conf()) {
    uint8_t hub_port = (uint8_t)config.hub_port();
    if (!config.has_fan_status()) {
      AWARN << hub_port << " not config fan state";
      continue;
    }

    if (IsMid40(hub_port)) {
      AWARN << "hub_port: " << hub_port << " not support config fan";
      continue;
    }

    if (devices_.find(hub_port) == devices_.end()) {
      continue;
    }

    for (const auto &device : devices_[hub_port]) {
      strncpy(req->lidar_cfg_list[req->count].broadcast_code, device.broadcast_code,
              sizeof(req->lidar_cfg_list[req->count].broadcast_code));
      req->lidar_cfg_list[req->count].state = config.fan_status();
      req->count++;
    }
  }

  if (req->count == 0) {
    callback(true);
    return;
  }

  uint32_t length =
      sizeof(HubSetPointCloudReturnModeRequest) +
      sizeof(SetPointCloudReturnModeRequestItem) * (req->count - 1);

  auto func_callback =
      new std::function<void(livox_status, uint8_t, HubFanControlResponse *)>(
          [=](livox_status status, uint8_t handle,
              HubFanControlResponse *response) {
            if ((handle >= kMaxLidarCount) || status != kStatusSuccess ||
                !response || response->ret_code != 0) {
              AERROR << "Hub set fan state fail!";
              callback(false);
              return;
            }
            AINFO << "Hub set fan state success!";
            callback(true);
          });
  HubFanControl(req, length, global_callback<HubFanControlResponse *>,
                func_callback);
}

void LivoxHubDriver::HubConfigCoordinate(std::function<void(bool)> callback) {
  auto func_callback = new std::function<void(livox_status, uint8_t, uint8_t)>(
      [=](livox_status status, uint8_t handle, uint8_t response) {
        if (status != kStatusSuccess || response != 0) {
          AERROR << "Set coordinate fail!";
          callback(false);
          return;
        }
        AINFO << "Set coordinate success!";
        callback(true);
      });
  SetCartesianCoordinate(hub_handle_, global_callback<uint8_t>, func_callback);
}

void LivoxHubDriver::HubSampling() {
  auto func_callback = new std::function<void(livox_status, uint8_t, uint8_t)>(
      [=](livox_status status, uint8_t handle, uint8_t response) {
        if ((status != kStatusSuccess) || (response != 0)) {
          AERROR << "Hub start sample fail : state:" << status
                 << "handle: " << handle << "response: " << response;
          return;
        }
        AINFO << "Hub start sample success";
      });
  HubStartSampling(global_callback<uint8_t>, func_callback);
}

bool LivoxHubDriver::IsMid40(uint8_t hub_port) {
  if (devices_.find(hub_port) == devices_.end()) {
    return false;
  }

  if (devices_[hub_port].front().dev_type == kDeviceTypeLidarMid40) {
    return true;
  }

  return false;
}

}  // namespace livox
}  // namespace drivers
}  // namespace apollo