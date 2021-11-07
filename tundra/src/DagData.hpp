#pragma once

#include "Common.hpp"
#include "BinaryData.hpp"
#include "Hash.hpp"
#include "PathUtil.hpp"
#include "Buffer.hpp"
#include "HashTable.hpp"
#include <functional>

namespace Frozen
{
namespace ScannerType
{
enum Enum
{
    kCpp = 0,
    kGeneric = 1
};
}

struct ScannerData
{
    FrozenEnum<ScannerType::Enum, int32_t> m_ScannerType;
    FrozenArray<FrozenString> m_IncludePaths;
    HashDigest m_ScannerGuid;
};

struct KeywordData
{
    FrozenString m_String;
    int16_t m_StringLength;
    int8_t m_ShouldFollow;
    int8_t m_Padding;
};

struct GenericScannerData : ScannerData
{
    enum
    {
        kFlagRequireWhitespace = 1 << 0,
        kFlagUseSeparators = 1 << 1,
        kFlagBareMeansSystem = 1 << 2
    };

    uint32_t m_Flags;
    FrozenArray<KeywordData> m_Keywords;
};

struct NamedNodeData
{
    FrozenString m_Name;
    int32_t m_NodeIndex;
};

struct DagFileSignature
{
    FrozenString m_Path;
    uint8_t m_Padding[4];
    uint64_t m_Timestamp;
};

struct DagEnvironmentVariableSignature
{
    FrozenString m_VariableName;
    FrozenString m_Value;
};

struct DagStatSignature
{
    enum Enum
    {
        File,
        Directory,
        DoesNotExist
    };

    FrozenString m_Path;
    uint32_t m_StatResult;
};

static_assert(offsetof(DagFileSignature, m_Timestamp) == 8, "struct layout");
static_assert(sizeof(DagFileSignature) == 16, "struct layout");

struct DagGlobSignature
{
    FrozenString m_Path;
    FrozenString m_Filter;
    HashDigest m_Digest;
    uint32_t m_Recurse;
};
static_assert(sizeof(HashDigest) + sizeof(FrozenString) + sizeof(FrozenString) + sizeof(uint32_t) == sizeof(DagGlobSignature), "struct layout");

struct EnvVarData
{
    FrozenString m_Name;
    FrozenString m_Value;
};

struct DagNode
{
    enum
    {
        // Bottom 8 bits are reserved for the action type
        kFlagActionTypeMask = (1 << 8) - 1,

        // Set in m_Flags if it is safe to overwrite the output files in place.  If
        // this flag is not present, the build system will remove the output files
        // before running the action. This is useful to prevent tools that
        // sometimes misbehave in the presence of old output files. ar is a good
        // example.
        kFlagOverwriteOutputs = 1 << 8,

        //if not set, we fail the build when a command prints anything unexpected to stdout or stderr
        kFlagAllowUnexpectedOutput = 1 << 10,

        kFlagAllowUnwrittenOutputFiles = 1 << 11,
        kFlagBanContentDigestForInputs = 1 << 12,

        kFlagCacheableByLeafInputs = 1 << 13
    };

    union {
        FrozenString m_Action;
        FrozenString m_WriteTextPayload;
    };
    FrozenString m_Annotation;
    FrozenString m_ProfilerOutput;
    FrozenArray<int32_t> m_ToBuildDependencies;
    FrozenArray<int32_t> m_ToUseDependencies;
    FrozenArray<FrozenFileAndHash> m_InputFiles;
    FrozenArray<FrozenFileAndHash> m_FilesThatMightBeIncluded;
    FrozenArray<FrozenFileAndHash> m_OutputFiles;
    FrozenArray<FrozenFileAndHash> m_OutputDirectories;
    FrozenArray<FrozenFileAndHash> m_AuxOutputFiles;
    FrozenArray<FrozenFileAndHash> m_FrontendResponseFiles;
    FrozenArray<FrozenString> m_AllowedOutputSubstrings;
    FrozenArray<EnvVarData> m_EnvVars;

    int32_t m_ScannerIndex;

    FrozenArray<int32_t> m_SharedResources;
    FrozenArray<DagFileSignature> m_FileSignatures;
    FrozenArray<DagStatSignature> m_StatSignatures;
    FrozenArray<DagGlobSignature> m_GlobSignatures;
    FrozenArray<FrozenFileAndHash> m_CachingInputIgnoreList;
    uint32_t m_FlagsAndActionType;
    uint32_t m_OriginalIndex;
    uint32_t m_DagNodeIndex;
};


struct SharedResourceData
{
    FrozenString m_Annotation;
    FrozenString m_CreateAction;
    FrozenString m_DestroyAction;
    FrozenArray<EnvVarData> m_EnvVars;
};

struct Dag
{
    static const uint32_t MagicNumber = 0x29a22148 ^ kTundraHashMagic;

    uint32_t m_MagicNumber;

    uint32_t m_HashedIdentifier;

    int32_t m_NodeCount;
    FrozenPtr<HashDigest> m_NodeGuids;
    FrozenPtr<DagNode> m_DagNodes;

    FrozenArray<NamedNodeData> m_NamedNodes;
    FrozenArray<int32_t> m_DefaultNodes;

    FrozenArray<SharedResourceData> m_SharedResources;

    FrozenArray<DagFileSignature> m_FileSignatures;
    FrozenArray<DagStatSignature> m_StatSignatures;
    FrozenArray<DagGlobSignature> m_GlobSignatures;
    FrozenArray<DagEnvironmentVariableSignature> m_EnvironmentVariableSignatures;

    //we should remove this feature, and exluseively use the new .TargetDirectories that live on DagNode.
    FrozenArray<FrozenFileAndHash> m_DirectoriesCausingImplicitDependencies;

    FrozenArray<FrozenPtr<ScannerData>> m_Scanners;

    // Hashes of filename extensions to use SHA-1 digest signing instead of timestamp signing.
    FrozenArray<uint32_t> m_ShaExtensionHashes;

    int32_t m_DaysToKeepUnreferencedNodesAround;
    int32_t m_EmitDataForBeeWhy;

    FrozenString m_StateFileName;
    FrozenString m_StateFileNameTmp;
    FrozenString m_StateFileNameMapped;
    FrozenString m_ScanCacheFileName;
    FrozenString m_ScanCacheFileNameTmp;
    FrozenString m_DigestCacheFileName;
    FrozenString m_DigestCacheFileNameTmp;
    FrozenString m_BuildTitle;
    FrozenString m_StructuredLogFileName;

    uint32_t m_MagicNumberEnd;
};


struct DagDerived
{
    static const uint32_t MagicNumber = 0x921ad1a8 ^ kTundraHashMagic;

    uint32_t m_MagicNumber;
    uint32_t m_NodeCount;


    //this is an array of the node's direct tobuild dependencies + the usedependencies of its build dependencies.  It boils down to a flat
    //list of everything that needs to have been produces for a node to start building.
    FrozenArray<FrozenArray<int32_t>> m_CombinedDependencies;

    FrozenArray<FrozenArray<uint32_t>> m_NodeBacklinks;

    FrozenArray<uint32_t> m_NodePoints;

    //for each node, a list of indices into that node's m_InputFiles to describe which of those input files are not generated by this graph.
    FrozenArray<FrozenArray<uint32_t>> m_NodeNonGeneratedInputIndicies;

    //all data below are SOA style arrays that contain information for cacheable nodes.  for nodes that are not cacheable, the entry is not populated
    //leaf inputs excluding leaf inputs that come from nodes we depend on that themselves are leaf input cacheable.

    //for each cacheable node: the list of explicitly found leaf inputs to be used to calculate a cachekey from.
    FrozenArray<FrozenArray<FrozenFileAndHash>> m_LeafInputs;

    //many cacheable nodes end up depending on other cacheable nodes. We do not adopt their leaf inputs
    //but it is important to include their leaf input signature in ours, so we need to know which ones they are.
    FrozenArray<FrozenArray<uint32_t>> m_DependentNodesThatThemselvesAreLeafInputCacheable;

    //of all our leaf inputs, some will have to be scanned for includes. We prebaked a list of files for each scanner
    //so at runtime we know exactly which files to scan how as part of calculating the leaf input signature.
    FrozenArray<FrozenArray<FrozenArray<FrozenFileAndHash>>> m_ScannersWithListOfFiles;

    //In order to implement validation that there are no files that influence the build that are not part of the leaf input signature
    //we need to know which of our dependency nodes might have had a dynamic includes that we did not know about yet.
    FrozenArray<FrozenArray<uint32_t>> m_DependentNodesWithScanners;


    //Since the commandlines and environment variables for all cacheable nodes as well as all their dependencies are known at graph-building time
    //we have already hashed them down so we no longer have to do that at runtime.
    FrozenArray<HashDigest> m_LeafInputHash_Offline;

    //convenience accessors to the arrays above, to make callsites a bit easier to read
    const FrozenArray<FrozenFileAndHash>& LeafInputsFor(int leafInputCacheableNode) const { return m_LeafInputs[leafInputCacheableNode]; }
    const FrozenArray<uint32_t>& DependentNodesThatThemselvesAreLeafInputCacheableFor(int leafInputCacheableNode) const { return m_DependentNodesThatThemselvesAreLeafInputCacheable[leafInputCacheableNode]; }
    const FrozenArray<FrozenArray<FrozenFileAndHash>>& ScannersWithListOfFilesFor(int leafInputCacheableNode) const { return m_ScannersWithListOfFiles[leafInputCacheableNode]; }
    const FrozenArray<uint32_t>& DependentNodesWithScannerFor(int leafInputCacheableNode) const { return m_DependentNodesWithScanners[leafInputCacheableNode]; }
    const HashDigest& LeafInputHashOfflineFor(int leafInputCacheableNode) const { return m_LeafInputHash_Offline[leafInputCacheableNode];}

    uint32_t m_MagicNumberEnd;
};

}

struct DagRuntimeData
{
    HashTable<int, kFlagPathStrings> m_OutputsToDagNodes;
    HashTable<int, kFlagPathStrings> m_OutputDirectoriesToDagNodes;
    const Frozen::Dag *m_Dag;
};

void DagRuntimeDataInit(DagRuntimeData* data, const Frozen::Dag* dag, MemAllocHeap *heap);
void DagRuntimeDataDestroy(DagRuntimeData* data);

bool FindDagNodeForFile(const DagRuntimeData* data, uint32_t filenameHash, const char* filename, const Frozen::DagNode **result);
bool IsFileGenerated(const DagRuntimeData* data, uint32_t filenameHash, const char* filename);

void FindDependentNodesFromRootIndex_IncludingSelf_NotRecursingIntoCacheableNodes(MemAllocHeap* heap, const Frozen::Dag* dag, const Frozen::DagNode& dagNode, Buffer<int32_t>& results, Buffer<int32_t>* dependenciesThatAreCacheableThemselves);

bool CheckDagSignatures(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, char *out_of_date_reason, int out_of_date_reason_maxlength);