

#include "mbed.h"
#include "arm_book_lib.h"
#include "SDBlockDevice.h"
#include "FATFileSystem.h"
#include <cstring>
#include <cstdio>

// =============================================================================
// Configuration
// =============================================================================
#define TEMP_THRESHOLD   25.0f     // °C
#define GAS_THRESHOLD   300.0f     // ppm
#define LM35_SCALE      100.0f     // reading * 280 = °C (lab-calibrated)
#define GAS_SCALE      1000.0f     // reading * 1000 = approx ppm
#define LOG_FILE        "/sd/alerts.txt"

// =============================================================================
// Hardware
// =============================================================================
AnalogIn         lm35(A1);
AnalogIn         gasSensor(A2);
UnbufferedSerial uart(USBTX, USBRX, 115200);

// SD card — SPI3 pins
SDBlockDevice    sd(PC_12, PC_11, PC_10, PA_4);   // MOSI, MISO, SCK, CS
FATFileSystem    fs("sd");

// Keypad
BusOut kRows(PB_3, PB_5, PC_7, PA_15);
BusIn  kCols(PB_12, PB_13, PB_15, PC_6);

// =============================================================================
// Global state
// =============================================================================
typedef enum { MONITOR_NONE, MONITOR_TEMP, MONITOR_GAS, MONITOR_BOTH } MonitorMode;

static MonitorMode monitorMode  = MONITOR_NONE;
static int         alertCount   = 0;
static bool        sdMounted    = false;
static char        printBuf[128];

// =============================================================================
// UART helpers  (mbed Newlib-nano: no %f in sprintf)
// =============================================================================
static void uartStr(const char* s) {
    uart.write(s, strlen(s));
}

static void floatToStr(float val, int decimals, char* out) {
    if (val < 0.0f) { *out++ = '-'; val = -val; }
    int whole = (int)val;
    if (decimals == 0) {
        sprintf(out, "%d", whole);
    } else {
        int dec = (int)((val - whole) * 10.0f);
        sprintf(out, "%d.%d", whole, dec);
    }
}

// =============================================================================
// SD card mount / unmount
// =============================================================================
bool mountSD() {
    if (sdMounted) return true;
    int err = fs.mount(&sd);
    if (err) {
        // Try reformatting if first mount fails (blank card)
        uartStr("[SD] Mount failed, attempting format...\r\n");
        err = fs.reformat(&sd);
        if (err) {
            uartStr("[SD] Format failed. Check SD card.\r\n");
            return false;
        }
        uartStr("[SD] Format successful.\r\n");
    }
    sdMounted = true;
    uartStr("[SD] Mounted successfully.\r\n");
    return true;
}

// =============================================================================
// Log an alert to SD card and confirm on UART
// =============================================================================
void logAlert(const char* sensorName, float value, const char* unit) {
    alertCount++;

    // Build log line
    char valStr[12];
    floatToStr(value, 1, valStr);
    int lineLen = sprintf(printBuf,
                          "Alert #%d | Sensor: %s | Value: %s %s | State: ACTIVE\r\n",
                          alertCount, sensorName, valStr, unit);

    // Print confirmation to serial monitor
    uartStr("Alert Logged: ");
    uartStr(sensorName);
    uartStr(" = ");
    uartStr(valStr);
    uartStr(" ");
    uartStr(unit);
    uartStr("\r\n");

    // Write to SD card
    if (!mountSD()) return;

    FILE* f = fopen(LOG_FILE, "a");
    if (f) {
        fwrite(printBuf, 1, lineLen, f);
        fclose(f);
    } else {
        uartStr("[SD] Failed to open alerts.txt for writing.\r\n");
    }
}

// =============================================================================
// Read and print all logged alerts from SD card
// =============================================================================
void readAlerts() {
    if (!mountSD()) return;

    FILE* f = fopen(LOG_FILE, "r");
    if (!f) {
        uartStr("[SD] No alert log found (alerts.txt does not exist yet).\r\n");
        return;
    }

    uartStr("\r\n========== Alert Log ==========\r\n");
    char line[128];
    int lineNum = 0;
    while (fgets(line, sizeof(line), f)) {
        uart.write(line, strlen(line));
        lineNum++;
    }
    fclose(f);

    if (lineNum == 0) {
        uartStr("(Log file is empty)\r\n");
    }
    uartStr("================================\r\n\r\n");
}

// =============================================================================
// Sensor reading + threshold check
// =============================================================================
float readTemp() { return lm35.read() * LM35_SCALE; }
float readGas()  { return gasSensor.read() * GAS_SCALE; }

void checkAndLog() {
    float temp = readTemp();
    float gas  = readGas();

    if (monitorMode == MONITOR_TEMP || monitorMode == MONITOR_BOTH) {
        if (temp > TEMP_THRESHOLD) {
            logAlert("Temp", temp, "C");
        }
    }

    if (monitorMode == MONITOR_GAS || monitorMode == MONITOR_BOTH) {
        if (gas > GAS_THRESHOLD) {
            logAlert("Gas", gas, "ppm");
        }
    }

    // Always print current readings to UART so user can observe
    char tStr[12], gStr[12];
    floatToStr(temp, 1, tStr);
    floatToStr(gas,  0, gStr);
    int len = sprintf(printBuf, "Monitoring [%s] | Temp: %s C | Gas: %s ppm\r\n",
                      monitorMode == MONITOR_TEMP ? "TEMP" :
                      monitorMode == MONITOR_GAS  ? "GAS"  : "BOTH",
                      tStr, gStr);
    uart.write(printBuf, len);
}

// =============================================================================
// Keypad scanner
// =============================================================================
const char keyMap[16] = {
    '1','2','3','A',
    '4','5','6','B',
    '7','8','9','C',
    '*','0','#','D'
};

char scanKeypad() {
    for (int r = 0; r < 4; r++) {
        kRows = (~(1 << r)) & 0xF;
        ThisThread::sleep_for(1ms);
        for (int c = 0; c < 4; c++) {
            if (((kCols >> c) & 1) == 0) {
                while (((kCols >> c) & 1) == 0);
                kRows = 0xF;
                return keyMap[r * 4 + c];
            }
        }
    }
    kRows = 0xF;
    return '\0';
}

// =============================================================================
// Print startup menu to UART
// =============================================================================
void printMenu() {
    uartStr("\r\n====== SD Card Sensor Logger ======\r\n");
    uartStr("  Keypad 'A' -> Monitor Temperature\r\n");
    uartStr("  Keypad 'B' -> Monitor Gas\r\n");
    uartStr("  Keypad 'C' -> Monitor Both\r\n");
    uartStr("  Keypad '*' -> Read all logged alerts\r\n");
    uartStr("  Keypad '#' -> Reset alert counter\r\n");
    uartStr("===================================\r\n");
    uartStr("Select a sensor to begin logging...\r\n\r\n");
}

// =============================================================================
// Main
// =============================================================================
int main() {
    kCols.mode(PullUp);

    // Attempt SD mount at startup
    mountSD();

    printMenu();

    while (true) {

        // --- Keypad ---
        char key = scanKeypad();

        if (key == 'A') {
            monitorMode = MONITOR_TEMP;
            alertCount  = 0;
            uartStr("[MODE] Temperature monitoring active. Threshold: >25.0 C\r\n");
        }
        else if (key == 'B') {
            monitorMode = MONITOR_GAS;
            alertCount  = 0;
            uartStr("[MODE] Gas monitoring active. Threshold: >300 ppm\r\n");
        }
        else if (key == 'C') {
            monitorMode = MONITOR_BOTH;
            alertCount  = 0;
            uartStr("[MODE] Both sensors monitoring active.\r\n");
        }
        else if (key == '*') {
            readAlerts();
        }
        else if (key == '#') {
            alertCount = 0;
            uartStr("[RESET] Alert counter reset to 0.\r\n");
        }

        // --- Sensor check (only if a mode has been selected) ---
        if (monitorMode != MONITOR_NONE) {
            checkAndLog();
        }

        ThisThread::sleep_for(2000ms);   // check every 2 seconds
    }
}