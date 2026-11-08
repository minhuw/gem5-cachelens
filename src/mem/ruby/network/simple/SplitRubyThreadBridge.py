# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject, PyBindMethod


class SplitRubyThreadBridge(SimObject):
    type = "SplitRubyThreadBridge"
    cxx_header = "mem/ruby/network/simple/SplitRubyThreadBridge.hh"
    cxx_class = "gem5::ruby::SplitRubyThreadBridge"
    cxx_exports = [PyBindMethod("activate"), PyBindMethod("deactivate"),
                   PyBindMethod("report"), PyBindMethod("readGuest"),
                   PyBindMethod("exerciseSync")]
    control_cpu = Param.BaseCPU(NULL, "Guest address space for stopped-state observations")
    ruby_system = Param.RubySystem("The single authoritative Ruby hierarchy")
    mode = Param.String("native", "native, codec, or thread")
    spin_wait = Param.Bool(False, "Busy-wait host barriers after parallel activation")
    fuse_sync = Param.Bool(False, "Bridge owns quantum sync; native async model events reject")
    mailbox_fence = Param.Bool(False, "Per-queue epoch fence; requires thread+spin_wait+fuse_sync")
    fence_leader = Param.Unsigned(2, "Fixed mailbox coordinator role; checked only when enabled")
    sync_trace_stride = Param.Unsigned(0, "Host diagnostic every N fences starting at epoch1; 0 disables clocks")
    quantum = Param.Tick(1000, "No greater than native cut-link latency")
    buffers = VectorParam.MessageBuffer([], "Cut links in native link/vnet order")
    source_roles = VectorParam.Int([], "0 main, 1 HNF island")
    destination_roles = VectorParam.Int([], "Destination owner for each cut")
    controllers = VectorParam.ClockedObject([], "Complete HNF controller islands")
    routers = VectorParam.ClockedObject([], "Their complete native Switches")

    queue_count = Param.Unsigned(2, "Total gem5 queues after stopped promotion")
    managers = VectorParam.SimObject([], "Explicit CPU/cache subtree managers")
    manager_roles = VectorParam.Int([], "Owner of each manager after activation")
    router_roles = VectorParam.Int([], "Owner of each selected router; empty means1")
    value_transport = Param.Bool(False, "Shared-process message clones instead of wire encoding")
    exercise_io = Param.Bool(False, "Directed exact-deadline handoff gate only")
