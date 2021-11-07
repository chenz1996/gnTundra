#include "EventLog.hpp"
#include "RuntimeNode.hpp"
#include "DagData.hpp"
#include <stdio.h>
#include "BinLogFormat.hpp"
#include "Common.hpp"
#include "Banned.hpp"

FILE* s_binlog_stream;
int s_bytes_written;
int s_amount_of_messages_written;
Mutex s_binlog_mutex;
bool s_initialized_binlog_mutex = false;
uint64_t s_LastBinLogFlush;
using namespace BinLogFormat;


static bool BinLogEnabled()
{
    return s_binlog_stream != nullptr;
}

static void CopyToStream(const void* void_ptr, int bytes_to_write)
{
    const char* ptr = (const char*)void_ptr;
    s_bytes_written += bytes_to_write;
    while(bytes_to_write > 0)
    {
        int written = fwrite(ptr, 1, bytes_to_write, s_binlog_stream);
        ptr += written;
        bytes_to_write -= written;
    }
}

template<typename T>
static void WriteToStream(const T& value)
{
    const char* ptr = (const char*)&value;
    CopyToStream(ptr, sizeof(T));
}


struct StringPayloadsForMessage
{
    static const int MAX_PAYLOADS = 10;
    int string_lengths_without_terminator[MAX_PAYLOADS];
    const char* string_ptrs[MAX_PAYLOADS];

    int _write_offset_for_next_string;
    int payload_count;

    StringPayloadsForMessage(int write_offset_for_next_string)
      : _write_offset_for_next_string(write_offset_for_next_string),
        payload_count(0)
    {
    }

    BinLogStringRef AddString(const char* ptr)
    {
        if (payload_count == MAX_PAYLOADS)
            Croak("too many strings");
        
        int length_without_terminator = ptr == nullptr ? 0 : strlen(ptr);
        string_lengths_without_terminator[payload_count] = length_without_terminator;
        string_ptrs[payload_count] = ptr;
        payload_count++;

        BinLogStringRef result;
        result.position_in_stream = ptr == nullptr ? 0 : _write_offset_for_next_string;
        
        _write_offset_for_next_string += sizeof(int) + length_without_terminator + 1;
        return result;
    }
};

static void WriteMessageNonGeneric(const StringPayloadsForMessage& string_payloads, const void* message, int messageSize, MessageType::Enum messageType)
{
    int string_segment_size = 0;
    for (int i=0; i!=string_payloads.payload_count; i++)
    {
        string_segment_size += sizeof(int) + string_payloads.string_lengths_without_terminator[i] + 1;
    }

    MessageHeader header;
    header.length_including_header = sizeof(MessageHeader) + messageSize + string_segment_size;
    header.type = messageType;
    header.message_sequence_number = s_amount_of_messages_written++;

    //useful for debugging:
    //printf("printing msg at %d.  length: %d type: %d seq:%d\n", s_bytes_written, header.length, header.type, header.message_sequence_number);

    WriteToStream(header);
    CopyToStream(message, messageSize);

    for (int i=0; i!=string_payloads.payload_count; i++)
    {
        int length_without_terminator = string_payloads.string_lengths_without_terminator[i];
        WriteToStream(length_without_terminator);
        CopyToStream(string_payloads.string_ptrs[i], length_without_terminator);
        WriteToStream((char)0);
    }
}

template<typename TMessage, typename UserFunc>
static void WriteMessage(UserFunc userFunc)
{
    if (!BinLogEnabled())
        return;

    {
        MutexScope mutexScope(&s_binlog_mutex);

        TMessage message;
        StringPayloadsForMessage string_payloads(s_bytes_written + sizeof(MessageHeader) + sizeof(TMessage));

        userFunc(string_payloads, message); 
        
        WriteMessageNonGeneric(string_payloads, &message, sizeof(TMessage), TMessage::MessageType);
    }

    //Flushing does need to happen with the mutex held.
    uint64_t now = TimerGet();
    if (TimerDiffSeconds(s_LastBinLogFlush, now) > 1)
    {
        fflush(s_binlog_stream);
        s_LastBinLogFlush = now;
    }
}

static BinLogStringRef FirstStringFromArray(const FrozenArray<FrozenFileAndHash>& arrayOfString, StringPayloadsForMessage& strings)
{
    if (arrayOfString.GetCount() == 0)
        return strings.AddString(nullptr);
    return strings.AddString(arrayOfString[0].m_Filename.Get());
}

static void EmitNodeInfoMessage(RuntimeNode* node)
{
    WriteMessage<NodeInfoMessage>([&](auto& string_payloads, auto& msg)
    {
        auto& dagnode = *node->m_DagNode;
        msg.node_index = dagnode.m_OriginalIndex;
        msg.output_file = FirstStringFromArray(dagnode.m_OutputFiles, string_payloads);
        msg.output_directory = FirstStringFromArray(dagnode.m_OutputDirectories, string_payloads);
        msg.annotation = string_payloads.AddString(dagnode.m_Annotation);
        msg.profiler_output = string_payloads.AddString(dagnode.m_ProfilerOutput);
        RuntimeNodeSet_SentBinLogNodeInfoMessage(node);
    });
}


namespace EventLog
{

bool IsEnabled()
{
    return s_binlog_stream != nullptr;
}

void Init(const char* path)
{
    s_amount_of_messages_written = 0;
    s_bytes_written = 0;
    s_LastBinLogFlush = TimerGet();

    MutexInit(&s_binlog_mutex);
    s_initialized_binlog_mutex = true;
    if (path == nullptr)
    {
        return;
    }

    s_binlog_stream = OpenFile(path, "wb");
    if (s_binlog_stream == nullptr)
        Croak("failed to open binlog file at %s", path);    

    StartOfFileHeader header;
    header.BinaryFormatIdentifier = StartOfFileHeader::ExpectedBinaryFormatIdentifier;
    WriteToStream(header);
}

void CloseStream()
{
    if (s_binlog_stream != nullptr)
        fclose(s_binlog_stream);
    s_binlog_stream = nullptr;
}

void Destroy()
{    
    CloseStream();    
    if (s_initialized_binlog_mutex)
        MutexDestroy(&s_binlog_mutex);
}

void EmitBuildStart(const char* dag_filename, int max_node_count, int highest_thread_id)
{
    WriteMessage<BuildStartMessage>([&](auto& string_payloads, auto& msg)
    {
        msg.max_dag_nodes = max_node_count;
        msg.highest_thread_id = highest_thread_id;
        msg.dag_filename = string_payloads.AddString(dag_filename);
    });
}

void EmitBuildFinish(BuildResult::Enum buildResult)
{
    WriteMessage<BuildFinishedMessage>([&](auto& string_payloads, auto& msg)
    {
        msg.build_result = buildResult;
    });
    CloseStream();
}

void EmitFirstTimeEnqueue(RuntimeNode* queued_node, RuntimeNode* enqueueing_node)
{
    if (!RuntimeNodeHas_SentBinLogNodeInfoMessage(queued_node))
        EmitNodeInfoMessage(queued_node);        
    
    WriteMessage<NodeEnqueuedMessage>([&](auto& string_payloads, auto& msg)
    {
        msg.enqueueing_node_index = enqueueing_node == nullptr ? -1 : enqueueing_node->m_DagNode->m_OriginalIndex;
        msg.queud_node_index = queued_node->m_DagNode->m_OriginalIndex;
    });
};

void EmitNodeUpToDate(RuntimeNode* node)
{
    WriteMessage<NodeUpToDateMessage>([&](auto& string_payloads, auto& msg)
    {
        msg.node_index = node->m_DagNode->m_OriginalIndex;   
    });
}

void EmitNodeStart(RuntimeNode* node, int thread_index)
{
    WriteMessage<NodeStartedMessage>([&](auto& string_payloads, auto& msg)
    {
        msg.node_index = node->m_DagNode->m_OriginalIndex;   
        msg.thread_index = thread_index;
    });
}

void EmitNodeFinish(RuntimeNode* node, HashDigest inputSignature, int exitcode, const char* output, int duration_in_ms, int thread_index)
{
    if (!RuntimeNodeHas_SentBinLogNodeInfoMessage(node))
        EmitNodeInfoMessage(node);

    WriteMessage<NodeFinishedMessage>([&](auto& string_payloads, auto& msg)
    {
        msg.node_index = node->m_DagNode->m_OriginalIndex;
        msg.exit_code = exitcode;
        msg.duration_in_ms = duration_in_ms;
        msg.cmdline = string_payloads.AddString(node->m_DagNode->m_Action.Get());
        msg.output = string_payloads.AddString(output);    
        msg.thread_index = thread_index;
    });
}

}