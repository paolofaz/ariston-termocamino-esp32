#pragma once
#include <stdint.h>

// Platform-independent debouncer. First stable sample is a baseline, not an event.
enum class Edge { None, Closed, Open };
class Contact {
    bool initialized_ = false, stable_ = false, candidate_ = false, sampled_ = false;
    uint32_t since_ = 0;
    uint32_t debounce_;
public:
    explicit Contact(uint32_t debounce = 3000): debounce_(debounce) {}
    Edge sample(bool closed, uint32_t now) {
        if (!sampled_ || closed != candidate_) {
            sampled_ = true; candidate_ = closed; since_ = now;
            return Edge::None;
        }
        if (uint32_t(now - since_) < debounce_) return Edge::None;
        if (!initialized_) {
            initialized_ = true; stable_ = candidate_; return Edge::None;
        }
        if (stable_ == candidate_) return Edge::None;
        stable_ = candidate_;
        return stable_ ? Edge::Closed : Edge::Open;
    }
    bool ready() const { return initialized_; }
    bool closed() const { return stable_; }
};
#ifndef CONTACT_INVERTED
#define CONTACT_INVERTED 0
#endif
static_assert(CONTACT_INVERTED==0 || CONTACT_INVERTED==1,"Invalid contact polarity");
constexpr bool InvertedContact=CONTACT_INVERTED==1;
// Keep physical contact state separate from inferred pump state.
constexpr bool pumpOn(bool closed) { return closed!=InvertedContact; }
constexpr int targetForContact(bool closed) { return pumpOn(closed)?0:1; }
inline int targetFor(Edge edge, bool operational) {
    if (!operational || edge == Edge::None) return -1;
    return targetForContact(edge==Edge::Closed);
}
