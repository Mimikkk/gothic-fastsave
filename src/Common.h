#pragma once
#include <cstdio>

namespace Gothic_I_Classic {
  // Optimization switches from GOTHIC.INI [ZFASTSAVELOAD]: 0 = off, 1 = on, 2 = A/B (alternates per session).
  enum OptId { OPT_Waypoints, OPT_AniInit, OPT_WorldLists, OPT_FileRead, OPT_StringMap, OPT_FileWrite, OPT_COUNT };
  extern bool g_optOn[OPT_COUNT];          // effective state, recomputed at every session start
  extern const char* const g_optKeys[OPT_COUNT];

  bool IsMainThread();
  bool InSession();
  bool InLoadSession();
  bool InSaveSession();

  // implemented in Optimizations.cpp
  void Opt_SessionBegin();
  void Opt_SessionEnd();
  void Opt_PrototypeDestroyed();
  void Opt_WriteStats(FILE* f);
}
