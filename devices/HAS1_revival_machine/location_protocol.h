#ifndef HAS3_LOCATION_PROTOCOL_H
#define HAS3_LOCATION_PROTOCOL_H

#include <Arduino.h>

#define HAS3_LOCAL_NAME_PREFIX "HAS3:"
#define HAS3_MAX_DEVICE_NAME_LEN 18

inline bool Has3IsDevicePrefix(char prefix)
{
  return prefix == 'B' || prefix == 'T' || prefix == 'S' ||
         prefix == 'U' || prefix == 'H' || prefix == 'C';
}

inline bool Has3IsValidDeviceName(const char *device_name)
{
  if (device_name == nullptr)
  {
    return false;
  }

  size_t len = strlen(device_name);
  if (len == 0 || len > HAS3_MAX_DEVICE_NAME_LEN)
  {
    return false;
  }

  for (size_t i = 0; i < len; i++)
  {
    char ch = device_name[i];
    bool ok = (ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-';
    if (!ok)
    {
      return false;
    }
  }

  return true;
}

#endif
