// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __SIM_PARTITION_PARTICIPANT_HH__
#define __SIM_PARTITION_PARTICIPANT_HH__

#include <algorithm>
#include <vector>

#include "base/logging.hh"

namespace gem5
{

/** Shared-state preparation and invariants, called with all queues stopped.
 * Registration is construction-time only; prepare may eagerly construct more
 * participants. No registry or partition-state writes occur on model threads.
 */
class PartitionParticipant
{
  private:
    static auto &registry()
    {
        static auto *objects = new std::vector<PartitionParticipant *>;
        return *objects;
    }
    inline static bool partitioned = false;

  protected:
    PartitionParticipant() { registry().push_back(this); }
    virtual ~PartitionParticipant()
    {
        auto &objects = registry();
        objects.erase(std::remove(objects.begin(), objects.end(), this),
                      objects.end());
    }
    virtual void prepareWhileStopped() {}
    virtual void checkWhileStopped() const {}
    virtual void endWhileStopped() {}

  public:
    PartitionParticipant(const PartitionParticipant &) = delete;
    PartitionParticipant &operator=(const PartitionParticipant &) = delete;
    static bool isPartitioned() { return partitioned; }
    static void prepareAllWhileStopped()
    {
        fatal_if(partitioned, "Partition participants already active");
        for (size_t i = 0; i < registry().size(); ++i)
            registry()[i]->prepareWhileStopped();
        partitioned = true;
    }
    static void checkAllWhileStopped()
    {
        for (auto *object : registry()) object->checkWhileStopped();
    }
    static void endAllWhileStopped()
    {
        checkAllWhileStopped();
        partitioned = false;
        for (auto *object : registry()) object->endWhileStopped();
    }
};

} // namespace gem5
#endif
