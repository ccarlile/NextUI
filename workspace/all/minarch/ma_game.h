#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "defines.h"

void Game_open(char* path);
void Game_close(void);
void Game_changeDisc(char* path);
int extract_zip(char** extensions);

// Sidecar files are user-supplied files named after the running rom, eg.
// cheats (/Cheats/GB/<rom>.cht) or walkthroughs (/Walkthroughs/GB/<rom>.txt).
// note: SIDECAR_MAX_PATHS must be large enough to contain one entry per extension supported by a core, plus ~5 more
// 48 is enough: retroarch cores with most extensions at the moment is VICE VIC-20 at 37 and rom-cleaner at 42
#define SIDECAR_MAX_PATHS 48

// candidate paths in `dir` for the current game with extension `ext` (no dot), most specific first; may contain globs
void Game_getSidecarPaths(const char* dir, const char* ext, char paths[SIDECAR_MAX_PATHS][MAX_PATH], int* count);
// first existing candidate from Game_getSidecarPaths, written to out_path (MAX_PATH)
bool Game_findSidecar(const char* dir, const char* ext, char* out_path);
