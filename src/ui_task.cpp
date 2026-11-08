#include "ui_task.h"
#include <M5Unified.h>
#include "app_state.h"
#include "config.h"
#include "fatal.h"
#include "fault_inject.h"

namespace {

struct Button {
    int x, y, w, h;
};

// 320x240 layout
const Button BTN_V_DN  = {8,   150, 68, 38};
const Button BTN_V_UP  = {84,  150, 68, 38};
const Button BTN_I_DN  = {168, 150, 68, 38};
const Button BTN_I_UP  = {244, 150, 68, 38};
const Button BTN_STEP  = {8,   196, 100, 38};
const Button BTN_POWER = {116, 196, 196, 38};

M5Canvas canvas(&M5.Display);
TaskHandle_t s_uiTask;
volatile int stepIdx = 1;  // written by the touch task, read by the UI task
const float V_STEPS[] = {0.1f, 0.5f, 1.0f};
const float I_STEPS[] = {0.01f, 0.1f, 0.5f};

bool hit(const Button& b, int px, int py) {
    return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}

void drawButton(const Button& b, const char* label, uint16_t color) {
    canvas.fillRoundRect(b.x, b.y, b.w, b.h, 6, color);
    canvas.setTextColor(WHITE, color);
    canvas.setTextDatum(middle_center);
    canvas.setTextSize(1);
    canvas.drawString(label, b.x + b.w / 2, b.y + b.h / 2, &fonts::Font4);
}

void drawReading(const char* value, const char* unit, int y) {
    canvas.setTextDatum(top_right);
    canvas.drawString(value, 200, y, &fonts::Font6);
    canvas.setTextDatum(top_left);
    canvas.drawString(unit, 208, y + 18, &fonts::Font4);
}

void draw(const PpsStatus& p, const BatteryStatus& b, const NetStatus& n) {
    canvas.fillSprite(BLACK);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(WHITE, BLACK);

    // status bar
    char buf[48];
    if (b.level >= 0) {
        snprintf(buf, sizeof(buf), "BAT %d%%%s", b.level, b.charging ? "+" : "");
    } else {
        snprintf(buf, sizeof(buf), "BAT --");
    }
    canvas.drawString(buf, 4, 4, &fonts::Font2);
    snprintf(buf, sizeof(buf), "AP %s (%d)", n.apIp, n.clients);
    canvas.setTextDatum(top_right);
    canvas.drawString(buf, 316, 4, &fonts::Font2);
    canvas.drawFastHLine(0, 24, 320, DARKGREY);

    if (!p.connected) {
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(RED, BLACK);
        canvas.drawString(p.halted ? "PPS halted" : "PPS not found", 160, 100, &fonts::Font4);
        if (p.outputUnknown) {
            canvas.setTextColor(ORANGE, BLACK);
            canvas.drawString("Output state UNKNOWN", 160, 135, &fonts::Font4);
        }
        canvas.pushSprite(0, 0);
        return;
    }

    // readings: Font6 only has digits and ".:-apm", so units use Font4
    canvas.setTextColor(GREEN, BLACK);
    snprintf(buf, sizeof(buf), "%.2f", p.vout);
    drawReading(buf, "V", 30);
    snprintf(buf, sizeof(buf), "%.3f", p.iout);
    drawReading(buf, "A", 78);

    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setTextDatum(top_left);
    snprintf(buf, sizeof(buf), "Set %.2fV %.2fA  %s", p.vset, p.iset, p.mode <= 1 ? "CV" : "CC");
    canvas.drawString(buf, 8, 126, &fonts::Font2);
    const ThermalStatus th = stateGetThermal();
    if (th.fanPresent) {
        snprintf(buf, sizeof(buf), "Vin %.1fV %.0fC F%u%%", p.vin, p.temp, th.fanDuty);
    } else {
        snprintf(buf, sizeof(buf), "Vin %.1fV %.0fC F--", p.vin, p.temp);
    }
    canvas.setTextDatum(top_right);
    canvas.drawString(buf, 316, 126, &fonts::Font2);

    // controls
    drawButton(BTN_V_DN, "V-", NAVY);
    drawButton(BTN_V_UP, "V+", NAVY);
    drawButton(BTN_I_DN, "I-", DARKCYAN);
    drawButton(BTN_I_UP, "I+", DARKCYAN);
    const int step = stepIdx;
    snprintf(buf, sizeof(buf), "%.2g/%.2g", V_STEPS[step], I_STEPS[step]);
    drawButton(BTN_STEP, buf, DARKGREY);
    if (!p.valid) {
        // Output state unknown: the button only forces OFF (see handleTouch).
        drawButton(BTN_POWER, "FORCE OFF", ORANGE);
        canvas.setTextDatum(top_center);
        canvas.setTextColor(ORANGE, BLACK);
        canvas.drawString("STALE", 160, 4, &fonts::Font2);
    } else {
        drawButton(BTN_POWER, p.enabled ? "OUTPUT ON" : "OUTPUT OFF", p.enabled ? RED : 0x03E0);
    }
    canvas.pushSprite(0, 0);
}

void handleTouch(int px, int py) {
    PpsStatus p = stateGetPps();
    if (!p.connected) return;
    const int step = stepIdx;
    PpsCmd c;
    if (hit(BTN_V_DN, px, py) || hit(BTN_V_UP, px, py)) {
        c.type = CMD_ADD_V;
        c.v    = hit(BTN_V_DN, px, py) ? -V_STEPS[step] : V_STEPS[step];
        ppsPostCmd(c);
    } else if (hit(BTN_I_DN, px, py) || hit(BTN_I_UP, px, py)) {
        c.type = CMD_ADD_I;
        c.i    = hit(BTN_I_DN, px, py) ? -I_STEPS[step] : I_STEPS[step];
        ppsPostCmd(c);
    } else if (hit(BTN_STEP, px, py)) {
        stepIdx = (step + 1) % 3;
    } else if (hit(BTN_POWER, px, py)) {
        // Never toggle from an untrusted state: with a stale read, only OFF is safe.
        c.type  = CMD_SET;
        c.hasEn = true;
        c.en    = p.valid && !p.enabled;
        ppsPostCmd(c, pdMS_TO_TICKS(100));
    }
}

// Touch runs in its own higher-priority task: a full-screen push takes ~40ms,
// and polling touch only between frames made short taps easy to miss.
void touchTask(void*) {
    for (;;) {
        {
            I2cLock lock;  // touch controller sits on the shared I2C bus
            M5.update();
        }
        auto t = M5.Touch.getDetail();
        if (t.wasPressed()) {
            handleTouch(t.x, t.y);
            xTaskNotifyGive(s_uiTask);  // redraw now instead of waiting for the next frame
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void uiTask(void*) {
    // ~150KB: prefer internal RAM (DMA-capable, ~20ms faster per frame), fall back to PSRAM.
    canvas.setColorDepth(16);
    canvas.setPsram(false);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
        canvas.setPsram(true);
        if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
            // Without the UI there is no local display or OFF button: stop safely.
            systemFatal("UI: sprite allocation failed");
        }
    }
    if (xTaskCreatePinnedToCore(touchTask, "touch", TASK_TOUCH_STACK, nullptr, TASK_TOUCH_PRIO, nullptr,
                                TASK_TOUCH_CORE) != pdPASS) {
        systemFatal("UI: touch task creation failed");
    }
    for (;;) {
        // Test builds only: run the fatal path from the UI task, as a real UI failure would.
        if (faultFatalRequested()) systemFatal("fault injection test");
        draw(stateGetPps(), stateGetBattery(), stateGetNet());
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

}  // namespace

bool uiTaskStart() {
    return xTaskCreatePinnedToCore(uiTask, "ui", TASK_UI_STACK, nullptr, TASK_UI_PRIO, &s_uiTask, TASK_UI_CORE) ==
           pdPASS;
}
