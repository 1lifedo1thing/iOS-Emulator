#pragma once
namespace lt {
// Guarded by the runtime mutex. The publisher alone schedules backend controls.
// A pending Pause cannot be erased by a quick Resume: actual PAUSED must be
// observed before Resume is submitted, then RUNNING before capture can restart.
class VmPauseControl {
    bool desired_pause = false, pause_barrier = false, pause_sent = false, resume_sent = false;
    bool engaged = false;

  public:
    enum class Action { none, pause, resume };
    void request(bool paused) {
        engaged = true;
        desired_pause = paused;
        if (paused)
            pause_barrier = true;
    }
    void shutdown_target() {
        engaged = true; // Stop explicitly authorizes waking a paused VM.
        desired_pause = false;
        if (!pause_sent)
            pause_barrier = false; // Only an unsubmitted Pause may be discarded.
    }
    Action step(bool actual_paused, bool engine_ready) {
        if (!engine_ready || !engaged)
            return Action::none; // Do not resume an unrelated/guest-owned pause.
        if (actual_paused) {
            pause_barrier = false;
            pause_sent = false;
        } else
            resume_sent = false;
        if (desired_pause || pause_barrier) {
            if (!actual_paused && !pause_sent) {
                pause_sent = true;
                return Action::pause;
            }
        } else if (actual_paused && !resume_sent) {
            resume_sent = true;
            return Action::resume;
        }
        if (!actual_paused && !desired_pause && !pause_barrier && !pause_sent && !resume_sent)
            engaged = false;
        return Action::none;
    }
    bool capture_allowed(bool actual_paused) const {
        return !actual_paused && !desired_pause && !pause_barrier && !pause_sent && !resume_sent;
    }
};
} // namespace lt
