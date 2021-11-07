#pragma once

//The final result of the build
namespace BuildResult
{
    enum Enum : int
    {
        kOk = 0,                   // All nodes built successfully
        kInterrupted = 1,          // User interrupted the build (e.g CTRL+C)
        kCroak = 2,                // An internal really bad error happened
        kBuildError = 3,           // An action in the build graph failed
        kRequireFrontendRerun = 4  // Frontend needs to run again
    };
}

namespace BinLogFormat
{

// Every binlog file starts with this header. When reading a binlog file you should verify if the identifier in the file
// matches the identifier we expect. if it doesn't, the file is older or newer than the code that's trying to read it.
struct StartOfFileHeader
{
    static const int ExpectedBinaryFormatIdentifier = 0x02dd1ffe;
    int BinaryFormatIdentifier;
};


//all the kinds of messsages supported by the file format
namespace MessageType
{
    enum Enum : int
    {
        BuildStarted = 1,
        NodeInfo = 2,
        NodeEnqueued = 3,
        NodeStarted = 4,
        NodeUpToDate = 5,
        NodeFinished = 6,
        BuildFinished = 7
    };
}

//After the file header, the rest of the file format is very simple. It's a never ending stream of messages. Each message has
//the following header
struct MessageHeader
{
    //the length of the entire message. That includes the header, the payload for the message, and any potential additional data after the payload and before the next message.
    //the only usecase fo this additional data today is strings. we could use it for arrays in the future too.
    int length_including_header;

    //the kind of message this is.
    MessageType::Enum type;

    //the sequence number for this message. This isn't technically required, but it's a nice safety mechanism to protect against bugs in the writing of these files and the reading
    //of them.
    int message_sequence_number;
};


// A BinLogStringRef contains just an integer. the integer points to a position from the start of the file, in which is stored. Typically these strings
// will be stored in the file immediately after the messagepayload of the message that uses them.
// 1) a 32bit int for the bytecount of the string
// 2) the actual UTF8 string is stored. 
struct BinLogStringRef
{
    int position_in_stream;
};

// Sent at the start of every build.
struct BuildStartMessage
{
    static const MessageType::Enum MessageType = MessageType::BuildStarted;
    
    //the maximum number of nodes that you can expect in this binlog file. You can use this to preallocate an array, for when you want to store information per node.
    //(like the NodeInfoMessage below)
    int max_dag_nodes;

    //the highest thread index that will be used in this build.
    int highest_thread_id;

    BinLogStringRef dag_filename;
};


//A message that contains information about a node. All subsequent messages about this node will refer to it by node_index. This message contains
//more detailed information, like outputfile, annotation, output_directory.
struct NodeInfoMessage
{
    static const MessageType::Enum MessageType = MessageType::NodeInfo;
    int node_index;
    BinLogStringRef output_file;
    BinLogStringRef output_directory;
    BinLogStringRef annotation;
    BinLogStringRef profiler_output;
};

//A message that gets sent when the backend has decided that a certain node will have to be made up to date.  Either by executing the action, or by analyzing that
//the previous version is still good.
struct NodeEnqueuedMessage
{
    static const MessageType::Enum MessageType = MessageType::NodeEnqueued;
    int queud_node_index;
    int enqueueing_node_index;
};

//Sent when a node has been determined to be up to date, not requiring executing.
struct NodeUpToDateMessage
{
    static const MessageType::Enum MessageType = MessageType::NodeUpToDate;
    int node_index;
};

//Sent when a node starts executing
struct NodeStartedMessage
{
    static const MessageType::Enum MessageType = MessageType::NodeStarted;
    int node_index;
    int thread_index;
};

//Send when a node finishes executing
struct NodeFinishedMessage
{
    static const MessageType::Enum MessageType = MessageType::NodeFinished;
    int node_index;
    int thread_index;
    int exit_code;
    int duration_in_ms;
    BinLogStringRef output;
    BinLogStringRef cmdline;
};

//Gets sent as final message in the build with the final build result
struct BuildFinishedMessage
{
    static const MessageType::Enum MessageType = MessageType::BuildFinished;
    BuildResult::Enum build_result;
};

}