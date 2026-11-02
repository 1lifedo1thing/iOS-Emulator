# Deferred CA surface ownership across attachment changes

A framebuffer can change color attachments while remaining bound. Previously,
the renderer marked its attached IOSurface dirty only on framebuffer binding,
`glFlush` or `glFinish`. The sequence below lost the first rendered surface:

1. Draw into tracked texture A on framebuffer F.
2. Replace F's color attachment with texture B (or the drawable renderbuffer).
3. Flush: the current attachment B is marked; A remains clean and is skipped by
   the deferred surface writeback.

The attachment handlers now mark A before replacing the color attachment. They
retain the existing frame-boundary flush policy, avoiding publication during
an unfinished CA composition. Depth/stencil attachment changes do not need this
color ownership transition. A failed mark returns before changing the attachment
or the framebuffer's drawable role. Desktop GL, native GLES and EAGL use the
same corrected handlers.

The offline regression includes both production handlers and the production
marking function. It verifies that A and B both survive the deferred flush
selection, and that no guest-storage copy occurs during the attachment switch.
The exact previous handlers fail the outgoing-A assertion. The storage copy in
this test is modeled; it is neither driver rendering nor guest acceptance.

An earlier interpretation of Linux image previews as a disappearing Home icon
grid was incorrect. Independent exact pixel comparisons found the complete
icon grid and dock unchanged across the retained minute-change pairs; only
status-clock pixels differed. Those Linux frames do not establish the proposed
composition failure, and no diagnostic or behavioral patch should be inferred
from that preview interpretation.

The lost-write sequence above remains an independently reproduced source defect.
It has not been established as the cause of any particular retained guest scene.
Driver probes and actual guest regressions remain distinct from this offline
ownership test. Preserve original captures and test failures when correcting
an interpretation of their contents.
