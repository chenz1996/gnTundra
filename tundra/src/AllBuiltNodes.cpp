#include "AllBuiltNodes.hpp"
#include "Common.hpp"
#include "StatCache.hpp"
#include "Driver.hpp"
#include "BinaryWriter.hpp"
#include "Profiler.hpp"
#include "Stats.hpp"
#include "DagGenerator.hpp"
#include "RuntimeNode.hpp"
#include "LeafInputSignature.hpp"
#include "MakeDirectories.hpp"
#include "Driver.hpp"
#include "SortedArrayUtil.hpp"

#include "Banned.hpp"

bool OutputFilesMissingFor(const Frozen::BuiltNode* builtNode, StatCache *stat_cache, ThreadState* thread_state)
{
    ProfilerScope prof_scope("OutputFilesMissingFor", thread_state->m_ThreadIndex);

    for (const FrozenFileAndHash &f : builtNode->m_OutputFiles)
    {
        FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);

        if (!i.Exists())
            return true;
    }

    return false;
}

struct StateSavingSegments
{
    BinarySegment *main;
    BinarySegment *guid;
    BinarySegment *built_nodes;
    BinarySegment *array;
    BinarySegment *string;
};


bool NodeWasUsedByThisDagPreviously(const Frozen::BuiltNode *previously_built_node, uint32_t current_dag_identifier)
{
    auto &previous_dags = previously_built_node->m_DagsWeHaveSeenThisNodeInPreviously;
    return std::find(previous_dags.begin(), previous_dags.end(), current_dag_identifier) != previous_dags.end();
}

template <class TNodeType>
static void save_node_sharedcode(Frozen::BuiltNodeResult::Enum builtNodeResult, const HashDigest *input_signature, const HashDigest* leafinput_signature, const TNodeType *src_node, const HashDigest *guid, const StateSavingSegments &segments, const DynamicallyGrowingCollectionOfPaths* additionalDiscoveredOutputFiles, bool emitDataForBeeWhy)
{
    //we're writing to two arrays in one go.  the FrozenArray<HashDigest> m_NodeGuids and the FrozenArray<BuiltNode> m_BuiltNodes
    //the hashdigest is quick
    BinarySegmentWriteHashDigest(segments.guid, *guid);

    //the rest not so much
    BinarySegmentWriteInt32(segments.built_nodes, builtNodeResult);
    BinarySegmentWriteHashDigest(segments.built_nodes, *input_signature);
    BinarySegmentWriteHashDigest(segments.built_nodes, *leafinput_signature);

    auto WriteFrozenFileAndHashIntoBuiltNodesStream = [segments](const FrozenFileAndHash& f) -> void {
        BinarySegmentWritePointer(segments.array, BinarySegmentPosition(segments.string));
        BinarySegmentWriteStringData(segments.string, f.m_Filename.Get());
        BinarySegmentWriteInt32(segments.array, f.m_FilenameHash);
    };

    int32_t file_count = src_node->m_OutputFiles.GetCount();
    BinarySegmentWriteInt32(segments.built_nodes, file_count + (additionalDiscoveredOutputFiles == nullptr ? 0 : additionalDiscoveredOutputFiles->Count()));
    BinarySegmentWritePointer(segments.built_nodes, BinarySegmentPosition(segments.array));
    for (int32_t i = 0; i < file_count; ++i)
        WriteFrozenFileAndHashIntoBuiltNodesStream(src_node->m_OutputFiles[i]);

    if (additionalDiscoveredOutputFiles != nullptr)
    {
        int additionalOutputFilesCount = additionalDiscoveredOutputFiles->Count();
        for (int32_t i = 0; i < additionalOutputFilesCount; ++i)
        {
            BinarySegmentWritePointer(segments.array, BinarySegmentPosition(segments.string));
            const char* path = additionalDiscoveredOutputFiles->Get(i);
            BinarySegmentWriteStringData(segments.string, path);
            BinarySegmentWriteInt32(segments.array, Djb2Hash(path));
        }
    }

    file_count = src_node->m_AuxOutputFiles.GetCount();
    BinarySegmentWriteInt32(segments.built_nodes, file_count);
    BinarySegmentWritePointer(segments.built_nodes, BinarySegmentPosition(segments.array));
    for (int32_t i = 0; i < file_count; ++i)
        WriteFrozenFileAndHashIntoBuiltNodesStream(src_node->m_AuxOutputFiles[i]);

    if (src_node->m_Action && emitDataForBeeWhy)
    {
        BinarySegmentWritePointer(segments.built_nodes, BinarySegmentPosition(segments.string));
        BinarySegmentWriteStringData(segments.string, src_node->m_Action);
    }
    else
    {
        BinarySegmentWriteNullPointer(segments.built_nodes);
    }

}


bool SaveAllBuiltNodes(Driver *self)
{
    TimingScope timing_scope(nullptr, &g_Stats.m_StateSaveTimeCycles);
    ProfilerScope prof_scope("Tundra Write AllBuiltNodes", 0);

    MemAllocLinearScope alloc_scope(&self->m_Allocator);

    BinaryWriter writer;
    BinaryWriterInit(&writer, &self->m_Heap);

    StateSavingSegments segments;
    BinarySegment *main_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *guid_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *built_nodes_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *array_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *string_seg = BinaryWriterAddSegment(&writer);

    HashTable<CommonStringRecord, kFlagCaseSensitive> shared_strings;
    HashTableInit(&shared_strings, &self->m_Heap);

    segments.main = main_seg;
    segments.guid = guid_seg;
    segments.built_nodes = built_nodes_seg;
    segments.array = array_seg;
    segments.string = string_seg;

    BinaryLocator guid_ptr = BinarySegmentPosition(guid_seg);
    BinaryLocator built_nodes_ptr = BinarySegmentPosition(built_nodes_seg);

    uint32_t dag_node_count = self->m_DagData->m_NodeCount;
    const HashDigest *dag_node_guids = self->m_DagData->m_NodeGuids;
    const Frozen::DagNode *dag_nodes = self->m_DagData->m_DagNodes;
    RuntimeNode *runtime_nodes = self->m_RuntimeNodes.m_Storage;
    const size_t runtime_nodes_count = self->m_RuntimeNodes.m_Size;

    std::sort(runtime_nodes, runtime_nodes + runtime_nodes_count, [=](const RuntimeNode &l, const RuntimeNode &r) {
        // We know guids are sorted, so all we need to do is compare pointers into that table.
        return l.m_DagNode < r.m_DagNode;
    });

    const HashDigest *old_guids = nullptr;
    const Frozen::BuiltNode *old_state = nullptr;
    uint32_t previously_built_nodes_count = 0;

    if (const Frozen::AllBuiltNodes *all_built_nodes = self->m_AllBuiltNodes)
    {
        old_guids = all_built_nodes->m_NodeGuids;
        old_state = all_built_nodes->m_BuiltNodes;
        previously_built_nodes_count = all_built_nodes->m_NodeCount;
    }

    int emitted_built_nodes_count = 0;
    uint32_t this_dag_hashed_identifier = self->m_DagData->m_HashedIdentifier;

    // Collapse runtime state to persistent state
    auto BuiltNodeResultFor = [](const RuntimeNode* runtime_node) -> Frozen::BuiltNodeResult::Enum {
        switch (runtime_node->m_BuildResult)
        {
            case NodeBuildResult::kUpToDate:
            case NodeBuildResult::kRanSuccesfully:
            case NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun:
            case NodeBuildResult::kUpToDateButDependeesRequireFrontendRerun:
                return RuntimeNodeGetInputSignatureMightBeIncorrect(runtime_node)
                    ? Frozen::BuiltNodeResult::kRanSuccessfullyButInputSignatureMightBeIncorrect
                    : Frozen::BuiltNodeResult::kRanSuccessfullyWithGuaranteedCorrectInputSignature;
            case NodeBuildResult::kDidNotRun:
            case NodeBuildResult::kRanFailed:
                return Frozen::BuiltNodeResult::kRanFailed;
        }
        Croak("MSVC cannot see the switch statement above can never be left");
    };

    auto EmitBuiltNodeFromRuntimeNode = [=, &emitted_built_nodes_count, &shared_strings](const RuntimeNode* runtime_node, const HashDigest *guid) -> void {
        emitted_built_nodes_count++;
        MemAllocLinear *scratch = &self->m_Allocator;

        const Frozen::DagNode* dag_node = runtime_node->m_DagNode;

        HashDigest leafInputSignatureDigest = {};
        if (runtime_node->m_CurrentLeafInputSignature)
            leafInputSignatureDigest = runtime_node->m_CurrentLeafInputSignature->digest;

        save_node_sharedcode(BuiltNodeResultFor(runtime_node), &runtime_node->m_CurrentInputSignature, &leafInputSignatureDigest, runtime_node->m_DagNode, guid, segments, runtime_node->m_DynamicallyDiscoveredOutputFiles, self->m_DagData->m_EmitDataForBeeWhy);

        int32_t file_count = self->m_DagData->m_EmitDataForBeeWhy ? dag_node->m_InputFiles.GetCount() : 0;
        BinarySegmentWriteInt32(built_nodes_seg, file_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            uint64_t timestamp = 0;
            uint32_t filenameHash = dag_node->m_InputFiles[i].m_FilenameHash;
            const FrozenString& filename = dag_node->m_InputFiles[i].m_Filename;
            FileInfo fileInfo = StatCacheStat(&self->m_StatCache, filename, filenameHash);
            if (fileInfo.Exists())
                timestamp = fileInfo.m_Timestamp;

            BinarySegmentWriteUint64(array_seg, timestamp);
            BinarySegmentWriteUint32(array_seg, filenameHash);
            WriteCommonStringPtr(array_seg, string_seg, filename, &shared_strings, scratch);
        }

        if (dag_node->m_ScannerIndex != -1)
        {
            BinarySegmentWriteInt32(built_nodes_seg, runtime_node->m_ImplicitInputs.m_RecordCount);
            BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));

            HashSetWalk(&runtime_node->m_ImplicitInputs, [=, &shared_strings](uint32_t index, uint32_t hash, const char *filename) {
                uint64_t timestamp = 0;
                FileInfo fileInfo = StatCacheStat(&self->m_StatCache, filename, hash);
                if (fileInfo.Exists())
                    timestamp = fileInfo.m_Timestamp;

                BinarySegmentWriteUint64(array_seg, timestamp);
                BinarySegmentWriteUint32(array_seg, hash);
                WriteCommonStringPtr(array_seg, string_seg, filename, &shared_strings, scratch);
            });
        }
        else
        {
            BinarySegmentWriteInt32(built_nodes_seg, 0);
            BinarySegmentWriteNullPointer(built_nodes_seg);
        }

        const Frozen::BuiltNode* built_node = runtime_node->m_BuiltNode;
        //we cast the empty_frozen_array below here to a FrozenArray<uint32_t> that is empty, so the code below gets a lot simpler.
        const FrozenArray<uint32_t> &previous_dags = (built_node == nullptr) ? FrozenArray<uint32_t>::empty() : built_node->m_DagsWeHaveSeenThisNodeInPreviously;

        bool haveToAddOurselves = std::find(previous_dags.begin(), previous_dags.end(), this_dag_hashed_identifier) == previous_dags.end();

        BinarySegmentWriteUint32(built_nodes_seg, previous_dags.GetCount() + (haveToAddOurselves ? 1 : 0));
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (auto &identifier : previous_dags)
            BinarySegmentWriteUint32(array_seg, identifier);

        if (haveToAddOurselves)
            BinarySegmentWriteUint32(array_seg, this_dag_hashed_identifier);
    };

    auto EmitBuiltNodeFromPreviouslyBuiltNode = [=, &emitted_built_nodes_count, &shared_strings](const Frozen::BuiltNode *built_node, const HashDigest *guid, const HashDigest* leafInputSignature = nullptr) -> void {

        if (leafInputSignature == nullptr)
            leafInputSignature = &built_node->m_LeafInputSignature;
        save_node_sharedcode(built_node->m_Result, &built_node->m_InputSignature, leafInputSignature, built_node, guid, segments, nullptr, self->m_DagData->m_EmitDataForBeeWhy);
        emitted_built_nodes_count++;

        int32_t file_count = self->m_DagData->m_EmitDataForBeeWhy ? built_node->m_InputFiles.GetCount() : 0;
        BinarySegmentWriteInt32(built_nodes_seg, file_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            BinarySegmentWriteUint64(array_seg, built_node->m_InputFiles[i].m_Timestamp);
            BinarySegmentWriteUint32(array_seg, built_node->m_InputFiles[i].m_FilenameHash);
            WriteCommonStringPtr(array_seg, string_seg, built_node->m_InputFiles[i].m_Filename, &shared_strings, &self->m_Allocator);
        }

        file_count = built_node->m_ImplicitInputFiles.GetCount();
        BinarySegmentWriteInt32(built_nodes_seg, file_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            BinarySegmentWriteUint64(array_seg, built_node->m_ImplicitInputFiles[i].m_Timestamp);
            BinarySegmentWriteUint32(array_seg, built_node->m_ImplicitInputFiles[i].m_FilenameHash);
            WriteCommonStringPtr(array_seg, string_seg, built_node->m_ImplicitInputFiles[i].m_Filename, &shared_strings, &self->m_Allocator);
        }

        int32_t dag_count = built_node->m_DagsWeHaveSeenThisNodeInPreviously.GetCount();
        BinarySegmentWriteInt32(built_nodes_seg, dag_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        BinarySegmentWrite(array_seg, built_node->m_DagsWeHaveSeenThisNodeInPreviously.GetArray(), dag_count * sizeof(uint32_t));
    };

    auto EmitBuiltNodeFromBothRuntimeNodeAndPreviouslyBuiltNode = [=](const RuntimeNode* runtime_node, const Frozen::BuiltNode* built_node, const HashDigest* guid)
    {
        switch (runtime_node->m_BuildResult)
        {
            case NodeBuildResult::kUpToDate:
            case NodeBuildResult::kUpToDateButDependeesRequireFrontendRerun:
                break;
            case NodeBuildResult::kRanFailed:
            case NodeBuildResult::kRanSuccesfully:
            case NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun:
                //ok so the runtime node actually ran. In this case the previously built node is useless, and we should emit completely from the runtime node.
                return EmitBuiltNodeFromRuntimeNode(runtime_node, guid);
            default:
                Croak("Unexpected nodebuilt result %d",runtime_node->m_BuildResult);
        }

        //ok, so the runtime node did not run, but was up to date. This situation is why this code path exists, because in this situation
        //the data we want to write out needs to come partially from the previously built node: the m_OutputFiles, since they contain output
        //files found in the node's targetdirectories after the node has succesfully ran in the past.
        //But some other data needs to come from the runtime node: the leaf input signature. It's possible, and likely, for the leaf input signature
        //of a node to change, but its actual direct-input-signature to not change. If we did not take special care in this scenario, the leaf input signature
        //of the node right now, will always be different from the leafinputsignature stored in the buildstate, which will cause never ending cache-queries (and misses)
        //to occur. To solve that we need to write the new leafinput signature into the buildstate.

        EmitBuiltNodeFromPreviouslyBuiltNode(built_node, guid, &runtime_node->m_CurrentLeafInputSignature->digest);
    };

    auto RuntimeNodeGuidForRuntimeNodeIndex = [=](size_t index) -> const HashDigest * {
        int dag_index = int(runtime_nodes[index].m_DagNode - dag_nodes);
        return dag_node_guids + dag_index;
    };

    auto BuiltNodeGuidForBuiltNodeIndex = [=](size_t index) -> const HashDigest * {
        return old_guids + index;
    };

    auto IsRuntimeNodeValidForWritingToBuiltNodes = [=](const RuntimeNode* runtime_node) -> bool {
        switch(runtime_node->m_BuildResult)
        {
            case NodeBuildResult::kDidNotRun:
                return false;
            case NodeBuildResult::kUpToDateButDependeesRequireFrontendRerun:
            case NodeBuildResult::kUpToDate:
            case NodeBuildResult::kRanSuccesfully:
            case NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun:
            case NodeBuildResult::kRanFailed:
                return true;
        }
        Croak("Unexpected NodeBuildResult %d",runtime_node->m_BuildResult);
        return true;
    };

    auto IsPreviouslyBuiltNodeValidForWritingToBuiltNodes = [=](const Frozen::BuiltNode* built_node, const HashDigest* guid) -> bool {
        // Make sure this node is still relevant before saving.
        bool node_is_in_dag = BinarySearch(dag_node_guids, dag_node_count, *guid) != nullptr;

        if (node_is_in_dag)
            return true;

        if (!NodeWasUsedByThisDagPreviously(built_node, this_dag_hashed_identifier))
            return true;

        for (auto& outputfile : built_node->m_OutputFiles)
        {
            // We want to make sure we keep all nodes that have at some point written files to disk which are still present
            if (StatCacheStat(&self->m_StatCache, outputfile.m_Filename.Get(), outputfile.m_FilenameHash).Exists())
                return true;
        }
        return false;
    };

    {
        size_t runtime_nodes_iterator = 0, previously_built_nodes_iterator = 0;

        //we have an array of RuntimeNodes,  and an array of BuiltNodes. We're going to write most of them
        //to a new frozen AllBuiltinNodes file that will be the next builds' BuiltNodes. We cannot just write out both though:
        //
        //most RuntimeNodes will have a matching pair in BuiltNodes, if the node had been built before. In this case, we want to write out the new RuntimeNode.
        //the RuntimeNode might not have actually executed. This case we want to preserve the old version in BuiltNodes if it existed.
        //
        //We need  to maintain the invariant that BuiltNodes is sorted by the built node's guid. In order to do that, we're going to walk both input arrays at the same time.
        //Both input arrays are also guaranteed to be already sorted, so we just need to make sure we interleave both arrays according to guid sort order.
        while (runtime_nodes_iterator < runtime_nodes_count && previously_built_nodes_iterator < previously_built_nodes_count)
        {
            //future optimization:  avoid doing the Is***ValidForWriting() checks this early, and instead do them after the GUID comparison.
            RuntimeNode* first_runtimenode_in_line = &runtime_nodes[runtime_nodes_iterator];
            if (!IsRuntimeNodeValidForWritingToBuiltNodes(first_runtimenode_in_line))
            {
                //this runtime node didn't run, let's skip over it.
                runtime_nodes_iterator++;
                continue;
            }

            const HashDigest * runtime_node_guid = RuntimeNodeGuidForRuntimeNodeIndex(runtime_nodes_iterator);
            const HashDigest * previously_built_guid = BuiltNodeGuidForBuiltNodeIndex(previously_built_nodes_iterator);
            const Frozen::BuiltNode* previously_built_node = &old_state[previously_built_nodes_iterator];

            if (!IsPreviouslyBuiltNodeValidForWritingToBuiltNodes(previously_built_node, previously_built_guid))
            {
                previously_built_nodes_iterator++;
                continue;
            }

            //ok, now we have a runtimenode that did actually run, and a previously built node that is still valid for this dag. let's figure out
            //which one we should write out first.

            int compare = CompareHashDigests(*runtime_node_guid, *previously_built_guid);

            if (compare > 0)
            {
                //for this one, we only have a previously built node. let's write it out.
                EmitBuiltNodeFromPreviouslyBuiltNode(previously_built_node, previously_built_guid);

                previously_built_nodes_iterator++;
            }
            else if (compare < 0)
            {
                //for this one, we only have a runtime node, let's write it out.
                EmitBuiltNodeFromRuntimeNode(first_runtimenode_in_line, runtime_node_guid);
                runtime_nodes_iterator++;
            }
            else
            {
                //for this one, we have both a previously built node, and a runtime node. We have a special codepath for that
                EmitBuiltNodeFromBothRuntimeNodeAndPreviouslyBuiltNode(first_runtimenode_in_line, previously_built_node, runtime_node_guid);
                runtime_nodes_iterator++;
                previously_built_nodes_iterator++;
            }
        }

        //ok, one of the arrays have been drained, so we can just blindly process the other one
        for( ; runtime_nodes_iterator < runtime_nodes_count; runtime_nodes_iterator++)
        {
            RuntimeNode* first_runtimenode_in_line = &runtime_nodes[runtime_nodes_iterator];
            if (!IsRuntimeNodeValidForWritingToBuiltNodes(first_runtimenode_in_line))
            {
                //this runtime node didn't run, let's skip over it.
                continue;
            }

            const HashDigest * runtime_node_guid = RuntimeNodeGuidForRuntimeNodeIndex(runtime_nodes_iterator);
            EmitBuiltNodeFromRuntimeNode(first_runtimenode_in_line, runtime_node_guid);
        }

        for ( ; previously_built_nodes_iterator < previously_built_nodes_count; previously_built_nodes_iterator++)
        {
            const Frozen::BuiltNode* previously_built_node = &old_state[previously_built_nodes_iterator];
            const HashDigest * previously_built_guid = BuiltNodeGuidForBuiltNodeIndex(previously_built_nodes_iterator);

            if (IsPreviouslyBuiltNodeValidForWritingToBuiltNodes(previously_built_node, previously_built_guid))
                EmitBuiltNodeFromPreviouslyBuiltNode(previously_built_node,previously_built_guid);
        }
    }

    // Complete main data structure.
    BinarySegmentWriteUint32(main_seg, Frozen::AllBuiltNodes::MagicNumber);
    BinarySegmentWriteInt32(main_seg, emitted_built_nodes_count);
    BinarySegmentWritePointer(main_seg, guid_ptr);
    BinarySegmentWritePointer(main_seg, built_nodes_ptr);
    BinarySegmentWriteUint32(main_seg, Frozen::AllBuiltNodes::MagicNumber);

    // Unmap old state data.
    MmapFileUnmap(&self->m_StateFile);
    self->m_AllBuiltNodes = nullptr;

    bool success = true;

    // Ensure that the target directories exist.
    PathBuffer path;
    PathInit(&path, self->m_DagData->m_StateFileName.Get());
    if (!MakeDirectoriesForFile(&self->m_StatCache, path))
    {
        Log(kError, "Failed to create directories for \"%s\"", path);
        success = false;
    }
    PathInit(&path, self->m_DagData->m_StateFileNameTmp.Get());
    if (!MakeDirectoriesForFile(&self->m_StatCache, path))
    {
        Log(kError, "Failed to create directories for \"%s\"", path);
        success = false;
    }

    if (!BinaryWriterFlush(&writer, self->m_DagData->m_StateFileNameTmp))
    {
        // BinaryWriterFlush logs its own errors, don't bother doing so here.
        success = false;
    }

    if (success)
    {
        // Commit atomically with a file rename.
        success = RenameFile(self->m_DagData->m_StateFileNameTmp, self->m_DagData->m_StateFileName);
        if (!success)
        {
            Log(kError, "Failed to rename \"%s\" to \"%s\"",
                self->m_DagData->m_StateFileNameTmp.Get(),
                self->m_DagData->m_StateFileName.Get());
        }
    }
    else
    {
        RemoveFileOrDir(self->m_DagData->m_StateFileNameTmp);
    }

    HashTableDestroy(&shared_strings);

    BinaryWriterDestroy(&writer);

    return success;
}
