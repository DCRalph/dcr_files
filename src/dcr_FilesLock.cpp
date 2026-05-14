#include "dcr_Files.h"
#include "dcr_Files_Internal.h"
#include <LittleFS.h>
#include <dcr_Logger.h>
#include <mutex>

#undef LOG_TAG
#define LOG_TAG "FILES_LOCK"

namespace Files
{
    namespace Lock
    {
        String lock(const String &filename)
        {
            if (isFileLocked(filename))
            {
                for (int retry = 0; retry < 3; retry++)
                {
                    delay(50 * (retry + 1));
                    if (!isFileLocked(filename))
                        break;
                    if (retry == 2)
                    {
                        debugE("File still locked after retries: %s", filename.c_str());
                        return "";
                    }
                }
            }

            String token = generateLockToken();
            {
                std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
                lockedFiles[filename] = token;
            }
            return token;
        }

        void unlock(const String &filename, const String &token)
        {
            if (!isFileLocked(filename))
                return;

            if (!token.isEmpty() && !isValidToken(filename, token))
            {
                debugE("Invalid token for unlocking file: %s", filename.c_str());
                return;
            }

            std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
            lockedFiles.erase(filename);
        }

        String readAndLock(const String &filename)
        {
            String token = lock(filename);
            if (token.isEmpty())
            {
                debugE("Failed to lock file for reading: %s", filename.c_str());
                return "";
            }

            String data = Files::readFromFile(filename);
            if (data.isEmpty() && !Files::exists(filename))
            {
                unlock(filename, token);
            }
            return data;
        }

        bool writeAndUnlock(const String &filename, const String &data, const String &token)
        {
            std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
            String workingToken = token;
            if (workingToken.isEmpty())
            {
                if (!isFileLocked(filename))
                {
                    debugE("File not locked: %s", filename.c_str());
                    return false;
                }
                workingToken = getFileLockToken(filename);
            }

            if (!isValidToken(filename, workingToken))
            {
                debugE("Invalid token for: %s", filename.c_str());
                return false;
            }

            File file = LittleFS.open(filename, "w");
            if (!file)
            {
                debugE("Failed to open file for writing: %s", filename.c_str());
                unlock(filename, workingToken);
                return false;
            }

            bool success = file.print(data);
            file.close();

            if (success)
            {
                debugI("File written: %s", filename.c_str());
            }
            else
            {
                debugE("Write failed: %s", filename.c_str());
            }

            unlock(filename, workingToken);
            return success;
        }

        bool appendWithLock(const String &filename, const String &data)
        {
            std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
            String token = lock(filename);
            if (token.isEmpty())
            {
                for (int retry = 0; retry < 3; retry++)
                {
                    delay(50 * (retry + 1));
                    token = lock(filename);
                    if (!token.isEmpty())
                        break;
                    if (retry == 2)
                    {
                        debugE("Failed to lock file for appending: %s", filename.c_str());
                        return false;
                    }
                }
            }

            File file = LittleFS.open(filename, "a");
            if (!file)
            {
                debugE("Failed to open file for appending: %s", filename.c_str());
                unlock(filename, token);
                return false;
            }

            bool success = file.print(data);
            file.close();

            if (success)
                debugI("File appended: %s", filename.c_str());
            else
                debugE("Append failed: %s", filename.c_str());

            unlock(filename, token);
            return success;
        }
    }
}
