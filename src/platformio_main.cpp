#include "app_entry.h"

#ifdef PLATFORMIO
void setup() { appSetup(); }

void loop() { appLoop(); }
#endif
