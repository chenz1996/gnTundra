#pragma once

#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "MemoryMappedFile.hpp"
#include "RuntimeNode.hpp"
#include "BuildQueue.hpp"
#include "Buffer.hpp"
#include "ScanCache.hpp"
#include "StatCache.hpp"
#include "DigestCache.hpp"


namespace Frozen {
    struct Dag;
    struct ScanData;
    struct AllBuiltNodes;
}


struct DriverOptions
{
    bool m_ShowHelp;
    bool m_ShowTargets;
    bool m_DebugMessages;
    bool m_Verbose;
    bool m_SpammyVerbose;
    bool m_DisplayStats;
    bool m_SilenceIfPossible;
    bool m_DontReusePreviousResults;
    bool m_DebugSigning;
    bool m_ContinueOnFailure;
    bool m_StandardInputCanary;
    bool m_DeferDagVerification;
    bool m_Inspect;
    bool m_DontPrintNodeResultsToStdout;
    int m_IdentificationColor;
    int m_VisualMaxNodes;
#if defined(TUNDRA_WIN32)
    bool m_RunUnprotected;
#endif
    int m_ThreadCount;
    const char *m_WorkingDir;
    const char *m_DAGFileName;
    const char* m_DagFileNameJson;
    const char *m_ProfileOutput;
    const char *m_IncludesOutput;
    const char *m_JustPrintLeafInputSignature;
    const char* m_BinLog;
};

void DriverOptionsInit(DriverOptions *self);

struct Driver
{
    MemAllocHeap m_Heap;
    MemAllocLinear m_Allocator;

    // Read-only memory mapped data - DAG data
    MemoryMappedFile m_DagFile;

    // Read-only memory mapped data - DAG data
    MemoryMappedFile m_DagDerivedFile;

    // Read-only memory mapped data - previous build state
    MemoryMappedFile m_StateFile;

    // Read-only memory mapped data - header scanning cache
    MemoryMappedFile m_ScanFile;

    // Stores pointers to mmaped data.
    const Frozen::Dag *m_DagData;
    const Frozen::DagDerived *m_DagDerivedData;
    const Frozen::AllBuiltNodes *m_AllBuiltNodes;
    const Frozen::ScanData *m_ScanData;

    DriverOptions m_Options;

    // Space for dynamic DAG node state
    Buffer<RuntimeNode> m_RuntimeNodes;

    MemAllocLinear m_ScanCacheAllocator;
    ScanCache m_ScanCache;

    MemAllocLinear m_StatCacheAllocator;
    StatCache m_StatCache;

    DigestCache m_DigestCache;
};

bool DriverInit(Driver *self, const DriverOptions *options);

bool DriverPrepareNodes(Driver *self, const char **targets, int target_count);

void DriverDestroy(Driver *self);

void DriverShowHelp(Driver *self);

void DriverShowTargets(Driver *self);
bool DriverReportIncludes(Driver *self);

void DriverReportStartup(Driver *self, const char **targets, int target_count);

void DriverRemoveStaleOutputs(Driver *self);

void DriverCleanOutputs(Driver *self);

BuildResult::Enum DriverBuild(Driver *self, int* out_finished_node_count, char* out_frontend_rerun_reason, const char** argv, int argc);

bool DriverInitData(Driver *self);

bool DriverSaveScanCache(Driver *self);
bool DriverSaveDigestCache(Driver *self);

void DriverInitializeTundraFilePaths(DriverOptions *driverOptions);
void DriverSelectNodes(const Frozen::Dag *dag, const char **targets, int target_count, Buffer<int32_t> *out_nodes, MemAllocHeap *heap);
