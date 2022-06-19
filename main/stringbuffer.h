
#pragma once

#include <stddef.h>
#include <stdarg.h>

struct StringBuffer
{
    char *buffer;
    size_t buflen;
    size_t written;
};

struct StringBuffer sb_create(char *buffer, size_t buflen)
{
    struct StringBuffer res =
        {
            .buffer = buffer,
            .buflen = buflen,
            .written = 0};
    return res;
}

static bool sb_format(struct StringBuffer *buffer, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (buffer->written < buffer->buflen)
    {
        buffer->written += vsnprintf(buffer->buffer + buffer->written, buffer->buflen - buffer->written, fmt, args);
    }
    va_end(args);
    return buffer->written < buffer->buflen;
}
