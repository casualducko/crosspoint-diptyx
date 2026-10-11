#pragma once

// Set by an activity to ask main's loop for a power off (Diptyx: the sleep screens are drawn, then the battery latch
// is released; with USB attached the board stays in standby).
extern volatile bool powerOffRequested;
