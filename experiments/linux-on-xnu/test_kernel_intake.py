"""Small Git fixtures validate input identity and fail-closed intake reports."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('intake', HERE / 'kernel_intake.py')
intake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(intake)


class IntakeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'linux'
        self.root.mkdir()
        self.git('init', '-q')
        for name in intake.REQUIRED:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('/* fixture, not a working kernel */\n')
        (self.root / 'Makefile').write_text('VERSION = 6\nPATCHLEVEL = 12\nSUBLEVEL = 1\nEXTRAVERSION =\nNAME = Test kernel\n')
        self.git('add', '.')
        self.git('-c', 'user.name=Intake Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'base')
        self.old = self.git('rev-parse', 'HEAD').strip()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True)

    def test_pinned_source_ignores_uncommitted_bytes(self):
        (self.root / 'kernel/fork.c').write_text('modified working tree\n')
        report = intake.inspect(self.root)
        self.assertEqual(report['linux_makefile_version'], '6.12.1')
        self.assertTrue(report['working_tree_dirty'])
        self.assertEqual(report['source_commit'], self.old)
        self.assertEqual(report['input_file_sha256']['kernel/fork.c'], intake.digest(b'/* fixture, not a working kernel */\n'))
        for field in ('kernel_converted', 'kernel_built', 'activation_allowed', 'systemd_on_xnu_proven', 'update_compatibility_proven'):
            self.assertFalse(report[field])

    def test_update_classifies_actual_changes(self):
        (self.root / 'mm/mmap.c').write_text('new revision\n')
        self.git('add', '.')
        self.git('-c', 'user.name=Intake Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'update')
        report = intake.inspect(self.root, baseline=self.old)
        self.assertEqual(report['review_domains']['memory'], ['mm/mmap.c'])
        self.assertEqual(report['baseline_commit'], self.old)
        self.assertFalse(report['activation_allowed'])

    def test_recipe_is_data_not_executable(self):
        recipe = self.base / 'PKGBUILD'
        marker = self.base / 'must-not-exist'
        recipe.write_text(f'pkgbase=linux-aarch64\npkgver=6.12.1\npkgrel=1\nprepare() {{ touch "{marker}"; }}\n')
        result = intake.inspect(self.root, recipe=recipe)
        self.assertEqual(result['arch_recipe']['literal_fields']['pkgbase'], 'linux-aarch64')
        self.assertFalse(marker.exists())
        self.assertTrue(result['recipe_source_version_match'])
        recipe.write_text('pkgbase=linux-aarch64\npkgver=7.3.6\npkgrel=1\n')
        mismatch = intake.inspect(self.root, recipe=recipe)
        self.assertFalse(mismatch['recipe_source_version_match'])
        self.assertTrue(mismatch['input_warnings'])
        config = self.base / 'config'
        config.write_text('CONFIG_X86_64=y\n')
        with self.assertRaises(ValueError):
            intake.inspect(self.root, config=config)

    def test_cli_preserves_existing_manifest(self):
        output = self.base / 'manifest.json'
        output.write_text('sentinel')
        result = subprocess.run([sys.executable, str(HERE / 'kernel_intake.py'), '--source', str(self.root), '--output', str(output)], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(output.read_text(), 'sentinel')


if __name__ == '__main__':
    unittest.main()
