#pragma once
#include "pages.h"

void DrawLive(PageCtx& p); // the live page: a card per picked sport, and the world's headlines
float LiveSheetK(Ui& ui); // the setup sheet's open anim, 0 while closed
void DrawLiveSetup(PageCtx& p, float k); // the setup sheet over everything, k = open anim 0..1
void LivePagesChanged(const std::vector<int>& was, const std::vector<int>& now, int livePage); // the live page joined the rail: the first time, setup opens
