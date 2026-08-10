from m5.objects.Ethernet import EtherInt
from m5.params import *
from m5.SimObject import SimObject


class LoadGeneratorPcap(SimObject):
    type = "LoadGeneratorPcap"
    cxx_header = "dev/net/load_generator_pcap.hh"
    cxx_class = "gem5::LoadGeneratorPcap"

    interface = EtherInt("interface")
    loadgen_id = Param.UInt8(0, "For match NIC")
    start_tick = Param.Tick(1, "Tick at which to start loadgenerator")
    stop_tick = Param.Tick(1, "Tick at which to stop loadgenerator")
    pcap_filename = Param.String("", "Filename of the pcap file")
    max_packetsize = Param.UInt64(
        1514,
        "Maximum full L2 frame size, including Ethernet header but not FCS",
    )
    port_filter = Param.UInt16(11211, "Filter pcap traces by port")
    stack_mode = Param.String("KernelStack", "DPDKStack")
    replace_src_ip = Param.String(
        "10.10.10.11", "Replace src IP to this value"
    )
    replace_dest_ip = Param.String(
        "10.10.10.10", "Replace dst IP to this value"
    )
    replay_mode = Param.String(
        "SimpleReplay",
        "SimpleReplay is constant-paced; alternatives adjust throughput",
    )
    packet_rate = Param.UInt64(1, "Packets per second")
    increment_interval = Param.UInt64(1, "Adaptive rate step")
    check_loss_interval = Param.UInt64(
        100, "Packets between adaptive loss checks"
    )
    check_loss_wait = Param.Latency(
        "1ms", "Wait after a loss window before adapting again"
    )
    exit_on_eof = Param.Bool(
        False, "Exit after the optional EOF drain delay instead of continuing"
    )
    eof_drain_delay = Param.Latency(
        "1ms", "Delay before exiting after PCAP EOF"
    )
