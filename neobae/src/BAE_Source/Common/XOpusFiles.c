/*****************************************************************************/
/*
**  XOpusFiles.c
**
**  Integration of libopusfile for Ogg Opus audio file support in miniBAE
**
**  This file provides decoding support for Ogg Opus audio files
**  using the reference libopusfile implementation.
*/
/*****************************************************************************/

#include "X_API.h"
#include "GenSnd.h"
#include "GenPriv.h"

#if USE_OPUS_DECODER == TRUE
#include <opusfile.h>
#include <opus/opus.h>

// Structure to hold Opus decoder state
typedef struct {
    OggOpusFile *of;
    XBOOL is_open;
} XOpusDecoder;


// Callback functions for libopusfile to read from XFILE
static int opus_read_func(void *stream, unsigned char *ptr, int nbytes)
{
    XFILE file = (XFILE)stream;
    XERR err;
    
    if (nbytes == 0) return 0;
    
    /* XFileRead returns 0 on success, -1 on failure */
    err = XFileRead(file, ptr, nbytes);
    if (err == 0) {
        return nbytes; /* Success - return number of bytes read */
    } else {
        return 0; /* EOF or error */
    }
}

static int opus_seek_func(void *stream, opus_int64 offset, int whence)
{
    XFILE file = (XFILE)stream;
    int32_t new_pos;
    
    switch (whence) {
        case SEEK_SET:
            new_pos = (int32_t)offset;
            break;
        case SEEK_CUR:
            new_pos = XFileGetPosition(file) + (int32_t)offset;
            break;
        case SEEK_END:
            new_pos = XFileGetLength(file) + (int32_t)offset;
            break;
        default:
            return -1;
    }
    
    if (XFileSetPosition(file, new_pos) == 0) {
        return 0;
    } else {
        return -1;
    }
}

static opus_int64 opus_tell_func(void *stream)
{
    XFILE file = (XFILE)stream;
    return (opus_int64)XFileGetPosition(file);
}

static OpusFileCallbacks opus_callbacks = {
    opus_read_func,
    opus_seek_func,
    opus_tell_func,
    NULL  // close_func - we handle closing ourselves
};
#endif


#if USE_OPUS_DECODER == TRUE

// Check if file is an Ogg Opus file
XBOOL XIsOpusFile(XFILE file)
{
    OggOpusFile *of;
    int error;
    long pos;
    
    if (file == NULL) return FALSE;
    
    // Save current position
    pos = XFileGetPosition(file);
    
    // Try to open as Opus file
    of = op_open_callbacks(file, &opus_callbacks, NULL, 0, &error);
    
    // Restore position
    XFileSetPosition(file, pos);
    
    if (of != NULL) {
        op_free(of);
        return TRUE;
    }
    
    return FALSE;
}

// Open Opus file for decoding
void* XOpenOpusFile(XFILE file)
{
    XOpusDecoder *decoder;
    int error;
    
    if (file == NULL) return NULL;
    
    decoder = (XOpusDecoder *)XNewPtr(sizeof(XOpusDecoder));
    if (decoder == NULL) return NULL;
    
    decoder->of = op_open_callbacks(file, &opus_callbacks, NULL, 0, &error);
    if (decoder->of == NULL) {
        XDisposePtr(decoder);
        return NULL;
    }
    
    decoder->is_open = TRUE;
    return decoder;
}

// Get Opus file information
OPErr XGetOpusFileInfo(void *decoder_handle, UINT32 *samples, UINT32 *sample_rate, 
                      UINT32 *channels, UINT32 *bit_depth)
{
    XOpusDecoder *decoder = (XOpusDecoder *)decoder_handle;
    const OpusHead *head;
    
    if (decoder == NULL || decoder->of == NULL || !decoder->is_open) {
        return PARAM_ERR;
    }
    
    head = op_head(decoder->of, -1);
    if (head == NULL) {
        return BAD_FILE;
    }
    
    *channels = head->channel_count;
    *sample_rate = 48000;  // Opus always decodes to 48kHz
    *samples = (UINT32)op_pcm_total(decoder->of, -1);
    *bit_depth = 16;  // Opus decodes to 16-bit PCM
    
    return NO_ERR;
}

// Decode Opus file data
long XDecodeOpusFile(void *decoder_handle, void *buffer, long buffer_size)
{
    XOpusDecoder *decoder = (XOpusDecoder *)decoder_handle;
    int samples_read;
    long bytes_read;
    
    if (decoder == NULL || decoder->of == NULL || !decoder->is_open) {
        return 0;
    }
    
    // op_read returns number of samples read per channel
    // We need to convert to bytes (16-bit samples)
    samples_read = op_read(decoder->of, (opus_int16 *)buffer, buffer_size / 2, NULL);
    
    if (samples_read < 0) {
        return 0;  // Error
    }
    
    bytes_read = samples_read * 2;  // 16-bit samples
    return bytes_read;
}

// Close Opus file
void XCloseOpusFile(void *decoder_handle)
{
    XOpusDecoder *decoder = (XOpusDecoder *)decoder_handle;
    
    if (decoder != NULL) {
        if (decoder->of != NULL && decoder->is_open) {
            op_free(decoder->of);
            decoder->is_open = FALSE;
        }
        XDisposePtr(decoder);
    }
}

#endif // USE_OPUS_DECODER