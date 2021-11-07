#include "NodeResultPrinting.hpp"
#include "DagData.hpp"
#include "BuildQueue.hpp"
#include "Exec.hpp"
#include "JsonWriter.hpp"
#include "LeafInputSignature.hpp"
#include <stdio.h>
#include <sstream>
#include <ctime>
#include <math.h>
#include "Driver.hpp"
#include "EventLog.hpp"

#if TUNDRA_UNIX
#include <unistd.h>
#include <stdarg.h>
#endif

#include "Banned.hpp"

struct NodeResultPrintData
{
    const Frozen::DagNode *node_data;
    const char *cmd_line;
    bool verbose;
    int duration;
    ValidationResult::Enum validation_result;
    const bool *untouched_outputs;
    const char *output_buffer;
    int processed_node_count;
    int number_of_nodes_ever_queued;
    MessageStatusLevel::Enum status_level;
    int return_code;
    bool was_preparation_error;
};

static bool EmitColors = false;

static uint64_t last_progress_message_of_any_job;
static const Frozen::DagNode *last_progress_message_job = nullptr;
static int total_number_node_results_printed = 0;

static int deferred_message_count = 0;
static NodeResultPrintData deferred_messages[kMaxBuildThreads];


static Mutex s_node_printing_mutex;
static bool s_DontPrintNodeResultsToStdout;

struct MutexedNodeResultPrintingStream
{
    MutexedNodeResultPrintingStream()
    {
        if (s_DontPrintNodeResultsToStdout)
            return;
        MutexLock(&s_node_printing_mutex);
        stream = stdout;
    }
    ~MutexedNodeResultPrintingStream()
    {
        if (s_DontPrintNodeResultsToStdout)
            return;
        MutexUnlock(&s_node_printing_mutex);
    }

    int vprint(const char *formatString, va_list args) const
    {
        if (s_DontPrintNodeResultsToStdout)
            return 0; 

        return vfprintf(stream, formatString, args);;
    }

    int print(const char *formatString, ...) const
    {
        if (s_DontPrintNodeResultsToStdout)
            return 0; 

        va_list args;
        va_start(args, formatString);
        int result = vfprintf(stream, formatString, args);
        va_end(args);
        return result;
    }

    void flush()
    {
        if (s_DontPrintNodeResultsToStdout)
            return;         
        fflush(stream);
    }
private:
    FILE* stream;   
};

#define printf error_only_print_to_MutexedNodeResultPrintingStream
#define vprintf error_only_print_to_MutexedNodeResultPrintingStream

static bool isTerminatingChar(char c)
{
    return c >= 0x40 && c <= 0x7E;
}

static bool IsEscapeCode(char c)
{
    return c == 0x1B;
}

static char *DetectEscapeCode(char *ptr)
{
    if (!IsEscapeCode(ptr[0]))
        return ptr;
    if (ptr[1] == 0)
        return ptr;

    //there are other characters valid than [ here, but for now we'll only support stripping ones that have [, as all color sequences have that.
    if (ptr[1] != '[')
        return ptr;

    char *endOfCode = ptr + 2;

    while (true)
    {
        char c = *endOfCode;
        if (c == 0)
            return ptr;
        if (isTerminatingChar(c))
            return endOfCode + 1;
        endOfCode++;
    }
}

void StripAnsiColors(char *buffer)
{
    char *writeCursor = buffer;
    char *readCursor = buffer;
    while (*readCursor)
    {
        char *adjusted = DetectEscapeCode(readCursor);
        if (adjusted != readCursor)
        {
            readCursor = adjusted;
            continue;
        }
        *writeCursor++ = *readCursor++;
    }
    *writeCursor++ = 0;
}

int s_IdentificationColor;
int m_VisualMaxNodes;

void InitNodeResultPrinting(const DriverOptions* driverOptions)
{
    MutexInit(&s_node_printing_mutex);
    last_progress_message_of_any_job = TimerGet() - 10000;

    s_DontPrintNodeResultsToStdout = driverOptions->m_DontPrintNodeResultsToStdout;
#if TUNDRA_UNIX
    if (isatty(fileno(stdout)))
    {
        EmitColors = true;
    }
#endif

#if TUNDRA_WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode))
    {
        const int ENABLE_VIRTUAL_TERMINAL_PROCESSING_impl = 0x0004;
        if (dwMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING_impl)
            EmitColors = true;
    }
#endif

    char *value = getenv("DOWNSTREAM_STDOUT_CONSUMER_SUPPORTS_COLOR");
    if (value != nullptr)
    {
        if (*value == '1')
            EmitColors = true;
        if (*value == '0')
            EmitColors = false;
    }
    s_IdentificationColor = driverOptions->m_IdentificationColor;
    m_VisualMaxNodes = driverOptions->m_VisualMaxNodes;
}

void DestroyNodeResultPrinting()
{
    MutexDestroy(&s_node_printing_mutex);
}

static void EnsureConsoleCanHandleColors()
{
#if TUNDRA_WIN32
    //We invoke this function before every printf that wants to emit a color, because it looks like child processes that tundra invokes
    //can and do SetConsoleMode() which affects our console. Sometimes a child process will set the consolemode to no longer have our flag
    //which makes all color output suddenly screw up.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode))
    {
        const int ENABLE_VIRTUAL_TERMINAL_PROCESSING_impl = 0x0004;
        DWORD newMode = dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING_impl;
        if (newMode != dwMode)
            SetConsoleMode(hOut, newMode);
    }
#endif
}

static void EmitColor(const MutexedNodeResultPrintingStream& stream, const char *colorsequence)
{
    if (EmitColors)
    {
        EnsureConsoleCanHandleColors();
        stream.print("%s", colorsequence);
    }
}

#define RED "\x1B[91m"
#define GRN "\x1B[32m"
#define YEL "\x1B[33m"
#define BLU "\x1B[34m"
#define MAG "\x1B[35m"
#define CYN "\x1B[36m"
#define GRAY "\x0B[37m"
#define WHT "\x1B[37m"
#define RESET "\x1B[0m"


static void PrintDiagnosticPrefix(const MutexedNodeResultPrintingStream& stream, const char *title, const char *color = YEL)
{
    EmitColor(stream,color);
    stream.print("##### %s\n", title);
    EmitColor(stream,RESET);
}

static void PrintDiagnosticFormat(const MutexedNodeResultPrintingStream& stream, const char *title, const char *formatString, ...)
{
    PrintDiagnosticPrefix(stream, title);
    va_list args;
    va_start(args, formatString);
    stream.vprint(formatString, args);
    va_end(args);
    stream.print("\n");
}

static void PrintDiagnostic(const MutexedNodeResultPrintingStream& stream, const char *title, const char *contents)
{
    if (contents != nullptr)
        PrintDiagnosticFormat(stream, title, "%s", contents);
}

static void PrintDiagnostic(const MutexedNodeResultPrintingStream& stream, const char *title, int content)
{
    PrintDiagnosticFormat(stream, title, "%d", content);
}

void EmitColorReset(const MutexedNodeResultPrintingStream& stream)
{
    EmitColor(stream, RESET);
}

void EmitColorForLevel(const MutexedNodeResultPrintingStream& stream, MessageStatusLevel::Enum status_level)
{
    if (status_level == MessageStatusLevel::Info)
        EmitColor(stream, WHT);
    if (status_level == MessageStatusLevel::Warning)
        EmitColor(stream, YEL);
    if (status_level == MessageStatusLevel::Success)
        EmitColor(stream, GRN);
    if (status_level == MessageStatusLevel::Failure)
        EmitColor(stream, RED);
}

void PrintServiceMessage(MessageStatusLevel::Enum status_level, const char *formatString, ...)
{
    MutexedNodeResultPrintingStream stream;
    EmitColorForLevel(stream,status_level);
    va_list args;
    va_start(args, formatString);
    stream.vprint(formatString, args);
    va_end(args);
    EmitColor(stream,RESET);
    stream.print("\n");
    stream.flush();
}


static void TrimOutputBuffer(OutputBufferData *buffer)
{
    auto isNewLine = [](char c) { return c == 0x0A || c == 0x0D; };

    int trimmedCursor = buffer->cursor;
    while (isNewLine(*(buffer->buffer + trimmedCursor - 1)) && trimmedCursor > 0)
        trimmedCursor--;

    buffer->buffer[trimmedCursor] = 0;
    if (!EmitColors)
    {
        StripAnsiColors(buffer->buffer);
    }
}

static void EmitBracketColor(const MutexedNodeResultPrintingStream& stream, MessageStatusLevel::Enum status_level)
{
    if (s_IdentificationColor == 1)
        EmitColor(stream, MAG);
    else if (s_IdentificationColor == 2)
        EmitColor(stream, BLU);
    else
        EmitColorForLevel(stream, status_level);
}

static void PrintMessageMaster(const MutexedNodeResultPrintingStream& stream, MessageStatusLevel::Enum status_level, int dividend, int divisor, int duration, const char* message, va_list args)
{
    EmitBracketColor(stream, status_level);
    stream.print("[");
    EmitColorForLevel(stream, status_level);

    int maxDigits = ceil(log10(divisor + 1));
    int visualMaxNodeDigits = ceil(log10(m_VisualMaxNodes + 1));
    int shouldPrint = 2*visualMaxNodeDigits + 2;

    int printed = 0;
    if (dividend >= 0)
    {
        printed += stream.print("%*d/%d ", maxDigits, dividend, divisor);
    }
    for (int i=printed; i<shouldPrint;i++)
        stream.print(" ");

    if (duration >= 0)
        stream.print("%2ds", duration);
    else
        stream.print("   ");


    EmitBracketColor(stream, status_level);
    stream.print("] ");
    EmitColor(stream, RESET);
    stream.vprint(message, args);
    stream.print("\n");
}


void PrintMessage(MessageStatusLevel::Enum status_level, int duration, const char* message, ...)
{
    MutexedNodeResultPrintingStream stream;
    va_list args;
    va_start(args,message);
    PrintMessageMaster(stream, status_level, -1, -1, duration, message, args);
    va_end(args);
}
void PrintMessage(MessageStatusLevel::Enum status_level, const char* message, ...)
{
    MutexedNodeResultPrintingStream stream;
    va_list args;
    va_start(args,message);
    PrintMessageMaster(stream, status_level, -1, -1, -1, message, args);
    va_end(args);
}
void PrintMessage(MessageStatusLevel::Enum status_level, int dividend, int divisor, int duration, const char* message, ...)
{
    MutexedNodeResultPrintingStream stream;
    va_list args;
    va_start(args,message);
    PrintMessageMaster(stream, status_level, dividend, divisor, duration, message, args);
    va_end(args);
}
void PrintMessage(MessageStatusLevel::Enum status_level, int duration, ExecResult *result, const char *message, ...)
{
    MutexedNodeResultPrintingStream stream;
    va_list args;
    va_start(args, message);
    PrintMessageMaster(stream, status_level, -1, -1, duration, message, args);
    va_end(args);

    if (result != nullptr && result->m_ReturnCode != 0)
    {
        TrimOutputBuffer(&result->m_OutputBuffer);
        stream.print("%s\n", result->m_OutputBuffer.buffer);
    }
}

static void ValidationErrorFor(const NodeResultPrintData* data, Buffer<const char*>& validationOutput, MemAllocHeap* heap)
{
    auto AddLine = [&](const char* line)
    {
        BufferAppendOne(&validationOutput, heap, line);
    };

    if (ValidationResult::UnexpectedConsoleOutputFail == data->validation_result)
    {
        AddLine("Failed because this command wrote something to the output that wasn't expected. We were expecting any of the following strings:");

        int count = data->node_data->m_AllowedOutputSubstrings.GetCount();

        for (int i = 0; i != count; i++)
            AddLine((const char *)data->node_data->m_AllowedOutputSubstrings[i]);
        if (count == 0)
            AddLine("<< no allowed strings >>");
        AddLine("but got:");
        AddLine(data->output_buffer);
        return;
    }

    if (ValidationResult::UnwrittenOutputFileFail == data->validation_result)
    {
        AddLine("Failed because this command failed to write the following output files:");
        for (int i = 0; i < data->node_data->m_OutputFiles.GetCount(); i++)
            if (data->untouched_outputs[i])
                AddLine((const char *)data->node_data->m_OutputFiles[i].m_Filename);
        return;
    }
    Croak("Unexpected validation result: %d, for node %s", data->validation_result, data->node_data->m_Annotation.Get());
}

static void PrintNodeResult(const MutexedNodeResultPrintingStream& stream, const NodeResultPrintData *data, BuildQueue *queue)
{
    va_list args = {};
    PrintMessageMaster(stream, data->status_level, data->processed_node_count, queue->m_AmountOfNodesEverQueued, data->duration, data->node_data->m_Annotation.Get(), args);

    if (data->verbose)
    {
        PrintDiagnostic(stream, "CommandLine", data->cmd_line);
        for (int i = 0; i != data->node_data->m_FrontendResponseFiles.GetCount(); i++)
        {
            char titleBuffer[1024];
            const char *file = data->node_data->m_FrontendResponseFiles[i].m_Filename;
            snprintf(titleBuffer, sizeof titleBuffer, "Contents of %s", file);

            char *content_buffer;
            FILE *f = OpenFile(file, "rb");
            if (!f)
            {
                int buffersize = 512;
                content_buffer = (char *)HeapAllocate(queue->m_Config.m_Heap, buffersize);
                snprintf(content_buffer, buffersize, "couldn't open %s for reading", file);
            }
            else
            {
                fseek(f, 0L, SEEK_END);
                size_t size = ftell(f);
                rewind(f);
                size_t buffer_size = size + 1;
                content_buffer = (char *)HeapAllocate(queue->m_Config.m_Heap, buffer_size);

                size_t read = fread(content_buffer, 1, size, f);
                content_buffer[read] = '\0';
                fclose(f);
            }
            PrintDiagnostic(stream, titleBuffer, content_buffer);
            HeapFree(queue->m_Config.m_Heap, content_buffer);
        }

        if (data->node_data->m_EnvVars.GetCount() > 0)
            PrintDiagnosticPrefix(stream, "Custom Environment Variables");
        for (int i = 0; i != data->node_data->m_EnvVars.GetCount(); i++)
        {
            auto &entry = data->node_data->m_EnvVars[i];
            stream.print("%s=%s\n", entry.m_Name.Get(), entry.m_Value.Get());
        }
        if (data->return_code == 0)
        {
            if (data->validation_result == ValidationResult::UnexpectedConsoleOutputFail || data->validation_result == ValidationResult::UnwrittenOutputFileFail)
            {
                Buffer<const char*> validationOutput;
                BufferInit(&validationOutput);
                ValidationErrorFor(data, validationOutput, queue->m_Config.m_Heap);

                PrintDiagnosticPrefix(stream, validationOutput[0], RED);
                for (int i = 1; i != validationOutput.GetCount(); i++)
                    stream.print("%s\n", validationOutput[i]);

                BufferDestroy(&validationOutput, queue->m_Config.m_Heap);
            }
        }
        if (data->return_code != 0)
            PrintDiagnostic(stream, "ExitCode", data->return_code);
    }

    if (data->output_buffer != nullptr)
    {
        if (data->verbose)
        {
            PrintDiagnosticPrefix(stream, "Output");

            stream.print("%s\n", data->output_buffer);
        }
        else if (0 != (data->validation_result != ValidationResult::SwallowStdout))
        {
            stream.print("%s\n", data->output_buffer);
        }
    }
}

static void PrintCacheOperationIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node, const char* hitOrMissMessage)
{
    if (IsStructuredLogActive())
    {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, hitOrMissMessage);

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node->m_DagNode->m_OriginalIndex);

        JsonWriteKeyName(&msg, "leafInputSignature");
        char hash[kDigestStringSize];
        DigestToString(hash, node->m_CurrentLeafInputSignature->digest);
        JsonWriteValueString(&msg, hash);

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
    }
}

void PrintCacheHitIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node)
{
    PrintCacheOperationIntoStructuredLog(thread_state, node, "cachehit");
}

void PrintCacheMissIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node)
{
    PrintCacheOperationIntoStructuredLog(thread_state, node, "cachemiss");
}

void PrintCacheHit(BuildQueue* queue, ThreadState *thread_state, double duration, RuntimeNode* node)
{
    CheckHasLock(&queue->m_Lock);

    PrintCacheHitIntoStructuredLog(thread_state, node);

    char buffer[1024];
    char hash[kDigestStringSize];
    DigestToString(hash, node->m_CurrentLeafInputSignature->digest);
    int written = snprintf(buffer, sizeof(buffer), "%s [CacheHit %s]", node->m_DagNode->m_Annotation.Get(), hash);
    if (written >0 && written < sizeof(buffer))
        PrintMessage(MessageStatusLevel::Success, queue->m_FinishedNodeCount, queue->m_AmountOfNodesEverQueued, duration, buffer);
}



static char *StrDupN(MemAllocHeap *allocator, const char *str, size_t len)
{
    size_t sz = len + 1;
    char *buffer = static_cast<char *>(HeapAllocate(allocator, sz));
    memcpy(buffer, str, sz - 1);
    buffer[sz - 1] = '\0';
    return buffer;
}

static char *StrDup(MemAllocHeap *allocator, const char *str)
{
    return StrDupN(allocator, str, strlen(str));
}

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
    bool was_preparation_error)
{
    int processedNodeCount = queue->m_FinishedNodeCount;
    bool failed = result->m_ReturnCode != 0 || validationResult >= ValidationResult::UnexpectedConsoleOutputFail;
    bool verbose = (failed && !was_preparation_error) || always_verbose;

    int duration = TimerDiffSeconds(time_exec_started, TimerGet());
    
    NodeResultPrintData data = {};
    data.node_data = node_data;
    data.cmd_line = cmd_line;
    data.verbose = verbose;
    data.duration = duration;
    data.validation_result = validationResult;
    data.untouched_outputs = untouched_outputs;
    data.processed_node_count = processedNodeCount;
    data.number_of_nodes_ever_queued = queue->m_AmountOfNodesEverQueued;
    data.status_level = failed ? MessageStatusLevel::Failure : MessageStatusLevel::Success;

    data.return_code = was_preparation_error ? 1 : result->m_ReturnCode;
    data.was_preparation_error = was_preparation_error;

    bool anyOutput = result->m_OutputBuffer.cursor > 0;
    if (anyOutput && verbose)
    {
        TrimOutputBuffer(&result->m_OutputBuffer);
        data.output_buffer = result->m_OutputBuffer.buffer;
    }
    else if (anyOutput && 0 != (validationResult != ValidationResult::SwallowStdout))
    {
        TrimOutputBuffer(&result->m_OutputBuffer);
        data.output_buffer = result->m_OutputBuffer.buffer;
    }

    if (IsStructuredLogActive())
    {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, "noderesult");

        JsonWriteKeyName(&msg, "processed_node_count");
        JsonWriteValueInteger(&msg, data.processed_node_count);

        JsonWriteKeyName(&msg, "number_of_nodes_ever_queued");
        JsonWriteValueInteger(&msg, data.number_of_nodes_ever_queued);

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node_data->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

        JsonWriteKeyName(&msg, "exitcode");
        JsonWriteValueInteger(&msg, result->m_ReturnCode != 0 ? result->m_ReturnCode : (failed ? 1 : 0));

        if (failed)
        {
            JsonWriteKeyName(&msg, "cmdline");
            JsonWriteValueString(&msg, node_data->m_Action.Get());

            if (node_data->m_FrontendResponseFiles.GetCount() > 0)
            {
                JsonWriteKeyName(&msg, "rsps");
                JsonWriteStartArray(&msg);
                for(const auto& rsp: node_data->m_FrontendResponseFiles)
                    JsonWriteValueString(&msg, rsp.m_Filename.Get());
                JsonWriteEndArray(&msg);
            }
        }

        if (data.node_data->m_ProfilerOutput.Get())
        {
            JsonWriteKeyName(&msg, "profiler_output");
            JsonWriteValueString(&msg, data.node_data->m_ProfilerOutput.Get());
        }

        if (data.node_data->m_OutputFiles.GetCount() > 0)
        {
            JsonWriteKeyName(&msg, "outputfile");
            JsonWriteValueString(&msg, data.node_data->m_OutputFiles[0].m_Filename);
        }

        if (data.node_data->m_OutputDirectories.GetCount() > 0)
        {
            JsonWriteKeyName(&msg, "outputdirectory");
            JsonWriteValueString(&msg, data.node_data->m_OutputDirectories[0].m_Filename);
        }

        if (failed && data.return_code == 0)
        {
            JsonWriteKeyName(&msg, "stdout");
            JsonWriteChar(&msg, '"');

            Buffer<const char*> validationOutput;
            BufferInit(&validationOutput);
            ValidationErrorFor(&data, validationOutput, queue->m_Config.m_Heap);

            for (int i = 0; i != validationOutput.GetCount(); i++)
            {
                JsonWriteRawString(&msg, validationOutput[i]);
                JsonWriteRawString(&msg, "\n");
            }

            BufferDestroy(&validationOutput, queue->m_Config.m_Heap);
            JsonWriteChar(&msg, '"');
        } else {
            if (data.output_buffer)
            {
                JsonWriteKeyName(&msg, "stdout");
                JsonWriteValueString(&msg, data.output_buffer);
            }
        }

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
    }

    if (s_DontPrintNodeResultsToStdout)
        return;

    // defer most of regular build failure output to the end of build, so that they are all
    // conveniently at the end of the log
    bool defer = failed && deferred_message_count < ARRAY_SIZE(deferred_messages);
    if (!defer)
    {
        MutexedNodeResultPrintingStream stream;
        PrintNodeResult(stream, &data, queue);
    }
    else
    {
        // copy data needed for output that might be coming from temporary/local storage
        if (data.cmd_line != nullptr)
            data.cmd_line = StrDup(queue->m_Config.m_Heap, data.cmd_line);
        if (data.output_buffer != nullptr)
            data.output_buffer = StrDup(queue->m_Config.m_Heap, data.output_buffer);

        if (untouched_outputs != nullptr)
        {
            int n_outputs = node_data->m_OutputFiles.GetCount();
            bool* untouched_outputs_copy = (bool*)HeapAllocate(queue->m_Config.m_Heap, n_outputs * sizeof(bool));
            memcpy(untouched_outputs_copy, untouched_outputs, n_outputs * sizeof(bool));
            data.untouched_outputs = untouched_outputs_copy;
        }
        // store data needed for deferred output
        deferred_messages[deferred_message_count] = data;
        deferred_message_count++;
    }

    total_number_node_results_printed++;
    last_progress_message_of_any_job = TimerGet();
    last_progress_message_job = node_data;
}

void PrintDeferredMessages(BuildQueue *queue)
{
    MutexedNodeResultPrintingStream stream;

    for (int i = 0; i < deferred_message_count; ++i)
    {
        const NodeResultPrintData &data = deferred_messages[i];
        PrintNodeResult(stream, &data, queue);
        if (data.cmd_line != nullptr)
            HeapFree(queue->m_Config.m_Heap, data.cmd_line);
        if (data.output_buffer != nullptr)
            HeapFree(queue->m_Config.m_Heap, data.output_buffer);
        if (data.untouched_outputs != nullptr)
            HeapFree(queue->m_Config.m_Heap, data.untouched_outputs);
    }
    stream.flush();
    deferred_message_count = 0;
}

void PrintNodeInProgress(const Frozen::DagNode *node_data, uint64_t time_of_start, const BuildQueue *queue, const char* message)
{
    MutexedNodeResultPrintingStream stream;    
    
    uint64_t now = TimerGet();
    int seconds_job_has_been_running_for = TimerDiffSeconds(time_of_start, now);
    double seconds_since_last_progress_message_of_any_job = TimerDiffSeconds(last_progress_message_of_any_job, now);

    if (message == nullptr)
        message = node_data->m_Annotation.Get();

    int acceptable_time_since_last_message = last_progress_message_job == node_data ? 10 : (total_number_node_results_printed == 0 ? 0 : 5);
    int only_print_if_slower_than = seconds_since_last_progress_message_of_any_job > 30 ? 0 : 5;

    if (seconds_since_last_progress_message_of_any_job > acceptable_time_since_last_message && seconds_job_has_been_running_for > only_print_if_slower_than)
    {
        int maxDigits = ceil(log10(m_VisualMaxNodes + 1));

        EmitColor(stream, YEL);
        stream.print("[BUSY %*ds] ", maxDigits * 2 - 1, seconds_job_has_been_running_for);
        EmitColor(stream, RESET);
        stream.print("%s\n", message);
        last_progress_message_of_any_job = now;
        last_progress_message_job = node_data;

        stream.flush();
    }

    return;
}


