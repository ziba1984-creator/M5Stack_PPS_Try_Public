#pragma once
// Test-only fault injection, compiled in only with -DPPS_FAULT_INJECTION.
// Serial commands (single characters):
//   L = make every PPS I2C access fail (simulated link loss)   R = restore
//   N = post "output ON"   O = post "output OFF"   F = systemFatal from the UI task
//   V / v = setpoint +0.1 V / -0.1 V (relative)
// The test build boots with PPS I2C failing (boot-time "output unknown" path); send R.
// Normal builds get no-op inline stubs.

#ifdef PPS_FAULT_INJECTION
bool faultPpsI2c();
bool faultFatalRequested();
bool faultTaskStart();
#else
inline bool faultPpsI2c() { return false; }
inline bool faultFatalRequested() { return false; }
inline bool faultTaskStart() { return true; }
#endif
