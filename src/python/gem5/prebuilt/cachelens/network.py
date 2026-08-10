# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Shared networking construction for CacheLens full-system boards."""

from typing import NamedTuple

from m5.defines import buildEnv
from m5.objects import (
    EtherLink,
    IGbE_e1000,
    LoadGenerator,
)
from m5.util.convert import (
    toLatency,
    toNetworkBandwidth,
)


class CacheLensNetwork(NamedTuple):
    nics: list
    loadgens: list
    links: list


def build_cachelens_network(
    *,
    num_nics: int = 0,
    num_loadgens: int = 0,
    loadgen_type: str = "Simple",
    packet_rate: int = 1,
    packet_size: int = 64,
    loadgen_start: int = 1,
    loadgen_stop: int = (1 << 64) - 1,
    loadgen_mode: str = "Static",
    pcap_filename: str = None,
    pcap_max_packet_size: int = 1514,
    pcap_port_filter: int = 11211,
    pcap_stack_mode: str = "KernelStack",
    pcap_src_ip: str = "10.10.10.11",
    pcap_dest_ip: str = "10.10.10.10",
    pcap_replay_mode: str = "SimpleReplay",
    pcap_increment_interval: int = 1,
    check_loss_interval: int = 5000,
    check_loss_wait: str = "1ms",
    link_speed: str = "1Gbps",
    link_delay: str = "200us",
    rx_fifo_size: str = "48KiB",
    tx_fifo_size: str = "16KiB",
    pcap_exit_on_eof: bool = False,
    pcap_eof_drain_delay: str = "1ms",
) -> CacheLensNetwork:
    """Build CacheLens NICs, load generators, and Ethernet links.

    ``link_delay`` is the one-way Ethernet propagation/transmit delay.
    """
    if num_nics < 0 or num_loadgens < 0:
        raise ValueError("NIC and load-generator counts must be non-negative.")
    if num_loadgens > num_nics:
        raise ValueError("The number of load generators cannot exceed NICs.")
    if loadgen_type not in ("Simple", "Pcap"):
        raise ValueError("loadgen_type must be 'Simple' or 'Pcap'.")
    if packet_rate <= 0:
        raise ValueError("packet_rate must be positive.")
    if check_loss_interval <= 0:
        raise ValueError("check_loss_interval must be positive.")
    if toNetworkBandwidth(link_speed) <= 0:
        raise ValueError("link_speed must be positive.")
    if toLatency(link_delay) < 0:
        raise ValueError("link_delay must not be negative.")
    if toLatency(check_loss_wait) <= 0:
        raise ValueError("check_loss_wait must be positive.")
    if loadgen_start > loadgen_stop:
        raise ValueError("loadgen_start must not exceed loadgen_stop.")

    if loadgen_type == "Simple":
        if packet_size < 22:
            raise ValueError("Simple load-generator packets need 22 bytes.")
        if loadgen_mode not in ("Static", "Increment", "Burst"):
            raise ValueError("Unknown simple load-generator mode.")
        if loadgen_mode == "Increment" and toLatency(
            check_loss_wait
        ) < 2 * toLatency(link_delay):
            raise ValueError("check_loss_wait must cover the modeled RTT.")
    else:
        if not buildEnv["HAVE_PCAP"]:
            raise RuntimeError(
                "Pcap load generation requires a gem5 binary built with "
                "libpcap support."
            )
        if not pcap_filename:
            raise ValueError("Pcap load generation requires a filename.")
        if pcap_max_packet_size < 14:
            raise ValueError(
                "Pcap maximum full L2 frame size must be at least 14 bytes."
            )
        if not 1 <= pcap_port_filter <= 65535:
            raise ValueError("Pcap port filter must be in [1, 65535].")
        if pcap_increment_interval <= 0:
            raise ValueError("Pcap increment interval must be positive.")
        if pcap_stack_mode not in ("KernelStack", "DPDKStack"):
            raise ValueError("Unknown Pcap stack mode.")
        if pcap_replay_mode not in (
            "SimpleReplay",
            "ReplayAndAdjustThroughput",
            "ConstThroughput",
        ):
            raise ValueError("Unknown Pcap replay mode.")
        if pcap_replay_mode == "ReplayAndAdjustThroughput" and toLatency(
            check_loss_wait
        ) < 2 * toLatency(link_delay):
            raise ValueError("check_loss_wait must cover the modeled RTT.")
        if toLatency(pcap_eof_drain_delay) < 0:
            raise ValueError("pcap_eof_drain_delay must not be negative.")
        if pcap_exit_on_eof and toLatency(
            pcap_eof_drain_delay
        ) < 2 * toLatency(link_delay):
            raise ValueError(
                "pcap_eof_drain_delay must cover the modeled link RTT."
            )

    nics = [
        IGbE_e1000(
            hardware_address=f"02:90:00:00:00:{index + 1:02x}",
            rx_fifo_size=rx_fifo_size,
            tx_fifo_size=tx_fifo_size,
        )
        for index in range(num_nics)
    ]
    loadgens = []
    links = []

    for index in range(num_loadgens):
        if loadgen_type == "Simple":
            loadgen = LoadGenerator(
                packet_rate=packet_rate,
                packet_size=packet_size,
                start_tick=loadgen_start,
                stop_tick=loadgen_stop,
                mode=loadgen_mode,
                loadgen_id=index,
                check_loss_interval=check_loss_interval,
                check_loss_wait=check_loss_wait,
            )
        else:
            from m5.objects import LoadGeneratorPcap

            loadgen = LoadGeneratorPcap(
                loadgen_id=index,
                start_tick=loadgen_start,
                stop_tick=loadgen_stop,
                pcap_filename=pcap_filename,
                max_packetsize=pcap_max_packet_size,
                port_filter=pcap_port_filter,
                stack_mode=pcap_stack_mode,
                replace_src_ip=pcap_src_ip,
                replace_dest_ip=pcap_dest_ip,
                replay_mode=pcap_replay_mode,
                packet_rate=packet_rate,
                increment_interval=pcap_increment_interval,
                check_loss_interval=check_loss_interval,
                check_loss_wait=check_loss_wait,
                exit_on_eof=pcap_exit_on_eof,
                eof_drain_delay=pcap_eof_drain_delay,
            )

        link = EtherLink(speed=link_speed, delay=link_delay)
        link.int0 = nics[index].interface
        link.int1 = loadgen.interface
        loadgens.append(loadgen)
        links.append(link)

    return CacheLensNetwork(nics=nics, loadgens=loadgens, links=links)
