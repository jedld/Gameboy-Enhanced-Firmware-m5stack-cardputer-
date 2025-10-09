#include "bluetooth_manager.h"

#include <algorithm>
#include <cstring>

constexpr uint16_t kHidServiceUuid = 0x1812;
constexpr uint16_t kHidReportUuid = 0x2A4D;
constexpr uint16_t kBootKeyboardInputUuid = 0x2A22;
constexpr uint32_t kScanDurationSeconds = 5;

namespace {

bool containsHidService(NimBLEAdvertisedDevice &device) {
  if(device.getServiceUUIDCount() == 0) {
    return true;
  }
  for(size_t i = 0; i < device.getServiceUUIDCount(); ++i) {
    NimBLEUUID uuid = device.getServiceUUID(i);
    if(uuid.equals(NimBLEUUID(kHidServiceUuid))) {
      return true;
    }
  }
  return false;
}

std::string deviceDisplayName(NimBLEAdvertisedDevice &device) {
  if(device.haveName()) {
    return device.getName();
  }
  return device.getAddress().toString();
}

}  // namespace

class AdvertisedCallbacks : public NimBLEAdvertisedDeviceCallbacks {
 public:
  void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
    if(advertisedDevice != nullptr) {
      BluetoothManager::instance().handleAdvertisedDevice(*advertisedDevice);
    }
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient *client) override {
    BluetoothManager::instance().handleClientConnect(client);
  }

  void onDisconnect(NimBLEClient *client) override {
    BluetoothManager::instance().handleClientDisconnect(client);
  }
};

AdvertisedCallbacks g_advertised_callbacks;
ClientCallbacks g_client_callbacks;

void hidNotifyCallback(NimBLERemoteCharacteristic *characteristic,
                       uint8_t *data,
                       size_t length,
                       bool) {
  BluetoothManager::instance().handleHidNotification(characteristic, data, length);
}

void scanCompleteCallback(NimBLEScanResults) {
  BluetoothManager::instance().handleScanComplete();
}

BluetoothManager &BluetoothManager::instance() {
  static BluetoothManager inst;
  return inst;
}

bool BluetoothManager::initialize() {
  if(initialized_) {
    return true;
  }

  NimBLEDevice::init("Cardputer GB");
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_advertised_callbacks, false);
  scan->setInterval(1600);
  scan->setWindow(800);
  scan->setActiveScan(true);
  scan->setDuplicateFilter(true);

  devices_.clear();
  scanning_ = false;
  initialized_ = true;
  return true;
}

void BluetoothManager::shutdown() {
  if(!initialized_) {
    return;
  }

  stopScan();
  disconnectAll();
  devices_.clear();
  NimBLEDevice::deinit(true);
  initialized_ = false;
  scanning_ = false;
}

void BluetoothManager::startScan() {
  if(!initialized_) {
    return;
  }
  if(scanning_) {
    return;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  // Preserve connected devices, prune stale disconnected entries before scanning.
  devices_.erase(std::remove_if(devices_.begin(),
                                devices_.end(),
                                [](const BluetoothDeviceInfo &info) {
                                  return !info.connected && !info.connecting;
                                }),
                 devices_.end());

  scan->clearResults();
  if(scan->start(kScanDurationSeconds, scanCompleteCallback)) {
    scanning_ = true;
  }
}

void BluetoothManager::stopScan() {
  if(!initialized_) {
    return;
  }
  NimBLEScan *scan = NimBLEDevice::getScan();
  if(scan->isScanning()) {
    scan->stop();
  }
  handleScanComplete();
}

bool BluetoothManager::connectDevice(size_t index) {
  if(index >= devices_.size()) {
    return false;
  }

  BluetoothDeviceInfo &device = devices_[index];
  if(device.connected || device.connecting) {
    return true;
  }

  NimBLEClient *client = NimBLEDevice::createClient();
  if(client == nullptr) {
    return false;
  }
  client->setClientCallbacks(&g_client_callbacks, false);

  device.connecting = true;
  device.client = client;

  if(!client->connect(device.address, false)) {
    device.connecting = false;
    device.client = nullptr;
    NimBLEDevice::deleteClient(client);
    return false;
  }

  return true;
}

void BluetoothManager::disconnectAll() {
  for(auto &device : devices_) {
    if(device.client != nullptr) {
      device.client->disconnect();
    }
  }

  if(keyboard_cb_) {
    for(uint16_t code = 0; code < 256; ++code) {
      keyboard_cb_(static_cast<uint8_t>(code), false);
    }
  }
}

void BluetoothManager::setKeyboardCallback(BluetoothKeyCallback cb) {
  keyboard_cb_ = std::move(cb);
}

void BluetoothManager::loop() {
  scanning_ = NimBLEDevice::getScan()->isScanning();
}

void BluetoothManager::handleAdvertisedDevice(NimBLEAdvertisedDevice &device) {
  if(!containsHidService(device)) {
    return;
  }

  BluetoothDeviceInfo *existing = findDevice(device.getAddress());
  if(existing != nullptr) {
    if(device.haveName() && existing->name != device.getName()) {
      existing->name = device.getName();
    }
    return;
  }

  BluetoothDeviceInfo info;
  info.address = device.getAddress();
  info.name = deviceDisplayName(device);
  devices_.push_back(info);
}

void BluetoothManager::handleScanComplete() {
  scanning_ = false;
}

void BluetoothManager::handleClientConnect(NimBLEClient *client) {
  if(client == nullptr) {
    return;
  }

  BluetoothDeviceInfo *info = findDevice(client);
  if(info == nullptr) {
    return;
  }

  info->connected = true;
  info->connecting = false;
  info->client = client;

  NimBLERemoteService *hid_service = client->getService(NimBLEUUID(kHidServiceUuid));
  if(hid_service == nullptr) {
    return;
  }

  NimBLERemoteCharacteristic *characteristic = nullptr;

  characteristic = hid_service->getCharacteristic(NimBLEUUID(kBootKeyboardInputUuid));
  if(characteristic == nullptr || !characteristic->canNotify()) {
    characteristic = hid_service->getCharacteristic(NimBLEUUID(kHidReportUuid));
  }

  if(characteristic == nullptr || !characteristic->canNotify()) {
    // Fallback: choose first notifiable characteristic in the service.
    std::vector<NimBLERemoteCharacteristic*> *characteristics = hid_service->getCharacteristics();
    if(characteristics != nullptr) {
      for(NimBLERemoteCharacteristic *candidate : *characteristics) {
        if(candidate != nullptr && candidate->canNotify()) {
          characteristic = candidate;
          break;
        }
      }
    }
  }

  if(characteristic != nullptr && characteristic->canNotify()) {
    if(characteristic->subscribe(true, hidNotifyCallback, true)) {
      info->input_characteristic = characteristic;
    }
  }
}

void BluetoothManager::handleClientDisconnect(NimBLEClient *client) {
  if(client == nullptr) {
    return;
  }

  BluetoothDeviceInfo *info = findDevice(client);
  if(info == nullptr) {
    NimBLEDevice::deleteClient(client);
    return;
  }

  if(keyboard_cb_) {
    for(uint16_t code = 0; code < 256; ++code) {
      keyboard_cb_(static_cast<uint8_t>(code), false);
    }
  }

  info->connected = false;
  info->connecting = false;
  info->input_characteristic = nullptr;
  info->client = nullptr;

  NimBLEDevice::deleteClient(client);
}

void BluetoothManager::handleHidNotification(NimBLERemoteCharacteristic *characteristic,
                                             const uint8_t *data,
                                             size_t length) {
  if(characteristic == nullptr || keyboard_cb_ == nullptr || length == 0) {
    return;
  }

  BluetoothDeviceInfo *info = findDevice(characteristic);
  if(info == nullptr) {
    return;
  }

  // Standard HID keyboard report: [modifiers][reserved][keys...]
  const uint8_t modifiers = data[0];
  const uint8_t *keys = (length > 2) ? data + 2 : nullptr;
  const size_t key_count = (length > 2) ? (length - 2) : 0;

  static uint8_t previous_keys[6] = {0};

  auto emit = [&](uint8_t key, bool pressed) {
    keyboard_cb_(key, pressed);
  };

  const uint8_t modifier_codes[] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7};
  for(size_t i = 0; i < sizeof(modifier_codes); ++i) {
    const uint8_t mask = 1U << i;
    const bool pressed = (modifiers & mask) != 0;
    emit(modifier_codes[i], pressed);
  }

  if(keys != nullptr) {
    for(size_t i = 0; i < key_count; ++i) {
      const uint8_t key = keys[i];
      if(key == 0) {
        continue;
      }
      bool already_pressed = false;
      for(uint8_t prev : previous_keys) {
        if(prev == key) {
          already_pressed = true;
          break;
        }
      }
      if(!already_pressed) {
        emit(key, true);
      }
    }

    for(uint8_t prev : previous_keys) {
      if(prev == 0) {
        continue;
      }
      bool still_pressed = false;
      for(size_t i = 0; i < key_count; ++i) {
        if(keys[i] == prev) {
          still_pressed = true;
          break;
        }
      }
      if(!still_pressed) {
        emit(prev, false);
      }
    }

    std::memset(previous_keys, 0, sizeof(previous_keys));
    const size_t copy_count = std::min<size_t>(sizeof(previous_keys), key_count);
    if(copy_count > 0) {
      std::memcpy(previous_keys, keys, copy_count);
    }
  }
}

BluetoothDeviceInfo *BluetoothManager::findDevice(const NimBLEAddress &address) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [&](const BluetoothDeviceInfo &info) {
    return info.address == address;
  });
  if(it == devices_.end()) {
    return nullptr;
  }
  return &(*it);
}

BluetoothDeviceInfo *BluetoothManager::findDevice(NimBLEClient *client) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [&](const BluetoothDeviceInfo &info) {
    return info.client == client;
  });
  if(it == devices_.end()) {
    return nullptr;
  }
  return &(*it);
}

BluetoothDeviceInfo *BluetoothManager::findDevice(NimBLERemoteCharacteristic *characteristic) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [&](const BluetoothDeviceInfo &info) {
    return info.input_characteristic == characteristic;
  });
  if(it == devices_.end()) {
    return nullptr;
  }
  return &(*it);
}
