#include "stdarg.h"
#include "Driver.hpp"
#include "NodeResultPrinting.hpp"
#include "DagGenerator.hpp"
#include "LoadFrozenData.hpp"
#include "Profiler.hpp"
#include "FileSign.hpp"
#include "DagDerivedCompiler.hpp"
#include "FileInfoHelper.hpp"
#include "DetectCyclicDependencies.hpp"
#include "DagData.hpp"
#include "Banned.hpp"

static bool ExitRequestingFrontendRun(const char *reason_fmt, ...)
{
    char buffer[1024];

    va_list args;
    va_start(args, reason_fmt);
    int written =vsnprintf(buffer,1024,reason_fmt, args);
    va_end(args);
    buffer[written] = 0;


    PrintMessage(MessageStatusLevel::Success, "Require frontend run.  %s", buffer);

    exit(BuildResult::kRequireFrontendRerun);
    return false;
}


bool LoadOrBuildDag(Driver *self, const char *dag_fn)
{
    const int out_of_date_reason_length = 500;
    char out_of_date_reason[out_of_date_reason_length + 1];

    snprintf(out_of_date_reason, out_of_date_reason_length, "(unknown reason)");

    char dagderived_filename[kMaxPathLength];
    snprintf(dagderived_filename, sizeof dagderived_filename, "%s_derived", dag_fn);
    dagderived_filename[sizeof(dagderived_filename) - 1] = '\0';

    FileInfo dagderived_info = GetFileInfo(dagderived_filename);

    if (self->m_Options.m_DagFileNameJson != nullptr)
    {
        if (!FreezeDagJson(self->m_Options.m_DagFileNameJson, dag_fn))
            return ExitRequestingFrontendRun("%s failed to freeze", self->m_Options.m_DagFileNameJson);
    }

    if (!LoadFrozenData<Frozen::Dag>(dag_fn, &self->m_DagFile, &self->m_DagData))
    {
        RemoveFileOrDir(dag_fn);
        RemoveFileOrDir(dagderived_filename);
        return ExitRequestingFrontendRun("%s couldn't be loaded", dag_fn);
    }

    //only check for cycles when the dag is fresh
    if (self->m_Options.m_DagFileNameJson != nullptr)
    {
        if (DetectCyclicDependencies(self->m_DagData, &self->m_Heap))
        {
            MmapFileUnmap(&self->m_DagFile);
            RemoveFileOrDir(dag_fn);
            RemoveFileOrDir(dagderived_filename);
            exit(BuildResult::kBuildError);
            return 0;
        }
    }

    if (!dagderived_info.Exists() || self->m_Options.m_DagFileNameJson != nullptr)
    {
        if (!CompileDagDerived(self->m_DagData, &self->m_Heap, &self->m_Allocator, &self->m_StatCache, dagderived_filename))
            return ExitRequestingFrontendRun("failed to create derived dag file %s", dagderived_filename);
    }

    if (!LoadFrozenData<Frozen::DagDerived>(dagderived_filename, &self->m_DagDerivedFile, &self->m_DagDerivedData))
    {
        RemoveFileOrDir(dag_fn);
        RemoveFileOrDir(dagderived_filename);
        return ExitRequestingFrontendRun("%s couldn't be loaded", dag_fn);
    }

    return true;
}
