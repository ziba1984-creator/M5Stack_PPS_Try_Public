#include "fatal.h"
#include <M5Unified.h>
#include "pps_ctrl.h"

void systemFatal(const char* reason) {
    const bool off = ppsShutdown();

    M5.Display.fillScreen(RED);
    M5.Display.setTextColor(WHITE, RED);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("FATAL", 8, 8, &fonts::Font4);
    M5.Display.drawString(reason, 8, 48, &fonts::Font2);
    M5.Display.drawString(off ? "PPS output: OFF (confirmed)" : "PPS output: OFF NOT CONFIRMED",
                          8, 80, &fonts::Font2);
    M5.Display.drawString("Restart required", 8, 112, &fonts::Font2);

    for (;;) {
        Serial.printf("FATAL: %s (PPS off %s)\n", reason, off ? "confirmed" : "NOT confirmed");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
