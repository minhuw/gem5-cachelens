# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject


class SplitRubyBridge(SimObject):
    type = "SplitRubyBridge"
    cxx_header = "mem/ruby/network/simple/SplitRubyBridge.hh"
    cxx_class = "gem5::ruby::SplitRubyBridge"
    ruby_system = Param.RubySystem("Registered Ruby topology")
    buffers = VectorParam.MessageBuffer([], "Cut buffers in global link/vnet order")
    source_roles = VectorParam.Int([], "Source owner per cut buffer")
    destination_roles = VectorParam.Int([], "Destination owner per cut buffer")
    monitor_buffers = VectorParam.MessageBuffer([], "Buffers required empty at global stop")
    role = Param.Int(-1, "-1 serial; otherwise exclusive partition owner")
    num_roles = Param.Int(2, "Number of partitions, between two and eight")
    transport = Param.String("socket", "socket (two roles) or shm (two to eight roles)")
    adaptive = Param.Bool(False, "Skip intervals proven idle by every partition")
    mode = Param.String("native", "native, codec, or split")
    socket_path = Param.String("", "Private two-process Unix socket")
    quantum = Param.Tick(1000, "Exclusive safe horizon, no larger than native cut latency")
    stop_tick = Param.Tick(10000000, "Agreed, quiescent stop horizon")
    owned_cpus = VectorParam.BaseCPU([], "Detailed CPUs exclusively advanced by this role")
    inactive_cpus = VectorParam.BaseCPU([], "Replica CPUs suspended before their first tick")
    expected_checksum = Param.UInt64(0, "Expected RAX result for the static x86 O3 fixture")
    config_tag = Param.String("", "Hash of identical static topology and ownership")
