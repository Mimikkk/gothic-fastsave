// zFastSaveLoad - Union plugin for Gothic I (1.08k_mod)
// Stage 1: profiler. Measures where time is spent while saving / loading
// a savegame / changing level and writes a report to System\zFastSaveLoad.log

#include <Windows.h>
#include <Union/Hook.h>
#include <ZenGin/zGothicAPI.h>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>
#include <cstdio>
#include "Sampler.h"

HOOKSPACE(Gothic_I_Classic, GetGameVersion() == Engine_G1);

namespace Gothic_I_Classic {

// ---------------------------------------------------------------- timing
static double g_freqMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart / 1000.0; }();
static inline long long Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static inline double Ms(long long t) { return (double)t / g_freqMs; }

static DWORD g_mainThread = 0;
static bool  g_session = false;      // inside save/load
static int   g_depth = 0;            // tree depth (main thread only)
static long long g_sessionStart = 0;
static std::string g_sessionName;

struct TreeEvent { int depth; std::string name; long long start; long long ticks; int calls; };
static std::vector<TreeEvent> g_tree;
static std::vector<int> g_stack;  // indices of open tree events

struct Named { double ms; std::string name; };
struct Agg {
  const char* name;
  volatile LONG calls = 0;
  volatile LONG64 ticks = 0;
  volatile LONG64 bytes = 0;
  std::vector<Named> slowest;  // main thread only
  volatile LONG nesting = 0;   // to count only outermost for recursive funcs
};

enum AggId {
  A_VdfsOpen, A_VdfsRead, A_FileOpen, A_FileRead, A_FileWrite,
  A_TexLoad, A_SndLoad, A_LoadVisual, A_MeshLoad, A_ProgMeshLoad, A_MorphMMB,
  A_ModelProtoLoad, A_ModelASC, A_LoadMAN, A_LoadMDM, A_NpcUnarchive, A_NpcInitModel,
  A_ArcReadObj, A_ArcWriteObj, A_VobPostLoad,
  A_COUNT
};
static Agg g_agg[A_COUNT] = {
  {"zFILE_VDFS::Open"}, {"zFILE_VDFS::Read"}, {"zFILE_FILE::Open"}, {"zFILE_FILE::Read"}, {"zFILE_FILE::Write"},
  {"zCTexture::LoadResourceData"}, {"zCSndFX::LoadResourceData"}, {"zCVisual::LoadVisual"}, {"zCMesh::Load"},
  {"zCProgMeshProto::Load"}, {"zCMorphMeshProto::LoadMMB"},
  {"zCModelPrototype::Load"}, {"zCModelPrototype::LoadModelASC (!)"}, {"zCModelAni::LoadMAN"}, {"zCModelMeshLib::LoadMDM"},
  {"oCNpc::Unarchive"}, {"oCNpc::InitModel"},
  {"zCArchiverBinSafe::ReadObject"}, {"zCArchiverBinSafe::WriteObject"}, {"zCVob::PostLoad"},
};

static void AggReset() {
  for (auto& a : g_agg) { a.calls = 0; a.ticks = 0; a.bytes = 0; a.slowest.clear(); }
}

static void AggAdd(AggId id, long long ticks, long long bytes = 0, const char* name = nullptr) {
  Agg& a = g_agg[id];
  InterlockedIncrement(&a.calls);
  InterlockedAdd64(&a.ticks, ticks);
  if (bytes) InterlockedAdd64(&a.bytes, bytes);
  if (name && GetCurrentThreadId() == g_mainThread) {
    double ms = Ms(ticks);
    if (a.slowest.size() < 8 || ms > a.slowest.back().ms) {
      a.slowest.push_back({ ms, name });
      std::sort(a.slowest.begin(), a.slowest.end(), [](auto& x, auto& y) { return x.ms > y.ms; });
      if (a.slowest.size() > 8) a.slowest.pop_back();
    }
  }
}

// ---------------------------------------------------------------- report
static std::string LogPath() {
  char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string p = buf; p = p.substr(0, p.find_last_of("\\/") + 1);
  return p + "zFastSaveLoad.log";
}

// Profiler: 0 = off, 1 = call tree + sampling profiler (flamegraph data), 3 = call tree + per-call I/O hooks
static int  g_profLevel = 1;
static bool g_profile = true;
static FILE* OpenLog() { return g_profile ? fopen(LogPath().c_str(), "a") : nullptr; }

static void WriteAggs(FILE* f) {
  fprintf(f, "  -- aggregated (inclusive, may overlap) --\n");
  for (auto& a : g_agg) {
    if (!a.calls) continue;
    fprintf(f, "  %-40s calls=%7ld  total=%9.1f ms", a.name, a.calls, Ms(a.ticks));
    if (a.bytes) fprintf(f, "  bytes=%.2f MB", a.bytes / 1048576.0);
    fprintf(f, "\n");
    for (auto& s : a.slowest) fprintf(f, "        %8.2f ms  %s\n", s.ms, s.name.c_str());
  }
}

static void EnsureConfigured();
static void SessionBegin(const std::string& name) {
  EnsureConfigured();
  g_session = true; g_depth = 0; g_tree.clear(); g_stack.clear(); AggReset();
  g_sessionName = name; g_sessionStart = Now();
  if (g_profLevel == 1) { if (Sampler::Active()) Sampler::End("STARTUP"); Sampler::Begin(); }
}

static int g_watchFrames = 0;
static long long g_watchStart = 0, g_watchWorst = 0, g_watchSum = 0;
static int g_watchSlow = 0;

static void SessionEnd(bool watchFrames) {
  long long total = Now() - g_sessionStart;
  if (FILE* f = OpenLog()) {
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "\n===== %02d:%02d:%02d  %s  TOTAL %.1f ms =====\n", st.wHour, st.wMinute, st.wSecond, g_sessionName.c_str(), Ms(total));
    for (auto& e : g_tree) {
      fprintf(f, "  %8.1f ms  @%7.1f  %*s%s", Ms(e.ticks), Ms(e.start - g_sessionStart), e.depth * 2, "", e.name.c_str());
      if (e.calls > 1) fprintf(f, "  (x%d)", e.calls);
      fprintf(f, "\n");
    }
    WriteAggs(f);
    fclose(f);
  }
  g_session = false;
  if (g_profLevel == 1) Sampler::End(g_sessionName.c_str());
  if (watchFrames) {
    g_watchFrames = 300; g_watchStart = Now(); g_watchWorst = 0; g_watchSum = 0; g_watchSlow = 0;
    AggReset();
  }
}

struct TreeScope {
  int idx = -1; long long t0;
  TreeScope(const std::string& name) {
    t0 = Now();
    if (!g_session || !g_profile || GetCurrentThreadId() != g_mainThread) return;
    // recursion (e.g. DisposeVobs over the vob tree): count it in the outermost call only
    for (int i : g_stack) if (g_tree[i].name == name) return;
    // repeated sibling calls: merge into one line with a call counter
    for (int i = (int)g_tree.size() - 1; i >= 0 && g_tree[i].depth >= g_depth; i--) {
      if (g_tree[i].depth == g_depth) {
        if (g_tree[i].name == name) idx = i;
        break;
      }
    }
    if (idx < 0) { idx = (int)g_tree.size(); g_tree.push_back({ g_depth, name, t0, 0, 0 }); }
    g_tree[idx].calls++;
    g_stack.push_back(idx);
    g_depth++;
  }
  ~TreeScope() {
    if (idx < 0) return;
    g_tree[idx].ticks += Now() - t0; g_depth--; g_stack.pop_back();
  }
};

struct AggScope {
  AggId id; long long t0; long long bytes = 0; std::string name; bool outer;
  AggScope(AggId i) : id(i) { outer = InterlockedIncrement(&g_agg[i].nesting) == 1; t0 = Now(); }
  ~AggScope() {
    long long dt = Now() - t0;
    InterlockedDecrement(&g_agg[id].nesting);
    if (outer) AggAdd(id, dt, bytes, name.empty() ? nullptr : name.c_str());
  }
};

static std::string S(const zSTRING& s) { const char* c = s.ToChar(); return c ? c : ""; }
static std::string S(const void* p) { return p ? S(*(const zSTRING*)p) : std::string(); }
static std::string ObjName(void* obj) { return obj ? S(((zCObject*)obj)->GetObjectName()) : std::string(); }

struct zStrByVal { char raw[sizeof(zSTRING)]; };  // pass-through of a by-value zSTRING (callee destroys it)

#define HOOK(addr, name, ...) Union::CreateHook(reinterpret_cast<void*>(addr), name, Union::HookType::Hook_Detours)

// ================================================================ config
// Settings live in GOTHIC.INI, section [ZFASTSAVELOAD]. The Workshop launcher overwrites files shipped with
// the mod on every start, so a separate .ini would lose the user's changes. zCOption::ReadInt also adds
// missing keys with their defaults, so they show up in GOTHIC.INI after the first run.
static int IniInt(const char* key, int def) {
  if (!zoptions) return def;
  return zoptions->ReadInt(zSTRING("ZFASTSAVELOAD"), key, def);
}

// ================================================================ O1: keep resources alive across a load
// While the old world is disposed, every texture / mesh / model prototype whose refcount drops to 0
// is freed - and the very same files are then read & decoded again from the VDFs for the new world.
// We hold one extra reference on all of them for the duration of the load, so the engine finds them
// by name (SearchName / hash table) and reuses them. Afterwards our references are dropped, so
// anything the new world does not use is freed normally.

static std::unordered_set<zCObject*> g_keptObjs;
static std::unordered_set<zCModelPrototype*> g_keptProtos;
static bool g_keeping = false;
static int g_loadCounter = 0;
static std::string g_keepInfo;

static bool DerivesFrom(zCClassDef* d, zCClassDef* base) {
  for (int i = 0; d && i < 32; i++, d = d->baseClassDef) if (d == base) return true;
  return false;
}

static void KeepResources() {
  g_keptObjs.clear(); g_keptProtos.clear();
  zCClassDef* targets[] = {
    (zCClassDef*)0x008CF110, // zCTexture
    (zCClassDef*)0x008C5B48, // zCProgMeshProto (+ zCMeshSoftSkin)
    (zCClassDef*)0x00873CD8, // zCModelMeshLib
    (zCClassDef*)0x008CEBD0, // zCSoundFX
  };
  char buf[256];
  g_keepInfo.clear();
  auto* list = zCClassDef::classDefList;
  for (zCClassDef* base : targets) {
    int living = 0, listed = 0, kept = 0;
    for (int i = 0; list && i < list->numInArray; i++) {
      zCClassDef* d = list->array[i];
      if (!DerivesFrom(d, base)) continue;
      living += d->numLivingObjects; listed += d->objectList.numInArray;
      for (int j = 0; j < d->objectList.numInArray; j++) {
        zCObject* o = d->objectList.parray[j];
        if (!o || o->refCtr <= 0 || g_keptObjs.count(o)) continue;
        o->refCtr++; g_keptObjs.insert(o); kept++;
      }
    }
    sprintf(buf, "%s living=%d listed=%d kept=%d; ", base->className.ToChar(), living, listed, kept);
    g_keepInfo += buf;
  }
  int protos = 0;
  for (zCModelPrototype* p = zCModelPrototype::s_modelRoot; p && protos < 100000; p = p->next) {
    if (p->refCtr <= 0) continue;
    p->refCtr++; g_keptProtos.insert(p); protos++;
  }
  sprintf(buf, "zCModelPrototype kept=%d", protos);
  g_keepInfo += buf;
  g_keeping = true;
}

static void ReleaseResources() {
  // g_keeping stays true while releasing: a release may cascade and delete other kept objects,
  // the delete hooks then remove them from the sets before we touch them.
  int freed = 0;
  std::vector<zCObject*> objs(g_keptObjs.begin(), g_keptObjs.end());
  for (zCObject* o : objs) {
    if (!g_keptObjs.erase(o)) continue;
    if (o->refCtr <= 1) freed++;
    o->Release();
  }
  std::vector<zCModelPrototype*> protos(g_keptProtos.begin(), g_keptProtos.end());
  for (zCModelPrototype* p : protos) {
    if (!g_keptProtos.erase(p)) continue;
    if (p->refCtr <= 1) freed++;
    p->Release();
  }
  g_keeping = false;
  g_keptObjs.clear(); g_keptProtos.clear();
  g_keepInfo += "; unused after load (freed now)=" + std::to_string(freed);
}

// Safety net: if the engine destroys an object without going through Release, forget it.
void __cdecl ObjectDeleted_h(zCObject*, zCClassDef*);
auto Hook_ObjectDeleted = HOOK(0x0058B4A0, &ObjectDeleted_h);
void __cdecl ObjectDeleted_h(zCObject* o, zCClassDef* d) {
  if (g_keeping) g_keptObjs.erase(o);
  Hook_ObjectDeleted(o, d);
}

void __fastcall ProtoDtor_h(zCModelPrototype*, void*);
auto Hook_ProtoDtor = HOOK(0x0056E240, &ProtoDtor_h);
void __fastcall ProtoDtor_h(zCModelPrototype* p, void* edx) {
  if (g_keeping) g_keptProtos.erase(p);
  Hook_ProtoDtor(p, edx);
}

static bool ShouldKeep(bool changeLevel) {
  int mode = IniInt(changeLevel ? "KeepResourcesOnChangeLevel" : "KeepResources", changeLevel ? 0 : 1);
  if (mode == 2) return (g_loadCounter++ % 2) == 1;  // A/B: off, on, off, on...
  return mode == 1;
}

// ================================================================ session roots
void __fastcall WriteSavegame_h(oCGame*, void*, int, int);
auto Hook_WriteSavegame = HOOK(0x0063AD80, &WriteSavegame_h);
void __fastcall WriteSavegame_h(oCGame* self, void* edx, int slot, int globals) {
  bool root = !g_session;
  if (root) SessionBegin("SAVE slot " + std::to_string(slot));
  { TreeScope s("oCGame::WriteSavegame"); Hook_WriteSavegame(self, edx, slot, globals); }
  if (root) SessionEnd(false);
}

void __fastcall LoadSavegame_h(oCGame*, void*, int, int);
auto Hook_LoadSavegame = HOOK(0x0063C2A0, &LoadSavegame_h);
void __fastcall LoadSavegame_h(oCGame* self, void* edx, int slot, int globals) {
  bool root = !g_session;
  bool keep = root && ShouldKeep(false);
  if (root) SessionBegin("LOAD slot " + std::to_string(slot) + (keep ? "  [KeepResources=ON]" : "  [KeepResources=OFF]"));
  if (keep) { TreeScope s("zFastSaveLoad::KeepResources"); KeepResources(); }
  { TreeScope s("oCGame::LoadSavegame"); Hook_LoadSavegame(self, edx, slot, globals); }
  if (keep) { TreeScope s("zFastSaveLoad::ReleaseResources"); ReleaseResources(); }
  if (keep) g_tree.push_back({ 0, "info: " + g_keepInfo, Now(), 0 });
  if (root) SessionEnd(true);
}

void __fastcall LoadGame_h(oCGame*, void*, int, const zSTRING&);
auto Hook_LoadGame = HOOK(0x0063C070, &LoadGame_h);
void __fastcall LoadGame_h(oCGame* self, void* edx, int slot, const zSTRING& lvl) {
  bool root = !g_session;
  if (root) SessionBegin("NEW GAME " + S(lvl));
  { TreeScope s("oCGame::LoadGame"); Hook_LoadGame(self, edx, slot, lvl); }
  if (root) SessionEnd(true);
}

void __fastcall ChangeLevel_h(oCGame*, void*, const zSTRING&, const zSTRING&);
auto Hook_ChangeLevel = HOOK(0x0063CD60, &ChangeLevel_h);
void __fastcall ChangeLevel_h(oCGame* self, void* edx, const zSTRING& lvl, const zSTRING& wp) {
  bool root = !g_session;
  bool keep = root && ShouldKeep(true);
  if (root) SessionBegin("CHANGELEVEL " + S(lvl) + (keep ? "  [KeepResources=ON]" : ""));
  if (keep) KeepResources();
  { TreeScope s("oCGame::ChangeLevel " + S(lvl)); Hook_ChangeLevel(self, edx, lvl, wp); }
  if (keep) { ReleaseResources(); g_tree.push_back({ 0, "info: " + g_keepInfo, Now(), 0 }); }
  if (root) SessionEnd(true);
}

// ================================================================ tree functions (thiscall, simple)
#define TREE_V0(addr, NAME, label) \
  void __fastcall NAME##_h(void*, void*); auto Hook_##NAME = HOOK(addr, &NAME##_h); \
  void __fastcall NAME##_h(void* self, void* edx) { TreeScope s(label); Hook_##NAME(self, edx); }
#define TREE_V1(addr, NAME, label, T1) \
  void __fastcall NAME##_h(void*, void*, T1); auto Hook_##NAME = HOOK(addr, &NAME##_h); \
  void __fastcall NAME##_h(void* self, void* edx, T1 a) { TreeScope s(label); Hook_##NAME(self, edx, a); }
#define TREE_I1(addr, NAME, label, T1) \
  int __fastcall NAME##_h(void*, void*, T1); auto Hook_##NAME = HOOK(addr, &NAME##_h); \
  int __fastcall NAME##_h(void* self, void* edx, T1 a) { TreeScope s(label); return Hook_##NAME(self, edx, a); }

TREE_V1(0x0063AC50, PreSave,  "oCGame::PreSaveGameProcessing", int)
TREE_V0(0x0063ACF0, PostSave, "oCGame::PostSaveGameProcessing")
TREE_V0(0x0063B9E0, ClearGameState, "oCGame::ClearGameState")
TREE_V0(0x0063BD00, InitNpcAttitudes, "oCGame::InitNpcAttitudes")
TREE_V0(0x0063BE00, CacheVisualsIn, "oCGame::CacheVisualsIn")
TREE_V0(0x0063C000, CacheVisualsOut, "oCGame::CacheVisualsOut")
TREE_V0(0x0063EFD0, CompileWorld, "oCGame::CompileWorld")
TREE_V0(0x00636170, WorldInit, "oCGame::WorldInit")
TREE_V0(0x00636BF0, NpcInit, "oCGame::NpcInit")
TREE_V1(0x00638080, OpenSavescreen, "oCGame::OpenSavescreen", bool)
TREE_V0(0x00638460, CloseSavescreen, "oCGame::CloseSavescreen")
TREE_V0(0x00638A30, CloseLoadscreen, "oCGame::CloseLoadscreen")
TREE_V0(0x005F5800, zDisposeWorld, "zCWorld::DisposeWorld")
TREE_V0(0x005F5790, zDisposeStaticWorld, "zCWorld::DisposeStaticWorld")
TREE_V0(0x006D7B10, oDisposeWorld, "oCWorld::DisposeWorld")
TREE_V0(0x006D7990, oDisposeVobs, "oCWorld::DisposeVobs")
TREE_I1(0x005F55F0, zDisposeVobs, "zCWorld::DisposeVobs(tree)", void*)
TREE_V1(0x005F8300, zWorldUnarchive, "zCWorld::Unarchive", void*)
TREE_V1(0x00708530, WayNetUnarchive, "zCWayNet::Unarchive", void*)
TREE_V1(0x00435F70, CopyFromCurrent, "oCSavegameManager::CopyFromCurrent", int)
TREE_V1(0x00435DC0, CopyToCurrent, "oCSavegameManager::CopyToCurrent", int)
TREE_V0(0x00436120, ClearCurrent, "oCSavegameManager::ClearCurrent")
TREE_V0(0x004355B0, SgmInit, "oCSavegameManager::Init")
TREE_V0(0x00435B20, SgmReinit, "oCSavegameManager::Reinit")
TREE_V1(0x00434100, UpdateThumbPic, "oCSavegameInfo::UpdateThumbPic", void*)
TREE_I1(0x006ED5C0, SaveGlobalVars, "zCParser::SaveGlobalVars", void*)
TREE_I1(0x006ED8C0, LoadGlobalVars, "zCParser::LoadGlobalVars", void*)

void __fastcall SetAndWrite_h(void*, void*, int, void*);
auto Hook_SetAndWrite = HOOK(0x00435B90, &SetAndWrite_h);
void __fastcall SetAndWrite_h(void* self, void* edx, int slot, void* info) { TreeScope s("oCSavegameManager::SetAndWriteSavegame"); Hook_SetAndWrite(self, edx, slot, info); }

void __fastcall OpenLoadscreen_h(void*, void*, bool, zStrByVal);
auto Hook_OpenLoadscreen = HOOK(0x006384C0, &OpenLoadscreen_h);
void __fastcall OpenLoadscreen_h(void* self, void* edx, bool b, zStrByVal s) { TreeScope sc("oCGame::OpenLoadscreen"); Hook_OpenLoadscreen(self, edx, b, s); }

void __fastcall oSaveWorld_h(void*, void*, const zSTRING&, int, int, int);
auto Hook_oSaveWorld = HOOK(0x0063AA20, &oSaveWorld_h);
void __fastcall oSaveWorld_h(void* self, void* edx, const zSTRING& n, int m, int a, int b) { TreeScope s("oCGame::SaveWorld " + S(n)); Hook_oSaveWorld(self, edx, n, m, a, b); }

int __fastcall zSaveWorld_h(void*, void*, const zSTRING&, int, int, int);
auto Hook_zSaveWorld = HOOK(0x005F9330, &zSaveWorld_h);
int __fastcall zSaveWorld_h(void* self, void* edx, const zSTRING& n, int m, int a, int b) { TreeScope s("zCWorld::SaveWorld mode=" + std::to_string(m) + " ascii=" + std::to_string(a)); return Hook_zSaveWorld(self, edx, n, m, a, b); }

int __fastcall oLoadWorld_h(void*, void*, const zSTRING&, int);
auto Hook_oLoadWorld = HOOK(0x0063A9D0, &oLoadWorld_h);
int __fastcall oLoadWorld_h(void* self, void* edx, const zSTRING& n, int m) { TreeScope s("oCGame::LoadWorld " + S(n) + " mode=" + std::to_string(m)); return Hook_oLoadWorld(self, edx, n, m); }

int __fastcall zLoadWorld_h(void*, void*, const zSTRING&, int);
auto Hook_zLoadWorld = HOOK(0x005F8A00, &zLoadWorld_h);
int __fastcall zLoadWorld_h(void* self, void* edx, const zSTRING& n, int m) { TreeScope s("zCWorld::LoadWorld " + S(n) + " mode=" + std::to_string(m)); return Hook_zLoadWorld(self, edx, n, m); }

int __fastcall LoadBspTree_h(void*, void*, void*, int);
auto Hook_LoadBspTree = HOOK(0x005F7C40, &LoadBspTree_h);
int __fastcall LoadBspTree_h(void* self, void* edx, void* file, int a) { TreeScope s("zCWorld::LoadBspTree"); return Hook_LoadBspTree(self, edx, file, a); }

void __fastcall LoadWorldStartup_h(void*, void*, const zSTRING&);
auto Hook_LoadWorldStartup = HOOK(0x0063F190, &LoadWorldStartup_h);
void __fastcall LoadWorldStartup_h(void* self, void* edx, const zSTRING& n) { TreeScope s("oCGame::LoadWorldStartup " + S(n)); Hook_LoadWorldStartup(self, edx, n); }

void __fastcall LoadWorldStat_h(void*, void*, zStrByVal);
auto Hook_LoadWorldStat = HOOK(0x0063F590, &LoadWorldStat_h);
void __fastcall LoadWorldStat_h(void* self, void* edx, zStrByVal n) { TreeScope s("oCGame::LoadWorldStat " + S((const zSTRING*)&n)); Hook_LoadWorldStat(self, edx, n); }

void __fastcall LoadWorldDyn_h(void*, void*, const zSTRING&);
auto Hook_LoadWorldDyn = HOOK(0x0063F800, &LoadWorldDyn_h);
void __fastcall LoadWorldDyn_h(void* self, void* edx, const zSTRING& n) { TreeScope s("oCGame::LoadWorldDyn " + S(n)); Hook_LoadWorldDyn(self, edx, n); }

void __fastcall InitWorldSavegame_h(void*, void*, int&, zSTRING&);
auto Hook_InitWorldSavegame = HOOK(0x0063E910, &InitWorldSavegame_h);
void __fastcall InitWorldSavegame_h(void* self, void* edx, int& a, zSTRING& n) { TreeScope s("oCGame::InitWorldSavegame"); Hook_InitWorldSavegame(self, edx, a, n); }

void __fastcall EnterWorld_h(void*, void*, void*, int, const zSTRING&);
auto Hook_EnterWorld = HOOK(0x0063EAD0, &EnterWorld_h);
void __fastcall EnterWorld_h(void* self, void* edx, void* npc, int a, const zSTRING& n) { TreeScope s("oCGame::EnterWorld"); Hook_EnterWorld(self, edx, npc, a, n); }

// ================================================================ aggregated hot functions
int __fastcall VdfsOpen_h(void*, void*, const zSTRING&, bool);
Union::Hook<decltype(&VdfsOpen_h)> Hook_VdfsOpen(nullptr);  // installed only with Profiler=3
int __fastcall VdfsOpen_h(void* self, void* edx, const zSTRING& n, bool w) { AggScope s(A_VdfsOpen); return Hook_VdfsOpen(self, edx, n, w); }

long __fastcall VdfsRead_h(void*, void*, void*, long);
Union::Hook<decltype(&VdfsRead_h)> Hook_VdfsRead(nullptr);  // installed only with Profiler=3
long __fastcall VdfsRead_h(void* self, void* edx, void* buf, long n) { AggScope s(A_VdfsRead); long r = Hook_VdfsRead(self, edx, buf, n); s.bytes = r > 0 ? r : 0; return r; }

int __fastcall FileOpen_h(void*, void*, const zSTRING&, bool);
Union::Hook<decltype(&FileOpen_h)> Hook_FileOpen(nullptr);  // installed only with Profiler=3
int __fastcall FileOpen_h(void* self, void* edx, const zSTRING& n, bool w) { AggScope s(A_FileOpen); return Hook_FileOpen(self, edx, n, w); }

long __fastcall FileRead_h(void*, void*, void*, long);
Union::Hook<decltype(&FileRead_h)> Hook_FileRead(nullptr);  // installed only with Profiler=3
long __fastcall FileRead_h(void* self, void* edx, void* buf, long n) { AggScope s(A_FileRead); long r = Hook_FileRead(self, edx, buf, n); s.bytes = r > 0 ? r : 0; return r; }

long __fastcall FileWrite_h(void*, void*, const void*, long);
Union::Hook<decltype(&FileWrite_h)> Hook_FileWrite(nullptr);  // installed only with Profiler=3
long __fastcall FileWrite_h(void* self, void* edx, const void* buf, long n) { AggScope s(A_FileWrite); s.bytes = n; return Hook_FileWrite(self, edx, buf, n); }

int __fastcall TexLoad_h(void*, void*);
Union::Hook<decltype(&TexLoad_h)> Hook_TexLoad(nullptr);  // installed only with Profiler=3
int __fastcall TexLoad_h(void* self, void* edx) { AggScope s(A_TexLoad); if (GetCurrentThreadId() == g_mainThread) s.name = ObjName(self); return Hook_TexLoad(self, edx); }

int __fastcall SndLoad_h(void*, void*);
Union::Hook<decltype(&SndLoad_h)> Hook_SndLoad(nullptr);  // installed only with Profiler=3
int __fastcall SndLoad_h(void* self, void* edx) { AggScope s(A_SndLoad); return Hook_SndLoad(self, edx); }

void* __cdecl LoadVisual_h(const zSTRING&);
Union::Hook<decltype(&LoadVisual_h)> Hook_LoadVisual(nullptr);  // installed only with Profiler=3
void* __cdecl LoadVisual_h(const zSTRING& n) { AggScope s(A_LoadVisual); s.name = S(n); return Hook_LoadVisual(n); }

void* __cdecl MeshLoad_h(const zSTRING&);
Union::Hook<decltype(&MeshLoad_h)> Hook_MeshLoad(nullptr);  // installed only with Profiler=3
void* __cdecl MeshLoad_h(const zSTRING& n) { AggScope s(A_MeshLoad); s.name = S(n); return Hook_MeshLoad(n); }

int __fastcall ProgMeshLoad_h(void*, void*, void*);
Union::Hook<decltype(&ProgMeshLoad_h)> Hook_ProgMeshLoad(nullptr);  // installed only with Profiler=3
int __fastcall ProgMeshLoad_h(void* self, void* edx, void* f) { AggScope s(A_ProgMeshLoad); return Hook_ProgMeshLoad(self, edx, f); }

int __fastcall MorphMMB_h(void*, void*, const zSTRING&);
Union::Hook<decltype(&MorphMMB_h)> Hook_MorphMMB(nullptr);  // installed only with Profiler=3
int __fastcall MorphMMB_h(void* self, void* edx, const zSTRING& n) { AggScope s(A_MorphMMB); s.name = S(n); return Hook_MorphMMB(self, edx, n); }

void* __cdecl ModelProtoLoad_h(const zSTRING&, void*);
Union::Hook<decltype(&ModelProtoLoad_h)> Hook_ModelProtoLoad(nullptr);  // installed only with Profiler=3
void* __cdecl ModelProtoLoad_h(const zSTRING& n, void* p) { AggScope s(A_ModelProtoLoad); s.name = S(n); return Hook_ModelProtoLoad(n, p); }

int __fastcall ModelASC_h(void*, void*, const zSTRING&);
Union::Hook<decltype(&ModelASC_h)> Hook_ModelASC(nullptr);  // installed only with Profiler=3
int __fastcall ModelASC_h(void* self, void* edx, const zSTRING& n) { AggScope s(A_ModelASC); s.name = S(n); return Hook_ModelASC(self, edx, n); }

int __fastcall LoadMAN_h(void*, void*, const zSTRING&, void*, const zSTRING&);
Union::Hook<decltype(&LoadMAN_h)> Hook_LoadMAN(nullptr);  // installed only with Profiler=3
int __fastcall LoadMAN_h(void* self, void* edx, const zSTRING& n, void* p, const zSTRING& m) { AggScope s(A_LoadMAN); s.name = S(n); return Hook_LoadMAN(self, edx, n, p, m); }

int __cdecl LoadMDM_h(const zSTRING&, void*, void*, void*);
Union::Hook<decltype(&LoadMDM_h)> Hook_LoadMDM(nullptr);  // installed only with Profiler=3
int __cdecl LoadMDM_h(const zSTRING& n, void* a, void* b, void* c) { AggScope s(A_LoadMDM); s.name = S(n); return Hook_LoadMDM(n, a, b, c); }

void __fastcall NpcUnarchive_h(void*, void*, void*);
Union::Hook<decltype(&NpcUnarchive_h)> Hook_NpcUnarchive(nullptr);  // installed only with Profiler=3
void __fastcall NpcUnarchive_h(void* self, void* edx, void* arc) { AggScope s(A_NpcUnarchive); Hook_NpcUnarchive(self, edx, arc); }

void __fastcall NpcInitModel_h(void*, void*);
Union::Hook<decltype(&NpcInitModel_h)> Hook_NpcInitModel(nullptr);  // installed only with Profiler=3
void __fastcall NpcInitModel_h(void* self, void* edx) { AggScope s(A_NpcInitModel); Hook_NpcInitModel(self, edx); }

// zCArchiverBinSafe::ReadObject(char const*, zCObject*)  -- __fastcall member: ecx=this, edx=name
void* __fastcall ArcReadObj_h(void*, const char*, void*);
Union::Hook<decltype(&ArcReadObj_h)> Hook_ArcReadObj(nullptr);  // installed only with Profiler=3
void* __fastcall ArcReadObj_h(void* self, const char* n, void* o) { AggScope s(A_ArcReadObj); return Hook_ArcReadObj(self, n, o); }

void __fastcall ArcWriteObj_h(void*, const char*, void*);
Union::Hook<decltype(&ArcWriteObj_h)> Hook_ArcWriteObj(nullptr);  // installed only with Profiler=3
void __fastcall ArcWriteObj_h(void* self, const char* n, void* o) { AggScope s(A_ArcWriteObj); Hook_ArcWriteObj(self, n, o); }

// ================================================================ frames after load (lazy cache-in stalls)
void __fastcall Render_h(void*, void*);
auto Hook_Render = HOOK(0x0063DBE0, &Render_h);
void __fastcall Render_h(void* self, void* edx) {
  if (!g_mainThread) g_mainThread = GetCurrentThreadId();
  EnsureConfigured();
  if (g_watchFrames <= 0 || g_session) { Hook_Render(self, edx); return; }
  long long t0 = Now();
  Hook_Render(self, edx);
  long long dt = Now() - t0;
  g_watchSum += dt;
  if (dt > g_watchWorst) g_watchWorst = dt;
  if (Ms(dt) > 50.0) g_watchSlow++;
  if (--g_watchFrames == 0) {
    if (FILE* f = OpenLog()) {
      fprintf(f, "  -- first 300 frames after load: wall=%.1f ms, render=%.1f ms, worst frame=%.1f ms, frames>50ms=%d --\n",
              Ms(Now() - g_watchStart), Ms(g_watchSum), Ms(g_watchWorst), g_watchSlow);
      WriteAggs(f);
      fclose(f);
    }
  }
}

void __fastcall MenuRender_h(void*, void*);
auto Hook_MenuRender = HOOK(0x004D0DA0, &MenuRender_h);
void __fastcall MenuRender_h(void* self, void* edx) {
  static bool first = true;
  if (first) {
    first = false;
    if (Sampler::Active() && !g_session) {
      Sampler::End("STARTUP");
      if (FILE* f = OpenLog()) { fprintf(f, "\n===== STARTUP (DLL load -> first menu frame) sampled =====\n"); fclose(f); }
    }
  }
  Hook_MenuRender(self, edx);
}

// Per-call I/O and resource hooks cost time themselves, so they are only installed with Profiler=3.
// Never use Hook::Disable(): union-api keeps the hook list in a singleton shared by all plugins, and
// removing an entry reallocates memory owned by another plugin's CRT (heap corruption -> crash).
static void InstallHotHooks() {
  // the HookSpace condition is shared between plugins; make sure ours is the active one while creating hooks
  auto& space = Union::HookSpace::GetCurrentInstance();
  auto saved = space.Condition;
  space.Condition = []() -> bool { return true; };
  Hook_VdfsOpen = HOOK(0x00444CA0, &VdfsOpen_h);
  Hook_VdfsRead = HOOK(0x004468C0, &VdfsRead_h);
  Hook_FileOpen = HOOK(0x0043F450, &FileOpen_h);
  Hook_FileRead = HOOK(0x00440980, &FileRead_h);
  Hook_FileWrite = HOOK(0x00440510, &FileWrite_h);
  Hook_TexLoad = HOOK(0x005CA5E0, &TexLoad_h);
  Hook_SndLoad = HOOK(0x004E7720, &SndLoad_h);
  Hook_LoadVisual = HOOK(0x005DA1F0, &LoadVisual_h);
  Hook_MeshLoad = HOOK(0x005501A0, &MeshLoad_h);
  Hook_ProgMeshLoad = HOOK(0x005A8F20, &ProgMeshLoad_h);
  Hook_MorphMMB = HOOK(0x00584230, &MorphMMB_h);
  Hook_ModelProtoLoad = HOOK(0x0056EA80, &ModelProtoLoad_h);
  Hook_ModelASC = HOOK(0x0057EB20, &ModelASC_h);
  Hook_LoadMAN = HOOK(0x0056CEB0, &LoadMAN_h);
  Hook_LoadMDM = HOOK(0x00580780, &LoadMDM_h);
  Hook_NpcUnarchive = HOOK(0x006A31E0, &NpcUnarchive_h);
  Hook_NpcInitModel = HOOK(0x00695020, &NpcInitModel_h);
  Hook_ArcReadObj = HOOK(0x0050F780, &ArcReadObj_h);
  Hook_ArcWriteObj = HOOK(0x0050E450, &ArcWriteObj_h);
  space.Condition = saved;
}

static void EnsureConfigured() {
  static bool configured = false;
  if (configured || !zoptions) return;
  configured = true;
  g_profLevel = IniInt("Profiler", 1);
  g_profile = g_profLevel != 0;
  IniInt("KeepResources", 1); IniInt("KeepResourcesOnChangeLevel", 0);  // make the keys appear in GOTHIC.INI
  if (g_profLevel == 3) InstallHotHooks();
  if (g_profile) if (FILE* f = OpenLog()) { fprintf(f, "\n##### zFastSaveLoad profiler active (level %d) #####\n", g_profLevel); fclose(f); }
}

} // namespace Gothic_I_Classic

BOOL WINAPI DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    if (GetGameVersion() != Engine_G1) return FALSE;
    Gothic_I_Classic::g_mainThread = GetCurrentThreadId();
    // zoptions does not exist yet: read the profiler level straight from GOTHIC.INI to profile the startup
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe; dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    int level = GetPrivateProfileIntA("ZFASTSAVELOAD", "Profiler", 1, (dir + "GOTHIC.INI").c_str());
    if (level == 1) {
      Sampler::Init((dir + "zFastSaveLoad_prof").c_str());
      Sampler::BeginAsync();
    }
  }
  return TRUE;
}
