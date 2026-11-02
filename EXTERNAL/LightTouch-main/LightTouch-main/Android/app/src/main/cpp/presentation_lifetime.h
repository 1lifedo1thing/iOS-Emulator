#pragma once

#include <cstdint>
#include <utility>

// Construct while the owner's mutex protects the pointer. Destroy outside that
// mutex: dropping the last platform reference may itself enter window code.
template<class Window, void (*Acquire)(Window *), void (*Release)(Window *)>
class PresentationWindow {
public:
    PresentationWindow(Window *value, uint64_t generation, uint64_t epoch)
        : value_(value), generation_(generation), epoch_(epoch) {
        if (value_) Acquire(value_);
    }
    ~PresentationWindow() { if (value_) Release(value_); }
    PresentationWindow(const PresentationWindow &) = delete;
    PresentationWindow &operator=(const PresentationWindow &) = delete;
    Window *get() const { return value_; }
    uint64_t generation() const { return generation_; }
    bool current(Window *value, uint64_t generation, uint64_t epoch) const {
        return value_ == value && generation_ == generation && epoch_ == epoch;
    }
private:
    Window *value_;
    uint64_t generation_, epoch_;
};

// Construct only after a successful platform lock, on that same thread.
// Even an early return must pair that lock with one post on the retained window.
template<class Window, int32_t (*Post)(Window *)>
class PresentationPost {
public:
    explicit PresentationPost(Window *value) : value_(value) {}
    ~PresentationPost() { if (value_) Post(value_); }
    PresentationPost(const PresentationPost &) = delete;
    PresentationPost &operator=(const PresentationPost &) = delete;
    int32_t finish() {
        Window *value=std::exchange(value_, nullptr);
        return value ? Post(value) : 0;
    }
private:
    Window *value_;
};
