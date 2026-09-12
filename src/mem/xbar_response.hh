/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_XBAR_RESPONSE_HH__
#define __MEM_XBAR_RESPONSE_HH__

#include <unordered_map>
#include <utility>

#include "base/logging.hh"
#include "base/types.hh"
#include "mem/request.hh"

namespace gem5
{

/** Reentrancy-safe routing for synchronously forwarded timing responses. */
class XBarResponseRoute
{
  public:
    using RouteMap = std::unordered_map<RequestPtr, PortID>;

    template <typename SendTimingResp>
    static bool
    forward(RouteMap &route_to, const RequestPtr &route_key,
            PortID destination, SendTimingResp &&send_timing_resp)
    {
        const size_t erased = route_to.erase(route_key);
        panic_if(erased != 1,
                 "Transparent response route for request %p is missing\n",
                 route_key.get());

        if (std::forward<SendTimingResp>(send_timing_resp)())
            return true;

        const bool inserted =
            route_to.emplace(route_key, destination).second;
        panic_if(!inserted,
                 "Transparent response route for request %p was replaced "
                 "during a rejected response callback\n",
                 route_key.get());
        return false;
    }
};

} // namespace gem5

#endif // __MEM_XBAR_RESPONSE_HH__
