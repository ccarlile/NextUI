#pragma once

#include <stdbool.h>

// Walkthroughs are plain .txt files named after the rom, just like cheats, eg.
// /Walkthroughs/GB/Super Mario Land (World).zip.txt or /Walkthroughs/GB/Super Mario Land.txt
// They are opened from the in-game Options menu, see OptionWalkthrough_openMenu.

void Walkthrough_init(void); // look up the walkthrough for the loaded game, call after Core_load
void Walkthrough_free(void);
bool Walkthrough_available(void);
