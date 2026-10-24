#pragma once

namespace sys {

struct BatteryInfo { // a laptop's battery right now; -1 where windows doesn't know
    int pct = -1;
    bool ac = false, charging = false, saver = false;
    int fullMwh = -1, designMwh = -1, remainMwh = -1; // full charge capacity, as built, left now
    int rateMw = 0; // + charging, - draining
    int secsLeft = -1; // windows' own estimate on battery
    bool report = false; // the capacity and rate came from the battery's own report
};

BatteryInfo ReadBattery(bool report); // report: also ask the battery for capacity and rate, a little slower

}
