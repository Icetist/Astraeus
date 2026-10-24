#pragma once

// energy saver (24h2+, battery saver before it), the same switch halobar's quick settings use
namespace sys {

bool SaverAvailable();
bool SaverOn();
bool SetSaver(bool on); // false if it had to fall back to opening settings

}
