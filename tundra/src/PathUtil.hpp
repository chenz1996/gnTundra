#pragma once

#include "Common.hpp"
#include "BinaryData.hpp"

#include <string.h>

#if ENABLED(TUNDRA_CASE_INSENSITIVE_FILESYSTEM)
#if defined(_MSC_VER) || defined(TUNDRA_WIN32_MINGW)
#define PathCompareN _strnicmp
#define PathCompare _stricmp
#else
#define PathCompareN strncasecmp
#define PathCompare strcasecmp
#endif
#else // ENABLED(TUNDRA_CASE_INSENSITIVE_FILESYSTEM)
#define PathCompareN strncmp
#define PathCompare strcmp
#endif // ENABLED(TUNDRA_CASE_INSENSITIVE_FILESYSTEM)

#if defined(TUNDRA_WIN32)
#define strncasecmp _strnicmp
#endif

static const int kMaxPathLength = 512;
static const int kMaxPathSegments = 64;

struct BinarySegment;

namespace PathType
{
enum Enum
{
    kUnix,
    kWindows,
#if defined(TUNDRA_WIN32)
    kNative = kWindows
#else
    kNative = kUnix
#endif
};
} // namespace PathType

struct PathBuffer
{
    enum
    {
        kFlagAbsolute = 1 << 0,
        kFlagWindowsDevicePath = 1 << 1
    };

    uint16_t SegLength(int i) const
    {
        CHECK(uint16_t(i) < m_SegCount);
        if (i > 0)
        {
            return uint16_t(m_SegEnds[i] - m_SegEnds[i - 1]);
        }
        else
        {
            return m_SegEnds[0];
        }
    }

    PathType::Enum m_Type;
    uint16_t m_Flags;
    uint16_t m_SegCount;
    uint16_t m_LeadingDotDots;
    uint16_t m_SegEnds[kMaxPathSegments];
    char m_Data[kMaxPathLength];
};

inline bool operator==(const PathBuffer &a, const PathBuffer &b)
{
    if (a.m_SegCount != b.m_SegCount)
        return false;

    if (a.m_SegCount == 0)
        return true;

    return 0 == memcmp(a.m_SegEnds, b.m_SegEnds, sizeof(uint16_t) * a.m_SegCount) &&
           0 == memcmp(a.m_Data, b.m_Data, a.SegLength(a.m_SegCount - 1));
}

inline bool operator!=(const PathBuffer &a, const PathBuffer &b)
{
    return !(a == b);
}

inline bool PathIsAbsolute(const PathBuffer *buffer)
{
    return 0 != (buffer->m_Flags & PathBuffer::kFlagAbsolute);
}

void PathInit(PathBuffer *buffer, const char *path, PathType::Enum type = PathType::kNative);

bool PathStripLast(PathBuffer *buffer);
void PathConcat(PathBuffer *buffer, const char *other);
void PathConcat(PathBuffer *buffer, const PathBuffer *other);

void PathFormat(char (&output)[kMaxPathLength], const PathBuffer *buffer);
void PathFormatPartial(char (&output)[kMaxPathLength], const PathBuffer *buffer, int start_seg, int end_seg);
const char* FindFileNameInside(const char* path);
