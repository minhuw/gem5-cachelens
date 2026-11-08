# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""SplitSim thread partitions of the CacheLens CHI hierarchy.

``attach_chi`` assigns whole Ruby controllers, together with their own
SimpleNetwork routers, to gem5 event queues and creates the
``SplitRubyThreadBridge`` that runs those queues as threads of one process.
Everything not assigned stays on queue 0. Only network int links between
routers on different queues are cut: the bridge stages their messages and
delivers them, with their native issue tick and latency, at a global fence
every quantum, so the quantum must not exceed any cut link's latency.

Call it after ``board._pre_instantiate()`` (the network is connected) and
before ``m5.instantiate()``. With the simulation stopped, call
``root.split_hnf.activate()`` before and ``root.split_hnf.deactivate()``
after the partitioned ``m5.simulate``. Activation must be cold (before the
first ``m5.simulate``) or follow a consumed workbegin exit.

CHI has no wire codec, so codec and thread modes use value transport
(shared-process message clones). Layouts move home nodes only: CPUs, their
L1/L2 controllers and sequencers, the MiscNode, DMA requestors and memory
controllers stay on queue 0, because CPU-side interrupts and timers and the
device/memory ports do not cross queues yet.

In thread mode a cut buffer holds a message only from its delivery at the
fence; the staging time before is reported per buffer by ``SPLIT_FS_STAGE``
(``occupancy_contract``). The raw ``m_buf_msgs`` average of a cut buffer
therefore differs from a serial run by exactly the staged ticks divided by
the measured interval; every other statistic is expected to be identical.
"""

from m5.objects import (
    DerivedClockDomain,
    SrcClockDomain,
)
from m5.proxy import BaseProxy
from m5.ticks import fixGlobalFrequency

from gem5.coherence_protocol import CoherenceProtocol

MODES = ("native", "codec", "thread")
OCCUPANCY_CONTRACT = "physical-buffer-plus-explicit-staging-v1"


def _hnf_queues(hierarchy, queues):
    """Split the HNFs into ``queues`` contiguous, equal groups (queues 1..)."""
    hnfs = list(hierarchy.hnfs)
    if not hnfs or len(hnfs) % queues:
        raise ValueError(
            f"{len(hnfs)} HNFs cannot be split evenly across {queues} queues"
        )
    per_queue = len(hnfs) // queues
    return [(hnf, 1 + index // per_queue) for index, hnf in enumerate(hnfs)]


# Each layout maps the hierarchy to (controller, queue) pairs, queue >= 1.
LAYOUTS = {
    "hnf1": lambda hierarchy: _hnf_queues(hierarchy, 1),
    "hnf2": lambda hierarchy: _hnf_queues(hierarchy, 2),
}


def _clock_period(obj):
    """Ticks per cycle of ``obj``'s fixed clock domain."""
    domain = obj.clk_domain
    divider = 1
    while True:
        while isinstance(domain, BaseProxy):
            domain = domain.unproxy(obj)
        if isinstance(domain, DerivedClockDomain):
            divider *= int(domain.clk_divider)
            obj, domain = domain, domain.clk_domain
            continue
        if not isinstance(domain, SrcClockDomain) or len(domain.clock) != 1:
            raise ValueError(
                f"{obj.path()}: cut links need one fixed source clock"
            )
        return int(domain.clock[0].getValue()) * divider


def _pinned_objects(board, hierarchy):
    """Objects that must stay on queue 0, with the reason."""
    pinned = {}

    def pin(tree, reason):
        for obj in tree.descendants():
            pinned[obj.path()] = reason

    cpu = "CPU partitions are not supported yet (CPU-side interrupts/timers)"
    for core in board.get_processor().get_cores():
        pin(core.get_simobject(), cpu)
    for cluster in hierarchy.core_clusters:
        for cache in (cluster.l1i, cluster.l1d, cluster.l2):
            pin(cache, cpu)
    port = "controllers with device or memory ports cannot move"
    for controller in (
        list(hierarchy.memory_controllers)
        + list(hierarchy.dma_requestors)
        + list(hierarchy.misc_nodes)
    ):
        pin(controller, port)
    return pinned


def attach_roles(
    root,
    board,
    mode,
    layout,
    roles,
    quantum=1000,
    value_transport=True,
    spin_wait=False,
):
    """Partition CHI controllers by an explicit ``(controller, queue)`` map.

    Each listed controller moves, with all of its descendants and its own
    router, to its queue (>= 1); queues must be numbered 1..N-1 without
    gaps. Returns metadata describing the partition and its cut links.
    """
    if mode not in MODES:
        raise ValueError(f"mode must be one of {', '.join(MODES)}")
    if mode != "native" and not value_transport:
        raise ValueError(
            "CHI has no wire codec; codec and thread modes need "
            "value_transport=True"
        )
    if spin_wait and mode != "thread":
        raise ValueError("spin_wait requires thread mode")
    if not isinstance(quantum, int) or quantum <= 0:
        raise ValueError("quantum must be a positive number of ticks")
    if getattr(root, "split_hnf", None) is not None:
        raise ValueError("root already has a thread bridge")
    hierarchy = board.get_cache_hierarchy()
    if hierarchy.get_coherence_protocol() != CoherenceProtocol.CHI or not (
        hasattr(hierarchy, "hnfs") and hasattr(hierarchy, "core_clusters")
    ):
        raise ValueError("attach requires the CacheLens CHI hierarchy")
    network = hierarchy.ruby_system.network
    if not network.ext_links or not network.int_links:
        raise ValueError("attach after board._pre_instantiate()")

    controllers, role_of = [], {}
    for controller, role in roles:
        if not isinstance(role, int) or not 1 <= role <= 7:
            raise ValueError(f"{controller.path()}: queue must be in 1..7")
        if controller.path() in role_of:
            raise ValueError(f"{controller.path()}: assigned twice")
        role_of[controller.path()] = role
        controllers.append((controller, role))
    queue_count = max(role_of.values(), default=0) + 1
    if sorted(set(role_of.values())) != list(range(1, queue_count)):
        raise ValueError("queues must be numbered 1..N-1 without gaps")

    pinned = _pinned_objects(board, hierarchy)
    managers = {}

    def own(tree, role):
        for obj in tree.descendants():
            path = obj.path()
            if path in pinned:
                raise ValueError(f"{layout} would move {path}: {pinned[path]}")
            previous = managers.get(path)
            if previous and previous[1] != role:
                raise ValueError(f"Conflicting ownership: {path}")
            managers[path] = (obj, role)

    routers, router_role = [], {}
    for controller, role in controllers:
        own(controller, role)
        ext = [e for e in network.ext_links if e.ext_node is controller]
        if len(ext) != 1:
            raise ValueError(f"{controller.path()}: expected one ext link")
        router = ext[0].int_node
        own(router, role)
        routers.append(router)
        router_role[router.path()] = role
    # A moved router must not also serve a controller left on another queue.
    for link in network.ext_links:
        role = router_role.get(link.int_node.path())
        if role is not None and role_of.get(link.ext_node.path(), 0) != role:
            raise ValueError(
                f"{link.int_node.path()} serves controllers on two queues"
            )

    fixGlobalFrequency()
    buffers, sources, destinations, links = [], [], [], []
    for index, link in enumerate(network.int_links):
        source = router_role.get(link.src_node.path(), 0)
        destination = router_role.get(link.dst_node.path(), 0)
        if source == destination:
            continue
        # The source router's throttle enqueues with the link latency.
        latency = int(link.latency) * _clock_period(link.src_node)
        if quantum > latency:
            raise ValueError(
                f"quantum {quantum} exceeds the {latency}-tick lookahead of "
                f"{link.path()}"
            )
        if any(int(buffer.buffer_size) != 0 for buffer in link.buffers):
            raise ValueError(f"{link.path()}: bounded cut buffers unsupported")
        buffers.extend(link.buffers)
        sources.extend([source] * len(link.buffers))
        destinations.extend([destination] * len(link.buffers))
        links.append(
            dict(
                index=index,
                link=link.path(),
                source=source,
                destination=destination,
                latency=latency,
                buffers=[buffer.path() for buffer in link.buffers],
            )
        )
    if not buffers:
        raise ValueError(f"{layout} cuts no network links")

    from m5.objects import SplitRubyThreadBridge

    ordered = sorted(managers.values(), key=lambda entry: entry[0].path())
    root.sim_quantum = quantum
    root.split_hnf = SplitRubyThreadBridge(
        ruby_system=hierarchy.ruby_system,
        control_cpu=board.get_processor().get_cores()[0].get_simobject(),
        mode=mode,
        quantum=quantum,
        spin_wait=spin_wait,
        queue_count=queue_count,
        managers=[obj for obj, _ in ordered],
        manager_roles=[role for _, role in ordered],
        controllers=[controller for controller, _ in controllers],
        routers=routers,
        router_roles=[role for _, role in controllers],
        value_transport=value_transport,
        buffers=buffers,
        source_roles=sources,
        destination_roles=destinations,
    )
    return dict(
        layout=layout,
        mode=mode,
        queue_count=queue_count,
        quantum=quantum,
        value_transport=value_transport,
        spin_wait=spin_wait,
        roles={controller.path(): role for controller, role in controllers},
        routers=dict(router_role),
        links=links,
        cut_buffers=len(buffers),
        managers={path: role for path, (_, role) in sorted(managers.items())},
        occupancy_contract=OCCUPANCY_CONTRACT,
    )


def attach_chi(
    root,
    board,
    mode,
    layout,
    quantum=1000,
    value_transport=True,
    spin_wait=False,
):
    """Partition the CacheLens CHI hierarchy with a named layout.

    hnf1: every HNF and its router on queue 1, the rest on queue 0.
    hnf2: the first half of the HNFs on queue 1, the second half on queue 2.
    """
    if layout not in LAYOUTS:
        raise ValueError(f"layout must be one of {', '.join(LAYOUTS)}")
    hierarchy = board.get_cache_hierarchy()
    if not hasattr(hierarchy, "hnfs"):
        raise ValueError("attach requires the CacheLens CHI hierarchy")
    return attach_roles(
        root,
        board,
        mode,
        layout,
        LAYOUTS[layout](hierarchy),
        quantum=quantum,
        value_transport=value_transport,
        spin_wait=spin_wait,
    )
