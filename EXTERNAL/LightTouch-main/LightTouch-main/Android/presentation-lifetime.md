# JNI presentation lifetime split

The presenter retains the current `ANativeWindow` under the native control mutex,
alongside its Surface generation, lifecycle epoch and copied frame metadata. It
then releases that mutex before window geometry, lock, pixel scaling and post.
The sole presenter owns all geometry and draw operations. A successful lock is
paired with exactly one post on the same retained window and thread, including
invalid layouts or detach during drawing. Reference release also occurs outside
the control mutex.

Window errors, presentation counters and failed-presentation cursor resets are
committed only if the pointer, generation and epoch still match. Reattaching the
same window pointer is a new generation. Pause/resume also changes the epoch and
resets the copy cursor so an unchanged cached frame can replay after Resume.
Stop changes the epoch without waiting for the presenter. A delayed attach
conversion cannot override a later native Surface request: each request reserves
its ticket before entering `ANativeWindow_fromSurface` outside the mutex.

Runtime calls and runtime lifetime stay under the control mutex. `present` is the
only writer of the fixed-size pixels vector and frame metadata; capture only
reads them while holding that mutex. The presenter's outside-mutex pixel reads
therefore remain stable until that iteration finishes. There is no second full
frame copy. Capture remains the latest copied cache, which may be older than the
engine's newest frame if a platform window operation stalls. Capture's Java
Bitmap work retains its existing locking; this change does not claim every native
control path is free of potentially blocking work.

A retained reference prevents object deletion; it does not guarantee a live
consumer or bound native lock/post duration. A detached window can finish a late
post, but it cannot update the new Surface's metrics or cursor. A permanently
blocked presenter cannot render a new Surface until its operation returns or the
worker exits. Clear/Stop never joins that thread, and the independent worker Stop
watchdog remains necessary. The existing ownerless old-view clear-after-new-view
ordering risk is separate from this native request ordering protection.

This implements a source-supported blocking-route correction. The recorded
FocusEvent ANR alone does not identify the blocked native call, and this source
change is not an actual ANR, guest rendering or lifecycle acceptance result.
Default graphics provider, guest callbacks and compiled diagnostic flags remain
unchanged.

The public NDK documents retained references and paired lock/post; its public
window interface supplies no cancellation or lock/post deadline guarantee.
See [Native Window](https://developer.android.com/ndk/reference/group/a-native-window).

## Focused ownership test

The test uses the production lifetime/stamp/post helpers and holds a synthetic
window operation behind a barrier while a controller detaches/replaces it. It
checks control admission, retained lifetime, same-address generation changes,
pause epoch changes and one post on normal/early-exit paths. It establishes those
native ownership properties, not Android or guest pixel behavior.

When a compiler slot is allocated:

```sh
c++ -std=c++17 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
  Android/scripts/tests/presentation_lifetime.cpp -o PRIVATE/presentation-lifetime
PRIVATE/presentation-lifetime
```

Also syntax-check the production JNI translation unit with the frozen Android
compile command and review its guard scopes. After integration, run a bounded
actual Surface/focus/pause/resume/Stop test with copied-frame and presentation
receipts. No synthetic test substitutes for that target validation.
