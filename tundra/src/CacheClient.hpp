#pragma once
#include "Hash.hpp"

namespace Frozen { struct DagNode; struct Dag; }
struct StatCache;
struct Mutex;
struct ThreadState;

namespace CacheResult
{
    enum Enum
    {
        DidNotTry,
        Failure,
        CacheMiss,
        Success,
    };
}


struct CacheClient
{
    static CacheResult::Enum AttemptRead(const Frozen::Dag* dag, const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, ThreadState* thread_state);
    static CacheResult::Enum AttemptWrite(const Frozen::Dag* dag, const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, ThreadState* thread_state, const char* ingredients_file);
};

void GetCachingBehaviourSettingsFromEnvironment(bool* attemptReads, bool* attemptWrites);
