#include "StatCache.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "Stats.hpp"

#include <algorithm>

#include "Banned.hpp"


void StatCacheInit(StatCache *self, MemAllocLinear *allocator, MemAllocHeap *heap)
{
  self->m_Allocator = allocator;
  self->m_Heap = heap;
  HashTableInit(&self->m_Files, heap);
  ReadWriteLockInit(&self->m_HashLock);
}

void StatCacheDestroy(StatCache *self)
{
  HashTableDestroy(&self->m_Files);
  ReadWriteLockDestroy(&self->m_HashLock);
}

static void StatCacheInsert(StatCache *self, uint32_t hash, const char *path, const FileInfo &info)
{
  ReadWriteLockWrite(&self->m_HashLock);
  HashTableInsert(&self->m_Files, hash, StrDup(self->m_Allocator, path), info);
  ReadWriteUnlockWrite(&self->m_HashLock);
}

static void StatCacheUpdate(StatCache *self, uint32_t hash, const char *path, const FileInfo &info)
{
  ReadWriteLockWrite(&self->m_HashLock);

  if (FileInfo *fi = HashTableLookup(&self->m_Files, hash, path))
  {
    *fi = info;
  } else
  {
    Croak("StatCacheUpdate called with %s but it was not present int statcache", path);
  }

  ReadWriteUnlockWrite(&self->m_HashLock);
}

void StatCacheMarkDirty(StatCache *self, const char *path, uint32_t hash)
{
  ReadWriteLockWrite(&self->m_HashLock);

  if (FileInfo *fi = HashTableLookup(&self->m_Files, hash, path))
  {
    fi->m_Flags = FileInfo::kFlagDirty;
  }

  ReadWriteUnlockWrite(&self->m_HashLock);
}

FileInfo StatCacheStat(StatCache *self, const char *path, uint32_t hash)
{
  ReadWriteLockRead(&self->m_HashLock);

  const FileInfo *existing_hashtable_entry = HashTableLookup(&self->m_Files, hash, path);

  if (existing_hashtable_entry != nullptr && 0 == (existing_hashtable_entry->m_Flags & FileInfo::kFlagDirty))
  {
    FileInfo result = *existing_hashtable_entry;
    ReadWriteUnlockRead(&self->m_HashLock);
    AtomicIncrement(&g_Stats.m_StatCacheHits);
    return result;
  }

  ReadWriteUnlockRead(&self->m_HashLock);

  AtomicIncrement(&g_Stats.m_StatCacheMisses);
  FileInfo file_info = GetFileInfo(path);

  if (existing_hashtable_entry == nullptr)
  {
    // There's a natural race condition here. Some other thread might come in,
    // stat the file and insert it before us. We just let that happen. The DAG
    // guarantees that we won't be writing to files that are being stat'd here,
    // so the result of these races is benign.
    StatCacheInsert(self, hash, path, file_info);
  }
  else
    StatCacheUpdate(self, hash, path, file_info);

  return file_info;
}


