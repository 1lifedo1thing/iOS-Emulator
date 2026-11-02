# Wake diagnosis

`GuestWakeDiagnosisTest` operates an existing worker through production Binder
controls. It captures native sleep/backlight/agent status and actual copied frames
before and after balanced Home, Power, and Home pulses. JUnit success means the
diagnosis completed; it does not mean the guest woke or displayed Home.

With matching main and test APKs installed, foreground MainActivity before each
`--no-restart` invocation so Android's cached-process freezer does not leave the
instrumentation waiting for a foreground launch:

```sh
adb -s "$ANDROID_SERIAL" shell am start -W -n org.lighttouch.android/.MainActivity
adb -s "$ANDROID_SERIAL" shell am instrument --no-restart -w \
  -e class org.lighttouch.android.GuestWakeDiagnosisTest \
  -e diagnoseGuestWake true -e resumeExisting true -e wakePulseHoldMs 500 \
  org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner
```

The new optional `wakePulseHoldMs` accepts integers from 150 through 500 and
defaults to 150. Candidate18's actual 500ms diagnosis completed, but all four raw
captures remained black with sleep set, backlight zero, and producer frames
unchanged. Neither the longer host hold nor the unowned-resize fix resolved wake.
Each pulse records host call and return times. Host elapsed time
uses Android BOOTTIME; it does not measure the guest's virtual button dwell or
prove guest delivery. Production button timing remains unchanged. Receipt and
timeline files are under app-private `wake-evidence-*`; raw captures are under
`screenshots/<evidence-folder>/`. Keep both alongside the instrumentation output.

The retained Poco 17 diagnosis completed but all four captures remained black,
with native sleep set, backlight zero, and no new producer frames. Two viewport
resizes occurred; the final Home pulse failed without another resize. Source
review found that native full input cancellation releases hardware buttons and
their virtual release timers. An unowned viewport resize now invalidates local
touch continuation without that global cancellation. Owned-contact resize still
releases input before admitting the new viewport, and focus loss, surface
destruction, Pause, and Stop retain full cleanup. This is a partial interference
fix; actual premature button release and the complete wake cause remain unproved.
Candidate18 changed the Surface after Home only; Power and the final Home still
failed with an unchanged Surface. Balanced admission counters and a fresh agent
sample did not prove button application, virtual dwell, or display wake.

`GuestDisplayAdmissionTest` adds a production View/fake endpoint regression for an
unrelated held Home pulse during unowned resize. It also retains the held-contact
cancel-before-surface case. These tests verify Android ownership behavior, not
native button application or guest wake response.
