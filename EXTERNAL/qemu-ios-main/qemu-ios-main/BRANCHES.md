# Integration and branch policy

`main` is the sole maintained engine branch. All former port/work branches and
linked development worktrees were retired after reviewed integration and
retention of original producer commits in main history. Upstream tracking refs
and research archive tags remain references; they are not active fork lanes.

The 2026-10-16 checkpoint includes Windows WGL/storage/worker/shutdown support,
desktop EGL diagnostics and the experimental symbolic-IR cache. The cache stays
disabled and unqualified; reconciliation establishes no new guest speedup or
target qualification. Rejected CPU paths remain historical experiments.

Exact source revisions in binary receipts remain authoritative. Build artifacts,
logs, private fixtures and saved devices remain outside Git. Use temporary
branches/worktrees only for assigned independent work, integrate reviewed changes,
preserve useful build outputs and original history, then retire redundant refs.
Do not force push, delete unmatched work or upload private assets. The app fork
follows the same main-only policy.

## Device fixes retained from the previous main

The previous desktop `main` (`e169e18b17`) contained four real fixes absent from
the tested port checkpoint `3e17ae14d4`: RGB scaler source stride, A4 output
gamma/CLCD frame IRQ, M2 LCD output gamma, and 32-bit AAC output slots. This normal
merge retains all four. The sole conflict in A4 `vbl_tick` retains both the CLCD
frame interrupt update and portable visible-console painting at vsync.

Focused production slices pass under ASan/UBSan for scaler, A4/M2 gamma, LCD
readback, AAC, A4 vsync (normal/stall/Apple variants), deferred publication and
frame deduplication. Two test dependencies were updated for the combined
production code. These checks are not a full engine build, a Mac application
regression, or Linux/Android guest acceptance. Existing APK26/engine results
continue to identify their earlier exact producers. New combined engine builds
and target regression must be recorded separately. No optional upstream120Hz
refresh feature or CPU backend replacement is introduced by reconciliation.
