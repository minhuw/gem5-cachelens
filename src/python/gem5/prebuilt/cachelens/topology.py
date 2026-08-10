# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects import (
    SimpleExtLink,
    SimpleIntLink,
    SimpleNetwork,
    Switch,
)


class SimpleCrossbar(SimpleNetwork):
    """A SimpleNetwork implementation equivalent to legacy Crossbar.py."""

    def __init__(
        self,
        ruby_system,
        *,
        link_latency: int = 1,
        router_latency: int = 1,
        buffer_size: int = 4,
    ):
        if link_latency <= 0 or router_latency <= 0:
            raise ValueError("Network latencies must be positive.")
        if buffer_size <= 0:
            raise ValueError("Network buffer_size must be positive.")

        super().__init__()
        self.netifs = []
        self.ruby_system = ruby_system
        # Keep the requested topology values separately so link/router
        # SimObjects can be constructed with explicit parameters below.
        self._link_latency = link_latency
        self._router_latency = router_latency
        self.buffer_size = buffer_size

    def connect(self, controllers):
        """Connect each controller router through one central crossbar."""
        controller_count = len(controllers)
        self.routers = [
            Switch(
                router_id=i,
                int_routing_latency=self._router_latency,
                ext_routing_latency=self._router_latency,
            )
            for i in range(controller_count + 1)
        ]
        crossbar = self.routers[-1]

        self.ext_links = [
            SimpleExtLink(
                link_id=i,
                ext_node=controller,
                int_node=self.routers[i],
                latency=self._link_latency,
            )
            for i, controller in enumerate(controllers)
        ]

        int_links = []
        for router in self.routers[:-1]:
            int_links.append(
                SimpleIntLink(
                    link_id=len(int_links),
                    src_node=router,
                    dst_node=crossbar,
                    latency=self._link_latency,
                )
            )
            int_links.append(
                SimpleIntLink(
                    link_id=len(int_links),
                    src_node=crossbar,
                    dst_node=router,
                    latency=self._link_latency,
                )
            )
        self.int_links = int_links
