# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

from m5.defines import buildEnv

from .cache_hierarchy import CacheLensCHIHierarchy
from .network import build_cachelens_network

__all__ = ["CacheLensCHIHierarchy", "build_cachelens_network"]

if buildEnv["USE_ARM_ISA"]:
    from .board import CacheLensArmBoard

    __all__.append("CacheLensArmBoard")

if buildEnv["USE_X86_ISA"]:
    from .board import CacheLensX86Board

    __all__.append("CacheLensX86Board")
