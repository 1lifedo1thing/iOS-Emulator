# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import hashlib
import json
from pathlib import Path
import signal
import shlex
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("retry", Path(__file__).resolve().parents[1]/"supervise_16k_retry.py")
retry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(retry)


class Supervision(unittest.TestCase):
    def arguments(self, directory):
        return SimpleNamespace(output=str(Path(directory)/"attempt"),adb="/synthetic/adb",
            emulator="/synthetic/emulator",avd="synthetic_16k",user="synthetic")

    def test_launch_pins_port_and_never_uses_auto_serial(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(retry.subprocess,"run",return_value=SimpleNamespace(returncode=0,stdout=b"List of devices attached\n")) as run, \
             mock.patch.object(retry.subprocess,"Popen") as popen, \
             mock.patch.object(retry.PROCESS_CONTEXT,"Process"):
            popen.return_value.pid = 987654
            attempt = retry.Attempt(self.arguments(directory))
            attempt.launch()
            self.assertEqual(run.call_args.args[0],["/synthetic/adb","devices","-l"])
            argv = popen.call_args.args[0]
            self.assertEqual(argv[argv.index("-port"):argv.index("-port")+2],["-port","5554"])
            self.assertEqual(argv[argv.index("-gpu"):argv.index("-gpu")+2],["-gpu","swiftshader"])
            self.assertEqual(argv[argv.index("-memory"):argv.index("-memory")+2],["-memory","2048"])
            self.assertEqual(argv[argv.index("-cores"):argv.index("-cores")+2],["-cores","2"])
            self.assertEqual(json.loads((attempt.output/"launch.json").read_text())["requestedGuestCores"],2)

    def test_reviewed_4096_request_is_recorded_without_heap_override(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(retry.subprocess,"run",return_value=SimpleNamespace(returncode=0,stdout=b"List of devices attached\n")), \
             mock.patch.object(retry.subprocess,"Popen") as popen, \
             mock.patch.object(retry.PROCESS_CONTEXT,"Process"):
            popen.return_value.pid=987654
            args=self.arguments(directory);args.memory_mb=4096
            attempt=retry.Attempt(args);attempt.launch()
            argv=popen.call_args.args[0]
            self.assertEqual(argv[argv.index("-memory"):argv.index("-memory")+2],["-memory","4096"])
            self.assertEqual(argv[argv.index("-cores"):argv.index("-cores")+2],["-cores","2"])
            self.assertFalse(any("heap" in a for a in argv))
            self.assertEqual(json.loads((attempt.output/"launch.json").read_text())["requestedRamMB"],4096)

    def test_unreviewed_memory_value_fails_before_any_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            args=[sys.executable,str(Path(retry.__file__)),"--allocation-granted","--user","synthetic",
                  "--adb","/synthetic/adb","--emulator","/synthetic/emulator","--fresh-library",
                  "--lock-sha256","a"*64,"--rom-sha256","b"*64,"--guest-sha256","c"*64,
                  "--output",str(Path(directory)/"attempt"),"--memory-mb","3072"]
            result=subprocess.run(args,capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertIn(b"invalid choice",result.stderr)
            self.assertFalse((Path(directory)/"attempt").exists())

    def test_existing_serial_refuses_launch_in_every_adb_state(self):
        for state in ("device","offline","unauthorized","no permissions"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as directory, \
                 mock.patch.object(retry.subprocess,"run",return_value=SimpleNamespace(returncode=0,
                    stdout=f"List of devices attached\nemulator-5554\t{state}\n".encode())), \
                 mock.patch.object(retry.subprocess,"Popen") as popen:
                attempt = retry.Attempt(self.arguments(directory))
                with self.assertRaisesRegex(RuntimeError,"already exists"):
                    attempt.launch()
                popen.assert_not_called()
                self.assertIsNone(attempt.avd)

    def test_reviewed_four_cores_changes_only_requested_cpu_argument(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(retry.subprocess,"run",return_value=SimpleNamespace(returncode=0,stdout=b"List of devices attached\n")), \
             mock.patch.object(retry.subprocess,"Popen") as popen, \
             mock.patch.object(retry.PROCESS_CONTEXT,"Process"):
            popen.return_value.pid=987654
            args=self.arguments(directory);args.memory_mb=4096;args.cores=4
            attempt=retry.Attempt(args);attempt.launch()
            self.assertEqual(popen.call_args.args[0],["sudo","-n","-u","synthetic","xvfb-run","-a","-s",
                "-screen 0 1280x1024x24","/synthetic/emulator","-avd","synthetic_16k","-port","5554",
                "-no-window","-no-snapshot","-no-audio","-gpu","swiftshader","-cores","4","-memory","4096"])
            receipt=json.loads((attempt.output/"launch.json").read_text())
            self.assertEqual((receipt["requestedGuestCores"],receipt["requestedRamMB"]),(4,4096))
            self.assertEqual((receipt["bootLimitSeconds"],receipt["stopAtSeconds"],receipt["hardLimitSeconds"]),(240,840,900))

    def test_unreviewed_cpu_value_fails_before_any_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            args=[sys.executable,str(Path(retry.__file__)),"--allocation-granted","--user","synthetic",
                  "--adb","/synthetic/adb","--emulator","/synthetic/emulator","--fresh-library",
                  "--lock-sha256","a"*64,"--rom-sha256","b"*64,"--guest-sha256","c"*64,
                  "--output",str(Path(directory)/"attempt"),"--cores","3"]
            result=subprocess.run(args,capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertIn(b"invalid choice",result.stderr)
            self.assertFalse((Path(directory)/"attempt").exists())

    def test_failed_enumeration_refuses_launch(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(retry.subprocess,"run",return_value=SimpleNamespace(returncode=1,stdout=b"")), \
             mock.patch.object(retry.subprocess,"Popen") as popen:
            with self.assertRaisesRegex(RuntimeError,"Cannot enumerate"):
                retry.Attempt(self.arguments(directory)).launch()
            popen.assert_not_called()

    def test_cleanup_handles_watchdog_that_did_not_start(self):
        for watcher in (None,SimpleNamespace(pid=None)):
            with self.subTest(watcher=watcher), tempfile.TemporaryDirectory() as directory, \
                 mock.patch.object(retry,"retain_descendants"), mock.patch.object(retry,"signal_owned"):
                attempt = retry.Attempt(self.arguments(directory))
                attempt.avd = mock.Mock(pid=987654)
                attempt.avd.poll.return_value = 0
                attempt.watcher = watcher
                attempt.adb = mock.Mock(return_value=(0,b""))
                attempt.cleanup()
                self.assertTrue((attempt.output/"terminal.json").exists())

    def test_remote_path_presence_requires_sentinel_not_adb_exit_zero(self):
        cases = [(0,b"LT_PATH_PRESENT\n",True),(0,b"LT_PATH_ABSENT\n",False),
                 (0,b"stat: No such file or directory\n",None),(0,b"LT_PATH_UNKNOWN\n",None),
                 (1,b"LT_PATH_ABSENT\n",None),(0,b"",None)]
        for code,output,expected in cases:
            with self.subTest(output=output), tempfile.TemporaryDirectory() as directory:
                attempt = retry.Attempt(self.arguments(directory))
                attempt.adb = mock.Mock(return_value=(code,output))
                self.assertIs(attempt.path_presence("files/library/records/test/state/.dirty"),expected)
                self.assertEqual(attempt.adb.call_args.args[0],"shell")
                self.assertIn("LT_PATH_PRESENT",attempt.adb.call_args.args[1])

    def test_presence_shell_handles_existing_absent_inaccessible_parent_and_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt = retry.Attempt(self.arguments(directory))
            root = Path(directory)
            (root/"present").write_text("retained")
            (root/"link").symlink_to(root/"absent")
            def shell(*argv):
                self.assertEqual(argv[0],"shell")
                words = shlex.split(argv[1])
                self.assertEqual(words[:3],["run-as",retry.PACKAGE,"sh"])
                run = subprocess.run(words[2:],capture_output=True)
                return run.returncode,run.stdout
            attempt.adb = shell
            self.assertIs(attempt.path_presence(str(root/"present")),True)
            self.assertIs(attempt.path_presence(str(root/"absent")),False)
            self.assertIs(attempt.path_presence(str(root/"missing-parent/absent")),None)
            self.assertIs(attempt.path_presence(str(root/"link")),True)

    def test_record_gate_does_not_launch_old_application(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(directory)
            args.record_id = "a"*32; args.record_sha256 = None
            lock,rom,guest = b"verified lock",b"verified ROM",b"verified guest"
            args.lock_sha256,args.rom_sha256,args.guest_sha256 = [hashlib.sha256(b).hexdigest() for b in (lock,rom,guest)]
            root = f"files/library/records/{args.record_id}"
            metadata = json.dumps({"schema":1,"id":args.record_id,"base_sha256":args.lock_sha256,
                "bootrom_sha256":args.rom_sha256,"guest_package_sha256":args.guest_sha256}).encode()
            files = {f"{root}/record.json":metadata,f"{root}/base/device.lock.json":lock,
                f"{root}/assets/bootrom.bin":rom,f"{root}/assets/guest.itpack":guest,
                "files/library/selected":args.record_id.encode()}
            attempt = retry.Attempt(args)
            attempt.path_presence = mock.Mock(side_effect=lambda path:not path.endswith("/.dirty"))
            calls = []
            def adb(*argv,**kwargs):
                calls.append(argv)
                if argv[:4] == ("exec-out","run-as",retry.PACKAGE,"cat"):
                    return 0,files[argv[4]]
                if argv == ("exec-out","run-as",retry.PACKAGE,"id","-u"):
                    return 0,b"10200\n"
                raise AssertionError(f"Unexpected controller operation in record gate: {argv}")
            attempt.adb = adb
            ready = attempt.verify_record("123")
            self.assertTrue(ready["serviceGatePassed"] and ready["recordGatePassed"])
            self.assertFalse(ready["uiGatePassed"])
            self.assertTrue(ready["visualUIInspectionRequired"])
            self.assertEqual(ready["uiGateDeadlineSeconds"],240)
            self.assertFalse(any("am" in call for call in calls))
            attempt.start=time.monotonic()-300
            with self.assertRaisesRegex(RuntimeError,"exceeded boot gate"):
                attempt.verify_record("123")
            # A separately verified import may finish after the UI boot gate;
            # it still uses the original clock and must precede Stop admission.
            bound=attempt.verify_record("123",imported_record=args.record_id,expected_uid=10200)
            self.assertEqual(bound["recordId"],args.record_id)
            self.assertTrue((attempt.output/"record-bound.json").exists())
            attempt.start=time.monotonic()-841
            with self.assertRaisesRegex(RuntimeError,"guest-start budget"):
                attempt.verify_record("123",imported_record=args.record_id,expected_uid=10200)

    def test_fresh_service_gate_needs_no_installed_app_or_record(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(directory); args.fresh_library = True
            attempt = retry.Attempt(args);attempt.start=0
            attempt.avd = mock.Mock();attempt.avd.poll.return_value=None
            attempt.logcat = mock.Mock()
            now = [0]
            def adb(*argv,**kwargs):
                self.assertNotIn("run-as",argv)
                if argv==("shell","getconf","PAGE_SIZE"):return 0,b"16384\n"
                if argv==("shell","getprop","sys.boot_completed"):return 0,b"1\n"
                if argv[:3]==("shell","service","check"):return 0,b"Service: found\n"
                if argv==("shell","pidof","system_server"):return 0,b"123\n"
                return 0,b""
            attempt.adb=adb
            with mock.patch.object(retry.time,"monotonic",side_effect=lambda:now[0]), \
                 mock.patch.object(retry.time,"sleep",side_effect=lambda _:now.__setitem__(0,now[0]+31)):
                ready=attempt.gate()
            self.assertTrue(ready["serviceGatePassed"])
            self.assertFalse(ready["recordGatePassed"] or ready["uiGatePassed"])
            self.assertIsNone(ready["recordId"]);self.assertIsNone(ready["appUid"])

    def import_attempt(self, directory, receipt_changes=None):
        args=self.arguments(directory);args.fresh_library=True
        args.lock_sha256="b"*64;args.rom_sha256="c"*64;args.guest_sha256="d"*64
        attempt=retry.Attempt(args);attempt.service_server_pid="123"
        record="a"*32;uid=10200;path="files/import-evidence-123/result.json"
        receipt={"pass":True,"recordId":record,"appUid":uid};receipt.update(receipt_changes or {})
        data=json.dumps(receipt).encode()
        command={"schema":1,"recordId":record,"appUid":uid,"importReceiptPath":path,
                 "importReceiptSha256":hashlib.sha256(data).hexdigest()}
        (attempt.output/"import-record.json").write_text(json.dumps(command))
        attempt.adb=mock.Mock(return_value=(0,data))
        attempt.verify_record=mock.Mock(return_value={})
        return attempt,command

    def test_closed_page_selector_rejects_invalid_values_before_launch(self):
        for size in (8192,0,65536):
            with self.subTest(size=size),tempfile.TemporaryDirectory() as directory:
                args=[sys.executable,str(Path(retry.__file__)),"--allocation-granted","--user","synthetic",
                    "--adb","/synthetic/adb","--emulator","/synthetic/emulator","--fresh-library",
                    "--lock-sha256","a"*64,"--rom-sha256","b"*64,"--guest-sha256","c"*64,
                    "--output",str(Path(directory)/"attempt"),"--expected-page-bytes",str(size)]
                result=subprocess.run(args,capture_output=True)
                self.assertEqual(result.returncode,2)
                self.assertIn(b"invalid choice",result.stderr)
                self.assertFalse((Path(directory)/"attempt").exists())

    def test_page_gate_default16k_explicit4k_and_exact_mismatch(self):
        for selector,actual in ((None,16384),(4096,4096),(None,4096),(4096,16384)):
            with self.subTest(selector=selector,actual=actual),tempfile.TemporaryDirectory() as directory:
                args=self.arguments(directory);args.fresh_library=True
                if selector is not None:args.expected_page_bytes=selector
                attempt=retry.Attempt(args);attempt.start=0;attempt.deadline=900
                attempt.avd=mock.Mock();attempt.avd.poll.return_value=None;attempt.logcat=mock.Mock()
                now=[0];calls=[]
                def adb(*argv,**kwargs):
                    calls.append(argv)
                    if argv==("shell","getconf","PAGE_SIZE"):return 0,str(actual).encode()+b"\n"
                    if argv==("shell","getprop","sys.boot_completed"):return 0,b"1\n"
                    if argv[:3]==("shell","service","check"):return 0,b"Service: found\n"
                    if argv==("shell","pidof","system_server"):return 0,b"123\n"
                    return 0,b""
                attempt.adb=adb;expected=16384 if selector is None else selector
                with mock.patch.object(retry.time,"monotonic",side_effect=lambda:now[0]), \
                     mock.patch.object(retry.time,"sleep",side_effect=lambda _:now.__setitem__(0,now[0]+31)):
                    if actual==expected:
                        ready=attempt.gate()
                        self.assertEqual((ready["pageBytes"],ready["expectedPageBytes"]),(actual,expected))
                        self.assertTrue(ready["serviceGatePassed"])
                        self.assertFalse(ready["uiGatePassed"] or ready["guestAcceptancePassed"])
                    else:
                        with self.assertRaisesRegex(RuntimeError,"differs from expected"):attempt.gate()
                        self.assertIsNone(attempt.actual_page_bytes)
                        self.assertFalse((attempt.output/"ready.json").exists())
                self.assertEqual((attempt.start,attempt.deadline),(0,900))
                self.assertIn(("shell","getconf","PAGE_SIZE"),calls)
                self.assertFalse(any("run-as" in argv or "am" in argv for argv in calls))

    def test_4k_selection_changes_receipt_without_launch_argument_or_deadline_changes(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(retry.subprocess,"run",return_value=SimpleNamespace(returncode=0,stdout=b"List of devices attached\n")), \
             mock.patch.object(retry.subprocess,"Popen") as popen, \
             mock.patch.object(retry.PROCESS_CONTEXT,"Process"):
            popen.return_value.pid=987654
            args=self.arguments(directory);args.expected_page_bytes=4096;args.cores=4;args.memory_mb=4096
            attempt=retry.Attempt(args);origin,deadline=attempt.start,attempt.deadline;attempt.launch()
            self.assertEqual(popen.call_args.args[0],["sudo","-n","-u","synthetic","xvfb-run","-a","-s",
                "-screen 0 1280x1024x24","/synthetic/emulator","-avd","synthetic_16k","-port","5554",
                "-no-window","-no-snapshot","-no-audio","-gpu","swiftshader","-cores","4","-memory","4096"])
            receipt=json.loads((attempt.output/"launch.json").read_text());self.assertEqual(receipt["expectedPageBytes"],4096)
            self.assertEqual((receipt["bootLimitSeconds"],receipt["stopAtSeconds"],receipt["hardLimitSeconds"]),(240,840,900))
            self.assertEqual((attempt.start,attempt.deadline),(origin,deadline))

    def test_actual_import_receipt_binds_once_after_boot_gate_without_clock_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt,command=self.import_attempt(directory)
            attempt.start=time.monotonic()-300;origin=attempt.start;deadline=attempt.deadline
            attempt.bind_imported_record()
            attempt.verify_record.assert_called_once_with("123",imported_record="a"*32,expected_uid=10200)
            self.assertEqual(attempt.record_id,"a"*32);self.assertEqual(attempt.bound_app_uid,10200)
            self.assertEqual((attempt.start,attempt.deadline),(origin,deadline))
            attempt.bind_imported_record();self.assertEqual(attempt.adb.call_count,1)
            command["recordId"]="e"*32
            (attempt.output/"import-record.json").write_text(json.dumps(command))
            with self.assertRaisesRegex(RuntimeError,"already fixed"):attempt.bind_imported_record()

    def test_failed_import_or_uid_mismatch_never_binds_controller_assertion(self):
        for changes in ({"pass":False},{"appUid":10201},{"recordId":"e"*32}):
            with self.subTest(changes=changes),tempfile.TemporaryDirectory() as directory:
                attempt,_=self.import_attempt(directory,changes)
                with self.assertRaisesRegex(RuntimeError,"did not pass"):attempt.bind_imported_record()
                self.assertIsNone(attempt.record_id);attempt.verify_record.assert_not_called()

    def test_wrong_receipt_hash_or_path_fails_before_record_admission(self):
        for change in ({"importReceiptSha256":"0"*64},{"importReceiptPath":"../private/receipt.json"}):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as directory:
                attempt,command=self.import_attempt(directory);command.update(change)
                (attempt.output/"import-record.json").write_text(json.dumps(command))
                with self.assertRaises(RuntimeError):attempt.bind_imported_record()
                self.assertIsNone(attempt.record_id);attempt.verify_record.assert_not_called()

    def test_unbound_stop_does_not_guess_dirty_record_path(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt=retry.Attempt(self.arguments(directory))
            calls=[]
            def adb(*argv,**kwargs):calls.append(argv);return 1,b""
            attempt.adb=adb;attempt.stop()
            self.assertFalse(any("/state/.dirty" in str(a) for argv in calls for a in argv))
            self.assertFalse(json.loads((attempt.output/"unbound-record-stop.json").read_text())["dirtyMarkerPathKnown"])

    def test_independent_deadline_disposes_hung_owned_child(self):
        # Actual OS subprocesses, no Android tools, compiler, window or guest.
        with tempfile.TemporaryDirectory() as directory:
            child = subprocess.Popen([sys.executable,"-c",
                "import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print('ready',flush=True); time.sleep(60)"],
                stdout=subprocess.PIPE,start_new_session=True)
            self.assertEqual(child.stdout.readline(),b"ready\n")
            done = retry.PROCESS_CONTEXT.Event()
            watcher = retry.PROCESS_CONTEXT.Process(target=retry.watchdog,
                args=(child.pid,time.monotonic()+.8,done,str(Path(directory)/"deadline.json")))
            watcher.start()
            try:
                self.assertIn(child.wait(timeout=3),(-signal.SIGTERM,-signal.SIGKILL))
                watcher.join(timeout=3)
                self.assertFalse(watcher.is_alive())
                self.assertTrue((Path(directory)/"deadline.json").exists())
            finally:
                done.set()
                if child.poll() is None:
                    child.kill()
                child.wait()
                child.stdout.close()
                watcher.join(timeout=3)

    def test_mismatching_start_ticks_cannot_signal_an_unowned_pid(self):
        child = subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])
        try:
            identity = retry.process_identity(child.pid)
            self.assertIsNotNone(identity)
            retry.signal_owned({child.pid:identity[1]+1},signal.SIGKILL)
            self.assertIsNone(child.poll())
        finally:
            child.kill(); child.wait()

    def test_clean_completion_disarms_deadline(self):
        child = subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])
        with tempfile.TemporaryDirectory() as directory:
            done = retry.PROCESS_CONTEXT.Event(); done.set()
            watcher = retry.PROCESS_CONTEXT.Process(target=retry.watchdog,
                args=(child.pid,time.monotonic(),done,str(Path(directory)/"deadline.json")))
            watcher.start(); watcher.join(timeout=3)
            try:
                self.assertFalse(watcher.is_alive())
                self.assertIsNone(child.poll())
                self.assertFalse((Path(directory)/"deadline.json").exists())
            finally:
                child.kill(); child.wait()

    def test_semantic_stop_uses_unique_enabled_control(self):
        xml = '<node package="org.lighttouch.android" clickable="true" enabled="true" bounds="[10,20][50,60]"><node text="Stop"/></node>'
        self.assertEqual(retry.semantic_stop_bounds(xml),(30,40))
        with self.assertRaises(ValueError):
            retry.semantic_stop_bounds(xml.replace('enabled="true"','enabled="false"'))
        with self.assertRaises(ValueError):
            retry.semantic_stop_bounds('<root>'+xml+xml.replace('[10,20][50,60]','[60,20][100,60]')+'</root>')
        with self.assertRaises(ValueError):
            retry.semantic_stop_bounds(xml.replace('org.lighttouch.android','another.application'))

    def test_reused_root_pid_cannot_adopt_an_unrelated_process_tree(self):
        child = subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])
        try:
            start = retry.process_identity(child.pid)[1]
            owned = {child.pid:start-1}
            retry.retain_descendants(child.pid,owned)
            self.assertEqual(owned,{child.pid:start-1})
            retry.signal_owned(owned,signal.SIGKILL)
            self.assertIsNone(child.poll())
        finally:
            child.kill(); child.wait()


if __name__ == "__main__":
    unittest.main()
