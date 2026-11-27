#pragma once

// Windows switches with no settings of astraeus' own: night light, hdr, the mobile hotspot and nearby sharing.
namespace sys::toggles {

bool NightLight(); // on now
bool SetNightLight(bool on); // as the settings app does it: the state windows keeps for it, which it follows at once

bool HdrCan(); // a screen that can show hdr
bool Hdr(); // on for every screen that can
bool SetHdr(bool on);

enum class Spot { Off, On, Busy, None }; // the hotspot: none when this pc can't share its connection
Spot Hotspot(); // slow the first time: off the ui thread
bool SetHotspot(bool on); // waits for windows: a worker thread only

bool Nearby(); // nearby sharing is on, for anyone or just your devices
void OpenNearby(); // windows lets only its own settings switch it

}
