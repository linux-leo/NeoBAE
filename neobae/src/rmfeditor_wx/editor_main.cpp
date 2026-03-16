#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/button.h>
#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/choice.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/numdlg.h>
#include <wx/scrolwin.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/timer.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
}

#include "editor_instrument_ext_dialog.h"
#include "editor_metadata_dialog.h"
#include "editor_pianoroll_panel.h"

namespace {

constexpr char const *kVersionString = "0.03 alpha";

enum {
    ID_TrackAdd = wxID_HIGHEST + 200,
    ID_TrackDelete,
    ID_SampleAdd,
    ID_SampleNewInstrument,
    ID_InstrumentEdit,
    ID_SampleDelete,
    ID_LoadBank,
    ID_UnloadBanks,
};

struct UndoTempoEventState {
    uint32_t tick;
    uint32_t microsecondsPerQuarter;
};

struct UndoCCEventState {
    unsigned char controller;
    uint32_t tick;
    unsigned char value;
};

struct UndoTrackState {
    std::vector<BAERmfEditorNoteInfo> notes;
    std::vector<UndoCCEventState> ccEvents;
};

struct UndoDocumentState {
    uint32_t tempoBpm;
    std::vector<UndoTempoEventState> tempoEvents;
    std::vector<UndoTrackState> tracks;
};

struct UndoEntry {
    wxString label;
    UndoDocumentState before;
    UndoDocumentState after;
};

static bool NoteInfoEquals(BAERmfEditorNoteInfo const &left, BAERmfEditorNoteInfo const &right) {
    return left.startTick == right.startTick &&
           left.durationTicks == right.durationTicks &&
           left.note == right.note &&
           left.velocity == right.velocity &&
           left.bank == right.bank &&
           left.program == right.program;
}

static bool UndoSnapshotsEqual(UndoDocumentState const &left, UndoDocumentState const &right) {
    if (left.tempoBpm != right.tempoBpm || left.tempoEvents.size() != right.tempoEvents.size() || left.tracks.size() != right.tracks.size()) {
        return false;
    }
    for (size_t index = 0; index < left.tempoEvents.size(); ++index) {
        if (left.tempoEvents[index].tick != right.tempoEvents[index].tick ||
            left.tempoEvents[index].microsecondsPerQuarter != right.tempoEvents[index].microsecondsPerQuarter) {
            return false;
        }
    }
    for (size_t trackIndex = 0; trackIndex < left.tracks.size(); ++trackIndex) {
        UndoTrackState const &leftTrack = left.tracks[trackIndex];
        UndoTrackState const &rightTrack = right.tracks[trackIndex];

        if (leftTrack.notes.size() != rightTrack.notes.size()) {
            return false;
        }
        for (size_t noteIndex = 0; noteIndex < leftTrack.notes.size(); ++noteIndex) {
            if (!NoteInfoEquals(leftTrack.notes[noteIndex], rightTrack.notes[noteIndex])) {
                return false;
            }
        }
        if (leftTrack.ccEvents.size() != rightTrack.ccEvents.size()) {
            return false;
        }
        for (size_t eventIndex = 0; eventIndex < leftTrack.ccEvents.size(); ++eventIndex) {
            if (leftTrack.ccEvents[eventIndex].controller != rightTrack.ccEvents[eventIndex].controller ||
                leftTrack.ccEvents[eventIndex].tick != rightTrack.ccEvents[eventIndex].tick ||
                leftTrack.ccEvents[eventIndex].value != rightTrack.ccEvents[eventIndex].value) {
                return false;
            }
        }
    }
    return true;
}

class MainFrame final : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, wxString::Format("NeoBAE Studio v%s", kVersionString), wxDefaultPosition, wxSize(1320, 860)),
          m_document(nullptr),
                    m_updatingControls(false),
                    m_playbackMixer(nullptr),
                    m_playbackSong(nullptr),
                    m_previewSound(nullptr),
                    m_playbackTimer(this),
                    m_ignoreSeekEvent(false),
                    m_autoFollowPlayhead(true),
                    m_bankToken(nullptr),
                    m_bankLoaded(false),
                    m_hasPendingUndo(false),
                    m_restoringUndo(false) {
        wxMenu *fileMenu;
                wxMenu *editMenu;
        wxMenuBar *menuBar;
        wxSplitterWindow *splitter;
        wxPanel *sidebar;
        wxPanel *editorPanel;
        wxBoxSizer *sidebarSizer;
        wxBoxSizer *editorSizer;
        wxFlexGridSizer *controlsSizer;
        wxBoxSizer *transportSizer;
        wxBoxSizer *sampleButtonSizer;

        fileMenu = new wxMenu();
        fileMenu->Append(wxID_OPEN, "&Open\tCtrl+O");
        fileMenu->Append(wxID_SAVEAS, "Save As &RMF\tCtrl+Shift+S");
        fileMenu->AppendSeparator();
        fileMenu->Append(ID_LoadBank, "Load &Bank (HSB)...\tCtrl+B");
        fileMenu->Append(ID_UnloadBanks, "&Unload All Banks");
        fileMenu->AppendSeparator();
        fileMenu->Append(wxID_EXIT, "E&xit");
        editMenu = new wxMenu();
        editMenu->Append(wxID_UNDO, "&Undo\tCtrl+Z");
        editMenu->Append(wxID_REDO, "&Redo\tCtrl+Y");
        menuBar = new wxMenuBar();
        menuBar->Append(fileMenu, "&File");
        menuBar->Append(editMenu, "&Edit");
        SetMenuBar(menuBar);

        splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
        sidebar = new wxPanel(splitter);
        editorPanel = new wxPanel(splitter);
        sidebarSizer = new wxBoxSizer(wxVERTICAL);
        editorSizer = new wxBoxSizer(wxVERTICAL);

        m_trackList = new wxListBox(sidebar, wxID_ANY);
        m_sampleList = new wxListBox(sidebar, wxID_ANY);
        m_sampleAddButton = new wxButton(sidebar, ID_SampleNewInstrument, "Add Instrument");
        sidebarSizer->Add(new wxStaticText(sidebar, wxID_ANY, "Tracks"), 0, wxALL, 8);
        sidebarSizer->Add(m_trackList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebarSizer->Add(new wxStaticText(sidebar, wxID_ANY, "Embedded Instruments"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebarSizer->Add(m_sampleList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sampleButtonSizer = new wxBoxSizer(wxHORIZONTAL);
        sampleButtonSizer->Add(m_sampleAddButton, 1, 0, 0);
        sidebarSizer->Add(sampleButtonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebar->SetSizer(sidebarSizer);

        controlsSizer = new wxFlexGridSizer(2, 6, 8, 10);
        controlsSizer->AddGrowableCol(1, 1);
        controlsSizer->AddGrowableCol(3, 1);
        controlsSizer->AddGrowableCol(5, 1);

        m_tempoSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 20, 400, 120);
        m_bankSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        m_programSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        m_transposeSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, -48, 48, 0);
        m_panSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 64);
        m_volumeSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 100);
        m_playButton = new wxButton(editorPanel, wxID_ANY, "Play");
        m_pauseButton = new wxButton(editorPanel, wxID_ANY, "Pause/Resume");
        m_stopButton = new wxButton(editorPanel, wxID_ANY, "Stop");
        m_metadataButton = new wxButton(editorPanel, wxID_ANY, "Metadata");
        m_playScopeChoice = new wxChoice(editorPanel, wxID_ANY);
        m_previewVolumeSlider = new wxSlider(editorPanel, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(120, -1), wxSL_HORIZONTAL);
        m_playScopeChoice->Append("All Tracks");
        m_playScopeChoice->Append("Current Track");
        m_playScopeChoice->SetSelection(0);

        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Tempo"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_tempoSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Bank"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_bankSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_programSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Transpose"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_transposeSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Pan"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_panSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Volume"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_volumeSpin, 1, wxEXPAND);

        transportSizer = new wxBoxSizer(wxHORIZONTAL);
        transportSizer->Add(m_playButton, 0, wxRIGHT, 8);
        transportSizer->Add(m_pauseButton, 0, wxRIGHT, 8);
        transportSizer->Add(m_stopButton, 0, wxRIGHT, 8);
        transportSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Playback"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        transportSizer->Add(m_playScopeChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        transportSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Preview Vol"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        transportSizer->Add(m_previewVolumeSlider, 0, wxALIGN_CENTER_VERTICAL, 0);
        transportSizer->AddStretchSpacer(1);
        transportSizer->Add(m_metadataButton, 0, wxALIGN_CENTER_VERTICAL);

        m_positionSlider = new wxSlider(editorPanel, wxID_ANY, 0, 0, 1000, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_positionLabel = new wxStaticText(editorPanel, wxID_ANY, "0:00.0 / 0:00.0");

        m_pianoRoll = CreatePianoRollPanel(editorPanel);
        editorSizer->Add(controlsSizer, 0, wxEXPAND | wxALL, 10);
        editorSizer->Add(transportSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        {
            wxBoxSizer *positionSizer;

            positionSizer = new wxBoxSizer(wxHORIZONTAL);
            positionSizer->Add(m_positionSlider, 1, wxEXPAND | wxRIGHT, 10);
            positionSizer->Add(m_positionLabel, 0, wxALIGN_CENTER_VERTICAL);
            editorSizer->Add(positionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        }
        editorSizer->Add(PianoRollPanel_AsWindow(m_pianoRoll), 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        editorPanel->SetSizer(editorSizer);

        PianoRollPanel_SetSelectionChangedCallback(m_pianoRoll, [this]() { UpdateControlsFromSelection(); });
        PianoRollPanel_SetSeekRequestedCallback(m_pianoRoll, [this](uint32_t tick) { SeekToTickFromPianoRoll(tick); });
        PianoRollPanel_SetUndoCallbacks(m_pianoRoll,
                        [this](wxString const &label) { BeginUndoAction(label); },
                        [this](wxString const &label) { CommitUndoAction(label); },
                        [this]() { CancelUndoAction(); });

        splitter->SplitVertically(sidebar, editorPanel, 240);
        splitter->SetMinimumPaneSize(180);

        CreateStatusBar(2);
        SetStatusText("Open an RMF or MIDI file to begin.");

        Bind(wxEVT_MENU, &MainFrame::OnOpen, this, wxID_OPEN);
        Bind(wxEVT_MENU, &MainFrame::OnSaveAs, this, wxID_SAVEAS);
        Bind(wxEVT_MENU, &MainFrame::OnUndo, this, wxID_UNDO);
        Bind(wxEVT_MENU, &MainFrame::OnRedo, this, wxID_REDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_EXIT);
        m_trackList->Bind(wxEVT_LISTBOX, &MainFrame::OnTrackSelected, this);
        m_trackList->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnTrackContextMenu, this);
        m_sampleList->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnSampleContextMenu, this);
        m_tempoSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTempoChanged, this);
        m_bankSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_programSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_transposeSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_panSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_volumeSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_bankSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_programSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_transposeSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_panSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_volumeSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_playButton->Bind(wxEVT_BUTTON, &MainFrame::OnPlay, this);
        m_pauseButton->Bind(wxEVT_BUTTON, &MainFrame::OnPauseResume, this);
        m_stopButton->Bind(wxEVT_BUTTON, &MainFrame::OnStop, this);
        m_previewVolumeSlider->Bind(wxEVT_SLIDER, &MainFrame::OnPreviewVolumeChanged, this);
        m_positionSlider->Bind(wxEVT_SLIDER, &MainFrame::OnSeekSlider, this);
        m_metadataButton->Bind(wxEVT_BUTTON, &MainFrame::OnMetadata, this);
        Bind(wxEVT_MENU, &MainFrame::OnTrackAdd, this, ID_TrackAdd);
        Bind(wxEVT_MENU, &MainFrame::OnTrackDelete, this, ID_TrackDelete);
        Bind(wxEVT_MENU, &MainFrame::OnSampleAdd, this, ID_SampleAdd);
        Bind(wxEVT_BUTTON, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnInstrumentEdit, this, ID_InstrumentEdit);
        Bind(wxEVT_MENU, &MainFrame::OnSampleDelete, this, ID_SampleDelete);
        m_sampleList->Bind(wxEVT_LISTBOX_DCLICK, &MainFrame::OnInstrumentEditDblClick, this);
        Bind(wxEVT_MENU, &MainFrame::OnLoadBank, this, ID_LoadBank);
        Bind(wxEVT_MENU, &MainFrame::OnUnloadBanks, this, ID_UnloadBanks);
        Bind(wxEVT_TIMER, &MainFrame::OnPlaybackTimer, this);
        LoadIniSettings();
        UpdateUndoMenuState();

    }

    ~MainFrame() override {
        SaveIniSettings();
        if (m_document) {
            BAERmfEditorDocument_Delete(m_document);
            m_document = nullptr;
        }
        StopPlayback(true);
        if (m_previewSound)
        {
            BAESound_Stop(m_previewSound, FALSE);
            BAESound_Delete(m_previewSound);
            m_previewSound = nullptr;
        }
        ShutdownPlaybackEngine();
    }

private:
    BAERmfEditorDocument *m_document;
    bool m_updatingControls;
    wxString m_currentPath;
    wxListBox *m_trackList;
    PianoRollPanel *m_pianoRoll;
    wxSpinCtrl *m_tempoSpin;
    wxSpinCtrl *m_bankSpin;
    wxSpinCtrl *m_programSpin;
    wxSpinCtrl *m_transposeSpin;
    wxSpinCtrl *m_panSpin;
    wxSpinCtrl *m_volumeSpin;
    wxButton *m_playButton;
    wxButton *m_pauseButton;
    wxButton *m_stopButton;
    wxButton *m_metadataButton;
    wxChoice *m_playScopeChoice;
    wxSlider *m_previewVolumeSlider;
    wxSlider *m_positionSlider;
    wxStaticText *m_positionLabel;
    wxListBox *m_sampleList;
    std::vector<uint32_t> m_sampleListMap; /* list row -> first sample index for that program */
    wxButton *m_sampleAddButton;
    BAEMixer m_playbackMixer;
    BAESong m_playbackSong;
    std::vector<unsigned char> m_playbackSongBlob;
    BAESound m_previewSound;
    wxString m_playbackTempPath;
    wxString m_previewSampleTempPath;
    wxTimer m_playbackTimer;
    bool m_ignoreSeekEvent;
    bool m_autoFollowPlayhead;
    BAEBankToken m_bankToken;
    bool m_bankLoaded;
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;
    UndoDocumentState m_pendingUndoState;
    wxString m_pendingUndoLabel;
    bool m_hasPendingUndo;
    bool m_restoringUndo;

    static wxString GetIniPath() {
        return wxFileName::GetHomeDir() + "/.nbstudio.ini";
    }

    void LoadIniSettings() {
        wxFileConfig config(wxEmptyString,
                            wxEmptyString,
                            GetIniPath(),
                            wxEmptyString,
                            wxCONFIG_USE_LOCAL_FILE);
        long previewVolume;

        previewVolume = 100;
        if (config.Read("audio/preview_volume", &previewVolume) && m_previewVolumeSlider) {
            m_previewVolumeSlider->SetValue(std::clamp<long>(previewVolume, 0, 100));
        }
    }

    void SaveIniSettings() {
        wxFileConfig config(wxEmptyString,
                            wxEmptyString,
                            GetIniPath(),
                            wxEmptyString,
                            wxCONFIG_USE_LOCAL_FILE);

        config.Write("audio/preview_volume", static_cast<long>(m_previewVolumeSlider ? m_previewVolumeSlider->GetValue() : 100));
        config.Flush();
    }

    BAE_UNSIGNED_FIXED GetPreviewVolumeFixed() const {
        int percent = 100;
        if (m_previewVolumeSlider) {
            percent = m_previewVolumeSlider->GetValue();
        }
        percent = std::clamp(percent, 0, 100);
        return static_cast<BAE_UNSIGNED_FIXED>((static_cast<double>(percent) / 100.0) * 65536.0);
    }

    double GetPreviewVolumeScale() const {
        int percent = 100;
        if (m_previewVolumeSlider) {
            percent = m_previewVolumeSlider->GetValue();
        }
        return static_cast<double>(std::clamp(percent, 0, 100)) / 100.0;
    }

    void ClearUndoHistory() {
        m_undoStack.clear();
        m_redoStack.clear();
        m_pendingUndoState = UndoDocumentState();
        m_pendingUndoLabel.clear();
        m_hasPendingUndo = false;
        UpdateUndoMenuState();
    }

    void UpdateUndoMenuState() {
        wxMenuBar *menuBar;

        menuBar = GetMenuBar();
        if (menuBar) {
            menuBar->Enable(wxID_UNDO, !m_undoStack.empty() && m_document != nullptr);
            menuBar->Enable(wxID_REDO, !m_redoStack.empty() && m_document != nullptr);
        }
    }

    bool CaptureUndoState(UndoDocumentState *outState) const {
        uint16_t trackCount;
        uint32_t tempoCount;

        if (!outState || !m_document) {
            return false;
        }
        outState->tempoBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &outState->tempoBpm);
        outState->tempoEvents.clear();
        outState->tracks.clear();

        tempoCount = 0;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) == BAE_NO_ERROR) {
            outState->tempoEvents.reserve(tempoCount);
            for (uint32_t eventIndex = 0; eventIndex < tempoCount; ++eventIndex) {
                UndoTempoEventState eventState;

                if (BAERmfEditorDocument_GetTempoEvent(m_document,
                                                       eventIndex,
                                                       &eventState.tick,
                                                       &eventState.microsecondsPerQuarter) == BAE_NO_ERROR) {
                    outState->tempoEvents.push_back(eventState);
                }
            }
        }

        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) != BAE_NO_ERROR) {
            return false;
        }
        outState->tracks.resize(trackCount);
        for (uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            UndoTrackState &trackState = outState->tracks[trackIndex];
            uint32_t noteCount;

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(m_document, trackIndex, &noteCount) == BAE_NO_ERROR) {
                trackState.notes.reserve(noteCount);
                for (uint32_t noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                    BAERmfEditorNoteInfo noteInfo;

                    if (BAERmfEditorDocument_GetNoteInfo(m_document, trackIndex, noteIndex, &noteInfo) == BAE_NO_ERROR) {
                        trackState.notes.push_back(noteInfo);
                    }
                }
            }
            for (int controllerValue = 0; controllerValue < 128; ++controllerValue) {
                uint32_t eventCount;
                unsigned char controller;

                controller = static_cast<unsigned char>(controllerValue);
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, controller, &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                trackState.ccEvents.reserve(trackState.ccEvents.size() + eventCount);
                for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    UndoCCEventState eventState;

                    if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                             trackIndex,
                                                             controller,
                                                             eventIndex,
                                                             &eventState.tick,
                                                             &eventState.value) == BAE_NO_ERROR) {
                        eventState.controller = controller;
                        trackState.ccEvents.push_back(eventState);
                    }
                }
            }
        }
        return true;
    }

    bool RestoreUndoState(UndoDocumentState const &state) {
        uint16_t trackCount;

        if (!m_document) {
            return false;
        }
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) != BAE_NO_ERROR || trackCount != state.tracks.size()) {
            return false;
        }
        if (BAERmfEditorDocument_SetTempoBPM(m_document, state.tempoBpm) != BAE_NO_ERROR) {
            return false;
        }
        {
            uint32_t tempoCount;

            tempoCount = 0;
            BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount);
            for (uint32_t eventIndex = tempoCount; eventIndex > 0; --eventIndex) {
                BAERmfEditorDocument_DeleteTempoEvent(m_document, eventIndex - 1);
            }
            for (UndoTempoEventState const &eventState : state.tempoEvents) {
                if (BAERmfEditorDocument_AddTempoEvent(m_document,
                                                       eventState.tick,
                                                       eventState.microsecondsPerQuarter) != BAE_NO_ERROR) {
                    return false;
                }
            }
        }
        for (uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            UndoTrackState const &trackState = state.tracks[trackIndex];
            uint32_t noteCount;

            noteCount = 0;
            BAERmfEditorDocument_GetNoteCount(m_document, trackIndex, &noteCount);
            for (uint32_t noteIndex = noteCount; noteIndex > 0; --noteIndex) {
                BAERmfEditorDocument_DeleteNote(m_document, trackIndex, noteIndex - 1);
            }
            for (BAERmfEditorNoteInfo const &noteInfo : trackState.notes) {
                if (BAERmfEditorDocument_AddNote(m_document,
                                                 trackIndex,
                                                 noteInfo.startTick,
                                                 noteInfo.durationTicks,
                                                 noteInfo.note,
                                                 noteInfo.velocity) != BAE_NO_ERROR) {
                    return false;
                }
            }
            for (uint32_t noteIndex = 0; noteIndex < trackState.notes.size(); ++noteIndex) {
                if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                                     trackIndex,
                                                     noteIndex,
                                                     &trackState.notes[noteIndex]) != BAE_NO_ERROR) {
                    return false;
                }
            }
            for (int controllerValue = 0; controllerValue < 128; ++controllerValue) {
                uint32_t eventCount;
                unsigned char controller;

                controller = static_cast<unsigned char>(controllerValue);
                eventCount = 0;
                BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, controller, &eventCount);
                for (uint32_t eventIndex = eventCount; eventIndex > 0; --eventIndex) {
                    BAERmfEditorDocument_DeleteTrackCCEvent(m_document, trackIndex, controller, eventIndex - 1);
                }
            }
            for (UndoCCEventState const &eventState : trackState.ccEvents) {
                if (eventState.controller > 127) {
                    continue;
                }
                    if (BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                             trackIndex,
                                                             eventState.controller,
                                                             eventState.tick,
                                                             eventState.value) != BAE_NO_ERROR) {
                        return false;
                    }
                }
        }
        return true;
    }

    void BeginUndoAction(wxString const &label) {
        if (!m_document || m_restoringUndo) {
            return;
        }
        if (!CaptureUndoState(&m_pendingUndoState)) {
            return;
        }
        m_pendingUndoLabel = label;
        m_hasPendingUndo = true;
    }

    void CommitUndoAction(wxString const &label) {
        UndoDocumentState afterState;
        UndoEntry entry;

        if (!m_document || !m_hasPendingUndo || m_restoringUndo) {
            return;
        }
        if (!CaptureUndoState(&afterState)) {
            CancelUndoAction();
            return;
        }
        if (UndoSnapshotsEqual(m_pendingUndoState, afterState)) {
            CancelUndoAction();
            return;
        }
        entry.label = label.empty() ? m_pendingUndoLabel : label;
        entry.before = m_pendingUndoState;
        entry.after = afterState;
        m_undoStack.push_back(entry);
        m_redoStack.clear();
        if (m_undoStack.size() > 128) {
            m_undoStack.erase(m_undoStack.begin());
        }
        m_pendingUndoState = UndoDocumentState();
        m_pendingUndoLabel.clear();
        m_hasPendingUndo = false;
        UpdateUndoMenuState();
    }

    void CancelUndoAction() {
        m_pendingUndoState = UndoDocumentState();
        m_pendingUndoLabel.clear();
        m_hasPendingUndo = false;
    }

    void OnUndo(wxCommandEvent &) {
        UndoEntry entry;

        if (!m_document || m_undoStack.empty()) {
            return;
        }
        entry = m_undoStack.back();
        m_undoStack.pop_back();
        m_restoringUndo = true;
        if (!RestoreUndoState(entry.before)) {
            m_restoringUndo = false;
            m_undoStack.push_back(entry);
            wxMessageBox("Failed to restore the previous note/event state.", "Undo Failed", wxOK | wxICON_ERROR, this);
            UpdateUndoMenuState();
            return;
        }
        m_restoringUndo = false;
        m_redoStack.push_back(entry);
        if (m_redoStack.size() > 128) {
            m_redoStack.erase(m_redoStack.begin());
        }
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, true);
        UpdateControlsFromSelection();
        SetStatusText(entry.label.empty() ? "Undo" : wxString::Format("Undid %s", entry.label), 0);
        UpdateUndoMenuState();
    }

    void OnRedo(wxCommandEvent &) {
        UndoEntry entry;

        if (!m_document || m_redoStack.empty()) {
            return;
        }
        entry = m_redoStack.back();
        m_redoStack.pop_back();
        m_restoringUndo = true;
        if (!RestoreUndoState(entry.after)) {
            m_restoringUndo = false;
            m_redoStack.push_back(entry);
            wxMessageBox("Failed to restore the redone note/event state.", "Redo Failed", wxOK | wxICON_ERROR, this);
            UpdateUndoMenuState();
            return;
        }
        m_restoringUndo = false;
        m_undoStack.push_back(entry);
        if (m_undoStack.size() > 128) {
            m_undoStack.erase(m_undoStack.begin());
        }
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, true);
        UpdateControlsFromSelection();
        SetStatusText(entry.label.empty() ? "Redo" : wxString::Format("Redid %s", entry.label), 0);
        UpdateUndoMenuState();
    }

    bool EnsurePlaybackEngine() {
        if (m_playbackMixer) {
            return true;
        }
        fprintf(stderr, "[nbstudio] Creating mixer...\n");
        m_playbackMixer = BAEMixer_New();
        if (!m_playbackMixer) {
            fprintf(stderr, "[nbstudio] BAEMixer_New() failed\n");
            return false;
        }
        BAEResult openResult = BAEMixer_Open(m_playbackMixer,
                                             BAE_RATE_44K,
                                             BAE_LINEAR_INTERPOLATION,
                                             BAE_USE_16 | BAE_USE_STEREO,
                                             64,
                                             16,
                                             32,
                                             TRUE);
        fprintf(stderr, "[nbstudio] BAEMixer_Open result=%d\n", static_cast<int>(openResult));
        if (openResult != BAE_NO_ERROR) {
            BAEMixer_Delete(m_playbackMixer);
            m_playbackMixer = nullptr;
            return false;
        }
        {
            BAERate actualRate;
            if (BAEMixer_GetRate(m_playbackMixer, &actualRate) == BAE_NO_ERROR) {
                fprintf(stderr, "[nbstudio] Mixer active rate=%ld\n", static_cast<long>(actualRate));
            }
        }
        if (!m_bankLoaded) {
#ifdef _BUILT_IN_PATCHES
            fprintf(stderr, "[nbstudio] Loading built-in bank...\n");
            BAEResult bankResult = BAEMixer_LoadBuiltinBank(m_playbackMixer, &m_bankToken);
            fprintf(stderr, "[nbstudio] BAEMixer_LoadBuiltinBank result=%d\n", static_cast<int>(bankResult));
            if (bankResult == BAE_NO_ERROR) {
                m_bankLoaded = true;
            }
#else
            fprintf(stderr, "[nbstudio] WARNING: _BUILT_IN_PATCHES not defined, no bank loaded!\n");
#endif
        }
        return true;
    }

    bool LoadBankFromFile(wxString const &path) {
        BAEBankToken newToken;
        wxScopedCharBuffer utf8;

        if (!EnsurePlaybackEngine()) {
            return false;
        }
        utf8 = path.utf8_str();
        if (BAEMixer_AddBankFromFile(m_playbackMixer, const_cast<char *>(utf8.data()), &newToken) != BAE_NO_ERROR) {
            return false;
        }
        m_bankToken = newToken;
        m_bankLoaded = true;
        char friendlyBuf[128];
        wxString bankName;
        if (BAE_GetBankFriendlyName(m_playbackMixer, newToken, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
            bankName = wxString::Format("Bank loaded: %s", friendlyBuf);
        } else {
            bankName = wxString::Format("Bank loaded: %s", wxFileNameFromPath(path));
        }
        SetStatusText(bankName, 0);
        return true;
    }

    void PopulateSampleList() {
        uint32_t sampleCount;
        uint32_t sampleIndex;

        m_sampleList->Clear();
        m_sampleListMap.clear();
        if (!m_document) {
            return;
        }
        sampleCount = 0;
        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        /* One list row per unique program number, showing the first sample's name. */
        std::vector<bool> seen(128, false);
        for (sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
            BAERmfEditorSampleInfo sampleInfo;
            uint32_t instID;
            BAERmfEditorInstrumentExtInfo instInfo;
            wxString instrumentName;

            if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (seen[sampleInfo.program]) {
                continue;
            }
            seen[sampleInfo.program] = true;
            /* Count how many samples share this program for the split hint. */
            uint32_t splitCount = 0;
            for (uint32_t j = 0; j < sampleCount; ++j) {
                BAERmfEditorSampleInfo other;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, j, &other) == BAE_NO_ERROR &&
                    other.program == sampleInfo.program) {
                    ++splitCount;
                }
            }

            instrumentName = sampleInfo.displayName ? sampleInfo.displayName : "(unnamed)";
            if (BAERmfEditorDocument_GetInstIDForSample(m_document, sampleIndex, &instID) == BAE_NO_ERROR &&
                BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &instInfo) == BAE_NO_ERROR &&
                instInfo.displayName && instInfo.displayName[0]) {
                instrumentName = instInfo.displayName;
            }

            wxString label = wxString::Format("P%u: %s",
                                              static_cast<unsigned>(sampleInfo.program),
                                              instrumentName);
            if (splitCount > 1) {
                label += wxString::Format(" (%u splits)", splitCount);
            }
            m_sampleList->Append(label);
            m_sampleListMap.push_back(sampleIndex);
        }
    }

    bool PreviewSampleAtKey(uint32_t sampleIndex,
                            int midiKey,
                            BAESampleInfo const *overrideInfo = nullptr,
                            int16_t splitVolumeOverride = 0,
                            unsigned char rootKeyOverride = 0xFF) {
        BAERmfEditorSampleInfo sampleInfo;
        BAEResult loadResult;
        BAEResult startResult;
        BAEResult rateResult;
        BAEResult stopResult;
        BAE_UNSIGNED_FIXED baseSampleRate;

        if (!m_document) {
            return false;
        }
        if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
            return false;
        }
        baseSampleRate = sampleInfo.sampleInfo.sampledRate;
        if (baseSampleRate < (4000U << 16)) {
            baseSampleRate = (44100U << 16);
        }
        if (!EnsurePlaybackEngine()) {
            return false;
        }
        if (m_previewSound) {
            BAESound_Stop(m_previewSound, FALSE);
            BAESound_Delete(m_previewSound);
            m_previewSound = nullptr;
        }
        m_previewSound = BAESound_New(m_playbackMixer);
        if (!m_previewSound) {
            return false;
        }

        loadResult = BAE_GENERAL_ERR;

        /* BAESound_LoadCustomSample expects signed 8-bit PCM input; it internally
           applies XPhase (subtract 128) to convert to the engine's unsigned format.
           However, waveform->theWaveform already holds unsigned 8-bit data (engine
           internal format, 0x80 = silence) from the RMF SND resource.  Passing it
           directly causes a double XPhase that shifts every sample by 128, producing
           severe DC-offset distortion ("hot/squelchy" audio).
           Pre-apply the same subtraction so the two applications cancel out. */
        auto loadCustomSampleFromEngineData = [&](void const *data, uint32_t frames,
                                                  uint16_t bits, uint16_t chans,
                                                  BAE_UNSIGNED_FIXED rate,
                                                  uint32_t loopS, uint32_t loopE) -> BAEResult {
            if (bits == 8) {
                /* Modify in-place: XPhase (subtract 128) converts unsigned engine format
                   to the signed format LoadCustomSample expects.  LoadCustomSample applies
                   XPhase again internally, and two subtractions of 128 mod 256 = identity,
                   so the document waveform is restored after the call.  No heap allocation
                   needed.  Safe because BAESound_Stop above released the audio thread's
                   access to this buffer. */
                uint8_t *data8 = const_cast<uint8_t *>(static_cast<uint8_t const *>(data));
                uint32_t dataSize = frames * static_cast<uint32_t>(chans);
                for (uint32_t i = 0; i < dataSize; ++i) data8[i] -= 128u;
                BAEResult r = BAESound_LoadCustomSample(m_previewSound, data8, frames, bits, chans, rate, loopS, loopE);
                for (uint32_t i = 0; i < dataSize; ++i) data8[i] -= 128u;  /* restore */
                return r;
            }
            return BAESound_LoadCustomSample(m_previewSound, const_cast<void *>(data), frames, bits, chans, rate, loopS, loopE);
        };

        /* When the caller provides live loop overrides, always use raw PCM so
           loop settings from the UI are honoured instead of stale file data. */
        if (overrideInfo) {
            void const *waveData;
            uint32_t frameCount;
            uint16_t bitSize;
            uint16_t channels;
            BAE_UNSIGNED_FIXED sampleRate;
            uint32_t loopStart;
            uint32_t loopEnd;

            waveData = nullptr;
            frameCount = 0;
            bitSize = 16;
            channels = 1;
            sampleRate = 0;
            if (BAERmfEditorDocument_GetSampleWaveformData(m_document,
                                                           sampleIndex,
                                                           &waveData,
                                                           &frameCount,
                                                           &bitSize,
                                                           &channels,
                                                           &sampleRate) == BAE_NO_ERROR &&
                frameCount > 0 &&
                (bitSize == 8 || bitSize == 16) &&
                (channels == 1 || channels == 2)) {
                if (sampleRate < (8000U << 16)) {
                    if (sampleRate >= 4000U && sampleRate <= 384000U) {
                        sampleRate <<= 16;
                    } else {
                        sampleRate = (44100U << 16);
                    }
                }
                loopStart = overrideInfo->startLoop;
                loopEnd   = overrideInfo->endLoop;
                if (loopStart >= frameCount || loopEnd > frameCount || loopEnd <= loopStart) {
                    loopStart = 0;
                    loopEnd   = 0;
                }
                loadResult = loadCustomSampleFromEngineData(waveData, frameCount,
                                                           bitSize, channels,
                                                           sampleRate,
                                                           loopStart, loopEnd);
                if (loadResult != BAE_NO_ERROR) {
                    loadResult = BAE_GENERAL_ERR; /* fall through to file paths */
                }
            }
        }

        if (loadResult == BAE_NO_ERROR) {
            goto preview_loaded;
        }
        loadResult = BAE_GENERAL_ERR;

        if (sampleInfo.sourcePath && sampleInfo.sourcePath[0]) {
            BAEFileType sourceType;

            sourceType = X_DetermineFileTypeByPath(sampleInfo.sourcePath);
            if (sourceType == BAE_WAVE_TYPE || sourceType == BAE_AIFF_TYPE || sourceType == BAE_AU_TYPE) {
                loadResult = BAESound_LoadFileSample(m_previewSound,
                                                     const_cast<char *>(sampleInfo.sourcePath),
                                                     sourceType);
            } else {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u source='%s' unsupported type=%d\n",
                        static_cast<unsigned>(sampleIndex),
                        sampleInfo.sourcePath,
                        static_cast<int>(sourceType));
            }
        }

        if (EnsurePreviewSampleTempPath()) {
            wxScopedCharBuffer utf8Path;
            BAEFileType exportedType;

            utf8Path = m_previewSampleTempPath.utf8_str();
            if (loadResult != BAE_NO_ERROR && BAERmfEditorDocument_ExportSampleToFile(m_document,
                                                        sampleIndex,
                                                        const_cast<char *>(utf8Path.data())) == BAE_NO_ERROR) {
                exportedType = X_DetermineFileTypeByPath(utf8Path.data());
                if (exportedType == BAE_INVALID_TYPE) {
                    exportedType = X_DetermineFileType(utf8Path.data());
                }
                if (exportedType == BAE_INVALID_TYPE) {
                    exportedType = BAE_WAVE_TYPE;
                }
                loadResult = BAESound_LoadFileSample(m_previewSound,
                                                     const_cast<char *>(utf8Path.data()),
                                                     exportedType);
            }
        }

        if (loadResult != BAE_NO_ERROR) {
            void const *waveData;
            uint32_t frameCount;
            uint16_t bitSize;
            uint16_t channels;
            BAE_UNSIGNED_FIXED sampleRate;
            uint32_t loopStart;
            uint32_t loopEnd;

            waveData = nullptr;
            frameCount = 0;
            bitSize = 16;
            channels = 1;
            sampleRate = 0;
            if (BAERmfEditorDocument_GetSampleWaveformData(m_document,
                                                           sampleIndex,
                                                           &waveData,
                                                           &frameCount,
                                                           &bitSize,
                                                           &channels,
                                                           &sampleRate) != BAE_NO_ERROR) {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u GetSampleWaveformData failed\n",
                        static_cast<unsigned>(sampleIndex));
                return false;
            }
            fprintf(stderr,
                    "[nbstudio] Preview sample %u raw format: frames=%u bits=%u channels=%u rate=0x%08lx loop=%u-%u root=%u basePitch=%u\n",
                    static_cast<unsigned>(sampleIndex),
                    static_cast<unsigned>(frameCount),
                    static_cast<unsigned>(bitSize),
                    static_cast<unsigned>(channels),
                    static_cast<unsigned long>(sampleRate),
                    static_cast<unsigned>(sampleInfo.sampleInfo.startLoop),
                    static_cast<unsigned>(sampleInfo.sampleInfo.endLoop),
                    static_cast<unsigned>(sampleInfo.rootKey),
                    static_cast<unsigned>(sampleInfo.sampleInfo.baseMidiPitch));
            if (sampleRate < (8000U << 16)) {
                if (sampleRate >= 4000U && sampleRate <= 384000U) {
                    sampleRate <<= 16;
                } else {
                    sampleRate = (44100U << 16);
                }
            }
            if (frameCount == 0 || (bitSize != 8 && bitSize != 16) || (channels != 1 && channels != 2)) {
                return false;
            }
            loopStart = sampleInfo.sampleInfo.startLoop;
            loopEnd = sampleInfo.sampleInfo.endLoop;
            if (overrideInfo) {
                loopStart = overrideInfo->startLoop;
                loopEnd   = overrideInfo->endLoop;
            }
            if (loopStart >= frameCount || loopEnd > frameCount || loopEnd <= loopStart) {
                loopStart = 0;
                loopEnd = 0;
            }
            loadResult = loadCustomSampleFromEngineData(waveData, frameCount,
                                                        bitSize, channels,
                                                        sampleRate,
                                                        loopStart, loopEnd);
            if (loadResult != BAE_NO_ERROR) {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u LoadCustomSample failed result=%d\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<int>(loadResult));
                return false;
            }
            fprintf(stderr,
                    "[nbstudio] Preview sample %u LoadCustomSample ok\n",
                    static_cast<unsigned>(sampleIndex));
        }

        preview_loaded:
        {
            BAESampleInfo loadedInfo;
            if (BAESound_GetInfo(m_previewSound, &loadedInfo) == BAE_NO_ERROR) {
                if (loadedInfo.sampledRate >= (4000U << 16)) {
                    baseSampleRate = loadedInfo.sampledRate;
                }
                fprintf(stderr,
                        "[nbstudio] Preview sample %u loaded info: frames=%u bits=%u channels=%u rate=%lu basePitch=%u\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<unsigned>(loadedInfo.waveFrames),
                        static_cast<unsigned>(loadedInfo.bitSize),
                        static_cast<unsigned>(loadedInfo.channels),
                        static_cast<unsigned long>(loadedInfo.sampledRate),
                        static_cast<unsigned>(loadedInfo.baseMidiPitch));
            }
        }

        BAESound_SetLoopCount(m_previewSound, 0);

        int previewRoot = static_cast<int>(sampleInfo.rootKey);
        if (rootKeyOverride <= 127) {
            previewRoot = static_cast<int>(rootKeyOverride);
        }
        if (previewRoot < 0 || previewRoot > 127) {
            previewRoot = static_cast<int>((overrideInfo ? overrideInfo->baseMidiPitch : sampleInfo.sampleInfo.baseMidiPitch) & 0x7F);
        }
        int semitones = midiKey - previewRoot;
        double ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);
        /* 16.16 fixed-point max representable Hz is 65535.999... */
        constexpr double kMinRateFixed = static_cast<double>(4000U << 16);
        constexpr double kMaxRateFixed = static_cast<double>(0xFFFF0000u);
        BAE_UNSIGNED_FIXED rate = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(static_cast<double>(baseSampleRate) * ratio,
                             kMinRateFixed,
                             kMaxRateFixed));
        int splitVolume = sampleInfo.splitVolume;
        if (overrideInfo) {
            splitVolume = splitVolumeOverride;
        }
        if (splitVolume <= 0) {
            splitVolume = 100;
        }
        double volumeScale = static_cast<double>(splitVolume) / 100.0;
        volumeScale *= GetPreviewVolumeScale();
        BAE_UNSIGNED_FIXED volume = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(0.7 * volumeScale,
                                             0.0,
                                             4.0) * 65536.0);

        rateResult = BAESound_SetRate(m_previewSound, rate);
        if (rateResult != BAE_NO_ERROR) {
            fprintf(stderr,
                    "[nbstudio] Preview sample %u SetRate failed result=%d rate=%lu\n",
                    static_cast<unsigned>(sampleIndex),
                    static_cast<int>(rateResult),
                    static_cast<unsigned long>(rate));
            return false;
        }
        startResult = BAESound_Start(m_previewSound, 0, volume, 0);
        if (startResult != BAE_NO_ERROR) {
            return false;
        }
        return true;
    }

    void StopPreviewSample() {
        if (m_previewSound) {
            BAESound_Stop(m_previewSound, FALSE);
            BAESound_Delete(m_previewSound);
            m_previewSound = nullptr;
        }
    }

    bool ExportSampleToPath(uint32_t sampleIndex, wxString const &path) {
        wxScopedCharBuffer utf8Path;

        if (!m_document) {
            return false;
        }
        utf8Path = path.utf8_str();
        return BAERmfEditorDocument_ExportSampleToFile(m_document,
                                                       sampleIndex,
                                                       const_cast<char *>(utf8Path.data())) == BAE_NO_ERROR;
    }

    bool ReplaceSampleFromPath(uint32_t sampleIndex, wxString const &path) {
        BAEResult result;
        BAESampleInfo sampleInfo;
        wxScopedCharBuffer utf8Path;

        if (!m_document) {
            return false;
        }
        utf8Path = path.utf8_str();
        result = BAERmfEditorDocument_ReplaceSampleFromFile(m_document,
                                                            sampleIndex,
                                                            const_cast<char *>(utf8Path.data()),
                                                            &sampleInfo);
        if (result == BAE_BAD_FILE_TYPE) {
            wxMessageBox("Unsupported sample codec. Supported imports are PCM WAV/AIFF, MP3, Ogg Vorbis, FLAC, and Opus.",
                         "Embedded Instruments",
                         wxOK | wxICON_ERROR,
                         this);
            return false;
        }
        if (result != BAE_NO_ERROR) {
            wxMessageBox("Failed to replace sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return false;
        }
        return true;
    }

    static uint16_t DisplayBankFromInternal(uint16_t internalBank) {
        if (internalBank >= 128) {
            return static_cast<uint16_t>((internalBank >> 7) & 0x7F);
        }
        return internalBank;
    }

    static uint16_t InternalBankFromDisplay(uint16_t displayBank) {
        return static_cast<uint16_t>((displayBank & 0x7F) << 7);
    }

    wxString BuildTrackLabel(int trackZeroBased, BAERmfEditorTrackInfo const &trackInfo) const {
        if (trackInfo.name && trackInfo.name[0]) {
            return wxString::Format("%u: %s", static_cast<unsigned>(trackZeroBased + 1), trackInfo.name);
        }
        return wxString::Format("%u: Ch %u Prog %u", static_cast<unsigned>(trackZeroBased + 1), static_cast<unsigned>(trackInfo.channel + 1), static_cast<unsigned>(trackInfo.program));
    }

    BAERmfEditorDocument *BuildSingleTrackPlaybackDocument(int trackIndex) {
        BAERmfEditorDocument *playDoc;
        uint16_t trackCount;
        unsigned char requiredPrograms[128];
        wxString tempBase;
        wxString tempPath;
        bool requiresZmf;
        BAEResult saveResult;
        wxScopedCharBuffer utf8TempPath;

        if (!m_document || trackIndex < 0) {
            return nullptr;
        }
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) != BAE_NO_ERROR ||
            trackIndex >= static_cast<int>(trackCount)) {
            return nullptr;
        }

        /* Clone from a serialized full document so track-internal data (including
         * conductor/meta setup on track 0) survives in single-track exports. */
        tempBase = wxFileName::CreateTempFileName("nbstudio_single_track");
        if (tempBase.empty()) {
            return nullptr;
        }
        requiresZmf = BAERmfEditorDocument_RequiresZmf(m_document) != 0;
        tempPath = tempBase + (requiresZmf ? ".zmf" : ".rmf");
        utf8TempPath = tempPath.utf8_str();
        saveResult = BAERmfEditorDocument_SaveAsRmf(m_document, const_cast<char *>(utf8TempPath.data()));
        if (saveResult != BAE_NO_ERROR) {
            if (wxFileExists(tempPath)) {
                wxRemoveFile(tempPath);
            }
            if (wxFileExists(tempBase)) {
                wxRemoveFile(tempBase);
            }
            return nullptr;
        }

        playDoc = BAERmfEditorDocument_LoadFromFile(const_cast<char *>(utf8TempPath.data()));
        if (wxFileExists(tempPath)) {
            wxRemoveFile(tempPath);
        }
        if (wxFileExists(tempBase)) {
            wxRemoveFile(tempBase);
        }
        if (!playDoc) {
            return nullptr;
        }

        if (BAERmfEditorDocument_GetTrackCount(playDoc, &trackCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }

        /* Keep conductor track 0 plus the selected track. */
        for (int idx = static_cast<int>(trackCount) - 1; idx >= 0; --idx) {
            bool keepTrack;

            keepTrack = (idx == 0) || (idx == trackIndex);
            if (!keepTrack) {
                if (BAERmfEditorDocument_DeleteTrack(playDoc, static_cast<uint16_t>(idx)) != BAE_NO_ERROR) {
                    BAERmfEditorDocument_Delete(playDoc);
                    return nullptr;
                }
            }
        }

        /* Re-apply single-track sample filtering now that the track set is final. */
        std::fill(requiredPrograms, requiredPrograms + 128, static_cast<unsigned char>(0));
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(playDoc, &trackCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }
        for (uint16_t ti = 0; ti < trackCount; ++ti) {
            BAERmfEditorTrackInfo info;
            uint32_t noteCount;

            if (BAERmfEditorDocument_GetTrackInfo(playDoc, ti, &info) == BAE_NO_ERROR) {
                if (info.program < 128) {
                    requiredPrograms[info.program] = 1;
                }
            }
            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(playDoc, ti, &noteCount) != BAE_NO_ERROR) {
                continue;
            }
            for (uint32_t ni = 0; ni < noteCount; ++ni) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(playDoc, ti, ni, &noteInfo) == BAE_NO_ERROR &&
                    noteInfo.program < 128) {
                    requiredPrograms[noteInfo.program] = 1;
                }
            }
        }

        {
            uint32_t sampleCount;

            sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(playDoc, &sampleCount) != BAE_NO_ERROR) {
                BAERmfEditorDocument_Delete(playDoc);
                return nullptr;
            }
            for (int si = static_cast<int>(sampleCount) - 1; si >= 0; --si) {
                BAERmfEditorSampleInfo sampleInfo;

                if (BAERmfEditorDocument_GetSampleInfo(playDoc, static_cast<uint32_t>(si), &sampleInfo) != BAE_NO_ERROR) {
                    continue;
                }
                if (sampleInfo.program >= 128 || !requiredPrograms[sampleInfo.program]) {
                    if (BAERmfEditorDocument_DeleteSample(playDoc, static_cast<uint32_t>(si)) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(playDoc);
                        return nullptr;
                    }
                }
            }
        }

        fprintf(stderr,
                "[nbstudio] Single-track build keepTrack0=1 selectedTrack=%d (program filter restored)\n",
                trackIndex);
        return playDoc;
    }

    void StopPlayback(bool releaseSong) {
        if (!m_playbackSong) {
            m_playbackTimer.Stop();
            if (releaseSong) {
                m_playbackSongBlob.clear();
            }
            return;
        }
        BAESong_Stop(m_playbackSong, FALSE);
        if (releaseSong) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            m_playbackTimer.Stop();
            m_playbackSongBlob.clear();
        }
    }

    void ShutdownPlaybackEngine() {
        if (m_playbackMixer) {
            BAEMixer_Close(m_playbackMixer);
            BAEMixer_Delete(m_playbackMixer);
            m_playbackMixer = nullptr;
        }
    }

    bool EnsurePlaybackTempPath(bool asMidi) {
        wxString targetPath;

        targetPath = wxFileName::GetTempDir() + (asMidi ? "/rmfeditwx_playback.mid" : "/rmfeditwx_playback.rmf");
        if (m_playbackTempPath == targetPath) {
            return true;
        }
        m_playbackTempPath = targetPath;
        return !m_playbackTempPath.empty();
    }

    bool EnsurePreviewSampleTempPath() {
        if (!m_previewSampleTempPath.empty()) {
            return true;
        }
        m_previewSampleTempPath = wxFileName::GetTempDir() + "/rmfeditwx_preview_sample.wav";
        return !m_previewSampleTempPath.empty();
    }

    bool BuildPreviewPlaybackBlob(BAERmfEditorDocument *playDoc, std::vector<unsigned char> *outData) {
        wxString tempBase;
        wxString preferredPath;
        wxString fallbackPath;
        wxString readPath;
        wxFile file;
        wxFileOffset dataSize;
        BAEResult saveResult;
        bool wroteTarget;
        bool requiresZmf;

        if (!playDoc || !outData) {
            return false;
        }
        tempBase = wxFileName::CreateTempFileName("nbstudio_preview");
        if (tempBase.empty()) {
            return false;
        }
        requiresZmf = (BAERmfEditorDocument_RequiresZmf(playDoc) != 0);
        preferredPath = tempBase + (requiresZmf ? ".zmf" : ".rmf");
        fallbackPath = tempBase + (requiresZmf ? ".rmf" : ".zmf");

        {
            wxScopedCharBuffer utf8Preferred = preferredPath.utf8_str();

            saveResult = BAERmfEditorDocument_SaveAsRmf(playDoc, const_cast<char *>(utf8Preferred.data()));
            wroteTarget = (saveResult == BAE_NO_ERROR) &&
                          wxFileExists(preferredPath) &&
                          (wxFileName(preferredPath).GetSize().GetValue() > 0);
        }

        if (wroteTarget) {
            readPath = preferredPath;
        } else {
            wxScopedCharBuffer utf8Fallback = fallbackPath.utf8_str();

            saveResult = BAERmfEditorDocument_SaveAsRmf(playDoc, const_cast<char *>(utf8Fallback.data()));
            wroteTarget = (saveResult == BAE_NO_ERROR) &&
                          wxFileExists(fallbackPath) &&
                          (wxFileName(fallbackPath).GetSize().GetValue() > 0);
            if (wroteTarget) {
                readPath = fallbackPath;
            }
        }

        if (!wroteTarget || readPath.empty()) {
            if (wxFileExists(preferredPath)) {
                wxRemoveFile(preferredPath);
            }
            if (wxFileExists(fallbackPath)) {
                wxRemoveFile(fallbackPath);
            }
            if (wxFileExists(tempBase)) {
                wxRemoveFile(tempBase);
            }
            return false;
        }
        if (!file.Open(readPath, wxFile::read)) {
            if (wxFileExists(preferredPath)) {
                wxRemoveFile(preferredPath);
            }
            if (wxFileExists(fallbackPath)) {
                wxRemoveFile(fallbackPath);
            }
            if (wxFileExists(tempBase)) {
                wxRemoveFile(tempBase);
            }
            return false;
        }
        dataSize = file.Length();
        if (dataSize <= 0) {
            file.Close();
            if (wxFileExists(preferredPath)) {
                wxRemoveFile(preferredPath);
            }
            if (wxFileExists(fallbackPath)) {
                wxRemoveFile(fallbackPath);
            }
            if (wxFileExists(tempBase)) {
                wxRemoveFile(tempBase);
            }
            return false;
        }
        outData->assign(static_cast<size_t>(dataSize), 0);
        if (file.Read(outData->data(), static_cast<size_t>(dataSize)) != dataSize) {
            file.Close();
            outData->clear();
            if (wxFileExists(preferredPath)) {
                wxRemoveFile(preferredPath);
            }
            if (wxFileExists(fallbackPath)) {
                wxRemoveFile(fallbackPath);
            }
            if (wxFileExists(tempBase)) {
                wxRemoveFile(tempBase);
            }
            return false;
        }
        file.Close();
        if (wxFileExists(preferredPath)) {
            wxRemoveFile(preferredPath);
        }
        if (wxFileExists(fallbackPath)) {
            wxRemoveFile(fallbackPath);
        }
        if (wxFileExists(tempBase)) {
            wxRemoveFile(tempBase);
        }
        return true;
    }

    bool CopyExtendedInstrumentDataForPrograms(BAERmfEditorDocument *dest,
                                               BAERmfEditorDocument const *src,
                                               unsigned char const *programFlags128) {
        uint32_t sampleCount;
        std::vector<uint32_t> copiedInstIds;

        if (!dest || !src || !programFlags128) {
            return false;
        }
        sampleCount = 0;
        if (BAERmfEditorDocument_GetSampleCount(src, &sampleCount) != BAE_NO_ERROR) {
            return false;
        }
        for (uint32_t i = 0; i < sampleCount; ++i) {
            BAERmfEditorSampleInfo sampleInfo;
            uint32_t instID;
            BAERmfEditorInstrumentExtInfo extInfo;

            if (BAERmfEditorDocument_GetSampleInfo(src, i, &sampleInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (sampleInfo.program >= 128 || !programFlags128[sampleInfo.program]) {
                continue;
            }
            if (BAERmfEditorDocument_GetInstIDForSample(src, i, &instID) != BAE_NO_ERROR) {
                continue;
            }
            if (std::find(copiedInstIds.begin(), copiedInstIds.end(), instID) != copiedInstIds.end()) {
                continue;
            }
            if (BAERmfEditorDocument_GetInstrumentExtInfo(src, instID, &extInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (BAERmfEditorDocument_SetInstrumentExtInfo(dest, instID, &extInfo) != BAE_NO_ERROR) {
                return false;
            }
            copiedInstIds.push_back(instID);
        }
        return true;
    }

    uint64_t TicksToMicroseconds(uint32_t tick) const {
        uint16_t tpq;
        uint32_t baseBpm;
        uint32_t tempoCount;
        uint32_t currentTick;
        uint32_t currentMicrosecondsPerQuarter;
        uint32_t tempoIndex;
        uint64_t usec;

        if (!m_document) {
            return 0;
        }
        tpq = 480;
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        if (tpq == 0) {
            tpq = 480;
        }
        baseBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &baseBpm);
        if (baseBpm == 0) {
            baseBpm = 120;
        }
        tempoCount = 0;
        currentTick = 0;
        currentMicrosecondsPerQuarter = 60000000UL / baseBpm;
        usec = 0;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return (static_cast<uint64_t>(tick) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
        }
        for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
            uint32_t eventTick;
            uint32_t eventMicrosecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &eventTick, &eventMicrosecondsPerQuarter) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick > tick) {
                break;
            }
            if (eventTick > currentTick) {
                usec += (static_cast<uint64_t>(eventTick - currentTick) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
            }
            currentTick = eventTick;
            if (eventMicrosecondsPerQuarter > 0) {
                currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
            }
        }
        if (tick > currentTick) {
            usec += (static_cast<uint64_t>(tick - currentTick) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
        }
        return usec;
    }

    uint32_t MicrosecondsToTicks(uint32_t usec) const {
        uint16_t tpq;
        uint32_t baseBpm;
        uint32_t tempoCount;
        uint32_t currentTick;
        uint32_t currentMicrosecondsPerQuarter;
        uint32_t tempoIndex;
        uint64_t remainingUsec;

        if (!m_document) {
            return 0;
        }
        tpq = 480;
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        if (tpq == 0) {
            tpq = 480;
        }
        baseBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &baseBpm);
        if (baseBpm == 0) {
            baseBpm = 120;
        }
        tempoCount = 0;
        currentTick = 0;
        currentMicrosecondsPerQuarter = 60000000UL / baseBpm;
        remainingUsec = usec;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return static_cast<uint32_t>((remainingUsec * static_cast<uint64_t>(tpq)) / static_cast<uint64_t>(currentMicrosecondsPerQuarter));
        }
        for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
            uint32_t eventTick;
            uint32_t eventMicrosecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &eventTick, &eventMicrosecondsPerQuarter) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick <= currentTick) {
                if (eventMicrosecondsPerQuarter > 0) {
                    currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
                }
                continue;
            }
            {
                uint32_t segmentTicks;
                uint64_t segmentUsec;

                segmentTicks = eventTick - currentTick;
                segmentUsec = (static_cast<uint64_t>(segmentTicks) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
                if (remainingUsec < segmentUsec) {
                    return currentTick + static_cast<uint32_t>((remainingUsec * static_cast<uint64_t>(tpq)) / static_cast<uint64_t>(currentMicrosecondsPerQuarter));
                }
                remainingUsec -= segmentUsec;
                currentTick = eventTick;
            }
            if (eventMicrosecondsPerQuarter > 0) {
                currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
            }
        }
        return currentTick + static_cast<uint32_t>((remainingUsec * static_cast<uint64_t>(tpq)) / static_cast<uint64_t>(currentMicrosecondsPerQuarter));
    }

    wxString FormatUsecClock(uint64_t usec) const {
        uint64_t totalSeconds;
        uint64_t minutes;
        uint64_t seconds;
        uint64_t tenths;

        totalSeconds = usec / 1000000ULL;
        minutes = totalSeconds / 60ULL;
        seconds = totalSeconds % 60ULL;
        tenths = (usec % 1000000ULL) / 100000ULL;
        return wxString::Format("%llu:%02llu.%llu",
                                static_cast<unsigned long long>(minutes),
                                static_cast<unsigned long long>(seconds),
                                static_cast<unsigned long long>(tenths));
    }

    void UpdatePositionLabel(uint64_t posUsec, uint64_t lenUsec) {
        if (!m_positionLabel) {
            return;
        }
        m_positionLabel->SetLabel(wxString::Format("%s / %s",
                                                   FormatUsecClock(posUsec),
                                                   FormatUsecClock(lenUsec)));
    }

    void UpdatePositionLabelFromDocumentTick(uint32_t tick) {
        uint32_t endTick;
        uint64_t posUsec;
        uint64_t lenUsec;

        endTick = PianoRollPanel_GetDocumentEndTick(m_pianoRoll);
        posUsec = TicksToMicroseconds(tick);
        lenUsec = TicksToMicroseconds(endTick);
        if (lenUsec < posUsec) {
            lenUsec = posUsec;
        }
        UpdatePositionLabel(posUsec, lenUsec);
    }

    int GetSelectedTrack() const {
        return m_trackList->GetSelection();
    }

    void LoadDocument(wxString const &path) {
        wxScopedCharBuffer utf8Path;
        BAERmfEditorDocument *document;

        utf8Path = path.utf8_str();
        document = BAERmfEditorDocument_LoadFromFile(const_cast<char *>(utf8Path.data()));
        if (!document) {
            wxMessageBox("Failed to open file as RMF or MIDI.", "Open Failed", wxOK | wxICON_ERROR, this);
            return;
        }
        if (m_document) {
        if (!m_previewSampleTempPath.empty() && wxFileExists(m_previewSampleTempPath)) {
            wxRemoveFile(m_previewSampleTempPath);
        }
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = document;
        ClearUndoHistory();
        m_currentPath = path;
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        UpdatePositionLabelFromDocumentTick(0);
        StopPlayback(true);
        PopulateTrackList();
        PopulateSampleList();
        UpdateFrameTitle();
        SetStatusText(path, 1);
    }

    void PopulateTrackList() {
        uint16_t trackCount;
        uint16_t trackIndex;

        m_trackList->Clear();
        if (!m_document) {
            UpdateControlsFromSelection();
            return;
        }
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);
        for (trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            BAERmfEditorTrackInfo trackInfo;
            wxString label;

            if (BAERmfEditorDocument_GetTrackInfo(m_document, trackIndex, &trackInfo) != BAE_NO_ERROR) {
                continue;
            }
            label = trackInfo.name && trackInfo.name[0]
                        ? wxString::Format("%u: %s", static_cast<unsigned>(trackIndex + 1), trackInfo.name)
                        : wxString::Format("%u: Ch %u Prog %u", static_cast<unsigned>(trackIndex + 1), static_cast<unsigned>(trackInfo.channel + 1), static_cast<unsigned>(trackInfo.program));
            m_trackList->Append(label);
        }
        if (!m_trackList->IsEmpty()) {
            m_trackList->SetSelection(0);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, 0);
        } else {
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, -1);
        }
        UpdateControlsFromSelection();
    }

    void UpdateControlsFromSelection() {
        uint32_t tempoBpm;
        BAERmfEditorTrackInfo trackInfo;
        int selectedTrack;

        m_updatingControls = true;
        selectedTrack = GetSelectedTrack();
        if (!m_document) {
            m_tempoSpin->SetValue(120);
            m_bankSpin->SetValue(0);
            m_programSpin->SetValue(0);
            m_transposeSpin->SetValue(0);
            m_panSpin->SetValue(64);
            m_volumeSpin->SetValue(100);
            m_updatingControls = false;
            return;
        }
        tempoBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &tempoBpm);
        m_tempoSpin->SetValue(static_cast<int>(tempoBpm));
        if (selectedTrack >= 0 && BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) == BAE_NO_ERROR) {
            BAERmfEditorNoteInfo selectedNote;

            if (PianoRollPanel_GetSelectedNoteInfo(m_pianoRoll, &selectedNote)) {
                m_bankSpin->SetValue(DisplayBankFromInternal(selectedNote.bank));
                m_programSpin->SetValue(selectedNote.program);
            } else {
                m_bankSpin->SetValue(DisplayBankFromInternal(trackInfo.bank));
                m_programSpin->SetValue(trackInfo.program);
            }
            m_transposeSpin->SetValue(trackInfo.transpose);
            m_panSpin->SetValue(trackInfo.pan);
            m_volumeSpin->SetValue(trackInfo.volume);
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll,
                                                InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue())),
                                                static_cast<unsigned char>(m_programSpin->GetValue()));
        }
        m_updatingControls = false;
    }

    void UpdateFrameTitle() {
        wxString title;

        title = "NeoBAE Studio v";
        title += kVersionString;
        if (!m_currentPath.empty()) {
            title += " - ";
            title += wxFileNameFromPath(m_currentPath);
        }
        SetTitle(title);
    }

    void OnOpen(wxCommandEvent &) {
        wxFileDialog dialog(this,
                            "Open RMF or MIDI",
                            wxEmptyString,
                            wxEmptyString,
                            "RMF and MIDI files (*.rmf;*.zmf;*.mid;*.midi)|*.rmf;*.zmf;*.mid;*.midi|RMF files (*.rmf;*.zmf)|*.rmf;*.zmf|MIDI files (*.mid;*.midi)|*.mid;*.midi|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            LoadDocument(dialog.GetPath());
        }
    }

    void OnSaveAs(wxCommandEvent &) {
        BAERmfEditorDocument *saveDoc;
        bool saveCurrentTrack;
        int selectedTrack;
        bool requiresZmf;

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save As RMF", wxOK | wxICON_INFORMATION, this);
            return;
        }
        saveDoc = m_document;
        saveCurrentTrack = (m_playScopeChoice && m_playScopeChoice->GetSelection() == 1);
        selectedTrack = GetSelectedTrack();
        if (saveCurrentTrack) {
            uint16_t trackCount;

            if (selectedTrack < 0) {
                trackCount = 0;
                if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) == BAE_NO_ERROR && trackCount > 0) {
                    selectedTrack = 0;
                    if (m_trackList && m_trackList->GetCount() > 0) {
                        m_trackList->SetSelection(0);
                    }
                    PianoRollPanel_SetSelectedTrack(m_pianoRoll, 0);
                }
            }
            saveDoc = BuildSingleTrackPlaybackDocument(selectedTrack);
            if (!saveDoc) {
                wxMessageBox("Failed to build selected track for RMF save.", "Save Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            fprintf(stderr,
                    "[nbstudio] SaveAs using single-track document selectedTrack=%d\n",
                    selectedTrack);
        }

        requiresZmf = BAERmfEditorDocument_RequiresZmf(saveDoc) != 0;

        wxFileDialog dialog(this,
                            requiresZmf ? "Save As ZMF (required by FLAC/Vorbis/Opus samples)"
                                        : "Save As RMF",
                            wxEmptyString,
                            wxEmptyString,
                            requiresZmf ? "ZMF files (*.zmf)|*.zmf"
                                        : "RMF and ZMF files (*.rmf;*.zmf)|*.rmf;*.zmf",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) {
            if (saveDoc != m_document) {
                BAERmfEditorDocument_Delete(saveDoc);
            }
            return;
        }
        {
            wxString targetPath = dialog.GetPath();
            /* Enforce .zmf extension when document contains FLAC/Vorbis/Opus samples */
            if (requiresZmf) {
                wxFileName fn(targetPath);
                if (fn.GetExt().Lower() != "zmf") {
                    fn.SetExt("zmf");
                    targetPath = fn.GetFullPath();
                }
            }
            wxScopedCharBuffer utf8TargetPath = targetPath.utf8_str();
            BAEResult saveResult;
            bool wroteTarget;

            saveResult = BAERmfEditorDocument_SaveAsRmf(saveDoc, const_cast<char *>(utf8TargetPath.data()));
            wroteTarget = (saveResult == BAE_NO_ERROR) && wxFileExists(targetPath) && (wxFileName(targetPath).GetSize().GetValue() > 0);

            if (!wroteTarget) {
                wxString tempBase = wxFileName::CreateTempFileName("nbstudio_save");
                wxString tempRmf = tempBase + ".rmf";
                wxScopedCharBuffer utf8TempPath = tempRmf.utf8_str();

                if (saveResult != BAE_NO_ERROR || BAERmfEditorDocument_SaveAsRmf(saveDoc, const_cast<char *>(utf8TempPath.data())) != BAE_NO_ERROR) {
                    if (wxFileExists(tempBase)) {
                        wxRemoveFile(tempBase);
                    }
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Failed to save RMF file.", "Save Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                if (wxFileExists(targetPath)) {
                    wxRemoveFile(targetPath);
                }
                if (!wxCopyFile(tempRmf, targetPath, true)) {
                    if (wxFileExists(tempRmf)) {
                        wxRemoveFile(tempRmf);
                    }
                    if (wxFileExists(tempBase)) {
                        wxRemoveFile(tempBase);
                    }
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Failed to write target file path.", "Save Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                if (wxFileExists(tempRmf)) {
                    wxRemoveFile(tempRmf);
                }
                if (wxFileExists(tempBase)) {
                    wxRemoveFile(tempBase);
                }
            }
            if (saveDoc != m_document) {
                BAERmfEditorDocument_Delete(saveDoc);
            }
            SetStatusText(targetPath, 1);
        }
    }

    void OnTrackSelected(wxCommandEvent &) {
        PianoRollPanel_SetSelectedTrack(m_pianoRoll, GetSelectedTrack());
        UpdateControlsFromSelection();
    }

    void OnTempoChanged(wxCommandEvent &) {
        if (!m_document || m_updatingControls) {
            return;
        }
        BeginUndoAction("Change Tempo");
        if (BAERmfEditorDocument_SetTempoBPM(m_document, static_cast<uint32_t>(m_tempoSpin->GetValue())) == BAE_NO_ERROR) {
            CommitUndoAction("Change Tempo");
            PianoRollPanel_RefreshFromDocument(m_pianoRoll, false);
        } else {
            CancelUndoAction();
        }
    }

    void OnTrackSettingsChanged(wxCommandEvent &) {
        BAERmfEditorTrackInfo trackInfo;
        BAERmfEditorNoteInfo selectedNote;
        int selectedTrack;

        if (!m_document || m_updatingControls) {
            return;
        }
        selectedTrack = GetSelectedTrack();
        if (selectedTrack < 0 || BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) != BAE_NO_ERROR) {
            return;
        }
        if (PianoRollPanel_GetSelectedNoteInfo(m_pianoRoll, &selectedNote)) {
            selectedNote.bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
            selectedNote.program = static_cast<unsigned char>(m_programSpin->GetValue());
            PianoRollPanel_SetSelectedNoteInstrument(m_pianoRoll, selectedNote.bank, selectedNote.program);
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, selectedNote.bank, selectedNote.program);
            return;
        }
        trackInfo.bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
        trackInfo.program = static_cast<unsigned char>(m_programSpin->GetValue());
        trackInfo.transpose = static_cast<int16_t>(m_transposeSpin->GetValue());
        trackInfo.pan = static_cast<unsigned char>(m_panSpin->GetValue());
        trackInfo.volume = static_cast<unsigned char>(m_volumeSpin->GetValue());
        if (BAERmfEditorDocument_SetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) == BAE_NO_ERROR) {
            if (selectedTrack >= 0 && selectedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetString(static_cast<unsigned>(selectedTrack), BuildTrackLabel(selectedTrack, trackInfo));
            }
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, trackInfo.bank, trackInfo.program);
            PianoRollPanel_Refresh(m_pianoRoll);
        }
    }

    void OnTrackContextMenu(wxContextMenuEvent &) {
        wxMenu menu;

        menu.Append(ID_TrackAdd, "Add Track");
        menu.Append(ID_TrackDelete, "Delete Track");
        PopupMenu(&menu);
    }

    void OnSampleContextMenu(wxContextMenuEvent &) {
        wxMenu menu;

        if (m_sampleList->GetSelection() >= 0) {
            menu.Append(ID_InstrumentEdit, "Edit Instrument...");
            menu.Append(ID_SampleDelete, "Delete Instrument");
        }
        PopupMenu(&menu);
    }

    void OnTrackAdd(wxCommandEvent &) {
        BAERmfEditorTrackSetup setup;
        BAERmfEditorTrackInfo firstTrackInfo;
        uint16_t trackCount;
        uint16_t newTrackIndex;
        int defaultChannel;
        long channelValue;
        long programValue;

        if (!m_document) {
            return;
        }
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);
        defaultChannel = 0;
        if (trackCount > 0 && BAERmfEditorDocument_GetTrackInfo(m_document, 0, &firstTrackInfo) == BAE_NO_ERROR) {
            defaultChannel = firstTrackInfo.channel;
        }
        channelValue = wxGetNumberFromUser("MIDI channel (1-16)", "Channel", "Add Track", defaultChannel + 1, 1, 16, this);
        if (channelValue < 0) {
            return;
        }
        programValue = wxGetNumberFromUser("Program (0-127)", "Program", "Add Track", 0, 0, 127, this);
        if (programValue < 0) {
            return;
        }
        setup.channel = static_cast<unsigned char>(channelValue - 1);
        setup.bank = 0;
        setup.program = static_cast<unsigned char>(programValue);
        wxString trackName = wxString::Format("Track %u", static_cast<unsigned>(trackCount + 1));
        wxScopedCharBuffer utf8 = trackName.utf8_str();
        setup.name = const_cast<char *>(utf8.data());
        if (BAERmfEditorDocument_AddTrack(m_document, &setup, &newTrackIndex) == BAE_NO_ERROR) {
            ClearUndoHistory();
            PopulateTrackList();
            m_trackList->SetSelection(newTrackIndex);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, newTrackIndex);
            UpdateControlsFromSelection();
        }
    }

    void OnTrackDelete(wxCommandEvent &) {
        int selectedTrack;

        if (!m_document) {
            return;
        }
        selectedTrack = GetSelectedTrack();
        if (selectedTrack < 0) {
            return;
        }
        if (wxMessageBox("Delete selected track?", "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        if (BAERmfEditorDocument_DeleteTrack(m_document, static_cast<uint16_t>(selectedTrack)) == BAE_NO_ERROR) {
            ClearUndoHistory();
            PopulateTrackList();
        }
    }

    void OnPlay(wxCommandEvent &) {
        wxScopedCharBuffer utf8CurrentPath;
        BAERmfEditorDocument *playDoc;
        bool singleTrackMode;
        int selectedTrack;
        BAEResult loadResult;
        std::vector<unsigned char> playbackBlob;
        BAELoadResult loadInfo;

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Playback", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize audio engine.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        BAERmfEditorDocument_DebugReportMidiRoundTripDiff(m_document);
        selectedTrack = GetSelectedTrack();
        singleTrackMode = (m_playScopeChoice->GetSelection() == 1);
        fprintf(stderr,
            "[nbstudio] OnPlay scope=%d selectedTrack=%d currentPath='%s'\n",
            singleTrackMode ? 1 : 0,
            selectedTrack,
            m_currentPath.empty() ? "" : static_cast<char const *>(m_currentPath.utf8_str()));
        if (singleTrackMode && selectedTrack < 0) {
            uint16_t trackCount;

            trackCount = 0;
            if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) == BAE_NO_ERROR && trackCount > 0) {
                selectedTrack = 0;
                if (m_trackList && m_trackList->GetCount() > 0) {
                    m_trackList->SetSelection(0);
                }
                PianoRollPanel_SetSelectedTrack(m_pianoRoll, 0);
            }
        }
        if (!m_currentPath.empty()) {
            utf8CurrentPath = m_currentPath.utf8_str();
        }
        // Use the same document-building path as OnSaveAs so preview and output are
        // serialized identically. The only difference is that the bytes are intercepted
        // from a temp file instead of written to a user-chosen path.
        bool ownPlayDoc = false;
        if (singleTrackMode) {
            playDoc = BuildSingleTrackPlaybackDocument(selectedTrack);
            ownPlayDoc = true;
            if (!playDoc) {
                wxMessageBox("Failed to build selected-track playback document.", "Playback Error", wxOK | wxICON_ERROR, this);
                return;
            }
        } else {
            playDoc = m_document;
        }
        if (!BuildPreviewPlaybackBlob(playDoc, &playbackBlob)) {
            if (ownPlayDoc) {
                BAERmfEditorDocument_Delete(playDoc);
            }
            wxMessageBox("Failed to build in-memory playback ZMF.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        if (ownPlayDoc) {
            BAERmfEditorDocument_Delete(playDoc);
        }
        StopPlayback(true);
        memset(&loadInfo, 0, sizeof(loadInfo));
        m_playbackSongBlob = std::move(playbackBlob);
        loadResult = BAEMixer_LoadFromMemory(m_playbackMixer,
                             m_playbackSongBlob.data(),
                             static_cast<uint32_t>(m_playbackSongBlob.size()),
                                             &loadInfo);
        fprintf(stderr,
                "[nbstudio] BAEMixer_LoadFromMemory result=%d type=%d fileType=%d\n",
                static_cast<int>(loadResult),
                static_cast<int>(loadInfo.type),
                static_cast<int>(loadInfo.fileType));
        if (loadResult != BAE_NO_ERROR || loadInfo.type != BAE_LOAD_TYPE_SONG || !loadInfo.data.song) {
            BAELoadResult_Cleanup(&loadInfo);
            m_playbackSongBlob.clear();
            wxMessageBox("Failed to load playback song from memory.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        m_playbackSong = loadInfo.data.song;
        loadInfo.type = BAE_LOAD_TYPE_NONE;
        loadInfo.data.song = nullptr;
        {
            BAEResult seekResult;
            BAEResult prerollResult;
            BAEResult loopResult;

            loopResult = BAESong_SetLoops(m_playbackSong, 0);
            seekResult = BAESong_SetMicrosecondPosition(m_playbackSong, 0);
            prerollResult = BAESong_Preroll(m_playbackSong);
            fprintf(stderr,
                    "[nbstudio] Pre-start prep loops=%d seek0=%d preroll=%d\n",
                    static_cast<int>(loopResult),
                    static_cast<int>(seekResult),
                    static_cast<int>(prerollResult));
        }
        BAEResult startResult = BAESong_Start(m_playbackSong, 0);
        fprintf(stderr, "[nbstudio] BAESong_Start result=%d\n", static_cast<int>(startResult));
        if (startResult != BAE_NO_ERROR) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            m_playbackSongBlob.clear();
            wxMessageBox("Failed to start playback.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        BAESong_SetVolume(m_playbackSong, GetPreviewVolumeFixed());
        {
            BAE_UNSIGNED_FIXED tempoFactor;
            BAEResult tempoSetResult;

            /* Ensure editor playback always starts at engine-normal 1.0x speed. */
            tempoSetResult = BAESong_SetMasterTempo(m_playbackSong, static_cast<BAE_UNSIGNED_FIXED>(65536));
            fprintf(stderr, "[nbstudio] BAESong_SetMasterTempo(1.0) result=%d\n", static_cast<int>(tempoSetResult));
            tempoFactor = 0;
            if (BAESong_GetMasterTempo(m_playbackSong, &tempoFactor) == BAE_NO_ERROR) {
                fprintf(stderr, "[nbstudio] Song master tempo factor=%lu\n", static_cast<unsigned long>(tempoFactor));
            }
        }
        m_playbackTimer.Start(40);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        m_autoFollowPlayhead = true;
        PianoRollPanel_SetPlayheadTick(m_pianoRoll, 0);
        {
            uint32_t lenUsec;

            lenUsec = 0;
            BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec);
            UpdatePositionLabel(0, lenUsec);
        }
        SetStatusText("Playing", 0);
    }

    void OnPauseResume(wxCommandEvent &) {
        BAE_BOOL isPaused;

        if (!m_playbackSong) {
            return;
        }
        isPaused = FALSE;
        BAESong_IsPaused(m_playbackSong, &isPaused);
        if (isPaused) {
            BAESong_Resume(m_playbackSong);
            SetStatusText("Playing", 0);
        } else {
            BAESong_Pause(m_playbackSong);
            SetStatusText("Paused", 0);
        }
    }

    void OnPreviewVolumeChanged(wxCommandEvent &) {
        if (m_playbackSong) {
            BAESong_SetVolume(m_playbackSong, GetPreviewVolumeFixed());
        }
    }

    void OnStop(wxCommandEvent &) {
        StopPlayback(true);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        UpdatePositionLabelFromDocumentTick(0);
        SetStatusText("Stopped", 0);
    }

    void OnPlaybackTimer(wxTimerEvent &) {
        BAE_BOOL isDone;

        if (!m_playbackSong) {
            m_playbackTimer.Stop();
            return;
        }
        {
            uint32_t posUsec;
            uint32_t lenUsec;

            posUsec = 0;
            lenUsec = 0;
            if (BAESong_GetMicrosecondPosition(m_playbackSong, &posUsec) == BAE_NO_ERROR &&
                BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec) == BAE_NO_ERROR &&
                lenUsec > 0) {
                int sliderPos;

                sliderPos = static_cast<int>((static_cast<uint64_t>(posUsec) * 1000ULL) / static_cast<uint64_t>(lenUsec));
                m_ignoreSeekEvent = true;
                m_positionSlider->SetValue(std::clamp(sliderPos, 0, 1000));
                m_ignoreSeekEvent = false;
                UpdatePositionLabel(posUsec, lenUsec);
                {
                    uint32_t playheadTick;

                    playheadTick = MicrosecondsToTicks(posUsec);
                    PianoRollPanel_SetPlayheadTick(m_pianoRoll, playheadTick);
                    if (m_autoFollowPlayhead) {
                        PianoRollPanel_EnsurePlayheadVisible(m_pianoRoll, playheadTick);
                    }
                }
            }
        }
        isDone = FALSE;
        if (BAESong_IsDone(m_playbackSong, &isDone) == BAE_NO_ERROR && isDone) {
            StopPlayback(true);
            m_ignoreSeekEvent = true;
            m_positionSlider->SetValue(1000);
            m_ignoreSeekEvent = false;
            UpdatePositionLabelFromDocumentTick(PianoRollPanel_GetDocumentEndTick(m_pianoRoll));
            SetStatusText("Playback complete", 0);
        }
    }

    void OnSeekSlider(wxCommandEvent &) {
        uint64_t lenUsec;
        uint64_t targetUsec;
        uint32_t seekTick;

        if (m_ignoreSeekEvent) {
            return;
        }
        m_autoFollowPlayhead = false;
        seekTick = 0;
        if (m_playbackSong) {
            uint32_t lenUsec32;

            lenUsec = 0;
            lenUsec32 = 0;
            if (BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec32) == BAE_NO_ERROR && lenUsec32 > 0) {
                lenUsec = lenUsec32;
                targetUsec = (lenUsec * static_cast<uint64_t>(m_positionSlider->GetValue())) / 1000ULL;
                BAESong_SetMicrosecondPosition(m_playbackSong, static_cast<uint32_t>(targetUsec));
                seekTick = MicrosecondsToTicks(static_cast<uint32_t>(targetUsec));
                UpdatePositionLabel(targetUsec, lenUsec);
            }
        } else {
            uint32_t endTick;

            endTick = PianoRollPanel_GetDocumentEndTick(m_pianoRoll);
            lenUsec = TicksToMicroseconds(endTick);
            targetUsec = (lenUsec * static_cast<uint64_t>(m_positionSlider->GetValue())) / 1000ULL;
            seekTick = MicrosecondsToTicks(static_cast<uint32_t>(targetUsec));
            UpdatePositionLabel(targetUsec, lenUsec);
        }
        PianoRollPanel_SetPlayheadTick(m_pianoRoll, seekTick);
        PianoRollPanel_JumpToTick(m_pianoRoll, seekTick);
    }

    void SeekToTickFromPianoRoll(uint32_t tick) {
        uint32_t endTick;
        uint32_t playheadTick;
        uint64_t totalUsec;
        uint64_t targetUsec;
        int sliderPos;

        if (m_ignoreSeekEvent || !m_document) {
            return;
        }
        endTick = PianoRollPanel_GetDocumentEndTick(m_pianoRoll);
        if (endTick == 0) {
            return;
        }
        tick = std::min(tick, endTick);
        playheadTick = tick;
        totalUsec = TicksToMicroseconds(endTick);
        targetUsec = TicksToMicroseconds(tick);
        if (totalUsec == 0) {
            totalUsec = 1;
        }
        if (m_playbackSong) {
            uint32_t lenUsec;

            lenUsec = 0;
            if (BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec) == BAE_NO_ERROR && lenUsec > 0) {
                if (targetUsec > lenUsec) {
                    targetUsec = lenUsec;
                }
                sliderPos = static_cast<int>((targetUsec * 1000ULL) / static_cast<uint64_t>(lenUsec));
                sliderPos = std::clamp(sliderPos, 0, 1000);
                m_autoFollowPlayhead = false;
                m_ignoreSeekEvent = true;
                m_positionSlider->SetValue(sliderPos);
                m_ignoreSeekEvent = false;
                BAESong_SetMicrosecondPosition(m_playbackSong, static_cast<uint32_t>(targetUsec));
                playheadTick = MicrosecondsToTicks(static_cast<uint32_t>(targetUsec));
                UpdatePositionLabel(targetUsec, lenUsec);
            }
        } else {
            sliderPos = static_cast<int>((targetUsec * 1000ULL) / totalUsec);
            sliderPos = std::clamp(sliderPos, 0, 1000);
            m_autoFollowPlayhead = false;
            m_ignoreSeekEvent = true;
            m_positionSlider->SetValue(sliderPos);
            m_ignoreSeekEvent = false;
            UpdatePositionLabel(targetUsec, totalUsec);
        }
        PianoRollPanel_SetPlayheadTick(m_pianoRoll, playheadTick);
    }

    void OnMetadata(wxCommandEvent &) {
        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Metadata", wxOK | wxICON_INFORMATION, this);
            return;
        }
        ShowMetadataDialog(this, m_document);
    }

    void OnLoadBank(wxCommandEvent &) {
        wxFileDialog dialog(this,
                            "Load HSB Bank",
                            wxEmptyString,
                            wxEmptyString,
                            "HSB banks (*.hsb)|*.hsb|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        if (!LoadBankFromFile(dialog.GetPath())) {
            wxMessageBox("Failed to load bank file.", "Load Bank", wxOK | wxICON_ERROR, this);
        }
    }

    void OnUnloadBanks(wxCommandEvent &) {
        if (!m_playbackMixer) {
            return;
        }
        StopPlayback(true);
        BAEMixer_UnloadBanks(m_playbackMixer);
        m_bankToken = nullptr;
        m_bankLoaded = false;
        SetStatusText("All banks unloaded", 0);
    }

    void OnSampleAdd(wxCommandEvent &) {
        wxFileDialog dialog(this,
                    "Add Embedded Instrument Sample",
                            wxEmptyString,
                            wxEmptyString,
                            "Supported audio (*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus)|*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        BAERmfEditorSampleSetup setup;
        BAESampleInfo info;
        BAEResult addResult;
        long program;
        long root;

        if (!m_document) {
            return;
        }
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        program = wxGetNumberFromUser("MIDI program (0-127)", "Program", "Embedded Instrument Mapping", 0, 0, 127, this);
        if (program < 0) {
            return;
        }
        root = wxGetNumberFromUser("Root key (0-127)", "Root Key", "Embedded Instrument Mapping", 60, 0, 127, this);
        if (root < 0) {
            return;
        }
        wxString displayName = wxFileNameFromPath(dialog.GetPath());
        wxScopedCharBuffer utf8Name = displayName.utf8_str();
        wxScopedCharBuffer utf8Path = dialog.GetPath().utf8_str();
        setup.program = static_cast<unsigned char>(program);
        setup.rootKey = static_cast<unsigned char>(root);
        setup.lowKey = 0;
        setup.highKey = 127;
        setup.displayName = const_cast<char *>(utf8Name.data());
        addResult = BAERmfEditorDocument_AddSampleFromFile(m_document,
                                                            const_cast<char *>(utf8Path.data()),
                                                            &setup,
                                                            &info);
        if (addResult != BAE_NO_ERROR) {
            if (addResult == BAE_BAD_FILE_TYPE) {
                wxMessageBox("Unsupported sample codec. Supported imports are PCM WAV/AIFF, MP3, Ogg Vorbis, FLAC, and Opus.",
                             "Embedded Instruments",
                             wxOK | wxICON_ERROR,
                             this);
                return;
            }
            wxMessageBox("Failed to add sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        PopulateSampleList();
    }

    void OnSampleNewInstrument(wxCommandEvent &) {
        BAERmfEditorSampleSetup setup;
        BAESampleInfo info;
        uint32_t newSampleIndex;
        uint32_t sampleCount;
        bool usedPrograms[128];
        long defaultProgram;
        long program;
        long root;

        if (!m_document) {
            return;
        }

        sampleCount = 0;
        memset(usedPrograms, 0, sizeof(usedPrograms));
        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        for (uint32_t i = 0; i < sampleCount; ++i) {
            BAERmfEditorSampleInfo sampleInfo;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &sampleInfo) == BAE_NO_ERROR &&
                sampleInfo.program < 128) {
                usedPrograms[sampleInfo.program] = true;
            }
        }
        defaultProgram = 0;
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                defaultProgram = i;
                break;
            }
        }

        program = wxGetNumberFromUser("MIDI program for new instrument (0-127)",
                                      "Program",
                                      "New Instrument",
                                      defaultProgram,
                                      0,
                                      127,
                                      this);
        if (program < 0) {
            return;
        }
        root = wxGetNumberFromUser("Root key (0-127)",
                                   "Root Key",
                                   "New Instrument",
                                   60,
                                   0,
                                   127,
                                   this);
        if (root < 0) {
            return;
        }

        setup.program = static_cast<unsigned char>(program);
        setup.rootKey = static_cast<unsigned char>(root);
        setup.lowKey = 0;
        setup.highKey = 127;
        setup.displayName = const_cast<char *>("New Instrument");

        if (BAERmfEditorDocument_AddEmptySample(m_document, &setup, &newSampleIndex, &info) != BAE_NO_ERROR) {
            wxMessageBox("Failed to create new instrument.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }

        PopulateSampleList();
        if (newSampleIndex < static_cast<uint32_t>(m_sampleList->GetCount())) {
            /* Find the list row that maps to this new sample's program. */
            for (size_t row = 0; row < m_sampleListMap.size(); ++row) {
                if (m_sampleListMap[row] == newSampleIndex) {
                    m_sampleList->SetSelection(static_cast<int>(row));
                    break;
                }
            }
        }

        {
            wxCommandEvent dummyEvent;
            OnInstrumentEditDblClick(dummyEvent);
        }

        {
            int curSel = m_sampleList->GetSelection();
            if (curSel >= 0 && static_cast<size_t>(curSel) < m_sampleListMap.size()) {
                uint32_t checkIndex = m_sampleListMap[static_cast<size_t>(curSel)];
                BAERmfEditorSampleInfo verifyInfo;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, checkIndex, &verifyInfo) == BAE_NO_ERROR &&
                    verifyInfo.sourcePath == NULL && verifyInfo.sampleInfo.waveFrames <= 1) {
                    /* User likely canceled without replacing; remove placeholder sample. */
                    BAERmfEditorDocument_DeleteSample(m_document, checkIndex);
                    PopulateSampleList();
                }
            }
        }
    }

    void OnSampleDelete(wxCommandEvent &) {
        int selected;
        uint32_t firstSampleIndex;
        BAERmfEditorSampleInfo firstInfo;
        uint32_t sampleCount;
        unsigned char program;
        std::vector<uint32_t> toDelete;

        if (!m_document) {
            return;
        }
        selected = m_sampleList->GetSelection();
        if (selected < 0 || static_cast<size_t>(selected) >= m_sampleListMap.size()) {
            return;
        }
        firstSampleIndex = m_sampleListMap[static_cast<size_t>(selected)];
        if (BAERmfEditorDocument_GetSampleInfo(m_document, firstSampleIndex, &firstInfo) != BAE_NO_ERROR) {
            return;
        }
        program = firstInfo.program;
        sampleCount = 0;
        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        for (uint32_t i = 0; i < sampleCount; ++i) {
            BAERmfEditorSampleInfo info;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) == BAE_NO_ERROR &&
                info.program == program) {
                toDelete.push_back(i);
            }
        }
        wxString msg = toDelete.size() > 1
            ? wxString::Format("Delete instrument P%u and all %u of its splits?",
                               static_cast<unsigned>(program),
                               static_cast<unsigned>(toDelete.size()))
            : wxString::Format("Delete instrument P%u?", static_cast<unsigned>(program));
        if (wxMessageBox(msg, "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        /* Delete in reverse order so earlier indices stay valid. */
        for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            BAERmfEditorDocument_DeleteSample(m_document, *it);
        }
        PopulateSampleList();
        int newSel = selected;
        if (newSel >= static_cast<int>(m_sampleList->GetCount())) {
            newSel = static_cast<int>(m_sampleList->GetCount()) - 1;
        }
        if (newSel >= 0) {
            m_sampleList->SetSelection(newSel);
        }
    }

    void OnInstrumentEdit(wxCommandEvent &) {
        OpenInstrumentExtEditor();
    }

    void OnInstrumentEditDblClick(wxCommandEvent &) {
        OpenInstrumentExtEditor();
    }

    void OpenInstrumentExtEditor() {
        int selected;
        uint32_t primarySampleIndex;
        uint32_t instID;
        BAERmfEditorInstrumentExtInfo extInfo;
        std::vector<uint32_t> deletedSampleIndices;
        std::vector<uint32_t> sampleIndices;
        std::vector<InstrumentEditorEditedSample> editedSamples;
        bool accepted;

        if (!m_document) {
            return;
        }
        selected = m_sampleList->GetSelection();
        if (selected < 0 || static_cast<size_t>(selected) >= m_sampleListMap.size()) {
            return;
        }
        primarySampleIndex = m_sampleListMap[static_cast<size_t>(selected)];

        /* Look up the instID and fetch extended info */
        memset(&extInfo, 0, sizeof(extInfo));
        if (BAERmfEditorDocument_GetInstIDForSample(m_document, primarySampleIndex, &instID) == BAE_NO_ERROR) {
            extInfo.instID = instID;
            BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &extInfo);
        }
        accepted = ShowInstrumentExtEditorDialog(
            this,
            m_document,
            primarySampleIndex,
            &extInfo,
            [this](uint32_t sampleIndex, int key, BAESampleInfo const *overrideInfo, int16_t splitVolumeOverride, unsigned char rootKeyOverride) {
                if (!PreviewSampleAtKey(sampleIndex, key, overrideInfo, splitVolumeOverride, rootKeyOverride)) {
                    wxMessageBox("Preview playback failed for this sample.",
                                 "Sample Preview", wxOK | wxICON_ERROR, this);
                }
            },
            [this]() {
                StopPreviewSample();
            },
            [this](uint32_t sampleIndex, wxString const &path) {
                return ReplaceSampleFromPath(sampleIndex, path);
            },
            [this](uint32_t sampleIndex, wxString const &path) {
                return ExportSampleToPath(sampleIndex, path);
            },
            &deletedSampleIndices,
            &sampleIndices,
            &editedSamples);

        if (!accepted) {
            StopPreviewSample();
            return;
        }
        StopPreviewSample();

        /* Apply sample edits */
        {
            bool ok = true;

            for (size_t i = 0; i < sampleIndices.size() && i < editedSamples.size(); ++i) {
                /* A sentinel index means the sample was created inside the dialog and needs
                 * to be added to the document first. */
                if (sampleIndices[i] == static_cast<uint32_t>(-1)) {
                    BAERmfEditorSampleSetup setup;
                    BAESampleInfo dummyInfo;
                    uint32_t newIdx = static_cast<uint32_t>(-1);
                    wxScopedCharBuffer setupName = editedSamples[i].displayName.utf8_str();
                    setup.program = editedSamples[i].program;
                    setup.rootKey = editedSamples[i].rootKey;
                    setup.lowKey = editedSamples[i].lowKey;
                    setup.highKey = editedSamples[i].highKey;
                    setup.displayName = const_cast<char *>(setupName.data());
                    if (BAERmfEditorDocument_AddEmptySample(m_document, &setup, &newIdx, &dummyInfo) != BAE_NO_ERROR) {
                        ok = false;
                        break;
                    }
                    sampleIndices[i] = newIdx;
                }
                BAERmfEditorSampleInfo info;
                wxScopedCharBuffer nameUtf8 = editedSamples[i].displayName.utf8_str();
                info.displayName = const_cast<char *>(nameUtf8.data());
                info.sourcePath = NULL;
                info.program = editedSamples[i].program;
                info.rootKey = editedSamples[i].rootKey;
                info.lowKey = editedSamples[i].lowKey;
                info.highKey = editedSamples[i].highKey;
                info.splitVolume = editedSamples[i].splitVolume;
                info.sampleInfo = editedSamples[i].sampleInfo;
                info.compressionType = editedSamples[i].compressionType;
                info.hasOriginalData = editedSamples[i].hasOriginalData ? TRUE : FALSE;
                if (BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndices[i], &info) != BAE_NO_ERROR) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                wxMessageBox("Failed to apply instrument sample edits.",
                             "Edit Instrument", wxOK | wxICON_ERROR, this);
            }
        }

        if (!deletedSampleIndices.empty()) {
            std::sort(deletedSampleIndices.begin(), deletedSampleIndices.end());
            deletedSampleIndices.erase(std::unique(deletedSampleIndices.begin(), deletedSampleIndices.end()), deletedSampleIndices.end());
            for (auto it = deletedSampleIndices.rbegin(); it != deletedSampleIndices.rend(); ++it) {
                BAERmfEditorDocument_DeleteSample(m_document, *it);
            }
        }

        PopulateSampleList();
        if (selected < static_cast<int>(m_sampleList->GetCount())) {
            m_sampleList->SetSelection(selected);
        }
    }
};

}  // namespace

class EditorApp final : public wxApp {
public:
    bool OnInit() override {
        auto *frame = new MainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(EditorApp);