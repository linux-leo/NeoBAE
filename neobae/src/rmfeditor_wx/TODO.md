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
- ~~Non-fatal assertion error when right clicking a sample or instrument~~
- ~~"Musical Keyboard"~~
  - ~~releasing key sometimes doesnt stop playback~~
  - ~~cannot play multiple notes~~
  - ~~highlight ALL active notes~~
  - ~~center "musical keyboard" around rootKey~~
- ~~Make it easier to get back to the channel dialog~~
- ~~Allow export of RMF/ZMF to MIDI with warning about the output file not having custom instruments/samples.~~
- ~~Visible/editable MIDI Loop representation in piano roll~~
- ~~Changing the program/bank of a note does not reflect in song preview or export~~
- ~~Right clicking a track in the track list should select the item under cursor like sample tree does~~
- ~~tick0 note seen as 2:0 instead of 2:4, missing CC? (`Rhodium.rmf`) (tick0 seems fine (2:1) for `DoorSlam.rmf`)~~
- ~~Sample mismatch when a SNDID = 0~~
- ~~Piano roll preview should respect channel and if its the percussion channel (9, zero indexed), it should preview drum notes when clicking notes~~
- ~~`Crazy Dream fix.rmf` plays bass/gt sample instead of saw lead~~
- ~~Opus RT loop end is off~~
- ~~so is normal Opus a little~~
- ~~Check ADSR (doesn't work in instrument dialog preview) `Grasp.rmf`~~
- ~~Instrument dialog preview allows playing of notes outside of key range~~
- ~~No polyphony on instrument dialog preview~~
- ~~Releasing one note when polyphonic on instrument dialog preview stops all notes~~
- ~~Pressing two notes at the same time in the instrument dialog preview, one note will be a piano~~
- ~~Custom instrument doesn't play in piano roll preview, but works in output~~
- ~~Custom instrument doesn't play in instrument dialog preview~~
- ~~Instrument dialog preview can be broken with key mashing~~
~~- Clone all used instruments~~
  - ~~Clone used instruments from MIDI stream to RMF instruments, reassigning the notes and events banks/programs as needed~~
  - ~~Example: A MIDI uses 0:1 piano. We clone it, it becomes 2:0, we reassign all 0:1 notes to 2:0.~~
  - ~~Cloned instruments should support pointers/aliasing/whatever is needed (eg Chippy bank)~~
  - ~~Needs to support percussions too~~
  - ~~If we clone multiple instruments using the same aliased samples, we should alias too~~
~~- Confirmation on exit if session data is different from source file~~
  ~~- Same when opening another file~~

# 0.08a
- ~~Resizable tracks (allow user to expand or shrink the track by click/dragging the far right end of the ruler)~~
- ~~Make piano in bank editor and instrument editor scrollable for full range~~
- ~~Start from nothing (currently the editor requires you to load a file)~~
- ~~Implement ZSB (Zefie Sound Bank), same deal as RMF/ZMF, used for newer codecs. Uses ZREZ.~~
- Cannot play 1:0 or 2:0 in MIDI Editor (also affects exports, but works on re-import)
- Hide bank editor behind warning that it is incomplete and broken
- Replacing a sample does not update the original sample node in the tree.
  - Creates a new duplicate sample entry, and only the currently edited instrument is updated to use it, leaving other instruments still pointing to the old sample.
- Fix pitch issues with bank compressor

# Future
- Interpolation configuration (none, linear, cubic, etc, current is just on/off)
- Neo Reverb for preview player
  - Custom .neoreverb support
  - means we need the reverb edit dialog from zefidi

# Harder stuff
- Sound Bank Editor
  - ~~need killswitch for bad ADSR~~
  - ~~need apply button~~
  - make sure we load the edited bank (not original) into memory when switching to midi tab
  - lots of bugs with sample editor
  - needs context menus still
  - musicial keyboard vs form fields

- Allow for automation like Volume to be able have a slide on it
  - for example, to easily make a fadein or fadeout
  - the edit dialog could have "start (item)" "end (item)"

- Externally imported MP3 samples (not encoded by us) may have a gap
  - How to address without breaking backwards compatiblity with the decoder?

- Allow configuration of SysEx and 'non-standard' CC commands
  - Where do we even put this stuff?  

- MIDI In (record to track)
  - Use RtMidi
  - Hook alsa/jack/winmm as we do with Zefidi
  - Allow MIDI in for instrument dialog preview
  - Need to find my MIDI keyboard

# Maybe (really hard stuff)
- SF2/DLS support
  - How? FluidSynth handles all of that.
  - Do we try to convert instrument data? But the ADSR is different in RMF/ZMF/HSB.
