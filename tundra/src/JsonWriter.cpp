#include "JsonWriter.hpp"
#include "MemAllocLinear.hpp"
#include <stdio.h>

#include "Banned.hpp"


struct JsonBlock
{
    enum
    {
        kBlockSize = KB(1) - sizeof(JsonBlock *)
    };

    uint8_t m_Data[kBlockSize];
    JsonBlock *m_Next;
};

void JsonWriteInit(JsonWriter *writer, MemAllocLinear *scratch)
{
    writer->m_Scratch = scratch;
    writer->m_Head = writer->m_Tail = LinearAllocate<JsonBlock>(scratch);
    writer->m_Write = writer->m_Head->m_Data;
    writer->m_TotalSize = 0;
    writer->m_PrependComma = false;
}

static void JsonWrite(JsonWriter *writer, const char *ch, size_t count)
{
    while (count > 0)
    {
        size_t space = JsonBlock::kBlockSize - (writer->m_Write - writer->m_Tail->m_Data);

        if (space == 0)
        {
            writer->m_Tail->m_Next = LinearAllocate<JsonBlock>(writer->m_Scratch);
            writer->m_Tail = writer->m_Tail->m_Next;
            writer->m_Write = writer->m_Tail->m_Data;
            space = JsonBlock::kBlockSize;
        }

        size_t writeSize = space < count ? space : count;

        memcpy(writer->m_Write, ch, writeSize);

        writer->m_Write += writeSize;
        writer->m_TotalSize += writeSize;
        ch += writeSize;
        count -= writeSize;
    }
}

void JsonWriteChar(JsonWriter *writer, char ch)
{
    JsonWrite(writer, &ch, 1);
}

void JsonWriteNewline(JsonWriter *writer)
{
    JsonWrite(writer, "\n", 1);
}

void JsonWriteStartObject(JsonWriter *writer)
{
    if (writer->m_PrependComma)
        JsonWriteChar(writer, ',');

    JsonWriteChar(writer, '{');
    writer->m_PrependComma = false;
}

void JsonWriteEndObject(JsonWriter *writer)
{
    JsonWriteChar(writer, '}');
    writer->m_PrependComma = true;
}

void JsonWriteStartArray(JsonWriter *writer)
{
    if (writer->m_PrependComma)
        JsonWriteChar(writer, ',');

    JsonWriteChar(writer, '[');
    writer->m_PrependComma = false;
}

void JsonWriteEndArray(JsonWriter *writer)
{
    JsonWriteChar(writer, ']');
    writer->m_PrependComma = true;
}

void JsonWriteKeyName(JsonWriter *writer, const char *keyName)
{
    JsonWriteValueString(writer, keyName);
    JsonWriteChar(writer, ':');
    writer->m_PrependComma = false;
}

void JsonWriteRawString(JsonWriter *writer, const char* value, size_t maxLen)
{
    size_t len = 0;
    while (*value != 0 && len < maxLen)
    {
        char ch = *(value++);
        if (ch == '"')
        {
            JsonWrite(writer, "\\\"", 2);
        }
        else if (ch == '\\')
        {
            JsonWrite(writer, "\\\\", 2);
        }
        else if (ch == 0x0A)
        {
            JsonWrite(writer, "\\n", 2);
        }
        else if (ch == 0x0D)
        {
            JsonWrite(writer, "\\r", 2);
        }
        else if (ch == 0x09)
        {
            JsonWrite(writer, "\\t", 2);
        }
        else if (ch == 0x0C)
        {
            JsonWrite(writer, "\\f", 2);
        }
        else if (ch == 0x08)
        {
            JsonWrite(writer, "\\b", 2);
        }
        else
        {
            JsonWriteChar(writer, ch);
        }
        ++len;
    }
}

void JsonWriteValueString(JsonWriter *writer, const char *value, size_t maxLen)
{
    if (writer->m_PrependComma)
        JsonWriteChar(writer, ',');

    if (value == nullptr)
    {
        JsonWrite(writer, "null", 4);
    }
    else
    {
        JsonWriteChar(writer, '"');
        JsonWriteRawString(writer, value, maxLen);
        JsonWriteChar(writer, '"');
    }

    writer->m_PrependComma = true;
}

void JsonWriteValueInteger(JsonWriter *writer, int64_t value)
{
    if (writer->m_PrependComma)
        JsonWriteChar(writer, ',');

    char buf[50];
    snprintf(buf, 50, "%" PRIi64, value);

    JsonWrite(writer, buf, strlen(buf));

    writer->m_PrependComma = true;
}

void JsonWriteToFile(JsonWriter *writer, FILE *fp)
{
    size_t remaining = writer->m_TotalSize;
    JsonBlock *block = writer->m_Head;
    while (remaining > 0)
    {
        size_t sizeThisBlock = (remaining < JsonBlock::kBlockSize) ? remaining : JsonBlock::kBlockSize;
        fwrite(block->m_Data, 1, sizeThisBlock, fp);
        remaining -= sizeThisBlock;
        block = block->m_Next;
    }
}

const char* JsonWriteToString(JsonWriter* writer, MemAllocLinear* heap)
{
    char* output = static_cast<char*>(LinearAllocate(heap, writer->m_TotalSize + 1, 1));
    char* write = output;

    size_t remaining = writer->m_TotalSize;
    JsonBlock* block = writer->m_Head;
    while (remaining > 0)
    {
        size_t sizeThisBlock = (remaining < JsonBlock::kBlockSize) ? remaining : JsonBlock::kBlockSize;
        memcpy(write, block->m_Data, sizeThisBlock);
        remaining -= sizeThisBlock;
        write += sizeThisBlock;
        block = block->m_Next;
    }

    *write = 0;
    return output;
}
