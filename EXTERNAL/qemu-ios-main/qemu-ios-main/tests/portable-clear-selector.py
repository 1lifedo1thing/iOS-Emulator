#!/usr/bin/env python3
"""Real CLI refusals and extracted actual configure argv; never configure/build."""
import ast
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'contrib/portable/build-engine.py'


class ClearSelector(unittest.TestCase):
    def test_real_cli_refuses_nonfresh_nonlinux_or_source_update(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            existing = base / 'existing'
            existing.mkdir()
            (existing / 'sentinel').write_text('preserve')
            dangling = base / 'dangling'
            dangling.symlink_to(base / 'missing')
            for destination, extra in [(existing, []), (dangling, []),
                                       (base / 'new-arm', ['--abi', 'arm64-v8a']),
                                       (base / 'new-x86', ['--abi', 'x86_64']),
                                       (base / 'new-update', ['--source-update'])]:
                result = subprocess.run([sys.executable, str(SCRIPT), str(destination),
                                         '--prefix', str(base / 'missing-deps'),
                                         '--crypto-prefix', str(base / 'missing-crypto'),
                                         '--experimental-tb-cache-clear', *extra],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('requires Linux, no source-update, and an absent fresh output', result.stderr)
                if destination not in [existing, dangling]:
                    self.assertFalse(destination.exists())
            self.assertEqual((existing / 'sentinel').read_text(), 'preserve')
            self.assertTrue(dangling.is_symlink())

    def test_actual_argv_changes_only_diagnostic_define(self):
        # Execute the exact two production AST statements that construct argv;
        # surrounding filesystem/configure/tool calls are intentionally absent.
        tree = ast.parse(SCRIPT.read_text())
        statements = [node for node in tree.body if
                      (isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and
                       target.id == 'args' for target in node.targets)) or
                      (isinstance(node, ast.If) and isinstance(node.test, ast.Attribute) and
                       ast.unparse(node.test) == 'a.experimental_tb_cache_clear')]
        self.assertEqual(len(statements), 2)
        code = compile(ast.Module(body=statements, type_ignores=[]), str(SCRIPT), 'exec')
        variants = []
        for selected in [False, True]:
            namespace = {'src': ROOT, 'prefix': Path('/fixture/deps'),
                         'a': SimpleNamespace(experimental_tb_cache_clear=selected)}
            exec(code, namespace)
            variants.append(namespace['args'])
        before, after = variants
        changed = [(old, new) for old, new in zip(before, after) if old != new]
        self.assertEqual(len(before), len(after))
        self.assertEqual(changed, [('--extra-cflags=-fPIC -DQEMU_IOS_PORTABLE_CRYPTO',
                                    '--extra-cflags=-fPIC -DQEMU_IOS_PORTABLE_CRYPTO '
                                    '-DQEMU_EXPERIMENTAL_TB_CACHE_STATS=1')])
        self.assertFalse(any('QEMU_EXPERIMENTAL_TB_JMP_CACHE_BITS' in str(arg) for arg in after))


if __name__ == '__main__':
    unittest.main()
