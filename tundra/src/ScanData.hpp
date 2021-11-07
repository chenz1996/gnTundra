#pragma once

#include "BinaryData.hpp"

namespace Frozen
{
struct ScanCacheEntry
{
    uint64_t m_FileTimestamp;
    FrozenArray<FrozenFileAndHash> m_IncludedFiles;
};

struct ScanData
{
    static const uint32_t MagicNumber = 0x1517000f ^ kTundraHashMagic;

    uint32_t m_MagicNumber;

    int32_t m_EntryCount;

    FrozenPtr<HashDigest> m_Keys;
    FrozenPtr<ScanCacheEntry> m_Data;
    FrozenPtr<uint64_t> m_AccessTimes;
    uint32_t m_MagicNumberEnd;
};
}
