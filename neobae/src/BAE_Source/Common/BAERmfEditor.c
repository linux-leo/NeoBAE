#include "NeoBAE.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "X_API.h"
#include "X_Formats.h"
#include "X_Assert.h"

typedef struct BAERmfEditorNote
{
    uint32_t startTick;
    uint32_t durationTicks;
    unsigned char note;
    unsigned char velocity;
    uint16_t bank;
    unsigned char program;
} BAERmfEditorNote;

typedef struct BAERmfEditorCCEvent
{
    uint32_t tick;
    unsigned char cc;    /* 0-127 = CC number; 0xFF = pitch bend sentinel */
    unsigned char value; /* CC value, or pitch bend LSB when cc == 0xFF */
    unsigned char data2; /* 0 for CC events; pitch bend MSB when cc == 0xFF */
} BAERmfEditorCCEvent;

typedef struct BAERmfEditorTrack
{
    char *name;
    unsigned char channel;
    uint16_t bank;
    unsigned char program;
    unsigned char pan;
    unsigned char volume;
    int16_t transpose;
    BAERmfEditorNote *notes;
    uint32_t noteCount;
    uint32_t noteCapacity;
    BAERmfEditorCCEvent *ccEvents;
    uint32_t ccEventCount;
    uint32_t ccEventCapacity;
} BAERmfEditorTrack;

typedef struct BAERmfEditorSample
{
    char *displayName;
    char *sourcePath;
    unsigned char program;
    uint32_t instID;       /* original INST resource ID (e.g. 562 for bank-2 prog 50) */
    unsigned char rootKey;
    unsigned char lowKey;
    unsigned char highKey;
    uint32_t sourceCompressionType;
    BAESampleInfo sampleInfo;
    GM_Waveform *waveform;
} BAERmfEditorSample;

typedef struct BAERmfEditorResourceEntry
{
    XResourceType type;
    XLongResourceID id;
    unsigned char pascalName[256];
    XPTR data;
    int32_t size;
} BAERmfEditorResourceEntry;

typedef struct BAERmfEditorTempoEvent
{
    uint32_t tick;
    uint32_t microsecondsPerQuarter;
} BAERmfEditorTempoEvent;

struct BAERmfEditorDocument
{
    uint32_t tempoBPM;
    uint16_t ticksPerQuarter;
    int16_t maxMidiNotes;
    int16_t maxEffects;
    int16_t mixLevel;
    int16_t songVolume;
    BAEReverbType reverbType;
    char *info[INFO_TYPE_COUNT];
    BAERmfEditorTrack *tracks;
    uint32_t trackCount;
    uint32_t trackCapacity;
    BAERmfEditorSample *samples;
    uint32_t sampleCount;
    uint32_t sampleCapacity;
    BAERmfEditorTempoEvent *tempoEvents;
    uint32_t tempoEventCount;
    uint32_t tempoEventCapacity;
    BAERmfEditorResourceEntry *originalResources;
    uint32_t originalResourceCount;
    uint32_t originalResourceCapacity;
    XLongResourceID originalSongID;
    XLongResourceID originalObjectResourceID;
    XResourceType originalMidiType;
    XBOOL loadedFromRmf;
    XBOOL isPristine;
};

typedef struct ByteBuffer
{
    unsigned char *data;
    uint32_t size;
    uint32_t capacity;
} ByteBuffer;

typedef struct MidiEventRecord
{
    uint32_t tick;
    unsigned char order;
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    uint16_t bank;
    unsigned char program;
    unsigned char applyProgram;
} MidiEventRecord;

typedef struct BAERmfEditorActiveNote
{
    struct BAERmfEditorActiveNote *next;
    uint32_t startTick;
    unsigned char channel;
    unsigned char note;
    unsigned char velocity;
    uint16_t bank;
    unsigned char program;
} BAERmfEditorActiveNote;

static BAEResult PV_CreatePascalName(char const *source, char outName[256]);
static BAEResult PV_GrowBuffer(void **buffer, uint32_t *capacity, uint32_t elementSize, uint32_t minimumCount);
static void PV_ClearTempoEvents(BAERmfEditorDocument *document);
static BAEResult PV_AddTempoEvent(BAERmfEditorDocument *document, uint32_t tick, uint32_t microsecondsPerQuarter);
static BAEResult PV_AddCCEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char cc, unsigned char value, unsigned char data2);
static int PV_CompareCCEvents(void const *left, void const *right);
static BAERmfEditorCCEvent *PV_FindTrackCCEvent(BAERmfEditorTrack *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex);
static BAERmfEditorCCEvent const *PV_FindTrackCCEventConst(BAERmfEditorTrack const *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex);

static char *PV_DuplicateString(char const *source)
{
    char *copy;
    uint32_t length;

    if (!source)
    {
        return NULL;
    }
    length = (uint32_t)strlen(source);
    copy = (char *)XNewPtr((int32_t)(length + 1));
    if (copy)
    {
        XBlockMove(source, copy, (int32_t)(length + 1));
    }
    return copy;
}

static void PV_FreeString(char **target)
{
    if (target && *target)
    {
        XDisposePtr(*target);
        *target = NULL;
    }
}

static void PV_MarkDocumentDirty(BAERmfEditorDocument *document)
{
    if (document)
    {
        document->isPristine = FALSE;
    }
}

static void PV_FreeOriginalResources(BAERmfEditorDocument *document)
{
    uint32_t index;

    if (!document)
    {
        return;
    }
    for (index = 0; index < document->originalResourceCount; ++index)
    {
        if (document->originalResources[index].data)
        {
            XDisposePtr(document->originalResources[index].data);
            document->originalResources[index].data = NULL;
        }
    }
    if (document->originalResources)
    {
        XDisposePtr(document->originalResources);
        document->originalResources = NULL;
    }
    document->originalResourceCount = 0;
    document->originalResourceCapacity = 0;
}

static void PV_ClearTempoEvents(BAERmfEditorDocument *document)
{
    if (!document)
    {
        return;
    }
    if (document->tempoEvents)
    {
        XDisposePtr(document->tempoEvents);
        document->tempoEvents = NULL;
    }
    document->tempoEventCount = 0;
    document->tempoEventCapacity = 0;
}

static BAEResult PV_AddTempoEvent(BAERmfEditorDocument *document, uint32_t tick, uint32_t microsecondsPerQuarter)
{
    uint32_t insertIndex;
    BAEResult result;

    if (!document || microsecondsPerQuarter == 0)
    {
        return BAE_PARAM_ERR;
    }

    insertIndex = 0;
    while (insertIndex < document->tempoEventCount && document->tempoEvents[insertIndex].tick < tick)
    {
        insertIndex++;
    }

    if (insertIndex < document->tempoEventCount && document->tempoEvents[insertIndex].tick == tick)
    {
        document->tempoEvents[insertIndex].microsecondsPerQuarter = microsecondsPerQuarter;
        return BAE_NO_ERROR;
    }

    result = PV_GrowBuffer((void **)&document->tempoEvents,
                           &document->tempoEventCapacity,
                           sizeof(BAERmfEditorTempoEvent),
                           document->tempoEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    if (insertIndex < document->tempoEventCount)
    {
        XBlockMove(&document->tempoEvents[insertIndex],
                   &document->tempoEvents[insertIndex + 1],
                   (int32_t)((document->tempoEventCount - insertIndex) * sizeof(BAERmfEditorTempoEvent)));
    }

    document->tempoEvents[insertIndex].tick = tick;
    document->tempoEvents[insertIndex].microsecondsPerQuarter = microsecondsPerQuarter;
    document->tempoEventCount++;
    return BAE_NO_ERROR;
}

static BAEResult PV_AddCCEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char cc, unsigned char value, unsigned char data2)
{
    BAEResult result;
    BAERmfEditorCCEvent *event;

    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_GrowBuffer((void **)&track->ccEvents,
                           &track->ccEventCapacity,
                           sizeof(BAERmfEditorCCEvent),
                           track->ccEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    event = &track->ccEvents[track->ccEventCount];
    event->tick = tick;
    event->cc = cc;
    event->value = value;
    event->data2 = data2;
    track->ccEventCount++;
    qsort(track->ccEvents, track->ccEventCount, sizeof(BAERmfEditorCCEvent), PV_CompareCCEvents);
    return BAE_NO_ERROR;
}

static int PV_CompareCCEvents(void const *left, void const *right)
{
    BAERmfEditorCCEvent const *a;
    BAERmfEditorCCEvent const *b;

    a = (BAERmfEditorCCEvent const *)left;
    b = (BAERmfEditorCCEvent const *)right;
    if (a->tick < b->tick)
    {
        return -1;
    }
    if (a->tick > b->tick)
    {
        return 1;
    }
    if (a->cc < b->cc)
    {
        return -1;
    }
    if (a->cc > b->cc)
    {
        return 1;
    }
    return 0;
}

static BAERmfEditorCCEvent *PV_FindTrackCCEvent(BAERmfEditorTrack *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex)
{
    uint32_t index;
    uint32_t count;

    if (!track)
    {
        return NULL;
    }
    count = 0;
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc)
        {
            if (count == eventIndex)
            {
                if (outActualIndex)
                {
                    *outActualIndex = index;
                }
                return &track->ccEvents[index];
            }
            count++;
        }
    }
    return NULL;
}

static BAERmfEditorCCEvent const *PV_FindTrackCCEventConst(BAERmfEditorTrack const *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex)
{
    uint32_t index;
    uint32_t count;

    if (!track)
    {
        return NULL;
    }
    count = 0;
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc)
        {
            if (count == eventIndex)
            {
                if (outActualIndex)
                {
                    *outActualIndex = index;
                }
                return &track->ccEvents[index];
            }
            count++;
        }
    }
    return NULL;
}

static BAEResult PV_CaptureOriginalResourcesFromFile(BAERmfEditorDocument *document, XFILE fileRef)
{
    XFILERESOURCEMAP map;
    int32_t nextOffset;
    int32_t resourceCount;
    int32_t resourceIndex;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    PV_FreeOriginalResources(document);

    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0 ||
        XGetLong(&map.mapID) != XFILERESOURCE_ID)
    {
        return BAE_BAD_FILE;
    }

    nextOffset = (int32_t)sizeof(XFILERESOURCEMAP);
    resourceCount = (int32_t)XGetLong(&map.totalResources);
    for (resourceIndex = 0; resourceIndex < resourceCount; ++resourceIndex)
    {
        int32_t nextHeader;
        int32_t rawType;
        int32_t rawID;
        XResourceType type;
        XLongResourceID id;
        char cName[256];
        BAERmfEditorResourceEntry *entry;
        XPTR resourceData;
        int32_t resourceSize;
        BAEResult growResult;

        if (XFileSetPosition(fileRef, nextOffset) != 0 ||
            XFileRead(fileRef, &nextHeader, (int32_t)sizeof(int32_t)) != 0 ||
            XFileRead(fileRef, &rawType, (int32_t)sizeof(int32_t)) != 0 ||
            XFileRead(fileRef, &rawID, (int32_t)sizeof(int32_t)) != 0)
        {
            PV_FreeOriginalResources(document);
            return BAE_FILE_IO_ERROR;
        }
        nextHeader = (int32_t)XGetLong(&nextHeader);
        type = (XResourceType)XGetLong(&rawType);
        id = (XLongResourceID)XGetLong(&rawID);

        resourceData = XGetFileResource(fileRef, type, id, NULL, &resourceSize);
        if (!resourceData)
        {
            PV_FreeOriginalResources(document);
            return BAE_BAD_FILE;
        }

        growResult = PV_GrowBuffer((void **)&document->originalResources,
                                   &document->originalResourceCapacity,
                                   sizeof(BAERmfEditorResourceEntry),
                                   document->originalResourceCount + 1);
        if (growResult != BAE_NO_ERROR)
        {
            XDisposePtr(resourceData);
            PV_FreeOriginalResources(document);
            return growResult;
        }

        entry = &document->originalResources[document->originalResourceCount];
        XSetMemory(entry, sizeof(*entry), 0);
        entry->type = type;
        entry->id = id;
        entry->data = resourceData;
        entry->size = resourceSize;
        cName[0] = 0;
        if (XGetFileResourceName(fileRef, type, id, cName) != FALSE)
        {
            if (PV_CreatePascalName(cName, (char *)entry->pascalName) != BAE_NO_ERROR)
            {
                entry->pascalName[0] = 0;
            }
        }
        document->originalResourceCount++;

        if (resourceIndex < (resourceCount - 1))
        {
            if (nextHeader <= nextOffset)
            {
                PV_FreeOriginalResources(document);
                return BAE_BAD_FILE;
            }
            nextOffset = nextHeader;
        }
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_GrowBuffer(void **buffer, uint32_t *capacity, uint32_t elementSize, uint32_t minimumCount)
{
    void *nextBuffer;
    uint32_t nextCapacity;

    if (*capacity >= minimumCount)
    {
        return BAE_NO_ERROR;
    }
    nextCapacity = *capacity ? (*capacity * 2) : 4;
    while (nextCapacity < minimumCount)
    {
        nextCapacity *= 2;
    }
    nextBuffer = XNewPtr((int32_t)(nextCapacity * elementSize));
    if (!nextBuffer)
    {
        return BAE_MEMORY_ERR;
    }
    if (*buffer && *capacity)
    {
        XBlockMove(*buffer, nextBuffer, (int32_t)(*capacity * elementSize));
        XDisposePtr(*buffer);
    }
    *buffer = nextBuffer;
    *capacity = nextCapacity;
    return BAE_NO_ERROR;
}

static BAEResult PV_ByteBufferReserve(ByteBuffer *buffer, uint32_t extraBytes)
{
    uint32_t required;

    required = buffer->size + extraBytes;
    return PV_GrowBuffer((void **)&buffer->data, &buffer->capacity, sizeof(unsigned char), required);
}

static BAEResult PV_ByteBufferAppend(ByteBuffer *buffer, void const *data, uint32_t length)
{
    BAEResult result;

    result = PV_ByteBufferReserve(buffer, length);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    if (length)
    {
        XBlockMove(data, buffer->data + buffer->size, (int32_t)length);
        buffer->size += length;
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_ByteBufferAppendByte(ByteBuffer *buffer, unsigned char value)
{
    return PV_ByteBufferAppend(buffer, &value, 1);
}

static BAEResult PV_ByteBufferAppendBE16(ByteBuffer *buffer, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8) & 0xFF);
    bytes[1] = (unsigned char)(value & 0xFF);
    return PV_ByteBufferAppend(buffer, bytes, 2);
}

static BAEResult PV_ByteBufferAppendBE32(ByteBuffer *buffer, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24) & 0xFF);
    bytes[1] = (unsigned char)((value >> 16) & 0xFF);
    bytes[2] = (unsigned char)((value >> 8) & 0xFF);
    bytes[3] = (unsigned char)(value & 0xFF);
    return PV_ByteBufferAppend(buffer, bytes, 4);
}

static BAEResult PV_ByteBufferAppendVLQ(ByteBuffer *buffer, uint32_t value)
{
    unsigned char encoded[5];
    int index;
    int outIndex;

    encoded[0] = (unsigned char)(value & 0x7F);
    index = 1;
    value >>= 7;
    while (value)
    {
        encoded[index++] = (unsigned char)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    for (outIndex = index - 1; outIndex >= 0; --outIndex)
    {
        BAEResult result;

        result = PV_ByteBufferAppendByte(buffer, encoded[outIndex]);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    return BAE_NO_ERROR;
}

static void PV_ByteBufferDispose(ByteBuffer *buffer)
{
    if (buffer->data)
    {
        XDisposePtr(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
    buffer->capacity = 0;
}

static BAEResult PV_SetDocumentString(char **target, char const *value)
{
    char *copy;

    copy = NULL;
    if (value && value[0])
    {
        copy = PV_DuplicateString(value);
        if (!copy)
        {
            return BAE_MEMORY_ERR;
        }
    }
    PV_FreeString(target);
    *target = copy;
    return BAE_NO_ERROR;
}

static AudioFileType PV_TranslateEditorFileType(BAEFileType fileType)
{
    switch (fileType)
    {
        case BAE_WAVE_TYPE:
            return FILE_WAVE_TYPE;
        case BAE_AIFF_TYPE:
            return FILE_AIFF_TYPE;
        default:
            return FILE_INVALID_TYPE;
    }
}

static BAEResult PV_AssignSongInfoString(SongResource_Info *songInfo, BAEInfoType infoType, char const *value)
{
    char *copy;

    copy = NULL;
    if (value && value[0])
    {
        copy = PV_DuplicateString(value);
        if (!copy)
        {
            return BAE_MEMORY_ERR;
        }
    }
    switch (infoType)
    {
        case TITLE_INFO:
            songInfo->title = copy;
            return BAE_NO_ERROR;
        case PERFORMED_BY_INFO:
            songInfo->performed = copy;
            return BAE_NO_ERROR;
        case COMPOSER_INFO:
            songInfo->composer = copy;
            return BAE_NO_ERROR;
        case COPYRIGHT_INFO:
            songInfo->copyright = copy;
            return BAE_NO_ERROR;
        case PUBLISHER_CONTACT_INFO:
            songInfo->publisher_contact_info = copy;
            return BAE_NO_ERROR;
        case USE_OF_LICENSE_INFO:
            songInfo->use_license = copy;
            return BAE_NO_ERROR;
        case LICENSED_TO_URL_INFO:
            songInfo->licensed_to_URL = copy;
            return BAE_NO_ERROR;
        case LICENSE_TERM_INFO:
            songInfo->license_term = copy;
            return BAE_NO_ERROR;
        case EXPIRATION_DATE_INFO:
            songInfo->expire_date = copy;
            return BAE_NO_ERROR;
        case COMPOSER_NOTES_INFO:
            songInfo->compser_notes = copy;
            return BAE_NO_ERROR;
        case INDEX_NUMBER_INFO:
            songInfo->index_number = copy;
            return BAE_NO_ERROR;
        case GENRE_INFO:
            songInfo->genre = copy;
            return BAE_NO_ERROR;
        case SUB_GENRE_INFO:
            songInfo->sub_genre = copy;
            return BAE_NO_ERROR;
        case TEMPO_DESCRIPTION_INFO:
            songInfo->tempo_description = copy;
            return BAE_NO_ERROR;
        case ORIGINAL_SOURCE_INFO:
            songInfo->original_source = copy;
            return BAE_NO_ERROR;
        default:
            if (copy)
            {
                XDisposePtr(copy);
            }
            return BAE_PARAM_ERR;
    }
}

static BAEResult PV_CreatePascalName(char const *source, char outName[256])
{
    uint32_t length;

    if (!outName)
    {
        return BAE_PARAM_ERR;
    }
    if (!source)
    {
        outName[0] = 0;
        return BAE_NO_ERROR;
    }
    length = (uint32_t)strlen(source);
    if (length > 255)
    {
        length = 255;
    }
    outName[0] = (char)length;
    if (length)
    {
        XBlockMove(source, outName + 1, (int32_t)length);
    }
    return BAE_NO_ERROR;
}

static uint16_t PV_ReadBE16(unsigned char const *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t PV_ReadBE32(unsigned char const *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static BAEResult PV_ReadVLQ(unsigned char const *data, uint32_t dataSize, uint32_t *ioOffset, uint32_t *outValue)
{
    uint32_t offset;
    uint32_t value;
    int count;

    if (!data || !ioOffset || !outValue)
    {
        return BAE_PARAM_ERR;
    }
    offset = *ioOffset;
    value = 0;
    for (count = 0; count < 4; ++count)
    {
        unsigned char byteValue;

        if (offset >= dataSize)
        {
            return BAE_BAD_FILE;
        }
        byteValue = data[offset++];
        value = (value << 7) | (uint32_t)(byteValue & 0x7F);
        if ((byteValue & 0x80) == 0)
        {
            *ioOffset = offset;
            *outValue = value;
            return BAE_NO_ERROR;
        }
    }
    return BAE_BAD_FILE;
}

static unsigned char PV_ClampMidi7Bit(int32_t value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 127)
    {
        return 127;
    }
    return (unsigned char)value;
}

static BAERmfEditorTrack *PV_GetTrack(BAERmfEditorDocument *document, uint16_t trackIndex)
{
    if (!document || trackIndex >= document->trackCount)
    {
        return NULL;
    }
    return &document->tracks[trackIndex];
}

static BAERmfEditorTrack const *PV_GetTrackConst(BAERmfEditorDocument const *document, uint16_t trackIndex)
{
    if (!document || trackIndex >= document->trackCount)
    {
        return NULL;
    }
    return &document->tracks[trackIndex];
}

static BAEResult PV_AddNoteToTrack(BAERmfEditorTrack *track,
                                   uint32_t startTick,
                                   uint32_t durationTicks,
                                   unsigned char note,
                                   unsigned char velocity,
                                   uint16_t bank,
                                   unsigned char program)
{
    BAEResult result;

    if (!track || durationTicks == 0)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_GrowBuffer((void **)&track->notes,
                           &track->noteCapacity,
                           sizeof(BAERmfEditorNote),
                           track->noteCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track->notes[track->noteCount].startTick = startTick;
    track->notes[track->noteCount].durationTicks = durationTicks;
    track->notes[track->noteCount].note = note;
    track->notes[track->noteCount].velocity = velocity ? velocity : 96;
    track->notes[track->noteCount].bank = bank;
    track->notes[track->noteCount].program = program;
    track->noteCount++;
    return BAE_NO_ERROR;
}

static BAEResult PV_SetTrackName(BAERmfEditorTrack *track, char const *name)
{
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    return PV_SetDocumentString(&track->name, name);
}

static BAEResult PV_ReadWholeFile(BAEPathName filePath, unsigned char **outData, uint32_t *outSize)
{
    XFILENAME fileName;
    XFILE fileRef;
    unsigned char *data;
    int32_t length;

    if (!filePath || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outData = NULL;
    *outSize = 0;
    XConvertPathToXFILENAME(filePath, &fileName);
    fileRef = XFileOpenForRead(&fileName);
    if (!fileRef)
    {
        return BAE_FILE_IO_ERROR;
    }
    length = XFileGetLength(fileRef);
    if (length <= 0)
    {
        XFileClose(fileRef);
        return BAE_BAD_FILE;
    }
    data = (unsigned char *)XNewPtr(length);
    if (!data)
    {
        XFileClose(fileRef);
        return BAE_MEMORY_ERR;
    }
    if (XFileSetPosition(fileRef, 0L) != 0 || XFileRead(fileRef, data, length) != 0)
    {
        XDisposePtr(data);
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);
    *outData = data;
    *outSize = (uint32_t)length;
    return BAE_NO_ERROR;
}

static BAERmfEditorActiveNote *PV_PushActiveNote(BAERmfEditorActiveNote **head,
                                                 uint32_t startTick,
                                                 unsigned char channel,
                                                 unsigned char note,
                                                 unsigned char velocity,
                                                 uint16_t bank,
                                                 unsigned char program)
{
    BAERmfEditorActiveNote *activeNote;

    activeNote = (BAERmfEditorActiveNote *)XNewPtr(sizeof(BAERmfEditorActiveNote));
    if (!activeNote)
    {
        return NULL;
    }
    XSetMemory(activeNote, sizeof(*activeNote), 0);
    activeNote->startTick = startTick;
    activeNote->channel = channel;
    activeNote->note = note;
    activeNote->velocity = velocity;
    activeNote->bank = bank;
    activeNote->program = program;
    activeNote->next = *head;
    *head = activeNote;
    return activeNote;
}

static BAERmfEditorActiveNote *PV_PopActiveNote(BAERmfEditorActiveNote **head,
                                                unsigned char channel,
                                                unsigned char note)
{
    BAERmfEditorActiveNote *current;
    BAERmfEditorActiveNote *previous;

    if (!head)
    {
        return NULL;
    }
    previous = NULL;
    current = *head;
    while (current)
    {
        if (current->channel == channel && current->note == note)
        {
            if (previous)
            {
                previous->next = current->next;
            }
            else
            {
                *head = current->next;
            }
            current->next = NULL;
            return current;
        }
        previous = current;
        current = current->next;
    }
    return NULL;
}

static void PV_DisposeActiveNotes(BAERmfEditorActiveNote **head)
{
    BAERmfEditorActiveNote *current;

    if (!head)
    {
        return;
    }
    current = *head;
    while (current)
    {
        BAERmfEditorActiveNote *next;

        next = current->next;
        XDisposePtr(current);
        current = next;
    }
    *head = NULL;
}

static BAEResult PV_FinalizeActiveNotes(BAERmfEditorTrack *track,
                                        BAERmfEditorActiveNote **activeNotes,
                                        uint32_t finalTick)
{
    BAEResult result;

    result = BAE_NO_ERROR;
    while (activeNotes && *activeNotes)
    {
        BAERmfEditorActiveNote *activeNote;
        uint32_t durationTicks;

        activeNote = *activeNotes;
        *activeNotes = activeNote->next;
        durationTicks = (finalTick > activeNote->startTick) ? (finalTick - activeNote->startTick) : 1;
        if (result == BAE_NO_ERROR)
        {
            result = PV_AddNoteToTrack(track,
                                       activeNote->startTick,
                                       durationTicks,
                                       activeNote->note,
                                       activeNote->velocity,
                                       activeNote->bank,
                                       activeNote->program);
        }
        XDisposePtr(activeNote);
    }
    return result;
}

static BAEResult PV_LoadMidiTrackIntoDocument(BAERmfEditorDocument *document,
                                              unsigned char const *trackData,
                                              uint32_t trackSize)
{
    BAERmfEditorTrackSetup setup;
    BAERmfEditorTrack *track;
    BAERmfEditorActiveNote *activeNotes;
    uint16_t trackIndex;
    uint32_t offset;
    uint32_t currentTick;
    unsigned char runningStatus;
    BAEResult result;
    XBOOL sawChannel;
    uint16_t channelBank[BAE_MAX_MIDI_CHANNELS];
    unsigned char channelProgram[BAE_MAX_MIDI_CHANNELS];
    uint16_t initChannel;

    XSetMemory(&setup, sizeof(setup), 0);
    setup.channel = 0;
    setup.bank = 0;
    setup.program = 0;
    setup.name = NULL;
    result = BAERmfEditorDocument_AddTrack(document, &setup, &trackIndex);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track = &document->tracks[trackIndex];
    track->pan = 64;
    track->volume = 100;
    track->transpose = 0;
    activeNotes = NULL;
    offset = 0;
    currentTick = 0;
    runningStatus = 0;
    sawChannel = FALSE;
    for (initChannel = 0; initChannel < BAE_MAX_MIDI_CHANNELS; ++initChannel)
    {
        channelBank[initChannel] = 0;
        channelProgram[initChannel] = 0;
    }

    while (offset < trackSize)
    {
        uint32_t delta;
        unsigned char status;
        unsigned char eventType;
        unsigned char channel;

        result = PV_ReadVLQ(trackData, trackSize, &offset, &delta);
        if (result != BAE_NO_ERROR)
        {
            PV_DisposeActiveNotes(&activeNotes);
            return result;
        }
        currentTick += delta;
        if (offset >= trackSize)
        {
            break;
        }
        status = trackData[offset++];
        if (status < 0x80)
        {
            if (runningStatus == 0)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            offset--;
            status = runningStatus;
        }
        else if (status < 0xF0)
        {
            runningStatus = status;
        }
        else
        {
            runningStatus = 0;
        }

        if (status == 0xFF)
        {
            unsigned char metaType;
            uint32_t metaLength;

            if (offset >= trackSize)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            metaType = trackData[offset++];
            result = PV_ReadVLQ(trackData, trackSize, &offset, &metaLength);
            if (result != BAE_NO_ERROR || offset + metaLength > trackSize)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            if (metaType == 0x03)
            {
                char *nameCopy;

                nameCopy = (char *)XNewPtr((int32_t)(metaLength + 1));
                if (!nameCopy)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_MEMORY_ERR;
                }
                if (metaLength)
                {
                    XBlockMove(trackData + offset, nameCopy, (int32_t)metaLength);
                }
                nameCopy[metaLength] = 0;
                PV_FreeString(&track->name);
                track->name = nameCopy;
            }
            else if (metaType == 0x51 && metaLength == 3)
            {
                uint32_t microsecondsPerQuarter;

                microsecondsPerQuarter = ((uint32_t)trackData[offset] << 16) |
                                         ((uint32_t)trackData[offset + 1] << 8) |
                                         (uint32_t)trackData[offset + 2];
                if (microsecondsPerQuarter > 0)
                {
                    result = PV_AddTempoEvent(document, currentTick, microsecondsPerQuarter);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                    if (document->tempoEventCount == 1)
                    {
                        document->tempoBPM = 60000000UL / microsecondsPerQuarter;
                    }
                }
            }
            else if (metaType == 0x2F)
            {
                offset += metaLength;
                break;
            }
            offset += metaLength;
            continue;
        }
        if (status == 0xF0 || status == 0xF7)
        {
            uint32_t sysexLength;

            result = PV_ReadVLQ(trackData, trackSize, &offset, &sysexLength);
            if (result != BAE_NO_ERROR || offset + sysexLength > trackSize)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            offset += sysexLength;
            continue;
        }

        eventType = (unsigned char)(status & 0xF0);
        channel = (unsigned char)(status & 0x0F);
        if (sawChannel == FALSE)
        {
            track->channel = channel;
            sawChannel = TRUE;
        }

        switch (eventType)
        {
            case NOTE_OFF:
            case NOTE_ON:
            {
                unsigned char noteValue;
                unsigned char velocity;
                BAERmfEditorActiveNote *activeNote;

                if (offset + 2 > trackSize)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_BAD_FILE;
                }
                noteValue = trackData[offset++];
                velocity = trackData[offset++];
                if (eventType == NOTE_ON && velocity > 0)
                {
                    activeNote = PV_PushActiveNote(&activeNotes,
                                                   currentTick,
                                                   channel,
                                                   noteValue,
                                                   velocity,
                                                   channelBank[channel],
                                                   channelProgram[channel]);
                    if (!activeNote)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return BAE_MEMORY_ERR;
                    }
                }
                else
                {
                    activeNote = PV_PopActiveNote(&activeNotes, channel, noteValue);
                    if (activeNote)
                    {
                        uint32_t durationTicks;

                        durationTicks = (currentTick > activeNote->startTick) ?
                                        (currentTick - activeNote->startTick) : 1;
                        result = PV_AddNoteToTrack(track,
                                                   activeNote->startTick,
                                                   durationTicks,
                                                   activeNote->note,
                                                   activeNote->velocity,
                                                   activeNote->bank,
                                                   activeNote->program);
                        XDisposePtr(activeNote);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                    }
                }
                break;
            }
            case POLY_AFTERTOUCH:
            case CONTROL_CHANGE:
            case PITCH_BEND:
            {
                unsigned char data1;
                unsigned char data2;

                if (offset + 2 > trackSize)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_BAD_FILE;
                }
                data1 = trackData[offset++];
                data2 = trackData[offset++];
                if (eventType == CONTROL_CHANGE)
                {
                    if (data1 == BANK_MSB)
                    {
                        channelBank[channel] = (uint16_t)(((uint16_t)data2 << 7) | (channelBank[channel] & 0x7F));
                        if (channel == track->channel)
                        {
                            track->bank = (uint16_t)(((uint16_t)data2 << 7) | (track->bank & 0x7F));
                        }
                    }
                    else if (data1 == BANK_LSB)
                    {
                        channelBank[channel] = (uint16_t)((channelBank[channel] & 0x3F80) | (uint16_t)(data2 & 0x7F));
                        if (channel == track->channel)
                        {
                            track->bank = (uint16_t)((track->bank & 0x3F80) | (uint16_t)(data2 & 0x7F));
                        }
                    }
                    else if (data1 == 7 && channel == track->channel)
                    {
                        track->volume = data2;
                    }
                    else if (data1 == 10 && channel == track->channel)
                    {
                        track->pan = data2;
                    }
                    /* Store all CCs for this channel except bank selects (handled via per-note bank tracking). */
                    if (channel == track->channel && data1 != BANK_MSB && data1 != BANK_LSB)
                    {
                        result = PV_AddCCEventToTrack(track, currentTick, data1, data2, 0);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                    }
                }
                else if (eventType == PITCH_BEND && channel == track->channel)
                {
                    /* Pitch bend: data1=LSB, data2=MSB. Stored with cc sentinel 0xFF. */
                    result = PV_AddCCEventToTrack(track, currentTick, 0xFF, data1, data2);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                break;
            }
            case PROGRAM_CHANGE:
            case CHANNEL_AFTERTOUCH:
            {
                unsigned char data1;

                if (offset + 1 > trackSize)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_BAD_FILE;
                }
                data1 = trackData[offset++];
                if (eventType == PROGRAM_CHANGE)
                {
                    channelProgram[channel] = data1;
                    if (channel == track->channel)
                    {
                        track->program = data1;
                    }
                }
                break;
            }
            default:
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
        }
    }

    result = PV_FinalizeActiveNotes(track, &activeNotes, currentTick);
    PV_DisposeActiveNotes(&activeNotes);
    return result;
}

static BAEResult PV_LoadMidiBytesIntoDocument(BAERmfEditorDocument *document,
                                              unsigned char const *data,
                                              uint32_t dataSize)
{
    uint32_t headerLength;
    uint16_t trackCount;
    uint16_t division;
    uint32_t offset;
    uint16_t trackIndex;

    if (!document || !data || dataSize < 14)
    {
        return BAE_BAD_FILE;
    }
    if (memcmp(data, "MThd", 4) != 0)
    {
        return BAE_BAD_FILE;
    }
    headerLength = PV_ReadBE32(data + 4);
    if (headerLength < 6 || dataSize < 8 + headerLength)
    {
        return BAE_BAD_FILE;
    }
    trackCount = PV_ReadBE16(data + 10);
    division = PV_ReadBE16(data + 12);
    if ((division & 0x8000) == 0 && division != 0)
    {
        document->ticksPerQuarter = division;
    }
    PV_ClearTempoEvents(document);
    offset = 8 + headerLength;
    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        uint32_t trackLength;
        BAEResult result;

        if (offset + 8 > dataSize || memcmp(data + offset, "MTrk", 4) != 0)
        {
            return BAE_BAD_FILE;
        }
        trackLength = PV_ReadBE32(data + offset + 4);
        offset += 8;
        if (offset + trackLength > dataSize)
        {
            return BAE_BAD_FILE;
        }
        result = PV_LoadMidiTrackIntoDocument(document, data + offset, trackLength);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        offset += trackLength;
    }
    if (document->tempoEventCount > 0 && document->tempoEvents[0].microsecondsPerQuarter > 0)
    {
        document->tempoBPM = 60000000UL / document->tempoEvents[0].microsecondsPerQuarter;
    }
    return (document->trackCount > 0) ? BAE_NO_ERROR : BAE_BAD_FILE;
}

static void PV_DecodeResourceName(char const *rawName, char outName[256])
{
    uint8_t len;

    if (!outName)
    {
        return;
    }
    outName[0] = 0;
    if (!rawName)
    {
        return;
    }
    /* Some paths return Pascal strings, others C strings. */
    len = (uint8_t)rawName[0];
    if (len > 0 && len < 64 && rawName[1] >= 32)
    {
        uint32_t copyLen = len;
        if (copyLen > 255)
        {
            copyLen = 255;
        }
        XBlockMove(rawName + 1, outName, (int32_t)copyLen);
        outName[copyLen] = 0;
        return;
    }
    XStrCpy(outName, rawName);
}

static BAEResult PV_AddEmbeddedSampleVariant(BAERmfEditorDocument *document,
                                             XFILE fileRef,
                                             XLongResourceID instID,
                                             char const *displayName,
                                             unsigned char program,
                                             XShortResourceID sndID,
                                             unsigned char rootKey,
                                             unsigned char lowKey,
                                             unsigned char highKey)
{
    XPTR sndData;
    int32_t sndSize;
    SampleDataInfo sdi;
    XPTR pcmData;
    XPTR pcmOwner;
    GM_Waveform *waveform;
    BAERmfEditorSample *sample;
    BAEResult growResult;

    /* Use the engine's canonical loader for SND/CSND/ESND handling. */
    XFileUseThisResourceFile(fileRef);
    sndData = XGetSoundResourceByID((XLongResourceID)sndID, &sndSize);
    if (!sndData)
    {
        return BAE_BAD_FILE;
    }

    XSetMemory(&sdi, sizeof(sdi), 0);
    pcmData = XGetSamplePtrFromSnd(sndData, &sdi);
    pcmOwner = NULL;
    if (sdi.pMasterPtr && sdi.pMasterPtr != sndData)
    {
        pcmOwner = sdi.pMasterPtr;
    }
    if (!pcmData)
    {
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }

    waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
    if (!waveform)
    {
        XDisposePtr(sndData);
        return BAE_MEMORY_ERR;
    }
    XSetMemory(waveform, sizeof(*waveform), 0);
    XTranslateFromSampleDataToWaveform(&sdi, waveform);
    {
        int32_t pcmSize;

        pcmSize = (int32_t)(sdi.frames * (sdi.bitSize / 8) * sdi.channels);
        if (pcmSize > 0)
        {
            XPTR ownedPcm = XNewPtr(pcmSize);
            if (!ownedPcm)
            {
                XDisposePtr((XPTR)waveform);
                if (pcmOwner)
                {
                    XDisposePtr(pcmOwner);
                }
                XDisposePtr(sndData);
                return BAE_MEMORY_ERR;
            }
            XBlockMove(pcmData, ownedPcm, pcmSize);
            waveform->theWaveform = (SBYTE *)ownedPcm;
        }
    }
    /* theWaveform now contains raw PCM copy; keep compression metadata aligned with data format */
    waveform->compressionType = C_NONE;
    /* When INST midiRootKey is 0 ("no override"), the engine relies on the SND's baseFrequency
     * for pitch calibration.  Recover the original baseFrequency from sdi.baseKey so that
     * sample->rootKey and the regenerated SND both carry the correct root note. */
    if (rootKey == 0)
    {
        if (sdi.baseKey > 0 && sdi.baseKey <= 127)
        {
            rootKey = (unsigned char)sdi.baseKey;
        }
        else
        {
            rootKey = 60; /* safe default: middle C */
        }
    }
    waveform->baseMidiPitch = rootKey;
    if (pcmOwner)
    {
        XDisposePtr(pcmOwner);
    }
    XDisposePtr(sndData);

    growResult = PV_GrowBuffer((void **)&document->samples,
                               &document->sampleCapacity,
                               sizeof(BAERmfEditorSample),
                               document->sampleCount + 1);
    if (growResult != BAE_NO_ERROR)
    {
        GM_FreeWaveform(waveform);
        return growResult;
    }
    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = waveform;
    sample->program = program;
    sample->instID = (uint32_t)instID;
    sample->rootKey = rootKey;
    sample->lowKey = lowKey;
    sample->highKey = highKey;
    sample->sourceCompressionType = sdi.compressionType;
    if (displayName && displayName[0])
    {
        sample->displayName = PV_DuplicateString(displayName);
    }
    else
    {
        char buf[32];
        sprintf(buf, "Sample P%u", (unsigned)program);
        sample->displayName = PV_DuplicateString(buf);
    }
    sample->sourcePath = NULL;
    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    document->sampleCount++;
    return BAE_NO_ERROR;
}

/* Extract INST + SND resources from an open RMF resource file and add them
   as editable samples in the document. Includes all key-split variants. */
static void PV_LoadEmbeddedSamplesFromRmf(BAERmfEditorDocument *document, XFILE fileRef)
{
    int32_t instIndex;

    for (instIndex = 0; ; ++instIndex)
    {
        XLongResourceID instID;
        XPTR instData;
        int32_t instSize;
        InstrumentResource *inst;
        int16_t splitCount;
        int16_t splitIndex;
        XShortResourceID baseSndID;
        int16_t baseRootKey;
        unsigned char program;
        char rawName[256];
        char instName[256];

        rawName[0] = 0;
        instData = XGetIndexedFileResource(fileRef, ID_INST, &instID, instIndex, rawName, &instSize);
        if (!instData)
        {
            break;
        }
        if (instSize < (int32_t)sizeof(InstrumentResource) || instID < 256)
        {
            XDisposePtr(instData);
            continue;
        }

        PV_DecodeResourceName(rawName, instName);
        inst = (InstrumentResource *)instData;
        baseSndID = (XShortResourceID)XGetShort(&inst->sndResourceID);
        baseRootKey = (int16_t)XGetShort(&inst->midiRootKey);
        splitCount = (int16_t)XGetShort(&inst->keySplitCount);
        program = (unsigned char)(instID % 128);

        /* Detect whether this INST uses miscParameter1 as the per-sample root key
         * (useSoundModifierAsRootKey), or whether the root key comes from the SND's
         * own baseFrequency.  The two paths require different loading strategies:
         *   useSoundModifierAsRootKey=TRUE  → miscParameter1 (split or INST) IS the root key
         *   useSoundModifierAsRootKey=FALSE → SND's baseFrequency is the root key
         *     (pass rootKey=0 to PV_AddEmbeddedSampleVariant so it falls back to sdi.baseKey)
         * NOTE: midiRootKey 0 and 60 are both no-ops in the engine (shift by 0 semitones).
         * For non-trivial masterRootKey values the effective root would be
         * masterRootKey + baseMidiPitch - 60, but that edge case is uncommon in practice.
         */
        {
            XBOOL useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
            int16_t instMiscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

        if (splitCount > 0)
        {
            for (splitIndex = 0; splitIndex < splitCount; ++splitIndex)
            {
                KeySplit split;
                unsigned char splitRootForLoad;

                XGetKeySplitFromPtr(inst, splitIndex, &split);
                if (useSoundModifierAsRootKey)
                {
                    /* miscParameter1 is the authoritative per-split root key */
                    int16_t splitRoot = split.miscParameter1;
                    if (splitRoot < 0 || splitRoot > 127)
                    {
                        splitRoot = baseRootKey;
                    }
                    splitRootForLoad = PV_ClampMidi7Bit(splitRoot);
                }
                else
                {
                    /* Root key comes from the SND's baseFrequency; pass 0 so
                     * PV_AddEmbeddedSampleVariant falls back to sdi.baseKey. */
                    splitRootForLoad = 0;
                }
                if (PV_AddEmbeddedSampleVariant(document,
                                                fileRef,
                                                instID,
                                                instName,
                                                program,
                                                split.sndResourceID,
                                                splitRootForLoad,
                                                PV_ClampMidi7Bit((int32_t)split.lowMidi),
                                                PV_ClampMidi7Bit((int32_t)split.highMidi)) != BAE_NO_ERROR)
                {
                    BAE_STDERR("[RMF] INST ID=%ld split=%d failed to load sndID=%d\n",
                               (long)instID, (int)splitIndex, (int)split.sndResourceID);
                }
            }
        }
        else
        {
            unsigned char nonSplitRootForLoad;
            if (useSoundModifierAsRootKey)
            {
                /* miscParameter1 holds the root key override for non-split instruments */
                nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
            }
            else
            {
                /* Root key comes from the SND's baseFrequency; pass 0 so
                 * PV_AddEmbeddedSampleVariant falls back to sdi.baseKey. */
                nonSplitRootForLoad = 0;
            }
            if (PV_AddEmbeddedSampleVariant(document,
                                            fileRef,
                                            instID,
                                            instName,
                                            program,
                                            baseSndID,
                                            nonSplitRootForLoad,
                                            0,
                                            127) != BAE_NO_ERROR)
            {
                BAE_STDERR("[RMF] INST ID=%ld failed to load base sndID=%d\n",
                           (long)instID, (int)baseSndID);
            }
        }
        }
        XDisposePtr(instData);
    }
}

/* Decode raw MIDI resource data for encrypted/compressed types (ecmi, emid, cmid).
 * Takes ownership of 'raw'. Returns decoded/decompressed MIDI data or NULL on failure.
 * Updates *ioSize with the final decoded size. */
static XPTR PV_DecodeMidiData(XPTR raw, XResourceType rtype, int32_t *ioSize)
{
    XPTR dec;

    if (!raw)
    {
        return NULL;
    }
    if (rtype == ID_ECMI)
    {
        BAE_STDERR("[RMF] ecmi: raw size=%ld, first bytes: %02x %02x %02x %02x\n",
                   (long)*ioSize,
                   ((unsigned char*)raw)[0], ((unsigned char*)raw)[1],
                   ((unsigned char*)raw)[2], ((unsigned char*)raw)[3]);
        XDecryptData(raw, (uint32_t)*ioSize);
        BAE_STDERR("[RMF] ecmi: after decrypt first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   ((unsigned char*)raw)[0], ((unsigned char*)raw)[1],
                   ((unsigned char*)raw)[2], ((unsigned char*)raw)[3],
                   ((unsigned char*)raw)[4], ((unsigned char*)raw)[5],
                   ((unsigned char*)raw)[6], ((unsigned char*)raw)[7]);
        dec = XDecompressPtr(raw, (uint32_t)*ioSize, TRUE);
        BAE_STDERR("[RMF] ecmi: XDecompressPtr returned %s, size=%ld\n",
                   dec ? "non-NULL" : "NULL", dec ? (long)XGetPtrSize(dec) : 0L);
        XDisposePtr(raw);
        if (dec)
        {
            *ioSize = (int32_t)XGetPtrSize(dec);
            BAE_STDERR("[RMF] ecmi: first 4 decompressed bytes: %02x %02x %02x %02x\n",
                       ((unsigned char*)dec)[0], ((unsigned char*)dec)[1],
                       ((unsigned char*)dec)[2], ((unsigned char*)dec)[3]);
        }
        return dec;
    }
    if (rtype == ID_EMID)
    {
        XDecryptData(raw, (uint32_t)*ioSize);
        return raw;
    }
    if (rtype == ID_CMID)
    {
        dec = XDecompressPtr(raw, (uint32_t)*ioSize, TRUE);
        XDisposePtr(raw);
        if (dec)
        {
            *ioSize = (int32_t)XGetPtrSize(dec);
        }
        return dec;
    }
    return raw;
}

static BAEResult PV_LoadRmfFileIntoDocument(BAERmfEditorDocument *document, BAEPathName filePath)
{
    XFILENAME name;
    XFILE fileRef;
    SongResource *songResource;
    SongResource_Info *songInfo;
    XPTR midiData;
    int32_t songSize;
    int32_t midiSize;
    BAEResult result;
    XLongResourceID songID;
    XShortResourceID objectResourceID;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }
    BAE_STDERR("[RMF] Loading RMF file: %s\n", filePath);
    result = BAE_BAD_FILE;
    songResource = NULL;
    songInfo = NULL;
    midiData = NULL;
    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenResource(&name, TRUE);
    if (!fileRef)
    {
        BAE_STDERR("[RMF] XFileOpenResource failed\n");
        return BAE_FILE_IO_ERROR;
    }
    result = PV_CaptureOriginalResourcesFromFile(document, fileRef);
    if (result != BAE_NO_ERROR)
    {
        BAE_STDERR("[RMF] Failed to capture original resource map result=%d\n", (int)result);
        XFileClose(fileRef);
        return result;
    }
    songResource = (SongResource *)XGetIndexedFileResource(fileRef, ID_SONG, &songID, 0, NULL, &songSize);
    if (!songResource)
    {
        BAE_STDERR("[RMF] No SONG resource found\n");
        XFileClose(fileRef);
        return BAE_BAD_FILE;
    }
    BAE_STDERR("[RMF] SONG resource found, ID=%ld, size=%ld\n", (long)songID, (long)songSize);
    songInfo = XGetSongResourceInfo(songResource, songSize);
    if (!songInfo)
    {
        BAE_STDERR("[RMF] XGetSongResourceInfo failed\n");
        XDisposePtr(songResource);
        XFileClose(fileRef);
        return BAE_BAD_FILE;
    }
    if (songInfo->songTempo > 0)
    {
        document->tempoBPM = (uint32_t)songInfo->songTempo;
    }
    document->maxMidiNotes = songInfo->maxMidiNotes;
    document->maxEffects = songInfo->maxEffects;
    document->mixLevel = songInfo->mixLevel;
    document->songVolume = songInfo->songVolume;
    document->reverbType = (BAEReverbType)songInfo->reverbType;
    if (songInfo->title)
    {
        BAERmfEditorDocument_SetInfo(document, TITLE_INFO, songInfo->title);
    }
    if (songInfo->composer)
    {
        BAERmfEditorDocument_SetInfo(document, COMPOSER_INFO, songInfo->composer);
    }
    if (songInfo->copyright)
    {
        BAERmfEditorDocument_SetInfo(document, COPYRIGHT_INFO, songInfo->copyright);
    }
    objectResourceID = songInfo->objectResourceID;
    document->originalSongID = songID;
    document->originalObjectResourceID = (XLongResourceID)objectResourceID;
    document->originalMidiType = ID_MIDI;
    BAE_STDERR("[RMF] objectResourceID=%d, tempo=%ld\n", (int)objectResourceID, (long)songInfo->songTempo);
    midiData = XGetFileResource(fileRef, ID_MIDI, objectResourceID, NULL, &midiSize);
    if (midiData)
    {
        document->originalMidiType = ID_MIDI;
    }
    if (!midiData)
    {
        BAE_STDERR("[RMF] No ID_MIDI with objectResourceID=%d, trying ID_MIDI_OLD\n", (int)objectResourceID);
        midiData = XGetFileResource(fileRef, ID_MIDI_OLD, objectResourceID, NULL, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_MIDI_OLD;
        }
    }
    if (!midiData)
    {
        BAE_STDERR("[RMF] No ID_MIDI_OLD with objectResourceID=%d, trying ID_ECMI\n", (int)objectResourceID);
        midiData = PV_DecodeMidiData(XGetFileResource(fileRef, ID_ECMI, objectResourceID, NULL, &midiSize), ID_ECMI, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_ECMI;
        }
    }
    if (!midiData)
    {
        BAE_STDERR("[RMF] No ID_ECMI with objectResourceID=%d, trying ID_EMID\n", (int)objectResourceID);
        midiData = PV_DecodeMidiData(XGetFileResource(fileRef, ID_EMID, objectResourceID, NULL, &midiSize), ID_EMID, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_EMID;
        }
    }
    if (!midiData)
    {
        BAE_STDERR("[RMF] No ID_EMID with objectResourceID=%d, trying ID_CMID\n", (int)objectResourceID);
        midiData = PV_DecodeMidiData(XGetFileResource(fileRef, ID_CMID, objectResourceID, NULL, &midiSize), ID_CMID, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_CMID;
        }
    }
    if (midiData)
    {
        BAE_STDERR("[RMF] Got MIDI data, size=%ld\n", (long)midiSize);
        result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
        BAE_STDERR("[RMF] PV_LoadMidiBytesIntoDocument result=%d, trackCount=%u\n", (int)result, document->trackCount);
    }
    else
    {
        BAE_STDERR("[RMF] No MIDI data found by objectResourceID\n");
    }
    if (result != BAE_NO_ERROR)
    {
        XLongResourceID fallbackID;
        int32_t fallbackIndex;

        BAE_STDERR("[RMF] Primary MIDI load failed, trying indexed fallback scan\n");
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = XGetIndexedFileResource(fileRef, ID_MIDI, &fallbackID, fallbackIndex++, NULL, &midiSize);
            if (!midiData)
            {
                BAE_STDERR("[RMF] No more indexed ID_MIDI resources\n");
                break;
            }
            BAE_STDERR("[RMF] Trying indexed ID_MIDI[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            BAE_STDERR("[RMF] indexed ID_MIDI result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_MIDI;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = XGetIndexedFileResource(fileRef, ID_MIDI_OLD, &fallbackID, fallbackIndex++, NULL, &midiSize);
            if (!midiData)
            {
                BAE_STDERR("[RMF] No more indexed ID_MIDI_OLD resources\n");
                break;
            }
            BAE_STDERR("[RMF] Trying indexed ID_MIDI_OLD[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            BAE_STDERR("[RMF] indexed ID_MIDI_OLD result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_MIDI_OLD;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = PV_DecodeMidiData(XGetIndexedFileResource(fileRef, ID_ECMI, &fallbackID, fallbackIndex++, NULL, &midiSize), ID_ECMI, &midiSize);
            if (!midiData)
            {
                BAE_STDERR("[RMF] No more indexed ID_ECMI resources\n");
                break;
            }
            BAE_STDERR("[RMF] Trying indexed ID_ECMI[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            BAE_STDERR("[RMF] indexed ID_ECMI result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_ECMI;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = PV_DecodeMidiData(XGetIndexedFileResource(fileRef, ID_EMID, &fallbackID, fallbackIndex++, NULL, &midiSize), ID_EMID, &midiSize);
            if (!midiData)
            {
                BAE_STDERR("[RMF] No more indexed ID_EMID resources\n");
                break;
            }
            BAE_STDERR("[RMF] Trying indexed ID_EMID[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            BAE_STDERR("[RMF] indexed ID_EMID result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_EMID;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = PV_DecodeMidiData(XGetIndexedFileResource(fileRef, ID_CMID, &fallbackID, fallbackIndex++, NULL, &midiSize), ID_CMID, &midiSize);
            if (!midiData)
            {
                BAE_STDERR("[RMF] No more indexed ID_CMID resources\n");
                break;
            }
            BAE_STDERR("[RMF] Trying indexed ID_CMID[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            BAE_STDERR("[RMF] indexed ID_CMID result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_CMID;
            }
        }
    }
    XDisposeSongResourceInfo(songInfo);
    XDisposePtr(songResource);
    if (midiData)
    {
        XDisposePtr(midiData);
    }
    /* Extract embedded SND/INST samples into the document sample list */
    if (result == BAE_NO_ERROR)
    {
        PV_LoadEmbeddedSamplesFromRmf(document, fileRef);
        document->loadedFromRmf = TRUE;
        document->isPristine = TRUE;
    }
    XFileClose(fileRef);
    return result;
}

static BAEResult PV_GetAvailableResourceID(XFILE fileRef,
                                          XResourceType resourceType,
                                          XLongResourceID startingID,
                                          XLongResourceID *outResourceID)
{
    XFILERESOURCEMAP map;
    int32_t nextOffset;
    int32_t resourceCount;
    int32_t resourceIndex;
    XLongResourceID nextID;

    if (!fileRef || !outResourceID)
    {
        return BAE_PARAM_ERR;
    }
    *outResourceID = 0;
    if (XGetUniqueFileResourceID(fileRef, resourceType, outResourceID) == 0 && *outResourceID != 0)
    {
        return BAE_NO_ERROR;
    }
    nextID = (startingID > 0) ? startingID : 1;
    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0 ||
        XGetLong(&map.mapID) != XFILERESOURCE_ID)
    {
        return BAE_FILE_IO_ERROR;
    }
    nextOffset = (int32_t)sizeof(XFILERESOURCEMAP);
    resourceCount = (int32_t)XGetLong(&map.totalResources);
    for (resourceIndex = 0; resourceIndex < resourceCount; ++resourceIndex)
    {
        int32_t headerNext;
        int32_t data;

        if (XFileSetPosition(fileRef, nextOffset) != 0 ||
            XFileRead(fileRef, &headerNext, (int32_t)sizeof(int32_t)) != 0 ||
            XFileRead(fileRef, &data, (int32_t)sizeof(int32_t)) != 0)
        {
            return BAE_FILE_IO_ERROR;
        }
        headerNext = (int32_t)XGetLong(&headerNext);
        if ((XResourceType)XGetLong(&data) == resourceType)
        {
            if (XFileRead(fileRef, &data, (int32_t)sizeof(int32_t)) != 0)
            {
                return BAE_FILE_IO_ERROR;
            }
            data = (int32_t)XGetLong(&data);
            if ((XLongResourceID)data >= nextID)
            {
                if (data == 0x7FFFFFFF)
                {
                    return BAE_FILE_IO_ERROR;
                }
                nextID = (XLongResourceID)(data + 1);
            }
        }
        if (resourceIndex < (resourceCount - 1))
        {
            if (headerNext <= nextOffset)
            {
                return BAE_FILE_IO_ERROR;
            }
            nextOffset = headerNext;
        }
    }
    *outResourceID = nextID;
    return BAE_NO_ERROR;
}

static BAEResult PV_EnsureResourceFileReady(XFILE fileRef)
{
    XFILERESOURCEMAP map;

    if (!fileRef)
    {
        return BAE_PARAM_ERR;
    }
    if (XFileSetLength(fileRef, 0) != 0)
    {
        return BAE_FILE_IO_ERROR;
    }
    XFileFreeResourceCache(fileRef);
    XPutLong(&map.mapID, XFILERESOURCE_ID);
    XPutLong(&map.version, 1);
    XPutLong(&map.totalResources, 0);
    if (XFileSetPosition(fileRef, 0L) != 0)
    {
        return BAE_FILE_IO_ERROR;
    }
    if (XFileWrite(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_FILE_IO_ERROR;
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_PrepareResourceFilePath(XFILENAME *name)
{
    XFILE fileRef;
    XFILERESOURCEMAP map;
    XBOOL isValid;

    if (!name)
    {
        return BAE_PARAM_ERR;
    }
    isValid = FALSE;
    fileRef = XFileOpenForRead(name);
    if (fileRef)
    {
        if (XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) == 0 &&
            XGetLong(&map.mapID) == XFILERESOURCE_ID)
        {
            isValid = TRUE;
        }
        XFileClose(fileRef);
    }
    if (isValid)
    {
        return BAE_NO_ERROR;
    }
    fileRef = XFileOpenForWrite(name, TRUE);
    if (!fileRef)
    {
        return BAE_FILE_IO_ERROR;
    }
    if (XFileSetLength(fileRef, 0) != 0)
    {
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XPutLong(&map.mapID, XFILERESOURCE_ID);
    XPutLong(&map.version, 1);
    XPutLong(&map.totalResources, 0);
    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);
    return BAE_NO_ERROR;
}

static BAEResult PV_WriteOriginalResources(BAERmfEditorDocument const *document, XFILE fileRef)
{
    uint32_t index;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    for (index = 0; index < document->originalResourceCount; ++index)
    {
        BAERmfEditorResourceEntry const *entry;

        entry = &document->originalResources[index];
        if (!entry->data || entry->size < 0)
        {
            return BAE_BAD_FILE;
        }
        if (XAddFileResource(fileRef,
                             entry->type,
                             entry->id,
                             entry->pascalName,
                             entry->data,
                             entry->size) != 0)
        {
            return BAE_FILE_IO_ERROR;
        }
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_EncodeMidiForResourceType(XResourceType resourceType,
                                              ByteBuffer const *plainMidi,
                                              XPTR *outData,
                                              int32_t *outSize)
{
    XPTR encoded;

    if (!plainMidi || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outData = NULL;
    *outSize = 0;

    if (resourceType == ID_MIDI || resourceType == ID_MIDI_OLD)
    {
        encoded = XNewPtr((int32_t)plainMidi->size);
        if (!encoded)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(plainMidi->data, encoded, (int32_t)plainMidi->size);
        *outData = encoded;
        *outSize = (int32_t)plainMidi->size;
        return BAE_NO_ERROR;
    }

    if (resourceType == ID_CMID || resourceType == ID_ECMI)
    {
        int32_t compressedSize;

        encoded = NULL;
        compressedSize = XCompressPtr(&encoded,
                                      (XPTR)plainMidi->data,
                                      plainMidi->size,
                                      X_RAW,
                                      NULL,
                                      NULL);
        if (compressedSize <= 0 || !encoded)
        {
            if (encoded)
            {
                XDisposePtr(encoded);
            }
            return BAE_BAD_FILE;
        }
        if (resourceType == ID_ECMI)
        {
            XEncryptData(encoded, (uint32_t)compressedSize);
        }
        *outData = encoded;
        *outSize = compressedSize;
        return BAE_NO_ERROR;
    }

    if (resourceType == ID_EMID)
    {
        encoded = XNewPtr((int32_t)plainMidi->size);
        if (!encoded)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(plainMidi->data, encoded, (int32_t)plainMidi->size);
        XEncryptData(encoded, plainMidi->size);
        *outData = encoded;
        *outSize = (int32_t)plainMidi->size;
        return BAE_NO_ERROR;
    }

    return BAE_PARAM_ERR;
}

static int PV_CompareMidiEvents(void const *left, void const *right)
{
    MidiEventRecord const *a;
    MidiEventRecord const *b;

    a = (MidiEventRecord const *)left;
    b = (MidiEventRecord const *)right;
    if (a->tick < b->tick)
    {
        return -1;
    }
    if (a->tick > b->tick)
    {
        return 1;
    }
    if (a->order < b->order)
    {
        return -1;
    }
    if (a->order > b->order)
    {
        return 1;
    }
    return 0;
}

static BAEResult PV_AppendMetaEvent(ByteBuffer *buffer, uint32_t delta, unsigned char type, void const *data, uint32_t length)
{
    BAEResult result;

    result = PV_ByteBufferAppendVLQ(buffer, delta);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(buffer, 0xFF);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(buffer, type);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendVLQ(buffer, length);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    return PV_ByteBufferAppend(buffer, data, length);
}

static BAEResult PV_BuildTempoTrack(BAERmfEditorDocument *document, ByteBuffer *trackData)
{
    uint32_t eventIndex;
    uint32_t previousTick;
    BAEResult result;

    previousTick = 0;
    if (document->tempoEventCount > 0)
    {
        for (eventIndex = 0; eventIndex < document->tempoEventCount; ++eventIndex)
        {
            unsigned char tempoBytes[3];
            BAERmfEditorTempoEvent const *tempoEvent;
            uint32_t delta;

            tempoEvent = &document->tempoEvents[eventIndex];
            if (tempoEvent->microsecondsPerQuarter == 0)
            {
                continue;
            }
            delta = tempoEvent->tick - previousTick;
            tempoBytes[0] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 16) & 0xFF);
            tempoBytes[1] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 8) & 0xFF);
            tempoBytes[2] = (unsigned char)(tempoEvent->microsecondsPerQuarter & 0xFF);
            result = PV_AppendMetaEvent(trackData, delta, 0x51, tempoBytes, 3);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
            previousTick = tempoEvent->tick;
        }
    }
    else
    {
        unsigned char tempoBytes[3];
        uint32_t microsecondsPerQuarter;

        microsecondsPerQuarter = 60000000UL / document->tempoBPM;
        tempoBytes[0] = (unsigned char)((microsecondsPerQuarter >> 16) & 0xFF);
        tempoBytes[1] = (unsigned char)((microsecondsPerQuarter >> 8) & 0xFF);
        tempoBytes[2] = (unsigned char)(microsecondsPerQuarter & 0xFF);
        result = PV_AppendMetaEvent(trackData, 0, 0x51, tempoBytes, 3);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    return PV_AppendMetaEvent(trackData, 0, 0x2F, NULL, 0);
}

static BAEResult PV_BuildTrackData(BAERmfEditorTrack const *track, ByteBuffer *trackData)
{
    MidiEventRecord *events;
    uint32_t eventCount;
    uint32_t eventIndex;
    uint32_t noteIndex;
    uint32_t previousTick;
    uint16_t currentBank;
    unsigned char currentProgram;
    BAEResult result;

    events = NULL;
    eventCount = (track->noteCount * 2) + track->ccEventCount;
    if (track->name && track->name[0])
    {
        result = PV_AppendMetaEvent(trackData, 0, 0x03, track->name, (uint32_t)strlen(track->name));
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    result = PV_ByteBufferAppendVLQ(trackData, 0);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F)));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, BANK_MSB);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)((track->bank >> 7) & 0x7F));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendVLQ(trackData, 0);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F)));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, BANK_LSB);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(track->bank & 0x7F));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendVLQ(trackData, 0);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F)));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, 7);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, track->volume);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendVLQ(trackData, 0);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F)));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, 10);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, track->pan);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendVLQ(trackData, 0);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(PROGRAM_CHANGE | (track->channel & 0x0F)));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(trackData, track->program);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    currentBank = track->bank;
    currentProgram = track->program;

    if (eventCount)
    {
        events = (MidiEventRecord *)XNewPtr((int32_t)(sizeof(MidiEventRecord) * eventCount));
        if (!events)
        {
            return BAE_MEMORY_ERR;
        }
        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote const *note;
            MidiEventRecord *noteOn;
            MidiEventRecord *noteOff;
            unsigned char transposedNote;

            note = &track->notes[noteIndex];
            noteOn = &events[noteIndex * 2];
            noteOff = &events[(noteIndex * 2) + 1];
            transposedNote = PV_ClampMidi7Bit((int32_t)note->note + (int32_t)track->transpose);

            noteOn->tick = note->startTick;
            noteOn->order = 2;
            noteOn->status = (unsigned char)(NOTE_ON | (track->channel & 0x0F));
            noteOn->data1 = transposedNote;
            noteOn->data2 = note->velocity;
            noteOn->bank = note->bank;
            noteOn->program = note->program;
            noteOn->applyProgram = 1;

            noteOff->tick = note->startTick + note->durationTicks;
            noteOff->order = 0;
            noteOff->status = (unsigned char)(NOTE_OFF | (track->channel & 0x0F));
            noteOff->data1 = transposedNote;
            noteOff->data2 = 0;
            noteOff->bank = note->bank;
            noteOff->program = note->program;
            noteOff->applyProgram = 0;
        }
        for (eventIndex = 0; eventIndex < track->ccEventCount; ++eventIndex)
        {
            MidiEventRecord *ccEvent;

            ccEvent = &events[(track->noteCount * 2) + eventIndex];
            ccEvent->tick = track->ccEvents[eventIndex].tick;
            ccEvent->order = 1;
            if (track->ccEvents[eventIndex].cc == 0xFF)
            {
                /* Pitch bend: sentinel cc=0xFF, value=LSB, data2=MSB */
                ccEvent->status = (unsigned char)(PITCH_BEND | (track->channel & 0x0F));
                ccEvent->data1 = track->ccEvents[eventIndex].value;
                ccEvent->data2 = track->ccEvents[eventIndex].data2;
            }
            else
            {
                ccEvent->status = (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F));
                ccEvent->data1 = track->ccEvents[eventIndex].cc;
                ccEvent->data2 = track->ccEvents[eventIndex].value;
            }
            ccEvent->bank = track->bank;
            ccEvent->program = track->program;
            ccEvent->applyProgram = 0;
        }
        qsort(events, eventCount, sizeof(MidiEventRecord), PV_CompareMidiEvents);
        previousTick = 0;
        for (eventIndex = 0; eventIndex < eventCount; ++eventIndex)
        {
            MidiEventRecord const *event;
            uint32_t delta;

            event = &events[eventIndex];
            delta = event->tick - previousTick;
            if (event->applyProgram)
            {
                if (event->bank != currentBank)
                {
                    result = PV_ByteBufferAppendVLQ(trackData, delta);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F)));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, BANK_MSB);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)((event->bank >> 7) & 0x7F));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendVLQ(trackData, 0);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F)));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, BANK_LSB);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(event->bank & 0x7F));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    delta = 0;
                    currentBank = event->bank;
                }
                if (event->program != currentProgram)
                {
                    result = PV_ByteBufferAppendVLQ(trackData, delta);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(PROGRAM_CHANGE | (track->channel & 0x0F)));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, event->program);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    delta = 0;
                    currentProgram = event->program;
                }
            }
            result = PV_ByteBufferAppendVLQ(trackData, delta);
            if (result != BAE_NO_ERROR)
            {
                XDisposePtr(events);
                return result;
            }
            result = PV_ByteBufferAppendByte(trackData, event->status);
            if (result != BAE_NO_ERROR)
            {
                XDisposePtr(events);
                return result;
            }
            result = PV_ByteBufferAppendByte(trackData, event->data1);
            if (result != BAE_NO_ERROR)
            {
                XDisposePtr(events);
                return result;
            }
            result = PV_ByteBufferAppendByte(trackData, event->data2);
            if (result != BAE_NO_ERROR)
            {
                XDisposePtr(events);
                return result;
            }
            previousTick = event->tick;
        }
        XDisposePtr(events);
    }

    return PV_AppendMetaEvent(trackData, 0, 0x2F, NULL, 0);
}

static BAEResult PV_BuildMidiFile(BAERmfEditorDocument *document, ByteBuffer *output)
{
    ByteBuffer tempoTrack;
    ByteBuffer trackData;
    BAEResult result;
    uint32_t trackIndex;
    uint16_t trackCount;

    XSetMemory(&tempoTrack, sizeof(tempoTrack), 0);
    XSetMemory(&trackData, sizeof(trackData), 0);
    result = PV_ByteBufferAppend(output, "MThd", 4);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendBE32(output, 6);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendBE16(output, 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    trackCount = (uint16_t)(document->trackCount + 1);
    result = PV_ByteBufferAppendBE16(output, trackCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendBE16(output, document->ticksPerQuarter);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_BuildTempoTrack(document, &tempoTrack);
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&tempoTrack);
        return result;
    }
    result = PV_ByteBufferAppend(output, "MTrk", 4);
    if (result == BAE_NO_ERROR)
    {
        result = PV_ByteBufferAppendBE32(output, tempoTrack.size);
    }
    if (result == BAE_NO_ERROR)
    {
        result = PV_ByteBufferAppend(output, tempoTrack.data, tempoTrack.size);
    }
    PV_ByteBufferDispose(&tempoTrack);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        XSetMemory(&trackData, sizeof(trackData), 0);
        result = PV_BuildTrackData(&document->tracks[trackIndex], &trackData);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&trackData);
            return result;
        }
        result = PV_ByteBufferAppend(output, "MTrk", 4);
        if (result == BAE_NO_ERROR)
        {
            result = PV_ByteBufferAppendBE32(output, trackData.size);
        }
        if (result == BAE_NO_ERROR)
        {
            result = PV_ByteBufferAppend(output, trackData.data, trackData.size);
        }
        PV_ByteBufferDispose(&trackData);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_AddSampleResources(BAERmfEditorDocument *document, XFILE fileRef)
{
    uint32_t index;
    XShortResourceID *sampleSndIDs;
    XLongResourceID *sampleInstIDs;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    if (document->sampleCount == 0)
    {
        return BAE_NO_ERROR;
    }

    sampleSndIDs = (XShortResourceID *)XNewPtr((int32_t)(document->sampleCount * sizeof(XShortResourceID)));
    sampleInstIDs = (XLongResourceID *)XNewPtr((int32_t)(document->sampleCount * sizeof(XLongResourceID)));
    if (!sampleSndIDs || !sampleInstIDs)
    {
        if (sampleSndIDs)
        {
            XDisposePtr((XPTR)sampleSndIDs);
        }
        if (sampleInstIDs)
        {
            XDisposePtr((XPTR)sampleInstIDs);
        }
        return BAE_MEMORY_ERR;
    }
    XSetMemory(sampleSndIDs, (int32_t)(document->sampleCount * sizeof(XShortResourceID)), 0);
    XSetMemory(sampleInstIDs, (int32_t)(document->sampleCount * sizeof(XLongResourceID)), 0);

    for (index = 0; index < document->sampleCount; ++index)
    {
        BAERmfEditorSample const *sample;
        XLongResourceID sndID;
        XPTR sndResource;
        OPErr opErr;
        BAEResult result;
        char pascalName[256];
        GM_Waveform writeWaveform;
        int32_t writeSampleRate;
        int32_t bytesPerFrame;
        uint32_t maxFramesBySize;
        int32_t loopStart;
        int32_t loopEnd;

        sample = &document->samples[index];
        result = PV_CreatePascalName(sample->displayName ? sample->displayName : sample->sourcePath, pascalName);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        if (PV_GetAvailableResourceID(fileRef, ID_SND, 1, &sndID) != BAE_NO_ERROR)
        {
            return BAE_FILE_IO_ERROR;
        }
        BAE_STDERR("[RMF Save] Sample[%u] program=%u waveform=%p theWaveform=%p waveSize=%ld\n",
                   (unsigned)index, (unsigned)sample->program,
                   (void *)sample->waveform,
                   sample->waveform ? (void *)sample->waveform->theWaveform : NULL,
                   sample->waveform ? (long)sample->waveform->waveSize : 0L);
        if (!sample->waveform)
        {
            return BAE_BAD_FILE;
        }
        if (!sample->waveform->theWaveform)
        {
            return BAE_BAD_FILE;
        }
        writeWaveform = *sample->waveform;

        if ((writeWaveform.bitSize != 8 && writeWaveform.bitSize != 16) ||
            (writeWaveform.channels != 1 && writeWaveform.channels != 2))
        {
            return BAE_BAD_FILE;
        }

        bytesPerFrame = (int32_t)((writeWaveform.bitSize / 8) * writeWaveform.channels);
        if (bytesPerFrame <= 0)
        {
            return BAE_BAD_FILE;
        }

        if (writeWaveform.waveFrames == 0 && writeWaveform.waveSize > 0)
        {
            writeWaveform.waveFrames = (uint32_t)(writeWaveform.waveSize / bytesPerFrame);
        }
        if (writeWaveform.waveSize <= 0 && writeWaveform.waveFrames > 0)
        {
            writeWaveform.waveSize = (int32_t)(writeWaveform.waveFrames * (uint32_t)bytesPerFrame);
        }
        if (writeWaveform.waveFrames == 0 || writeWaveform.waveSize <= 0)
        {
            return BAE_BAD_FILE;
        }

        maxFramesBySize = (uint32_t)(writeWaveform.waveSize / bytesPerFrame);
        if (maxFramesBySize == 0)
        {
            return BAE_BAD_FILE;
        }
        if (writeWaveform.waveFrames > maxFramesBySize)
        {
            writeWaveform.waveFrames = maxFramesBySize;
        }
        writeWaveform.waveSize = (int32_t)(writeWaveform.waveFrames * (uint32_t)bytesPerFrame);

        writeSampleRate = writeWaveform.sampledRate;
        if (writeSampleRate < (4000L << 16))
        {
            if (writeSampleRate >= 4000L && writeSampleRate <= 384000L)
            {
                /* Legacy/mixed paths can carry raw Hz instead of 16.16 fixed-point. */
                writeSampleRate <<= 16;
            }
            else
            {
                writeSampleRate = 44100L << 16;
            }
            writeWaveform.sampledRate = writeSampleRate;
        }

        loopStart = (int32_t)writeWaveform.startLoop;
        loopEnd = (int32_t)writeWaveform.endLoop;
        if (loopStart < 0 || loopStart >= (int32_t)writeWaveform.waveFrames ||
            loopEnd <= loopStart || loopEnd > (int32_t)writeWaveform.waveFrames)
        {
            loopStart = 0;
            loopEnd = 0;
        }
        writeWaveform.startLoop = (uint32_t)loopStart;
        writeWaveform.endLoop = (uint32_t)loopEnd;

        BAE_STDERR("[RMF Save] Sample[%u] rate raw=0x%08lx write=0x%08lx\n",
                   (unsigned)index,
                   (unsigned long)(uint32_t)sample->waveform->sampledRate,
                   (unsigned long)(uint32_t)writeWaveform.sampledRate);
        BAE_STDERR("[RMF Save] Sample[%u] frames=%u size=%ld bits=%u ch=%u loop=%u-%u\n",
                   (unsigned)index,
                   (unsigned)writeWaveform.waveFrames,
                   (long)writeWaveform.waveSize,
                   (unsigned)writeWaveform.bitSize,
                   (unsigned)writeWaveform.channels,
                   (unsigned)writeWaveform.startLoop,
                   (unsigned)writeWaveform.endLoop);
        sndResource = NULL;
        opErr = XCreateSoundObjectFromData(&sndResource,
                                           &writeWaveform,
                                           C_NONE,
                                           CS_DEFAULT,
                                           NULL,
                                           NULL);
        BAE_STDERR("[RMF Save] Sample[%u] XCreateSoundObjectFromData opErr=%d sndResource=%p\n",
                   (unsigned)index, (int)opErr, (void *)sndResource);
        if (opErr != NO_ERR || !sndResource)
        {
            return BAE_BAD_FILE;
        }
        XSetSoundBaseKey(sndResource, sample->rootKey);
        XSetSoundLoopPoints(sndResource, (int32_t)writeWaveform.startLoop, (int32_t)writeWaveform.endLoop);
        XSetSoundEmbeddedStatus(sndResource, TRUE);
        if (XAddFileResource(fileRef, ID_SND, sndID, pascalName, sndResource, XGetPtrSize(sndResource)) != 0)
        {
            XDisposePtr(sndResource);
            XDisposePtr((XPTR)sampleSndIDs);
            XDisposePtr((XPTR)sampleInstIDs);
            return BAE_FILE_IO_ERROR;
        }
        XDisposePtr(sndResource);
        sampleSndIDs[index] = (XShortResourceID)sndID;
        sampleInstIDs[index] = (sample->instID != 0)
                                ? (XLongResourceID)sample->instID
                                : (XLongResourceID)(256 + (uint32_t)sample->program);
    }

    for (index = 0; index < document->sampleCount; ++index)
    {
        uint32_t prior;
        uint32_t splitCount;
        uint32_t splitIndex;
        uint32_t sampleIndex;
        uint32_t leaderIndex;
        uint32_t leaderFrames;
        XLongResourceID instID;
        BAERmfEditorSample const *leaderSample;
        char pascalName[256];
        BAEResult result;

        instID = sampleInstIDs[index];
        for (prior = 0; prior < index; ++prior)
        {
            if (sampleInstIDs[prior] == instID)
            {
                break;
            }
        }
        if (prior < index)
        {
            continue;
        }

        splitCount = 0;
        leaderIndex = index;
        leaderFrames = 0;
        for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
        {
            if (sampleInstIDs[sampleIndex] == instID)
            {
                BAERmfEditorSample const *candidate;
                uint32_t frames;

                splitCount++;
                candidate = &document->samples[sampleIndex];
                frames = 0;
                if (candidate->waveform)
                {
                    frames = candidate->waveform->waveFrames;
                }
                else
                {
                    frames = candidate->sampleInfo.waveFrames;
                }
                if (frames >= leaderFrames)
                {
                    leaderFrames = frames;
                    leaderIndex = sampleIndex;
                }
            }
        }
        if (splitCount == 0)
        {
            continue;
        }

        leaderSample = &document->samples[leaderIndex];
        result = PV_CreatePascalName(leaderSample->displayName ? leaderSample->displayName : leaderSample->sourcePath,
                                     pascalName);
        if (result != BAE_NO_ERROR)
        {
            XDisposePtr((XPTR)sampleSndIDs);
            XDisposePtr((XPTR)sampleInstIDs);
            return result;
        }

        {
            InstrumentResource instrument;

            if (splitCount > 1)
            {
                enum
                {
                    kInstOffset_sndResourceID = 0,
                    kInstOffset_midiRootKey = 2,
                    kInstOffset_panPlacement = 4,
                    kInstOffset_flags1 = 5,
                    kInstOffset_flags2 = 6,
                    kInstOffset_smodResourceID = 7,
                    kInstOffset_miscParameter1 = 8,
                    kInstOffset_miscParameter2 = 10,
                    kInstOffset_keySplitCount = 12,
                    kInstOffset_keySplitData = 14,
                    kKeySplitFileSize = 8,
                    kInstTailSize = 10
                };
                int32_t instSize;
                XBYTE *instBytes;
                int32_t tailOffset;
                uint32_t *instSampleIndices;
                uint32_t collected;
                uint32_t i;

                instSampleIndices = (uint32_t *)XNewPtr((int32_t)(splitCount * sizeof(uint32_t)));
                if (!instSampleIndices)
                {
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return BAE_MEMORY_ERR;
                }

                collected = 0;
                for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
                {
                    if (sampleInstIDs[sampleIndex] == instID)
                    {
                        instSampleIndices[collected++] = sampleIndex;
                    }
                }

                /* Keep split matching deterministic for engine key-range lookup. */
                for (i = 0; i + 1 < collected; ++i)
                {
                    uint32_t j;
                    for (j = i + 1; j < collected; ++j)
                    {
                        BAERmfEditorSample const *a;
                        BAERmfEditorSample const *b;
                        if (instSampleIndices[i] == instSampleIndices[j])
                        {
                            continue;
                        }
                        a = &document->samples[instSampleIndices[i]];
                        b = &document->samples[instSampleIndices[j]];
                        if (b->lowKey < a->lowKey ||
                            (b->lowKey == a->lowKey && b->highKey < a->highKey))
                        {
                            uint32_t t;
                            t = instSampleIndices[i];
                            instSampleIndices[i] = instSampleIndices[j];
                            instSampleIndices[j] = t;
                        }
                    }
                }

                instSize = (int32_t)(kInstOffset_keySplitData + (int32_t)(collected * kKeySplitFileSize) + kInstTailSize);
                instBytes = (XBYTE *)XNewPtr(instSize);
                if (!instBytes)
                {
                    XDisposePtr((XPTR)instSampleIndices);
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return BAE_MEMORY_ERR;
                }
                XSetMemory(instBytes, instSize, 0);

                XPutShort(instBytes + kInstOffset_sndResourceID, (uint16_t)sampleSndIDs[leaderIndex]);
                XPutShort(instBytes + kInstOffset_midiRootKey, 60);
                instBytes[kInstOffset_panPlacement] = 0;
                instBytes[kInstOffset_flags1] = ZBF_useSampleRate; /* engine must scale pitch for actual sample rate */
                instBytes[kInstOffset_flags2] = 0;
                instBytes[kInstOffset_smodResourceID] = 0;
                XPutShort(instBytes + kInstOffset_miscParameter1, 60);
                XPutShort(instBytes + kInstOffset_miscParameter2, 100);
                XPutShort(instBytes + kInstOffset_keySplitCount, (uint16_t)collected);

                for (i = 0; i < collected; ++i)
                {
                    BAERmfEditorSample const *splitSample;
                    XBYTE *splitPtr;

                    splitSample = &document->samples[instSampleIndices[i]];
                    splitPtr = instBytes + kInstOffset_keySplitData + (i * kKeySplitFileSize);
                    splitPtr[0] = (XBYTE)splitSample->lowKey;
                    splitPtr[1] = (XBYTE)splitSample->highKey;
                    XPutShort(splitPtr + 2, (uint16_t)sampleSndIDs[instSampleIndices[i]]);
                    XPutShort(splitPtr + 4, (uint16_t)splitSample->rootKey);
                    XPutShort(splitPtr + 6, 100);
                }

                tailOffset = (int32_t)(kInstOffset_keySplitData + (int32_t)(collected * kKeySplitFileSize));
                XPutShort(instBytes + tailOffset + 0, 0);      /* tremoloCount */
                XPutShort(instBytes + tailOffset + 2, 0x8000); /* tremoloEnd */
                XPutShort(instBytes + tailOffset + 4, 0);      /* reserved_3 */
                XPutShort(instBytes + tailOffset + 6, 0);      /* descriptorName */
                XPutShort(instBytes + tailOffset + 8, 0);      /* descriptorFlags */

                BAE_STDERR("[RMF Save] INST id=%ld splitCount=%u using split map leaderSample=%u leaderFrames=%u\n",
                           (long)instID,
                           (unsigned)collected,
                           (unsigned)leaderIndex,
                           (unsigned)leaderFrames);
                if (XAddFileResource(fileRef, ID_INST, instID, pascalName, instBytes, instSize) != 0)
                {
                    XDisposePtr((XPTR)instBytes);
                    XDisposePtr((XPTR)instSampleIndices);
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return BAE_FILE_IO_ERROR;
                }

                XDisposePtr((XPTR)instBytes);
                XDisposePtr((XPTR)instSampleIndices);
                continue;
            }

            XSetMemory(&instrument, sizeof(instrument), 0);
            XPutShort(&instrument.sndResourceID, (uint16_t)sampleSndIDs[leaderIndex]);
            XPutShort(&instrument.midiRootKey, 60);
            instrument.flags1 = ZBF_useSampleRate; /* engine must scale pitch for actual sample rate */
            XPutShort(&instrument.miscParameter1, (uint16_t)leaderSample->rootKey);
            XPutShort(&instrument.miscParameter2, 256);
            XPutShort(&instrument.keySplitCount, 0);
            XPutShort(&instrument.tremoloCount, 0);
            XPutShort(&instrument.tremoloEnd, 0x8000);
            BAE_STDERR("[RMF Save] INST id=%ld fallback midiRootKey=%d sampleRootKey=%u\n",
                       (long)instID,
                       60,
                       (unsigned)leaderSample->rootKey);
            if (XAddFileResource(fileRef, ID_INST, instID, pascalName, &instrument, (int32_t)sizeof(instrument)) != 0)
            {
                XDisposePtr((XPTR)sampleSndIDs);
                XDisposePtr((XPTR)sampleInstIDs);
                return BAE_FILE_IO_ERROR;
            }
        }
    }

    XDisposePtr((XPTR)sampleSndIDs);
    XDisposePtr((XPTR)sampleInstIDs);
    return BAE_NO_ERROR;
}

static BAEResult PV_AddSongResource(BAERmfEditorDocument *document, XFILE fileRef, XLongResourceID midiResourceID)
{
    SongResource_Info *songInfo;
    SongResource *songResource;
    XLongResourceID songID;
    char pascalName[256];
    uint32_t infoIndex;
    BAEResult result;

    songInfo = XNewSongResourceInfo();
    if (!songInfo)
    {
        return BAE_MEMORY_ERR;
    }
    XClearSongResourceInfo(songInfo);
    songInfo->songType = SONG_TYPE_RMF;
    songInfo->objectResourceID = (XShortResourceID)midiResourceID;
    songInfo->maxMidiNotes = document->maxMidiNotes;
    songInfo->maxEffects = document->maxEffects;
    songInfo->mixLevel = document->mixLevel;
    songInfo->reverbType = (int16_t)document->reverbType;
    songInfo->songVolume = document->songVolume;
    songInfo->songTempo = (int32_t)document->tempoBPM;
    songInfo->songPitchShift = 0;
    songInfo->songLocked = FALSE;
    songInfo->songEmbedded = FALSE;
    for (infoIndex = 0; infoIndex < INFO_TYPE_COUNT; ++infoIndex)
    {
        if (document->info[infoIndex])
        {
            result = PV_AssignSongInfoString(songInfo, (BAEInfoType)infoIndex, document->info[infoIndex]);
            if (result != BAE_NO_ERROR)
            {
                XDisposeSongResourceInfo(songInfo);
                return result;
            }
        }
    }
    songResource = XNewSongFromSongResourceInfo(songInfo);
    XDisposeSongResourceInfo(songInfo);
    if (!songResource)
    {
        return BAE_MEMORY_ERR;
    }
    if (PV_GetAvailableResourceID(fileRef, ID_SONG, 1, &songID) != BAE_NO_ERROR)
    {
        XDisposeSongPtr(songResource);
        return BAE_FILE_IO_ERROR;
    }
    PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Untitled RMF", pascalName);
    if (XAddFileResource(fileRef, ID_SONG, songID, pascalName, songResource, XGetPtrSize(songResource)) != 0)
    {
        XDisposeSongPtr(songResource);
        return BAE_FILE_IO_ERROR;
    }
    XDisposeSongPtr(songResource);
    return BAE_NO_ERROR;
}

static BAEResult PV_AddSongResourceWithID(BAERmfEditorDocument *document,
                                          XFILE fileRef,
                                          XLongResourceID midiResourceID,
                                          XLongResourceID songID,
                                          unsigned char const *pascalName)
{
    SongResource_Info *songInfo;
    SongResource *songResource;
    uint32_t infoIndex;
    BAEResult result;
    char fallbackPascalName[256];
    void const *songName;

    songInfo = XNewSongResourceInfo();
    if (!songInfo)
    {
        return BAE_MEMORY_ERR;
    }
    XClearSongResourceInfo(songInfo);
    songInfo->songType = SONG_TYPE_RMF;
    songInfo->objectResourceID = (XShortResourceID)midiResourceID;
    songInfo->maxMidiNotes = document->maxMidiNotes;
    songInfo->maxEffects = document->maxEffects;
    songInfo->mixLevel = document->mixLevel;
    songInfo->reverbType = (int16_t)document->reverbType;
    songInfo->songVolume = document->songVolume;
    songInfo->songTempo = (int32_t)document->tempoBPM;
    songInfo->songPitchShift = 0;
    songInfo->songLocked = FALSE;
    songInfo->songEmbedded = FALSE;
    for (infoIndex = 0; infoIndex < INFO_TYPE_COUNT; ++infoIndex)
    {
        if (document->info[infoIndex])
        {
            result = PV_AssignSongInfoString(songInfo, (BAEInfoType)infoIndex, document->info[infoIndex]);
            if (result != BAE_NO_ERROR)
            {
                XDisposeSongResourceInfo(songInfo);
                return result;
            }
        }
    }
    songResource = XNewSongFromSongResourceInfo(songInfo);
    XDisposeSongResourceInfo(songInfo);
    if (!songResource)
    {
        return BAE_MEMORY_ERR;
    }

    if (pascalName && pascalName[0])
    {
        songName = pascalName;
    }
    else
    {
        PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Untitled RMF", fallbackPascalName);
        songName = fallbackPascalName;
    }

    if (XAddFileResource(fileRef, ID_SONG, songID, songName, songResource, XGetPtrSize(songResource)) != 0)
    {
        XDisposeSongPtr(songResource);
        return BAE_FILE_IO_ERROR;
    }
    XDisposeSongPtr(songResource);
    return BAE_NO_ERROR;
}

BAERmfEditorDocument *BAERmfEditorDocument_New(void)
{
    BAERmfEditorDocument *document;

    document = (BAERmfEditorDocument *)XNewPtr(sizeof(BAERmfEditorDocument));
    if (document)
    {
        XSetMemory(document, sizeof(BAERmfEditorDocument), 0);
        document->tempoBPM = 120;
        document->ticksPerQuarter = 480;
        document->maxMidiNotes = 24;
        document->maxEffects = 4;
        document->mixLevel = 8;
        document->songVolume = 127;
        document->reverbType = BAE_REVERB_TYPE_1;
        document->originalSongID = 0;
        document->originalObjectResourceID = 0;
        document->originalMidiType = 0;
        document->loadedFromRmf = FALSE;
        document->isPristine = FALSE;
    }
    return document;
}

BAERmfEditorDocument *BAERmfEditorDocument_LoadFromFile(BAEPathName filePath)
{
    BAERmfEditorDocument *document;
    BAEFileType fileType;
    BAEResult result;

    if (!filePath)
    {
        return NULL;
    }
    fileType = X_DetermineFileTypeByPath(filePath);
    document = BAERmfEditorDocument_New();
    if (!document)
    {
        return NULL;
    }
    if (fileType == BAE_MIDI_TYPE)
    {
        unsigned char *data;
        uint32_t dataSize;

        result = PV_ReadWholeFile(filePath, &data, &dataSize);
        if (result == BAE_NO_ERROR)
        {
            result = PV_LoadMidiBytesIntoDocument(document, data, dataSize);
            XDisposePtr(data);
        }
    }
    else if (fileType == BAE_RMF)
    {
        result = PV_LoadRmfFileIntoDocument(document, filePath);
    }
    else
    {
        result = BAE_BAD_FILE_TYPE;
    }
    if (result != BAE_NO_ERROR)
    {
        BAERmfEditorDocument_Delete(document);
        return NULL;
    }
    return document;
}

BAEResult BAERmfEditorDocument_Delete(BAERmfEditorDocument *document)
{
    uint32_t index;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    for (index = 0; index < INFO_TYPE_COUNT; ++index)
    {
        PV_FreeString(&document->info[index]);
    }
    for (index = 0; index < document->trackCount; ++index)
    {
        PV_FreeString(&document->tracks[index].name);
        if (document->tracks[index].notes)
        {
            XDisposePtr(document->tracks[index].notes);
        }
        if (document->tracks[index].ccEvents)
        {
            XDisposePtr(document->tracks[index].ccEvents);
        }
    }
    if (document->tracks)
    {
        XDisposePtr(document->tracks);
    }
    for (index = 0; index < document->sampleCount; ++index)
    {
        PV_FreeString(&document->samples[index].displayName);
        PV_FreeString(&document->samples[index].sourcePath);
        if (document->samples[index].waveform)
        {
            GM_FreeWaveform(document->samples[index].waveform);
        }
    }
    if (document->samples)
    {
        XDisposePtr(document->samples);
    }
    PV_ClearTempoEvents(document);
    PV_FreeOriginalResources(document);
    XDisposePtr(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SetTempoBPM(BAERmfEditorDocument *document, uint32_t bpm)
{
    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (bpm == 0 || bpm > 960)
    {
        return BAE_PARAM_ERR;
    }
    /* Explicit tempo edit means use a fixed tempo unless a new MIDI map is loaded/copied. */
    PV_ClearTempoEvents(document);
    document->tempoBPM = bpm;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_CopyTempoMapFrom(BAERmfEditorDocument *dest,
                                                BAERmfEditorDocument const *src)
{
    BAEResult result;
    uint32_t eventIndex;

    if (!dest || !src)
    {
        return BAE_PARAM_ERR;
    }

    PV_ClearTempoEvents(dest);
    dest->tempoBPM = src->tempoBPM;

    if (src->tempoEventCount == 0)
    {
        return BAE_NO_ERROR;
    }

    result = PV_GrowBuffer((void **)&dest->tempoEvents,
                           &dest->tempoEventCapacity,
                           sizeof(BAERmfEditorTempoEvent),
                           src->tempoEventCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    for (eventIndex = 0; eventIndex < src->tempoEventCount; ++eventIndex)
    {
        dest->tempoEvents[eventIndex] = src->tempoEvents[eventIndex];
    }
    dest->tempoEventCount = src->tempoEventCount;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTempoEventCount(BAERmfEditorDocument const *document,
                                                  uint32_t *outCount)
{
    if (!document || !outCount)
    {
        return BAE_PARAM_ERR;
    }
    *outCount = document->tempoEventCount;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTempoEvent(BAERmfEditorDocument const *document,
                                             uint32_t eventIndex,
                                             uint32_t *outTick,
                                             uint32_t *outMicrosecondsPerQuarter)
{
    if (!document || !outTick || !outMicrosecondsPerQuarter)
    {
        return BAE_PARAM_ERR;
    }
    if (eventIndex >= document->tempoEventCount)
    {
        return BAE_PARAM_ERR;
    }
    *outTick = document->tempoEvents[eventIndex].tick;
    *outMicrosecondsPerQuarter = document->tempoEvents[eventIndex].microsecondsPerQuarter;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_AddTempoEvent(BAERmfEditorDocument *document,
                                             uint32_t tick,
                                             uint32_t microsecondsPerQuarter)
{
    BAEResult result;

    if (!document || microsecondsPerQuarter == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (document->tempoEventCount == 0 && tick > 0)
    {
        uint32_t baseBpm;

        baseBpm = document->tempoBPM ? document->tempoBPM : 120;
        result = PV_AddTempoEvent(document, 0, 60000000UL / baseBpm);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    result = PV_AddTempoEvent(document, tick, microsecondsPerQuarter);
    if (result == BAE_NO_ERROR && tick == 0)
    {
        document->tempoBPM = 60000000UL / microsecondsPerQuarter;
    }
    if (result == BAE_NO_ERROR)
    {
        PV_MarkDocumentDirty(document);
    }
    return result;
}

BAEResult BAERmfEditorDocument_SetTempoEvent(BAERmfEditorDocument *document,
                                             uint32_t eventIndex,
                                             uint32_t tick,
                                             uint32_t microsecondsPerQuarter)
{
    BAEResult result;

    if (!document || microsecondsPerQuarter == 0 || eventIndex >= document->tempoEventCount)
    {
        return BAE_PARAM_ERR;
    }
    document->tempoEvents[eventIndex].tick = tick;
    document->tempoEvents[eventIndex].microsecondsPerQuarter = microsecondsPerQuarter;
    while (eventIndex > 0 && document->tempoEvents[eventIndex - 1].tick > document->tempoEvents[eventIndex].tick)
    {
        BAERmfEditorTempoEvent tempEvent;

        tempEvent = document->tempoEvents[eventIndex - 1];
        document->tempoEvents[eventIndex - 1] = document->tempoEvents[eventIndex];
        document->tempoEvents[eventIndex] = tempEvent;
        eventIndex--;
    }
    while ((eventIndex + 1) < document->tempoEventCount &&
           document->tempoEvents[eventIndex + 1].tick < document->tempoEvents[eventIndex].tick)
    {
        BAERmfEditorTempoEvent tempEvent;

        tempEvent = document->tempoEvents[eventIndex + 1];
        document->tempoEvents[eventIndex + 1] = document->tempoEvents[eventIndex];
        document->tempoEvents[eventIndex] = tempEvent;
        eventIndex++;
    }
    if (document->tempoEventCount > 0 && document->tempoEvents[0].tick == 0 && document->tempoEvents[0].microsecondsPerQuarter > 0)
    {
        document->tempoBPM = 60000000UL / document->tempoEvents[0].microsecondsPerQuarter;
    }
    result = BAE_NO_ERROR;
    PV_MarkDocumentDirty(document);
    return result;
}

BAEResult BAERmfEditorDocument_DeleteTempoEvent(BAERmfEditorDocument *document,
                                                uint32_t eventIndex)
{
    if (!document || eventIndex >= document->tempoEventCount)
    {
        return BAE_PARAM_ERR;
    }
    if (eventIndex + 1 < document->tempoEventCount)
    {
        XBlockMove(&document->tempoEvents[eventIndex + 1],
                   &document->tempoEvents[eventIndex],
                   (int32_t)((document->tempoEventCount - (eventIndex + 1)) * sizeof(BAERmfEditorTempoEvent)));
    }
    document->tempoEventCount--;
    if (document->tempoEventCount > 0 && document->tempoEvents[0].tick == 0 && document->tempoEvents[0].microsecondsPerQuarter > 0)
    {
        document->tempoBPM = 60000000UL / document->tempoEvents[0].microsecondsPerQuarter;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTrackCCEventCount(BAERmfEditorDocument const *document,
                                                    uint16_t trackIndex,
                                                    unsigned char cc,
                                                    uint32_t *outCount)
{
    BAERmfEditorTrack const *track;
    uint32_t index;
    uint32_t count;

    if (!document || !outCount)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    count = 0;
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc)
        {
            count++;
        }
    }
    *outCount = count;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTrackCCEvent(BAERmfEditorDocument const *document,
                                               uint16_t trackIndex,
                                               unsigned char cc,
                                               uint32_t eventIndex,
                                               uint32_t *outTick,
                                               unsigned char *outValue)
{
    BAERmfEditorTrack const *track;
    BAERmfEditorCCEvent const *event;

    if (!document || !outTick || !outValue)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    event = PV_FindTrackCCEventConst(track, cc, eventIndex, NULL);
    if (!event)
    {
        return BAE_PARAM_ERR;
    }
    *outTick = event->tick;
    *outValue = event->value;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_AddTrackCCEvent(BAERmfEditorDocument *document,
                                               uint16_t trackIndex,
                                               unsigned char cc,
                                               uint32_t tick,
                                               unsigned char value)
{
    BAERmfEditorTrack *track;
    BAEResult result;

    if (!document || value > 127 || (cc != 7 && cc != 10 && cc != 11))
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_AddCCEventToTrack(track, tick, cc, value, 0);
    if (result == BAE_NO_ERROR)
    {
        if (cc == 7 && tick == 0)
        {
            track->volume = value;
        }
        if (cc == 10 && tick == 0)
        {
            track->pan = value;
        }
        PV_MarkDocumentDirty(document);
    }
    return result;
}

BAEResult BAERmfEditorDocument_SetTrackCCEvent(BAERmfEditorDocument *document,
                                               uint16_t trackIndex,
                                               unsigned char cc,
                                               uint32_t eventIndex,
                                               uint32_t tick,
                                               unsigned char value)
{
    BAERmfEditorTrack *track;
    BAERmfEditorCCEvent *event;

    if (!document || value > 127 || (cc != 7 && cc != 10 && cc != 11))
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    event = PV_FindTrackCCEvent(track, cc, eventIndex, NULL);
    if (!event)
    {
        return BAE_PARAM_ERR;
    }
    event->tick = tick;
    event->value = value;
    qsort(track->ccEvents, track->ccEventCount, sizeof(BAERmfEditorCCEvent), PV_CompareCCEvents);
    if (cc == 7 && tick == 0)
    {
        track->volume = value;
    }
    if (cc == 10 && tick == 0)
    {
        track->pan = value;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_DeleteTrackCCEvent(BAERmfEditorDocument *document,
                                                  uint16_t trackIndex,
                                                  unsigned char cc,
                                                  uint32_t eventIndex)
{
    BAERmfEditorTrack *track;
    uint32_t actualIndex;

    if (!document || (cc != 7 && cc != 10 && cc != 11))
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    if (!PV_FindTrackCCEvent(track, cc, eventIndex, &actualIndex))
    {
        return BAE_PARAM_ERR;
    }
    if (actualIndex + 1 < track->ccEventCount)
    {
        XBlockMove(&track->ccEvents[actualIndex + 1],
                   &track->ccEvents[actualIndex],
                   (int32_t)((track->ccEventCount - (actualIndex + 1)) * sizeof(BAERmfEditorCCEvent)));
    }
    track->ccEventCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTempoBPM(BAERmfEditorDocument const *document, uint32_t *outBpm)
{
    if (!document || !outBpm)
    {
        return BAE_PARAM_ERR;
    }
    *outBpm = document->tempoBPM;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SetTicksPerQuarter(BAERmfEditorDocument *document, uint16_t ticksPerQuarter)
{
    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (ticksPerQuarter == 0)
    {
        return BAE_PARAM_ERR;
    }
    document->ticksPerQuarter = ticksPerQuarter;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTicksPerQuarter(BAERmfEditorDocument const *document, uint16_t *outTicksPerQuarter)
{
    if (!document || !outTicksPerQuarter)
    {
        return BAE_PARAM_ERR;
    }
    *outTicksPerQuarter = document->ticksPerQuarter;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SetInfo(BAERmfEditorDocument *document, BAEInfoType infoType, char const *value)
{
    BAEResult result;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (infoType < 0 || infoType >= INFO_TYPE_COUNT)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_SetDocumentString(&document->info[infoType], value);
    if (result == BAE_NO_ERROR)
    {
        PV_MarkDocumentDirty(document);
    }
    return result;
}

char const *BAERmfEditorDocument_GetInfo(BAERmfEditorDocument const *document, BAEInfoType infoType)
{
    if (!document || infoType < 0 || infoType >= INFO_TYPE_COUNT)
    {
        return NULL;
    }
    return document->info[infoType];
}

BAEResult BAERmfEditorDocument_AddTrack(BAERmfEditorDocument *document,
                                        BAERmfEditorTrackSetup const *setup,
                                        uint16_t *outTrackIndex)
{
    BAEResult result;
    BAERmfEditorTrack *track;

    if (!document || !setup)
    {
        return BAE_PARAM_ERR;
    }
    if (setup->channel >= BAE_MAX_MIDI_CHANNELS || setup->bank > 16383 || setup->program >= 128)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_GrowBuffer((void **)&document->tracks,
                           &document->trackCapacity,
                           sizeof(BAERmfEditorTrack),
                           document->trackCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track = &document->tracks[document->trackCount];
    XSetMemory(track, sizeof(*track), 0);
    track->channel = setup->channel;
    track->bank = setup->bank;
    track->program = setup->program;
    track->pan = 64;
    track->volume = 100;
    track->transpose = 0;
    if (setup->name)
    {
        track->name = PV_DuplicateString(setup->name);
        if (!track->name)
        {
            return BAE_MEMORY_ERR;
        }
    }
    if (outTrackIndex)
    {
        *outTrackIndex = (uint16_t)document->trackCount;
    }
    document->trackCount++;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTrackCount(BAERmfEditorDocument const *document,
                                             uint16_t *outTrackCount)
{
    if (!document || !outTrackCount)
    {
        return BAE_PARAM_ERR;
    }
    *outTrackCount = (uint16_t)document->trackCount;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetTrackInfo(BAERmfEditorDocument const *document,
                                            uint16_t trackIndex,
                                            BAERmfEditorTrackInfo *outTrackInfo)
{
    BAERmfEditorTrack const *track;

    if (!outTrackInfo)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    outTrackInfo->name = track->name;
    outTrackInfo->channel = track->channel;
    outTrackInfo->bank = track->bank;
    outTrackInfo->program = track->program;
    outTrackInfo->pan = track->pan;
    outTrackInfo->volume = track->volume;
    outTrackInfo->transpose = track->transpose;
    outTrackInfo->noteCount = track->noteCount;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SetTrackInfo(BAERmfEditorDocument *document,
                                            uint16_t trackIndex,
                                            BAERmfEditorTrackInfo const *trackInfo)
{
    BAERmfEditorTrack *track;
    BAEResult result;

    if (!trackInfo)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    if (trackInfo->channel >= BAE_MAX_MIDI_CHANNELS ||
        trackInfo->bank > 16383 ||
        trackInfo->program >= 128 ||
        trackInfo->pan > 127 ||
        trackInfo->volume > 127 ||
        trackInfo->transpose < -127 ||
        trackInfo->transpose > 127)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_SetTrackName(track, trackInfo->name);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track->channel = trackInfo->channel;
    track->bank = trackInfo->bank;
    track->program = trackInfo->program;
    track->pan = trackInfo->pan;
    track->volume = trackInfo->volume;
    track->transpose = trackInfo->transpose;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_DeleteTrack(BAERmfEditorDocument *document,
                                           uint16_t trackIndex)
{
    BAERmfEditorTrack *track;

    if (!document || trackIndex >= document->trackCount)
    {
        return BAE_PARAM_ERR;
    }
    track = &document->tracks[trackIndex];
    PV_FreeString(&track->name);
    if (track->notes)
    {
        XDisposePtr(track->notes);
        track->notes = NULL;
    }
    if (track->ccEvents)
    {
        XDisposePtr(track->ccEvents);
        track->ccEvents = NULL;
    }
    if (trackIndex + 1 < document->trackCount)
    {
        XBlockMove(&document->tracks[trackIndex + 1],
                   &document->tracks[trackIndex],
                   (int32_t)((document->trackCount - (trackIndex + 1)) * sizeof(BAERmfEditorTrack)));
    }
    document->trackCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_AddNote(BAERmfEditorDocument *document,
                                       uint16_t trackIndex,
                                       uint32_t startTick,
                                       uint32_t durationTicks,
                                       unsigned char note,
                                       unsigned char velocity)
{
    BAERmfEditorTrack *track;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (note > 127 || velocity > 127)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    {
        BAEResult result;

        result = PV_AddNoteToTrack(track, startTick, durationTicks, note, velocity, track->bank, track->program);
        if (result == BAE_NO_ERROR)
        {
            PV_MarkDocumentDirty(document);
        }
        return result;
    }
}

BAEResult BAERmfEditorDocument_GetNoteCount(BAERmfEditorDocument const *document,
                                            uint16_t trackIndex,
                                            uint32_t *outNoteCount)
{
    BAERmfEditorTrack const *track;

    if (!outNoteCount)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    *outNoteCount = track->noteCount;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetNoteInfo(BAERmfEditorDocument const *document,
                                           uint16_t trackIndex,
                                           uint32_t noteIndex,
                                           BAERmfEditorNoteInfo *outNoteInfo)
{
    BAERmfEditorTrack const *track;
    BAERmfEditorNote const *note;

    if (!outNoteInfo)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track || noteIndex >= track->noteCount)
    {
        return BAE_PARAM_ERR;
    }
    note = &track->notes[noteIndex];
    outNoteInfo->startTick = note->startTick;
    outNoteInfo->durationTicks = note->durationTicks;
    outNoteInfo->note = note->note;
    outNoteInfo->velocity = note->velocity;
    outNoteInfo->bank = note->bank;
    outNoteInfo->program = note->program;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SetNoteInfo(BAERmfEditorDocument *document,
                                           uint16_t trackIndex,
                                           uint32_t noteIndex,
                                           BAERmfEditorNoteInfo const *noteInfo)
{
    BAERmfEditorTrack *track;

    if (!noteInfo || noteInfo->durationTicks == 0 || noteInfo->note > 127 || noteInfo->velocity > 127 || noteInfo->program > 127 || noteInfo->bank > 16383)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track || noteIndex >= track->noteCount)
    {
        return BAE_PARAM_ERR;
    }
    track->notes[noteIndex].startTick = noteInfo->startTick;
    track->notes[noteIndex].durationTicks = noteInfo->durationTicks;
    track->notes[noteIndex].note = noteInfo->note;
    track->notes[noteIndex].velocity = noteInfo->velocity;
    track->notes[noteIndex].bank = noteInfo->bank;
    track->notes[noteIndex].program = noteInfo->program;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_DeleteNote(BAERmfEditorDocument *document,
                                          uint16_t trackIndex,
                                          uint32_t noteIndex)
{
    BAERmfEditorTrack *track;

    track = PV_GetTrack(document, trackIndex);
    if (!track || noteIndex >= track->noteCount)
    {
        return BAE_PARAM_ERR;
    }
    if (noteIndex + 1 < track->noteCount)
    {
        XBlockMove(&track->notes[noteIndex + 1],
                   &track->notes[noteIndex],
                   (int32_t)((track->noteCount - (noteIndex + 1)) * sizeof(BAERmfEditorNote)));
    }
    track->noteCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_AddSampleFromFile(BAERmfEditorDocument *document,
                                                 BAEPathName filePath,
                                                 BAERmfEditorSampleSetup const *setup,
                                                 BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    BAEFileType fileType;
    AudioFileType audioFileType;
    XFILENAME fileName;
    GM_Waveform *waveform;
    OPErr opErr;
    BAEResult result;
    uint32_t index;

    if (!document || !filePath || !setup)
    {
        return BAE_PARAM_ERR;
    }
    if (setup->program >= 128)
    {
        return BAE_PARAM_ERR;
    }
    for (index = 0; index < document->sampleCount; ++index)
    {
        if (document->samples[index].program == setup->program)
        {
            return BAE_ALREADY_EXISTS;
        }
    }
    fileType = X_DetermineFileTypeByPath(filePath);
    if (fileType != BAE_WAVE_TYPE && fileType != BAE_AIFF_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    audioFileType = PV_TranslateEditorFileType(fileType);
    if (audioFileType == FILE_INVALID_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    XConvertPathToXFILENAME(filePath, &fileName);
    waveform = GM_ReadFileIntoMemory(&fileName, audioFileType, TRUE, &opErr);
    if (!waveform || opErr != NO_ERR)
    {
        if (waveform)
        {
            GM_FreeWaveform(waveform);
        }
        return BAE_BAD_FILE;
    }
    result = PV_GrowBuffer((void **)&document->samples,
                           &document->sampleCapacity,
                           sizeof(BAERmfEditorSample),
                           document->sampleCount + 1);
    if (result != BAE_NO_ERROR)
    {
        GM_FreeWaveform(waveform);
        return result;
    }
    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = waveform;
    sample->program = setup->program;
    sample->rootKey = setup->rootKey;
    sample->lowKey = setup->lowKey;
    sample->highKey = setup->highKey;
    sample->sourceCompressionType = waveform->compressionType;
    sample->displayName = PV_DuplicateString(setup->displayName ? setup->displayName : filePath);
    sample->sourcePath = PV_DuplicateString(filePath);
    if (!sample->displayName || !sample->sourcePath)
    {
        PV_FreeString(&sample->displayName);
        PV_FreeString(&sample->sourcePath);
        GM_FreeWaveform(waveform);
        XSetMemory(sample, sizeof(*sample), 0);
        return BAE_MEMORY_ERR;
    }
    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }
    document->sampleCount++;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_AddEmptySample(BAERmfEditorDocument *document,
                                              BAERmfEditorSampleSetup const *setup,
                                              uint32_t *outSampleIndex,
                                              BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    BAEResult result;
    GM_Waveform *waveform;
    int16_t *pcm;

    if (!document || !setup)
    {
        return BAE_PARAM_ERR;
    }
    if (setup->program >= 128 || setup->rootKey > 127 || setup->lowKey > 127 || setup->highKey > 127 || setup->lowKey > setup->highKey)
    {
        return BAE_PARAM_ERR;
    }

    waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
    if (!waveform)
    {
        return BAE_MEMORY_ERR;
    }
    XSetMemory(waveform, sizeof(*waveform), 0);
    pcm = (int16_t *)XNewPtr((int32_t)sizeof(int16_t));
    if (!pcm)
    {
        XDisposePtr((XPTR)waveform);
        return BAE_MEMORY_ERR;
    }
    pcm[0] = 0;
    waveform->theWaveform = (SBYTE *)pcm;
    waveform->waveFrames = 1;
    waveform->waveSize = sizeof(int16_t);
    waveform->bitSize = 16;
    waveform->channels = 1;
    waveform->sampledRate = 22050L << 16;
    waveform->baseMidiPitch = setup->rootKey;
    waveform->startLoop = 0;
    waveform->endLoop = 0;
    waveform->compressionType = C_NONE;

    result = PV_GrowBuffer((void **)&document->samples,
                           &document->sampleCapacity,
                           sizeof(BAERmfEditorSample),
                           document->sampleCount + 1);
    if (result != BAE_NO_ERROR)
    {
        GM_FreeWaveform(waveform);
        return result;
    }

    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = waveform;
    sample->program = setup->program;
    sample->rootKey = setup->rootKey;
    sample->lowKey = setup->lowKey;
    sample->highKey = setup->highKey;
    sample->sourceCompressionType = C_NONE;
    sample->displayName = PV_DuplicateString(setup->displayName ? setup->displayName : "New Instrument");
    sample->sourcePath = NULL;
    if (!sample->displayName)
    {
        GM_FreeWaveform(waveform);
        XSetMemory(sample, sizeof(*sample), 0);
        return BAE_MEMORY_ERR;
    }

    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;

    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }
    if (outSampleIndex)
    {
        *outSampleIndex = document->sampleCount;
    }
    document->sampleCount++;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetSampleCount(BAERmfEditorDocument const *document,
                                              uint32_t *outSampleCount)
{
    if (!document || !outSampleCount)
    {
        return BAE_PARAM_ERR;
    }
    *outSampleCount = document->sampleCount;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetSampleInfo(BAERmfEditorDocument const *document,
                                             uint32_t sampleIndex,
                                             BAERmfEditorSampleInfo *outSampleInfo)
{
    BAERmfEditorSample const *sample;

    if (!document || !outSampleInfo || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    outSampleInfo->displayName = sample->displayName;
    outSampleInfo->sourcePath = sample->sourcePath;
    outSampleInfo->program = sample->program;
    outSampleInfo->rootKey = sample->rootKey;
    outSampleInfo->lowKey = sample->lowKey;
    outSampleInfo->highKey = sample->highKey;
    outSampleInfo->sampleInfo = sample->sampleInfo;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SetSampleInfo(BAERmfEditorDocument *document,
                                             uint32_t sampleIndex,
                                             BAERmfEditorSampleInfo const *sampleInfo)
{
    BAERmfEditorSample *sample;
    BAEResult result;

    if (!document || !sampleInfo || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    if (sampleInfo->program >= 128 ||
        sampleInfo->rootKey > 127 ||
        sampleInfo->lowKey > 127 ||
        sampleInfo->highKey > 127 ||
        sampleInfo->lowKey > sampleInfo->highKey)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    result = PV_SetDocumentString(&sample->displayName, sampleInfo->displayName);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    sample->program = sampleInfo->program;
    sample->rootKey = sampleInfo->rootKey;
    sample->lowKey = sampleInfo->lowKey;
    sample->highKey = sampleInfo->highKey;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_DeleteSample(BAERmfEditorDocument *document,
                                            uint32_t sampleIndex)
{
    BAERmfEditorSample *sample;

    if (!document || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    PV_FreeString(&sample->displayName);
    PV_FreeString(&sample->sourcePath);
    if (sample->waveform)
    {
        GM_FreeWaveform(sample->waveform);
        sample->waveform = NULL;
    }
    if (sampleIndex + 1 < document->sampleCount)
    {
        XBlockMove(&document->samples[sampleIndex + 1],
                   &document->samples[sampleIndex],
                   (int32_t)((document->sampleCount - (sampleIndex + 1)) * sizeof(BAERmfEditorSample)));
    }
    document->sampleCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_ReplaceSampleFromFile(BAERmfEditorDocument *document,
                                                     uint32_t sampleIndex,
                                                     BAEPathName filePath,
                                                     BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    BAEFileType fileType;
    AudioFileType audioFileType;
    XFILENAME fileName;
    GM_Waveform *waveform;
    OPErr opErr;
    char *pathCopy;

    if (!document || !filePath || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    fileType = X_DetermineFileTypeByPath(filePath);
    if (fileType != BAE_WAVE_TYPE && fileType != BAE_AIFF_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    audioFileType = PV_TranslateEditorFileType(fileType);
    if (audioFileType == FILE_INVALID_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    XConvertPathToXFILENAME(filePath, &fileName);
    waveform = GM_ReadFileIntoMemory(&fileName, audioFileType, TRUE, &opErr);
    if (!waveform || opErr != NO_ERR)
    {
        if (waveform)
        {
            GM_FreeWaveform(waveform);
        }
        return BAE_BAD_FILE;
    }

    pathCopy = PV_DuplicateString(filePath);
    if (!pathCopy)
    {
        GM_FreeWaveform(waveform);
        return BAE_MEMORY_ERR;
    }

    sample = &document->samples[sampleIndex];
    if (sample->waveform)
    {
        GM_FreeWaveform(sample->waveform);
    }
    sample->waveform = waveform;
    PV_FreeString(&sample->sourcePath);
    sample->sourcePath = pathCopy;

    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    sample->sourceCompressionType = waveform->compressionType;
    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetSampleWaveformData(BAERmfEditorDocument const *document,
                                                     uint32_t sampleIndex,
                                                     void const **outWaveData,
                                                     uint32_t *outFrameCount,
                                                     uint16_t *outBitSize,
                                                     uint16_t *outChannels,
                                                     BAE_UNSIGNED_FIXED *outSampleRate)
{
    BAERmfEditorSample const *sample;
    GM_Waveform const *waveform;
    uint32_t frameCount;
    uint32_t bytesPerFrame;

    if (!document || !outWaveData || !outFrameCount || !outBitSize || !outChannels || !outSampleRate)
    {
        return BAE_PARAM_ERR;
    }
    if (sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }

    sample = &document->samples[sampleIndex];
    waveform = sample->waveform;
    if (!waveform || !waveform->theWaveform || waveform->bitSize == 0 || waveform->channels == 0)
    {
        return BAE_BAD_FILE;
    }

    bytesPerFrame = (uint32_t)(waveform->channels * (waveform->bitSize / 8));
    frameCount = waveform->waveFrames;
    if (frameCount == 0 && bytesPerFrame > 0)
    {
        frameCount = (uint32_t)(waveform->waveSize / bytesPerFrame);
    }

    *outWaveData = (void const *)waveform->theWaveform;
    *outFrameCount = frameCount;
    *outBitSize = waveform->bitSize;
    *outChannels = waveform->channels;
    *outSampleRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_GetSampleCodecDescription(BAERmfEditorDocument const *document,
                                                         uint32_t sampleIndex,
                                                         char *outCodec,
                                                         uint32_t outCodecSize)
{
    BAERmfEditorSample const *sample;

    if (!document || !outCodec || outCodecSize == 0 || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    outCodec[0] = 0;
    XGetCompressionName((int32_t)sample->sourceCompressionType, outCodec);
    if (outCodec[0] == 0)
    {
        XStrCpy(outCodec, "Unknown");
    }
    if (XStrLen(outCodec) >= (int32_t)outCodecSize)
    {
        outCodec[outCodecSize - 1] = 0;
    }
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_ExportSampleToFile(BAERmfEditorDocument const *document,
                                                  uint32_t sampleIndex,
                                                  BAEPathName filePath)
{
    BAERmfEditorSample const *sample;
    XFILENAME fileName;
    AudioFileType outType;
    char const *ext;
    GM_Waveform waveCopy;
    OPErr opErr;

    if (!document || !filePath || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    if (!sample->waveform || !sample->waveform->theWaveform)
    {
        return BAE_BAD_FILE;
    }

    ext = strrchr(filePath, '.');
    if (ext && (!XStrCmp(ext, ".aif") || !XStrCmp(ext, ".aiff") ||
               !XStrCmp(ext, ".AIF") || !XStrCmp(ext, ".AIFF")))
    {
        outType = FILE_AIFF_TYPE;
    }
    else
    {
        outType = FILE_WAVE_TYPE;
    }

    waveCopy = *sample->waveform;
    XConvertPathToXFILENAME(filePath, &fileName);
    opErr = GM_WriteFileFromMemory(&fileName, &waveCopy, outType);
    return (opErr == NO_ERR) ? BAE_NO_ERROR : BAE_FILE_IO_ERROR;
}

BAEResult BAERmfEditorDocument_CopySamplesFrom(BAERmfEditorDocument *dest,
                                               BAERmfEditorDocument const *src)
{
    return BAERmfEditorDocument_CopySamplesForPrograms(dest, src, NULL, NULL);
}

static BAEResult PV_CopySampleEntry(BAERmfEditorDocument *dest,
                                    BAERmfEditorSample const *srcSample)
{
    BAEResult result;
    BAERmfEditorSample *dstSample;
    GM_Waveform *waveform;

    result = PV_GrowBuffer((void **)&dest->samples,
                           &dest->sampleCapacity,
                           sizeof(BAERmfEditorSample),
                           dest->sampleCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    dstSample = &dest->samples[dest->sampleCount];
    XSetMemory(dstSample, sizeof(*dstSample), 0);
    dstSample->displayName = PV_DuplicateString(srcSample->displayName);
    dstSample->sourcePath = PV_DuplicateString(srcSample->sourcePath);
    dstSample->program = srcSample->program;
    dstSample->instID = srcSample->instID;
    dstSample->rootKey = srcSample->rootKey;
    dstSample->lowKey = srcSample->lowKey;
    dstSample->highKey = srcSample->highKey;
    dstSample->sourceCompressionType = srcSample->sourceCompressionType;
    dstSample->sampleInfo = srcSample->sampleInfo;
    waveform = NULL;
    if (srcSample->waveform)
    {
        waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
        if (!waveform)
        {
            return BAE_MEMORY_ERR;
        }
        *waveform = *srcSample->waveform;
        waveform->theWaveform = NULL;
        if (srcSample->waveform->theWaveform && srcSample->waveform->waveSize > 0)
        {
            XPTR pcmCopy = XNewPtr((int32_t)srcSample->waveform->waveSize);
            if (!pcmCopy)
            {
                XDisposePtr((XPTR)waveform);
                return BAE_MEMORY_ERR;
            }
            XBlockMove(srcSample->waveform->theWaveform, pcmCopy, (int32_t)srcSample->waveform->waveSize);
            waveform->theWaveform = (SBYTE *)pcmCopy;
        }
    }
    dstSample->waveform = waveform;
    dest->sampleCount++;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_CopySamplesForPrograms(BAERmfEditorDocument *dest,
                                                      BAERmfEditorDocument const *src,
                                                      unsigned char const *programFlags128,
                                                      uint32_t *outCopiedCount)
{
    uint32_t i;
    uint32_t copiedCount;
    BAEResult result;

    if (!dest || !src)
    {
        return BAE_PARAM_ERR;
    }
    copiedCount = 0;
    for (i = 0; i < src->sampleCount; i++)
    {
        BAERmfEditorSample const *srcSample;

        srcSample = &src->samples[i];
        if (programFlags128)
        {
            if (srcSample->program >= 128)
            {
                continue;
            }
            if (!programFlags128[srcSample->program])
            {
                continue;
            }
        }
        result = PV_CopySampleEntry(dest, srcSample);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        copiedCount++;
    }
    if (outCopiedCount)
    {
        *outCopiedCount = copiedCount;
    }
    PV_MarkDocumentDirty(dest);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_Validate(BAERmfEditorDocument *document)
{
    uint32_t trackIndex;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (document->trackCount == 0)
    {
        return BAE_PARAM_ERR;
    }
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track;

        track = &document->tracks[trackIndex];
        if (track->channel >= BAE_MAX_MIDI_CHANNELS ||
            track->bank > 16383 ||
            track->program >= 128 ||
            track->pan > 127 ||
            track->volume > 127 ||
            track->transpose < -127 ||
            track->transpose > 127)
        {
            return BAE_PARAM_ERR;
        }
    }
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorDocument_SaveAsRmf(BAERmfEditorDocument *document,
                                         BAEPathName filePath)
{
    XFILENAME name;
    XFILE fileRef;
    XLongResourceID midiID;
    ByteBuffer midiData;
    char midiName[256];
    BAEResult result;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }
    result = BAERmfEditorDocument_Validate(document);
    BAE_STDERR("[RMF Save] Validate result=%d, trackCount=%u\n", (int)result, document->trackCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    XSetMemory(&midiData, sizeof(midiData), 0);
    result = PV_BuildMidiFile(document, &midiData);
    BAE_STDERR("[RMF Save] BuildMidiFile result=%d, size=%u\n", (int)result, midiData.size);
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&midiData);
        return result;
    }
    XConvertPathToXFILENAME(filePath, &name);
    result = PV_PrepareResourceFilePath(&name);
    BAE_STDERR("[RMF Save] PrepareResourceFilePath result=%d\n", (int)result);
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&midiData);
        return result;
    }
    fileRef = XFileOpenResource(&name, FALSE);
    BAE_STDERR("[RMF Save] XFileOpenResource fileRef=%p\n", (void *)fileRef);
    if (!fileRef)
    {
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    result = PV_EnsureResourceFileReady(fileRef);
    BAE_STDERR("[RMF Save] EnsureResourceFileReady result=%d\n", (int)result);
    if (result != BAE_NO_ERROR)
    {
        XFileClose(fileRef);
        PV_ByteBufferDispose(&midiData);
        return result;
    }

    if (document->loadedFromRmf && document->originalResourceCount > 0 && document->originalObjectResourceID != 0)
    {
        uint32_t resourceIndex;
        XBOOL wroteMidi;
        XBOOL wroteSong;

        wroteMidi = FALSE;
        wroteSong = FALSE;
        for (resourceIndex = 0; resourceIndex < document->originalResourceCount; ++resourceIndex)
        {
            BAERmfEditorResourceEntry const *entry;

            entry = &document->originalResources[resourceIndex];
            if (entry->type == XFILECACHE_ID)
            {
                continue;
            }
            /* SND and INST resources are regenerated from document->samples below;
             * skip any originals so the new packing fully replaces them. */
            if (entry->type == ID_SND || entry->type == ID_INST)
            {
                continue;
            }
            if (entry->type == document->originalMidiType && entry->id == document->originalObjectResourceID)
            {
                XPTR encodedMidi;
                int32_t encodedMidiSize;

                result = PV_EncodeMidiForResourceType(document->originalMidiType,
                                                      &midiData,
                                                      &encodedMidi,
                                                      &encodedMidiSize);
                if (result != BAE_NO_ERROR)
                {
                    XFileClose(fileRef);
                    PV_ByteBufferDispose(&midiData);
                    return result;
                }
                if (XAddFileResource(fileRef,
                                     document->originalMidiType,
                                     document->originalObjectResourceID,
                                     entry->pascalName,
                                     encodedMidi,
                                     encodedMidiSize) != 0)
                {
                    XDisposePtr(encodedMidi);
                    XFileClose(fileRef);
                    PV_ByteBufferDispose(&midiData);
                    return BAE_FILE_IO_ERROR;
                }
                XDisposePtr(encodedMidi);
                wroteMidi = TRUE;
                continue;
            }
            if (entry->type == ID_SONG && entry->id == document->originalSongID)
            {
                result = PV_AddSongResourceWithID(document,
                                                  fileRef,
                                                  document->originalObjectResourceID,
                                                  document->originalSongID,
                                                  entry->pascalName);
                if (result != BAE_NO_ERROR)
                {
                    XFileClose(fileRef);
                    PV_ByteBufferDispose(&midiData);
                    return result;
                }
                wroteSong = TRUE;
                continue;
            }
            if (XAddFileResource(fileRef,
                                 entry->type,
                                 entry->id,
                                 entry->pascalName,
                                 entry->data,
                                 entry->size) != 0)
            {
                XFileClose(fileRef);
                PV_ByteBufferDispose(&midiData);
                return BAE_FILE_IO_ERROR;
            }
        }
        if (!wroteMidi)
        {
            char midiPascalName[256];
            XPTR encodedMidi;
            int32_t encodedMidiSize;

            PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Song", midiPascalName);
            result = PV_EncodeMidiForResourceType(document->originalMidiType ? document->originalMidiType : ID_MIDI,
                                                  &midiData,
                                                  &encodedMidi,
                                                  &encodedMidiSize);
            if (result != BAE_NO_ERROR)
            {
                XFileClose(fileRef);
                PV_ByteBufferDispose(&midiData);
                return result;
            }
            if (XAddFileResource(fileRef,
                                 document->originalMidiType ? document->originalMidiType : ID_MIDI,
                                 document->originalObjectResourceID,
                                 midiPascalName,
                                 encodedMidi,
                                 encodedMidiSize) != 0)
            {
                XDisposePtr(encodedMidi);
                XFileClose(fileRef);
                PV_ByteBufferDispose(&midiData);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(encodedMidi);
        }
        if (!wroteSong)
        {
            result = PV_AddSongResourceWithID(document,
                                              fileRef,
                                              document->originalObjectResourceID,
                                              document->originalSongID ? document->originalSongID : 1,
                                              NULL);
            if (result != BAE_NO_ERROR)
            {
                XFileClose(fileRef);
                PV_ByteBufferDispose(&midiData);
                return result;
            }
        }
        result = PV_AddSampleResources(document, fileRef);
        BAE_STDERR("[RMF Save] loadedFromRmf AddSampleResources result=%d, sampleCount=%u\n",
                   (int)result, document->sampleCount);
        if (result != BAE_NO_ERROR)
        {
            XFileClose(fileRef);
            PV_ByteBufferDispose(&midiData);
            return result;
        }
        if (XCleanResourceFile(fileRef) == FALSE)
        {
            XFileClose(fileRef);
            PV_ByteBufferDispose(&midiData);
            return BAE_FILE_IO_ERROR;
        }
        XFileClose(fileRef);
        PV_ByteBufferDispose(&midiData);
        return BAE_NO_ERROR;
    }

    if (PV_GetAvailableResourceID(fileRef, ID_MIDI, 1, &midiID) != BAE_NO_ERROR)
    {
        BAE_STDERR("[RMF Save] GetAvailableResourceID for ID_MIDI failed\n");
        XFileClose(fileRef);
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    BAE_STDERR("[RMF Save] Writing MIDI resource id=%ld size=%u\n", (long)midiID, midiData.size);
    PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Song", midiName);
    if (XAddFileResource(fileRef, ID_MIDI, midiID, midiName, midiData.data, midiData.size) != 0)
    {
        BAE_STDERR("[RMF Save] XAddFileResource(MIDI) failed\n");
        XFileClose(fileRef);
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    result = PV_AddSampleResources(document, fileRef);
    BAE_STDERR("[RMF Save] AddSampleResources result=%d, sampleCount=%u\n", (int)result, document->sampleCount);
    if (result == BAE_NO_ERROR)
    {
        result = PV_AddSongResource(document, fileRef, midiID);
        BAE_STDERR("[RMF Save] AddSongResource result=%d\n", (int)result);
    }
    if (result == BAE_NO_ERROR)
    {
        if (XCleanResourceFile(fileRef) == FALSE)
        {
            BAE_STDERR("[RMF Save] XCleanResourceFile failed\n");
            result = BAE_FILE_IO_ERROR;
        }
    }
    BAE_STDERR("[RMF Save] Final result=%d\n", (int)result);
    XFileClose(fileRef);
    PV_ByteBufferDispose(&midiData);
    return result;
}

BAEResult BAERmfEditorDocument_SaveAsMidi(BAERmfEditorDocument *document,
                                          BAEPathName filePath)
{
    XFILENAME name;
    XFILE fileRef;
    ByteBuffer midiData;
    BAEResult result;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }
    result = BAERmfEditorDocument_Validate(document);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    XSetMemory(&midiData, sizeof(midiData), 0);
    result = PV_BuildMidiFile(document, &midiData);
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&midiData);
        return result;
    }
    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenForWrite(&name, TRUE);
    if (!fileRef)
    {
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    if (XFileSetLength(fileRef, 0) != 0 ||
        XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, midiData.data, (int32_t)midiData.size) != 0)
    {
        XFileClose(fileRef);
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);
    PV_ByteBufferDispose(&midiData);
    return BAE_NO_ERROR;
}
