#pragma once

#include "Common.hpp"
#include "Hash.hpp"
#include "Buffer.hpp"
#include "HashTable.hpp"
#include "DynamicallyGrowingCollectionOfPaths.hpp"

namespace NodeBuildResult
{
    enum Enum
    {
        kDidNotRun = 0,
        kUpToDate,
        kUpToDateButDependeesRequireFrontendRerun,
        kRanSuccesfully,
        kRanFailed,
        kRanSuccessButDependeesRequireFrontendRerun
    };
}

namespace RuntimeNodeFlags
{
    static const uint16_t kQueued = 1 << 0;
    static const uint16_t kActive = 1 << 1;
    static const uint16_t kHasEverBeenQueued = 1 << 2;
    static const uint16_t kExplicitlyRequested = 1 << 3;
    static const uint16_t kExplicitlyRequestedThroughUseDependency = 1 << 4;
    static const uint16_t kAttemptedCacheLookup = 1 << 5;
    static const uint16_t kInputSignatureMightBeIncorrect = 1 << 6;
    static const uint16_t kSentBinLogNodeInfoMessage = 1 << 7;
}

namespace Frozen
{
    struct DagNode;
    struct DagNodeDerived;
    struct BuiltNode;
}

struct SinglyLinkedPathList;
struct LeafInputSignatureData;

struct RuntimeNode
{
    uint16_t m_Flags;
    uint32_t m_DagNodeIndex;
#if ENABLED(CHECKED_BUILD)
    const char *m_DebugAnnotation;
#endif
    const Frozen::DagNode *m_DagNode;
    const Frozen::BuiltNode *m_BuiltNode;

    NodeBuildResult::Enum m_BuildResult;
    bool m_Finished;
    HashDigest m_CurrentInputSignature;

    DynamicallyGrowingCollectionOfPaths* m_DynamicallyDiscoveredOutputFiles;
    LeafInputSignatureData* m_CurrentLeafInputSignature;
    HashSet<kFlagPathStrings> m_ImplicitInputs;
};

inline bool RuntimeNodeIsQueued(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kQueued);
}

inline bool RuntimeNodeHasEverBeenQueued(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kHasEverBeenQueued);
}

inline void RuntimeNodeFlagQueued(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kQueued;
    runtime_node->m_Flags |= RuntimeNodeFlags::kHasEverBeenQueued;
}

inline void RuntimeNodeFlagUnqueued(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags &= ~RuntimeNodeFlags::kQueued;
}

inline bool RuntimeNodeIsActive(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kActive);
}

inline void RuntimeNodeFlagActive(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kActive;
}

inline void RuntimeNodeSetAttemptedCacheLookup(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kAttemptedCacheLookup;
}

inline bool RuntimeNodeHasAttemptedCacheLookup(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kAttemptedCacheLookup);
}

inline void RuntimeNodeSet_SentBinLogNodeInfoMessage(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kSentBinLogNodeInfoMessage;
}

inline bool RuntimeNodeHas_SentBinLogNodeInfoMessage(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kSentBinLogNodeInfoMessage);
}

inline void RuntimeNodeFlagInactive(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags &= ~RuntimeNodeFlags::kActive;
}

inline bool RuntimeNodeIsExplicitlyRequested(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kExplicitlyRequested);
}

inline void RuntimeNodeSetExplicitlyRequested(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kExplicitlyRequested;
}


inline bool RuntimeNodeIsExplicitlyRequestedThroughUseDependency(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kExplicitlyRequestedThroughUseDependency);
}

inline void RuntimeNodeSetExplicitlyRequestedThroughUseDependency(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kExplicitlyRequestedThroughUseDependency;
}

inline void RuntimeNodeSetInputSignatureMightBeIncorrect(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kInputSignatureMightBeIncorrect;
}

inline bool RuntimeNodeGetInputSignatureMightBeIncorrect(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kInputSignatureMightBeIncorrect);
}
