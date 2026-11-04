"""The distributed neutral assets must reproduce without image libraries or source portraits."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from generate_public_art import generate


class ProceduralArtTests(unittest.TestCase):
    def test_all_generated_bytes_match_the_shipped_assets(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            generate(output)
            files = [path for path in output.rglob('*') if path.is_file()]
            self.assertEqual(len(files), 22)
            for path in files:
                relative = path.relative_to(output)
                self.assertEqual(path.read_bytes(), (ROOT / relative).read_bytes(), str(relative))


if __name__ == '__main__':
    unittest.main()
