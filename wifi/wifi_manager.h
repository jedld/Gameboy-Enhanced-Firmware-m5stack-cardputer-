#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef ENABLE_WIFI_AIRDROP
#define ENABLE_WIFI_AIRDROP 0
#endif

#if ENABLE_WIFI_AIRDROP

class WiFiManager {
public:
  static WiFiManager &instance();

  bool initialize();
  void shutdown();

  bool startAccessPoint(const char *ssid = nullptr, const char *password = nullptr);
  void stopAccessPoint();

  bool isAccessPointActive() const { return ap_active_; }
  const char *getAccessPointSSID() const { return ap_ssid_; }
  const char *getAccessPointIP() const { return ap_ip_.c_str(); }

  void loop();

  bool isReady() const { return initialized_; }

private:
  WiFiManager() = default;

  bool initialized_ = false;
  bool ap_active_ = false;
  char ap_ssid_[33] = "Cardputer-Airdrop";
  char ap_password_[65] = "";
  std::string ap_ip_;
};

#else  // ENABLE_WIFI_AIRDROP

class WiFiManager {
public:
  static WiFiManager &instance() {
    static WiFiManager inst;
    return inst;
  }

  bool initialize() { return false; }
  void shutdown() {}

  bool startAccessPoint(const char * = nullptr, const char * = nullptr) { return false; }
  void stopAccessPoint() {}

  bool isAccessPointActive() const { return false; }
  const char *getAccessPointSSID() const { return ""; }
  const char *getAccessPointIP() const { return ""; }

  void loop() {}

  bool isReady() const { return false; }
};

#endif  // ENABLE_WIFI_AIRDROP

