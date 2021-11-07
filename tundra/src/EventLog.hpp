#pragma once
#include "Hash.hpp"
#include "BuildQueue.hpp"
#include "BinLogFormat.hpp"

struct RuntimeNode;

namespace EventLog
{
    void Init(const char* path);
    void Destroy();

    bool IsEnabled();
    void EmitBuildStart(const char* dag_filename, int max_node_count, int highest_thread_id);
    void EmitBuildFinish(BuildResult::Enum buildResult);
    void EmitNodeUpToDate(RuntimeNode* node);
    void EmitNodeStart(RuntimeNode* node, int thread_index);
    void EmitNodeFinish(RuntimeNode* node, HashDigest inputSignature, int exitcode, const char* output, int duration_in_ms, int thread_index);
    void EmitFirstTimeEnqueue(RuntimeNode* queued_node, RuntimeNode* enqueueing_node);
}