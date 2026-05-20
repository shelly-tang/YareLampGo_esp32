#ifndef LAMPGO_LED_SERIAL_H
#define LAMPGO_LED_SERIAL_H

#include <stdint.h>

namespace LedSerial {

bool begin();
bool isReady();

bool setMode(int mode);
bool setModeName(const char *name);
bool setBrightness(int brightness);

int resolveMode(const char *name);
const char *modeName(int mode);

int currentMode();
int currentBrightness();
uint32_t lastWriteMs();
const char *lastCommand();

const char *driverName();
int pixelPin();
int pixelCount();
int panelCount();
bool outputOk();

int txPin();
int rxPin();
uint32_t baudRate();

}

#endif
