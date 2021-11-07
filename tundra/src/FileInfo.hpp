#pragma once

#include "Common.hpp"

struct FileInfo
{
    enum
    {
        kFlagExists = 1 << 0,
        kFlagError = 1 << 1,
        kFlagFile = 1 << 2,
        kFlagDirectory = 1 << 3,
        kFlagSymlink = 1 << 4, // also a junction on Windows
        kFlagReadOnly     = 1 << 5,
        kFlagDirty = 1 << 30   // used by stat cache
    };

    uint32_t m_Flags;
    uint64_t m_Size;
    uint64_t m_Timestamp;

    bool Exists() const { return 0 != (kFlagExists & m_Flags); }
    bool IsFile() const { return 0 != (kFlagFile & m_Flags); }
    bool IsDirectory() const { return 0 != (kFlagDirectory & m_Flags); }
    bool IsSymlink() const { return 0 != (kFlagSymlink & m_Flags); }
    bool IsReadOnly()  const { return 0 != (kFlagReadOnly & m_Flags); }
};

FileInfo GetFileInfo(const char *path);

bool ShouldFilter(const char *name);
bool ShouldFilter(const char *name, size_t len);

void ListDirectory(
    const char *dir,
    const char *filter,
    bool recurse,
    void *user_data,
    void (*callback)(void *user_data, const FileInfo &info, const char *path));

bool DeleteDirectory(const char* path);
