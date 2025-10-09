#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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
