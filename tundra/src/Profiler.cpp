#include "Profiler.hpp"
#include "Common.hpp"
#include "MemAllocLinear.hpp"
#include "MemAllocHeap.hpp"

#include <stdio.h>

#include "Banned.hpp"

const size_t kProfilerThreadMaxEvents = 32 * 1024;                        // max this # of events per thread
const size_t kProfilerThreadStringsSize = kProfilerThreadMaxEvents * 128; // that many bytes of name string storage per thread

struct ProfilerEvent
{
    uint64_t m_Time;
    uint64_t m_Duration;
    const char *m_Name;
    const char *m_Info;
    const char *m_Color;
};

struct ProfilerThread
{
    MemAllocLinear m_ScratchStrings;
    ProfilerEvent *m_Events;
    int m_EventCount;
    bool m_IsBegin;
};

struct ProfilerState
{
    MemAllocHeap m_Heap;
    char *m_FileName;
    ProfilerThread *m_Threads;
    int m_ThreadCount;
};

static ProfilerState s_ProfilerState;

bool g_ProfilerEnabled;

void ProfilerInit(const char *fileName, int threadCount)
{
    CHECK(!g_ProfilerEnabled);
    CHECK(threadCount > 0);

    g_ProfilerEnabled = true;

    s_ProfilerState.m_ThreadCount = threadCount;

    HeapInit(&s_ProfilerState.m_Heap);

    size_t fileNameLen = strlen(fileName);
    s_ProfilerState.m_FileName = (char *)HeapAllocate(&s_ProfilerState.m_Heap, fileNameLen + 1);
    memcpy(s_ProfilerState.m_FileName, fileName, fileNameLen + 1);

    s_ProfilerState.m_Threads = HeapAllocateArray<ProfilerThread>(&s_ProfilerState.m_Heap, s_ProfilerState.m_ThreadCount);
    for (int i = 0; i < s_ProfilerState.m_ThreadCount; ++i)
    {
        ProfilerThread &thread = s_ProfilerState.m_Threads[i];
        thread.m_Events = HeapAllocateArray<ProfilerEvent>(&s_ProfilerState.m_Heap, kProfilerThreadMaxEvents);
        thread.m_EventCount = 0;
        LinearAllocInit(&thread.m_ScratchStrings, &s_ProfilerState.m_Heap, kProfilerThreadStringsSize, "profilerStrings");
        thread.m_IsBegin = false;
    }
}

static void EscapeString(const char *src, char *dst, int dstSpace)
{
    while (*src && dstSpace > 2) // some input chars might expand into 2 in the output
    {
        char c = *src;
        switch (c)
        {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            *dst++ = '\\';
            *dst++ = c;
            dstSpace -= 2;
            break;
        default:
            if (c >= 32 && c < 126)
            {
                *dst++ = c;
                dstSpace -= 1;
            }
        }
        ++src;
    }
    *dst = 0;
}

void ProfilerWriteOutput()
{
    uint64_t timeStampAtStartOfWriteOutput = TimerGet();

    FILE *f = OpenFile(s_ProfilerState.m_FileName, "w");
    if (!f)
    {
        Log(kWarning, "profiler: failed to write profiler output file into '%s'", s_ProfilerState.m_FileName);
        return;
    }

    bool justRawTraceEvents = strstr(s_ProfilerState.m_FileName, "traceevents") != nullptr;

    // See https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/edit for
    // Chrome Tracing profiler format description

    if (!justRawTraceEvents)
    {
        fputs("{\n", f);
        // JSON does not support comments, so emit "how to use this" as a fake string value
        fputs("\"instructions_readme\": \"1) Open Chrome, 2) go to chrome://tracing, 3) click Load, 4) navigate to this file.\",\n", f);
        fputs("\"traceEvents\":[\n", f);
    }
    
    //we're emitting our pid as 12345. This is a bit weird, because it's not our real process ID, but in practice it works well. bee_backend is not a program
    //where you can have two running at the same time.  Since only 1 can run at the same time, it's very nice if all the different bee-backend runs of a combined
    //profile json file end up in the same "horizontal track" in chrome trace viewer.  you can achieve this by making sure they all have the same pid.
    //we're using an integer instead of 'bee_backend" string before, as I suspect that this is one of the reasons why several 3rd party trace file viewers choke
    //on bee's profile json files.
    fputs("{ \"cat\":\"\", \"pid\":12345, \"tid\":0, \"ts\":0, \"ph\":\"M\", \"name\":\"process_name\", \"args\": { \"name\":\"bee_backend\" } }\n", f);

    for (int i = 0; i < s_ProfilerState.m_ThreadCount; ++i)
    {
        const ProfilerThread &thread = s_ProfilerState.m_Threads[i];
        for (int j = 0; j < thread.m_EventCount; ++j)
        {
            const ProfilerEvent &evt = thread.m_Events[j];
            char name[1024];
            char info[1024];
            EscapeString(evt.m_Name, name, sizeof(name));
            EscapeString(evt.m_Info, info, sizeof(info));

            const char *cnameEntry = "";
            char buffer[100];
            if (evt.m_Color != nullptr)
            {
                snprintf(buffer, sizeof(buffer), "\"cname\":\"%s\", ", evt.m_Color);
                cnameEntry = buffer;
            }

            fprintf(f, ",{ \"pid\":12345, \"tid\":%d, \"ts\":%" PRIu64 ", \"dur\":%" PRIu64 ", \"ph\":\"X\", \"name\": \"%s\", %s \"args\": { \"detail\":\"%s\" }}\n", i, evt.m_Time, evt.m_Duration, name, cnameEntry, info);
        }
    }

    uint64_t durationOfWritingProfilerOutput = TimerGet() - timeStampAtStartOfWriteOutput;
    fprintf(f, ",{ \"pid\":12345, \"tid\":0, \"ts\":%" PRIu64 ", \"dur\":%" PRIu64 ", \"ph\":\"X\", \"name\": \"ProfilerWriteOutput\" }\n", timeStampAtStartOfWriteOutput, durationOfWritingProfilerOutput);

    if (!justRawTraceEvents)
    {
        fputs("\n]\n", f);
        fputs("}\n", f);
    } else {
        //in raw trace events mode, every line should be a tracevent plus a comma
        fputs(",", f);
    }

    fclose(f);
}

void ProfilerDestroy()
{
    if (!g_ProfilerEnabled)
        return;

    for (int i = 0; i < s_ProfilerState.m_ThreadCount; ++i)
    {
        ProfilerThread &thread = s_ProfilerState.m_Threads[i];
        if (thread.m_IsBegin)
            ProfilerEndImpl(i);
    }

    ProfilerWriteOutput();

    for (int i = 0; i < s_ProfilerState.m_ThreadCount; ++i)
    {
        ProfilerThread &thread = s_ProfilerState.m_Threads[i];
        Log(kSpam, "profiler: thread %i had %d events, %.1f KB strings", i, thread.m_EventCount, double(thread.m_ScratchStrings.m_Offset) / 1024.0);
        HeapFree(&s_ProfilerState.m_Heap, thread.m_Events);
        LinearAllocDestroy(&thread.m_ScratchStrings);
    }
    HeapFree(&s_ProfilerState.m_Heap, s_ProfilerState.m_Threads);
    HeapFree(&s_ProfilerState.m_Heap, s_ProfilerState.m_FileName);
    HeapDestroy(&s_ProfilerState.m_Heap);
}

void ProfilerBeginImpl(const char *name, int threadIndex, const char *info, const char *color)
{
    CHECK(g_ProfilerEnabled);
    CHECK(threadIndex >= 0 && threadIndex < s_ProfilerState.m_ThreadCount);
    ProfilerThread &thread = s_ProfilerState.m_Threads[threadIndex];
    CHECK(!thread.m_IsBegin);
    thread.m_IsBegin = true;
    if (thread.m_EventCount >= kProfilerThreadMaxEvents)
    {
        Log(kWarning, "profiler: max events (%d) reached on thread %i, '%s' and later won't be recorded", (int)kProfilerThreadMaxEvents, threadIndex, name);
        return;
    }
    ProfilerEvent &evt = thread.m_Events[thread.m_EventCount++];
    evt.m_Time = TimerGet();
    evt.m_Color = color;

    // split input name by first space
    const char *nextWord = strchr(name, ' ');
    if (nextWord == nullptr || info != nullptr)
    {
        evt.m_Name = StrDup(&thread.m_ScratchStrings, name);
        evt.m_Info = info != nullptr ? StrDup(&thread.m_ScratchStrings, info) : "";
    }
    else
    {
        evt.m_Name = StrDupN(&thread.m_ScratchStrings, name, nextWord - name);
        evt.m_Info = StrDup(&thread.m_ScratchStrings, nextWord + 1);
    }
}

void ProfilerEndImpl(int threadIndex)
{
    CHECK(g_ProfilerEnabled);
    CHECK(threadIndex >= 0 && threadIndex < s_ProfilerState.m_ThreadCount);
    ProfilerThread &thread = s_ProfilerState.m_Threads[threadIndex];
    CHECK(thread.m_IsBegin);
    CHECK(thread.m_EventCount > 0);
    thread.m_IsBegin = false;
    if (thread.m_EventCount > kProfilerThreadMaxEvents)
        return;
    ProfilerEvent &evt = thread.m_Events[thread.m_EventCount - 1];
    evt.m_Duration = TimerGet() - evt.m_Time;
}

