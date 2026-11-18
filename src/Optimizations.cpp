// zFastSaveLoad - algorithmic optimizations of Gothic I (1.08k_mod) save/load paths.
//
// Every replacement reproduces the exact behaviour of the original engine function as read from the
// disassembly of GothicMod.exe (addresses below). Each one has a GOTHIC.INI switch in [ZFASTSAVELOAD].
//
//  FastWaypoints  zCWayNet::GetWaypoint       binary search over a linked list -> same search over an array
//  FastAniInit    oCAniCtrl_Human::InitAllAnis builds ~600 animation names per NPC -> self-verified memo
//  FastWorldLists oCWorld::InsertInLists      linear "already in list?" scans -> hash set of members
//  FastFileRead   zFILE_VDFS::Read            1-byte / few-byte reads through vdfs32g -> 64 KB read-ahead
//  FastStringMap  GETStringMap (97 buckets)   long hash chains in the archiver -> shadow hash index
//  FastFileWrite  zFILE_FILE::Write           2M tiny fwrite calls while saving -> 64 KB write buffer

#include <Windows.h>
#include <Union/Hook.h>
#include <ZenGin/zGothicAPI.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include "Common.h"

HOOKSPACE(Gothic_I_Classic_Opt, GetGameVersion() == Engine_G1);

namespace Gothic_I_Classic {

#define HOOK(addr, fn) Union::CreateHook(reinterpret_cast<void*>(addr), fn, Union::HookType::Hook_Detours)

struct Stats { long hintHit, hintMiss, removeSkip, wpHasFast; long fastWp, fastAniHit, fastAniCheck, fastAniBad, listSkip, readServed, readFills, mapFast, mapRebuild, writeBuffered, writeFlushes; };
static Stats S{};

// engine string layout: zSTRING = { vtbl, std::string { alloc, char* ptr, size_t len, size_t cap } }
static inline const char* ZPtr(const void* z) { const char* p = *(const char* const*)((const char*)z + 8); return p ? p : ""; }
static inline unsigned ZLen(const void* z) { return *(const unsigned*)((const char*)z + 12); }

// ---------------------------------------------------------------- hinted sorted-list insertion
typedef void(__thiscall* InsSortFn)(void* list, void* elem);
typedef int(__cdecl* ListCmpFn)(void* a, void* b);

// zCListSort<T>::InsertSort inserts after every element e with Compare(new, e) >= 0 (stable upper bound).
// If Compare(new, lastInserted) >= 0 the walk may start at lastInserted: identical result. We start the
// original function on a fake list header whose "next" is the hint node; since the first comparison is
// >= 0, the fake header itself is never modified and the node is linked into the real list.
struct Hint { void* list = nullptr; void* node = nullptr; void* data = nullptr; long hits = 0, misses = 0; bool off = false; };
static Hint g_hint[4];   // 0 voblist, 1 npcs, 2 items, 3 waypoints
static thread_local bool t_capture = false;
static thread_local void* t_captured = nullptr;
static void HintsReset() { for (auto& h : g_hint) h = Hint{}; }
static void HintForget(void* data) { for (auto& h : g_hint) if (h.data == data) { h.node = nullptr; h.data = nullptr; } }

void* __fastcall PoolAlloc_h(void*, void*);
auto Hook_PoolAlloc = HOOK(0x0054F170, &PoolAlloc_h);
void* __fastcall PoolAlloc_h(void* pool, void* edx) {
  void* r = Hook_PoolAlloc(pool, edx);
  if (t_capture && !t_captured) t_captured = r;
  return r;
}

static void HintedInsert(InsSortFn orig, Hint& h, void* list, void* elem) {
  // Only for the oCWorld vob lists, whose comparators are pure address differences (0x006D5EB0 /
  // 0x006D5EC0 / 0x006D5EE0). A comparator with side effects (e.g. zCWayNet::SortWpList) must see exactly
  // the original sequence of comparisons.
  uintptr_t c = *(uintptr_t*)list;
  if (c != 0x006D5EB0 && c != 0x006D5EC0 && c != 0x006D5EE0) { orig(list, elem); return; }
  if (!h.off && h.list == list && h.node && *(void**)((char*)h.node + 4) == h.data) {
    ListCmpFn cmp = *(ListCmpFn*)list;
    if (cmp(elem, h.data) >= 0) {
      struct { void* cmp; void* data; void* next; } fake = { *(void**)list, nullptr, h.node };
      t_capture = true; t_captured = nullptr;
      orig(&fake, elem);
      t_capture = false;
      if (t_captured) { h.node = t_captured; h.data = elem; }
      h.hits++; S.hintHit++;
      return;
    }
    h.misses++; S.hintMiss++;
    if (h.misses > 64 && h.misses > h.hits) h.off = true;   // insert order does not follow the sort order
  }
  t_capture = !h.off; t_captured = nullptr;
  orig(list, elem);
  t_capture = false;
  if (!h.off && t_captured) { h.list = list; h.node = t_captured; h.data = elem; }
}

// =====================================================================================================
// FastWaypoints - zCWayNet::GetWaypoint (0x00706730, __fastcall: ecx = this, edx = const zSTRING&)
// The original binary-searches wplist (a sorted *linked list*: every probe walks the list from the
// head). Same algorithm, same probes, same side effect (probe copies its vob name into wp->name),
// but on an array snapshot of the list.
// =====================================================================================================
struct WpCache { void* net = nullptr; void* head = nullptr; std::vector<void*> arr; long calls = 0; };
static WpCache g_wp;

static void WpInvalidate() { g_wp.net = nullptr; g_wp.arr.clear(); }

static void WpBuild(void* net) {
  g_wp.arr.clear();
  for (void* node = *(void**)((char*)net + 0x30); node; node = *(void**)((char*)node + 8))
    g_wp.arr.push_back(*(void**)((char*)node + 4));
  g_wp.net = net; g_wp.head = *(void**)((char*)net + 0x30); g_wp.calls = 0;
}

typedef void* (__thiscall* StrAssignFn)(void* self, const void* src, unsigned pos, unsigned n);
static const StrAssignFn StrAssign = (StrAssignFn)0x00402E10;

void* __fastcall GetWaypoint_h(void* net, const void* name);
auto Hook_GetWaypoint = HOOK(0x00706730, &GetWaypoint_h);
void* __fastcall GetWaypoint_h(void* net, const void* name) {
  if (!g_optOn[OPT_Waypoints] || !IsMainThread()) return Hook_GetWaypoint(net, name);
  if (g_wp.net != net || g_wp.head != *(void**)((char*)net + 0x30) || ++g_wp.calls > 8192) WpBuild(net);
  auto& a = g_wp.arr;
  int n = (int)a.size();
  if (n <= 0) return nullptr;
  const char* q = ZPtr(name); unsigned ql = ZLen(name);
  int lo = 0, hi = n - 1, mid = (lo + hi) / 2;
  for (;;) {
    char* wp = (char*)a[mid];
    if (void* vob = *(void**)(wp + 0x70)) {                       // wp->name = wp->wpvob->GetObjectName()
      const zSTRING& on = ((zCObject*)vob)->GetObjectName();
      StrAssign(wp + 0x60, (const char*)&on + 4, 0, 0xFFFFFFFFu);
    }
    const char* w = ZPtr(wp + 0x5C); unsigned wl = ZLen(wp + 0x5C);
    int c = memcmp(q, w, ql < wl ? ql : wl);
    if (c == 0) c = ql < wl ? -1 : (ql != wl ? 1 : 0);
    else c = c < 0 ? -1 : 1;
    if (hi > lo) {
      if (c > 0) lo = mid + 1;
      else if (c < 0) hi = mid - 1;
      else { S.fastWp++; return wp; }
      mid = (lo + hi) / 2;
    } else {
      S.fastWp++;
      return c == 0 ? wp : nullptr;
    }
  }
}

// anything that may change the waypoint list drops the snapshot
void __fastcall WnIns1_h(void*, void*, float, float, float);
auto Hook_WnIns1 = HOOK(0x007033E0, &WnIns1_h);
void __fastcall WnIns1_h(void* s, void* e, float x, float y, float z) { WpInvalidate(); Hook_WnIns1(s, e, x, y, z); WpInvalidate(); }
void __fastcall WnIns2_h(void*, void*, void*);
auto Hook_WnIns2 = HOOK(0x007034F0, &WnIns2_h);
void __fastcall WnIns2_h(void* s, void* e, void* a) { WpInvalidate(); Hook_WnIns2(s, e, a); WpInvalidate(); }
void __fastcall WnIns3_h(void*, void*, void*, void*, void*);
auto Hook_WnIns3 = HOOK(0x00703520, &WnIns3_h);
void __fastcall WnIns3_h(void* s, void* e, void* a, void* b, void* c) { WpInvalidate(); Hook_WnIns3(s, e, a, b, c); WpInvalidate(); }
void __fastcall WnDel_h(void*, void*, void*);
auto Hook_WnDel = HOOK(0x007036A0, &WnDel_h);
void __fastcall WnDel_h(void* s, void* e, void* a) { WpInvalidate(); Hook_WnDel(s, e, a); WpInvalidate(); }
void __fastcall WnUnarc_h(void*, void*, void*);
auto Hook_WnUnarc = HOOK(0x00708530, &WnUnarc_h);
static void* g_wnUnarcNet = nullptr;
static bool g_wnBuilt = false;
static std::unordered_set<void*> g_wnMembers;
void __fastcall WnUnarc_h(void* s, void* e, void* a) {
  WpInvalidate();
  g_hint[3] = Hint{};
  // The member set is built lazily at the first HasWaypoint call (not here): Unarchive may drop the old
  // waypoints first, and a new waypoint allocated at a freed address must not be seen as present.
  bool fast = g_optOn[OPT_Waypoints] && IsMainThread();
  g_wnMembers.clear(); g_wnBuilt = false;
  if (fast) g_wnUnarcNet = s;
  Hook_WnUnarc(s, e, a);
  g_wnUnarcNet = nullptr; g_wnMembers.clear(); g_wnBuilt = false; g_hint[3] = Hint{};
  WpInvalidate();
}

// zCWayNet::HasWaypoint(zCWaypoint*) (0x00703360): linear walk over wplist
int __fastcall WnHas_h(void*, void*, void*);
auto Hook_WnHas = HOOK(0x00703360, &WnHas_h);
int __fastcall WnHas_h(void* net, void* e, void* wp) {
  if (net == g_wnUnarcNet && IsMainThread()) {
    if (!g_wnBuilt) {
      for (void* n = *(void**)((char*)net + 0x30); n; n = *(void**)((char*)n + 8)) g_wnMembers.insert(*(void**)((char*)n + 4));
      g_wnBuilt = true;
    }
    S.wpHasFast++;
    return g_wnMembers.count(wp) ? 1 : 0;
  }
  return Hook_WnHas(net, e, wp);
}

// zCListSort<zCWaypoint>::InsertSort (0x0070A400)
void __fastcall WpInsSort_h(void*, void*, void*);
auto Hook_WpInsSort = HOOK(0x0070A400, &WpInsSort_h);
void __fastcall WpInsSort_h(void* l, void* e, void* wp) {
  // No insertion hint here: the comparator SortWpList has side effects (it copies wp->wpvob's name into
  // the compared waypoint, and wpvob may still be stale during Unarchive), so only the original sequence
  // of comparisons is safe. We only keep the member set current.
  Hook_WpInsSort(l, e, wp);
  if (g_wnBuilt && g_wnUnarcNet && IsMainThread() && l == (char*)g_wnUnarcNet + 0x28) g_wnMembers.insert(wp);
}
void __fastcall WnUnarcOld_h(void*, void*, void*);
auto Hook_WnUnarcOld = HOOK(0x00707A30, &WnUnarcOld_h);
void __fastcall WnUnarcOld_h(void* s, void* e, void* a) { WpInvalidate(); Hook_WnUnarcOld(s, e, a); WpInvalidate(); }
void __fastcall WnDtor_h(void*, void*);
auto Hook_WnDtor = HOOK(0x007031A0, &WnDtor_h);
void __fastcall WnDtor_h(void* s, void* e) { WpInvalidate(); Hook_WnDtor(s, e); }

// =====================================================================================================
// FastAniInit - oCAniCtrl_Human::InitAllAnis (0x0061D5B0, thiscall, no args)
// Pure function of the model's prototype list (it only reads the model and writes ani ids / pointers
// into the controller). Memoized per prototype list, but only after the memo has been verified
// byte-for-byte against the original 3 times in a row; afterwards re-verified every 32nd call. Any
// mismatch disables the memo for that model for good. Only active while loading.
// =====================================================================================================
static const size_t kAniCtrlSize = 0x11F8;
struct AniMemo {
  int state = 0;           // 0 = learning, 1 = trusted, -1 = disabled
  int good = 0, calls = 0;
  std::vector<uint8_t> mask, after;
  std::vector<std::pair<uint16_t, uint16_t>> runs;   // contiguous masked ranges, for a fast replay
  void BuildRuns() {
    runs.clear();
    for (size_t i = 0; i < mask.size();) {
      if (!mask[i]) { i++; continue; }
      size_t j = i; while (j < mask.size() && mask[j]) j++;
      runs.push_back({ (uint16_t)i, (uint16_t)(j - i) });
      i = j;
    }
  }
};
struct VecHash { size_t operator()(const std::vector<void*>& v) const { size_t h = 0; for (void* p : v) h = h * 1000003u ^ (size_t)p; return h; } };
static std::unordered_map<std::vector<void*>, AniMemo, VecHash> g_ani;

void Opt_PrototypeDestroyed() { g_ani.clear(); }

void __fastcall InitAllAnis_h(void*, void*);
auto Hook_InitAllAnis = HOOK(0x0061D5B0, &InitAllAnis_h);
void __fastcall InitAllAnis_h(void* self, void* edx) {
  if (!InSession() || !g_optOn[OPT_AniInit] || !IsMainThread()) { Hook_InitAllAnis(self, edx); return; }
  char* model = *(char**)((char*)self + 0x68);
  if (!model) { Hook_InitAllAnis(self, edx); return; }
  void** protos = *(void***)(model + 0x58);
  int num = *(int*)(model + 0x60);
  if (num <= 0 || num > 64 || !protos) { Hook_InitAllAnis(self, edx); return; }
  std::vector<void*> key(protos, protos + num);
  AniMemo& m = g_ani[key];
  m.calls++;

  if (m.state == 1 && (m.calls % 32) != 0) {               // trusted: replay
    uint8_t* o = (uint8_t*)self;
    for (auto& r : m.runs) memcpy(o + r.first, m.after.data() + r.first, r.second);
    S.fastAniHit++;
    return;
  }
  if (m.state == -1) { Hook_InitAllAnis(self, edx); return; }

  // learning / verification run
  std::vector<uint8_t> pre((uint8_t*)self, (uint8_t*)self + kAniCtrlSize);
  Hook_InitAllAnis(self, edx);
  const uint8_t* post = (const uint8_t*)self;
  S.fastAniCheck++;
  if (m.mask.empty()) {
    m.mask.assign(kAniCtrlSize, 0);
    for (size_t i = 0; i < kAniCtrlSize; i++) m.mask[i] = pre[i] != post[i];
    m.after.assign(post, post + kAniCtrlSize);
    m.good = 0;
    return;
  }
  bool ok = true, grew = false;
  for (size_t i = 0; i < kAniCtrlSize; i++) {
    uint8_t predicted = m.mask[i] ? m.after[i] : pre[i];
    if (predicted == post[i]) continue;
    if (!m.mask[i] && pre[i] != post[i] && post[i] == m.after[i]) { m.mask[i] = 1; grew = true; continue; }  // same value, just unseen before
    ok = false; break;
  }
  if (!ok) { m.state = -1; S.fastAniBad++; return; }
  if (grew) { m.good = 0; if (m.state == 1) m.state = 0; return; }
  if (m.state == 0 && ++m.good >= 3) { m.state = 1; m.BuildRuns(); }
}

// =====================================================================================================
// FastWorldLists - oCWorld::InsertInLists (0x006D7670, __fastcall: ecx = world, edx = vob)
// Original: for oCVob / oCNpc / oCItem it walks voblist / voblist_npcs / voblist_items to check whether
// the vob is already there and only then InsertSort()s it (+AddRef). We keep a member set per list
// (built once per load, kept current through the InsertSort and RemoveFromLists hooks). A "not a member"
// answer skips the walk; a "member" answer still uses the original walk.
// =====================================================================================================
static const uintptr_t CD_oCVob = 0x008DC418, CD_oCNpc = 0x008DB408, CD_oCItem = 0x008DAA80;
struct ListSet { void* list = nullptr; bool built = false; std::unordered_set<void*> s; };
static ListSet g_ls[3];  // 0 = voblist, 1 = npcs, 2 = items

static void* ClassDefOf(void* obj) { return ((void* (__thiscall*)(void*))(*(void***)obj)[0])(obj); }
static bool Derives(void* cd, uintptr_t base) { for (int i = 0; cd && i < 32; i++, cd = *(void**)((char*)cd + 0x3C)) if ((uintptr_t)cd == base) return true; return false; }
static bool WalkHas(void* list, void* vob) { for (void* n = *(void**)((char*)list + 8); n; n = *(void**)((char*)n + 8)) if (*(void**)((char*)n + 4) == vob) return true; return false; }

static ListSet& SetFor(void* list, int idx) {
  ListSet& ls = g_ls[idx];
  if (!ls.built || ls.list != list) {
    ls.s.clear(); ls.list = list; ls.built = true;
    for (void* n = *(void**)((char*)list + 8); n; n = *(void**)((char*)n + 8)) ls.s.insert(*(void**)((char*)n + 4));
  }
  return ls;
}
static void SetsReset() { for (auto& ls : g_ls) { ls.built = false; ls.s.clear(); ls.list = nullptr; } }


void __fastcall InsSortVob_h(void*, void*, void*);
auto Hook_InsSortVob = HOOK(0x006D8260, &InsSortVob_h);
static bool HintsActive() { return InLoadSession() && g_optOn[OPT_WorldLists] && IsMainThread(); }
void __fastcall InsSortVob_h(void* l, void* e, void* v) { if (HintsActive()) HintedInsert((InsSortFn)(void*)Hook_InsSortVob, g_hint[0], l, v); else Hook_InsSortVob(l, e, v); if (IsMainThread() && g_ls[0].built && g_ls[0].list == l) g_ls[0].s.insert(v); }
void __fastcall InsSortNpc_h(void*, void*, void*);
auto Hook_InsSortNpc = HOOK(0x006D82F0, &InsSortNpc_h);
void __fastcall InsSortNpc_h(void* l, void* e, void* v) { if (HintsActive()) HintedInsert((InsSortFn)(void*)Hook_InsSortNpc, g_hint[1], l, v); else Hook_InsSortNpc(l, e, v); if (IsMainThread() && g_ls[1].built && g_ls[1].list == l) g_ls[1].s.insert(v); }
void __fastcall InsSortItem_h(void*, void*, void*);
auto Hook_InsSortItem = HOOK(0x006D8140, &InsSortItem_h);
void __fastcall InsSortItem_h(void* l, void* e, void* v) { if (HintsActive()) HintedInsert((InsSortFn)(void*)Hook_InsSortItem, g_hint[2], l, v); else Hook_InsSortItem(l, e, v); if (IsMainThread() && g_ls[2].built && g_ls[2].list == l) g_ls[2].s.insert(v); }

void __fastcall RemoveFromLists_h(void*, void*);
auto Hook_RemoveFromLists = HOOK(0x006D7750, &RemoveFromLists_h);
void __fastcall RemoveFromLists_h(void* w, void* vob) {
  // RemoveFromLists = AddRef, unlink from the lists that contain the vob, Release. For a vob in none of
  // the lists that is a no-op; the sets are exact while loading (all inserts go through the hooked
  // InsertSort, bulk removals reset them), so the three list walks can be skipped.
  if (vob && InLoadSession() && g_optOn[OPT_WorldLists] && IsMainThread() && ((zCObject*)vob)->refCtr > 0) {
    ListSet& a = SetFor(*(void**)((char*)w + 0x6268), 0);
    ListSet& b = SetFor(*(void**)((char*)w + 0x626C), 1);
    ListSet& c = SetFor(*(void**)((char*)w + 0x6270), 2);
    if (!a.s.count(vob) && !b.s.count(vob) && !c.s.count(vob)) { S.removeSkip++; return; }
  }
  if (IsMainThread()) HintForget(vob);
  Hook_RemoveFromLists(w, vob);
  if (IsMainThread()) for (auto& ls : g_ls) if (ls.built) ls.s.erase(vob);
}

// bulk removals (not through RemoveFromLists): drop sets and hints
void __fastcall OwDisposeVobs_h(void*, void*);
auto Hook_OwDisposeVobs = HOOK(0x006D7990, &OwDisposeVobs_h);
void __fastcall OwDisposeVobs_h(void* w, void* e) { SetsReset(); HintsReset(); Hook_OwDisposeVobs(w, e); SetsReset(); HintsReset(); }
void __fastcall OwDisposeWorld_h(void*, void*);
auto Hook_OwDisposeWorld = HOOK(0x006D7B10, &OwDisposeWorld_h);
void __fastcall OwDisposeWorld_h(void* w, void* e) { SetsReset(); HintsReset(); Hook_OwDisposeWorld(w, e); SetsReset(); HintsReset(); }
void __fastcall OwDtor_h(void*, void*);
auto Hook_OwDtor = HOOK(0x006D5F30, &OwDtor_h);
void __fastcall OwDtor_h(void* w, void* e) { SetsReset(); HintsReset(); Hook_OwDtor(w, e); }

static bool MemberFast(void* list, int idx, void* vob) {
  ListSet& ls = SetFor(list, idx);
  if (!ls.s.count(vob)) { S.listSkip++; return false; }
  return WalkHas(list, vob);   // set says member: trust only the real list
}

void __fastcall InsertInLists_h(void*, void*);
auto Hook_InsertInLists = HOOK(0x006D7670, &InsertInLists_h);
void __fastcall InsertInLists_h(void* w, void* vob) {
  if (!InLoadSession() || !g_optOn[OPT_WorldLists] || !IsMainThread()) { Hook_InsertInLists(w, vob); return; }
  if (!vob) return;
  void* cd = ClassDefOf(vob);
  if (!cd || !Derives(cd, CD_oCVob)) return;
  void* voblist = *(void**)((char*)w + 0x6268);
  if (!MemberFast(voblist, 0, vob)) { ((InsSortFn)0x006D8260)(voblist, vob); ((zCObject*)vob)->refCtr++; }
  if (Derives(ClassDefOf(vob), CD_oCNpc)) {
    void* npcs = *(void**)((char*)w + 0x626C);
    if (!MemberFast(npcs, 1, vob)) { ((InsSortFn)0x006D82F0)(npcs, vob); ((zCObject*)vob)->refCtr++; return; }
  }
  if (Derives(ClassDefOf(vob), CD_oCItem)) {
    void* items = *(void**)((char*)w + 0x6270);
    if (!MemberFast(items, 2, vob)) { ((InsSortFn)0x006D8140)(items, vob); ((zCObject*)vob)->refCtr++; }
  }
}

// =====================================================================================================
// FastFileRead - read-ahead for zFILE_VDFS (VDFS mode only)
// zFILE_VDFS::Read(void*,long) locks a global critical section and calls vdf_fread for every call; the
// ASCII archiver reads text one byte at a time (17M calls for WORLD.ZEN). Small reads are served from a
// 64 KB buffer. Semantics kept: return value, vdfResult (+0x2A00), vdfEOF (+0x2A04, set when a read
// returns less than requested), Pos(). Any other operation first moves the real file position back to
// the logical one.
// =====================================================================================================
struct RBuf {
  bool active = false, lastFillShort = false;
  int seqSmall = 0;            // consecutive small sequential reads seen while not buffering
  long fillSize = 0;           // grows 4 KB -> 64 KB while the data keeps being consumed sequentially
  long base = 0;               // logical file position of data[0]
  long len = 0, off = 0;
  std::vector<char> data;
};
static SRWLOCK g_rbLock = SRWLOCK_INIT;
static std::unordered_map<void*, RBuf*> g_rb;
static thread_local void* t_rbFile = nullptr;
static thread_local RBuf* t_rbBuf = nullptr;
static const long kRBuf = 64 * 1024, kRSmall = 4096, kRFirst = 4096, kRStartAfter = 4;

static RBuf* RB(void* f, bool create) {
  if (t_rbFile == f) return t_rbBuf;
  AcquireSRWLockExclusive(&g_rbLock);
  RBuf* b = nullptr;
  auto it = g_rb.find(f);
  if (it != g_rb.end()) b = it->second;
  else if (create) { b = new RBuf(); g_rb[f] = b; }
  ReleaseSRWLockExclusive(&g_rbLock);
  if (b) { t_rbFile = f; t_rbBuf = b; }
  return b;
}

long __fastcall VRead_h(void*, void*, void*, long);
auto Hook_VRead = HOOK(0x004468C0, &VRead_h);
int __fastcall VSeek_h(void*, void*, long);
auto Hook_VSeek = HOOK(0x00445400, &VSeek_h);
long __fastcall VPos_h(void*, void*);
auto Hook_VPos = HOOK(0x00445720, &VPos_h);

static void RDrop(RBuf* b) { b->active = false; b->len = b->off = 0; b->lastFillShort = false; b->seqSmall = 0; b->fillSize = kRFirst; }
static void RSync(void* f) {
  RBuf* b = RB(f, false);
  if (!b || !b->active) return;
  if (b->off < b->len) Hook_VSeek(f, nullptr, b->base + b->off);
  RDrop(b);
}
static void RRelease(void* f) {
  RBuf* b = RB(f, false);
  if (!b) return;
  RDrop(b);
  std::vector<char>().swap(b->data);
}

long __fastcall VRead_h(void* f, void* edx, void* dst, long n) {
  bool vdfMode = *(volatile bool*)0x0085F2CC && *(long*)((char*)f + 0x29FC) >= 0;
  if (!vdfMode || n <= 0 || n > kRSmall || !g_optOn[OPT_FileRead]) {
    RSync(f);
    return Hook_VRead(f, edx, dst, n);
  }
  RBuf* b = RB(f, true);
  if (!b->active) {
    // Only start buffering after a few small sequential reads: patterns like "read a header, seek
    // elsewhere" (script symbols at startup) would otherwise read whole blocks for nothing.
    if (++b->seqSmall < kRStartAfter) return Hook_VRead(f, edx, dst, n);
    b->base = Hook_VPos(f, nullptr);
    b->len = b->off = 0; b->lastFillShort = false; b->active = true; b->fillSize = kRFirst;
    if (b->data.size() != (size_t)kRBuf) b->data.resize(kRBuf);
  }
  long done = 0;
  while (done < n) {
    if (b->off >= b->len) {
      if (b->lastFillShort) break;                      // real end of file reached
      if (b->len) b->fillSize = b->fillSize * 2 > kRBuf ? kRBuf : b->fillSize * 2;   // previous block fully used
      b->base += b->len; b->off = 0;
      long r = Hook_VRead(f, edx, b->data.data(), b->fillSize);
      S.readFills++;
      if (r < 0) { RDrop(b); return done ? done : r; }
      b->len = r; b->lastFillShort = r < b->fillSize;
      if (r == 0) break;
    }
    long take = b->len - b->off;
    if (take > n - done) take = n - done;
    memcpy((char*)dst + done, b->data.data() + b->off, take);
    b->off += take; done += take;
  }
  *(long*)((char*)f + 0x2A00) = done;
  *(bool*)((char*)f + 0x2A04) = done < n;
  S.readServed++;
  return done;
}

int __fastcall VSeek_h(void* f, void* edx, long pos) { if (RBuf* b = RB(f, false)) RDrop(b); return Hook_VSeek(f, edx, pos); }
long __fastcall VPos_h(void* f, void* edx) {
  RBuf* b = RB(f, false);
  if (b && b->active) return b->base + b->off;
  return Hook_VPos(f, edx);
}
int __fastcall VReadStr_h(void*, void*, void*);
auto Hook_VReadStr = HOOK(0x00446750, &VReadStr_h);
int __fastcall VReadStr_h(void* f, void* e, void* s) { RSync(f); return Hook_VReadStr(f, e, s); }
int __fastcall VReadChars_h(void*, void*, char*);
auto Hook_VReadChars = HOOK(0x004465A0, &VReadChars_h);
int __fastcall VReadChars_h(void* f, void* e, char* s) { RSync(f); return Hook_VReadChars(f, e, s); }
int __fastcall VReset_h(void*, void*);
auto Hook_VReset = HOOK(0x00445370, &VReset_h);
int __fastcall VReset_h(void* f, void* e) { if (RBuf* b = RB(f, false)) RDrop(b); return Hook_VReset(f, e); }
int __fastcall VOpen1_h(void*, void*, const zSTRING&, bool);
auto Hook_VOpen1 = HOOK(0x00444CA0, &VOpen1_h);
int __fastcall VOpen1_h(void* f, void* e, const zSTRING& p, bool w) { RRelease(f); return Hook_VOpen1(f, e, p, w); }
int __fastcall VOpen2_h(void*, void*, bool);
auto Hook_VOpen2 = HOOK(0x00445090, &VOpen2_h);
int __fastcall VOpen2_h(void* f, void* e, bool w) { RRelease(f); return Hook_VOpen2(f, e, w); }

// =====================================================================================================
// FastFileWrite - write buffer for zFILE_FILE::Write(void*,long) while saving
// Positions: Pos() = position at buffer start + buffered bytes. Every other operation flushes first.
// =====================================================================================================
struct WBuf { bool active = false; long base = 0; std::vector<char> data; size_t len = 0; };
static std::unordered_map<void*, WBuf> g_wb;
static const size_t kWBuf = 64 * 1024, kWSmall = 4096;

long __fastcall FWrite_h(void*, void*, const void*, long);
auto Hook_FWrite = HOOK(0x00440510, &FWrite_h);
long __fastcall FPos_h(void*, void*);
auto Hook_FPos = HOOK(0x00440380, &FPos_h);

static long g_wbCount = 0;   // number of files with buffered data (main thread only)
static bool WFlush(void* f) {
  if (!g_wbCount || !IsMainThread()) return true;
  auto it = g_wb.find(f);
  if (it == g_wb.end() || !it->second.active) return true;
  WBuf& b = it->second;
  b.active = false; g_wbCount--;
  bool ok = true;
  if (b.len) { long w = Hook_FWrite(f, nullptr, b.data.data(), (long)b.len); ok = w == (long)b.len; S.writeFlushes++; }
  b.len = 0;
  return ok;
}
static void WFlushAll() {
  if (g_wb.empty() || !IsMainThread()) return;
  std::vector<void*> fs; for (auto& kv : g_wb) fs.push_back(kv.first);
  for (void* f : fs) WFlush(f);
  g_wb.clear();
}

long __fastcall FWrite_h(void* f, void* edx, const void* src, long n) {
  if (!InSaveSession() || n <= 0 || (size_t)n > kWSmall || !g_optOn[OPT_FileWrite] || !IsMainThread()) {
    WFlush(f);
    return Hook_FWrite(f, edx, src, n);
  }
  WBuf& b = g_wb[f];
  if (!b.active) { b.base = Hook_FPos(f, nullptr); b.len = 0; b.active = true; g_wbCount++; if (b.data.size() != kWBuf) b.data.resize(kWBuf); }
  if (b.len + n > kWBuf && !WFlush(f)) return Hook_FWrite(f, edx, src, n);
  if (!b.active) { b.base = Hook_FPos(f, nullptr); b.len = 0; b.active = true; g_wbCount++; }
  memcpy(b.data.data() + b.len, src, n);
  b.len += n;
  S.writeBuffered++;
  return n;
}
long __fastcall FPos_h(void* f, void* edx) {
  if (!g_wbCount || !IsMainThread()) return Hook_FPos(f, edx);
  auto it = g_wb.find(f);
  if (it != g_wb.end() && it->second.active) return it->second.base + (long)it->second.len;
  return Hook_FPos(f, edx);
}

// every other zFILE_FILE / zFILE_VDFS operation flushes pending writes first
#define FLUSH_HOOK0(addr, NAME, R) \
  R __fastcall NAME##_h(void*, void*); auto Hook_##NAME = HOOK(addr, &NAME##_h); \
  R __fastcall NAME##_h(void* f, void* e) { WFlush(f); return Hook_##NAME(f, e); }
#define FLUSH_HOOK1(addr, NAME, R, T) \
  R __fastcall NAME##_h(void*, void*, T); auto Hook_##NAME = HOOK(addr, &NAME##_h); \
  R __fastcall NAME##_h(void* f, void* e, T a) { WFlush(f); return Hook_##NAME(f, e, a); }
#define FLUSH_HOOK2(addr, NAME, R, T1, T2) \
  R __fastcall NAME##_h(void*, void*, T1, T2); auto Hook_##NAME = HOOK(addr, &NAME##_h); \
  R __fastcall NAME##_h(void* f, void* e, T1 a, T2 b) { WFlush(f); return Hook_##NAME(f, e, a, b); }

FLUSH_HOOK0(0x004400B0, FFlushBuffer, long)
FLUSH_HOOK0(0x00440100, FClose, int)
FLUSH_HOOK0(0x00440340, FReset, int)
FLUSH_HOOK0(0x00440350, FAppend, void)
FLUSH_HOOK0(0x00440370, FEof, bool)
FLUSH_HOOK0(0x0043F990, FSize, long)
FLUSH_HOOK1(0x00440390, FSeek, int, long)
FLUSH_HOOK1(0x004403B0, FWriteCStr, int, const char*)
FLUSH_HOOK1(0x004404F0, FWriteZStr, int, const void*)
FLUSH_HOOK1(0x00440730, FReadChar, int, char*)
FLUSH_HOOK1(0x00440750, FReadCStr, int, char*)
FLUSH_HOOK1(0x00440790, FReadZStr, int, void*)
FLUSH_HOOK2(0x00440980, FReadRaw, long, void*, long)
FLUSH_HOOK1(0x0043F820, FGetStats, int, void*)
int __fastcall VClose_h(void*, void*);
auto Hook_VClose = HOOK(0x00445310, &VClose_h);
int __fastcall VClose_h(void* f, void* e) { WFlush(f); if (IsMainThread()) g_wb.erase(f); RRelease(f); return Hook_VClose(f, e); }
long __fastcall VSize_h(void*, void*);
auto Hook_VSize = HOOK(0x00445380, &VSize_h);
long __fastcall VSize_h(void* f, void* e) { WFlush(f); return Hook_VSize(f, e); }

// =====================================================================================================
// FastStringMap - GETStringMap<unsigned long> used by zCArchiverBinSafe
// The map has a fixed 97 buckets and the bucket numbers are stored in the save file, so its layout
// must not change. We keep a shadow index key -> node next to it. Nodes: +0 next, +8 value,
// +0C GECString* (first dword = char*), +10 data. Lookups compare with _mbscmp (== strcmp, SBCS).
// =====================================================================================================
struct Shadow { void* buckets = nullptr; unsigned count = 0; std::unordered_map<std::string, void*> idx; };
static std::unordered_map<void*, Shadow> g_sm;

static inline const char* NodeKey(void* node) { void* gs = *(void**)((char*)node + 0x0C); return gs ? *(const char**)gs : nullptr; }
static inline unsigned HashBucket(const char* k, unsigned size) { unsigned h = 0; for (; *k; k++) h = h * 33 + (int)(signed char)*k; return size ? h % size : 0; }

static Shadow* ShadowFor(void* map) {
  void** buckets = *(void***)((char*)map + 4);
  unsigned size = *(unsigned*)((char*)map + 8), count = *(unsigned*)((char*)map + 0x0C);
  Shadow& sh = g_sm[map];
  if (sh.buckets == buckets && sh.count == count) return &sh;
  sh.idx.clear(); sh.buckets = buckets; sh.count = count;
  S.mapRebuild++;
  if (buckets)
    for (unsigned b = 0; b < size; b++)
      for (void* n = buckets[b]; n; n = *(void**)n) {
        const char* k = NodeKey(n);
        if (k && HashBucket(k, size) == b) sh.idx.emplace(k, n);   // first in chain wins, like the original walk
      }
  return &sh;
}
static void ShadowAdd(void* map, void* node, unsigned bucket) {
  auto it = g_sm.find(map);
  if (it == g_sm.end()) return;
  Shadow& sh = it->second;
  unsigned size = *(unsigned*)((char*)map + 8);
  const char* k = NodeKey(node);
  if (!k || sh.buckets != *(void**)((char*)map + 4) || sh.count + 1 != *(unsigned*)((char*)map + 0x0C)) { g_sm.erase(it); return; }
  sh.count++;
  if (HashBucket(k, size) == bucket) sh.idx[k] = node;                // new node is the chain head
}

void* __fastcall SmGetElement_h(void*, void*, const char*, unsigned*, unsigned);
auto Hook_SmGetElement = HOOK(0x00511690, &SmGetElement_h);
void* __fastcall SmGetElement_h(void* map, void* edx, const char* key, unsigned* bucket, unsigned value) {
  if (value != 0xFFFFFFFFu || !key || !g_optOn[OPT_StringMap] || !IsMainThread()) return Hook_SmGetElement(map, edx, key, bucket, value);
  unsigned size = *(unsigned*)((char*)map + 8);
  *bucket = HashBucket(key, size);
  if (!*(void**)((char*)map + 4)) return nullptr;
  Shadow* sh = ShadowFor(map);
  S.mapFast++;
  auto it = sh->idx.find(key);
  return it == sh->idx.end() ? nullptr : it->second;
}

unsigned __fastcall SmLookup_h(void*, void*, const char*, unsigned**, unsigned);
auto Hook_SmLookup = HOOK(0x00511460, &SmLookup_h);
unsigned __fastcall SmLookup_h(void* map, void* edx, const char* key, unsigned** data, unsigned value) {
  if (value != 0xFFFFFFFFu || !key || !g_optOn[OPT_StringMap] || !IsMainThread()) return Hook_SmLookup(map, edx, key, data, value);
  if (!*(void**)((char*)map + 4)) return 0xFFFFFFFFu;
  Shadow* sh = ShadowFor(map);
  S.mapFast++;
  auto it = sh->idx.find(key);
  if (it == sh->idx.end()) return 0xFFFFFFFFu;
  char* node = (char*)it->second;
  *data = *(unsigned**)(node + 0x10);
  return *(unsigned*)(node + 8);
}

unsigned __fastcall SmSetAt_h(void*, void*, const char*, unsigned*);
auto Hook_SmSetAt = HOOK(0x00510FE0, &SmSetAt_h);
unsigned __fastcall SmSetAt_h(void* map, void* edx, const char* key, unsigned* data) {
  unsigned before = *(unsigned*)((char*)map + 0x0C);
  unsigned r = Hook_SmSetAt(map, edx, key, data);
  if (!IsMainThread()) return r;   // the shadow notices the changed count and rebuilds
  if (*(unsigned*)((char*)map + 0x0C) == before + 1 && key) {
    unsigned b = HashBucket(key, *(unsigned*)((char*)map + 8));
    void** buckets = *(void***)((char*)map + 4);
    if (buckets) ShadowAdd(map, buckets[b], b);
  }
  return r;
}

unsigned __fastcall SmSetAtForce_h(void*, void*, const char*, unsigned*, unsigned, unsigned);
auto Hook_SmSetAtForce = HOOK(0x005111D0, &SmSetAtForce_h);
unsigned __fastcall SmSetAtForce_h(void* map, void* edx, const char* key, unsigned* data, unsigned bucket, unsigned value) {
  unsigned before = *(unsigned*)((char*)map + 0x0C);
  unsigned r = Hook_SmSetAtForce(map, edx, key, data, bucket, value);
  if (!IsMainThread()) return r;
  void** buckets = *(void***)((char*)map + 4);
  if (*(unsigned*)((char*)map + 0x0C) == before + 1 && buckets && bucket < *(unsigned*)((char*)map + 8))
    ShadowAdd(map, buckets[bucket], bucket);
  return r;
}

void __fastcall SmRemoveAll_h(void*, void*, bool);
auto Hook_SmRemoveAll = HOOK(0x00511360, &SmRemoveAll_h);
void __fastcall SmRemoveAll_h(void* map, void* edx, bool b) { bool m = IsMainThread(); if (m) g_sm.erase(map); Hook_SmRemoveAll(map, edx, b); if (m) g_sm.erase(map); }

// zCArchiverBinSafe::Close frees its string map: drop the shadow so a new map at the same address
// can never be confused with it.
void __fastcall BsClose_h(void*, void*);
auto Hook_BsClose = HOOK(0x0050C320, &BsClose_h);
void __fastcall BsClose_h(void* arc, void* edx) {
  void* map = *(void**)((char*)arc + 0x98);
  Hook_BsClose(arc, edx);
  if (map && IsMainThread()) g_sm.erase(map);
}

// =====================================================================================================
void Opt_SessionBegin() {
  SetsReset(); HintsReset();
  S = Stats{};
}

void Opt_SessionEnd() {
  WFlushAll();
  SetsReset(); HintsReset();
  g_sm.clear();
}

void Opt_WriteStats(FILE* f) {
  fprintf(f, "  -- lists: insert hint hit=%ld miss=%ld  removals skipped=%ld  HasWaypoint fast=%ld\n", S.hintHit, S.hintMiss, S.removeSkip, S.wpHasFast);
  fprintf(f, "  -- optimizations: waypoint=%ld  ani memo hit=%ld check=%ld bad=%ld (models trusted=%d)  list scans skipped=%ld\n"
             "                    read served=%ld fills=%ld  stringmap fast=%ld rebuilds=%ld  writes buffered=%ld flushes=%ld --\n",
          S.fastWp, S.fastAniHit, S.fastAniCheck, S.fastAniBad,
          (int)std::count_if(g_ani.begin(), g_ani.end(), [](auto& kv) { return kv.second.state == 1; }),
          S.listSkip, S.readServed, S.readFills, S.mapFast, S.mapRebuild, S.writeBuffered, S.writeFlushes);
}

} // namespace Gothic_I_Classic
