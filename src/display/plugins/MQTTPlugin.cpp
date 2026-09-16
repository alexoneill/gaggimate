#include "MQTTPlugin.h"
#include "../core/Controller.h"
#include <ArduinoJson.h>
#include <ctime>
#include <esp_log.h>

namespace {

const String LOG_TAG = F("MQTTPlugin");

// Produces the HomeAssistant device identifier.
String deviceId() {
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    return mac;
}

// Produces the fully qualified device MQTTT topic name given a base topic name.
String topicFQN(const std::string &topic) {
    char fqn[80];
    snprintf(fqn, sizeof(fqn), "gaggimate/%s/%s", deviceId().c_str(), topic.c_str());
    return String(fqn);
}

} // namespace

bool MQTTPlugin::connect(Controller *controller) {
    const Settings settings = controller->getSettings();
    const String ip = settings.getHomeAssistantIP();
    const int haPort = settings.getHomeAssistantPort();
    const String clientId = "GaggiMate";
    const String haUser = settings.getHomeAssistantUser();
    const String haPassword = settings.getHomeAssistantPassword();

    client.begin(ip.c_str(), haPort, net);
    client.setKeepAlive(10);
    client.onMessage(std::bind(&MQTTPlugin::messageReceived, this, std::placeholders::_1, std::placeholders::_2));

    ESP_LOGI(LOG_TAG.c_str(), "Connecting to %s:%d", ip.c_str(), haPort);
    for (int i = 0; i < MQTT_CONNECTION_RETRIES; i++) {
        ESP_LOGD(LOG_TAG.c_str(), "Attempt (%d/%d)", i + 1, MQTT_CONNECTION_RETRIES);
        if (client.connect(clientId.c_str(), haUser.c_str(), haPassword.c_str())) {
            ESP_LOGI(LOG_TAG.c_str(), "Successfully connected");

            // Subscribe for control messages.
            client.subscribe(topicFQN("controller/mode/set"));
            client.subscribe(topicFQN("controller/profile/set"));
            client.subscribe(topicFQN("controller/brew_button/trigger"));

            ESP_LOGI(LOG_TAG.c_str(), "Subscribed to control topics");

            this->exportDiscovery.set(true);
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

    JsonDocument device;
    JsonDocument origin;
    JsonDocument components;

    // Device information
    device["ids"] = deviceId();
    device["name"] = "GaggiMate";
    device["mf"] = "GaggiMate";
    device["mdl"] = "GaggiMate";
    device["sn"] = deviceId();
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
    JsonDocument profile;
    JsonDocument brewSwitch;
    JsonDocument steamSwitch;

    // Boiler temperature component.
    boilerTemperature["name"] = "Boiler Temperature";
    boilerTemperature["p"] = "sensor";
    boilerTemperature["device_class"] = "temperature";
    boilerTemperature["unit_of_measurement"] = "°C";
    boilerTemperature["value_template"] = "{{ value | float | round(0) }}";
    boilerTemperature["unique_id"] = "boiler0Tmp";
    boilerTemperature["state_topic"] = topicFQN("boilers/0/temperature/state");

    // Boiler target temperature component.
    boilerTargetTemperature["name"] = "Boiler Target Temperature";
    boilerTargetTemperature["p"] = "sensor";
    boilerTargetTemperature["device_class"] = "temperature";
    boilerTargetTemperature["unit_of_measurement"] = "°C";
    boilerTargetTemperature["value_template"] = "{{ value | float | round(0) }}";
    boilerTargetTemperature["unique_id"] = "boiler0TargetTmp";
    boilerTargetTemperature["state_topic"] = topicFQN("boilers/0/targetTemperature/state");

    // Mode select.
    mode["name"] = "Mode";
    mode["p"] = "select";
    mode["unique_id"] = "mode";
    mode["state_topic"] = topicFQN("controller/mode/state");
    mode["command_topic"] = topicFQN("controller/mode/set");
    JsonArray modeOptions = mode["options"].to<JsonArray>();
    modeOptions.add("Brew");
    modeOptions.add("Grind");
    modeOptions.add("Steam");
    modeOptions.add("Water");
    modeOptions.add("Standby");

    // Profile select.
    profile["name"] = "Profile";
    profile["p"] = "select";
    profile["unique_id"] = "profile";
    profile["state_topic"] = topicFQN("controller/profile/state");
    profile["command_topic"] = topicFQN("controller/profile/set");

    // Gaggiamate allows multiple profiles with the same name, while
    // HomeAssistant expects unique options for a 'select' component. The
    // solution is therefore to use the Gaggiamate-assigned unique identifiers
    // for the profiles to differentiate. However, this is not a great UX as it
    // may not be easy to know the profile from the unique ID (esp. for the
    // 'Default' profile).
    //
    // To solve this, we use a mapping within the value_template and
    // command_template to allow the Gaggiamate to communnicate in unique ids
    // while Home Assistant displays and works with the user-friendly names. We
    // must construct a mapping on-the-fly and encode that as part of the
    // templates below.
    String profileValueTemplate = "{% set mapper = {";
    String commandValueTemplate = "{% set mapper = {";

    JsonArray profileOptions = profile["options"].to<JsonArray>();
    std::vector<String> profiles = controller->getProfileManager()->listProfiles();
    for (int i = 0; i < profiles.size(); i++) {
        Profile prof;
        if (!controller->getProfileManager()->loadProfile(profiles[i], prof)) {
            // Skip unknown profiles.
            continue;
        }

        // Simple escaping to prevent a bad JSON blob.
        String profileName = prof.label;
        profileName.replace("'", "_");
        // Another quirk, Gaggiamate allows two profiles with the same name,
        // HomeAssistant expects unique keys in the 'select' component. We can
        // simply add zero-width spaces at the end of the names to ensure that
        // no two entries are identical.
        for (int j = 0; j < i; j++) {
            profileName += "\u200B";
        }

        // Make sure we are separating consecutive entries in the mapping.
        if (i > 0) {
            profileValueTemplate += ",";
            commandValueTemplate += ",";
        }

        profileValueTemplate += "'" + profiles[i] + "': '" + profileName + "'";
        commandValueTemplate += "'" + profileName + "': '" + profiles[i] + "'";
        profileOptions.add(profileName);
    }
    profile["value_template"] = profileValueTemplate + "} %}{{ mapper[value] }}";
    profile["command_template"] = commandValueTemplate + "} %}{{ mapper[value] }}";

    // Brew momentary switch.
    //
    // Matches the behavior of the physical brew switch.
    brewSwitch["name"] = "Brew";
    brewSwitch["p"] = "button";
    brewSwitch["unique_id"] = "brew_button";
    brewSwitch["command_topic"] = topicFQN("controller/brew_button/trigger");

    // Register all components by their ID.
    cmps["boiler"] = boilerTemperature;
    cmps["boiler_target"] = boilerTargetTemperature;
    cmps["mode"] = mode;
    cmps["profile"] = profile;
    cmps["brew_button"] = brewSwitch;

    // Prepare the payload for Home Assistant discovery
    JsonDocument payload;
    payload["dev"] = device;
    payload["o"] = origin;
    payload["cmps"] = cmps;
    payload["qos"] = 2;

    char publishTopic[80];
    snprintf(publishTopic, sizeof(publishTopic), "%s/device/%s/config", haTopic.c_str(), deviceId().c_str());

    String payloadStr;
    serializeJson(payload, payloadStr);

    ESP_LOGD(LOG_TAG.c_str(), "Publishing discovery %s: %s", publishTopic, payloadStr.c_str());
    client.publish(publishTopic, payloadStr);
}

void MQTTPlugin::publish(const std::string &topic, const std::string &message) {
    if (!client.connected())
        return;

    String fqn = topicFQN(topic);
    ESP_LOGI(LOG_TAG.c_str(), "Publishing %s: %s", fqn.c_str(), message.c_str());
    client.publish(fqn, message.c_str());
}

void MQTTPlugin::setup(Controller *controller, PluginManager *pluginManager) {
    this->controller = controller;
    this->pluginManager = pluginManager;
    pluginManager->on("controller:wifi:connect", [this](const Event &) { this->hasWifi = true; });
    pluginManager->on("controller:wifi:disconnect", [this](const Event &) { this->hasWifi = false; });

    pluginManager->on("boiler:currentTemperature:change",
                      [this](Event const &event) { this->currentTemperature.set(round(event.getFloat("value"))); });
    pluginManager->on("boiler:targetTemperature:change",
                      [this](Event const &event) { this->targetTemperature.set(round(event.getFloat("value"))); });
    pluginManager->on("controller:mode:change", [this](Event const &event) { this->mode.set(event.getInt("value")); });
    pluginManager->on("profiles:profile:select",
                      [this](Event const &event) { this->profile.set(event.getString("id").c_str()); });

    // Setup some initial state.
    pluginManager->on("controller:ready", [this](const Event &) {
        // Only update profile, mode is changed soon after this event.
        this->profile.set(this->controller->getProfileManager()->getSelectedProfile().id.c_str());
        this->isReady = true;
    });
}

void MQTTPlugin::loop() {
    client.loop();

    if (!hasWifi) {
        return;
    }
    // If the client has lost connection, reconnect.
    if (!client.connected() && !connect(controller)) {
        return;
    }

    if (this->exportDiscovery.should_report()) {
        this->exportDiscovery.report(); // ignore value, report below.
        publishDiscovery(controller);
    }
    if (this->currentTemperature.should_report()) {
        String tempStr(this->currentTemperature.report(), 2);
        publish("boilers/0/temperature/state", tempStr.c_str());
    }
    if (this->targetTemperature.should_report()) {
        String tempStr(this->targetTemperature.report(), 2);
        publish("boilers/0/targetTemperature/state", tempStr.c_str());
    }
    if (this->mode.should_report()) {
        const char *modeStr = "";
        // clang-format off
        switch (this->mode.report()) {
        case MODE_STANDBY: modeStr = "Standby"; break;
        case MODE_BREW:    modeStr = "Brew";    break;
        case MODE_STEAM:   modeStr = "Steam";   break;
        case MODE_WATER:   modeStr = "Water";   break;
        case MODE_GRIND:   modeStr = "Grind";   break;
        }
        // clang-format on
        publish("controller/mode/state", modeStr);
    }
    if (this->profile.should_report()) {
        publish("controller/profile/state", this->profile.report());
    }
}

void MQTTPlugin::messageReceived(String &topic, String &payload) {
    if (!this->isReady) {
        return;
    }

    ESP_LOGI(LOG_TAG.c_str(), "Message received on %s: %s", topic.c_str(), payload.c_str());

    if (topic == topicFQN("controller/mode/set")) {
        if (payload == "Brew") {
            controller->deactivate();
            controller->setMode(MODE_BREW);
        } else if (payload == "Grind") {
            controller->setMode(MODE_GRIND);
            controller->deactivate();
        } else if (payload == "Standby") {
            controller->activateStandby();
        } else if (payload == "Steam") {
            controller->setMode(MODE_STEAM);
            controller->deactivate();
        } else if (payload == "Water") {
            controller->setMode(MODE_WATER);
            controller->deactivate();
        } else {
            ESP_LOGW(LOG_TAG, "Payload on %s unsupported: %s", topic.c_str(), payload.c_str());
        }
        return;
    }

    if (topic == topicFQN("controller/profile/set")) {
        if (controller->getProfileManager()->profileExists(payload)) {
            controller->getProfileManager()->selectProfile(payload);
        } else {
            ESP_LOGW(LOG_TAG, "Payload on %s unsupported: %s", topic.c_str(), payload.c_str());
        }
        return;
    }

    if (topic == topicFQN("controller/brew_button/trigger")) {
        if (controller->getMode() == MODE_BREW) {
            if (!controller->isActive()) {
                controller->activate();
            } else {
                controller->deactivate();
                controller->clear();
            }
        } else {
            controller->deactivate();
            controller->setMode(MODE_BREW);
        }
        return;
    }
}
