#include "../../app/src/main/cpp/presentation_lifetime.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

// This checks ownership, obsolete-result rejection and control admission with
// a window operation deliberately held behind a barrier. It proves no Android
// buffer/GL/guest rendering behavior.
namespace {
std::mutex control;
thread_local bool owns_control=false;
struct ControlLock {
    std::lock_guard<std::mutex> lock{control};
    ControlLock() { owns_control=true; }
    ~ControlLock() { owns_control=false; }
};
struct Window {
    std::atomic<int> refs{1}, acquisitions{0}, releases{0}, posts{0};
    std::thread::id drawing_thread;
};
void acquire(Window *w) {
    assert(owns_control);
    assert(w->refs.fetch_add(1)>0);
    ++w->acquisitions;
}
void release(Window *w) {
    assert(!owns_control);
    assert(w->refs.fetch_sub(1)>0);
    ++w->releases;
}
int32_t post(Window *w) {
    assert(!owns_control);
    assert(w->refs>0 && w->drawing_thread==std::this_thread::get_id());
    ++w->posts;
    return -7; // A stale post error must not reset the new presentation cursor.
}
using Snapshot=PresentationWindow<Window,acquire,release>;
using Post=PresentationPost<Window,post>;

void pending_post(bool same_pointer, bool epoch_only, bool early_return) {
    Window old, replacement;
    replacement.refs=0;
    Window *active=&old;
    uint64_t generation=1, epoch=1, cursor=23;
    bool ready=false, resume=false;
    std::mutex barrier;
    std::condition_variable changed;
    std::atomic<bool> admitted{false};
    std::thread presenter([&] {
        std::optional<Snapshot> snapshot;
        {
            ControlLock lock;
            snapshot.emplace(active,generation,epoch);
            assert(snapshot->current(active,generation,epoch));
            assert(!snapshot->current(&replacement,generation,epoch));
        }
        assert(!owns_control);
        old.drawing_thread=std::this_thread::get_id();
        {
            Post finish(snapshot->get()); // Represents a successful native lock.
            {
                std::unique_lock<std::mutex> lock(barrier);
                ready=true;changed.notify_all();
                if(!changed.wait_for(lock,std::chrono::seconds(2),[&] { return resume; }))std::abort();
            }
            if(!early_return) {
                assert(finish.finish()==-7);
                assert(finish.finish()==0); // Cannot double-post on scope exit.
            }
        }
        {
            ControlLock lock;
            if(snapshot->current(active,generation,epoch))cursor=0;
        }
        snapshot.reset(); // Last old-window reference must be released outside.
    });
    {
        std::unique_lock<std::mutex> lock(barrier);
        if(!changed.wait_for(lock,std::chrono::seconds(2),[&] { return ready; }))std::abort();
    }
    // A separate controller must finish while the platform post remains held.
    std::thread controller([&] {
        Window *retired=nullptr;
        {
            ControlLock lock;
            if(epoch_only) {
                ++epoch; // Pause/resume: same pointer and Surface generation.
            } else {
                retired=active;active=nullptr;++generation;++epoch;
                if(same_pointer) {
                    acquire(&old);active=&old;++generation;++epoch;
                } else { replacement.refs=1;active=&replacement; }
            }
            cursor=91; // New attach/epoch's state must survive the old error.
        }
        if(retired)release(retired);
        {
            std::lock_guard<std::mutex> lock(barrier);
            admitted=true;changed.notify_all();
        }
    });
    {
        std::unique_lock<std::mutex> lock(barrier);
        if(!changed.wait_for(lock,std::chrono::seconds(2),[&] { return admitted.load(); }))std::abort();
        assert(old.refs>0); // The detached drawing window is still alive.
        resume=true;changed.notify_all();
    }
    controller.join();presenter.join();
    assert(cursor==91 && old.posts==1);
    assert(old.refs==(epoch_only || same_pointer ? 1 : 0));
    if(active)release(active);
    assert(old.refs==0);
    assert(old.acquisitions==(!epoch_only && same_pointer ? 2 : 1));
    assert(old.releases==(!epoch_only && same_pointer ? 3 : 2));
    assert(replacement.refs==0);
}
}
int main() {
    pending_post(false,false,false); // Detach/new pointer while the post blocks.
    pending_post(true,false,false);  // Same address, newer generation still stale.
    pending_post(true,true,false);   // Same pointer/generation, newer epoch stale.
    pending_post(false,false,true);  // Early exit still posts exactly once.
    std::cout<<"PASS retained window lifetime, blocked-post control admission, generation/epoch and paired post\n";
}
