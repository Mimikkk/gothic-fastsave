#pragma once
namespace Sampler {
  void Init(const char* outDir);   // start the (idle) sampler thread
  void BeginAsync();               // request sampling of the calling thread (safe in DllMain)
  void Begin();                    // same, waits until sampling has started
  void End(const char* label);     // stop and write <outDir>\NNN_<label>.txt
  bool Active();
}
