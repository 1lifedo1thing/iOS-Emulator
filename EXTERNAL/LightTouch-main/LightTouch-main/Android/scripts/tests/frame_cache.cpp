#include "../../app/src/main/cpp/frame_cache.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

static lt_frame_info info(uint32_t width=320,uint32_t height=480,uint64_t sequence=1) {
    return {sizeof(lt_frame_info),1,width,height,width*4,1,sequence,99,uint64_t(width)*height*4};
}
static void no_frame_and_first_size() {
    FrameCopyCache cache;uint64_t since=0;lt_error error=LT_INIT(lt_error);
    assert(cache.pixels.empty() && cache.pixels.capacity()==0 && !cache.frame.width);
    unsigned queries=0,copies=0;
    auto none=[&](uint64_t observed,lt_frame_info &,lt_error &){assert(observed==0);++queries;return LT_AGAIN;};
    auto copy=[&](void *p,uint64_t capacity,uint64_t &next,lt_frame_info &out,lt_error &){
        ++copies;out=info();assert(capacity==out.bytes);static_cast<uint8_t *>(p)[0]=23;next=out.sequence;return LT_OK;
    };
    assert(cache.latest(since,error,none,copy,[]{})==LT_AGAIN);
    assert(queries==1 && !copies && !since && cache.pixels.capacity()==0);
    auto query=[](uint64_t observed,lt_frame_info &out,lt_error &){assert(observed==0);out=info();return LT_OK;};
    assert(cache.latest(since,error,query,copy,[]{})==LT_OK);
    assert(cache.pixels.size()==614400 && cache.pixels[0]==23 && since==1 && cache.frame.sequence==1);
    assert(cache.latest(since,error,[](uint64_t s,lt_frame_info &,lt_error &){assert(s==1);return LT_AGAIN;},copy,[]{})==LT_AGAIN);
    assert(copies==1 && cache.frame.sequence==1 && cache.pixels[0]==23);
}
static void racing_growth_commits_only_successful_copy() {
    FrameCopyCache cache;cache.pixels.resize(614400);cache.frame=info();uint64_t since=1;
    lt_error error=LT_INIT(lt_error);unsigned probes=0,copies=0,invalidated=0;
    auto query=[&](uint64_t observed,lt_frame_info &out,lt_error &){assert(observed==1);out=++probes==1?info(320,480,2):info(640,960,3);return LT_OK;};
    auto copy=[&](void *p,uint64_t capacity,uint64_t &next,lt_frame_info &out,lt_error &){
        if(++copies==1){assert(capacity==614400);next=900;return LT_INVALID;}
        assert(!cache.frame.width && capacity==2457600);out=info(640,960,4);
        static_cast<uint8_t *>(p)[0]=42;next=4;return LT_OK;
    };
    auto invalid=[&]{++invalidated;assert(!cache.frame.width && !cache.frame.bytes);};
    assert(cache.latest(since,error,query,copy,invalid)==LT_OK);
    assert(probes==2 && copies==2 && invalidated==2 && since==4);
    assert(cache.frame.width==640 && cache.frame.sequence==4 && cache.pixels[0]==42);
}
static void successive_non_power_of_two_growth_has_no_geometric_capacity() {
    FrameCopyCache cache;uint64_t since=0;lt_error error=LT_INIT(lt_error);
    for(uint32_t width:{257u,258u,259u,261u}) {
        const auto desired=info(width,257,since+1);
        assert(cache.latest(since,error,[&](uint64_t observed,lt_frame_info &out,lt_error &){assert(observed+1==desired.sequence);out=desired;return LT_OK;},
            [&](void *p,uint64_t capacity,uint64_t &next,lt_frame_info &out,lt_error &){
                assert(!cache.frame.width && capacity==desired.bytes);
                static_cast<uint8_t *>(p)[capacity-1]=31;out=desired;next=desired.sequence;return LT_OK;
            },[]{})==LT_OK);
        assert(cache.pixels.size()==desired.bytes && cache.pixels.capacity()==desired.bytes);
        assert(cache.pixels.capacity()<=LT_FRAME_CAPACITY && cache.pixels.back()==31);
    }
}
static void growth_then_no_frame_and_exhausted_copy_keep_watermark() {
    FrameCopyCache cache;cache.pixels.resize(614400);cache.frame=info();uint64_t since=1;lt_error error=LT_INIT(lt_error);
    auto larger=[](uint64_t observed,lt_frame_info &out,lt_error &){assert(observed==1);out=info(640,960,2);return LT_OK;};
    assert(cache.latest(since,error,larger,[](void *,uint64_t,uint64_t &,lt_frame_info &,lt_error &){return LT_AGAIN;},[]{})==LT_AGAIN);
    assert(since==1 && !cache.frame.width && !cache.frame.bytes && cache.pixels.size()==2457600);
    unsigned copies=0;
    assert(cache.latest(since,error,larger,[&](void *,uint64_t,uint64_t &next,lt_frame_info &,lt_error &){++copies;next=600;return LT_INVALID;},[]{})==LT_INVALID);
    assert(copies==3 && since==1 && !cache.frame.width);
}
static void invalid_metadata_never_allocates_or_copies() {
    auto good=info();
    for(unsigned fault=0;fault<6;++fault){
        auto bad=good;
        switch(fault){case 0:bad.width=0;break;case 1:bad.width=4097;break;
            case 2:bad.stride=UINT32_MAX;bad.bytes=uint64_t(bad.stride)*bad.height;break;
            case 3:bad.format=2;break;case 4:bad.bytes=LT_FRAME_CAPACITY+1ull;break;
            case 5:bad.sequence=0;break;}
        FrameCopyCache cache;uint64_t since=0;lt_error error=LT_INIT(lt_error);unsigned copies=0;
        assert(cache.latest(since,error,[&](uint64_t,lt_frame_info &out,lt_error &){out=bad;return LT_OK;},
            [&](void *,uint64_t,uint64_t &,lt_frame_info &,lt_error &){++copies;return LT_OK;},[]{})==LT_ENGINE);
        assert(!copies && !since && !cache.frame.width && cache.pixels.capacity()==0);
    }
}
static void copied_metadata_failure_invalidates_previous_cache() {
    FrameCopyCache cache;cache.pixels.resize(614400);cache.frame=info();uint64_t since=1;lt_error error=LT_INIT(lt_error);
    assert(cache.latest(since,error,[](uint64_t,lt_frame_info &out,lt_error &){out=info(320,480,2);return LT_OK;},
        [](void *,uint64_t,uint64_t &next,lt_frame_info &out,lt_error &){out=info(640,960,2);next=2;return LT_OK;},[]{})==LT_ENGINE);
    assert(since==1 && !cache.frame.width && !cache.frame.bytes);
}
static void blocked_old_window_keeps_storage_until_presenter_returns() {
    FrameCopyCache cache;cache.pixels.resize(614400);cache.frame=info();cache.pixels[0]=7;uint64_t since=1;
    std::mutex guard,barrier;std::condition_variable changed;bool waiting=false,release=false;
    uint64_t generation=1,epoch=1;unsigned staleCommits=0;
    std::thread presenter([&]{
        uint8_t *borrowed;uint64_t ownGeneration,ownEpoch;
        {std::lock_guard<std::mutex> lock(guard);borrowed=cache.pixels.data();ownGeneration=generation;ownEpoch=epoch;}
        {std::unique_lock<std::mutex> lock(barrier);waiting=true;changed.notify_all();changed.wait(lock,[&]{return release;});}
        assert(borrowed[0]==7); // Old scale/post still owns the unchanged byte storage.
        std::lock_guard<std::mutex> lock(guard);
        if(ownGeneration==generation && ownEpoch==epoch)++staleCommits;
        lt_error error=LT_INIT(lt_error);
        assert(cache.latest(since,error,[](uint64_t observed,lt_frame_info &out,lt_error &){assert(observed==1);out=info(640,960,2);return LT_OK;},
            [&](void *,uint64_t capacity,uint64_t &next,lt_frame_info &out,lt_error &){assert(!cache.frame.width && capacity==2457600);out=info(640,960,2);next=2;return LT_OK;},[]{})==LT_OK);
    });
    {std::unique_lock<std::mutex> lock(barrier);assert(changed.wait_for(lock,std::chrono::seconds(1),[&]{return waiting;}));}
    {std::lock_guard<std::mutex> lock(guard);++generation;++epoch;
        assert(cache.pixels.size()==614400 && cache.frame.width==320 && cache.pixels[0]==7);}
    {std::lock_guard<std::mutex> lock(barrier);release=true;}changed.notify_all();presenter.join();
    assert(!staleCommits && since==2 && cache.frame.width==640 && cache.pixels.size()==2457600);
}
int main(){no_frame_and_first_size();racing_growth_commits_only_successful_copy();
    successive_non_power_of_two_growth_has_no_geometric_capacity();
    growth_then_no_frame_and_exhausted_copy_keep_watermark();invalid_metadata_never_allocates_or_copies();
    copied_metadata_failure_invalidates_previous_cache();blocked_old_window_keeps_storage_until_presenter_returns();
    std::cout<<"frame cache ownership/size/retry checks passed\n";}
