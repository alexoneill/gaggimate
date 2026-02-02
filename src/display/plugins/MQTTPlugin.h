#ifndef MQTTPLUGIN_H
#define MQTTPLUGIN_H
#include "../core/Plugin.h"
#include <MQTT.h>
#include <WiFi.h>
#include <ctime>

constexpr int MQTT_CONNECTION_RETRIES = 5;
constexpr int MQTT_CONNECTION_DELAY = 1000;

class MQTTPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    bool connect(Controller *controller);
    void loop() override;

  private:
    void publish(const std::string &topic, const std::string &message);
    void publishBrewState(const char *state);
    void publishDiscovery(Controller *controller);
    Controller *controller = nullptr;
    MQTTClient client;
    WiFiClient net;

    // State controlling updates.
    bool hasWifi = false;
    std::time_t lastDiscoveryTime = 0;
    std::time_t lastStateTime = 0;

    // State periodically exported.
    std::optional<float> currentTemperature = std::nullopt;
    std::optional<float> targetTemperature = std::nullopt;
    std::optional<int> mode = std::nullopt;
    std::optional<bool> brewing = std::nullopt;
};

#endif // MQTTPLUGIN_H
