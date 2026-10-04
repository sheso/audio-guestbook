#pragma once

#include <Arduino.h>

// Starts the web interface on port 80
void webBegin();
// Print every HTTP request on the USB serial monitor
void webSetLog(bool on);
// One line about the requests served so far, for the "status" command
String webStats();
