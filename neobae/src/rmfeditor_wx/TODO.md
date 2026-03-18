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
- Musical keyboard doesn't stop when key released
- Make it easier to get back to the channel dialog
  - remove code to 'select same menu option'
- Allow export of RMF/ZMF to MIDI with warning about the output file not having custom instruments/samples.
- Visible/editable MIDI Loop representation in piano roll
- Interpolation configuration (none, linear, cubic, etc, current is just on/off)

# Future
- `Crazy Dream fix.rmf` plays bass/gt sample instead of saw lead
- Allow for automation like Volume to be able have a slide on it so its easy to make a fadeout/fadein for example, the edit dialog could have "start (item)" "end (item)"
- Resizable tracks (allow user to expand or shrink the track by click/dragging the far right end of the ruler)
- Neo Reverb for preview player
  - Custom .neoreverb support
  - means we need the reverb edit dialog from zefidi
- Larger instrument dialog piano not working 100% correctly

# Harder stuff
- MP3 externally imported samples (not encoded by us) may have a gap
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