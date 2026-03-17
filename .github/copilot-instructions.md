# miniBAE Development Guidelines

## Architecture Overview

miniBAE is a cross-platform audio engine with these core components:

- **Audio Engine**: Core synthesis and playback in `src/BAE_Source/Common/`
- **Platform Layer**: OS abstraction in `src/BAE_Source/Platform/` 
- **Format Handlers**: Support for MIDI, RMF, ZMF, WAV, AIFF, AU, FLAC, OGG VORBIS, MP3
- **Optional Formats**: SoundFont 2.0, SoundFont 3.0, DLS, XMF, MXMF
- **Applications**: `playbae` (CLI) and `zefidi` (GUI) frontends
- **Sample Cache**: Efficient memory management for audio samples
- **Android App**: at `src/miniBAEDroid`

## Build System

### Key Make Variables
- `USE_SDL=1`: Enable SDL2 audio output
- `DEBUG=1`: Enable debug output in builds (playbae: stderr, zefidi: zefidi.log)
- `LDEBUG=1`: Disable optimizations and don't strip for proper gdb debugging
- `NOAUTO=1`: Manual control of features (disable auto-enabling)
- `MP3_DEC=1/MP3_ENC=1`: MP3 support
- `FLAC_DEC=1/FLAC_ENC=1`: FLAC support  
- `VORBIS_DEC=1/VORBIS_ENC=1`: OGG Vorbis support
- `OPUS_DEC=1/OPUS_ENC=1`: Opus support
- `SF2_SUPPORT=1`: SoundFont 2.0 support
    - `USE_FLUIDSYNTH=1`: FluidSynth SF2 with DLS support, required for `XMF_SUPPORT=1` 
- `XMF_SUPPORT=1`: Support for XMF files (requires FluidSynth)
- `ZMF_SUPPORT=1`: Support for ZMF files with modern codecs (FLAC, Vorbis, Opus)
- `KARAOKE=1`: MIDI karaoke lyrics support

### XMF and MXMF Support
- Requires `USE_FLUIDSYNTH=1`, due to DLS requirement
- XMF: Extended MIDI with DLS samples
- MXMF: Beatnik's proprietary format with DLS samples
- Loader in `src/BAE_Source/Common/GenXMF.c`

## RMF Editor
- Does not currently support XMF/MXMF
- Does not support SoundFont 2.0 or DLS instruments
- Focused on RMF/ZMF authoring with support for modern codecs
- Editor code in `src/rmfeditor_wx/`

### Build Commands
```bash
# Linux SDL2 build
make clean && make -j$(nproc)

# Windows MinGW cross-compile  
make -f Makefile.mingw USE_SDL=1 -j$(nproc)

# Windows MinGW GUI cross-compile  
make -f Makefile.gui-mingw -j$(nproc)

# GUI application
make -f Makefile.gui -j$(nproc)

# WebAssembly
make -f Makefile.emcc -j$(nproc)

# When debugging
make DEBUG=1 -j$(nproc)

# When debugging crashes
make DEBUG=1 LDEBUG=1 -j$(nproc)

# When debugging XMF or MXMF files (requires FluidSynth)
make DEBUG=1 USE_FLUIDSYNTH=1 -j$(nproc)

# Building the RMF Editor
make -f Makefile.rmfeditor-wx -j$(nproc)

# Debugging with playbae (XMF)
bin/playbae -f ../content/xmf/midnightsoul.xmf -t 3 2>&1
```

### FluidSynth Quirks
- When loading a DLS, it will always report `fluidsynth: error: Not a SoundFont file`, we must ignore that "error".

### Platform Support
- **Linux**: SDL2/SDL3, ALSA/JACK (GUI)
- **Windows**: MinGW, DirectSound/SDL2/SDL3, WinMM MIDI (zefidi GUI)
- **macOS**: Native audio (TODO)
- **WebAssembly**: Emscripten

## RMF and ZMF Reference

### Container IDs and Detection
- **RMF** uses the `IREZ` resource-map header (`XFILERESOURCE_ID`)
- **ZMF (Zefie Music Format)** uses the `ZREZ` resource-map header (`XFILERESOURCE_ZMF_ID`)
- Content-based detection in `src/BAE_Source/Common/XFileTypes.c` recognizes both `IREZ` and `ZREZ`
- Tooling in `src/rmfinfo/rmfinfo.c` also treats both headers as valid RMF-family containers

### RMF Structure (High-Level)
- Resource files start with `XFILERESOURCEMAP` (`mapID`, `version`, `totalResources`)
- Resources are stored as linked entries (`nextentry`, `resourceType`, `resourceID`, name, length, payload)
- Common resources for authored songs:
    - `ID_SONG`: song object metadata and MIDI relationship
    - `ID_INST`: instrument definitions, splits, and synthesis parameters
    - `ID_SND` / `ID_CSND` / `ID_ESND`: sample payloads (raw or encoded)
    - `ID_BANK`: bank metadata where applicable

### ZMF Compatibility Rules
- ZMF keeps RMF resource structure, but is used when samples rely on modern codecs
- Runtime load path rejects **IREZ** files containing FLAC/Vorbis/Opus sample subtypes with `BAE_UNSUPPORTED_FORMAT`
- Editor save logic chooses header by extension:
    - `.zmf` -> `ZREZ`
    - anything else (including `.rmf`) -> `IREZ`
- `BAERmfEditorDocument_RequiresZmf()` forces ZMF workflows when document samples target or preserve FLAC/Vorbis/Opus

## Engine Quirks and Gotchas

### Build and Flags
- Keep `clean` separate from parallel build (`make clean && make -j$(nproc)`), do not combine `clean` with `-j` target chains
- `ZMF_SUPPORT=1` cascades codec support (OGG, MP3, FLAC, Vorbis, Opus; decode and encode)
- `SF2_SUPPORT=1` forces `USE_FLUIDSYNTH=1`; if FluidSynth is off, `XMF_SUPPORT` is disabled
- `USE_SDL2` and `USE_SDL3` are mutually exclusive in `inc/Makefile.common`

### Runtime and Platform Behavior
- FluidSynth DLS load path logs `Not a SoundFont file`; this message is expected and should not be treated as fatal
- On Linux/WSL systems without `/dev/snd/seq`, ALSA-enabled GUI MIDI device access can segfault
- `zefidi` file dialogs rely on `zenity`, `kdialog`, or `yad`; missing tools can break Open/Load/Export/Record actions

### Audio Engine and Data Handling
- Mixer pipeline uses high-precision intermediate buffers and hard saturation on final 16-bit output (no soft limiter)
- Many timing/rate values in RMF and engine internals are fixed-point (16.16); treat raw Hz and fixed values carefully in tooling
- Memory APIs are manual (`XNewPtr`/`XDisposePtr`); leaked `XPTR` blocks are a common failure mode in new code

## Debugging Fast-Path

1. Start with `make DEBUG=1` for verbose logs; use `make DEBUG=1 LDEBUG=1` for symbolized crash debugging.
2. For RMF/ZMF load failures, verify the header first (`IREZ` vs `ZREZ`) using `rmfinfo`.
3. If an RMF with modern codecs fails to load, re-save as `.zmf` so the container header is `ZREZ`.
4. When debugging DLS/SF2 behavior with FluidSynth, ignore the expected `Not a SoundFont file` noise and focus on subsequent errors.

## Code Patterns

### Memory Management
```c
// Allocate memory
XPTR data = XNewPtr(size);
if (!data) return MEMORY_ERR;

// Free memory  
XDisposePtr(data);
```

### Error Handling
```c
OPErr result = some_function();
if (result != NO_ERR) {
    // Handle error
    return result;
}
```

### Fixed-Point Arithmetic
```c
// Convert float to 16.16 fixed point
XFIXED fixed = (XFIXED)(float_value * 65536.0f);

// Convert back to float
float float_value = (float)fixed / 65536.0f;
```

### Conditional Compilation
```c
#if USE_SF2_SUPPORT == TRUE
// SF2-specific code
#endif

#if USE_DLS_SUPPORT == TRUE
// DLS-specific code
#endif

#ifdef _ZEFI_GUI
// GUI-specific code  
#endif
```

### Structure Alignment
```c
// Force 8-byte alignment for performance
struct GM_Instrument {
    // ... fields ...
} __attribute__((aligned(8)));
```

## Audio Format Support

### MIDI Processing
- Real-time synthesis with wavetable instruments
- ADSR envelopes, LFOs, filters
- Support for General MIDI, SoundFont 2.0
- Karaoke lyrics via meta events

### Sample Cache System
- Efficient memory management for audio samples
- Reference counting for shared samples
- Cache hit/miss tracking

### Platform Audio Output
- SDL2 for cross-platform compatibility
- Hardware MIDI input/output (GUI)
- Real-time audio mixing and effects

## Integration Points

### External Libraries
- **SDL2**: Cross-platform audio/video
- **FLAC/Ogg/Vorbis**: Audio codecs
- **RtMidi**: Hardware MIDI support
- **LAME**: MP3 encoding

### File Formats
- **MIDI**: Standard MIDI files (.mid)
- **RMF**: Beatnik Rich Music Format (.rmf)  
- **Audio**: WAV, AIFF, AU, FLAC, OGG, MP3
- **Banks**: SoundFont 2.0 (.sf2), DLS (.dls), HSB patches

## Development Workflow

### Adding New Features
1. Check `inc/Makefile.common` for build flags
2. Add conditional compilation blocks
3. Update platform-specific code if needed
4. Test across supported platforms

### Debugging Audio Issues
1. Enable debug builds: `make DEBUG=1`
2. Check debug output in `playbae` with verbose flags
3. Use `GM_GetRealtimeAudioInformation()` for voice status
4. Verify sample cache usage with memory tools

### Debugging crashes
1. Enable debug builds: `make DEBUG=1 LDEBUG=1`
2. Run playbae with gdb: `gdb --args bin/playbae -f /mnt/d/Music/MIDI/Mario/kart-credits.mid -t 3`

### Testing
- Test with various audio formats and sample rates
- Verify cross-platform compatibility
- Check memory usage with `BAE_GetSizeOfMemoryUsed()`
- Test real-time performance and latency

## Common Pitfalls

### Memory Leaks
- Always free `XPTR` allocations with `XDisposePtr()`
- Check reference counting in sample cache
- Use debug builds to track allocations

### Platform Differences
- Audio output APIs vary by platform
- File path handling differs (Unix vs Windows)
- Threading models may differ

### Performance Issues
- Fixed-point math for real-time audio
- Avoid allocations during audio processing
- Use appropriate interpolation modes
- Monitor voice usage and CPU load

### Build Configuration
- Many features are optional - check which are enabled
- Some codecs require external libraries
- Cross-compilation has specific requirements