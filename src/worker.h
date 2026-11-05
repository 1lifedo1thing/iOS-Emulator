// The worker: one process per running device ("iOS-Emulator.exe --worker ...").
//
// It loads the engine, boots the device and then:
//   - copies every new screen frame into the shared memory the UI created (common.h, SharedFrame);
//   - plays the guest's sound;
//   - records the screen and sound to a video when asked (recorder.h);
//   - reads commands from stdin and writes events to a pipe, one JSON object per line;
//   - hosts the USB developer services (services.h) for this device;
//   - runs the device's web proxy (proxy.h) and installs its certificate in iOS.
//
// Commands (UI -> worker), each a JSON object with a "cmd" field:
//   {"cmd":"touch","contacts":[{"id":1,"phase":0,"x":0.5,"y":0.5}]}   phase: 0 down, 1 move, 2 up, 3 cancel
//                                             Only the fingers that changed; the others stay as they are.
//                                             At most 8 changes, and the engine takes or refuses them as
//                                             one: going over the board's maxContacts fingers, or a move
//                                             or down while paused, loses the whole command (ups work
//                                             while paused). Put ups first, so their slots are free.
//   {"cmd":"button","button":0,"down":true}                           0 Home, 1 Sleep/Wake, 2 Vol+, 3 Vol-
//   {"cmd":"motion","x":0,"y":0,"z":9.8}      accelerometer, m/s^2, portrait axes (x right, y up, z out of
//                                             the screen; at rest it points up: lying flat is z 9.8, standing
//                                             upright y 9.8, on its side with Home right x 9.8). The worker
//                                             repeats the last sample until the next one (the engine
//                                             drops a sample after 250 ms); one sent before iOS is up
//                                             applies once it is
//   {"cmd":"type","text":"hello\n"}      types into the text field that has focus, through it_typein
//                                        (the guest tools' typing library); "\b" deletes a character,
//                                        "\n" is a line break (Return in a one-line field)
//   {"cmd":"pause","paused":true}   {"cmd":"mute","muted":true}   {"cmd":"internet","on":true}
//   {"cmd":"webproxy","on":true}                                      HTTPS through the web proxy's bridge
//   {"cmd":"record","to":"C:/x.mp4","landscape":false}   starts recording the screen and sound (sideways, for
//                                                        its whole length, with "landscape"); it needs .mp4
//   {"cmd":"record"}                                     ends the recording ("stop" and an exit also do)
//   {"cmd":"stop"}                                                    shuts iOS down cleanly, then exits
//   Developer requests carry an "id" and get {"event":"reply","id":..,"ok":..,"result"|"error":..} back:
//     {"cmd":"info"}                                         what lockdown reports about the device
//     {"cmd":"files.list","path":"/"}                        [{name, type, size, modified, target}]
//     {"cmd":"files.download","path":..,"to":"C:/x"}   {"cmd":"files.upload","from":"C:/x","path":..}
//     {"cmd":"files.delete","path":..}   {"cmd":"files.mkdir","path":..}   {"cmd":"files.rename","path":..,"to":..}
//     {"cmd":"apps.list"}   {"cmd":"apps.launch","bundle":..}
//     {"cmd":"apps.install","ipa":"C:/app.ipa"}              refused at once if it needs a newer iOS (ipa.h)
//     {"cmd":"packages.install","deb":"C:/tweak.deb"}        jailbroken devices: dpkg -i, undone if dependencies
//                                                            are missing, then uicache and a respring if needed
//     {"cmd":"shell","command":"ls /","sh":true}             runs as root; result {status, output}
//     {"cmd":"ui.dump"}                                      the front app's views, one per line, with their
//                                                            text (it_typein); result {views}
//     {"cmd":"syslog","on":true}                             streams "log" events
//     {"cmd":"forward","on":true,"port":2222,"devicePort":22}
//     {"cmd":"snapshot.save","name":"booted","landscape":false}   saves the machine, disk and screen to
//                                                            <device>\snapshots\<name>\ (snapshot.h); result
//                                                            {path, bytes}; needs --save-snapshots. Input and
//                                                            typing do nothing meanwhile: the guest is stopped
//
// Started with --snapshot <folder>, the worker restores that snapshot instead of booting, on a
// throwaway copy of its disk: nothing the guest writes is kept, and "stop" quits at once instead
// of shutting iOS down.
//
// Started with --load-state <folder>, it replaces the device's state\ with that save state's and
// restores the machine: the run goes on from there like any other, and "stop" shuts iOS down.
//
// Events (worker -> UI), each with an "event" field:
//   board   {machine, width, height, capabilities, maxContacts}   once, before booting
//   status  {state: starting|running|paused|stopping, sleeping, agent}   when it changes
//   reply   {id, ok, error?, result?}                     the answer to a developer request
//   progress {id, text}                                   how a long request (an install) is going
//   log     {line}                                        a system log line, while capturing
//   proxy   {status, error}                               setting up the web proxy (its certificate in iOS)
//   typing  {message}                                     what couldn't be typed, and why
//   recording {on, file?, error?}                         a recording started, or ended: saved as file, or failed
//   exit    {clean, message}                              last line before the worker exits
#pragma once

#include "common.h"

// Runs a worker with the arguments after "--worker". Returns the process exit code.
//   --device <dir>  --frames <mapping name>  --events <pipe handle>
//   [--internet] [--web-proxy] [--headphones] [--muted] [--save-snapshots]
//   [--snapshot <folder> --state <dir>]   restore a snapshot, using <dir> as the throwaway state folder
//   [--load-state <folder>]               restore a save state over the device's real state folder
int RunWorker(const std::vector<std::string> &args);
