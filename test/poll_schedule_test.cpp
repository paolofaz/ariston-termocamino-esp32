#include "poll_schedule.h"
#include <cassert>
int main(){
    PollSchedule p;
    assert(p.due(0));
    p.completed(100,true);
    assert(!p.due(100));
    assert(p.remaining(100)==3600000);
    assert(!p.due(3600099));
    assert(p.due(3600100));
    p.completed(3600100,false);
    assert(!p.due(3900099));
    assert(p.due(3900100));
    p.completed(4000000,true);
    assert(p.due(7600000));
    p.completed(0xffffff00u,false);
    assert(!p.due(uint32_t(0xffffff00u+299999u)));
    assert(p.due(uint32_t(0xffffff00u+300000u)));
}
