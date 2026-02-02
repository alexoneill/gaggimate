#include "MQTTPlugin.h"
#include "../core/Controller.h"
#include <ArduinoJson.h>
#include <ctime>
#include <esp_log.h>

const String LOG_TAG = F("MQTTPlugin");

const int DISCOVERY_FREQUENCY_SEC = 60;
const int STATE_FREQUENCY_SEC = 5;

bool MQTTPlugin::connect(Controller *controller) {
    const Settings settings = controller->getSettings();
    const String ip = settings.getHomeAssistantIP();
    const int haPort = settings.getHomeAssistantPort();
    const String clientId = "GaggiMate";
    const String haUser = settings.getHomeAssistantUser();
    const String haPassword = settings.getHomeAssistantPassword();

    client.begin(ip.c_str(), haPort, net);
    client.setKeepAlive(10);
    ESP_LOGI(LOG_TAG.c_str(), "Connecting to %s:%d", ip.c_str(), haPort);
    for (int i = 0; i < MQTT_CONNECTION_RETRIES; i++) {
        ESP_LOGD(LOG_TAG.c_str(), "Attempt (%d/%d)", i + 1, MQTT_CONNECTION_RETRIES);
        if (client.connect(clientId.c_str(), haUser.c_str(), haPassword.c_str())) {
            ESP_LOGI(LOG_TAG.c_str(), "Successfully connected");
            return true;
        }
        delay(MQTT_CONNECTION_DELAY);
    }
    ESP_LOGW(LOG_TAG.c_str(), "Connection failed");
    return false;
}

void MQTTPlugin::publishDiscovery(Controller *controller) {
    if (!client.connected())
        return;
    const Settings settings = controller->getSettings();
    const String haTopic = settings.getHomeAssistantTopic();
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    const char *cmac = mac.c_str();

    JsonDocument device;
    JsonDocument origin;
    JsonDocument components;

    // Device information
    device["ids"] = cmac;
    device["name"] = "GaggiMate";
    device["mf"] = "GaggiMate";
    device["mdl"] = "GaggiMate";
    device["sn"] = cmac;
    device["sw"] = controller->getSystemInfo().version;
    device["hw"] = controller->getSystemInfo().hardware;

    // Origin information
    origin["name"] = "GaggiMate";
    origin["sw"] = controller->getSystemInfo().version;
    origin["url"] = "https://gaggimate.eu/";

    // Components information
    JsonDocument cmps;
    JsonDocument boilerTemperature;
    JsonDocument boilerTargetTemperature;
    JsonDocument mode;

    boilerTemperature["name"] = "Boiler Temperature";
    boilerTemperature["p"] = "sensor";
    boilerTemperature["device_class"] = "temperature";
    boilerTemperature["unit_of_measurement"] = "°C";
    boilerTemperature["value_template"] = "{{ value_json.temperature | round(2) }}";
    boilerTemperature["unique_id"] = "boiler0Tmp";
    boilerTemperature["state_topic"] = "gaggimate/" + String(cmac) + "/boilers/0/temperature";

    boilerTargetTemperature["name"] = "Boiler Target Temperature";
    boilerTargetTemperature["p"] = "sensor";
    boilerTargetTemperature["device_class"] = "temperature";
    boilerTargetTemperature["unit_of_measurement"] = "°C";
    boilerTargetTemperature["value_template"] = "{{ value_json.temperature | round(2) }}";
    boilerTargetTemperature["unique_id"] = "boiler0TargetTmp";
    boilerTargetTemperature["state_topic"] = "gaggimate/" + String(cmac) + "/boilers/0/targetTemperature";

    mode["name"] = "Mode";
    mode["p"] = "text";
    mode["device_class"] = "text";
    mode["value_template"] = "{{ value_json.mode_str }}";
    mode["unique_id"] = "mode";
    mode["state_topic"] = "gaggimate/" + String(cmac) + "/controller/mode";

    cmps["boiler"] = boilerTemperature;
    cmps["boiler_target"] = boilerTargetTemperature;
    cmps["mode"] = mode;

    // Prepare the payload for Home Assistant discovery
    JsonDocument payload;
    payload["dev"] = device;
    payload["o"] = origin;
    payload["cmps"] = cmps;
    payload["state_topic"] = "gaggimate/" + String(cmac) + "/state";
    payload["qos"] = 2;

    char publishTopic[80];
    snprintf(publishTopic, sizeof(publishTopic), "%s/device/%s/config", haTopic.c_str(), cmac);

    String payloadStr;
    serializeJson(payload, payloadStr);

    ESP_LOGD(LOG_TAG.c_str(), "Publishing discovery %s: %s", publishTopic, payloadStr.c_str());
    client.publish(publishTopic, payloadStr);
}

void MQTTPlugin::publish(const std::string &topic, const std::string &message) {
    if (!client.connected())
        return;
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    const char *cmac = mac.c_str();
    char publishTopic[80];
    snprintf(publishTopic, sizeof(publishTopic), "gaggimate/%s/%s", cmac, topic.c_str());

    ESP_LOGD(LOG_TAG.c_str(), "Publishing %s: %s", publishTopic, message.c_str());
    client.publish(publishTopic, message.c_str());
}

void MQTTPlugin::publishBrewState(const char *state) {
    char json[100];
    std::time_t now = std::time(nullptr); // Get current timestame
    snprintf(json, sizeof(json), R"({"state":"%s","timestamp":%ld})", state, now);
    publish("controller/brew/state", json);
}

void MQTTPlugin::setup(Controller *controller, PluginManager *pluginManager) {
    this->controller = controller;
    pluginManager->on("controller:wifi:connect", [this, controller](const Event &) {
        this->hasWifi = true;
    });
    pluginManager->on("controller:wifi:disconnect", [this, controller](const Event &) {
        this->hasWifi = false;
    });

    pluginManager->on("boiler:currentTemperature:change", [this](Event const &event) {
        this->currentTemperature = event.getFloat("value");
    });
    pluginManager->on("boiler:targetTemperature:change", [this](Event const &event) {
        this->targetTemperature = event.getFloat("value");
    });
    pluginManager->on("controller:mode:change", [this](Event const &event) {
        this->mode = event.getInt("value");
    });
    pluginManager->on("controller:brew:start", [this](Event const &) {
        this->brewing = true;
    });
    pluginManager->on("controller:brew:end", [this](Event const &) {
        this->brewing = false;
    });
}

void MQTTPlugin::loop() {
    client.loop();

    if (!hasWifi) {
      return;
    }

    std::time_t now = std::time(nullptr); // Get current timestamp

    bool updateDiscovery = false;
    if (now - lastDiscoveryTime >= DISCOVERY_FREQUENCY_SEC) {
        updateDiscovery = true;
        lastDiscoveryTime = now;
    }

    bool updateState = false;
    if (now - lastStateTime >= STATE_FREQUENCY_SEC) {
        updateState = true;
        lastStateTime = now;
    }

    if (!updateDiscovery && !updateState) {
        return;
    }

    // If the client has lost connection, reconnect.
    if (!client.connected()) {
        if (!connect(controller)) {
            return;
        }
    }

    if (updateDiscovery) {
        publishDiscovery(controller);
    }
    if (updateState) {
        if (currentTemperature.has_value()) {
            char json[50];
            snprintf(json, sizeof(json), R"***({"temperature":%02f})***", *currentTemperature);
            publish("boilers/0/temperature", json);
        }
        if (targetTemperature.has_value()) {
            char json[50];
            snprintf(json, sizeof(json), R"***({"temperature":%02f})***", *targetTemperature);
            publish("boilers/0/targetTemperature", json);
        }
        if (mode.has_value()) {
            const char *modeStr;
            switch (*mode) {
            case 0:
                modeStr = "Standby";
                break;
            case 1:
                modeStr = "Brew";
                break;
            case 2:
                modeStr = "Steam";
                break;
            case 3:
                modeStr = "Water";
                break;
            case 4:
                modeStr = "Grind";
                break;
            default:
                modeStr = "Unknown";
                break; // Fallback in case of unexpected value
            }
            char json[100];
            snprintf(json, sizeof(json), R"({"mode":%d,"mode_str":"%s"})", *mode, modeStr);
            publish("controller/mode", json);
        }
        if (brewing.has_value()) {
            char json[100];
            snprintf(json, sizeof(json), R"({"state":"%s","timestamp":%ld})", *brewing ? "brewing" : "not brewing", now);
            publish("controller/brew/state", json);
        }
    }
}
