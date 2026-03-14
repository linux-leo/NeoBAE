#pragma once

#include <stdint.h>

#include <functional>

#include <wx/string.h>

extern "C" {
#include "NeoBAE.h"
}

class wxWindow;

class PianoRollPanel;

PianoRollPanel *CreatePianoRollPanel(wxWindow *parent);
wxWindow *PianoRollPanel_AsWindow(PianoRollPanel *panel);
void PianoRollPanel_SetDocument(PianoRollPanel *panel, BAERmfEditorDocument *document);
void PianoRollPanel_SetSelectedTrack(PianoRollPanel *panel, int trackIndex);
void PianoRollPanel_SetSelectionChangedCallback(PianoRollPanel *panel, std::function<void()> callback);
void PianoRollPanel_SetUndoCallbacks(PianoRollPanel *panel,
                                     std::function<void(wxString const &)> beginCallback,
                                     std::function<void(wxString const &)> commitCallback,
                                     std::function<void()> cancelCallback);
void PianoRollPanel_RefreshFromDocument(PianoRollPanel *panel, bool clearSelection);
bool PianoRollPanel_GetSelectedNoteInfo(PianoRollPanel *panel, BAERmfEditorNoteInfo *outNoteInfo);
void PianoRollPanel_SetSelectedNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program);
void PianoRollPanel_SetNewNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program);
void PianoRollPanel_SetPlayheadTick(PianoRollPanel *panel, uint32_t tick);
void PianoRollPanel_EnsurePlayheadVisible(PianoRollPanel *panel, uint32_t tick);
void PianoRollPanel_JumpToTick(PianoRollPanel *panel, uint32_t tick);
void PianoRollPanel_ClearPlayhead(PianoRollPanel *panel);
void PianoRollPanel_Refresh(PianoRollPanel *panel);
