#!/usr/bin/env python3
"""Guard against empty-array expansion under `set -u` in the shell tools.

macOS runners still ship bash 3.2, where expanding "${arr[@]}" on an EMPTY
array under `set -u` aborts with "unbound variable". bash >= 4.4 does not,
so this class of bug is invisible on Linux and only ever breaks macOS CI --
which is exactly how it reached us: a game packaged without --runtime-dir
failed at `RUNTIME_DIRS[@]: unbound variable`.

The safe idiom, used throughout these scripts, is:

    for x in ${arr[@]+"${arr[@]}"}; do

This test fails on any array that is declared empty and later expanded
without that guard.
"""

import os
import re
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Arrays that are declared empty but provably populated before every
# expansion, so the guard would be noise. Keyed (script, array) -> why.
ALLOWED = {
    ("stage_setup_sdk.sh", "args"):
        "unconditionally appended just above the loop",
    ("package_setup_host.sh", "stage_args"):
        "declared with contents, not empty",
}

DECL = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)=\(\s*\)", re.M)
GUARDED = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\+")
USE = re.compile(r'"\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\}"')
SET_U = re.compile(r"^set -[a-z]*u", re.M)
# Lines of context in which a preceding size check still counts.
LOOKBACK = 3
COUNT = re.compile(r"\$\{#([A-Za-z_][A-Za-z0-9_]*)\[@\]\}")


def shell_scripts():
    for root, dirs, files in os.walk(TOOLS):
        dirs[:] = [d for d in dirs if d not in (".git", "__pycache__")]
        for f in files:
            if f.endswith(".sh"):
                yield os.path.join(root, f)


class NounsetArrayTest(unittest.TestCase):
    def test_no_unguarded_empty_array_expansion(self):
        offenders = []
        for path in shell_scripts():
            with open(path, errors="replace") as fh:
                text = fh.read()
            if not SET_U.search(text):
                continue
            declared_empty = set(DECL.findall(text))
            if not declared_empty:
                continue
            name = os.path.basename(path)
            lines = text.split("\n")
            for lineno, line in enumerate(lines, 1):
                # A guarded expansion contains "${arr[@]+" on the same line.
                guarded = set(GUARDED.findall(line))
                # A ${#arr[@]} size check shortly before the expansion is a
                # real guard too -- it is how vendor_deps.sh protects its
                # loops -- and ${#arr[@]} itself is safe under bash 3.2.
                window = "\n".join(lines[max(0, lineno - 1 - LOOKBACK):lineno])
                counted = set(COUNT.findall(window))
                for arr in USE.findall(line):
                    if arr not in declared_empty:
                        continue
                    if arr in guarded or arr in counted:
                        continue
                    if (name, arr) in ALLOWED:
                        continue
                    offenders.append("%s:%d  %s -> %s"
                                     % (name, lineno, arr, line.strip()[:60]))
        self.assertEqual(
            offenders, [],
            "unguarded empty-array expansion under `set -u` "
            "(breaks on bash 3.2 / macOS CI); use ${arr[@]+\"${arr[@]}\"}:\n  "
            + "\n  ".join(offenders))

    def test_the_detector_actually_fires(self):
        """A detector that can never fail is not a guard."""
        text = 'set -euo pipefail\nFOO=()\nfor x in "${FOO[@]}"; do :; done\n'
        declared = set(DECL.findall(text))
        self.assertIn("FOO", declared)
        line = 'for x in "${FOO[@]}"; do :; done'
        self.assertIn("FOO", USE.findall(line))
        self.assertNotIn("FOO", set(GUARDED.findall(line)))

    def test_guarded_form_is_recognised(self):
        line = 'for x in ${FOO[@]+"${FOO[@]}"}; do :; done'
        self.assertIn("FOO", set(GUARDED.findall(line)))

    def test_size_check_counts_as_a_guard(self):
        line = 'if [[ ${#FOO[@]} -gt 0 ]]; then for x in "${FOO[@]}"; do :; done'
        self.assertIn("FOO", set(COUNT.findall(line)))

    def test_size_check_on_an_earlier_line_counts(self):
        """vendor_deps.sh guards one line above the loop."""
        window = 'if [[ ${#FOO[@]} -gt 0 ]]; then\n  for x in "${FOO[@]}"; do'
        self.assertIn("FOO", set(COUNT.findall(window)))

    def test_size_check_on_a_different_array_does_not_count(self):
        window = 'if [[ ${#BAR[@]} -gt 0 ]]; then\n  for x in "${FOO[@]}"; do'
        self.assertNotIn("FOO", set(COUNT.findall(window)))


if __name__ == "__main__":
    unittest.main()
