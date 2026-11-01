import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('asset_audit', Path(__file__).resolve().parents[1] / 'tools' / 'asset_audit.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class AssetAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'input'
        self.root.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def test_deterministic_hashes_and_signature_not_validation(self):
        sample = b'\x89PNG\r\n\x1a\n' + b'invalid synthetic png data'
        (self.root / 'z.png').write_bytes(sample)
        (self.root / 'a.dds').write_bytes(b'not DDS')
        first = audit.audit(self.root)
        self.assertTrue(first['complete'], first['diagnostics'])
        self.assertEqual(first, audit.audit(self.root))
        self.assertEqual([x['path'] for x in first['files']], ['a.dds', 'z.png'])
        self.assertEqual(first['files'][1]['sha256'], hashlib.sha256(sample).hexdigest())
        self.assertEqual(first['files'][1]['signature_hint'], 'png-signature')
        self.assertFalse(first['files'][1]['validated_asset'])
        self.assertEqual(first['files'][0]['signature_hint'], 'unknown')

    def test_empty_file_and_bounded_header(self):
        (self.root / 'empty').write_bytes(b'')
        (self.root / 'large').write_bytes(b'x' * (audit.BLOCK_SIZE + 50))
        result = audit.audit(self.root)
        self.assertTrue(result['complete'], result['diagnostics'])
        self.assertEqual(result['files'][0]['sha256'], hashlib.sha256(b'').hexdigest())
        self.assertEqual(len(result['files'][1]['first_bytes_hex']), 128)

    def test_size_and_total_caps_report_incomplete(self):
        (self.root / 'a').write_bytes(b'12345')
        (self.root / 'b').write_bytes(b'12345')
        report = audit.audit(self.root, max_file_bytes=4)
        self.assertFalse(report['complete'])
        self.assertEqual(report['bytes_hashed'], 0)
        report = audit.audit(self.root, max_total_bytes=5)
        self.assertFalse(report['complete'])
        self.assertEqual(report['bytes_hashed'], 5)

    def test_entry_and_depth_limits(self):
        (self.root / 'a').mkdir()
        (self.root / 'a' / 'b').mkdir()
        (self.root / 'a' / 'b' / 'data').write_bytes(b'example')
        self.assertFalse(audit.audit(self.root, max_entries=1)['complete'])
        self.assertFalse(audit.audit(self.root, max_depth=1)['complete'])

    def test_output_cannot_modify_input_or_child(self):
        (self.root / 'original').write_bytes(b'unchanged')
        for target in [self.root / 'report.json', self.root / 'original', self.root / 'nested' / 'report.json']:
            with self.assertRaises(ValueError):
                audit.write_report(self.root, target, {})
        self.assertEqual((self.root / 'original').read_bytes(), b'unchanged')
        output = self.base / 'report.json'
        audit.write_report(self.root, output, audit.audit(self.root))
        self.assertTrue(json.loads(output.read_text())['complete'], json.loads(output.read_text())['diagnostics'])

    def test_sibling_with_shared_prefix_is_allowed(self):
        output_dir = self.base / 'input-reports'
        output_dir.mkdir()
        audit.write_report(self.root, output_dir / 'report.json', {})
        self.assertTrue((output_dir / 'report.json').is_file())

    def test_symlink_is_reported_and_never_hashed(self):
        target = self.base / 'private'
        target.mkdir()
        (target / 'never-read').write_bytes(b'external synthetic data')
        try:
            (self.root / 'linked').symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            self.skipTest('Symlink creation unavailable on this host: ' + str(exc))
        report = audit.audit(self.root)
        self.assertFalse(report['complete'])
        self.assertEqual(report['files'], [])
        self.assertIn('reparse', report['diagnostics'][0]['error'])
        with self.assertRaises(ValueError):
            audit.audit(self.root / 'linked')

    @unittest.skipUnless(os.name == 'nt', 'Windows timestamp semantics')
    def test_windows_identity_does_not_compare_deprecated_ctime(self):
        from types import SimpleNamespace
        first = SimpleNamespace(st_dev=1, st_ino=2, st_size=3, st_mtime_ns=4, st_ctime_ns=5)
        second = SimpleNamespace(st_dev=1, st_ino=2, st_size=3, st_mtime_ns=4, st_ctime_ns=6)
        self.assertEqual(audit.identity(first), audit.identity(second))
        second.st_mtime_ns = 7
        self.assertNotEqual(audit.identity(first), audit.identity(second))

    def test_windows_reparse_attribute(self):
        info = mock.Mock(st_mode=0, st_file_attributes=0x400)
        self.assertTrue(audit.is_link(info))

    def test_opened_path_verification_failure_is_reported(self):
        (self.root / 'file').write_bytes(b'anything')
        with mock.patch.object(audit, 'verify_open_path', side_effect=OSError('path differs')):
            report = audit.audit(self.root)
        self.assertFalse(report['complete'])
        self.assertEqual(report['bytes_hashed'], 0)
        self.assertEqual(report['files'], [])

    def test_growth_during_hash_is_rejected(self):
        target = self.root / 'changing'
        target.write_bytes(b'hello')
        before = target.stat()
        original_read = os.read
        changed = False
        def read_and_change(fd, count):
            nonlocal changed
            chunk = original_read(fd, count)
            if not changed:
                changed = True
                with target.open('ab') as writer:
                    writer.write(b'growth')
            return chunk
        with mock.patch.object(audit.os, 'read', side_effect=read_and_change):
            with self.assertRaises(OSError):
                audit.hash_file(target, before, 1024, 1024)

    def test_non_regular_object_is_reported(self):
        import stat
        from types import SimpleNamespace
        target = self.root / 'special'
        target.write_bytes(b'never read')
        original_lstat = Path.lstat
        def inspected(item, *args, **kwargs):
            if item == target:
                return SimpleNamespace(st_mode=stat.S_IFIFO, st_file_attributes=0)
            return original_lstat(item, *args, **kwargs)
        with mock.patch.object(Path, 'lstat', inspected):
            report = audit.audit(self.root)
        self.assertFalse(report['complete'])
        self.assertEqual(report['files'], [])
        self.assertIn('non-regular', report['diagnostics'][0]['error'])

    def test_failed_hash_consumes_read_budget(self):
        (self.root / 'a').write_bytes(b'12345')
        (self.root / 'b').write_bytes(b'12345')
        real_hash = audit.hash_file
        def consume_then_fail(item, before, max_file_bytes, budget, on_read):
            result = real_hash(item, before, max_file_bytes, budget, on_read)
            raise OSError('Synthetic failure after reading')
        with mock.patch.object(audit, 'hash_file', side_effect=consume_then_fail):
            report = audit.audit(self.root, max_total_bytes=5)
        self.assertFalse(report['complete'])
        self.assertEqual(report['bytes_read'], 5)
        self.assertEqual(report['bytes_hashed'], 0)
        self.assertEqual(len(report['diagnostics']), 2)

    def test_bad_parameters_and_cli_exit_codes(self):
        for value in [0, -1, 0.5, True]:
            with self.assertRaises(ValueError):
                audit.audit(self.root, max_entries=value)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(audit.main([str(self.root), '--output', str(self.base / 'ok.json')]), 0)
            (self.root / 'large').write_bytes(b'123')
            self.assertEqual(audit.main([str(self.root), '--output', str(self.base / 'partial.json'), '--max-file-bytes', '1']), 1)
            self.assertEqual(audit.main([str(self.root), '--output', str(self.root / 'bad.json')]), 2)
            self.assertEqual(audit.main([str(self.base / 'missing'), '--output', str(self.base / 'none.json')]), 2)
            with self.assertRaises(SystemExit) as caught:
                audit.main([str(self.root), '--output', str(self.base / 'none.json'), '--max-depth', '0'])
            self.assertEqual(caught.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
