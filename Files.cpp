#include "Files.h"
#include "ExperimentMetaPair.h"
#include "Files_Internal.h"
#include "IFileStorage.h"
#include <LittleFS.h>
#include <Logger.h>
#include <MutexRegistry.h>
#include <esp_heap_caps.h>

#undef LOG_TAG
#define LOG_TAG "FILES"

// Shared internal state
std::map<String, String> lockedFiles;

FreeRtosRaii::RecursiveMutex &filesFsMutex()
{
    static FreeRtosRaii::RecursiveMutex m;
    return m;
}

FreeRtosRaii::Mutex &filesLockedMapMutex()
{
    static FreeRtosRaii::Mutex m;
    return m;
}

namespace Files
{
    FreeRtosRaii::RecursiveMutex &fsMutex() { return filesFsMutex(); }
}

String generateLockToken()
{
    return String(millis()) + String(random(1000, 9999));
}

String getFileLockToken(String filename)
{
    std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
    auto it = lockedFiles.find(filename);
    return (it != lockedFiles.end()) ? it->second : "";
}

bool isValidToken(String filename, String token)
{
    std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
    if (token.isEmpty())
        return false;
    auto it = lockedFiles.find(filename);
    return (it != lockedFiles.end() && it->second == token);
}

bool isFileLocked(String filename)
{
    std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
    return lockedFiles.find(filename) != lockedFiles.end();
}

bool checkLockPermission(String filename, String token, const String &operation)
{
    if (!isFileLocked(filename))
        return true;

    if (token.isEmpty() || !isValidToken(filename, token))
    {
        debugE("%s operation blocked - file locked: %s",
               operation.c_str(), filename.c_str());
        return false;
    }
    return true;
}

namespace
{
    constexpr size_t FILE_READ_CHUNK_SIZE = 512;
}

namespace Files
{
    void init()
    {
        RtosUtils::registerMutex(filesFsMutex(), "littlefs");
        RtosUtils::registerMutex(filesLockedMapMutex(), "lockedFilesMap");
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!LittleFS.begin(true))
        {
            debugW("Mount Failed");
            return;
        }
        debugI("Mounted");
    }

    bool exists(const String &filename)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        return LittleFS.exists(filename);
    }

    bool ensureFileExists(const String &filename, bool logError)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        const String normalized = filesEnsureLeadingSlash(filename);
        if (normalized.isEmpty())
        {
            if (logError)
            {
                debugE("Cannot create file with empty path");
            }
            return false;
        }

        if (LittleFS.exists(normalized))
        {
            return true;
        }

        File file = LittleFS.open(normalized, "w");
        if (!file)
        {
            if (logError)
            {
                debugE("Failed to create file: %s", normalized.c_str());
            }
            return false;
        }

        file.close();
        return true;
    }

    void writeToFile(const String &filename, const String &data, const String &token, bool logError)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!checkLockPermission(filename, token, "Write"))
            return;

        File file = LittleFS.open(filename, "w");
        if (!file)
        {
            if (logError)
            {
                debugE("Failed to open file for writing: %s", filename.c_str());
            }
            return;
        }

        if (file.print(data))
        {
            debugD("File written: %s", filename.c_str());
        }
        else
        {
            if (logError)
            {
                debugE("Write failed: %s", filename.c_str());
            }
        }
        file.close();
    }

    void writeToFile(const String &filename, const String &data, bool logError)
    {
        writeToFile(filename, data, "", logError);
    }

    void writeBinaryData(String filename, uint8_t *data, size_t size, String token, bool logError)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!checkLockPermission(filename, token, "Binary write"))
            return;
        if (!data)
        {
            if (logError)
                debugE("Null data pointer");
            return;
        }

        File file = LittleFS.open(filename, "w");
        if (!file)
        {
            if (logError)
            {
                debugE("Failed to open file for writing: %s", filename.c_str());
            }
            return;
        }

        size_t bytesWritten = file.write(data, size);
        if (bytesWritten == size)
        {
            debugD("Binary file written: %s (%u bytes)",
                   filename.c_str(), static_cast<unsigned>(size));
        }
        else
        {
            if (logError)
            {
                debugE("Binary write failed: %s (wrote %u of %u bytes)",
                       filename.c_str(), static_cast<unsigned>(bytesWritten), static_cast<unsigned>(size));
            }
        }
        file.close();
    }

    void writeBinaryData(String filename, uint8_t *data, size_t size, bool logError)
    {
        writeBinaryData(filename, data, size, "", logError);
    }

    size_t readBinaryData(String filename, uint8_t *data, size_t size, String token)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (isFileLocked(filename) && !token.isEmpty() && !isValidToken(filename, token))
        {
            debugW("Reading from locked file: %s", filename.c_str());
        }

        if (!data)
        {
            debugE("Null data pointer");
            return 0;
        }

        bool prevSave = logger.isSaveToFile();
        logger.noSave();
        logger.pauseSecondarySink();

        File file = LittleFS.open(filename, "r");
        if (!file)
        {
            debugE("Failed to open file for reading: %s", filename.c_str());
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return 0;
        }

        size_t availableSize = file.available();
        size_t bytesToRead = min(size, availableSize);
        size_t bytesRead = file.read(data, bytesToRead);

        file.close();
        if (prevSave)
            logger.save();
        logger.resumeSecondarySink();

        if (bytesRead > 0)
        {
            debugD("Binary file read: %s (%u bytes)",
                   filename.c_str(), static_cast<unsigned>(bytesRead));
        }
        return bytesRead;
    }

    String readFromFile(const String &filename)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (isFileLocked(filename))
        {
            debugW("Reading from locked file: %s", filename.c_str());
        }

        bool prevSave = logger.isSaveToFile();
        logger.noSave();
        logger.pauseSecondarySink();

        File file = LittleFS.open(filename, "r");
        if (!file)
        {
            debugE("Failed to open file for reading: %s", filename.c_str());
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return "";
        }

        const size_t fileSize = file.size();
        String data;
        if (fileSize > 0 && !data.reserve(fileSize))
        {
            debugE("Failed to reserve memory for file: %s", filename.c_str());
            file.close();
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return "";
        }

        char buffer[FILE_READ_CHUNK_SIZE + 1];
        while (file.available())
        {
            const size_t bytesRead = file.readBytes(buffer, FILE_READ_CHUNK_SIZE);
            if (bytesRead == 0)
            {
                break;
            }

            buffer[bytesRead] = '\0';
            if (!data.concat(buffer, bytesRead))
            {
                debugE("Failed to append file contents: %s", filename.c_str());
                file.close();
                if (prevSave)
                    logger.save();
                logger.resumeSecondarySink();
                return "";
            }
        }
        file.close();

        if (prevSave)
            logger.save();
        logger.resumeSecondarySink();
        return data;
    }

    char *getFileAsString(const String &path)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        bool prevSave = logger.isSaveToFile();
        logger.noSave();
        logger.pauseSecondarySink();

        File file = LittleFS.open(path);
        if (!file)
        {
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return nullptr;
        }

        size_t fileSize = file.size();
        if (fileSize == 0)
        {
            file.close();
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return nullptr;
        }

        char *buffer = (char *)ps_calloc(fileSize + 1, sizeof(char));
        if (!buffer)
        {
            debugE("Failed to allocate memory for file: %s", path.c_str());
            file.close();
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return nullptr;
        }

        size_t bytesRead = file.readBytes(buffer, fileSize);
        file.close();

        if (prevSave)
            logger.save();
        logger.resumeSecondarySink();

        if (bytesRead != fileSize)
        {
            free(buffer);
            return nullptr;
        }
        return buffer;
    }

    void appendToFile(const String &filename, const String &data, const String &token)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!checkLockPermission(filename, token, "Append"))
            return;

        bool tempLocked = false;
        String workingToken = token;

        if (token.isEmpty())
        {
            workingToken = Lock::lock(filename);
            if (workingToken.isEmpty())
            {
                debugE("Failed to acquire lock for append: %s", filename.c_str());
                return;
            }
            tempLocked = true;
        }

        bool prevSave = logger.isSaveToFile();
        logger.noSave();
        logger.pauseSecondarySink();

        if (!ensureFileExists(filename, true))
        {
            if (tempLocked)
                Lock::unlock(filename, workingToken);
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return;
        }

        File file = LittleFS.open(filename, "a");
        if (!file)
        {
            debugE("Failed to open file for appending: %s", filename.c_str());
            if (tempLocked)
                Lock::unlock(filename, workingToken);
            if (prevSave)
                logger.save();
            logger.resumeSecondarySink();
            return;
        }

        if (file.print(data))
        {
            if (!prevSave)
            {
                debugD("Message appended: %s", filename.c_str());
            }
        }
        else
        {
            debugE("Append failed: %s", filename.c_str());
        }

        file.close();
        if (tempLocked)
            Lock::unlock(filename, workingToken);
        if (prevSave)
            logger.save();
        logger.resumeSecondarySink();
    }

    void appendToFile(const String &filename, const String &data)
    {
        appendToFile(filename, data, "");
    }

    bool deleteFile(String filename, String token)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!checkLockPermission(filename, token, "Delete"))
            return false;

        if (LittleFS.remove(filename))
        {
            debugD("File deleted: %s", filename.c_str());
            {
                std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
                auto it = lockedFiles.find(filename);
                if (it != lockedFiles.end())
                {
                    lockedFiles.erase(it);
                }
            }
            return true;
        }
        else
        {
            debugE("Delete failed: %s", filename.c_str());
            return false;
        }
    }

    bool deleteFile(String filename)
    {
        return deleteFile(filename, "");
    }

    void createDir(String dir)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (LittleFS.mkdir(dir))
        {
            debugD("Dir created: %s", dir.c_str());
        }
        else
        {
            debugE("Create dir failed: %s", dir.c_str());
        }
    }

    void deleteDir(String dir)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        File root = LittleFS.open(dir);
        if (!root)
        {
            debugE("Failed to open directory for deleting: %s", dir.c_str());
            return;
        }
        if (!root.isDirectory())
        {
            debugE("Not a directory: %s", dir.c_str());
            root.close();
            return;
        }

        debugD("Deleting dir: %s", dir.c_str());

        File file = root.openNextFile();
        while (file)
        {
            String name = String(file.name());
            String fullPath = (dir == "/") ? ("/" + name) : (dir + "/" + name);

            if (file.isDirectory())
            {
                file.close();
                deleteDir(fullPath);
            }
            else
            {
                file.close();
                deleteFile(fullPath, "");
            }
            file = root.openNextFile();
        }
        root.close();

        if (LittleFS.rmdir(dir))
        {
            debugD("Dir deleted: %s", dir.c_str());
        }
        else
        {
            debugE("Delete dir failed: %s", dir.c_str());
        }
    }

    String listDir(String dir, int indent)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        File root = LittleFS.open(dir);
        String output = "";

        if (!root)
        {
            debugE("Failed to open directory: %s", dir.c_str());
            return "[ERROR] [LittleFS] Failed to open directory: " + dir + "\n";
        }
        if (!root.isDirectory())
        {
            debugE("Not a directory: %s", dir.c_str());
            String error = "[ERROR] [LittleFS] Not a directory: " + dir + "\n";
            root.close();
            return error;
        }

        if (indent == 0)
        {
            output += "DIR: " + dir + "\n";
            output += listDir(dir, 1);
            root.close();
            return output;
        }

        String spaces = " ";
        for (int i = 0; i < indent; i++)
        {
            spaces += "| ";
        }

        File file = root.openNextFile();
        while (file)
        {
            String name = String(file.name());
            String fullPath = (dir == "/") ? ("/" + name) : (dir + "/" + name);

            if (file.isDirectory())
            {
                output += spaces + "DIR: " + name + "\n";
                file.close(); // Explicitly close before recursive call
                output += listDir(fullPath, indent + 1);
            }
            else
            {
                output += spaces + "FILE: " + name + " - SIZE: " + String(file.size()) + "\n";
                file.close(); // Explicitly close before reassignment
            }
            file = root.openNextFile();
        }

        root.close();
        return output;
    }

    void listDirSerial(String dir)
    {
        String output = listDir(dir);
        debugD("%s", output.c_str());
    }

    void renameFile(String oldName, String newName)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!checkLockPermission(oldName, "", "Rename"))
            return;
        if (!checkLockPermission(newName, "", "Rename"))
            return;

        if (LittleFS.rename(oldName, newName))
        {
            debugD("File renamed: %s -> %s", oldName.c_str(), newName.c_str());
            {
                std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
                auto it = lockedFiles.find(oldName);
                if (it != lockedFiles.end())
                {
                    String token = it->second;
                    lockedFiles.erase(it);
                    lockedFiles[newName] = token;
                }
            }
        }
        else
        {
            debugE("Rename failed: %s -> %s", oldName.c_str(), newName.c_str());
        }
    }

    void renameDir(String oldName, String newName)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (LittleFS.rename(oldName, newName))
        {
            debugD("Dir renamed: %s -> %s", oldName.c_str(), newName.c_str());
        }
        else
        {
            debugE("Rename dir failed: %s -> %s", oldName.c_str(), newName.c_str());
        }
    }

    void copyFile(String oldName, String newName)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (isFileLocked(oldName))
        {
            debugW("Copying from locked file: %s", oldName.c_str());
        }
        if (!checkLockPermission(newName, "", "Copy"))
            return;

        File sourceFile = LittleFS.open(oldName, "r");
        if (!sourceFile)
        {
            debugE("Failed to open source file: %s", oldName.c_str());
            return;
        }

        File destFile = LittleFS.open(newName, "w");
        if (!destFile)
        {
            debugE("Failed to open destination file: %s", newName.c_str());
            sourceFile.close();
            return;
        }

        const size_t bufferSize = 512;
        uint8_t buffer[bufferSize];

        while (sourceFile.available())
        {
            size_t bytesRead = sourceFile.read(buffer, bufferSize);
            if (destFile.write(buffer, bytesRead) != bytesRead)
            {
                debugE("Copy write failed: %s", newName.c_str());
                break;
            }
        }

        sourceFile.close();
        destFile.close();
        debugD("File copied: %s -> %s", oldName.c_str(), newName.c_str());
    }

    void copyDir(String oldName, String newName)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        File root = LittleFS.open(oldName);
        if (!root || !root.isDirectory())
        {
            debugE("Invalid source directory: %s", oldName.c_str());
            if (root)
                root.close();
            return;
        }

        if (!LittleFS.mkdir(newName))
        {
            debugE("Failed to create destination directory: %s", newName.c_str());
            root.close();
            return;
        }

        File file = root.openNextFile();
        while (file)
        {
            String name = String(file.name());
            String srcPath = oldName + "/" + name;
            String destPath = newName + "/" + name;

            if (file.isDirectory())
            {
                file.close(); // Explicitly close before recursive call
                copyDir(srcPath, destPath);
            }
            else
            {
                file.close(); // Explicitly close before function call
                copyFile(srcPath, destPath);
            }
            file = root.openNextFile();
        }
        root.close();
    }

    void moveFile(String oldName, String newName)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (!checkLockPermission(oldName, "", "Move"))
            return;
        if (!checkLockPermission(newName, "", "Move"))
            return;

        if (LittleFS.rename(oldName, newName))
        {
            debugD("File moved: %s -> %s", oldName.c_str(), newName.c_str());
            {
                std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
                auto it = lockedFiles.find(oldName);
                if (it != lockedFiles.end())
                {
                    String token = it->second;
                    lockedFiles.erase(it);
                    lockedFiles[newName] = token;
                }
            }
        }
        else
        {
            copyFile(oldName, newName);
            deleteFile(oldName, "");
        }
    }

    void moveDir(String oldName, String newName)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (LittleFS.rename(oldName, newName))
        {
            debugD("Dir moved: %s -> %s", oldName.c_str(), newName.c_str());
        }
        else
        {
            copyDir(oldName, newName);
            deleteDir(oldName);
        }
    }

    void format()
    {
        {
            std::lock_guard<FreeRtosRaii::Mutex> mapLock(filesLockedMapMutex());
            lockedFiles.clear();
        }
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (LittleFS.format())
        {
            debugI("Formatted");
        }
        else
        {
            debugE("Format failed");
        }
    }

    size_t getFileSize(const String &filename, const String &token)
    {
        std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
        if (isFileLocked(filename) && !token.isEmpty() && !isValidToken(filename, token))
        {
            debugW("Getting size of locked file: %s", filename.c_str());
        }

        File file = LittleFS.open(filename, "r");
        if (!file)
        {
            debugE("Failed to open file for reading size: %s", filename.c_str());
            return 0;
        }

        size_t fileSize = file.size();
        file.close();
        return fileSize;
    }


    void logMemoryUsage(const String &context)
    {
        debugD("%s - Free heap: %d bytes, Largest free block: %d bytes",
               context.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
}

// --- IFileStorage default implementation --------------------------

namespace
{
    class LittleFSFileStorage : public IFileStorage
    {
    public:
        bool exists(const char *path) override
        {
            return Files::exists(String(path));
        }

        bool writeFile(const char *path, const uint8_t *data, size_t len) override
        {
            Files::writeBinaryData(String(path), const_cast<uint8_t *>(data), len, true);
            return true;
        }

        size_t readFile(const char *path, uint8_t *buf, size_t bufLen) override
        {
            return Files::readBinaryData(String(path), buf, bufLen);
        }

        bool appendFile(const char *path, const uint8_t *data, size_t len) override
        {
            std::lock_guard<FreeRtosRaii::RecursiveMutex> fsLock(filesFsMutex());
            File file = LittleFS.open(path, "a");
            if (!file)
                return false;
            const size_t written = file.write(data, len);
            file.close();
            return written == len;
        }

        bool deleteFile(const char *path) override
        {
            return Files::deleteFile(String(path));
        }

        size_t fileSize(const char *path) override
        {
            return Files::getFileSize(String(path));
        }

        bool writeString(const char *path, const String &data) override
        {
            Files::writeToFile(String(path), data, true);
            return true;
        }

        String readString(const char *path) override
        {
            return Files::readFromFile(String(path));
        }

        String acquireLock(const char *path) override
        {
            return Files::Lock::lock(String(path));
        }

        void releaseLock(const char *path, const String &token) override
        {
            Files::Lock::unlock(String(path), token);
        }
    };

    LittleFSFileStorage gFileStorage;
}

IFileStorage &fileStorage = gFileStorage;

