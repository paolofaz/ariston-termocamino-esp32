#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include "control_flow.h"
struct AristonConfig {
    String username, password, plant;
};
class AristonApi {
    const AristonConfig& cfg_;
    String token_;
    char diagnostic_[192]{};
    void trace(const char* stage,int status,int tlsError=0);
    uint32_t blockedAt_ = 0, cooldown_ = 0;
    uint8_t throttles_ = 0;
    uint32_t authBlockedAt_=0;
    bool authBlocked_=false;
    int request(const char* method, const String& path, const JsonDocument* body, JsonDocument& out,
                const std::function<bool()>* valid=nullptr, bool* attempted=nullptr);
    int login(const std::function<bool()>& valid);
    int prepare(String& plant, JsonDocument& features, int& current, int target, bool& allowed, const std::function<bool()>& valid);
    int readMode(const String& plant, const JsonDocument& features, int& current, int target, bool& allowed, const std::function<bool()>* valid=nullptr);
public:
    const char* diagnostic() const { return diagnostic_; }
    explicit AristonApi(const AristonConfig& cfg): cfg_(cfg) {}
    // target -1 is read-only. stillValid is rechecked immediately before the only control POST.
    AristonResult execute(int target, const std::function<bool()>& stillValid);
};
