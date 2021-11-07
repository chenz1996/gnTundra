#pragma once

#include "Common.hpp"
#include "Mutex.hpp"
#include "ConditionVar.hpp"
#include "Thread.hpp"
#include "MemAllocLinear.hpp"
#include "MemAllocHeap.hpp"
#include "JsonWriter.hpp"
#include "DagData.hpp"
#include "BuildLoop.hpp"
#include "Buffer.hpp"
#include "BinLogFormat.hpp"

struct MemAllocHeap;
struct RuntimeNode;
struct ScanCache;
struct StatCache;
struct DigestCache;
struct DriverOptions;

enum
{
    kMaxBuildThreads = 128
};

struct BuildQueueConfig
{
    enum
    {
        // Print command lines to the TTY as actions are executed.
        kFlagEchoCommandLines = 1 << 0,
    };

    const DriverOptions* m_DriverOptions;
    uint32_t m_Flags;
    MemAllocHeap *m_Heap;
    MemAllocLinear *m_LinearAllocator;
    const Frozen::Dag* m_Dag;
    const Frozen::DagNode *m_DagNodes;
    const Frozen::DagDerived* m_DagDerived;
    DagRuntimeData m_DagRuntimeData;
    RuntimeNode *m_RuntimeNodes;
    int m_TotalRuntimeNodeCount;
    Buffer<int32_t> m_RequestedNodes;
    ScanCache *m_ScanCache;
    StatCache *m_StatCache;
    DigestCache *m_DigestCache;
    int m_ShaDigestExtensionCount;
    const uint32_t *m_ShaDigestExtensions;
    void *m_FileSigningLog;
    Mutex *m_FileSigningLogMutex;
    const Frozen::SharedResourceData *m_SharedResources;
    int m_SharedResourcesCount;
    bool m_AttemptCacheReads;
    bool m_AttemptCacheWrites;
};

struct BuildQueue;

struct ThreadState
{
    MemAllocHeap m_LocalHeap;
    MemAllocLinear m_ScratchAlloc;
    int m_ThreadIndex; // Global thread index, main thread is 0, workers starting at 1.
    BuildQueue *m_Queue;
    Buffer<uint64_t> m_TimestampStorage;

    // For tracking which invalidated glob/file signature is causing a frontend rerun to be required
    // Only storing one of each type is sufficient for figuring out what message to give the user
    const Frozen::DagGlobSignature *m_GlobCausingFrontendRerun;
    const FrozenString *m_FileCausingFrontendRerun;
};

namespace VerificationStatus
{
    enum Enum
    {
        WaitingForBuildProgramInputToBecomeAvailable,
        RequiredVerification,
        BeingVerified,
        Passed,
        Failed
    };
};

struct BuildQueue
{
    Mutex m_Lock;
    ConditionVariable m_WorkAvailable;
    ConditionVariable m_BuildFinishedConditionalVariable;
    Mutex m_BuildFinishedMutex;
    bool m_BuildFinishedConditionalVariableSignaled;

    VerificationStatus::Enum m_DagVerificationStatus;

    Buffer<int32_t> m_WorkStack;
    Buffer<const FrozenFileAndHash*> m_QueueForNonGeneratedFileToEartlyStat;
    HashSet<kFlagCaseSensitive> m_InputFilesAlreadyQueuedForEarlyStatting;

    BuildQueueConfig m_Config;

    BuildResult::Enum m_FinalBuildResult;
    uint32_t m_FinishedNodeCount;
    uint32_t m_AmountOfNodesEverQueued;

    ThreadId m_Threads[kMaxBuildThreads];
    ThreadState m_ThreadState[kMaxBuildThreads];
    uint32_t *m_SharedResourcesCreated;
    Mutex m_SharedResourcesLock;
};

void BuildQueueInit(BuildQueue *queue, const BuildQueueConfig *config, const char** targets, int target_count);

BuildResult::Enum BuildQueueBuild(BuildQueue *queue, MemAllocLinear* scratch);

void BuildQueueDestroy(BuildQueue *queue);

bool HasBuildStoppingFailures(const BuildQueue *queue);

static const int kRerunReasonBufferSize = kMaxPathLength + 128;
void BuildQueueGetFrontendRerunReason(BuildQueue* queue, char* out_frontend_rerun_reason);
