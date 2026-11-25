#include "automation.h"
#include <cassert>
int main() {
    Contact c(3000);
    assert(c.sample(false,0)==Edge::None);
    assert(c.sample(false,3000)==Edge::None && c.ready());
    assert(c.sample(true,4000)==Edge::None);
    assert(c.sample(false,4100)==Edge::None);
    assert(c.sample(true,4200)==Edge::None);
    assert(c.sample(true,7199)==Edge::None);
    assert(c.sample(true,7200)==Edge::Closed);
    assert(c.sample(true,8000)==Edge::None);
    assert(c.sample(false,9000)==Edge::None);
    assert(c.sample(false,12000)==Edge::Open);
    assert(targetFor(Edge::Closed,true)==0);
    assert(targetFor(Edge::Open,true)==1);
    assert(targetFor(Edge::Closed,false)==-1);
    assert(targetFor(Edge::Open,false)==-1);
    assert(targetFor(Edge::None,true)==-1);
    Contact reboot;
    assert(reboot.sample(true,0)==Edge::None);
    assert(reboot.sample(true,3000)==Edge::None && reboot.closed());
    Contact wrap(3000);
    assert(wrap.sample(false,0xffffff00)==Edge::None);
    assert(wrap.sample(false,2744)==Edge::None && wrap.ready());
    Contact summer;
    summer.sample(false,0); summer.sample(false,3000);
    summer.sample(true,4000);
    assert(targetFor(summer.sample(true,7000),false)==-1);
    assert(targetFor(summer.sample(true,8000),true)==-1);
}
