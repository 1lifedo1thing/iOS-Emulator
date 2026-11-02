#pragma once
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace lt {
// One boot's verdict on the guest package the guest's loader (qemu-ios contrib/it-boot) reports
// running, as the macOS app's GuestPackage.verdict gives it: good once the guest has been healthy
// (its agent answering) for 10 s; bad if it never got there in 300 s of running time. The verdicts
// go into the next boots' offers. it_boot reverts a package no verdict has called good after two
// boots (MAX_TRIES) and refuses it from then on, so without them an updated guest package lasts
// two boots on these hosts. Time only counts while the guest runs: a paused guest is not sick.
class GuestPackageJudge {
public:
    enum class Verdict { none, good, bad };
    static constexpr uint64_t good_after_ns = 10'000'000'000ull, bad_after_ns = 300'000'000'000ull;
    // agent: qemu_ios_agent_status (1 alive); reported/serial: qemu_ios_guest_package_report.
    // Returns a verdict at most once per boot.
    Verdict observe(uint64_t now, bool running, uint32_t agent, bool reported, int64_t serial) {
        uint64_t step = have_last_ && now > last_ ? now - last_ : 0;
        have_last_ = true;
        last_ = now;
        if (done_ || !running) {
            healthy_ns_ = 0;
            return Verdict::none;
        }
        elapsed_ns_ += step;
        healthy_ns_ = agent == 1 ? healthy_ns_ + step : 0;
        if (!reported)
            return Verdict::none;
        serial_ = serial;
        if (healthy_ns_ >= good_after_ns) {
            done_ = true;
            return Verdict::good;
        }
        if (elapsed_ns_ >= bad_after_ns) {
            done_ = true;
            return Verdict::bad;
        }
        return Verdict::none;
    }
    int64_t serial() const { return serial_; }
private:
    bool have_last_ = false, done_ = false;
    uint64_t last_ = 0, elapsed_ns_ = 0, healthy_ns_ = 0;
    int64_t serial_ = -1;
};

// The verdicts a device keeps (state/guest-verdicts, "good N" / "bad N" lines), as the macOS app keeps
// its record's guest.lastGood and guest.bad: the last good serial and every bad one. A good verdict
// clears that serial from the bad list; the last good serial and the seed are never called bad (it_boot
// will not let the seed go anyway).
struct GuestVerdicts {
    static constexpr size_t max_bad = 16; // it_boot keeps 16 (MAX_SERIALS)
    int64_t good = -1;
    std::vector<int64_t> bad;
    static GuestVerdicts parse(const std::string &text) {
        GuestVerdicts v;
        std::istringstream in(text);
        std::string kind;
        long long serial;
        while (in >> kind >> serial) {
            if (kind == "good")
                v.judge(GuestPackageJudge::Verdict::good, serial, -1);
            else if (kind == "bad")
                v.judge(GuestPackageJudge::Verdict::bad, serial, -1);
        }
        return v;
    }
    // True when it changed anything.
    bool judge(GuestPackageJudge::Verdict verdict, int64_t serial, int64_t seed) {
        if (serial < 0)
            return false;
        bool listed = std::find(bad.begin(), bad.end(), serial) != bad.end();
        if (verdict == GuestPackageJudge::Verdict::good) {
            if (good == serial && !listed)
                return false;
            good = serial;
            bad.erase(std::remove(bad.begin(), bad.end(), serial), bad.end());
            return true;
        }
        if (verdict != GuestPackageJudge::Verdict::bad || listed || serial == good || serial == seed)
            return false;
        bad.push_back(serial);
        if (bad.size() > max_bad)
            bad.erase(bad.begin());
        return true;
    }
    std::string text(const char *prefix = "") const {
        std::string out;
        if (good >= 0)
            out += prefix + std::string("good ") + std::to_string(good) + "\n";
        for (auto s : bad)
            out += prefix + std::string("bad ") + std::to_string(s) + "\n";
        return out;
    }
};
}
