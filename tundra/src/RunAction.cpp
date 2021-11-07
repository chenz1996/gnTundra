#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "RuntimeNode.hpp"
#include "Scanner.hpp"
#include "FileInfo.hpp"
#include "AllBuiltNodes.hpp"
#include "SignalHandler.hpp"
#include "Exec.hpp"
#include "Stats.hpp"
#include "StatCache.hpp"
#include "FileSign.hpp"
#include "Hash.hpp"
#include "Atomic.hpp"
#include "Profiler.hpp"
#include "NodeResultPrinting.hpp"
#include "OutputValidation.hpp"
#include "DigestCache.hpp"
#include "SharedResources.hpp"
#include "InputSignature.hpp"
#include "MakeDirectories.hpp"
#include "BuildLoop.hpp"
#include "RunAction.hpp"
#include "FileInfo.hpp"
#include "Actions.cpp"
#include "EventLog.hpp"
#include <stdarg.h>
#include <algorithm>
#include <stdio.h>
#if defined(TUNDRA_WIN32)
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#include "Banned.hpp"


static int SlowCallback(void *user_data)
{
    SlowCallbackData *data = (SlowCallbackData *)user_data;
    PrintNodeInProgress(data->node_data, data->time_of_start, data->build_queue);
    return 1;
}

static bool IsRunShellCommandAction(RuntimeNode* node)
{
    return (node->m_DagNode->m_FlagsAndActionType & Frozen::DagNode::kFlagActionTypeMask) == ActionType::kRunShellCommand;
}

static bool AllowUnwrittenOutputFiles(RuntimeNode* node)
{
    return node->m_DagNode->m_FlagsAndActionType & Frozen::DagNode::kFlagAllowUnwrittenOutputFiles;
}

static ExecResult RunActualAction(RuntimeNode* node, ThreadState* thread_state, Mutex* queue_lock, ValidationResult::Enum* out_validationresult)
{
    auto& node_data = node->m_DagNode;
    ActionType::Enum actionType = static_cast<ActionType::Enum>(node_data->m_FlagsAndActionType & Frozen::DagNode::kFlagActionTypeMask);
    switch(actionType)
    {
        case ActionType::kRunShellCommand:
        {
            // Repack frozen env to pointers on the stack.
            int env_count = node_data->m_EnvVars.GetCount();
            EnvVariable *env_vars = (EnvVariable *)alloca(env_count * sizeof(EnvVariable));
            for (int i = 0; i < env_count; ++i)
            {
                env_vars[i].m_Name = node_data->m_EnvVars[i].m_Name;
                env_vars[i].m_Value = node_data->m_EnvVars[i].m_Value;
            }

            SlowCallbackData slowCallbackData;
            slowCallbackData.node_data = node_data;
            slowCallbackData.time_of_start = TimerGet();
            slowCallbackData.build_queue = thread_state->m_Queue;

            // thread index 0 is reserved for the main thread, job ids are starting with the first worker at 1
            int job_id = thread_state->m_ThreadIndex - 1;
            auto result = ExecuteProcess(node_data->m_Action, env_count, env_vars, thread_state->m_Queue->m_Config.m_Heap, job_id, SlowCallback, &slowCallbackData);
            *out_validationresult = ValidateExecResultAgainstAllowedOutput(&result, node_data);
            return result;
        }
        case ActionType::kWriteTextFile:
        {
            *out_validationresult = ValidationResult::Pass;
            return WriteTextFile(node_data->m_WriteTextPayload, node_data->m_OutputFiles[0].m_Filename, thread_state->m_Queue->m_Config.m_Heap);
        }
        case ActionType::kCopyFiles:
        {
            *out_validationresult = ValidationResult::Pass;
            return CopyFiles(node_data->m_InputFiles.GetArray(), node_data->m_OutputFiles.GetArray(), node_data->m_InputFiles.GetCount(), thread_state->m_Queue->m_Config.m_StatCache, thread_state->m_Queue->m_Config.m_Heap);
        }
        case ActionType::kUnknown:
        default:
        {
            // Unknown action - fail with an appropriate error message
            *out_validationresult = ValidationResult::Pass;

            ExecResult result;
            char tmpBuffer[1024];
            InitOutputBuffer(&result.m_OutputBuffer, thread_state->m_Queue->m_Config.m_Heap);
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Unknown action type %d (%s)", actionType, ActionType::ToString(actionType));
            EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));
            result.m_ReturnCode = -1;

            return result;
        }
    }
}

void PostRunActionBookkeeping(RuntimeNode* node, ThreadState* thread_state)
{
    if (node->m_DagNode->m_OutputDirectories.GetCount() > 0)
    {
        node->m_DynamicallyDiscoveredOutputFiles = (DynamicallyGrowingCollectionOfPaths*) HeapAllocate(thread_state->m_Queue->m_Config.m_Heap, sizeof(DynamicallyGrowingCollectionOfPaths));
        node->m_DynamicallyDiscoveredOutputFiles->Initialize(thread_state->m_Queue->m_Config.m_Heap);
    }

    for(const auto& d: node->m_DagNode->m_OutputDirectories)
    {
        node->m_DynamicallyDiscoveredOutputFiles->AddFilesInDirectory(d.m_Filename.Get());
    }

    auto& digest_cache = thread_state->m_Queue->m_Config.m_DigestCache;
    auto& stat_cache = thread_state->m_Queue->m_Config.m_StatCache;
    for (const FrozenFileAndHash &output : node->m_DagNode->m_OutputFiles)
    {
        DigestCacheMarkDirty(digest_cache, output.m_Filename, output.m_FilenameHash);
        StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
    }
};

NodeBuildResult::Enum RunAction(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node, Mutex *queue_lock)
{
    CheckDoesNotHaveLock(&queue->m_Lock);

    MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

    const Frozen::DagNode *node_data = node->m_DagNode;

    const char *cmd_line = node_data->m_Action;

    if (IsRunShellCommandAction(node) && (!cmd_line || cmd_line[0] == '\0'))
        return NodeBuildResult::kRanSuccesfully;

    StatCache *stat_cache = queue->m_Config.m_StatCache;
    const char *annotation = node_data->m_Annotation;

    int profiler_thread_id = thread_state->m_ThreadIndex;
    bool echo_cmdline = 0 != (queue->m_Config.m_Flags & BuildQueueConfig::kFlagEchoCommandLines);

    auto FailWithPreparationError = [thread_state,node_data](const char* formatString, ...) -> NodeBuildResult::Enum
    {
        ExecResult result = {0, false};
        char buffer[2000];
        va_list args;
        va_start(args, formatString);
        vsnprintf(buffer, sizeof(buffer), formatString, args);
        va_end(args);

        result.m_ReturnCode = 1;

        InitOutputBuffer(&result.m_OutputBuffer, &thread_state->m_LocalHeap);
        result.m_FrozenNodeData = node_data;

        EmitOutputBytesToDestination(&result, buffer, strlen(buffer));

        PrintNodeResult(&result, node_data, "", thread_state->m_Queue, thread_state, false, TimerGet(), ValidationResult::Pass, nullptr, true);

        ExecResultFreeMemory(&result);

        return NodeBuildResult::kRanFailed;
    };

    EventLog::EmitNodeStart(node, thread_state->m_ThreadIndex);

    // See if we need to remove the output files before running anything.
    if (0 == (node_data->m_FlagsAndActionType & Frozen::DagNode::kFlagOverwriteOutputs))
    {
        for (const FrozenFileAndHash &output : node_data->m_OutputFiles)
        {
            Log(kDebug, "Removing output file %s before running action", output.m_Filename.Get());
            RemoveFileOrDir(output.m_Filename);
            StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
        }

        for (const FrozenFileAndHash &outputDir : node_data->m_OutputDirectories)
        {
            Log(kDebug, "Removing output directory %s before running action", outputDir.m_Filename.Get());

            FileInfo fileInfo = GetFileInfo(outputDir.m_Filename);
            if (fileInfo.IsDirectory())
            {
                StatCacheMarkDirty(stat_cache, outputDir.m_Filename, outputDir.m_FilenameHash);
                if (!DeleteDirectory(outputDir.m_Filename.Get()))
                    return FailWithPreparationError("Failed to remove directory %s as part of preparing to actually running this node",outputDir.m_Filename.Get());
            }
        }
    }

    auto EnsureParentDirExistsFor = [=](const FrozenFileAndHash &fileAndHash) -> bool {
        PathBuffer output;
        PathInit(&output, fileAndHash.m_Filename);

        if (!MakeDirectoriesForFile(stat_cache, output))
        {
            FailWithPreparationError("Failed to create output directory for targetfile %s as part of preparing to actually running this node",fileAndHash.m_Filename.Get());
            return false;
        }
        return true;
    };

    for (const FrozenFileAndHash &output_file : node_data->m_AuxOutputFiles)
        if (!EnsureParentDirExistsFor(output_file))
            return NodeBuildResult::kRanFailed;

    for (const FrozenFileAndHash &output_dir : node_data->m_OutputDirectories)
    {
        PathBuffer path;
        PathInit(&path, output_dir.m_Filename);
        if (!MakeDirectoriesRecursive(stat_cache, path))
            return NodeBuildResult::kRanFailed;
    }

    for (const FrozenFileAndHash &output_file : node_data->m_OutputFiles)
        if (!EnsureParentDirExistsFor(output_file))
            return NodeBuildResult::kRanFailed;



    size_t n_outputs = (size_t)node_data->m_OutputFiles.GetCount();

    bool *untouched_outputs = (bool *)LinearAllocate(&thread_state->m_ScratchAlloc, n_outputs, (size_t)sizeof(bool));
    memset(untouched_outputs, 0, n_outputs * sizeof(bool));

    auto passedOutputValidation = ValidationResult::Pass;

    for (int i = 0; i < node_data->m_SharedResources.GetCount(); ++i)
    {
        if (!SharedResourceAcquire(queue, &thread_state->m_LocalHeap, node_data->m_SharedResources[i]))
        {
            return FailWithPreparationError("failed to create shared resource %s", queue->m_Config.m_SharedResources[node_data->m_SharedResources[i]].m_Annotation.Get());
        }
    }

    Log(kSpam, "Launching process");
    TimingScope timing_scope(&g_Stats.m_ExecCount, &g_Stats.m_ExecTimeCycles);
    ProfilerScope prof_scope(annotation, profiler_thread_id);

    uint64_t *pre_timestamps = (uint64_t *)LinearAllocate(&thread_state->m_ScratchAlloc, n_outputs, (size_t)sizeof(uint64_t));

    if (!AllowUnwrittenOutputFiles(node))
    {
        uint64_t current_time = time(NULL);

        for (int i = 0; i < n_outputs; i++)
        {
            FileInfo info = GetFileInfo(node_data->m_OutputFiles[i].m_Filename);
            pre_timestamps[i] = info.m_Timestamp;

            if (info.m_Timestamp == current_time)
            {
                // This file has been created so recently that a very fast action might not
                // actually be recognised as modifying the timestamp on the file. To avoid
                // this, backdate the file by a second, so that any actual activity will definitely
                // cause the timestamp to change.
                struct utimbuf times;
                pre_timestamps[i] = times.actime = times.modtime = current_time - 1;
                utime(node_data->m_OutputFiles[i].m_Filename, &times);
            }
        }
    }

    uint64_t time_of_start = TimerGet();
    ExecResult result = RunActualAction(node, thread_state, queue_lock, &passedOutputValidation);

    if (passedOutputValidation == ValidationResult::Pass && !AllowUnwrittenOutputFiles(node))
    {
        for (int i = 0; i < n_outputs; i++)
        {
            FileInfo info = GetFileInfo(node_data->m_OutputFiles[i].m_Filename);
            bool untouched = pre_timestamps[i] == info.m_Timestamp;
            untouched_outputs[i] = untouched;
            if (untouched)
                passedOutputValidation = ValidationResult::UnwrittenOutputFileFail;
        }
    }

    PostRunActionBookkeeping(node, thread_state);

    
    //maybe consider changing this to use a dedicated lock for printing, instead of using the queuelock.
    if (EventLog::IsEnabled())
    {
        int duration_in_ms = TimerDiffSeconds(TimerGet(), time_of_start) * 1000;
        EventLog::EmitNodeFinish(node, node->m_CurrentInputSignature, result.m_ReturnCode, result.m_OutputBuffer.buffer, duration_in_ms, thread_state->m_ThreadIndex);    
    } 
    
    PrintNodeResult(&result, node_data, cmd_line, thread_state->m_Queue, thread_state, echo_cmdline, time_of_start, passedOutputValidation, untouched_outputs, false);
    
    ExecResultFreeMemory(&result);

    if (0 == result.m_ReturnCode && passedOutputValidation < ValidationResult::UnexpectedConsoleOutputFail)
        return NodeBuildResult::kRanSuccesfully;

    return NodeBuildResult::kRanFailed;
}

