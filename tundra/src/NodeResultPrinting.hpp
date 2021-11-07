#pragma once

#include <ctime>
#include "OutputValidation.hpp"
#include <stdint.h>

struct ExecResult;
namespace Frozen { struct DagNode; };
struct BuildQueue;
struct ThreadState;
struct DriverOptions;
struct RuntimeNode;

namespace MessageStatusLevel
{
enum Enum
{
    Success = 0,
    Failure = 1,
    Warning = 2,
    Info = 3,
};
}


void PrintMessage(MessageStatusLevel::Enum status_level, const char* message, ...);
void PrintMessage(MessageStatusLevel::Enum status_level, int duration, const char* message, ...);
void PrintMessage(MessageStatusLevel::Enum status_level, int duration, ExecResult *result, const char *message, ...);
void PrintCacheHit(BuildQueue* queue, ThreadState *thread_state, double duration, RuntimeNode* node);
void PrintCacheMissIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node);
void EmitColorForLevel(MessageStatusLevel::Enum status_level);
void EmitColorReset();
void InitNodeResultPrinting(const DriverOptions* driverOptions);
void DestroyNodeResultPrinting();

void PrintNodeResult(
    ExecResult *result,
    const Frozen::DagNode *node_data,
    const char *cmd_line,
    BuildQueue *queue,
    ThreadState *thread_state,
    bool always_verbose,
    uint64_t time_exec_started,
    ValidationResult::Enum validationResult,
    const bool *untouched_outputs,
    bool was_preparation_error);
void PrintNodeInProgress(const Frozen::DagNode *node_data, uint64_t time_of_start, const BuildQueue *queue, const char* message = nullptr);
void PrintDeferredMessages(BuildQueue *queue);
void PrintServiceMessage(MessageStatusLevel::Enum statusLevel, const char *formatString, ...);
void StripAnsiColors(char *buffer);

