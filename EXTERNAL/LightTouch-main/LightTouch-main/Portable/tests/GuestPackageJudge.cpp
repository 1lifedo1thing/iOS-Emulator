#include "../src/GuestPackageJudge.hpp"
#include <cassert>

int main() {
    using lt::GuestPackageJudge;
    using lt::GuestVerdicts;
    using V = GuestPackageJudge::Verdict;
    constexpr uint64_t second = 1'000'000'000ull;
    // Good once the agent has answered for 10 s straight after the loader reported; once only.
    GuestPackageJudge healthy;
    assert(healthy.observe(0, true, 0, false, -1) == V::none);
    for (uint64_t t = 1; t < 30; t++)
        assert(healthy.observe(t * second, true, 1, false, -1) == V::none); // No report: no verdict.
    assert(healthy.observe(31 * second, true, 1, true, 27) == V::good);     // Healthy 31 s by now.
    assert(healthy.serial() == 27);
    assert(healthy.observe(32 * second, true, 1, true, 27) == V::none);

    // A stale or absent agent restarts the 10 s; a paused guest neither heals nor sickens.
    GuestPackageJudge flaky;
    flaky.observe(0, true, 1, true, 5);
    assert(flaky.observe(9 * second, true, 1, true, 5) == V::none);
    assert(flaky.observe(10 * second, true, 2, true, 5) == V::none);
    assert(flaky.observe(19 * second, true, 1, true, 5) == V::none);
    assert(flaky.observe(1000 * second, false, 1, true, 5) == V::none); // Paused for 981 s.
    assert(flaky.observe(1001 * second, true, 1, true, 5) == V::none);
    assert(flaky.observe(1011 * second, true, 1, true, 5) == V::good);

    // Bad after 300 s of running time without 10 healthy seconds.
    GuestPackageJudge sick;
    sick.observe(0, true, 0, true, 9);
    assert(sick.observe(299 * second, true, 0, true, 9) == V::none);
    assert(sick.observe(300 * second, true, 0, true, 9) == V::bad);
    assert(sick.observe(400 * second, true, 1, true, 9) == V::none);

    // The record: one good serial, a bounded bad list, the good one and the seed never bad.
    GuestVerdicts v;
    assert(v.judge(V::bad, 26, 1));
    assert(!v.judge(V::bad, 26, 1));
    assert(!v.judge(V::bad, 1, 1));
    assert(v.judge(V::good, 26, 1)); // Called good later: off the bad list.
    assert(v.good == 26 && v.bad.empty());
    assert(!v.judge(V::bad, 26, 1));
    assert(!v.judge(V::good, 26, 1));
    for (int s = 100; s < 120; s++)
        v.judge(V::bad, s, 1);
    assert(v.bad.size() == GuestVerdicts::max_bad && v.bad.front() == 104);
    auto back = GuestVerdicts::parse(v.text());
    assert(back.good == 26 && back.bad == v.bad);
    assert(GuestVerdicts::parse("good 3\nbad 4\n").text("verdict ") == "verdict good 3\nverdict bad 4\n");
    assert(GuestVerdicts::parse("").text().empty());
    return 0;
}
