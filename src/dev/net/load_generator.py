from m5.objects.Ethernet import EtherInt
from m5.params import *
from m5.SimObject import SimObject


class LoadGenerator(SimObject):
    type = "LoadGenerator"
    cxx_header = "dev/net/load_generator.hh"
    cxx_class = "gem5::LoadGenerator"

    interface = EtherInt("interface")
    start_tick = Param.Tick(1, "Tick at whcih to start loadgenerator")
    stop_tick = Param.Tick(1, "Tick at which to stop loadgenerator")
    packet_size = Param.UInt64(64, "Packet size in bytes")
    packet_rate = Param.UInt64(100, "Number of packets per second to send")
    loadgen_id = Param.UInt8(0, "For match NIC")
    burst_width = Param.Tick(1, "Width of a packet burst in picoseconds")
    burst_gap = Param.Tick(1, "Time of gap between bursts in picoseconds")
    mode = Param.String("Increment", "LoadgenMode")
    check_loss_interval = Param.UInt64(
        5000, "Packets between adaptive loss checks"
    )
    check_loss_wait = Param.Latency(
        "1ms", "Wait after a loss window before adapting again"
    )
