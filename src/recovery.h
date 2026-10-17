/*
   Recovery hold on the two front buttons.

   The two automatic paths back into the device -- no credentials stored, and
   repeated connection failures -- both assume the join does not succeed. They
   do nothing about the case where the device happily joins a network you cannot
   reach: a router that changed subnets, a guest VLAN, an SSID that turned out to
   be someone else's. Then there is no way in at all.

   This is that way in. It needs no network, no cable and no browser -- only
   physical access, which is exactly what someone standing at the panel has.

   Both buttons, held together for 15 s: the factory reset, the same as the
   web interface's -- everything stored goes, the panel configuration too.
   Each button alone will switch a relay, and a gesture that erases settings
   must not be reachable by switching a light.

   The right button alone, held 15 s, only opens the setup hotspot (erasing
   nothing): for a panel on a network you cannot reach, with its password
   known. The display shows how to join it.
*/

#pragma once

#include <cstdint>

namespace recovery {

void begin();

/* Poll from the main loop. Holding both buttons for kHoldUs is the factory
   reset: everything stored is erased and the panel reboots into its setup
   hotspot. */
void tick(int64_t now_us);

}  // namespace recovery
