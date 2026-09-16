#ifndef MQTTPLUGIN_H
#define MQTTPLUGIN_H
#include "../core/Plugin.h"
#include <MQTT.h>
#include <WiFi.h>
#include <ctime>

constexpr int MQTT_CONNECTION_RETRIES = 5;
constexpr int MQTT_CONNECTION_DELAY = 1000;
constexpr int MQTT_DISCOVERY_STALE_SEC = 60;
constexpr int MQTT_STATE_STALE_SEC = 15;

namespace impl { // Internal, not part of the API.

// Helper class to track how often a reportable value changes and trigger
// reporting of that value. The goal is to report the value when either:
//   * Enough time has elapsed since the last time the value was reported
//   * The value itself changes
template <typename T> class ReportableData {
  public:
    explicit ReportableData(int max_report_interval_sec) { this->max_report_interval_sec = max_report_interval_sec; }

    // Setting the value should trigger a report event, unless the set data
    // remains the same.
    void set(T data) {
        // Only compare against data in the container if we have set it before.
        if (this->reported.has_value() && data == this->data) {
            return;
        }

        this->data = data;
        this->stale = true;
        this->reported = std::time(nullptr); // Get current timestamp
    }

    // Report the data, resetting the trackers. The user should take the result
    // and report it somewhere.
    T report() {
        this->stale = false;
        this->reported = std::time(nullptr); // Get current timestamp
        return this->data;
    }

    // If the client should call report().
    bool should_report() {
        std::time_t now = std::time(nullptr); // Get current timestamp
        return this->stale || (this->reported.has_value() && now - *this->reported > this->max_report_interval_sec);
    }

  private:
    // Ctor args.
    int max_report_interval_sec;

    // Tracking report state.
    bool stale = false;
    std::optional<std::time_t> reported = std::nullopt;
    T data;
};

} // namespace impl

class MQTTPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    bool connect(Controller *controller);
    void loop() override;

  private:
    void publish(const std::string &topic, const std::string &message);
    void publishDiscovery(Controller *controller);
    void messageReceived(String &topic, String &payload);

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    MQTTClient client;
    WiFiClient net;

    // State controlling updates.
    bool hasWifi = false;
    bool isReady = false;

    // State periodically exported.
    impl::ReportableData<bool> exportDiscovery{MQTT_DISCOVERY_STALE_SEC};
    impl::ReportableData<float> currentTemperature{MQTT_STATE_STALE_SEC};
    impl::ReportableData<float> targetTemperature{MQTT_STATE_STALE_SEC};
    impl::ReportableData<int> mode{MQTT_STATE_STALE_SEC};
    impl::ReportableData<std::string> profile{MQTT_STATE_STALE_SEC};
};

#endif // MQTTPLUGIN_H
