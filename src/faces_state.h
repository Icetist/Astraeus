#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "theme.h"

// What the faces keep between frames and restarts: the timer and stopwatch, the world's cities, water and habits, the keyboard and the theme.
namespace faces::state {

std::vector<std::wstring> Zones(size_t most); // the picked cities
SYSTEMTIME ZoneNow(const std::wstring& city);
std::wstring Offset(const std::wstring& city); // "+6h" against here

double TimerLeft(); // seconds
double TimerLength();
bool TimerRunning();
void TimerToggle();
void TimerReset();
void TimerCheck(); // rings once when it runs out

double WatchSeconds();
bool WatchRunning();
void WatchToggle();
void WatchReset();
void WatchLap();
const std::vector<double>& WatchLaps();

int Today(); // yyyymmdd
int Water();
int WaterGoal();
void WaterAdd(int by); // a glass more drunk also restarts the wait for the next reminder
int WaterMl(); // a glass, in ml
bool WaterDue(); // once a reminder: they're on, the goal isn't met and the next glass is due
void WaterLater(int seconds); // ask again after this long: snoozed, or nowhere to ask just now

struct Habit {
    std::wstring name;
    int last = 0, streak = 0;
};
std::vector<Habit> Habits();
void HabitAdd(const std::wstring& name);
void HabitFlip(size_t at);
void HabitDrop(size_t at);

std::wstring Language();
void NextLanguage();
bool LightTheme();
void SetLightTheme(bool light);

void DrawPhoto(Canvas& c, const Theme& t, const D2D1_RECT_F& r);
void NextPhoto();

}
