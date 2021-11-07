#include "StandardInputCanary.hpp"
#include "Thread.hpp"
#include "SignalHandler.hpp"
#include "BuildQueue.hpp"
#include <stdio.h>
#include "Banned.hpp"

static ThreadRoutineReturnType TUNDRA_STDCALL ListenToStdin(void *param)
{
    BuildQueue* queue = (BuildQueue*) param;
    
    while(true)
    {
        char c = fgetc(stdin);
        if (c == EOF)
        {
            SignalSet("stdin closed");
            return 0;
        }
        if (c == 's')
        {
            queue->m_DagVerificationStatus = VerificationStatus::RequiredVerification;
            //getting an s on stdin means that the input data for the buildprogram has been produced. This data is going
            //to be part of the filesignatures of this dag, so we can now validate the dag of the build that we've already
            //speculatively started
            CondSignal(&queue->m_WorkAvailable);
        } else
        {
            CroakAbort("Unexpected stdin");
        }
    }    
}

void StandardInputCanary::Initialize(BuildQueue* queue)
{
    ThreadStart(ListenToStdin, queue, "Canary (stdin)");
}