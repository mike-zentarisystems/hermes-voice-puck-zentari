"""Release trust-boundary tests: drift, symlinks, excluded runtime data and byte stability."""
import json
from pathlib import Path
import sys
import subprocess
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

    def release_check(self, root):
        return subprocess.run(
            [sys.executable, '-B', str(ROOT / 'tools/check_release.py'),
             '--root', str(root), '--verify-manifest'],
            capture_output=True, text=True, check=False,
        )

    def test_release_check_cli_accepts_exact_manifest_without_rewriting(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root / 'README.md').write_text('Reviewed source\n')
            self.record(root)
            recorded = (root / 'RELEASE-MANIFEST.json').read_bytes()
            result = self.release_check(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)
            self.assertEqual((root / 'RELEASE-MANIFEST.json').read_bytes(), recorded)

    def test_release_check_cli_rejects_manifest_drift_without_rewriting(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = root / 'README.md'
            source.write_text('Reviewed source\n'); self.record(root)
            recorded = (root / 'RELEASE-MANIFEST.json').read_bytes()
            source.write_text('Changed after the manifest was recorded\n')
            result = self.release_check(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn('manifest drift', result.stdout)
            self.assertEqual((root / 'RELEASE-MANIFEST.json').read_bytes(), recorded)

    def test_release_check_cli_rejects_missing_or_invalid_manifest(self):
        for content in (None, 'not-json', '{}'):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); (root / 'README.md').write_text('Reviewed source\n')
                if content is not None:
                    (root / 'RELEASE-MANIFEST.json').write_text(content)
                result = self.release_check(root)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('manifest', result.stdout)

    def test_release_check_cli_rejects_added_or_removed_files(self):
        for change in ('added', 'removed'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                source = root / 'README.md'
                source.write_text('Reviewed source\n')
                self.record(root)
                recorded = (root / 'RELEASE-MANIFEST.json').read_bytes()
                if change == 'added':
                    (root / 'unreviewed.py').write_text('print("new source")\n')
                else:
                    source.unlink()
                result = self.release_check(root)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('manifest drift', result.stdout)
                self.assertEqual((root / 'RELEASE-MANIFEST.json').read_bytes(), recorded)

    def test_validation_workflow_verifies_instead_of_rewriting_manifest(self):
        workflow = (ROOT / '.github/workflows/validate.yml').read_text(encoding='utf-8')
        self.assertIn('python tools/check_release.py --verify-manifest', workflow)
        self.assertNotIn('--write-manifest', workflow)

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
