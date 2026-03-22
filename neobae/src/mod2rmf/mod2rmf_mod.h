#ifndef MOD2RMF_MOD_H
#define MOD2RMF_MOD_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MOD_FORMAT_UNKNOWN = 0,
    MOD_FORMAT_15,
    MOD_FORMAT_31
} ModFormat;

typedef struct {
    unsigned char sampleCount;
    unsigned char channelCount;
    unsigned char songLength;
    unsigned char restartPos;
    unsigned char orders[128];
    uint32_t patternCount;
    size_t headerSize;
    size_t patternDataOffset;
    size_t sampleDataOffset;
    char title[21];
} ModHeader;

uint32_t mod2rmf_detect_mod_channel_count(const unsigned char *sig);
ModFormat mod2rmf_detect_mod_format(const unsigned char *data, size_t size);
int mod2rmf_try_parse_layout(ModHeader *h,
                             const unsigned char *data,
                             size_t size,
                             unsigned char sampleCount,
                             unsigned char channelCount,
                             size_t songLenOff,
                             size_t restartOff,
                             size_t ordersOff,
                             size_t headerSize,
                             uint32_t maxSamples);

#endif
