// SPDX-License-Identifier: BSD-3-Clause
#include "mem/dma_trace.hh"

#include <cerrno>
#include <cstring>

#include "base/logging.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

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

DmaTraceSink::DmaTraceSink(const std::string &path, const std::string &nic_)
    : file(fopen(path.c_str(), "w")), nic(nic_)
{
    fatal_if(!file, "Cannot create DMA trace %s: %s", path, strerror(errno));
}

DmaTraceSink::~DmaTraceSink()
{
    panic_if(fclose(file), "DMA trace close failed");
}

void
DmaTraceSink::emit(const std::string &event, const std::string &fields)
{
    const std::string line = "{\"event\":" + dmaTraceQuote(event) +
        ",\"tick\":" + std::to_string(curTick()) +
        ",\"nic\":" + dmaTraceQuote(nic) + "," + fields + "}\n";
    fatal_if(fwrite(line.data(), 1, line.size(), file) != line.size(),
             "DMA observation write failed");
}

void
DmaTraceSink::flush()
{
    fatal_if(fflush(file), "DMA observation flush failed");
}

std::string
DmaTraceOperation::fields() const
{
    return "\"wire_req_id\":" + std::to_string(wireId) +
        ",\"observed_instance\":" + std::to_string(instance) +
        ",\"wire_address\":" + std::to_string(wireAddress) +
        ",\"pci_to_dma_address\":" + std::to_string(targetAddress) +
        ",\"length_bytes\":" + std::to_string(length) +
        ",\"direction\":\"" + (read ? "read" : "write") + "\"";
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
    operation->sink->emit(event, fields());
}

void
observeDmaLookup(const RequestPtr &req, bool reqValid, Addr line,
    const std::string &cache, bool read, bool hit,
    const std::string &state, bool localDirty, bool privateOwner)
{
    // Only lookups of traced DMA requests; untraced requests are not logged.
    auto ext = req && reqValid ? req->getExtension<DmaTraceExtension>() : nullptr;
    if (!ext) return;
    ext->operation->sink->emit("accepted_cache_lookup", ext->fields() +
        ",\"counter\":\"" + (read ? "dmaReadRequests" : "dmaWriteRequests") +
        "\",\"cache\":" + dmaTraceQuote(cache) +
        ",\"lookup_line\":" + std::to_string(line) +
        ",\"tag_present\":" + (hit ? "true" : "false") +
        ",\"native_state\":" + dmaTraceQuote(state) +
        ",\"local_dirty\":" + (localDirty ? "true" : "false") +
        ",\"private_owner\":" + (privateOwner ? "true" : "false"));
}
} // namespace gem5
