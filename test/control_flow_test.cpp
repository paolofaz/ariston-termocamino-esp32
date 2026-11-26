#include "control_flow.h"
#include <cassert>
int main(){
    bool valid=true, allowed=true, prepOk=true;
    int old=1, after=0, posts=0, reads=0, preps=0;
    PostKind kind=PostKind::Accepted;
    auto run=[&](int target){
        return controlFlow(target,[&](){return valid;},
            [&]()->Preparation{++preps;return {prepOk,"errore",{old,100},allowed};},
            [&](int prev)->PostResult{assert(prev==old);++posts;return {kind,"post",kind!=PostKind::NotSent};},
            [&]()->Observation{++reads;return {after,after>=0?101:0};});
    };
    assert(run(0).outcome==Outcome::Confirmed && posts==1 && reads==1);
    kind=PostKind::Uncertain; after=0;
    assert(run(0).outcome==Outcome::Confirmed && posts==2 && reads==2);
    after=-1;
    auto r=run(0);assert(r.outcome==Outcome::Uncertain && r.observation.mode==1 && r.observation.at==100 && posts==3 && reads==3);
    after=1;
    r=run(0);assert(r.outcome==Outcome::Uncertain && r.observation.at==101 && posts==4 && reads==4);
    kind=PostKind::Rejected;assert(run(0).outcome==Outcome::Rejected && posts==5 && reads==4);
    kind=PostKind::NotSent;assert(run(0).outcome==Outcome::NotSent && posts==6 && reads==4);
    assert(run(-1).outcome==Outcome::ReadOk && posts==6);
    old=0;assert(run(0).outcome==Outcome::Already && posts==6);
    old=5;assert(run(1).outcome==Outcome::Protected && posts==6);
    old=1;allowed=false;assert(run(0).outcome==Outcome::NotSent && posts==6);
    prepOk=false;assert(run(0).outcome==Outcome::NotSent && posts==6);
    valid=false;int n=preps;assert(run(0).outcome==Outcome::Cancelled && preps==n);
    valid=true;
    r=controlFlow(0,[&](){return valid;},[&]()->Preparation{valid=false;return {true,"",{1,100},true};},
        [&](int)->PostResult{assert(false);return {PostKind::Accepted,"",true};},
        [&]()->Observation{assert(false);return {};});
    assert(r.outcome==Outcome::Cancelled);
}
