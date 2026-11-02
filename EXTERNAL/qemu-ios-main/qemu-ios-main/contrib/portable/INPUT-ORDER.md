# Portable input admission and execution order

AIO inserts individual one-shot bottom halves at the head of its pending list.
When several calls arrive before one guest poll, separate callbacks execute in
reverse order within that slice. A host mutex around admission does not make
those callbacks FIFO.

The non-Apple shared engine uses one internal FIFO for portable contact batches,
input cancellation, legacy touch/sequence events, hardware buttons and generated
portable QMP lifecycle controls. A single drain invokes at most 64 callbacks per
poll and schedules another drain if work remains. It releases the queue mutex
before callbacks or disposal, including callbacks that acquire the frame mutex.
Contact request admission, ID publication and FIFO insertion share the frame
mutex interval, preserving concurrent request admission order. Coordinates,
request IDs, timestamp rejection and model report capacity rules are unchanged.

The queue does not coalesce host contact transitions. The existing digitizer
model may coalesce ordinary motion while retaining release/start reports.
`QUEUED` remains admission, and the existing latest-request status reports the
callback result. Neither establishes a driver acknowledgment or visible gesture.

VM teardown closes admission before discarding pending payloads on the VM thread
under the BQL. A detached request also checks that the session remains live.
Discarding a contact request decrements its pending count and marks the latest
request `NOT_READY`; it never marks dropped work applied. An outstanding drain
keeps its scheduled flag until it observes the empty queue. Each new guest uses
a fresh process; this is not an in-process restart interface.

Generated portable QMP methods retain their existing void signature. Admission
that races VM teardown is ignored, as it was for an already inactive VM; it is
not reported as successful guest execution. The original macOS source and its
one-shot scheduling remain unchanged. No public engine ABI was added.

Run the focused production slices:

```sh
ASAN_OPTIONS=detect_leaks=1 timeout 30s tests/slice/run.sh tests/slice/ipod-input-fifo.c
ASAN_OPTIONS=detect_leaks=1 timeout 30s tests/slice/run.sh tests/slice/ipod-portable-touch.c
```

The FIFO slice uses production AIO insertion/dequeue and queue macros, rather
than an immediate-callback stub. It checks burst BEGIN/MOVE/UP, CANCEL before
contact reuse, unchanged execution/admission errors, pending bounds, terminal
disposal, reentrant admission and bounded draining. Its Apple variant checks the
original scheduling behavior. These checks do not attribute an observed target
rejection to reordering without a target retest.
