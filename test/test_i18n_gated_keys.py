"""Gated string keys ship only in builds that define their feature flag."""

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from gen_i18n import gated_prefixes_to_drop, main  # noqa: E402


class GatedPrefixesTest(unittest.TestCase):
    def test_flag_present_keeps_keys(self):
        self.assertEqual(gated_prefixes_to_drop(["-DFOO=1", "-DCROSSPOINT_AUDIOBOOK=1", "-DCROSSPOINT_SD_LOG=1"]), [])
        self.assertNotIn("STR_AUDIOBOOK_", gated_prefixes_to_drop("-D CROSSPOINT_AUDIOBOOK"))

    def test_each_prefix_follows_its_own_flag(self):
        self.assertEqual(gated_prefixes_to_drop("-DCROSSPOINT_AUDIOBOOK=1"), ["STR_SD_LOG_"])
        self.assertEqual(gated_prefixes_to_drop("-DCROSSPOINT_SD_LOG=1"), ["STR_AUDIOBOOK_"])

    def test_flag_absent_or_off_drops_keys(self):
        both = ["STR_AUDIOBOOK_", "STR_SD_LOG_"]
        self.assertEqual(gated_prefixes_to_drop(["-DFOO=1"]), both)
        self.assertIn("STR_AUDIOBOOK_", gated_prefixes_to_drop("-DCROSSPOINT_AUDIOBOOK=0"))
        self.assertIn("STR_AUDIOBOOK_", gated_prefixes_to_drop("-DCROSSPOINT_AUDIOBOOK_X=1"))
        self.assertEqual(gated_prefixes_to_drop(None), both)


class GatedGenerationTest(unittest.TestCase):
    def generate(self, drop):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "tr").mkdir()
            (tmp / "out").mkdir()
            (tmp / "src").mkdir()
            (tmp / "tr" / "english.yaml").write_text(
                '_language_name: "English"\n_language_code: "EN"\n_order: "0"\n_bcp47: "en"\n'
                'STR_OK: "OK"\nSTR_AUDIOBOOK_PLAY: "Play"\n'
            )
            (tmp / "src" / "a.cpp").write_text("#if X\nauto a = STR_AUDIOBOOK_PLAY;\n#endif\nauto b = STR_OK;\n")
            main(str(tmp / "tr"), str(tmp / "out"), [str(tmp / "src")], True, False, None, drop)
            return (tmp / "out" / "I18nKeys.h").read_text()

    def test_gated_key_stripped_without_flag(self):
        keys = self.generate(["STR_AUDIOBOOK_"])
        self.assertIn("STR_OK", keys)
        self.assertNotIn("STR_AUDIOBOOK_PLAY", keys)

    def test_gated_key_kept_with_flag(self):
        self.assertIn("STR_AUDIOBOOK_PLAY", self.generate([]))


if __name__ == "__main__":
    unittest.main()
