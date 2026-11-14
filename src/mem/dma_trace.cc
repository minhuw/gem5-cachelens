// SPDX-License-Identifier: BSD-3-Clause
#include "mem/dma_trace.hh"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "base/logging.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
std::weak_ptr<DmaTraceSink> DmaTraceSink::current;

std::string
dmaTraceQuote(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else out += c;
    }
    return out + "\"";
}

DmaTraceSink::DmaTraceSink(const std::string &path,
    const std::string &nic_, const std::string &epoch_)
    : file(nullptr), nic(nic_), epoch(epoch_)
{
    fatal_if(nic.empty() || epoch.empty(),
             "DMA observation requires explicit NIC and fresh epoch");
    fatal_if(path.empty() || path.front() != '/',
             "DMA observation requires an absolute fresh output path");
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    fatal_if(fd < 0, "Cannot exclusively create DMA trace %s: %s",
             path, strerror(errno));
    file = fdopen(fd, "w");
    fatal_if(!file, "Cannot open DMA trace stream: %s", strerror(errno));
}

DmaTraceSink::~DmaTraceSink()
{
    if (file) {
        int rc = fclose(file);
        panic_if(rc, "DMA trace close failed");
    }
}

void
DmaTraceSink::activate(const std::shared_ptr<DmaTraceSink> &sink)
{
    fatal_if(!current.expired(), "DMA observation supports one NIC only");
    current = sink;
    sink->emit("scope", "\"population\":\"all_wire_dma_and_existing_eligible_llc_counters\","
        "\"traffic_class\":\"unknown\",\"producer_op_seq\":null,"
        "\"packet_identity\":null,\"cpu_joint_state\":null,"
        "\"translation_stage\":\"pciToDma_host_bridge_before_downstream_translation\"");
}

void
DmaTraceSink::emit(const std::string &event, const std::string &fields)
{
    const std::string line = "{\"schema\":\"cachelens.gem5.dma\",\"version\":1,"
        "\"event\":" + dmaTraceQuote(event) + ",\"tick\":" +
        std::to_string(curTick()) + ",\"ticks_per_second\":" +
        std::to_string(sim_clock::as_int::s) + ",\"nic\":" + dmaTraceQuote(nic) +
        ",\"epoch\":" + dmaTraceQuote(epoch) + "," + fields + "}\n";
    fatal_if(fwrite(line.data(), 1, line.size(), file) != line.size() ||
             fflush(file), "DMA observation write/flush failed");
}

std::string
DmaTraceOperation::fields() const
{
    return "\"wire_req_id\":" + std::to_string(wireId) +
        ",\"observed_instance\":" + std::to_string(instance) +
        ",\"wire_address\":" + std::to_string(wireAddress) +
        ",\"pci_to_dma_address\":" + std::to_string(targetAddress) +
        ",\"length_bytes\":" + std::to_string(length) +
        ",\"direction\":\"" + (read ? "read" : "write") +
        "\",\"traffic_class\":\"unknown\"";
}

std::string
DmaTraceExtension::fields() const
{
    return operation->fields() + ",\"fragment_address\":" +
        std::to_string(fragmentAddress) + ",\"fragment_size\":" +
        std::to_string(fragmentSize) + ",\"operation_offset\":" +
        std::to_string(operationOffset) + ",\"fragment_line\":" +
        std::to_string(lineAddress);
}

void
DmaTraceExtension::emit(const std::string &event) const
{
    operation->sink->emit(event, fields() +
        ",\"population\":\"actual_dma_port_packet_fragments\"");
}

void
observeDmaLookup(const RequestPtr &req, bool reqValid, Addr line,
    const std::string &cache, bool read, bool hit,
    const std::string &state, bool localDirty, bool privateOwner)
{
    auto sink = DmaTraceSink::active();
    if (!sink) return;
    auto ext = req && reqValid ? req->getExtension<DmaTraceExtension>() : nullptr;
    const std::string identity = ext ? ext->fields() :
        "\"wire_req_id\":null,\"observed_instance\":null,"
        "\"traffic_class\":\"unknown\",\"unmatched_reason\":\"" +
        std::string(!req ? "protocol_request_pointer_unavailable" :
                    (!reqValid ? "protocol_request_marked_invalid" :
                                 "request_has_no_trace_extension")) + "\"";
    sink->emit("accepted_cache_lookup", identity +
        ",\"population\":\"existing_eligible_dma_llc_lookup_counter\","
        "\"counter\":\"" + (read ? "dmaReadRequests" : "dmaWriteRequests") +
        "\",\"cache\":" + dmaTraceQuote(cache) +
        ",\"lookup_line\":" + std::to_string(line) +
        ",\"lookup_direction\":\"" + (read ? "read" : "write") +
        "\",\"tag_present\":" + (hit ? "true" : "false") +
        ",\"native_state\":" + dmaTraceQuote(state) +
        ",\"local_dirty\":" + (localDirty ? "true" : "false") +
        ",\"private_owner\":" + (privateOwner ? "true" : "false") +
        ",\"ddio_eligible\":true,\"data_array_hit\":null,\"any_cache_hit\":null");
}
} // namespace gem5
