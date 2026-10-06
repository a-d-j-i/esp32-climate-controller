#pragma once
#include <Arduino.h>

// Starts the Secure Shell server task (one session at a time). Supports an interactive
// shell (ssh admin@climate.local) and single commands (ssh admin@climate.local status).
void sshBegin();
String sshHostKeyFingerprint();
