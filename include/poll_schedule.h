#pragma once
#include <stdint.h>

// Main-loop-only scheduler. No cloud access or control writes here.
class PollSchedule {
    uint32_t last_=0;
    uint32_t interval_=0;
    bool scheduled_=false;
public:
    static constexpr uint32_t NormalMs=3600000;
    static constexpr uint32_t RetryMs=300000;
    uint32_t remaining(uint32_t now) const {
        if(!scheduled_)return 0;
        uint32_t elapsed=now-last_;
        return elapsed>=interval_?0:interval_-elapsed;
    }
    bool due(uint32_t now) const {return remaining(now)==0;}
    void completed(uint32_t now,bool ok){
        last_=now;interval_=ok?NormalMs:RetryMs;scheduled_=true;
    }
};
