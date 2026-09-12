/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_HMC_CONTROLLER_PARAMS_HH__
#define __MEM_HMC_CONTROLLER_PARAMS_HH__

#include "base/logging.hh"
#include "params/HMCController.hh"

namespace gem5
{

/** Validate HMCController parameters before constructing its xbar base. */
class HMCControllerParamsValidator
{
  public:
    static const HMCControllerParams &
    validate(const HMCControllerParams &p)
    {
        fatal_if(p.timing_transparent,
                 "HMCController does not support timing_transparent\n");
        return p;
    }
};

} // namespace gem5

#endif // __MEM_HMC_CONTROLLER_PARAMS_HH__
