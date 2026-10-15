#pragma once
#include <Arduino.h>
#include <vector>

// Easter eggs: guests record them on the Teensy for a dialled number ("egg-<number>.wav"), and the
// sync task copies them to EGGS_DIR/<number>.wav. They are only shown in the password-protected
// service view, so they are kept apart from the recordings and their index.
struct Egg {
  String number; // as dialled, e.g. "4243"
  uint32_t size; // bytes
  uint32_t mtime; // local time as unix timestamp, 0 until the first sync after a reboot
};

bool validEggNumber(const String& number);
String eggPath(const String& number);     // /guestbook/eggs/4243.wav
String eggPartPath(const String& number); // /guestbook/eggs/4243.part, while copying

// Copied eggs, sorted by number
std::vector<Egg> eggsList();
bool eggFind(const String& number, Egg& egg);
// Dates come from the Teensy with every EGGS listing
void eggsSetMtime(const String& number, uint32_t mtime);
