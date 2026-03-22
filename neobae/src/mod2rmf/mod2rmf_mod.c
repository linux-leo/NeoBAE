#include <string.h>

#include "mod2rmf_mod.h"

uint32_t mod2rmf_detect_mod_channel_count(const unsigned char *sig)
{
    if (!sig)
    {
        return 0;
    }

    if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) || !memcmp(sig, "FLT4", 4))
    {
        return 4;
    }
    if (!memcmp(sig, "FLT8", 4) || !memcmp(sig, "CD81", 4) || !memcmp(sig, "OKTA", 4))
    {
        return 8;
    }
    if (sig[0] >= '1' && sig[0] <= '9' && sig[1] >= '0' && sig[1] <= '9' &&
        (sig[2] == 'C' || sig[2] == 'c') && (sig[3] == 'H' || sig[3] == 'h'))
    {
        return (uint32_t)((sig[0] - '0') * 10 + (sig[1] - '0'));
    }
    if (sig[0] >= '1' && sig[0] <= '9' &&
        (sig[1] == 'C' || sig[1] == 'c') &&
        (sig[2] == 'H' || sig[2] == 'h') &&
        (sig[3] == 'N' || sig[3] == 'n'))
    {
        return (uint32_t)(sig[0] - '0');
    }
    return 0;
}

ModFormat mod2rmf_detect_mod_format(const unsigned char *data, size_t size)
{
    uint32_t channels;
    if (!data || size < 1084)
    {
        return MOD_FORMAT_UNKNOWN;
    }

    channels = mod2rmf_detect_mod_channel_count(data + 1080);
    if (channels >= 1 && channels <= 32)
    {
        return MOD_FORMAT_31;
    }
    if (size >= 600)
    {
        return MOD_FORMAT_15;
    }
    return MOD_FORMAT_UNKNOWN;
}

int mod2rmf_try_parse_layout(ModHeader *h,
                             const unsigned char *data,
                             size_t size,
                             unsigned char sampleCount,
                             unsigned char channelCount,
                             size_t songLenOff,
                             size_t restartOff,
                             size_t ordersOff,
                             size_t headerSize,
                             uint32_t maxSamples)
{
    uint32_t maxPattern;
    size_t patternBytes;
    size_t sampleDataOffset;
    size_t i;

    if (!h || !data || size < headerSize || channelCount < 1 || channelCount > 32)
    {
        return 0;
    }

    if (sampleCount > maxSamples)
    {
        return 0;
    }

    if (data[songLenOff] == 0 || data[songLenOff] > 128)
    {
        return 0;
    }

    maxPattern = 0;
    for (i = 0; i < 128; ++i)
    {
        if (data[ordersOff + i] > maxPattern)
        {
            maxPattern = data[ordersOff + i];
        }
    }

    patternBytes = (size_t)(maxPattern + 1u) * 64u * (size_t)channelCount * 4u;
    sampleDataOffset = headerSize + patternBytes;
    if (sampleDataOffset > size)
    {
        return 0;
    }

    memset(h, 0, sizeof(*h));
    h->sampleCount = sampleCount;
    h->channelCount = channelCount;
    h->songLength = data[songLenOff];
    h->restartPos = data[restartOff];
    memcpy(h->orders, data + ordersOff, 128);
    h->patternCount = maxPattern + 1u;
    h->headerSize = headerSize;
    h->patternDataOffset = headerSize;
    h->sampleDataOffset = sampleDataOffset;
    memcpy(h->title, data, 20);
    h->title[20] = '\0';
    return 1;
}
