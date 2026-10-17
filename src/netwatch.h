#pragma once

#include <cstdint>

/* Watches that the network still works while the station counts as joined,
   by asking the gateway (ping) every minute: a panel once stayed "connected"
   and unreachable until its power was cut. Not answering for 3 minutes: the
   station leaves and joins again; for 5 minutes: a restart -- 10, 20, ...
   minutes for each further one in a row (at most 2 hours), so a router that is
   simply away does not keep restarting the panel. Armed once the gateway has
   answered at all, so one that does not answer pings never sets it off. */
namespace netwatch {

/* Call from the main loop; `joined`: the station is on its network. */
void tick(int64_t now_us, bool joined);

/* Restarts the watch has made in a row (kept across them). */
uint32_t restarts();

}  // namespace netwatch
