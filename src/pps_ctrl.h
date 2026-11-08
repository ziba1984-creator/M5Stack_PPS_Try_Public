#pragma once

bool ppsTaskStart();

// Stops the PPS task from applying any further command and forces output OFF
// (with retries and read-back). Safe to call even if the task or the RTOS
// objects were never created. Returns true if OFF was confirmed.
bool ppsShutdown();
