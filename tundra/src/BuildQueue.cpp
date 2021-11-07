#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "Profiler.hpp"
#include "SignalHandler.hpp"
#include "SharedResources.hpp"
#include "NodeResultPrinting.hpp"
#include "RuntimeNode.hpp"
#include "BuildLoop.hpp"
#include "Driver.hpp"
#include "NodeResultPrinting.hpp"
#include "BinLogFormat.hpp"
#include <stdarg.h>
#include <algorithm>

#include <stdio.h>
#include "Banned.hpp"

using namespace BinLogFormat;

static void ThreadStateInit(ThreadState *self, BuildQueue *queue, size_t scratch_size, int thread_index)
{
    HeapInit(&self->m_LocalHeap);
    LinearAllocInit(&self->m_ScratchAlloc, &self->m_LocalHeap, scratch_size, "thread-local scratch");
    self->m_ThreadIndex = thread_index;
    self->m_Queue = queue;
    self->m_GlobCausingFrontendRerun = nullptr;
    self->m_FileCausingFrontendRerun = nullptr;
    BufferInitWithCapacity(&self->m_TimestampStorage, &self->m_LocalHeap, 100);
}

static void ThreadStateDestroy(ThreadState *self)
{
    LinearAllocDestroy(&self->m_ScratchAlloc, true);
    BufferDestroy(&self->m_TimestampStorage, &self->m_LocalHeap);
    HeapDestroy(&self->m_LocalHeap);
}

static ThreadRoutineReturnType TUNDRA_STDCALL BuildThreadRoutine(void *param)
{
    ThreadState *thread_state = static_cast<ThreadState *>(param);

    LinearAllocSetOwner(&thread_state->m_ScratchAlloc, ThreadCurrent());

    BuildLoop(thread_state);

    return 0;
}



void BuildQueueInit(BuildQueue *queue, const BuildQueueConfig *config, const char** targets, int target_count)
{
    ProfilerScope prof_scope("Tundra BuildQueueInit", 0);

    MutexInit(&queue->m_Lock);
    CondInit(&queue->m_WorkAvailable);
    CondInit(&queue->m_BuildFinishedConditionalVariable);
    MutexInit(&queue->m_BuildFinishedMutex);

    MutexLock(&queue->m_Lock);

    MemAllocHeap *heap = config->m_Heap;

    BufferInitWithCapacity(&queue->m_WorkStack, heap, 1024);
    BufferInitWithCapacity(&queue->m_QueueForNonGeneratedFileToEartlyStat, heap, 1024);

    HashSetInit(&queue->m_InputFilesAlreadyQueuedForEarlyStatting, heap);

    queue->m_Config = *config;
    queue->m_FinalBuildResult = BuildResult::kOk;
    queue->m_FinishedNodeCount = 0;
    queue->m_BuildFinishedConditionalVariableSignaled = false;
    queue->m_AmountOfNodesEverQueued = 0;
    queue->m_DagVerificationStatus = config->m_DriverOptions->m_DeferDagVerification
            ? VerificationStatus::WaitingForBuildProgramInputToBecomeAvailable
            : VerificationStatus::RequiredVerification;
    queue->m_SharedResourcesCreated = HeapAllocateArrayZeroed<uint32_t>(heap, config->m_SharedResourcesCount);
    MutexInit(&queue->m_SharedResourcesLock);

    BufferInitWithCapacity(&queue->m_Config.m_RequestedNodes, queue->m_Config.m_Heap, 32);
    DriverSelectNodes(queue->m_Config.m_Dag, targets, target_count, &queue->m_Config.m_RequestedNodes,  queue->m_Config.m_Heap);

    SignalHandlerSetCondition(&queue->m_BuildFinishedConditionalVariable);

    // Create build threads.
    for (int i = 0, thread_count = queue->m_Config.m_DriverOptions->m_ThreadCount; i < thread_count; ++i)
    {
        ThreadState *thread_state = &queue->m_ThreadState[i];

        //we give a thread id of i+1, so that we can reserve threadid 0 for the main thread.
        ThreadStateInit(thread_state, queue, MB(32), i+1);

        Log(kDebug, "starting build thread %d", i);
        queue->m_Threads[i] = ThreadStart(BuildThreadRoutine, thread_state, "Build Thread");
    }
}

void BuildQueueDestroy(BuildQueue *queue)
{
    Log(kDebug, "destroying build queue");
    const BuildQueueConfig *config = &queue->m_Config;

    for (int i = 0, thread_count = config->m_DriverOptions->m_ThreadCount; i < thread_count; ++i)
    {
        {
            ProfilerScope profile_scope("JoinBuildThread", 0);
            ThreadJoin(queue->m_Threads[i]);
        }
        ProfilerScope profile_scope("ThreadStateDestroy", 0);
        ThreadStateDestroy(&queue->m_ThreadState[i]);
    }


    {
        //It is an attractive optimization to destroy these resources before the threadjoins, so that the threadjoins take less time.
        //This turns out to be a dangerous optimization, as the BuildFinsished conditional variable gets signaled as soon as the buildresult
        //will guaranteed no longer change. There might still be in flight buildthreads finishing some work though. So we either need to ThreadJoin()
        //them all first (like we do now), or we need to refactor the signaling of buildfinished to only happen when the last buildthread exits.
        ProfilerScope profile_scope("SharedResourceDestroy", 0);
        // Destroy any shared resources that were created
        for (int i = 0; i < config->m_SharedResourcesCount; ++i)
            if (queue->m_SharedResourcesCreated[i] > 0)
                SharedResourceDestroy(queue, config->m_Heap, i);
    }

    // Output any deferred error messages.
    PrintDeferredMessages(queue);

    ProfilerScope profile_scope("BuildQueueDestroyTail", 0);
    MemAllocHeap *heap = queue->m_Config.m_Heap;
    BufferDestroy(&queue->m_Config.m_RequestedNodes, heap);

    // Deallocate storage.
    BufferDestroy(&queue->m_WorkStack, heap);
    BufferDestroy(&queue->m_QueueForNonGeneratedFileToEartlyStat, heap);

    HashSetDestroy(&queue->m_InputFilesAlreadyQueuedForEarlyStatting);

    HeapFree(heap, queue->m_SharedResourcesCreated);
    MutexDestroy(&queue->m_SharedResourcesLock);

    CondDestroy(&queue->m_WorkAvailable);

    MutexDestroy(&queue->m_Lock);
    MutexDestroy(&queue->m_BuildFinishedMutex);

    // Unblock all signals on the main thread.
    SignalHandlerSetCondition(nullptr);
}

BuildResult::Enum BuildQueueBuild(BuildQueue *queue, MemAllocLinear* scratch)
{    
    // Initialize build queue with index range to build
    RuntimeNode *runtime_nodes = queue->m_Config.m_RuntimeNodes;
    {
        ProfilerScope scope("EnqueueRequestedNodes",0);
        for (auto requestedNode:  queue->m_Config.m_RequestedNodes)
        {
            RuntimeNode *runtime_node = runtime_nodes + requestedNode;
            EnqueueNodeWithoutWakingAwaiters(queue, queue->m_Config.m_LinearAllocator, runtime_node, nullptr);
        }
    }

    {
        ProfilerScope scope("SortWorkingStack",0);
        SortWorkingStack(queue);
    }

    CondBroadcast(&queue->m_WorkAvailable);
    CondWait(&queue->m_BuildFinishedConditionalVariable, &queue->m_Lock);
    MutexUnlock(&queue->m_Lock);

    const char* signalReason = SignalGetReason();
    if (signalReason)
    {
        if (IsStructuredLogActive())
        {
            MemAllocLinearScope allocScope(scratch);

            JsonWriter msg;
            JsonWriteInit(&msg, scratch);
            JsonWriteStartObject(&msg);

            JsonWriteKeyName(&msg, "msg");
            JsonWriteValueString(&msg, "interrupted");

            JsonWriteKeyName(&msg, "reason");
            JsonWriteValueString(&msg, signalReason);

            JsonWriteEndObject(&msg);
            LogStructured(&msg);
        }
        return BuildResult::kInterrupted;
    }

    return queue->m_FinalBuildResult;
}

void BuildQueueGetFrontendRerunReason(BuildQueue* queue, char* out_frontend_rerun_reason)
{
    if (queue->m_FinalBuildResult != BuildResult::kRequireFrontendRerun)
        return;

    for (int i = 0; i < queue->m_Config.m_DriverOptions->m_ThreadCount; ++i)
    {
        ThreadState& thread_state = queue->m_ThreadState[i];

        if(thread_state.m_GlobCausingFrontendRerun != nullptr)
        {
            snprintf(out_frontend_rerun_reason, kRerunReasonBufferSize, "contents change of %s", thread_state.m_GlobCausingFrontendRerun->m_Path.Get());
            return;
        }

        if(thread_state.m_FileCausingFrontendRerun != nullptr)
        {
            snprintf(out_frontend_rerun_reason, kRerunReasonBufferSize, "timestamp change of %s", thread_state.m_FileCausingFrontendRerun->Get());
            return;
        }
    }

}
