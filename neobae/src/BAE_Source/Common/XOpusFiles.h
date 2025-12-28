/*****************************************************************************/
/*
**  XOpusFiles.h
**
**  Header for Ogg Opus audio file support in miniBAE
*/
/*****************************************************************************/

#ifndef __X_OPUS_FILES__
#define __X_OPUS_FILES__

#include "X_API.h"

#if USE_OPUS_DECODER == TRUE

#ifdef __cplusplus
extern "C" {
#endif


// Opus decoder functions
XBOOL XIsOpusFile(XFILE file);
void* XOpenOpusFile(XFILE file);
OPErr XGetOpusFileInfo(void *decoder_handle, UINT32 *samples, UINT32 *sample_rate, 
                      UINT32 *channels, UINT32 *bit_depth);
long XDecodeOpusFile(void *decoder_handle, void *buffer, long buffer_size);
void XCloseOpusFile(void *decoder_handle);

#ifdef __cplusplus
}
#endif

#endif // USE_OPUS_DECODER

#endif // __X_OPUS_FILES__