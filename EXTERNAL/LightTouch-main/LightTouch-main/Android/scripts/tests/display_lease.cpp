#include "../../app/src/main/cpp/display_lease.h"
#include <cassert>
#include <iostream>

namespace {
bool guarded=false;
struct Window { int refs=1,releases=0; };
void release(Window *window) { assert(!guarded);assert(window->refs>0);--window->refs;++window->releases; }
using Converted=ConvertedWindow<Window,release>;
void obsolete_conversion_and_release() {
    DisplayLease state;Window old, replacement;Window *active=nullptr;
    const auto a=state.claim();const auto pending_a=state.begin(a);
    {
        Converted incoming(&old); // A's conversion returns after B took over.
        const auto b=state.claim();const auto pending_b=state.begin(b);
        { Converted next(&replacement);guarded=true;
          assert(state.current(pending_b));active=next.take();guarded=false; }
        guarded=true;
        assert(!state.current(pending_a));assert(!state.release(a));assert(state.owns(b));
        guarded=false;
    }
    assert(old.refs==0 && old.releases==1);assert(active==&replacement && replacement.refs==1);
    release(active);assert(replacement.refs==0 && replacement.releases==1);
}
void retired_controls_and_duplicate_callbacks() {
    DisplayLease state;const auto a=state.claim();const auto b=state.claim();
    int native_cancel=0,native_pause=0,native_touch=0;
    // These are the same ownership predicates used inside JNI's guard.
    if(state.owns(a)){++native_cancel;++native_pause;++native_touch;}
    assert(native_cancel==0 && native_pause==0 && native_touch==0);
    assert(state.begin(a).owner==0);assert(!state.release(a));
    auto first=state.begin(b);auto duplicate=state.begin(b);
    assert(!state.current(first));assert(state.current(duplicate));
    assert(state.release(b));assert(!state.release(b));assert(!state.current(duplicate));
}
void same_worker_foreground_return_requires_fresh_native_token() {
    DisplayLease state;const auto a=state.claim();auto pending=state.begin(a);
    const auto b=state.claim();assert(!state.owns(a));
    const auto returning_a=state.claim();assert(returning_a>a && returning_a>b);
    assert(!state.current(pending));assert(!state.release(a));assert(!state.detach(b));
    assert(state.owns(returning_a) && state.current(state.begin(returning_a)));
}
void surface_loss_keeps_background_authority_but_retired_owner_cannot_pause() {
    DisplayLease state;const auto a=state.claim();auto pending=state.begin(a);
    assert(state.detach(a));assert(!state.current(pending));assert(state.owns(a));
    int pauses=0;if(state.owns(a))++pauses;assert(pauses==1); // Surface destroy then Activity stop.
    assert(state.current(state.begin(a))); // Same Activity can reattach without minting.
    const auto b=state.claim();
    assert(!state.detach(a));if(state.owns(a))++pauses;
    assert(pauses==1 && state.owns(b));assert(!state.release(a));
}
void no_contact_attach_preserves_buttons_and_held_takeover_requires_cancel() {
    DisplayLease state;state.claim();bool button_held=true;int cancellations=0;
    auto takeover=[&]{ if(state.needsOwnedRelease()){++cancellations;button_held=false;state.contactsReleased();}return state.claim(); };
    auto b=takeover();assert(cancellations==0 && button_held);
    state.begin(b);state.begin(b);assert(!state.contactsHeld() && button_held);
    state.admittedContact(7,0);state.admittedContact(23,0);state.admittedContact(7,1);
    state.admittedContact(7,2);assert(state.contactsHeld());
    b=takeover();assert(cancellations==1 && !button_held && !state.contactsHeld());
    state.beginAdmittedBatch();state.admittedContact(7,0);state.admittedContact(7,2);
    state.submittedInput(1,b);state.inputCompleted(1);button_held=true;
    takeover();assert(cancellations==1 && button_held);
}
void admitted_cancel_releases_only_its_id_and_last_cancel_preserves_new_pulse() {
    DisplayLease state;auto a=state.claim();
    state.admittedContact(7,0);state.admittedContact(23,0);
    state.admittedContact(7,3);assert(state.contactsHeld()); // ID23 remains held.
    state.admittedContact(23,3);assert(!state.contactsHeld());
    state.submittedInput(2,a);state.inputCompleted(2);assert(!state.needsOwnedRelease());
    bool power_held=true;int cancellations=0;
    // A new unrelated pulse follows the successful CANCEL admissions.
    if(state.needsOwnedRelease()){++cancellations;power_held=false;}
    assert(state.detach(a));state.claim();
    assert(cancellations==0 && power_held && !state.contactsHeld());
}
void delayed_rejection_cannot_release_new_owner_but_current_rejected_up_can() {
    DisplayLease state;auto a=state.claim();
    state.admittedContact(7,0);state.admittedContact(7,2);
    state.submittedInput(41,a); // A's queued UP may still fail with an empty ledger.
    const auto b=state.claim();bool power_held=true;int cancellations=0;
    assert(!state.reportsRejectedInput(41));
    if(state.cancelsRejectedInput(41)){++cancellations;power_held=false;state.contactsReleased();}
    assert(cancellations==0 && power_held && state.owns(b));
    state.submittedInput(42,b); // Current B admitted no held contacts.
    assert(state.reportsRejectedInput(42));assert(!state.cancelsRejectedInput(42));
    assert(power_held);state.inputCompleted(42);
    state.admittedContact(23,0);state.submittedInput(43,b);
    assert(!state.reportsRejectedInput(41));assert(state.cancelsRejectedInput(43));
    state.admittedContact(23,2);state.submittedInput(44,b);
    assert(!state.contactsHeld());assert(state.cancelsRejectedInput(44));
    state.contactsReleased();assert(!state.reportsRejectedInput(44)); // No duplicate cancel after a full release.
    state.submittedInput(45,b);state.inputCompleted(45);
    assert(!state.reportsRejectedInput(45)); // Terminal success discharges delayed cleanup.
    state.submittedInput(46,0);state.claim();
    assert(state.reportsRejectedInput(46) && state.cancelsRejectedInput(46)); // Explicit legacy/global input.
}
void pending_release_is_balanced_before_handoff_or_detach_and_completed_release_is_not_cancelled() {
    for(const int phase:{2,3})for(const bool detach:{false,true}) {
        DisplayLease state;const auto a=state.claim();bool guest_contact_held=true;int cancellations=0;
        state.admittedContact(7,0);state.admittedContact(7,phase);
        state.submittedInput(71,a); // Guest DOWN delivered, release queued but not delivered.
        assert(!state.contactsHeld() && state.pendingOwnedRelease());
        if(state.needsOwnedRelease()){++cancellations;guest_contact_held=false;state.contactsReleased();}
        if(detach)assert(state.detach(a));
        const auto b=state.claim();bool power_held=true;
        assert(!guest_contact_held && cancellations==1 && state.owns(b));
        // The queued old release now fails; it cannot issue a second cancellation.
        assert(!state.reportsRejectedInput(71) && !state.cancelsRejectedInput(71));
        assert(power_held && cancellations==1);
    }
    DisplayLease state;auto a=state.claim();state.admittedContact(7,0);state.admittedContact(7,2);
    state.submittedInput(72,a);bool guest_contact_held=false,power_held=true;
    state.inputCompleted(72); // The pre-handoff status refresh observes terminal success.
    assert(!state.needsOwnedRelease());assert(state.detach(a));state.claim();
    assert(!guest_contact_held && power_held && !state.reportsRejectedInput(72));
}
void failed_release_keeps_old_obligation_retryable_without_minting_new_owner() {
    DisplayLease state;const auto a=state.claim();state.admittedContact(7,0);
    const auto generation=state.inputGeneration(a);
    state.submittedInput(81,a);state.inputCompleted(81); // DOWN accepted; ledger is still held.
    const bool cancelled=false; // Production error branch: detach only, do not retire/mint.
    if(cancelled){state.contactsReleased();state.release(a);}else assert(state.detach(a));
    assert(state.owns(a) && state.contactsHeld() && state.needsOwnedRelease());
    assert(state.inputGeneration(a)==generation); // BUSY/failure cannot invalidate authority.
    state.contactsReleased();assert(state.release(a));const auto b=state.claim();
    assert(b==a+1 && !state.needsOwnedRelease()); // No hidden failed-claim token mint.
    state.admittedContact(7,0);state.admittedContact(7,2);state.submittedInput(82,b);
    assert(state.detach(b));assert(state.pendingOwnedRelease() && state.needsOwnedRelease());
}
void failed_rejected_up_and_touch_errors_preserve_pending_cleanup_for_next_claim() {
    DisplayLease state;const auto a=state.claim();state.admittedContact(7,0);state.admittedContact(7,2);
    state.submittedInput(91,a);assert(!state.contactsHeld());
    assert(state.reportsRejectedInput(91) && state.cancelsRejectedInput(91));
    const bool cancellation_succeeded=false;
    if(cancellation_succeeded)state.contactsReleased(); // Failure must NOT call inputCompleted.
    assert(state.pendingOwnedRelease() && state.needsOwnedRelease());
    // No-frame / out-of-viewport / immediate admission failures use this same
    // pending-aware release predicate, never ledger-only fake success.
    for(int failure=0;failure<3;++failure) {
        assert(state.needsOwnedRelease());assert(!state.contactsHeld());
        assert(state.pendingOwnedRelease());
    }
    state.contactsReleased();const auto b=state.claim();
    assert(state.owns(b) && !state.reportsRejectedInput(91));
}
void completion_covers_only_ids_in_its_own_batch() {
    for(const bool old_release_actually_succeeded:{false,true}) {
        DisplayLease state;const auto a=state.claim();
        state.beginAdmittedBatch();state.admittedContact(7,0);state.admittedContact(23,0);
        state.submittedInput(100,a);state.inputCompleted(100);
        state.beginAdmittedBatch();state.admittedContact(7,2);state.submittedInput(101,a);
        // Latest-only status never exposed UP7's outcome. Model both rejection
        // (guest7 held) and unobserved success (conservative cancellation needed).
        bool guest7_held=!old_release_actually_succeeded;
        state.beginAdmittedBatch();state.admittedContact(23,2);state.submittedInput(102,a);
        state.inputCompleted(102); // UP23 succeeded; it says nothing about UP7.
        assert(!state.contactsHeld() && state.pendingOwnedRelease());
        assert(!state.reportsRejectedInput(101));
        int cancellations=0;
        if(state.needsOwnedRelease()){++cancellations;guest7_held=false;state.contactsReleased();}
        const auto b=state.claim();bool power_held=true;
        assert(!guest7_held && cancellations==1 && state.owns(b));
        assert(!state.cancelsRejectedInput(101) && power_held); // No late second cancel.
    }
    // Observing each exact terminal success discharges only that batch's debt.
    DisplayLease state;const auto a=state.claim();
    state.beginAdmittedBatch();state.admittedContact(7,0);state.admittedContact(23,0);
    state.submittedInput(110,a);state.inputCompleted(110);
    state.beginAdmittedBatch();state.admittedContact(7,3);state.submittedInput(111,a);
    assert(state.contactsHeld() && state.pendingOwnedRelease());
    state.inputCompleted(111);assert(state.contactsHeld() && !state.pendingOwnedRelease());
    state.beginAdmittedBatch();state.admittedContact(23,2);state.submittedInput(112,a);
    state.inputCompleted(112);assert(!state.needsOwnedRelease());
    bool power_held=true;if(state.needsOwnedRelease())power_held=false;
    assert(state.detach(a));state.claim();assert(power_held);
}
void unknown_empty_release_does_not_create_debt_and_overflow_remains_conservative() {
    DisplayLease state;auto a=state.claim();
    state.beginAdmittedBatch();state.admittedContact(999,2);state.submittedInput(120,a);
    assert(state.reportsRejectedInput(120) && !state.cancelsRejectedInput(120));
    for(int32_t id=0;id<65;++id) {
        state.beginAdmittedBatch();state.admittedContact(id,0);state.submittedInput(200+id*2,a);
        state.beginAdmittedBatch();state.admittedContact(id,2);state.submittedInput(201+id*2,a);
    }
    state.inputCompleted(329);assert(state.pendingOwnedRelease());
    state.contactsReleased();assert(!state.needsOwnedRelease());
}
void successful_global_cancel_requires_fresh_generation_but_up_does_not() {
    DisplayLease state;const auto a=state.claim();const auto first=state.inputGeneration(a);
    assert(first && state.currentInput(a,first));
    state.beginAdmittedBatch();state.admittedContact(7,0);state.admittedContact(23,0);
    state.submittedInput(401,a);state.inputCompleted(401);
    state.beginAdmittedBatch();state.admittedContact(7,2);state.submittedInput(402,a);state.inputCompleted(402);
    assert(state.contactsHeld() && state.currentInput(a,first)); // Independent UP keeps23 usable.
    state.contactsReleased(); // Successful focus cancellation; loss and gain may be unpolled.
    assert(state.owns(a) && !state.currentInput(a,first) && !state.needsOwnedRelease());
    const auto resumed=state.inputGeneration(a);assert(resumed>first);
    // JNI's guarded mismatch branch admits nothing and does not cancel current input.
    state.admittedContact(23,0);bool new_contact=true,power=true;
    if(state.currentInput(a,first)){state.contactsReleased();new_contact=power=false;}
    assert(new_contact && power && state.currentInput(a,resumed));
    state.contactsReleased();const auto b=state.claim();const auto b_generation=state.inputGeneration(b);
    assert(state.inputGeneration(a)==0 && !state.currentInput(a,b_generation));
    if(state.owns(a)){state.contactsReleased();power=false;}
    assert(power && state.inputGeneration(b)==b_generation && state.currentOwner()==b);
}

}
int main() {
    obsolete_conversion_and_release();retired_controls_and_duplicate_callbacks();
    surface_loss_keeps_background_authority_but_retired_owner_cannot_pause();
    same_worker_foreground_return_requires_fresh_native_token();
    no_contact_attach_preserves_buttons_and_held_takeover_requires_cancel();
    admitted_cancel_releases_only_its_id_and_last_cancel_preserves_new_pulse();
    delayed_rejection_cannot_release_new_owner_but_current_rejected_up_can();
    pending_release_is_balanced_before_handoff_or_detach_and_completed_release_is_not_cancelled();
    failed_release_keeps_old_obligation_retryable_without_minting_new_owner();
    failed_rejected_up_and_touch_errors_preserve_pending_cleanup_for_next_claim();
    completion_covers_only_ids_in_its_own_batch();
    unknown_empty_release_does_not_create_debt_and_overflow_remains_conservative();
    successful_global_cancel_requires_fresh_generation_but_up_does_not();
    std::cout<<"PASS display lease stale controls/conversion, converted refs, contact handoff and empty callback pulse preservation\n";
}
