// Sampling profiler for the game's main thread.
// A background thread suspends the main thread ~1000x/s, copies its stack and records every dword that
// looks like a return address (points into executable code and is preceded by a CALL instruction).
// Symbolization and filtering of stale frames happen offline (tools/flame.py) using gothic-api Names.txt.
// Nothing is allocated while the main thread is suspended (it may hold the heap lock).

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include "Sampler.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

namespace Sampler {

struct CodeRange { uintptr_t lo, hi; };
static std::vector<CodeRange> g_code;
static HANDLE g_thread = nullptr, g_mainHandle = nullptr, g_wake = nullptr;
static DWORD g_mainTid = 0;
static volatile LONG g_active = 0;     // 1 = sampling requested
static volatile LONG g_busy = 0;       // sampler is inside a sample
static uintptr_t g_stackBase = 0;
static std::string g_outDir;
static int g_seq = 0;

static const size_t kStackCopy = 256 * 1024;
static uint8_t* g_stackBuf = nullptr;

// stack key: list of (ret, target) pairs; target = 0 for indirect calls. Leaf (EIP) first.
using Key = std::vector<uint64_t>;
static std::map<Key, uint32_t> g_samples;
static uint32_t g_total = 0, g_failed = 0;

struct ThreadTime { DWORD tid; uint64_t cpu100ns; };
static std::vector<ThreadTime> g_t0;
static long long g_wall0 = 0;
static IO_COUNTERS g_io0{};

static uint64_t FtToU(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static std::vector<ThreadTime> SnapThreads() {
  std::vector<ThreadTime> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te{ sizeof(te) };
  DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c, e, k, u;
    if (GetThreadTimes(h, &c, &e, &k, &u)) out.push_back({ te.th32ThreadID, FtToU(k) + FtToU(u) });
    CloseHandle(h);
  }
  CloseHandle(snap);
  return out;
}

static void RefreshCodeRanges() {
  g_code.clear();
  HMODULE mods[1024]; DWORD need = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
  for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
    auto base = (uint8_t*)mods[i];
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++)
      if (sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)
        g_code.push_back({ (uintptr_t)base + sec[s].VirtualAddress, (uintptr_t)base + sec[s].VirtualAddress + sec[s].Misc.VirtualSize });
  }
}

static inline bool InCode(uintptr_t a) {
  for (auto& r : g_code) if (a >= r.lo + 8 && a < r.hi) return true;
  return false;
}

// Is `ret` preceded by a CALL? Returns 1 = direct (E8, target set), 2 = indirect, 0 = no.
static int CallBefore(uintptr_t ret, uintptr_t& target) {
  const uint8_t* p = (const uint8_t*)ret;
  if (p[-5] == 0xE8) { target = ret + *(const int32_t*)(p - 4); return 1; }
  if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && (p[-1] & 0xC0) == 0xC0) return 2;               // call reg
  if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && (p[-1] & 0xC0) == 0x00 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5) return 2; // call [reg]
  if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return 2;               // call [reg+d8]
  if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC7) == 0x04) return 2;               // call [sib]
  if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10 && (p[-3] & 0xC7) == 0x44) return 2;               // call [sib+d8]
  if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10 && ((p[-5] & 0xC0) == 0x80 || (p[-5] & 0xC7) == 0x05)) return 2; // [reg+d32], [d32]
  if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10 && (p[-6] & 0xC7) == 0x84) return 2;               // [sib+d32]
  return 0;
}

static void TakeSample() {
  CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (SuspendThread(g_mainHandle) == (DWORD)-1) { g_failed++; return; }
  size_t n = 0; uintptr_t esp = 0, eip = 0;
  if (GetThreadContext(g_mainHandle, &ctx)) {
    esp = ctx.Esp; eip = ctx.Eip;
    if (esp && g_stackBase > esp) {
      n = g_stackBase - esp; if (n > kStackCopy) n = kStackCopy;
      memcpy(g_stackBuf, (void*)esp, n);
    }
  }
  ResumeThread(g_mainHandle);
  if (!eip) { g_failed++; return; }

  Key key; key.reserve(128);
  key.push_back((uint64_t)eip);
  for (size_t off = 0; off + 4 <= n && key.size() < 400; off += 4) {
    uintptr_t v = *(uint32_t*)(g_stackBuf + off);
    if (!InCode(v)) continue;
    uintptr_t t = 0;
    int kind = CallBefore(v, t);
    if (!kind) continue;
    key.push_back((uint64_t)v | ((uint64_t)(kind == 1 ? t : 0) << 32));
  }
  g_samples[key]++;
  g_total++;
}

static volatile LONG g_pending = 0;
static DWORD g_pendTid = 0;
static uintptr_t g_pendStack = 0;

static void Setup() {
  if (g_pendTid != g_mainTid || !g_mainHandle) {
    if (g_mainHandle) CloseHandle(g_mainHandle);
    g_mainHandle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_pendTid);
    g_mainTid = g_pendTid;
  }
  g_stackBase = g_pendStack;
  RefreshCodeRanges();
  g_samples.clear(); g_total = 0; g_failed = 0;
  g_t0 = SnapThreads();
  GetProcessIoCounters(GetCurrentProcess(), &g_io0);
  LARGE_INTEGER c; QueryPerformanceCounter(&c); g_wall0 = c.QuadPart;
  timeBeginPeriod(1);
}

static DWORD WINAPI ThreadProc(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  for (;;) {
    if (g_pending) { Setup(); InterlockedExchange(&g_active, 1); InterlockedExchange(&g_pending, 0); }
    if (!g_active) { WaitForSingleObject(g_wake, INFINITE); continue; }
    InterlockedExchange(&g_busy, 1);
    if (g_active && g_mainHandle) {
      __try { TakeSample(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_failed++; ResumeThread(g_mainHandle); }
      if ((g_total & 255) == 0) RefreshCodeRanges();  // modules loaded later (plugins during startup)
    }
    InterlockedExchange(&g_busy, 0);
    Sleep(1);
  }
  return 0;
}

void Init(const char* outDir) {
  if (g_thread) return;
  g_outDir = outDir;
  CreateDirectoryA(outDir, nullptr);
  g_stackBuf = (uint8_t*)VirtualAlloc(nullptr, kStackCopy, MEM_COMMIT, PAGE_READWRITE);
  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
}

// Safe inside DllMain: all setup runs on the sampler thread.
void BeginAsync() {
  if (!g_thread || g_active || g_pending) return;
  g_pendTid = GetCurrentThreadId();
  g_pendStack = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
  InterlockedExchange(&g_pending, 1);
  SetEvent(g_wake);
}

void Begin() {
  BeginAsync();
  for (int i = 0; i < 200 && g_pending; i++) Sleep(1);
}
bool Active() { return g_active != 0; }

void End(const char* label) {
  for (int i = 0; i < 200 && g_pending; i++) Sleep(1);
  if (!g_active) return;
  InterlockedExchange(&g_active, 0);
  while (g_busy) Sleep(0);
  timeEndPeriod(1);
  LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
  double wallMs = (c.QuadPart - g_wall0) * 1000.0 / f.QuadPart;
  auto t1 = SnapThreads();
  IO_COUNTERS io1{}; GetProcessIoCounters(GetCurrentProcess(), &io1);

  char path[MAX_PATH];
  sprintf(path, "%s\\%03d_%s.txt", g_outDir.c_str(), ++g_seq, label);
  for (char* p = path + g_outDir.size() + 1; *p; p++) if (strchr(" :[]=*?\"<>|/\\", *p)) *p = '_';
  FILE* out = fopen(path, "w");
  if (!out) return;
  fprintf(out, "# label %s\n# wall_ms %.1f\n# samples %u failed %u\n", label, wallMs, g_total, g_failed);
  fprintf(out, "# io read_ops %llu read_mb %.2f write_ops %llu write_mb %.2f\n",
          io1.ReadOperationCount - g_io0.ReadOperationCount, (io1.ReadTransferCount - g_io0.ReadTransferCount) / 1048576.0,
          io1.WriteOperationCount - g_io0.WriteOperationCount, (io1.WriteTransferCount - g_io0.WriteTransferCount) / 1048576.0);
  for (auto& t : t1) {
    uint64_t before = 0;
    for (auto& s : g_t0) if (s.tid == t.tid) before = s.cpu100ns;
    double ms = (t.cpu100ns - before) / 10000.0;
    if (ms >= 1.0) fprintf(out, "# thread %lu%s cpu_ms %.1f\n", t.tid, t.tid == g_mainTid ? " (main)" : "", ms);
  }
  HMODULE mods[1024]; DWORD need = 0;
  if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
      MODULEINFO mi{}; char name[MAX_PATH] = {};
      GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
      GetModuleFileNameA(mods[i], name, MAX_PATH);
      fprintf(out, "module %08X %08X %s\n", (unsigned)(uintptr_t)mi.lpBaseOfDll, (unsigned)mi.SizeOfImage, name);
    }
  }
  for (auto& kv : g_samples) {
    fprintf(out, "%u", kv.second);
    for (uint64_t a : kv.first) {
      uint32_t r = (uint32_t)a, t = (uint32_t)(a >> 32);
      if (t) fprintf(out, " %X>%X", r, t); else fprintf(out, " %X", r);
    }
    fprintf(out, "\n");
  }
  fclose(out);
  g_samples.clear();
}

} // namespace Sampler
