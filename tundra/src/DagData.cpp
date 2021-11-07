#include "DagData.hpp"
#include "Buffer.hpp"
#include "HashTable.hpp"
#include "FileInfo.hpp"
#include "FileInfoHelper.hpp"
#include "FileSign.hpp"
#include "Banned.hpp"


void FindDependentNodesFromRootIndex_IncludingSelf_NotRecursingIntoCacheableNodes(MemAllocHeap* heap, const Frozen::Dag* dag, const Frozen::DagNode& dagNode, Buffer<int32_t>& results, Buffer<int32_t>* dependenciesThatAreCacheableThemselves)
{
    Buffer<int32_t> node_stack;
    BufferInitWithCapacity(&node_stack, heap, 1024);

    const size_t node_word_count = (dag->m_NodeCount + 31) / 32;
    uint32_t *node_visited_bits = HeapAllocateArrayZeroed<uint32_t>(heap, node_word_count);

    auto AddToResultsAndNodeStackIfNotYetAdded = [&](int dag_index, bool isRootSearchNode = false)
    {
        auto& dagNode = dag->m_DagNodes[dag_index];

        const int dag_word = dag_index / 32;
        const int dag_bit = 1 << (dag_index & 31);
        if (0 == (node_visited_bits[dag_word] & dag_bit))
        {
            if ((dagNode.m_FlagsAndActionType & Frozen::DagNode::kFlagCacheableByLeafInputs) && !isRootSearchNode)
            {
                if (dependenciesThatAreCacheableThemselves)
                    BufferAppendOne(dependenciesThatAreCacheableThemselves, heap, dag_index);
                return;
            }

            node_visited_bits[dag_word] |= dag_bit;
            BufferAppendOne(&results, heap, dag_index);
            BufferAppendOne(&node_stack, heap, dag_index);
        }
    };

    AddToResultsAndNodeStackIfNotYetAdded(dagNode.m_DagNodeIndex, true);

    AddToResultsAndNodeStackIfNotYetAdded(dagNode.m_DagNodeIndex);
    if (dependenciesThatAreCacheableThemselves)
        BufferClear(dependenciesThatAreCacheableThemselves);

    while (node_stack.m_Size > 0)
    {
        int dag_index = BufferPopOne(&node_stack);

        for (auto buildDependency: dag->m_DagNodes[dag_index].m_ToBuildDependencies)
            AddToResultsAndNodeStackIfNotYetAdded(buildDependency);
        for (auto useDependency: dag->m_DagNodes[dag_index].m_ToUseDependencies)
            AddToResultsAndNodeStackIfNotYetAdded(useDependency);
    }

    HeapFree(heap, node_visited_bits);
    BufferDestroy(&node_stack, heap);
}

void DagRuntimeDataInit(DagRuntimeData* data, const Frozen::Dag* dag, MemAllocHeap *heap)
{
    HashTableInit(&data->m_OutputsToDagNodes, heap);
    HashTableInit(&data->m_OutputDirectoriesToDagNodes, heap);
    for (int i = 0; i<dag->m_NodeCount; i++)
    {
        const Frozen::DagNode* node = dag->m_DagNodes + i;
        for(auto &output: node->m_OutputFiles)
            HashTableInsert(&data->m_OutputsToDagNodes, output.m_FilenameHash, output.m_Filename.Get(), i);
        for(auto &output: node->m_OutputDirectories)
            HashTableInsert(&data->m_OutputDirectoriesToDagNodes, output.m_FilenameHash, output.m_Filename.Get(), i);
    }

    // We currently don't populate m_OutputDirectories for all nodes.
    // Some output directries still only show up in m_DirectoriesCausingImplicitDependencies.
    // For those, we don't know which node they came from, so we use the special index value of -1.
    for (auto& d: dag->m_DirectoriesCausingImplicitDependencies)
        HashTableInsert(&data->m_OutputDirectoriesToDagNodes, d.m_FilenameHash, d.m_Filename.Get(), -1);

    data->m_Dag = dag;
}

void DagRuntimeDataDestroy(DagRuntimeData* data)
{
    HashTableDestroy(&data->m_OutputsToDagNodes);
    HashTableDestroy(&data->m_OutputDirectoriesToDagNodes);
}

bool FindDagNodeForFile(const DagRuntimeData* data, uint32_t filenameHash, const char* filename, const Frozen::DagNode **result)
{
    if (int* nodeIndex = HashTableLookup(&data->m_OutputsToDagNodes, filenameHash, filename))
    {
        *result = data->m_Dag->m_DagNodes + *nodeIndex;
        return true;
    }

    PathBuffer filePath;
    PathInit(&filePath, filename);

    while (PathStripLast(&filePath))
    {
        char path[kMaxPathLength];
        PathFormat(path, &filePath);
        if (int* nodeIndex = HashTableLookup(&data->m_OutputDirectoriesToDagNodes, Djb2HashPath(path), path))
        {
            if (*nodeIndex == -1)
                *result = nullptr;
            else
                *result = data->m_Dag->m_DagNodes + *nodeIndex;
            return true;
        }
    }
    return false;
}

bool IsFileGenerated(const DagRuntimeData* data, uint32_t filenameHash, const char* filename)
{
    const Frozen::DagNode *dummy;
    return FindDagNodeForFile(data, filenameHash, filename, &dummy);
}

bool CheckDagSignatures(const Frozen::Dag* dag_data, MemAllocHeap* heap, MemAllocLinear* scratch, char *out_of_date_reason, int out_of_date_reason_maxlength)
{
#if ENABLED(CHECKED_BUILD)
    // Paranoia - make sure the data is sorted.
    for (int i = 1, count = dag_data->m_NodeCount; i < count; ++i)
    {
        if (dag_data->m_NodeGuids[i] < dag_data->m_NodeGuids[i - 1])
            Croak("DAG data is not sorted by guid");
    }
#endif

    Log(kDebug, "checking file signatures for DAG data");

    // Check timestamps of frontend files used to produce the DAG
    for (const Frozen::DagFileSignature &sig : dag_data->m_FileSignatures)
    {
        const char *path = sig.m_Path;

        uint64_t timestamp = sig.m_Timestamp;
        FileInfo info = GetFileInfo(path);

        if (info.m_Timestamp != timestamp)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "FileSignature timestamp changed: %s", sig.m_Path.Get());
            return false;
        }
    }

    for (const Frozen::DagStatSignature &sig : dag_data->m_StatSignatures)
    {
        const char *path = sig.m_Path;
        FileInfo info = GetFileInfo(path);

        if (GetStatSignatureStatusFor(info) != sig.m_StatResult)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "StatSignature changed: %s", sig.m_Path.Get());
            return false;
        }
    }


    // Check directory listing fingerprints
    // Note that the digest computation in here must match the one in LuaListDirectory
    // The digests computed there are stored in the signature block by frontend code.
    for (const Frozen::DagGlobSignature &sig : dag_data->m_GlobSignatures)
    {
        HashDigest digest = CalculateGlobSignatureFor(sig.m_Path, sig.m_Filter, sig.m_Recurse, heap, scratch);

        // Compare digest with the one stored in the signature block
        if (0 != memcmp(&digest, &sig.m_Digest, sizeof digest))
        {
            char stored[kDigestStringSize], actual[kDigestStringSize];
            DigestToString(stored, sig.m_Digest);
            DigestToString(actual, digest);
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "directory contents changed: %s", sig.m_Path.Get());
            Log(kInfo, "DAG out of date: file glob change for %s (%s => %s)", sig.m_Path.Get(), stored, actual);
            return false;
        }
    }

    for (const Frozen::DagEnvironmentVariableSignature& sig: dag_data->m_EnvironmentVariableSignatures)
    {
        const char* currentValue = getenv(sig.m_VariableName.Get());
        if (strcmp(currentValue, sig.m_Value.Get()) != 0)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "Environment variable '%s' changed from '%s' to '%s'", sig.m_VariableName.Get(), sig.m_Value.Get(), currentValue);
            return false;
        }
    }

    return true;
}