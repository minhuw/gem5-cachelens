// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_DMA_TRACE_HH__
#define __MEM_DMA_TRACE_HH__

#include <cstdio>
#include <memory>
#include <string>

#include "mem/request.hh"

namespace gem5
{

// Per-device host-side JSONL observation; one object per line:
// {"event":...,"tick":...,"nic":...,<event fields>}. Never serialized.
class DmaTraceSink
{
    FILE *file;
  public:
    const std::string nic;
    DmaTraceSink(const std::string &path, const std::string &nic);
    ~DmaTraceSink();
    void emit(const std::string &event, const std::string &fields);
    void flush();
};

struct DmaTraceOperation
{
    const std::shared_ptr<DmaTraceSink> sink;
    const uint64_t wireId;
    // Native wire acceptance sequence, NOT producer op_seq or allocation age.
    const uint64_t instance;
    const Addr wireAddress;
    const Addr targetAddress;
    const uint64_t length;
    const bool read;
    std::string fields() const;
};
using DmaTraceOperationPtr = std::shared_ptr<const DmaTraceOperation>;

class DmaTraceExtension : public Extension<Request, DmaTraceExtension>
{
  public:
    const DmaTraceOperationPtr operation;
    const Addr fragmentAddress;
    const unsigned fragmentSize;
    const uint64_t operationOffset;
    const Addr lineAddress;
    DmaTraceExtension(DmaTraceOperationPtr op, Addr address, unsigned size,
                      uint64_t offset, Addr line)
        : operation(std::move(op)), fragmentAddress(address),
          fragmentSize(size), operationOffset(offset), lineAddress(line) {}
    std::unique_ptr<ExtensionBase> clone() const override
    { return std::make_unique<DmaTraceExtension>(*this); }
    std::string fields() const;
    void emit(const std::string &event) const;
};

std::string dmaTraceQuote(const std::string &text);
void observeDmaLookup(const RequestPtr &req, bool reqValid, Addr line,
    const std::string &cache, bool read, bool hit,
    const std::string &state, bool localDirty, bool privateOwner);

} // namespace gem5
#endif
