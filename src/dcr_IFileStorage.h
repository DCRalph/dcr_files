#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

class IFileStorage
{
public:
  virtual ~IFileStorage() = default;

  // byte buffer API
  virtual bool exists(const char *path) = 0;
  virtual bool writeFile(const char *path, const uint8_t *data, size_t len) = 0;
  virtual size_t readFile(const char *path, uint8_t *buf, size_t bufLen) = 0;
  virtual bool appendFile(const char *path, const uint8_t *data, size_t len) = 0;
  virtual bool deleteFile(const char *path) = 0;
  virtual size_t fileSize(const char *path) = 0;

  // String overloads (NetLink credential storage needs these)
  virtual bool writeString(const char *path, const String &data) = 0;
  virtual String readString(const char *path) = 0;

  // Lock primitives — token-based, matches existing Files::Lock API.
  // acquireLock() returns an empty String on failure.
  virtual String acquireLock(const char *path) = 0;
  virtual void releaseLock(const char *path, const String &token) = 0;
};

extern IFileStorage &fileStorage;
