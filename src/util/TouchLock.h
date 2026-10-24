#pragma once

// Touch lock for a unit shut in a case, where the touch controller sees the other unit and fires taps and the Home
// pad. Boards with a capacitive Home key only. Holding both page buttons for two seconds toggles it; the lock is kept
// across sleep and, on a linked pair, copied to the other unit (see UnitLinkService).
namespace touchlock {
bool locked();
// Sets the lock, saves it and shows a short popup.
void set(bool on);
// Once per main loop pass: the two-button gesture and clearing the popup.
void tick();
// Boot: restores the saved lock.
void restore();
}  // namespace touchlock
