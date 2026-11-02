# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib,json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import ui_approval as ui


class UI(unittest.TestCase):
    def controller(self, root, elapsed=220):
        class Controller:
            out=root
            def elapsed(self):return elapsed
            def write(self,name,value):(root/name).write_text(json.dumps(value))
        return Controller()

    def fixture(self, root, name='post-wait-current.png'):
        path=root/name;path.write_bytes(b'synthetic inspected PNG')
        pin=hashlib.sha256(path.read_bytes()).hexdigest()
        (root/(path.stem+'-receipt.json')).write_text(json.dumps({'elapsedSeconds':199.911,'sha256':pin,'dimensions':[1080,2424]}))
        return path,pin

    def test_absolute_current_capture_normalizes_without_rename_or_backdating(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);path,pin=self.fixture(root)
            ui.approve(self.controller(root),str(path),pin,lambda b:(1080,2424))
            gate=json.loads((root/'ui-gate.json').read_text())
            self.assertEqual(gate['image'],'post-wait-current.png');self.assertEqual(gate['captureElapsedSeconds'],199.911)
            self.assertEqual(gate['elapsedSeconds'],220);self.assertTrue(path.exists())
            self.assertFalse((root/'post-wait-ui.png').exists());self.assertFalse(gate['guestAcceptancePassed'])

    def test_relative_legacy_capture_still_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);path,pin=self.fixture(root,'postinstall-ui.png')
            ui.approve(self.controller(root),path.name,pin,lambda b:(1080,2424))
            self.assertTrue((root/'ui-gate.json').exists())

    def test_unknown_outside_symlink_and_wrong_pin_are_local_preaction_errors(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);path,pin=self.fixture(root)
            for image in (None, '../post-wait-current.png','unknown.png',str(root.parent/'post-wait-current.png')):
                with self.assertRaises(ui.UIApprovalUsageError):ui.approve(self.controller(root),image,pin,lambda b:(1080,2424))
            with self.assertRaises(ui.UIApprovalUsageError):ui.approve(self.controller(root),str(path),'0'*64,lambda b:(1080,2424))
            path.unlink();path.symlink_to(root/'outside')
            with self.assertRaises(ui.UIApprovalUsageError):ui.normalized_capture(root,str(path))
            self.assertFalse((root/'ui-gate.json').exists());self.assertFalse((root/'request-stop').exists())

    def test_original240_or_stopping_refuses_even_correct_image(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);path,pin=self.fixture(root)
            with self.assertRaises(RuntimeError):ui.approve(self.controller(root,240),str(path),pin,lambda b:(1080,2424))
            (root/'request-stop').touch()
            with self.assertRaises(RuntimeError):ui.approve(self.controller(root),str(path),pin,lambda b:(1080,2424))
            self.assertFalse((root/'ui-gate.json').exists())

    def test_capture_provenance_mismatch_is_fatal_not_usage(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);path,pin=self.fixture(root)
            (root/(path.stem+'-receipt.json')).write_text(json.dumps({'elapsedSeconds':250,'sha256':pin,'dimensions':[1080,2424]}))
            with self.assertRaises(RuntimeError) as error:ui.approve(self.controller(root),str(path),pin,lambda b:(1080,2424))
            self.assertFalse(ui.local_usage_rejection('approve-ui',error.exception))

    def test_actual_cli_error_handler_local_rejection_keeps_original_attempt_running(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);c=self.controller(root)
            self.assertTrue(ui.handle_controller_error(c,'approve-ui',ui.UIApprovalUsageError('unknown filename')))
            self.assertFalse((root/'request-stop').exists())
            self.assertTrue(json.loads((root/'approval-usage-rejected-approve-ui.json').read_text())['preactionLocalUsageError'])
            self.assertFalse(ui.handle_controller_error(c,'approve-ui',RuntimeError('original deadline')))
            self.assertTrue((root/'request-stop').exists())
            self.assertFalse(json.loads((root/'controller-failure-approve-ui.json').read_text())['guestAcceptancePassed'])

    def test_cli_classification_only_preaction_approval_does_not_cancel(self):
        for stage,error,expected in (('approve-ui',ui.UIApprovalUsageError('bad argument'),True),('diagnose',ui.UIApprovalUsageError('bad argument'),False),('approve-ui',RuntimeError('lost ownership'),False),('approve-ui',ValueError('corrupt actual PNG'),False)):
            self.assertEqual(ui.local_usage_rejection(stage,error),expected)


if __name__=='__main__':unittest.main()
