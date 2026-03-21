# Plan: Bank Editor with Tabbed Interface

## TL;DR
Add a Bank Editor tab alongside the existing MIDI Editor in NeoBAE Studio. The user can switch between tabs freely. When switching from Bank Editor → MIDI Editor, the edited bank is hot-loaded from memory into the mixer for real-time preview. The bank editor displays instruments organized by bank, samples per instrument, and an embedded full instrument editor panel. New C APIs are needed in BAERmfEditor.c for bank-level sample enumeration.

## Phase 1: New C APIs for Bank Sample Enumeration

Currently only 3 bank APIs exist (`BAERmfEditorBank_GetInstrumentCount`, `BAERmfEditorBank_GetInstrumentInfo`, `BAERmfEditorBank_ResolveInstID`). No APIs exist to enumerate samples within a bank instrument.

**Steps:**
1. Define new structures and APIs in `NeoBAE.h`:
   - `BAERmfEditorBank_GetInstrumentSampleCount(bankToken, instrumentIndex, &outCount)` — count key splits/samples for an instrument
   - `BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, instrumentIndex, sampleIndex, &outInfo)` — get sample details (root key, key range, sample rate, loop points, codec, waveform data pointer, frame count)
   - `BAERmfEditorBank_GetInstrumentExtInfo(bankToken, instrumentIndex, &outExtInfo)` — get full ADSR/LFO/LPF/flags data, reusing `BAERmfEditorInstrumentExtInfo` struct
   - `BAERmfEditorBank_SetInstrumentExtInfo(bankToken, instrumentIndex, &extInfo)` — write back edited instrument parameters
   - `BAERmfEditorBank_SetInstrumentSampleInfo(bankToken, instrumentIndex, sampleIndex, &sampleInfo)` — write back edited sample parameters
   - `BAERmfEditorBank_SaveToFile(bankToken, filePath)` — serialize modified bank back to HSB/ZSB
   - Consider: `BAERmfEditorBank_GetSampleWaveform(bankToken, instrumentIndex, sampleIndex, &outData, &outFrames, &outChannels, &outBitDepth)` for waveform display

2. Implement these in `BAERmfEditor.c` (around line 12433+ where existing bank APIs live):
   - Use `XGetIndexedFileResource()` to fetch INST data
   - Parse INST header for key split array (KeySplit structs at offset 13+)
   - For each split, locate the SND resource by `sndResourceID`
   - Decode SND header for sample rate, bit depth, channels, loop points, codec type
   - For ExtInfo: reuse the same INST parsing logic used by `BAERmfEditorDocument_GetInstrumentExtInfo` but operating on bank file resources instead of document resources

**Files:**
- `src/BAE_Source/Common/NeoBAE.h` — new struct/function declarations
- `src/BAE_Source/Common/BAERmfEditor.c` — implementations (~12433+)

## Phase 2: Tabbed Main Window Architecture

**Steps:**
3. Add a `wxNotebook` (or `wxSimplebook`) to the right side of the splitter in `MainFrame` constructor, replacing the current direct `editorPanel`:
   - **Tab 0: "MIDI Editor"** — contains all current editor controls (tempo, transport, piano roll, etc.)
   - **Tab 1: "Bank Editor"** — contains the new bank editor panel
   - The splitter's right pane becomes the notebook instead of `editorPanel`
   - Current `editorPanel` becomes a child of Tab 0's page

4. Store the notebook as member `m_editorNotebook` and track mode with an enum (`EditorMode::MidiEditor`, `EditorMode::BankEditor`).

5. Bind `wxEVT_NOTEBOOK_PAGE_CHANGED` to handle tab switches:
   - On switch to MIDI Editor: hot-load the edited bank from memory (see Phase 4)
   - On switch to Bank Editor: ensure the bank panel reflects current bank state
   - Update sidebar content (tracks vs bank instruments) appropriately, OR use separate sidebars per tab

6. Update `OnOpen()` file dialog to include HSB/ZSB extensions. When an HSB/ZSB is opened:
   - Load it via `BAEMixer_AddBankFromFile()` (existing flow)
   - Switch notebook to Bank Editor tab automatically
   - Populate the bank editor with the loaded bank's instruments
   - When a MIDI/RMF/ZMF is opened, switch to MIDI Editor tab

**Files:**
- `neobae/src/rmfeditor_wx/editor_main.cpp` — MainFrame constructor restructuring, tab switch handling, OnOpen changes

## Phase 3: Bank Editor Panel (new file)

**Steps:**
7. Create `editor_bank.cpp` and `editor_bank.h` with a `BankEditorPanel` class (wxPanel subclass):
   
   **Layout:**
   ```
   BankEditorPanel (wxPanel)
   ├── Left: Instrument/Sample Browser (wxSplitterWindow vertical)
   │   ├── Top: Instrument List (wxTreeCtrl or wxListCtrl)
   │   │   ├── Bank 0: General MIDI
   │   │   │   ├── 0: Acoustic Grand Piano (GM: 1)
   │   │   │   ├── 1: Bright Acoustic Piano (GM: 2)
   │   │   │   └── ...
   │   │   ├── Bank 1: Beatnik Special - Melodic
   │   │   ├── Bank 1: Beatnik Special - Percussion
   │   │   ├── Bank 2: Custom - Melodic
   │   │   └── Bank 2: Custom - Percussion
   │   └── Bottom: Sample List (wxListCtrl)
   │       ├── "C3-D#3 (Root=C4)"
   │       ├── "E3-G3 (Root=F4)"
   │       └── ...
   └── Right: Instrument/Sample Detail Panel
       ├── Instrument properties (name, flags, ADSR, LFO, LPF)
       ├── Sample properties (root key, range, loop points, codec, waveform)
       └── Piano keyboard for preview
   ```

8. Populate instrument list from loaded bank using `BAERmfEditorBank_GetInstrumentCount/Info`:
   - Group instruments by bank number (instID / 128)
   - Separate percussion (bank containing channel 9 instruments, typically instID >= 640) from melodic
   - Display format: `"{program}: {name}"` under bank group headers
   - Use bank-friendly names: "Bank 0: General MIDI", "Bank 1: Beatnik Special", etc.

9. On instrument selection → populate sample list using new `BAERmfEditorBank_GetInstrumentSampleCount/Info` APIs:
   - Show each key split as a row: range, root key, sample rate, codec
   - For non-split instruments (keySplitCount == 0), show single sample entry

10. On sample selection → populate the detail panel with embedded instrument editor content:
    - Refactor `InstrumentExtEditorDialog` to extract reusable panel classes:
      - `InstrumentParamsPanel` — ADSR, LFO, LPF, flags (from BuildInstrumentTab)
      - `SampleDetailPanel` — waveform, loop points, codec info, key range (from BuildSamplesTab)
      - `PianoKeyboardPanel` — already a standalone class (line 109 of editor_instrument_ext_dialog.cpp)
    - These panels are used both in the bank editor (embedded) and in the instrument dialog (for backward compatibility)
    - The bank editor hosts them inline; the dialog wraps them in a wxDialog

11. Add toolbar/buttons for bank editing operations:
    - Save Bank (HSB/ZSB)
    - Add/Delete Instrument
    - Add/Delete Sample (key split)
    - Compress Sample / Compress All
    - The save path should detect whether to use IREZ or ZREZ based on extension (.hsb → IREZ, .zsb → ZREZ) and codec usage

**Files:**
- `neobae/src/rmfeditor_wx/editor_bank.cpp` — new file, BankEditorPanel implementation
- `neobae/src/rmfeditor_wx/editor_bank.h` — new file, BankEditorPanel C-style interface (matching project pattern)
- `neobae/src/rmfeditor_wx/editor_instrument_ext_dialog.cpp` — refactor to extract reusable panels
- `neobae/src/rmfeditor_wx/editor_instrument_ext_dialog.h` — updated declarations

## Phase 4: Bank Hot-Reload on Tab Switch

**Steps:**
12. When user switches from Bank Editor → MIDI Editor tab:
    - Serialize the current bank state to memory using a new API: `BAERmfEditorBank_SaveToMemory(bankToken, &outData, &outSize)`
    - Unload the current bank from the mixer: `BAEMixer_UnloadBank(mixer, token)`
    - Reload the modified bank from memory: `BAEMixer_AddBankFromMemory(mixer, data, size, &newToken)`
    - Update `m_bankToken` and `m_bankTokens` vector with new token
    - Update the "Current: {bankName}" menu item
    - This ensures MIDI playback uses the latest edited instruments

13. Add a "dirty" flag to BankEditorPanel so hot-reload only happens when the bank was actually modified.

**Files:**
- `src/BAE_Source/Common/NeoBAE.h` — `BAERmfEditorBank_SaveToMemory` declaration
- `src/BAE_Source/Common/BAERmfEditor.c` — implementation
- `neobae/src/rmfeditor_wx/editor_main.cpp` — tab switch handler, bank reload logic

## Phase 5: Sidebar Adaptation

**Steps:**
14. Decide sidebar strategy for bank mode. Two options:
    - **Option A (Recommended):** Repurpose the existing sidebar — when Bank Editor tab is active, hide track list and show instrument browser in the sidebar. When MIDI Editor tab is active, restore tracks + sample tree.
    - **Option B:** Each tab has its own sidebar built into its panel (instrument browser is part of BankEditorPanel), and the main sidebar only shows for MIDI mode.
    
    Going with **Option A** keeps the layout consistent. Implement by showing/hiding sidebar child widgets on tab switch.

15. Add bank-specific context menu to the instrument tree in bank mode:
    - Edit Instrument
    - Clone Instrument
    - Alias Instrument
    - Delete Instrument
    - Compress Samples
    - Add New Instrument
    - Compress All Samples

(side note: we should probably implement all of those (some already are) in the MIDI editor)

**Files:**
- `neobae/src/rmfeditor_wx/editor_main.cpp` — sidebar toggling logic
- `neobae/src/rmfeditor_wx/editor_bank.cpp` — context menus

## Relevant Files

- `neobae/src/BAE_Source/Common/NeoBAE.h` — API declarations; existing bank API block at line ~3262; `BAERmfEditorBankInstrumentInfo` struct, `BAERmfEditorInstrumentExtInfo`, `BAERmfEditorSampleInfo`
- `neobae/src/BAE_Source/Common/BAERmfEditor.c` — API implementations; existing bank enumeration at ~12433; INST parsing, KeySplit struct handling
- `neobae/src/BAE_Source/Common/GenPatch.c` — `PV_GetInstrument()` at ~795, INST header binary layout reference, KeySplit struct at ~252
- `neobae/src/rmfeditor_wx/editor_main.cpp` — MainFrame class at line 276, constructor UI layout 276-650, `LoadDocument()` at 4609, `OnOpen()` at 4774, `LoadBankFromFile()` at 1492, `PopulateSampleList()` at 1541
- `neobae/src/rmfeditor_wx/editor_instrument_ext_dialog.cpp` — `InstrumentExtEditorDialog` at line 1087; reusable panels: `PianoKeyboardPanel` (109), `WaveformPanelExt` (509), `ADSRGraphPanel` (808), `LFOWaveformGraphPanel` (971); `BuildInstrumentTab()` and `BuildSamplesTab()` methods
- `neobae/src/rmfeditor_wx/editor_instrument_ext_dialog.h` — dialog interface, `ShowInstrumentExtEditorDialog()` function
- `neobae/src/rmfeditor_wx/editor_bank.cpp` — **NEW** BankEditorPanel implementation
- `neobae/src/rmfeditor_wx/editor_bank.h` — **NEW** BankEditorPanel interface
- `neobae/src/BAE_Source/Common/X_Formats.h` — resource type constants (ID_INST, ID_SND, etc.)

## Verification

1. Build with `make -f Makefile.rmfeditor-wx -j$(nproc)` — confirm no compile errors
2. Open an RMF/MIDI file — verify MIDI Editor tab appears and works identically to current behavior
3. Open an HSB/ZSB file — verify Bank Editor tab activates and instruments are listed correctly with proper bank grouping
4. Click an instrument in bank editor — verify sample list populates with correct key splits
5. Click a sample — verify embedded instrument editor shows waveform, ADSR, LFO, loop points, keyboard
6. Edit an instrument parameter (e.g., ADSR stage) and switch to MIDI Editor tab — verify the bank hot-reloads and playback uses updated instrument
7. Switch back to Bank Editor — verify edits are preserved
8. Save bank as HSB — verify IREZ header; save as ZSB with modern codecs — verify ZREZ header
9. Test with built-in bank (no external bank loaded) — bank editor should be empty or show built-in instruments
10. Test `rmfinfo` on saved bank files to verify structural integrity

## Decisions

- **Start From Scratch** Allow the user to start creating a bank without loading one
- **Tabbed interface** (wxNotebook) chosen over destroy/rebuild — allows seamless switching and preserves state
- **Hot-reload on tab switch** — bank is serialized to memory and reloaded into mixer when switching Bank Editor → MIDI Editor, only if dirty
- **Full editing from the start** — not read-only; includes ADSR/LFO/LPF editing, sample replacement, save
- **Embedded full instrument editor** — reusable panels extracted from `InstrumentExtEditorDialog`, not simplified view
- **Option A sidebar** — repurpose existing sidebar with show/hide rather than duplicate sidebars
- **New C APIs** — required for sample enumeration from bank files (none currently exist)
- **Scope includes:** instrument/sample browsing, inline editing, bank save (HSB/ZSB), hot-reload
- **Scope excludes:** SF2/DLS support, MIDI recording, starting from scratch (no blank document)
- **Bank Editor Bank Loading Error Handling:** Show the user an error. Revert to a blank empty document.
- **Tab Switching Bank Loading Error Handling:** If the bank in the bank editor fails to load to memory when switching to the MIDI tab, we should show an error (preferrably with the error code and translation), then fall back to our embedded bank.

## Further Considerations

1. **Undo/Redo for bank edits**: The current undo system is document-based (snapshot of `BAERmfEditorDocument`). Bank edits should have a separate undo stack. This could be implemented by storing snapshots of the bank's instrument/sample state in memory, or by implementing command-based undo for individual parameter changes. For Phase 1, we can start without undo and add it in a later phase once the core editing functionality is stable.

2. **Instrument preview in bank editor**: The existing preview system uses `BAESong` for MIDI playback and `BAESound` for raw sample preview. For bank instrument preview, we can reuse the `PianoKeyboardPanel` callbacks to trigger note-on/off via the mixer with the bank's instruments loaded. This should work with the existing `PreviewPianoRollNote()` pattern.

3. **Percussion bank display**: Group under "Bank N - Percussion" with instrument name showing the note mapping (e.g., "35: Acoustic Bass Drum").

## Bug Fix: Codec Detection in Sample List

### Problem
The bank editor sample list shows incorrect information for FLAC, Vorbis, and Opus compressed samples: 0 Hz sample rate, "PCM" codec label, 0 frames, etc. All fields that should be populated from the SND resource header are zeroed out.

### Root Cause
`BAERmfEditorBank_GetInstrumentSampleInfo()` in BAERmfEditor.c (line ~13196) calls `XGetSampleInfoFromSnd()` to parse SND resources. It only uses the results when the function returns 0 (success):
```c
if (XGetSampleInfoFromSnd(sndData, &sampleInfo) == 0)
{
    outInfo->sampleRate = ...;   /* only populated on success */
    ...
}
```

`XGetSampleInfoFromSnd()` in SampleTools.c (line ~716) handles XType3Header resources by reading the header fields (rate, channels, bitSize, frames, loops, baseKey, compressionType) and then validating `compressionType` in a switch. The switch has cases for:
- `C_NONE` — accepted
- `C_IMA4`, `C_IMA4_WAV` — accepted
- `C_MPEG_*` (all MP3 bitrates) — accepted, with frame recalculation

But **FLAC, Vorbis, and Opus are NOT handled** — they fall to `default: err = -3`. This causes the entire result to be discarded by the caller even though the header fields (rate, channels, etc.) were already correctly parsed from the XType3Header before the switch.

Note: `XGetSamplePtrFromSnd()` (the full decode version, line ~930) DOES handle all three codecs correctly — it has explicit `case C_FLAC:`, `case C_VORBIS:`, `case C_OPUS:` blocks. The info-only version was never updated to match.

### Fix

**File:** `src/BAE_Source/Common/SampleTools.c` — `XGetSampleInfoFromSnd()`, XType3Header switch (~line 848)

Add cases for FLAC, Vorbis, and Opus matching what `XGetSamplePtrFromSnd` does:

```c
#if USE_FLAC_DECODER == TRUE
                    case C_FLAC:
                        pOutInfo->size = XGetLong(&header3->encodedBytes);
                        pOutInfo->frames = XGetLong(&header3->frameCount);
                        pOutInfo->bitSize = 16;
                        break;
#endif
#if USE_VORBIS_DECODER == TRUE
                    case C_VORBIS:
                        pOutInfo->size = XGetLong(&header3->encodedBytes);
                        pOutInfo->frames = XGetLong(&header3->frameCount);
                        pOutInfo->bitSize = 16;
                        break;
#endif
#if USE_OPUS_DECODER == TRUE
                    case C_OPUS:
                        pOutInfo->size = XGetLong(&header3->encodedBytes);
                        pOutInfo->frames = XGetLong(&header3->frameCount);
                        pOutInfo->bitSize = 16;
                        break;
#endif
```

These go in the `case XType3Header:` block, after the MPEG cases and before `default:`.

**Why `bitSize = 16`:** FLAC/Vorbis/Opus always decode to 16-bit PCM. The header may store a different value (e.g., 8-bit for pre-encode source), but the decoded output is 16-bit.

**Opus round-trip note:** For Opus round-trip samples, the stored `sampleRate` in the header is the true source rate (e.g., 22050), not 48000. This is correct for display purposes — the sample list should show the original rate. No special handling needed here since we're reading the header directly.

### Verification
1. Load an HSB/ZSB bank containing FLAC, Vorbis, or Opus compressed samples
2. Verify the sample list shows correct: sample rate (not 0), codec name (not "PCM"), frame count, bit depth
3. Verify PCM, ADPCM, and MP3 samples still display correctly (regression check)

## Feature: Sample Editor Dialog

### Overview
Add a modal dialog for editing individual sample properties within a bank instrument. The dialog is opened by double-clicking a sample in the bank editor's sample list, or via a context menu "Edit Sample" action. It provides editable controls for sample metadata, loop points, key mapping, codec recompression, and a waveform display.

### Dialog Layout
```
+--------------------------------------------------------+
|  Edit Sample: SND 1234                                 |
+--------------------------------------------------------+
|  [Waveform Display Panel]                              |
|  (shows PCM waveform with loop markers)                |
|                                                        |
+--------------------------------------------------------+
|  Sample Properties                                     |
|  +-----------+----------+-----------+----------+       |
|  | Root Key  | [60   ]  | Low Key   | [0    ]  |       |
|  | High Key  | [127  ]  | Split Vol | [100  ]  |       |
|  +-----------+----------+-----------+----------+       |
|                                                        |
|  Audio Info (read-only unless recompressing)            |
|  +-----------+----------+-----------+----------+       |
|  | Rate (Hz) | 22050    | Frames    | 48000    |       |
|  | Bit Depth | 16-bit   | Channels  | mono     |       |
|  | Codec     | Vorbis   |                      |       |
|  +-----------+----------+-----------+----------+       |
|                                                        |
|  Loop                                                  |
|  [x] Enable Loop  Start: [1000 ] End: [45000 ]        |
|  (12345 frames, 559.4 ms)                              |
|                                                        |
|  Compression                                           |
|  Codec: [Don't Change v]  Bitrate: [--- v]             |
|  Opus Mode: [Audio v]                                  |
|                                                        |
|  Actions                                               |
|  [Replace Sample...] [Export WAV...] [Preview]         |
|                                                        |
+--------------------------------------------------------+
|                        [OK]  [Cancel]  [Apply]         |
+--------------------------------------------------------+
```

### Implementation Steps

#### Step 1: New C APIs for Sample Data Access

**File:** `src/BAE_Source/Common/NeoBAE.h`

```c
/* Decode and return the raw PCM waveform data for a bank sample.
 * Used for waveform display and sample export.
 * Caller must free *outData with XDisposePtr. */
BAEResult BAERmfEditorBank_GetSampleWaveformData(BAEBankToken bankToken,
                                                  uint32_t instrumentIndex,
                                                  uint32_t sampleIndex,
                                                  unsigned char **outData,
                                                  uint32_t *outFrames,
                                                  int16_t *outChannels,
                                                  int16_t *outBitDepth,
                                                  uint32_t *outSampleRate);

/* Replace the SND resource data for a sample in a bank instrument.
 * newSndData should be a complete SND resource blob (as produced by
 * XCreateSoundObjectFromData). The old SND is removed and the new
 * one is inserted. */
BAEResult BAERmfEditorBank_ReplaceSampleSnd(BAEBankToken bankToken,
                                             XShortResourceID sndResourceID,
                                             XPTR newSndData,
                                             int32_t newSndSize);
```

**File:** `src/BAE_Source/Common/BAERmfEditor.c`

Implement `BAERmfEditorBank_GetSampleWaveformData`:
1. Look up the SND resource via `XGetFileResource()` (try ID_SND, ID_CSND, ID_ESND)
2. Call `XGetSamplePtrFromSnd()` to decode to PCM
3. Copy decoded data to output buffer
4. Return sample metadata (frames, channels, bitDepth, rate)

Implement `BAERmfEditorBank_ReplaceSampleSnd`:
1. Delete the existing SND resource with `XDeleteFileResource()`
2. Add the new SND resource with `XAddFileResource()` using same resource ID
3. This is used when recompressing or replacing a sample

#### Step 2: Sample Editor Dialog (C++ / wxWidgets)

**New files:**
- `src/rmfeditor_wx/editor_sample_dialog.cpp`
- `src/rmfeditor_wx/editor_sample_dialog.h`

**Dialog class: `SampleEditorDialog`** (wxDialog subclass)

Constructor parameters:
- `wxWindow *parent`
- `BAEBankToken bankToken`
- `uint32_t instrumentIndex`
- `uint32_t sampleIndex`

On open:
1. Call `BAERmfEditorBank_GetInstrumentSampleInfo()` to populate property fields
2. Call `BAERmfEditorBank_GetSampleWaveformData()` to get PCM for waveform display
3. Populate all controls with current values

**Controls:**
- **Waveform panel**: Reuse `WaveformPanelExt` from `editor_instrument_panels.h` (already exists, renders PCM waveform with loop markers)
- **Root Key / Low Key / High Key / Split Volume**: wxSpinCtrl controls (editable)
- **Sample Rate / Frames / Bit Depth / Channels / Codec**: wxStaticText (read-only info display)
- **Loop Enable**: wxCheckBox; Loop Start/End: wxSpinCtrl (editable)
- **Compression codec**: wxChoice with "Don't Change", "RAW PCM", "ADPCM", "MP3", "VORBIS", "FLAC", "OPUS", "OPUS (Round-Trip)" — reuse pattern from `editor_instrument_ext_dialog.cpp` (line ~1218)
- **Bitrate**: wxChoice, populated dynamically based on codec selection — reuse `CompressionTypeToCodecBitrate()` / `CodecBitrateToCompressionType()` from NeoBAE.h
- **Opus Mode**: wxChoice ("Audio", "Voice"), enabled only for Opus codecs
- **Replace Sample button**: Opens file dialog for WAV/AIFF/FLAC/OGG, imports new audio, creates new SND via `XCreateSoundObjectFromData()`, calls `BAERmfEditorBank_ReplaceSampleSnd()`
- **Export WAV button**: Writes decoded PCM to WAV file (use `XTranslateFromSampleDataToWaveform()` + WAV writer)
- **Preview button**: Play the decoded sample through `BAESound`

On OK/Apply:
1. Update key mapping: Call `BAERmfEditorBank_SetInstrumentSampleInfo()` with edited rootKey, lowKey, highKey, splitVolume, loopStart, loopEnd
2. If codec changed (not "Don't Change"):
   a. Decode current SND to PCM via `XGetSamplePtrFromSnd()`
   b. Re-encode via `XCreateSoundObjectFromData()` with selected codec/bitrate/subtype
   c. Replace SND resource via `BAERmfEditorBank_ReplaceSampleSnd()`
3. Refresh the sample list and detail display in the bank editor

#### Step 3: Wire Up to Bank Editor

**File:** `src/rmfeditor_wx/editor_bank.cpp`

1. Bind `wxEVT_LIST_ITEM_ACTIVATED` (double-click) on `sampleList` to open `SampleEditorDialog`
2. Add context menu on right-click with "Edit Sample...", "Export WAV...", "Delete Sample"
3. After dialog closes with OK, call `PopulateSampleList()` and `ShowSampleDetail()` to refresh

#### Step 4: Add to Makefile

**File:** `neobae/Makefile.rmfeditor-wx`

Add `editor_sample_dialog.cpp` to the source list.

### Relevant Existing Code to Reuse
- `WaveformPanelExt` (editor_instrument_panels.h) — waveform rendering with loop markers
- `PianoKeyboardPanel` (editor_instrument_panels.h) — preview keyboard
- Codec/bitrate selection pattern from `editor_instrument_ext_dialog.cpp` BuildSamplesTab() (line ~1218)
- `CompressionTypeToCodecBitrate()` / `CodecBitrateToCompressionType()` from NeoBAE.h — codec↔bitrate mapping
- `CompressionTypeName()` from editor_bank.cpp — codec display names
- `XCreateSoundObjectFromData()` from SampleTools.c — encode PCM to SND resource
- `XGetSamplePtrFromSnd()` from SampleTools.c — decode SND to PCM

### Verification
1. Open a bank file in the bank editor
2. Select an instrument, then double-click a sample in the sample list
3. Verify the dialog displays: correct waveform, correct property values, correct codec info
4. Edit root key, save — verify the instrument plays at the new pitch
5. Edit loop points, save — verify loop playback is correct
6. Change codec to Vorbis 64k, save — verify the sample is recompressed and plays correctly
7. Replace sample with a new WAV file — verify the new audio plays
8. Export WAV — verify the exported file is valid
9. Cancel the dialog — verify no changes were made
