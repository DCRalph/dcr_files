#pragma once

#include <Arduino.h>
#include <dcr_taskManager/FreeRtosRaii.h>
#include <map>

// Shared internal implementation details for the Files namespace.

extern std::map<String, String> lockedFiles;

FreeRtosRaii::RecursiveMutex &filesFsMutex();
FreeRtosRaii::Mutex &filesLockedMapMutex();

// Helper functions for file locking
String generateLockToken();
String getFileLockToken(String filename);
bool isValidToken(String filename, String token);
bool isFileLocked(String filename);
bool checkLockPermission(String filename, String token, const String &operation);

// Normalizes filesystem paths for LittleFS (leading slash).
inline String filesEnsureLeadingSlash(const String &path)
{
    if (path.isEmpty())
        return "";
    return path.startsWith("/") ? path : ("/" + path);
}
