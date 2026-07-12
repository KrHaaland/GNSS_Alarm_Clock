// SimConsole.h — simulated-GNSS serial console (env:sim / -DGNSS_SIM only).
// Lets you type coordinates (and a UTC time) into the USB serial monitor to
// drive the timezone/clock pipeline without a real GPS fix. Compiled to
// nothing in normal builds. See platformio.ini [env:sim].
#pragma once

void sim_console_begin(); // print banner + help (call once, late in setup)
void sim_console_task();  // poll the serial line (call each loop)
