#include "battery.h"

#include <windows.h>

#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Power.h>

namespace sys {

BatteryInfo ReadBattery(bool report) {
    BatteryInfo b;
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) { // one cheap call, nothing held
        b.ac = ps.ACLineStatus == 1;
        if (!(ps.BatteryFlag & BATTERY_FLAG_NO_BATTERY) && ps.BatteryLifePercent <= 100) b.pct = ps.BatteryLifePercent;
        b.charging = b.ac && (ps.BatteryFlag & BATTERY_FLAG_CHARGING);
        b.saver = ps.SystemStatusFlag == 1;
        if (ps.BatteryLifeTime != BATTERY_LIFE_UNKNOWN) b.secsLeft = (int)ps.BatteryLifeTime;
    }
    if (!report || b.pct < 0) return b;
    try { // every battery summed, as the system tray shows it
        namespace WDP = winrt::Windows::Devices::Power;
        auto r = WDP::Battery::AggregateBattery().GetReport();
        auto num = [](const winrt::Windows::Foundation::IReference<int32_t>& v) { return v ? (int)v.Value() : -1; };
        b.fullMwh = num(r.FullChargeCapacityInMilliwattHours());
        b.designMwh = num(r.DesignCapacityInMilliwattHours());
        b.remainMwh = num(r.RemainingCapacityInMilliwattHours());
        int rate = num(r.ChargeRateInMilliwatts());
        b.rateMw = rate == -1 ? 0 : rate;
        b.charging = r.Status() == winrt::Windows::System::Power::BatteryStatus::Charging;
        b.report = b.fullMwh > 0;
    } catch (...) {
        OutputDebugStringW(L"[sys] battery report unavailable\n");
    }
    return b;
}

}
