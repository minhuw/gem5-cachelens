# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import unittest

from m5.defines import buildEnv

from gem5.coherence_protocol import CoherenceProtocol
from gem5.prebuilt import cachelens
from gem5.prebuilt.cachelens import *  # noqa: F403
from gem5.runtime import get_supported_protocols


class CacheLensExportTestSuite(unittest.TestCase):
    def test_wildcard_import_advertises_only_supported_hierarchies(
        self,
    ) -> None:
        supported = get_supported_protocols()
        hierarchy_protocols = {
            "CacheLensCHIHierarchy": CoherenceProtocol.CHI,
            "CacheLensMESITwoLevelHierarchy": (
                CoherenceProtocol.MESI_TWO_LEVEL
            ),
        }

        for name, protocol in hierarchy_protocols.items():
            with self.subTest(name=name):
                expected = protocol in supported
                self.assertEqual(expected, name in cachelens.__all__)
                self.assertEqual(expected, name in globals())
                if expected:
                    self.assertIs(globals()[name], getattr(cachelens, name))
                else:
                    with self.assertRaisesRegex(
                        ImportError,
                        f"{name} requires the compiled Ruby protocol",
                    ):
                        exec(
                            f"from gem5.prebuilt.cachelens import {name}",
                            {},
                        )

    def test_common_exports_remain_available(self) -> None:
        self.assertIn("build_cachelens_network", cachelens.__all__)
        self.assertIn("build_cachelens_network", globals())
        if buildEnv["USE_X86_ISA"]:
            self.assertIn("CacheLensX86Board", cachelens.__all__)
            self.assertIn("CacheLensX86Board", globals())
        if buildEnv["USE_ARM_ISA"]:
            self.assertIn("CacheLensArmBoard", cachelens.__all__)
            self.assertIn("CacheLensArmBoard", globals())


if __name__ in ("__main__", "__m5_main__"):
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        CacheLensExportTestSuite
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
