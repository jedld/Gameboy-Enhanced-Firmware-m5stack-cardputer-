#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifndef ENABLE_BLUETOOTH
#define ENABLE_BLUETOOTH 1
#endif

#if ENABLE_BLUETOOTH

#include <NimBLEDevice.h>

struct BluetoothDeviceInfo {
  NimBLEAddress address;
  std::string name;
  bool connected = false;
  bool connecting = false;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *input_characteristic = nullptr;

  std::string label() const {
    if(!name.empty()) {
      return name;
    }
    return address.toString();
  }
};

using BluetoothKeyCallback = std::function<void(uint8_t keycode, bool pressed)>;

class AdvertisedCallbacks;
class ClientCallbacks;

class BluetoothManager {
public:
  static BluetoothManager &instance();

  bool initialize();
  void shutdown();

  void startScan();
  void stopScan();

  bool isScanning() const { return scanning_; }

  const std::vector<BluetoothDeviceInfo> &devices() const { return devices_; }

  bool connectDevice(size_t index);
  void disconnectAll();

  void setKeyboardCallback(BluetoothKeyCallback cb);

  void loop();

  bool isReady() const { return initialized_; }

private:
  friend class AdvertisedCallbacks;
  friend class ClientCallbacks;
  friend void hidNotifyCallback(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool);
  friend void scanCompleteCallback(NimBLEScanResults);

  BluetoothManager() = default;

  void handleAdvertisedDevice(NimBLEAdvertisedDevice &device);
  void handleScanComplete();
  void handleClientConnect(NimBLEClient *client);
  void handleClientDisconnect(NimBLEClient *client);
  void handleHidNotification(NimBLERemoteCharacteristic *characteristic,
                             const uint8_t *data,
                             size_t length);

  BluetoothDeviceInfo *findDevice(const NimBLEAddress &address);
  BluetoothDeviceInfo *findDevice(NimBLEClient *client);
  BluetoothDeviceInfo *findDevice(NimBLERemoteCharacteristic *characteristic);

  bool initialized_ = false;
  bool scanning_ = false;
  BluetoothKeyCallback keyboard_cb_;
  std::vector<BluetoothDeviceInfo> devices_;
};

#else  // ENABLE_BLUETOOTH

struct BluetoothDeviceInfo {
  std::string name;
  bool connected = false;
  bool connecting = false;

  std::string label() const {
    if(!name.empty()) {
      return name;
    }
    return std::string();
  }
};

using BluetoothKeyCallback = std::function<void(uint8_t keycode, bool pressed)>;

class BluetoothManager {
public:
  static BluetoothManager &instance() {
    static BluetoothManager inst;
    return inst;
  }

  bool initialize() { return false; }
  void shutdown() {}

  void startScan() {}
  void stopScan() {}

  bool isScanning() const { return false; }

  const std::vector<BluetoothDeviceInfo> &devices() const { return devices_; }

  bool connectDevice(size_t) { return false; }
  void disconnectAll() {}

  void setKeyboardCallback(BluetoothKeyCallback) {}

  void loop() {}

  bool isReady() const { return false; }

private:
  BluetoothManager() = default;

  std::vector<BluetoothDeviceInfo> devices_;
};

#endif  // ENABLE_BLUETOOTH
