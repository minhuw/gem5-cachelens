# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Public CacheLens stdlib components.

Protocol- and ISA-specific components are imported lazily because a
single-protocol or single-ISA gem5 binary only contains its generated
SimObjects.
"""

from m5.defines import buildEnv

from gem5.coherence_protocol import CoherenceProtocol
from gem5.runtime import get_supported_protocols

from .network import build_cachelens_network

_HIERARCHY_MODULES = {
    "CacheLensCHIHierarchy": (
        CoherenceProtocol.CHI,
        ".cache_hierarchy",
    ),
    "CacheLensMESITwoLevelHierarchy": (
        CoherenceProtocol.MESI_TWO_LEVEL,
        ".mesi_two_level_cache_hierarchy",
    ),
}
_SUPPORTED_PROTOCOLS = get_supported_protocols()

__all__ = ["build_cachelens_network"]
__all__.extend(
    name
    for name, (protocol, _) in _HIERARCHY_MODULES.items()
    if protocol in _SUPPORTED_PROTOCOLS
)

if buildEnv["USE_ARM_ISA"]:
    __all__.append("CacheLensArmBoard")

if buildEnv["USE_X86_ISA"]:
    __all__.append("CacheLensX86Board")


def __getattr__(name):
    if name in _HIERARCHY_MODULES:
        required, module_name = _HIERARCHY_MODULES[name]
        supported = get_supported_protocols()
        if required not in supported:
            available = ", ".join(
                sorted(protocol.value for protocol in supported)
            )
            raise ImportError(
                f"{name} requires the compiled Ruby protocol "
                f"{required.value}; this gem5 binary provides: "
                f"{available or 'none'}."
            )

        if module_name == ".cache_hierarchy":
            from .cache_hierarchy import CacheLensCHIHierarchy

            component = CacheLensCHIHierarchy
        else:
            from .mesi_two_level_cache_hierarchy import (
                CacheLensMESITwoLevelHierarchy,
            )

            component = CacheLensMESITwoLevelHierarchy
    elif name == "CacheLensArmBoard" and buildEnv["USE_ARM_ISA"]:
        from .board import CacheLensArmBoard

        component = CacheLensArmBoard
    elif name == "CacheLensX86Board" and buildEnv["USE_X86_ISA"]:
        from .board import CacheLensX86Board

        component = CacheLensX86Board
    else:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

    globals()[name] = component
    return component
