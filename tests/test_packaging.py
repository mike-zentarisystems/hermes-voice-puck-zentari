"""Release trust-boundary tests: drift, symlinks, excluded runtime data and byte stability."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_release import check, manifest
from package_release import package


class PackagingTests(unittest.TestCase):
    def record(self, root):
        errors, entries = check(root)
        self.assertEqual(errors, [])
        (root / 'RELEASE-MANIFEST.json').write_text(json.dumps(manifest(root, entries)), encoding='utf-8')

    def test_deterministic_archive_excludes_runtime_and_git(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp); root = base / 'source'; root.mkdir()
            (root / 'README.md').write_text('Public source\n')
            for name in ('.runtime', '.git', 'node_modules'):
                (root / name).mkdir(); (root / name / 'not-public').write_text('synthetic local data')
            self.record(root)
            package(root, base / 'one.zip'); package(root, base / 'two.zip')
            self.assertEqual((base / 'one.zip').read_bytes(), (base / 'two.zip').read_bytes())
            with zipfile.ZipFile(base / 'one.zip') as archive:
                self.assertEqual(archive.namelist(), ['zenti-preview/README.md', 'zenti-preview/RELEASE-MANIFEST.json'])

    def test_modified_source_and_existing_archive_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp); root = base / 'source'; root.mkdir()
            (root / 'README.md').write_text('Version one')
            self.record(root)
            package(root, base / 'one.zip')
            with self.assertRaises(FileExistsError): package(root, base / 'one.zip')
            (root / 'README.md').write_text('Version two')
            with self.assertRaises(ValueError): package(root, base / 'two.zip')

    def test_source_symlinks_refused(self):
        if sys.platform == 'win32':
            self.skipTest('Linux release filesystem required for symlink creation')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'link').symlink_to('/etc/passwd')
            with self.assertRaises(ValueError): check(root)

    def test_nested_environment_variants_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root / 'deploy').mkdir()
            (root / 'deploy/.env.production').write_text('FIXTURE_PASSWORD=not-a-secret')
            errors, _ = check(root)
            self.assertTrue(any('forbidden credential/runtime file' in item for item in errors))

    def test_existing_checksum_is_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp); root = base / 'source'; root.mkdir()
            (root / 'README.md').write_text('Version one'); self.record(root)
            sidecar = base / 'release.zip.sha256'; sidecar.write_text('prior evidence')
            with self.assertRaises(FileExistsError): package(root, base / 'release.zip')
            self.assertEqual(sidecar.read_text(), 'prior evidence')
            self.assertFalse((base / 'release.zip').exists())

    def test_private_markers_are_rejected_even_inside_scanner(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/'tools').mkdir()
            target=root/'tools/check_release.py'
            target.write_text('192.'+'168.'+'77.12')
            errors,_=check(root)
            self.assertTrue(any('private-host' in item for item in errors))
            target.write_text('C:/'+'Users/'+'example-person/config')
            errors,_=check(root)
            self.assertTrue(any('private-path' in item for item in errors))


if __name__ == '__main__':
    unittest.main()
