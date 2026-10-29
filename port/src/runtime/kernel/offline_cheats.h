#pragma once
namespace offline_cheats {
// Bit 0 protects side 1, bit 1 protects side 2 (including each side's tag partner).
int HealthSides();
void SetHealthSides(int sides);
bool InfiniteMoney();
void SetInfiniteMoney(bool enabled);
bool UnlockContent();
void SetUnlockContent(bool enabled);
bool Allowed();
void SetOfflineMenu(bool offline);
}
