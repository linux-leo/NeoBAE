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

# Future
- Make it easier to get back to the channel dialog, as selecting the same menu option doesn't trigger it again
- Interpolation configuration (none, linear, cubic, etc, current is just on/off)
- Allow export of RMF/ZMF to MIDI with warning about the output file not having custom instruments/samples.
- `Crazy Dream fix.rmf` plays bass/gt sample instead of saw lead
- Allow for automation like Volume to be able have a slide on it so its easy to make a fadeout/fadein for example, the edit dialog could have "start (item)" "end (item)"
- Changing the program of an instrument may not reflect in preview/output
- Resizable tracks (allow user to expand or shrink the track by click/dragging the far right end of the ruler)
- Visible/editable MIDI Loop representation in piano roll
- Neo Reverb for preview player
- Larger instrument dialog piano not working 100% correctly
- MP3 externally imported samples (not encoded by us) may have a gap
- Start from nothing (currently the editor requires you to load a file)
- Allow configuration of SysEx and 'non-standard' CC commands
- MIDI In (record to track)