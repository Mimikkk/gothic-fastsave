#pragma once
#include <Windows.h>
#include <string>
#include <algorithm>

// A development build installed as System\Autorun\zFastSaveLoad_dev.dll takes precedence: any other copy
// of the plugin (e.g. the one the Workshop launcher copies in) stays inactive - no hooks, no threads -
// so both can sit in Autorun without being loaded twice.
inline bool ZFSL_Active() {
  static int state = -1;
  if (state < 0) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ZFSL_Active, &self);
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(self, buf, MAX_PATH);
    std::string path = buf, lower = buf;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    bool isDev = lower.find("_dev.dll") != std::string::npos;
    std::string dir = path.substr(0, path.find_last_of("\\/") + 1);
    bool devPresent = GetFileAttributesA((dir + "zFastSaveLoad_dev.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    state = (isDev || !devPresent) ? 1 : 0;
  }
  return state == 1;
}
