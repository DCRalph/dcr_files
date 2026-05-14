#pragma once

#include <Arduino.h>
#include <dcr_taskManager/FreeRtosRaii.h>
#include <LittleFS.h>

namespace Files
{
  // Exposes the recursive mutex that guards LittleFS access. Callers that
  // bypass the Files:: API and touch LittleFS directly must take this lock
  // for the duration of their access.
  FreeRtosRaii::RecursiveMutex &fsMutex();

  // Core file operations
  void init();
  bool exists(const String &filename);
  bool ensureFileExists(const String &filename, bool logError = true);

  // File read/write operations
  void writeToFile(const String &filename, const String &data, bool logError = true);
  void writeToFile(const String &filename, const String &data, const String &token, bool logError = true);
  String readFromFile(const String &filename);
  char *getFileAsString(const String &path);
  void appendToFile(const String &filename, const String &data);
  void appendToFile(const String &filename, const String &data, const String &token);

  // File deletion
  bool deleteFile(String filename);
  bool deleteFile(String filename, String token);

  // Directory operations
  void createDir(String dir);
  void deleteDir(String dir);
  String listDir(String dir, int indent = 0);
  void listDirSerial(String dir);

  // File operations
  void renameFile(String oldName, String newName);
  void renameDir(String oldName, String newName);
  void copyFile(String oldName, String newName);
  void copyDir(String oldName, String newName);
  void moveFile(String oldName, String newName);
  void moveDir(String oldName, String newName);

  // File system operations
  void format();
  size_t getFileSize(const String &filename, const String &token = "");


  void logMemoryUsage(const String &context);

  // Binary data operations
  void writeBinaryData(String filename, uint8_t *data, size_t size, bool logError = true);
  void writeBinaryData(String filename, uint8_t *data, size_t size, String token, bool logError = true);
  size_t readBinaryData(String filename, uint8_t *data, size_t size, String token = "");

  // Lock operations
  namespace Lock
  {
    String lock(const String &filename);
    void unlock(const String &filename, const String &token = "");
    String readAndLock(const String &filename);
    bool writeAndUnlock(const String &filename, const String &data, const String &token = "");
    bool appendWithLock(const String &filename, const String &data);
  }
}
