#pragma once

#include <cstdio>
#include <cstring>

// Release versions are "<major>.<minor>.<patch>-echo.<n>": the upstream
// CrossPoint base, then the EchoPoint release counter on that base. Dev and
// release-candidate builds append "-dev-<branch>-<sha>" or "-rc+<sha>".
namespace firmware_version {

struct Parts {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int echo = 0;  // Missing on upstream versions, which sort before "-echo.1".
};

inline Parts parse(const char* version) {
  Parts parts;
  sscanf(version, "%d.%d.%d", &parts.major, &parts.minor, &parts.patch);
  if (const char* counter = strstr(version, "-echo.")) sscanf(counter, "-echo.%d", &parts.echo);
  return parts;
}

// Whether the release `latest` should replace the running build `current`.
inline bool isNewer(const char* latest, const char* current) {
  if (strcmp(latest, current) == 0) return false;
  const Parts l = parse(latest);
  const Parts c = parse(current);
  if (l.major != c.major) return l.major > c.major;
  if (l.minor != c.minor) return l.minor > c.minor;
  if (l.patch != c.patch) return l.patch > c.patch;
  if (l.echo != c.echo) return l.echo > c.echo;
  // A release candidate comes before the release with the same number.
  return strstr(current, "-rc") != nullptr;
}

}  // namespace firmware_version
