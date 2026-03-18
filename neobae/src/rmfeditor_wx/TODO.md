# 0.06a
- ~~Move "Opus Mode" into row with Codec/Bitrate in instrument dialog~~
- ~~Add "musical keyboard" to instrument dialog preview.~~
  - ~~Reference GUI for keybinds~~
- ~~Support stereo waveform in instrument dialog~~
- ~~Allow configuring loop point with mouse and waveform image~~
- ~~Show currently loaded bank somewhere~~
- ~~Add MIDI track rename context menu option~~
- ~~Title filename: currently sticks to last loaded file (media or session). If its currently a media file, and the user saves a session, the title should update to the .nbs session name.~~
- ~~Save/Export: populate the filename with the currently loaded file and lowest extension~~
- ~~Playback only certain channels~~
- ~~Add "All" "None" "Invert" buttons to channel picker~~
- ~~Export only certain channel(s)~~

# 0.07a
- Changing the program/bank of an note does not reflect in preview/output
- tick0 note seen as 2:0 instead of 2:4, missing CC? (`Rhodium.rmf`)
- ~~Musical keyboard doesn't stop when key released~~
- ~~Make it easier to get back to the channel dialog~~
- ~~Allow export of RMF/ZMF to MIDI with warning about the output file not having custom instruments/samples.~~
- Visible/editable MIDI Loop representation in piano roll
- Interpolation configuration (none, linear, cubic, etc, current is just on/off)

# Future
- Tab design, one tab for MIDI data (current), one for instrument and sample data (replace instrument dialog), one for Bank Editing
  - Bank Edit tab has all instruments/samples listed in a large tree
  - Context menu options for Clone/Alias functionality
  - Context menu for compressing one instrument's samples or All instruments samples in the bank like we have for songs
  - Maybe MIDI Data and Bank Editor should be mutually exclusive (show midi data for MID/RMF/ZMF, show Bank Editor for HSB/ZSB)

- "Clone all used instruments" - Clone used instruments from MIDI stream to RMF instruments, reassigning their bank/program as needed
- `Crazy Dream fix.rmf` plays bass/gt sample instead of saw lead
- Allow for automation like Volume to be able have a slide on it so its easy to make a fadeout/fadein for example, the edit dialog could have "start (item)" "end (item)"
- Resizable tracks (allow user to expand or shrink the track by click/dragging the far right end of the ruler)
- Neo Reverb for preview player
  - Custom .neoreverb support
  - means we need the reverb edit dialog from zefidi
- Larger instrument dialog piano not working 100% correctly

# Harder stuff
- Externally imported MP3 samples (not encoded by us) may have a gap
  - How to address without breaking backwards compatiblity with the decoder?

- Start from nothing (currently the editor requires you to load a file)
  - Requires resizable tracks, probably more

- Allow configuration of SysEx and 'non-standard' CC commands
  - Where do we even put this stuff?  

- MIDI In (record to track)
  - Use RtMidi
  - Hook alsa/jack/winmm as we do with Zefidi
  - Allow MIDI in for instrument dialog preview
  - Need to find my MIDI keyboard

- Sound Bank Editor
  - Allow editing of soundbanks (HSB, ZSB) directly
  - Add ZSB format (like ZMF, it would mimic HSB but use a ZREZ header and be used mostly for the new codecs)
  
# Maybe (really hard stuff)
- SF2/DLS support
  - How? FluidSynth handles all of that.
  - Do we try to convert instrument data? But the ADSR is different in RMF/ZMF/HSB.
