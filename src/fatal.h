#pragma once

// Unrecoverable startup failure: forces PPS output OFF, shows the reason on the
// display and the serial port, and never returns.
[[noreturn]] void systemFatal(const char* reason);
