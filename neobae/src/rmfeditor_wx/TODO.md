# 0.05a
- ~~Fix SND export issue~~
- Sus out sample codec issues
  - ~~MP3 gap + loop points (MUST BE CAREFUL WHAT WE CHANGE)~~
  - ~~Opus to mp3 = wrong pitch~~
  - ~~Opus to mp3 = wrong pitch in instrument preview, fine in output~~
  - ~~MP3 loop is not accurate in instrument preview~~
  - ~~MP3 bitrate is not respected in instrument preview, fine in output~~
  - ~~Opus loop points~~
  - ~~Opus Dynamic frequency scaling~~
  - ~~neho's crazy compression tactic (Round‑Trip Resampling)~~
  - ~~Opus (std and RT) sped up~~
  - ~~Opus RT preview needs speed down~~
  - ~~ADPCM~~
  - ~~VORBIS~~
  - ~~FLAC~~
- ~~Switch tempfile system to memory loader~~

# 0.06a


# Future
- `Crazy Dream fix.rmf` plays bass/gt sample instead of saw lead
- Allow for automation like Volume to be able have a slide on it so its easy to make a fadeout/fadein for example, the edit dialog could have "start (item)" "end (item)"
- Changing the program of an instrument may not reflect in preview/output
- Resizable tracks (allow user to expand or shrink the track by click/dragging the far right end of the ruler)
- Visible/editable loop representation in piano roll
- Neo Reverb for preview player
- Larger instrument dialog piano not working 100% correctly
- MP3 externally imported samples (not encoded by us) may have a gap
- Start from nothing (currently the editor requires you to load a file)
- Allow configuration of SysEx and 'non-standard' CC commands
- Enhanced Exporting
  - Currently we list all tracks and allow exporting a single track.
  - We should enable channel exporting in the same manner.
  - It should allow exporting certain channels (multiple possible, eg ch9 and ch10 only).
  - It should also allow exporting of each selected channel to a seperate RMF files (eg `user_selected_name[chX].rmf`)
- MIDI In (record to track)