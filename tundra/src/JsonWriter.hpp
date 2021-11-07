#pragma once

#include <stdint.h>
#include <stdio.h>

struct MemAllocLinear;
struct JsonBlock;

struct JsonWriter
{
    MemAllocLinear *m_Scratch;
    JsonBlock *m_Head;
    JsonBlock *m_Tail;
    uint8_t *m_Write;
    bool m_PrependComma;
    uint64_t m_TotalSize;
};

void JsonWriteInit(JsonWriter *writer, MemAllocLinear *heap);

void JsonWriteStartObject(JsonWriter *writer);
void JsonWriteEndObject(JsonWriter *writer);

void JsonWriteStartArray(JsonWriter *writer);
void JsonWriteEndArray(JsonWriter *writer);

void JsonWriteKeyName(JsonWriter *writer, const char *keyName);

void JsonWriteChar(JsonWriter *writer, char ch);
void JsonWriteRawString(JsonWriter *writer, const char* value, size_t maxLen = (size_t)-1);
void JsonWriteValueString(JsonWriter *writer, const char *value, size_t maxLen = (size_t)-1);
void JsonWriteValueInteger(JsonWriter *writer, int64_t value);

void JsonWriteNewline(JsonWriter *writer);

void JsonWriteToFile(JsonWriter *writer, FILE *fp);
const char* JsonWriteToString(JsonWriter* writer, MemAllocLinear* heap);
