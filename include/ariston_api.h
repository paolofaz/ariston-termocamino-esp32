#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
struct AristonConfig {
    String username, password, plant;
    bool dryRun = true;
};
struct AristonResult {
    const char* message;
    int verified = -1;
};
class AristonApi {
    const AristonConfig& cfg_;
    String token_;
    uint32_t blockedAt_ = 0, cooldown_ = 0;
    uint8_t throttles_ = 0;
    int request(const char* method, const String& path, const JsonDocument* body, JsonDocument& out);
    int login();
    int prepare(String& plant, JsonDocument& features, int& current, int target, bool& allowed);
    int readMode(const String& plant, const JsonDocument& features, int& current, int target, bool& allowed);
public:
    explicit AristonApi(const AristonConfig& cfg): cfg_(cfg) {}
    // target -1 is read-only. stillValid is rechecked immediately before the only control POST.
    AristonResult execute(int target, const std::function<bool()>& stillValid);
};
