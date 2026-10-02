// silicon - BIOS-style firmware screens (firmware.cpp)
#pragma once

int  firmware_boot();      // POST screen + optional setup / boot menu; returns 0 terminal, 1 desktop
void firmware_setup();     // the setup utility on its own (the 'setup' command)
