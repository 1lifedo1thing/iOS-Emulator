#!/usr/bin/env python3
"""Notice input classification: real tiny archives, no compiler or guest."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('notices', Path(__file__).resolve().parents[1] /
                                              'contrib/portable/package-notices.py')
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class ArchiveNotices(unittest.TestCase):
    def test_actual_empty_regular_archive_is_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'libunknown.a'
            subprocess.run(['ar', 'rcs', str(path)], check=True)
            self.assertEqual(subprocess.check_output(['ar', 't', str(path)]), b'')
            expected = notices.sha(path.read_bytes())
            linked, empty = notices.notice_archive_inputs({str(path): expected})
            self.assertEqual(linked, {'libunknown.a': expected})
            self.assertEqual(empty, {'libunknown.a': {'sha256': expected, 'member_count': 0,
                                                    'input_paths': [str(path)]}})

    def test_actual_unknown_archive_with_member_requires_review(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            member = root / 'payload.txt'
            member.write_text('a real archive member, not compiled code\n')
            archive = root / 'libunknown.a'
            subprocess.run(['ar', 'rcs', str(archive), str(member)], check=True)
            self.assertEqual(subprocess.check_output(['ar', 't', str(archive)]), b'payload.txt\n')
            with self.assertRaisesRegex(ValueError, 'unmapped selected archives'):
                notices.notice_archive_inputs({str(archive): notices.sha(archive.read_bytes())})

    def test_thin_and_malformed_unknown_inputs_are_not_exempt(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / 'libunknown.a'
            for data in [b'!<thin>\n', b'', b'!<arch>\nextra']:
                archive.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'unmapped selected archives'):
                    notices.notice_archive_inputs({str(archive): notices.sha(data)})

    def test_changed_empty_archive_still_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / 'libunknown.a'
            archive.write_bytes(b'!<arch>\n')
            with self.assertRaisesRegex(ValueError, 'linked archive changed'):
                notices.notice_archive_inputs({str(archive): '0' * 64})


if __name__ == '__main__':
    unittest.main()
