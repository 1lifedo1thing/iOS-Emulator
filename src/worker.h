// The worker: one process per running device ("iOS-Emulator.exe --worker ...").
//
// It loads the engine, boots the device and then:
//   - copies every new screen frame into the shared memory the UI created (common.h, SharedFrame);
//   - plays the guest's sound;
//   - reads commands from stdin and writes events to a pipe, one JSON object per line;
//   - hosts the USB developer services (services.h) for this device;
//   - runs the device's web proxy (proxy.h) and installs its certificate in iOS.
//
// Commands (UI -> worker), each a JSON object with a "cmd" field:
//   {"cmd":"touch","contacts":[{"id":1,"phase":0,"x":0.5,"y":0.5}]}   phase: 0 down, 1 move, 2 up, 3 cancel
//   {"cmd":"button","button":0,"down":true}                           0 Home, 1 Sleep/Wake, 2 Vol+, 3 Vol-
//   {"cmd":"motion","x":0,"y":0,"z":9.8}                              accelerometer, m/s^2, portrait axes
//   {"cmd":"type","text":"hello\n"}      types by tapping iOS's on-screen keyboard (keyboard.h), into the
//                                        text field that has focus; "\n" taps Return, "\b" Delete
//   {"cmd":"pause","paused":true}   {"cmd":"mute","muted":true}   {"cmd":"internet","on":true}
//   {"cmd":"webproxy","on":true}                                      HTTPS through the web proxy's bridge
//   {"cmd":"stop"}                                                    shuts iOS down cleanly, then exits
//   Developer requests carry an "id" and get {"event":"reply","id":..,"ok":..,"result"|"error":..} back:
//     {"cmd":"info"}                                         what lockdown reports about the device
//     {"cmd":"files.list","path":"/"}                        [{name, type, size, modified, target}]
//     {"cmd":"files.download","path":..,"to":"C:/x"}   {"cmd":"files.upload","from":"C:/x","path":..}
//     {"cmd":"files.delete","path":..}   {"cmd":"files.mkdir","path":..}   {"cmd":"files.rename","path":..,"to":..}
//     {"cmd":"apps.list"}   {"cmd":"apps.install","ipa":"C:/app.ipa"}   {"cmd":"apps.launch","bundle":..}
//     {"cmd":"shell","command":"ls /","sh":true}             runs as root; result {status, output}
//     {"cmd":"syslog","on":true}                             streams "log" events
//     {"cmd":"forward","on":true,"port":2222,"devicePort":22}
//     {"cmd":"snapshot.save","name":"booted"}                saves the machine to <device>\snapshots\<name>\
//                                                            (snapshot.h); result {path, bytes}; needs --save-snapshots
//
// Started with --snapshot <folder>, the worker restores that snapshot instead of booting, on a
// throwaway copy of its disk: nothing the guest writes is kept, and "stop" quits at once instead
// of shutting iOS down.
//
// Events (worker -> UI), each with an "event" field:
//   board   {machine, width, height, capabilities}       once, before booting
//   status  {state: starting|running|paused|stopping, sleeping, agent}   when it changes
//   reply   {id, ok, error?, result?}                     the answer to a developer request
//   progress {id, text}                                   how a long request (an app install) is going
//   log     {line}                                        a system log line, while capturing
//   proxy   {status, error}                               setting up the web proxy (its certificate in iOS)
//   typing  {message}                                     what couldn't be typed, and why
//   exit    {clean, message}                              last line before the worker exits
#pragma once

#include "common.h"

// Runs a worker with the arguments after "--worker". Returns the process exit code.
//   --device <dir>  --frames <mapping name>  --events <pipe handle>
//   [--internet] [--web-proxy] [--headphones] [--muted] [--save-snapshots]
//   [--snapshot <folder> --state <dir>]   restore a snapshot, using <dir> as the throwaway state folder
int RunWorker(const std::vector<std::string> &args);
