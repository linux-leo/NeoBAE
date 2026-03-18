#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <zlib.h>

#include <wx/dcbuffer.h>
#include <wx/button.h>
#include <wx/choicdlg.h>
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
#include <wx/treectrl.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
}

#include "editor_instrument_ext_dialog.h"
#include "editor_metadata_dialog.h"
#include "editor_pianoroll_panel.h"
#include "batch_compress_dialog.h"

#ifdef __WXMSW__
#include <wx/msw/winundef.h>
#endif

namespace {

constexpr char const *kVersionString = "0.04.4 alpha";

static bool IsOpusCompressionType(BAERmfEditorCompressionType compressionType) {
    switch (compressionType) {
        case BAE_EDITOR_COMPRESSION_OPUS_12K:
        case BAE_EDITOR_COMPRESSION_OPUS_16K:
        case BAE_EDITOR_COMPRESSION_OPUS_24K:
        case BAE_EDITOR_COMPRESSION_OPUS_32K:
        case BAE_EDITOR_COMPRESSION_OPUS_48K:
        case BAE_EDITOR_COMPRESSION_OPUS_64K:
        case BAE_EDITOR_COMPRESSION_OPUS_96K:
        case BAE_EDITOR_COMPRESSION_OPUS_128K:
        case BAE_EDITOR_COMPRESSION_OPUS_256K:
            return true;
        default:
            return false;
    }
}

static bool IsMidiSaveExtension(wxString const &path) {
    wxString ext = wxFileName(path).GetExt().Lower();
    return ext == "mid" || ext == "midi";
}

enum {
    ID_TrackAdd = wxID_HIGHEST + 200,
    ID_TrackDelete,
    ID_SampleAdd,
    ID_SampleNewInstrument,
    ID_InstrumentEdit,
    ID_SampleDelete,
    ID_LoadBank,
    ID_ReloadInternalBank,
    ID_PlaybackTimer,
    ID_NotePreviewTimer,
    ID_CompressInstrument,
    ID_CompressAllInstruments,
    ID_CloneFromBank,
    ID_AliasFromBank,
    ID_SaveSession,
    ID_SaveSessionAs,
};

static constexpr uint8_t  kNbsMagic[4] = {'N', 'B', 'S', '\0'};
static constexpr uint16_t kNbsVersion = 1;
static constexpr uint16_t kNbsFieldRmfBlob      = 0x0001;
static constexpr uint16_t kNbsFieldSettings     = 0x0002;
static constexpr uint16_t kNbsFieldOriginalPath = 0x0003;

#pragma pack(push, 1)
struct NbsSessionSettings {
    uint8_t  settingsVersion;
    int32_t  previewVolume;
    int32_t  previewReverbType;
    uint8_t  previewLoopEnabled;
    int32_t  midiStorageMode;
    int32_t  selectedTrack;
};
#pragma pack(pop)

static void AppendLE16(std::vector<unsigned char> &buf, uint16_t val) {
    buf.push_back(static_cast<unsigned char>(val & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 8) & 0xFF));
}

static void AppendLE32(std::vector<unsigned char> &buf, uint32_t val) {
    buf.push_back(static_cast<unsigned char>(val & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 16) & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 24) & 0xFF));
}

static uint16_t ReadLE16(unsigned char const *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t ReadLE32(unsigned char const *p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

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
    std::vector<unsigned char> serializedDocument;
};

struct UndoEntry {
    wxString label;
    UndoDocumentState before;
    UndoDocumentState after;
};

class SampleTreeItemData final : public wxTreeItemData {
public:
    SampleTreeItemData(uint32_t assetID, uint32_t sampleIndex, bool isAssetNode)
        : m_assetID(assetID),
          m_sampleIndex(sampleIndex),
          m_isAssetNode(isAssetNode) {}

    uint32_t GetAssetID() const { return m_assetID; }
    uint32_t GetSampleIndex() const { return m_sampleIndex; }
    bool IsAssetNode() const { return m_isAssetNode; }

private:
    uint32_t m_assetID;
    uint32_t m_sampleIndex;
    bool m_isAssetNode;
};

static unsigned char NormalizeRootKeyForSingleKeySplit(unsigned char rootKey,
                                                        unsigned char lowKey,
                                                        unsigned char highKey) {
    if (rootKey == 0 && lowKey <= 127 && highKey <= 127 && lowKey == highKey) {
        return lowKey;
    }
    return rootKey;
}

static bool NoteInfoEquals(BAERmfEditorNoteInfo const &left, BAERmfEditorNoteInfo const &right) {
    return left.startTick == right.startTick &&
           left.durationTicks == right.durationTicks &&
           left.note == right.note &&
           left.velocity == right.velocity &&
           left.channel == right.channel &&
           left.bank == right.bank &&
           left.program == right.program;
}

static bool UndoSnapshotsEqual(UndoDocumentState const &left, UndoDocumentState const &right) {
    if (!left.serializedDocument.empty() && !right.serializedDocument.empty()) {
        if (left.serializedDocument.size() != right.serializedDocument.size()) {
            return false;
        }
        return std::equal(left.serializedDocument.begin(),
                          left.serializedDocument.end(),
                          right.serializedDocument.begin());
    }
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
        : wxFrame(nullptr, wxID_ANY, wxString::Format("NeoBAE Studio v%s", kVersionString), wxDefaultPosition, wxSize(1400, 860)),
          m_document(nullptr),
                    m_updatingControls(false),
                    m_playbackMixer(nullptr),
                    m_playbackSong(nullptr),
                    m_notePreviewSong(nullptr),
                    m_previewSound(nullptr),
                    m_playbackTimer(this, ID_PlaybackTimer),
                    m_notePreviewTimer(this, ID_NotePreviewTimer),
                    m_notePreviewActive(false),
                    m_notePreviewChannel(0),
                    m_notePreviewNote(0),
                    m_ignoreSeekEvent(false),
                    m_autoFollowPlayhead(true),
                    m_bankToken(nullptr),
                    m_bankLoaded(false),
                    m_pendingLoopHotReload(false),
                    m_pendingLoopHotReloadPosUsec(0),
                    m_pendingLoopHotReloadPaused(false),
                    m_loadingLoopControls(false),
                    m_hasPendingUndo(false),
                    m_restoringUndo(false) {
                SetMinSize(wxSize(1280, 720));
#ifdef __WXMSW__
        SetIcon(wxICON(APPICON));
#endif

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
        fileMenu->Append(wxID_SAVEAS, "Save &As...\tCtrl+Shift+S");
        fileMenu->Append(ID_SaveSession, "Save S&ession\tCtrl+S");
        fileMenu->Append(ID_SaveSessionAs, "Save Session &As (.nbs)...");
        fileMenu->AppendSeparator();
        fileMenu->Append(ID_LoadBank, "Load &Bank (HSB)...\tCtrl+B");
        fileMenu->Append(ID_CloneFromBank, "Clone &Instrument from Bank...");
        fileMenu->Append(ID_AliasFromBank, "&Alias Instrument from Bank...");
        fileMenu->Append(ID_ReloadInternalBank, "&Reload Internal Bank");
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
        m_sampleTree = new wxTreeCtrl(sidebar, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        m_sampleAddButton = new wxButton(sidebar, ID_SampleNewInstrument, "Add Instrument");
        sidebarSizer->Add(new wxStaticText(sidebar, wxID_ANY, "Tracks"), 0, wxALL, 8);
        sidebarSizer->Add(m_trackList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebarSizer->Add(new wxStaticText(sidebar, wxID_ANY, "Sample Tree"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebarSizer->Add(m_sampleTree, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
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
        m_midiStorageChoice = new wxChoice(editorPanel, wxID_ANY);
        m_previewVolumeSlider = new wxSlider(editorPanel, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(120, -1), wxSL_HORIZONTAL);
        m_previewReverbChoice = new wxChoice(editorPanel, wxID_ANY);
        m_previewLoopCheck = new wxCheckBox(editorPanel, wxID_ANY, "Loop");
        m_midiLoopEnableCheck = new wxCheckBox(editorPanel, wxID_ANY, "MIDI Loop Markers");
        m_midiLoopStartText = new wxTextCtrl(editorPanel, wxID_ANY, "0:00.000", wxDefaultPosition, wxSize(110, -1));
        m_midiLoopEndText = new wxTextCtrl(editorPanel, wxID_ANY, "0:00.000", wxDefaultPosition, wxSize(110, -1));
        m_playScopeChoice->Append("All Tracks");
        m_playScopeChoice->Append("Current Track");
        m_playScopeChoice->SetSelection(0);
        m_midiStorageChoice->Append("CMID (best effort)");
        m_midiStorageChoice->Append("ECMI (encrypt + compress)");
        m_midiStorageChoice->Append("EMID (encrypt)");
        m_midiStorageChoice->Append("MIDI (plain)");
        m_midiStorageChoice->SetSelection(1);
        m_previewReverbChoice->Append("None");
        m_previewReverbChoice->Append("Igor's Closet");
        m_previewReverbChoice->Append("Igor's Garage");
        m_previewReverbChoice->Append("Igor's Acoustic Lab");
        m_previewReverbChoice->Append("Igor's Cavern");
        m_previewReverbChoice->Append("Igor's Dungeon");
        m_previewReverbChoice->Append("Small Reflections");
        m_previewReverbChoice->Append("Early Reflections");
        m_previewReverbChoice->Append("Basement");
        m_previewReverbChoice->Append("Banquet Hall");
        m_previewReverbChoice->Append("Catacombs");
        m_previewReverbChoice->SetSelection(0);

        {
            wxSize compactSpinSize(128, -1);

            m_tempoSpin->SetMinSize(compactSpinSize);
            m_bankSpin->SetMinSize(compactSpinSize);
            m_programSpin->SetMinSize(compactSpinSize);
            m_transposeSpin->SetMinSize(compactSpinSize);
            m_panSpin->SetMinSize(compactSpinSize);
            m_volumeSpin->SetMinSize(compactSpinSize);
        }
        m_midiStorageChoice->SetMinSize(wxSize(220, -1));
        m_metadataButton->SetMinSize(wxSize(110, -1));
        m_previewReverbChoice->SetMinSize(wxSize(170, -1));
        m_previewLoopCheck->SetValue(false);

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
        transportSizer->Add(m_previewVolumeSlider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        transportSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Reverb"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        transportSizer->Add(m_previewReverbChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        transportSizer->Add(m_previewLoopCheck, 0, wxALIGN_CENTER_VERTICAL, 0);

        {
            wxBoxSizer *topRowSizer;
            wxBoxSizer *leftControlsSizer;
            wxBoxSizer *extraControlsSizer;
            wxBoxSizer *midiStorageRow;
            wxBoxSizer *midiLoopRow;

            topRowSizer = new wxBoxSizer(wxHORIZONTAL);
            leftControlsSizer = new wxBoxSizer(wxVERTICAL);
            extraControlsSizer = new wxBoxSizer(wxVERTICAL);
            midiStorageRow = new wxBoxSizer(wxHORIZONTAL);

            midiStorageRow->Add(new wxStaticText(editorPanel, wxID_ANY, "MIDI Type"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            midiStorageRow->Add(m_midiStorageChoice, 0, wxALIGN_CENTER_VERTICAL, 0);
            extraControlsSizer->Add(midiStorageRow, 0, wxALIGN_LEFT | wxBOTTOM, 8);
            midiLoopRow = new wxBoxSizer(wxHORIZONTAL);
            midiLoopRow->Add(m_midiLoopEnableCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            midiLoopRow->Add(new wxStaticText(editorPanel, wxID_ANY, "Start Time"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            midiLoopRow->Add(m_midiLoopStartText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            midiLoopRow->Add(new wxStaticText(editorPanel, wxID_ANY, "End Time"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            midiLoopRow->Add(m_midiLoopEndText, 0, wxALIGN_CENTER_VERTICAL, 0);
            extraControlsSizer->Add(m_metadataButton, 0, wxEXPAND, 0);

            leftControlsSizer->Add(controlsSizer, 0, wxEXPAND | wxBOTTOM, 8);
            leftControlsSizer->Add(midiLoopRow, 0, wxALIGN_LEFT, 0);

            topRowSizer->Add(leftControlsSizer, 1, wxEXPAND | wxRIGHT, 12);
            topRowSizer->Add(extraControlsSizer, 0, wxALIGN_TOP, 0);
            editorSizer->Add(topRowSizer, 0, wxEXPAND | wxALL, 10);
        }

        m_positionSlider = new wxSlider(editorPanel, wxID_ANY, 0, 0, 1000, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_positionLabel = new wxStaticText(editorPanel, wxID_ANY, "0:00.0 / 0:00.0");

        m_pianoRoll = CreatePianoRollPanel(editorPanel);
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
        PianoRollPanel_SetNotePreviewRequestedCallback(m_pianoRoll,
                                                       [this](uint16_t bank,
                                                              unsigned char program,
                                                              unsigned char note,
                                                              uint32_t durationTicks,
                                                              int trackIndex) {
                                                           PreviewPianoRollNote(bank, program, note, durationTicks, trackIndex);
                                                       });
        PianoRollPanel_SetNotePreviewStopRequestedCallback(m_pianoRoll,
                                                           [this]() {
                                                               StopPianoRollPreview(false);
                                                           });
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
        Bind(wxEVT_MENU, &MainFrame::OnSaveSession, this, ID_SaveSession);
        Bind(wxEVT_MENU, &MainFrame::OnSaveSessionAs, this, ID_SaveSessionAs);
        Bind(wxEVT_MENU, &MainFrame::OnUndo, this, wxID_UNDO);
        Bind(wxEVT_MENU, &MainFrame::OnRedo, this, wxID_REDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_EXIT);
        m_trackList->Bind(wxEVT_LISTBOX, &MainFrame::OnTrackSelected, this);
        m_trackList->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnTrackContextMenu, this);
        m_sampleTree->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnSampleContextMenu, this);
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
        m_previewReverbChoice->Bind(wxEVT_CHOICE, &MainFrame::OnPreviewReverbChanged, this);
        m_previewLoopCheck->Bind(wxEVT_CHECKBOX, &MainFrame::OnPreviewLoopChanged, this);
        m_midiLoopEnableCheck->Bind(wxEVT_CHECKBOX, &MainFrame::OnMidiLoopMarkersChanged, this);
        m_midiLoopStartText->Bind(wxEVT_TEXT, &MainFrame::OnMidiLoopMarkersChanged, this);
        m_midiLoopEndText->Bind(wxEVT_TEXT, &MainFrame::OnMidiLoopMarkersChanged, this);
        m_positionSlider->Bind(wxEVT_SLIDER, &MainFrame::OnSeekSlider, this);
        m_metadataButton->Bind(wxEVT_BUTTON, &MainFrame::OnMetadata, this);
        Bind(wxEVT_MENU, &MainFrame::OnTrackAdd, this, ID_TrackAdd);
        Bind(wxEVT_MENU, &MainFrame::OnTrackDelete, this, ID_TrackDelete);
        Bind(wxEVT_MENU, &MainFrame::OnSampleAdd, this, ID_SampleAdd);
        Bind(wxEVT_BUTTON, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnInstrumentEdit, this, ID_InstrumentEdit);
        Bind(wxEVT_MENU, &MainFrame::OnSampleDelete, this, ID_SampleDelete);
        m_sampleTree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &MainFrame::OnInstrumentEditDblClick, this);
        Bind(wxEVT_MENU, &MainFrame::OnLoadBank, this, ID_LoadBank);
        Bind(wxEVT_MENU, &MainFrame::OnReloadInternalBank, this, ID_ReloadInternalBank);
        Bind(wxEVT_MENU, &MainFrame::OnCompressInstrument, this, ID_CompressInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnCompressAllInstruments, this, ID_CompressAllInstruments);
        Bind(wxEVT_MENU, &MainFrame::OnCloneFromBank, this, ID_CloneFromBank);
        Bind(wxEVT_MENU, &MainFrame::OnAliasFromBank, this, ID_AliasFromBank);
        Bind(wxEVT_TIMER, &MainFrame::OnPlaybackTimer, this, ID_PlaybackTimer);
        Bind(wxEVT_TIMER, &MainFrame::OnNotePreviewTimer, this, ID_NotePreviewTimer);
        LoadIniSettings();
        RefreshMidiLoopControlsFromDocument();
        UpdateUndoMenuState();

    }

    ~MainFrame() override {
        SaveIniSettings();
        if (m_document) {
            BAERmfEditorDocument_Delete(m_document);
            m_document = nullptr;
        }
        StopPlayback(true);
        StopPianoRollPreview(true);
        if (m_previewSound)
        {
            BAESound_Stop(m_previewSound, FALSE);
            BAESound_Delete(m_previewSound);
            m_previewSound = nullptr;
        }
        ShutdownPlaybackEngine();
    }

    void OpenPathFromCli(wxString const &path) {
        wxString ext;

        if (path.empty()) {
            return;
        }
        ext = wxFileName(path).GetExt().Lower();
        if (ext == "nbs") {
            LoadSession(path);
        } else {
            LoadDocument(path);
        }
    }

private:
    BAERmfEditorDocument *m_document;
    bool m_updatingControls;
    wxString m_currentPath;
    wxString m_sessionPath;
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
    wxChoice *m_midiStorageChoice;
    wxSlider *m_previewVolumeSlider;
    wxChoice *m_previewReverbChoice;
    wxCheckBox *m_previewLoopCheck;
    wxCheckBox *m_midiLoopEnableCheck;
    wxTextCtrl *m_midiLoopStartText;
    wxTextCtrl *m_midiLoopEndText;
    wxSlider *m_positionSlider;
    wxStaticText *m_positionLabel;
    wxTreeCtrl *m_sampleTree;
    wxButton *m_sampleAddButton;
    BAEMixer m_playbackMixer;
    BAESong m_playbackSong;
    std::vector<unsigned char> m_playbackSongBlob;
    BAESong m_notePreviewSong;
    std::vector<unsigned char> m_notePreviewSongBlob;
    BAESound m_previewSound;
    wxString m_playbackTempPath;
    wxString m_previewSampleTempPath;
    wxTimer m_playbackTimer;
    wxTimer m_notePreviewTimer;
    bool m_notePreviewActive;
    unsigned char m_notePreviewChannel;
    unsigned char m_notePreviewNote;
    bool m_ignoreSeekEvent;
    bool m_autoFollowPlayhead;
    BAEBankToken m_bankToken;
    bool m_bankLoaded;
    std::vector<BAEBankToken> m_bankTokens; /* all loaded banks for enumeration */
    bool m_pendingLoopHotReload;
    uint32_t m_pendingLoopHotReloadPosUsec;
    bool m_pendingLoopHotReloadPaused;
    bool m_loadingLoopControls;
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;
    UndoDocumentState m_pendingUndoState;
    wxString m_pendingUndoLabel;
    bool m_hasPendingUndo;
    bool m_restoringUndo;

    static wxString GetIniPath() {
        return wxFileName::GetHomeDir() + "/.nbstudio.ini";
    }

    BAEReverbType GetSelectedPreviewReverbType() const {
        int selection;

        selection = m_previewReverbChoice ? m_previewReverbChoice->GetSelection() : 0;
        selection = std::clamp(selection, 0, 10);
        return static_cast<BAEReverbType>(static_cast<int>(BAE_REVERB_TYPE_1) + selection);
    }

    void SetSelectedPreviewReverbType(BAEReverbType reverbType) {
        int selection;

        selection = static_cast<int>(reverbType) - static_cast<int>(BAE_REVERB_TYPE_1);
        selection = std::clamp(selection, 0, 10);
        if (m_previewReverbChoice) {
            m_previewReverbChoice->SetSelection(selection);
        }
    }

    void ApplyPreviewReverbToMixer() {
        if (m_playbackMixer) {
            BAEMixer_SetDefaultReverb(m_playbackMixer, GetSelectedPreviewReverbType());
        }
    }

    bool IsPreviewLoopEnabled() const {
        return m_previewLoopCheck ? m_previewLoopCheck->GetValue() : false;
    }

    void ApplyPreviewLoopSettingToSong() {
        if (m_playbackSong) {
            BAESong_SetLoops(m_playbackSong, IsPreviewLoopEnabled() ? 32767 : 0);
        }
    }

    void RefreshMidiLoopControlsFromDocument() {
        XBOOL enabled;
        uint32_t startTick;
        uint32_t endTick;
        int32_t loopCount;

        m_loadingLoopControls = true;
        if (!m_document ||
            BAERmfEditorDocument_GetMidiLoopMarkers(m_document,
                                                    &enabled,
                                                    &startTick,
                                                    &endTick,
                                                    &loopCount) != BAE_NO_ERROR) {
            if (m_midiLoopEnableCheck) m_midiLoopEnableCheck->SetValue(false);
            if (m_midiLoopStartText) m_midiLoopStartText->SetValue("0:00.000");
            if (m_midiLoopEndText) m_midiLoopEndText->SetValue("0:00.000");
        } else {
            if (m_midiLoopEnableCheck) m_midiLoopEnableCheck->SetValue(enabled != FALSE);
            if (m_midiLoopStartText) m_midiLoopStartText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(startTick)));
            if (m_midiLoopEndText) {
                uint32_t displayEnd = endTick;
                if (displayEnd == 0 && startTick > 0) {
                    displayEnd = startTick + 1;
                }
                m_midiLoopEndText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(displayEnd)));
            }
        }
        if (m_midiLoopStartText) m_midiLoopStartText->Enable(m_midiLoopEnableCheck && m_midiLoopEnableCheck->GetValue());
        if (m_midiLoopEndText) m_midiLoopEndText->Enable(m_midiLoopEnableCheck && m_midiLoopEnableCheck->GetValue());
        m_loadingLoopControls = false;
    }

    void LoadIniSettings() {
        wxFileConfig config(wxEmptyString,
                            wxEmptyString,
                            GetIniPath(),
                            wxEmptyString,
                            wxCONFIG_USE_LOCAL_FILE);
        long previewVolume;
        long previewReverb;
        bool previewLoop;
        long midiStorage;

        previewVolume = 100;
        if (config.Read("audio/preview_volume", &previewVolume) && m_previewVolumeSlider) {
            m_previewVolumeSlider->SetValue(std::clamp<long>(previewVolume, 0, 100));
        }
        previewReverb = static_cast<long>(BAE_REVERB_TYPE_1);
        if (config.Read("audio/preview_reverb", &previewReverb)) {
            SetSelectedPreviewReverbType(static_cast<BAEReverbType>(previewReverb));
        } else {
            SetSelectedPreviewReverbType(BAE_REVERB_TYPE_1);
        }
        previewLoop = false;
        if (config.Read("audio/preview_loop", &previewLoop) && m_previewLoopCheck) {
            m_previewLoopCheck->SetValue(previewLoop);
        }
        midiStorage = 1;
        if (config.Read("rmf/midi_storage_mode", &midiStorage) && m_midiStorageChoice) {
            m_midiStorageChoice->SetSelection(std::clamp<long>(midiStorage, 0, 3));
        }
    }

    void SaveIniSettings() {
        wxFileConfig config(wxEmptyString,
                            wxEmptyString,
                            GetIniPath(),
                            wxEmptyString,
                            wxCONFIG_USE_LOCAL_FILE);

        config.Write("audio/preview_volume", static_cast<long>(m_previewVolumeSlider ? m_previewVolumeSlider->GetValue() : 100));
        config.Write("audio/preview_reverb", static_cast<long>(GetSelectedPreviewReverbType()));
        config.Write("audio/preview_loop", IsPreviewLoopEnabled());
        config.Write("rmf/midi_storage_mode", static_cast<long>(m_midiStorageChoice ? m_midiStorageChoice->GetSelection() : 1));
        config.Flush();
    }

    BAERmfEditorMidiStorageType GetSelectedMidiStorageType() const {
        int selection;

        selection = m_midiStorageChoice ? m_midiStorageChoice->GetSelection() : 1;
        switch (selection) {
            case 0:
                return BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT;
            case 2:
                return BAE_EDITOR_MIDI_STORAGE_EMID;
            case 3:
                return BAE_EDITOR_MIDI_STORAGE_MIDI;
            case 1:
            default:
                return BAE_EDITOR_MIDI_STORAGE_ECMI;
        }
    }

    void SetSelectedMidiStorageType(BAERmfEditorMidiStorageType storageType) {
        int selection;

        selection = 1;
        switch (storageType) {
            case BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT:
                selection = 0;
                break;
            case BAE_EDITOR_MIDI_STORAGE_EMID:
                selection = 2;
                break;
            case BAE_EDITOR_MIDI_STORAGE_MIDI:
                selection = 3;
                break;
            case BAE_EDITOR_MIDI_STORAGE_ECMI:
            default:
                selection = 1;
                break;
        }
        if (m_midiStorageChoice) {
            m_midiStorageChoice->SetSelection(selection);
        }
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
        InvalidatePianoRollPreviewSong();
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

    bool CaptureSerializedDocument(std::vector<unsigned char> *outBytes) const {
        wxString tempPath;
        wxScopedCharBuffer utf8Path;
        wxFile file;
        wxFileOffset length;

        if (!outBytes || !m_document) {
            return false;
        }

        tempPath = wxFileName::CreateTempFileName("nbstudio_undo");
        if (tempPath.empty()) {
            return false;
        }

        utf8Path = tempPath.utf8_str();
        if (BAERmfEditorDocument_SaveAsRmf(m_document, const_cast<char *>(utf8Path.data())) != BAE_NO_ERROR) {
            wxRemoveFile(tempPath);
            return false;
        }

        if (!file.Open(tempPath, wxFile::read)) {
            wxRemoveFile(tempPath);
            return false;
        }
        length = file.Length();
        if (length <= 0) {
            file.Close();
            wxRemoveFile(tempPath);
            return false;
        }

        outBytes->assign(static_cast<size_t>(length), 0);
        if (file.Read(outBytes->data(), static_cast<size_t>(length)) != length) {
            outBytes->clear();
            file.Close();
            wxRemoveFile(tempPath);
            return false;
        }

        file.Close();
        wxRemoveFile(tempPath);
        return true;
    }

    bool RestoreSerializedDocument(std::vector<unsigned char> const &bytes) {
        wxString tempPath;
        wxScopedCharBuffer utf8Path;
        wxFile file;
        BAERmfEditorDocument *loadedDocument;

        if (bytes.empty()) {
            return false;
        }

        tempPath = wxFileName::CreateTempFileName("nbstudio_undo");
        if (tempPath.empty()) {
            return false;
        }

        if (!file.Open(tempPath, wxFile::write)) {
            wxRemoveFile(tempPath);
            return false;
        }
        if (file.Write(bytes.data(), bytes.size()) != (wxFileOffset)bytes.size()) {
            file.Close();
            wxRemoveFile(tempPath);
            return false;
        }
        file.Close();

        utf8Path = tempPath.utf8_str();
        loadedDocument = BAERmfEditorDocument_LoadFromFile(const_cast<char *>(utf8Path.data()));
        wxRemoveFile(tempPath);
        if (!loadedDocument) {
            return false;
        }

        if (m_document) {
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = loadedDocument;
        /* Keep the piano roll's document pointer in sync so it doesn't
         * access the freed document the next time it redraws or receives
         * a SetSelectedTrack call. */
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        return true;
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
        outState->serializedDocument.clear();

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
        if (!CaptureSerializedDocument(&outState->serializedDocument)) {
            outState->serializedDocument.clear();
        }
        return true;
    }

    bool RestoreUndoState(UndoDocumentState const &state) {
        uint16_t trackCount;

        if (!m_document) {
            return false;
        }
        if (!state.serializedDocument.empty()) {
            return RestoreSerializedDocument(state.serializedDocument);
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
        {
            int savedTrack = GetSelectedTrack();
            PopulateTrackList();
            if (savedTrack >= 0 && savedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetSelection(savedTrack);
                PianoRollPanel_SetSelectedTrack(m_pianoRoll, savedTrack);
            }
        }
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, true);
        InvalidatePianoRollPreviewSong();
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
        {
            int savedTrack = GetSelectedTrack();
            PopulateTrackList();
            if (savedTrack >= 0 && savedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetSelection(savedTrack);
                PianoRollPanel_SetSelectedTrack(m_pianoRoll, savedTrack);
            }
        }
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, true);
        InvalidatePianoRollPreviewSong();
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
        ApplyPreviewReverbToMixer();
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
                m_bankTokens.push_back(m_bankToken);
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
        /* Unload existing banks before loading new one */
        StopPlayback(true);
        BAEMixer_UnloadBanks(m_playbackMixer);
        m_bankToken = nullptr;
        m_bankLoaded = false;
        m_bankTokens.clear();
        /* Reload internal bank first so it remains available */
#ifdef _BUILT_IN_PATCHES
        {
            BAEBankToken builtinToken;
            if (BAEMixer_LoadBuiltinBank(m_playbackMixer, &builtinToken) == BAE_NO_ERROR) {
                m_bankTokens.push_back(builtinToken);
            }
        }
#endif
        utf8 = path.utf8_str();
        if (BAEMixer_AddBankFromFile(m_playbackMixer, const_cast<char *>(utf8.data()), &newToken) != BAE_NO_ERROR) {
            /* Restore internal bank state even if external load fails */
            if (!m_bankTokens.empty()) {
                m_bankToken = m_bankTokens[0];
                m_bankLoaded = true;
            }
            return false;
        }
        m_bankToken = newToken;
        m_bankLoaded = true;
        m_bankTokens.push_back(newToken);
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
        uint32_t assetCount;
        wxTreeItemId root;

        m_sampleTree->DeleteAllItems();
        root = m_sampleTree->AddRoot("Sample Assets");
        if (!m_document) {
            return;
        }
        assetCount = 0;
        if (BAERmfEditorDocument_GetSampleAssetCount(m_document, &assetCount) != BAE_NO_ERROR) {
            return;
        }
        for (uint32_t assetIndex = 0; assetIndex < assetCount; ++assetIndex) {
            BAERmfEditorSampleAssetInfo assetInfo;
            wxTreeItemId assetNode;
            wxString assetName;
            wxString assetLabel;

            if (BAERmfEditorDocument_GetSampleAssetInfo(m_document, assetIndex, &assetInfo) != BAE_NO_ERROR) {
                continue;
            }
            assetName = assetInfo.displayName ? assetInfo.displayName : "(unnamed sample)";
            assetLabel = wxString::Format("A%u: %s (%u uses)",
                                          static_cast<unsigned>(assetInfo.assetID),
                                          assetName,
                                          static_cast<unsigned>(assetInfo.usageCount));
            assetNode = m_sampleTree->AppendItem(root,
                                                 assetLabel,
                                                 -1,
                                                 -1,
                                                 new SampleTreeItemData(assetInfo.assetID,
                                                                        static_cast<uint32_t>(-1),
                                                                        true));

            for (uint32_t usageIndex = 0; usageIndex < assetInfo.usageCount; ++usageIndex) {
                uint32_t sampleIndex;
                BAERmfEditorSampleInfo sampleInfo;
                uint32_t instID;
                BAERmfEditorInstrumentExtInfo instInfo;
                wxString usageLabel;
                wxString instName;

                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                                   assetInfo.assetID,
                                                                   usageIndex,
                                                                   &sampleIndex) != BAE_NO_ERROR) {
                    continue;
                }
                if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
                    continue;
                }

                instName = sampleInfo.displayName ? sampleInfo.displayName : "(unnamed instrument)";
                if (BAERmfEditorDocument_GetInstIDForSample(m_document, sampleIndex, &instID) == BAE_NO_ERROR &&
                    BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &instInfo) == BAE_NO_ERROR &&
                    instInfo.displayName && instInfo.displayName[0]) {
                    instName = instInfo.displayName;
                }

                {
                    XBOOL isAlias = FALSE;
                    BAERmfEditorDocument_IsSampleBankAlias(m_document, sampleIndex, &isAlias);
                    usageLabel = wxString::Format("%sP%u %s [%u-%u] rk=%u",
                                                  isAlias ? "[Alias] " : "",
                                                  static_cast<unsigned>(sampleInfo.program),
                                                  instName,
                                                  static_cast<unsigned>(sampleInfo.lowKey),
                                                  static_cast<unsigned>(sampleInfo.highKey),
                                                  static_cast<unsigned>(sampleInfo.rootKey));
                }
                m_sampleTree->AppendItem(assetNode,
                                         usageLabel,
                                         -1,
                                         -1,
                                         new SampleTreeItemData(assetInfo.assetID,
                                                                sampleIndex,
                                                                false));
            }
            m_sampleTree->Expand(assetNode);
        }
        m_sampleTree->Expand(root);
    }

    SampleTreeItemData *GetSelectedSampleTreeData() const {
        wxTreeItemId selected;

        selected = m_sampleTree->GetSelection();
        if (!selected.IsOk()) {
            return nullptr;
        }
        return dynamic_cast<SampleTreeItemData *>(m_sampleTree->GetItemData(selected));
    }

    bool GetSelectedSampleIndexFromTree(uint32_t *outSampleIndex) const {
        SampleTreeItemData *data;

        if (!outSampleIndex) {
            return false;
        }
        data = GetSelectedSampleTreeData();
        if (!data) {
            return false;
        }

        if (!data->IsAssetNode()) {
            *outSampleIndex = data->GetSampleIndex();
            return true;
        }

        {
            uint32_t sampleIndex;
            if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                               data->GetAssetID(),
                                                               0,
                                                               &sampleIndex) == BAE_NO_ERROR) {
                *outSampleIndex = sampleIndex;
                return true;
            }
        }
        return false;
    }

    void SelectTreeItemForSample(uint32_t sampleIndex) {
        wxTreeItemId root;
        wxTreeItemIdValue assetCookie;
        wxTreeItemId assetNode;

        root = m_sampleTree->GetRootItem();
        if (!root.IsOk()) {
            return;
        }

        assetNode = m_sampleTree->GetFirstChild(root, assetCookie);
        while (assetNode.IsOk()) {
            wxTreeItemIdValue usageCookie;
            wxTreeItemId usageNode = m_sampleTree->GetFirstChild(assetNode, usageCookie);
            while (usageNode.IsOk()) {
                SampleTreeItemData *data;

                data = dynamic_cast<SampleTreeItemData *>(m_sampleTree->GetItemData(usageNode));
                if (data && !data->IsAssetNode() && data->GetSampleIndex() == sampleIndex) {
                    m_sampleTree->SelectItem(usageNode);
                    return;
                }
                usageNode = m_sampleTree->GetNextChild(assetNode, usageCookie);
            }
            assetNode = m_sampleTree->GetNextChild(root, assetCookie);
        }
    }

    bool PreviewSampleAtKey(uint32_t sampleIndex,
                            int midiKey,
                            BAESampleInfo const *overrideInfo = nullptr,
                            int16_t splitVolumeOverride = 0,
                            unsigned char rootKeyOverride = 0xFF,
                            BAERmfEditorCompressionType compressionOverride = BAE_EDITOR_COMPRESSION_DONT_CHANGE) {
        BAERmfEditorSampleInfo sampleInfo;
        BAEResult loadResult;
        BAEResult startResult;
        BAEResult rateResult;
        BAEResult stopResult;
        BAE_UNSIGNED_FIXED baseSampleRate;
        int previewRoot;
        int semitones;
        int loadRateOctaveShift;
        constexpr double kMinRateFixed = static_cast<double>(4000U << 16);
        constexpr double kMaxRateFixed = static_cast<double>(0xFFFF0000u);

        if (!m_document) {
            return false;
        }
        if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
            return false;
        }

        previewRoot = static_cast<int>(NormalizeRootKeyForSingleKeySplit(sampleInfo.rootKey,
                                                                          sampleInfo.lowKey,
                                                                          sampleInfo.highKey));
        if (rootKeyOverride <= 127) {
            previewRoot = static_cast<int>(rootKeyOverride);
        }
        if (previewRoot < 0 || previewRoot > 127) {
            previewRoot = static_cast<int>((overrideInfo ? overrideInfo->baseMidiPitch : sampleInfo.sampleInfo.baseMidiPitch) & 0x7F);
        }
        semitones = midiKey - previewRoot;
        loadRateOctaveShift = 0;

        baseSampleRate = sampleInfo.sampleInfo.sampledRate;
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

        if (overrideInfo &&
            compressionOverride != BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
            compressionOverride != BAE_EDITOR_COMPRESSION_PCM) {
            wxString compressedPath;

            if (BuildCompressedPreviewSampleFile(sampleIndex, compressionOverride, &compressedPath)) {
                wxScopedCharBuffer utf8CompressedPath = compressedPath.utf8_str();
                BAEFileType compressedType = X_DetermineFileTypeByPath(utf8CompressedPath.data());
                if (compressedType == BAE_INVALID_TYPE) {
                    compressedType = X_DetermineFileType(utf8CompressedPath.data());
                }
                if (compressedType != BAE_INVALID_TYPE) {
                    loadResult = BAESound_LoadFileSample(m_previewSound,
                                                         const_cast<char *>(utf8CompressedPath.data()),
                                                         compressedType);
                }
                if (wxFileExists(compressedPath)) {
                    wxRemoveFile(compressedPath);
                }
            }
        }

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

        auto normalizePreviewRate = [](BAE_UNSIGNED_FIXED rate) -> BAE_UNSIGNED_FIXED {
            if (rate == 0) {
                return (44100U << 16);
            }
            if (rate >= 4000U && rate <= 384000U) {
                return (rate << 16);
            }
            return rate;
        };

        baseSampleRate = normalizePreviewRate(baseSampleRate);

        auto adaptLoadRateForPitch = [&](BAE_UNSIGNED_FIXED &ioRate,
                                         int &outOctaveShift) {
            double desiredRate;
            int maxSemitones;
            int octaveShift;
            BAE_UNSIGNED_FIXED shiftedRate;

            if (ioRate == 0) {
                outOctaveShift = 0;
                return;
            }

            maxSemitones = std::max(0, 127 - previewRoot);
            desiredRate = static_cast<double>(ioRate) * std::pow(2.0, static_cast<double>(maxSemitones) / 12.0);
            shiftedRate = ioRate;
            octaveShift = 0;

            while (desiredRate > kMaxRateFixed && shiftedRate > (8000U << 16)) {
                shiftedRate >>= 1;
                desiredRate *= 0.5;
                ++octaveShift;
            }
            if (shiftedRate < (4000U << 16)) {
                shiftedRate = (4000U << 16);
            }

            ioRate = shiftedRate;
            outOctaveShift = octaveShift;
        };

          /* When the caller provides live loop overrides and compressed preview is
              not requested, use raw PCM so loop settings from the UI are honoured. */
          if (loadResult != BAE_NO_ERROR && overrideInfo) {
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
                if (overrideInfo->sampledRate > 0) {
                    sampleRate = overrideInfo->sampledRate;
                }
                sampleRate = normalizePreviewRate(sampleRate);
                loopStart = overrideInfo->startLoop;
                loopEnd   = overrideInfo->endLoop;
                if (loopStart >= frameCount || loopEnd > frameCount || loopEnd <= loopStart) {
                    loopStart = 0;
                    loopEnd   = 0;
                }
                adaptLoadRateForPitch(sampleRate, loadRateOctaveShift);
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
            if (overrideInfo && overrideInfo->sampledRate > 0) {
                sampleRate = overrideInfo->sampledRate;
            }
            sampleRate = normalizePreviewRate(sampleRate);
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
            adaptLoadRateForPitch(sampleRate, loadRateOctaveShift);
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
            uint32_t loopStart;
            uint32_t loopEnd;
            uint32_t sourceFrames;
            uint32_t targetFrames;

            loopStart = overrideInfo ? overrideInfo->startLoop : sampleInfo.sampleInfo.startLoop;
            loopEnd = overrideInfo ? overrideInfo->endLoop : sampleInfo.sampleInfo.endLoop;
            sourceFrames = overrideInfo && overrideInfo->waveFrames > 0
                ? overrideInfo->waveFrames
                : sampleInfo.sampleInfo.waveFrames;
            targetFrames = 0;
            if (BAESound_GetInfo(m_previewSound, &loadedInfo) == BAE_NO_ERROR) {
                if (loadedInfo.sampledRate >= (4000U << 16)) {
                    baseSampleRate = loadedInfo.sampledRate;
                }
                targetFrames = loadedInfo.waveFrames;
                fprintf(stderr,
                        "[nbstudio] Preview sample %u loaded info: frames=%u bits=%u channels=%u rate=%lu basePitch=%u\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<unsigned>(loadedInfo.waveFrames),
                        static_cast<unsigned>(loadedInfo.bitSize),
                        static_cast<unsigned>(loadedInfo.channels),
                        static_cast<unsigned long>(loadedInfo.sampledRate),
                        static_cast<unsigned>(loadedInfo.baseMidiPitch));
            }

            if (sourceFrames == 0) {
                sourceFrames = sampleInfo.sampleInfo.waveFrames;
            }
            if (targetFrames == 0) {
                targetFrames = sourceFrames;
            }
            if (loopEnd > loopStart && sourceFrames > 0 && targetFrames > 0) {
                if (sourceFrames != targetFrames) {
                    uint32_t mappedStart;
                    uint32_t mappedEnd;
                    bool isAdpcmOverride;

                    isAdpcmOverride = compressionOverride == BAE_EDITOR_COMPRESSION_ADPCM;

                    if (isAdpcmOverride)
                    {
                        /* Keep ADPCM preview loops in source-frame coordinates. */
                        mappedStart = loopStart;
                        mappedEnd = loopEnd;
                    }
                    else
                    {
                        mappedStart = static_cast<uint32_t>((((uint64_t)loopStart * (uint64_t)targetFrames) +
                                                             ((uint64_t)sourceFrames / 2ULL)) /
                                                            (uint64_t)sourceFrames);
                        mappedEnd = static_cast<uint32_t>((((uint64_t)loopEnd * (uint64_t)targetFrames) +
                                                           ((uint64_t)sourceFrames / 2ULL)) /
                                                          (uint64_t)sourceFrames);
                    }
                    if (mappedStart > targetFrames) mappedStart = targetFrames;
                    if (mappedEnd > targetFrames) mappedEnd = targetFrames;
                    if (mappedEnd <= mappedStart) {
                        if (mappedStart < targetFrames) {
                            mappedEnd = mappedStart + 1;
                        } else if (targetFrames > 0) {
                            mappedStart = targetFrames - 1;
                            mappedEnd = targetFrames;
                        } else {
                            mappedStart = 0;
                            mappedEnd = 0;
                        }
                    }
                    loopStart = mappedStart;
                    loopEnd = mappedEnd;
                }
                if (loopStart < targetFrames && loopEnd <= targetFrames && loopEnd > loopStart) {
                    BAESound_SetSampleLoopPoints(m_previewSound, loopStart, loopEnd);
                    BAESound_SetLoopCount(m_previewSound, 0xFFFFFFFFu);
                } else {
                    BAESound_SetLoopCount(m_previewSound, 0);
                }
            } else {
                BAESound_SetLoopCount(m_previewSound, 0);
            }
        }

        /* Check whether this instrument has playAtSampledFreq set.  When the
         * flag is active the engine plays every note at the sample's native
         * rate with no pitch transposition, so the preview must do the same. */
        bool playAtSampledFreq = false;
        {
            uint32_t instID = 0;
            BAERmfEditorInstrumentExtInfo extInfo;
            if (BAERmfEditorDocument_GetInstIDForSample(m_document, sampleIndex, &instID) == BAE_NO_ERROR) {
                memset(&extInfo, 0, sizeof(extInfo));
                if (BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &extInfo) == BAE_NO_ERROR) {
                    playAtSampledFreq = (extInfo.flags2 & 0x40) != 0; /* ZBF_playAtSampledFreq */
                }
            }
        }

        previewRoot = static_cast<int>(NormalizeRootKeyForSingleKeySplit(sampleInfo.rootKey,
                                                                          sampleInfo.lowKey,
                                                                          sampleInfo.highKey));
        if (rootKeyOverride <= 127) {
            previewRoot = static_cast<int>(rootKeyOverride);
        }
        if (previewRoot < 0 || previewRoot > 127) {
            previewRoot = static_cast<int>((overrideInfo ? overrideInfo->baseMidiPitch : sampleInfo.sampleInfo.baseMidiPitch) & 0x7F);
        }
        BAE_UNSIGNED_FIXED rate;
        if (playAtSampledFreq) {
            /* Play at the sample's native rate with no transposition, matching
             * the engine's behaviour (GenSynth: NotePitch = 1.0 * sampleRate/22050).
             * Use the document's sample rate directly — baseSampleRate may have
             * been halved by adaptLoadRateForPitch which we must not honour here. */
            BAE_UNSIGNED_FIXED nativeRate = sampleInfo.sampleInfo.sampledRate;
            if (nativeRate < (4000U << 16)) {
                if (nativeRate >= 4000U && nativeRate <= 384000U) {
                    nativeRate <<= 16;
                } else {
                    nativeRate = (44100U << 16);
                }
            }
            rate = nativeRate;
        } else {
            semitones = midiKey - previewRoot + loadRateOctaveShift * 12;
            double ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);
            rate = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(static_cast<double>(baseSampleRate) * ratio,
                                 kMinRateFixed,
                                 kMaxRateFixed));
        }
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

    static unsigned char RawMidiBankFromInternal(uint16_t internalBank) {
        if (internalBank >= 128) {
            return static_cast<unsigned char>((internalBank >> 7) & 0x7F);
        }
        return static_cast<unsigned char>(internalBank & 0x7F);
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

    void InvalidatePianoRollPreviewSong() {
        StopPianoRollPreview(true);
    }

    bool EnsurePianoRollPreviewSong() {
        BAEResult loadResult;
        BAELoadResult loadInfo;

        if (m_notePreviewSong) {
            return true;
        }
        if (!m_document) {
            return false;
        }
        if (!EnsurePlaybackEngine()) {
            return false;
        }
        if (!BuildPreviewPlaybackBlob(m_document, &m_notePreviewSongBlob)) {
            return false;
        }
        memset(&loadInfo, 0, sizeof(loadInfo));
        loadResult = BAEMixer_LoadFromMemory(m_playbackMixer,
                                             m_notePreviewSongBlob.data(),
                                             static_cast<uint32_t>(m_notePreviewSongBlob.size()),
                                             &loadInfo);
        if (loadResult != BAE_NO_ERROR || loadInfo.type != BAE_LOAD_TYPE_SONG || !loadInfo.data.song) {
            BAELoadResult_Cleanup(&loadInfo);
            m_notePreviewSongBlob.clear();
            return false;
        }
        m_notePreviewSong = loadInfo.data.song;
        loadInfo.type = BAE_LOAD_TYPE_NONE;
        loadInfo.data.song = nullptr;
        BAESong_Preroll(m_notePreviewSong);
        BAESong_SetVolume(m_notePreviewSong, GetPreviewVolumeFixed());
        BAESong_SetLoops(m_notePreviewSong, 0);
        return true;
    }

    void StopPianoRollPreview(bool releaseSong) {
        if (m_notePreviewTimer.IsRunning()) {
            m_notePreviewTimer.Stop();
        }
        if (m_notePreviewSong && m_notePreviewActive) {
            BAESong_NoteOff(m_notePreviewSong,
                            m_notePreviewChannel,
                            m_notePreviewNote,
                            0,
                            0);
            BAESong_Stop(m_notePreviewSong, FALSE);
        }
        m_notePreviewActive = false;
        if (releaseSong && m_notePreviewSong) {
            BAESong_AllNotesOff(m_notePreviewSong, 0);
            BAESong_Stop(m_notePreviewSong, FALSE);
            BAESong_Delete(m_notePreviewSong);
            m_notePreviewSong = nullptr;
            m_notePreviewSongBlob.clear();
        }
    }

    void PreviewPianoRollNote(uint16_t bank,
                              unsigned char program,
                              unsigned char note,
                              uint32_t durationTicks,
                              int trackIndex) {
        BAERmfEditorTrackInfo trackInfo;
        uint64_t noteDurationUsec;
        uint32_t noteDurationMs;
        uint32_t songLengthUsec;
        BAEResult startResult;
        BAEResult programResult;
        unsigned char channel;

        if (!m_document) {
            return;
        }
        if (trackIndex < 0 || BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(trackIndex), &trackInfo) != BAE_NO_ERROR) {
            return;
        }
        if (!EnsurePianoRollPreviewSong()) {
            return;
        }

        channel = trackInfo.channel;
        StopPianoRollPreview(false);

        /* Start from the song end so tick-0 events do not fire during manual note preview. */
        songLengthUsec = 0;
        if (BAESong_GetMicrosecondLength(m_notePreviewSong, &songLengthUsec) == BAE_NO_ERROR && songLengthUsec > 0) {
            BAESong_SetMicrosecondPosition(m_notePreviewSong, songLengthUsec - 1);
        }

        /* Keep preview simple and deterministic: run the preview song only while auditioning. */
        BAESong_Preroll(m_notePreviewSong);
        if (BAESong_Start(m_notePreviewSong, 0) != BAE_NO_ERROR) {
            return;
        }
        if (songLengthUsec > 0) {
            BAESong_SetMicrosecondPosition(m_notePreviewSong, songLengthUsec - 1);
        }

        /* Ensure the preview channel is audible regardless of prior track automation state. */
        BAESong_UnmuteChannel(m_notePreviewSong, channel);
        BAESong_ControlChange(m_notePreviewSong, channel, 7, 127, 0);
        BAESong_ControlChange(m_notePreviewSong, channel, 11, 127, 0);

        programResult = BAESong_ProgramBankChange(m_notePreviewSong,
                                                  channel,
                                                  program,
                                                  RawMidiBankFromInternal(bank),
                                                  0);
        if (programResult != BAE_NO_ERROR) {
            return;
        }
        startResult = BAESong_NoteOnWithLoad(m_notePreviewSong, channel, note, 100, 0);
        if (startResult != BAE_NO_ERROR) {
            return;
        }

        m_notePreviewActive = true;
        m_notePreviewChannel = channel;
        m_notePreviewNote = note;

        noteDurationUsec = TicksToMicroseconds(std::max<uint32_t>(durationTicks, 1));
        noteDurationMs = static_cast<uint32_t>(std::clamp<uint64_t>((noteDurationUsec + 999ULL) / 1000ULL, 40ULL, 15000ULL));
        m_notePreviewTimer.StartOnce(static_cast<int>(noteDurationMs));
    }

    void OnNotePreviewTimer(wxTimerEvent &) {
        StopPianoRollPreview(false);
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
        StopPianoRollPreview(true);
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

    bool BuildCompressedPreviewSampleFile(uint32_t sampleIndex,
                                          BAERmfEditorCompressionType compressionType,
                                          wxString *outPath) {
        wxString docBasePath;
        wxString docPath;
        wxString sampleBasePath;
        wxString samplePath;
        wxScopedCharBuffer utf8DocPath;
        wxScopedCharBuffer utf8SamplePath;
        BAERmfEditorDocument *tempDoc;
        BAERmfEditorSampleInfo info;
        std::string displayName;
        BAEResult result;

        if (!m_document || !outPath) {
            return false;
        }
        if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE ||
            compressionType == BAE_EDITOR_COMPRESSION_PCM) {
            return false;
        }

        docBasePath = wxFileName::CreateTempFileName("nbstudio_cmp_doc");
        if (docBasePath.empty()) {
            return false;
        }
        docPath = docBasePath + ".rmf";
        utf8DocPath = docPath.utf8_str();

        if (BAERmfEditorDocument_SaveAsRmf(m_document, const_cast<char *>(utf8DocPath.data())) != BAE_NO_ERROR) {
            if (wxFileExists(docPath)) wxRemoveFile(docPath);
            if (wxFileExists(docBasePath)) wxRemoveFile(docBasePath);
            return false;
        }

        tempDoc = BAERmfEditorDocument_LoadFromFile(const_cast<char *>(utf8DocPath.data()));
        if (wxFileExists(docPath)) wxRemoveFile(docPath);
        if (wxFileExists(docBasePath)) wxRemoveFile(docBasePath);
        if (!tempDoc) {
            return false;
        }

        if (BAERmfEditorDocument_GetSampleInfo(tempDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        displayName = info.displayName ? info.displayName : "";
        info.displayName = const_cast<char *>(displayName.c_str());
        info.compressionType = compressionType;
        if (BAERmfEditorDocument_SetSampleInfo(tempDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        sampleBasePath = wxFileName::CreateTempFileName("nbstudio_cmp_sample");
        if (sampleBasePath.empty()) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        switch (compressionType) {
            case BAE_EDITOR_COMPRESSION_MP3_32K:
            case BAE_EDITOR_COMPRESSION_MP3_48K:
            case BAE_EDITOR_COMPRESSION_MP3_64K:
            case BAE_EDITOR_COMPRESSION_MP3_96K:
            case BAE_EDITOR_COMPRESSION_MP3_128K:
            case BAE_EDITOR_COMPRESSION_MP3_192K:
            case BAE_EDITOR_COMPRESSION_MP3_256K:
            case BAE_EDITOR_COMPRESSION_MP3_320K:
                samplePath = sampleBasePath + ".mp3";
                break;
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:
            case BAE_EDITOR_COMPRESSION_VORBIS_48K:
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:
            case BAE_EDITOR_COMPRESSION_VORBIS_80K:
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:
            case BAE_EDITOR_COMPRESSION_VORBIS_128K:
            case BAE_EDITOR_COMPRESSION_VORBIS_160K:
            case BAE_EDITOR_COMPRESSION_VORBIS_192K:
            case BAE_EDITOR_COMPRESSION_VORBIS_256K:
                samplePath = sampleBasePath + ".ogg";
                break;
            case BAE_EDITOR_COMPRESSION_OPUS_12K:
            case BAE_EDITOR_COMPRESSION_OPUS_16K:
            case BAE_EDITOR_COMPRESSION_OPUS_24K:
            case BAE_EDITOR_COMPRESSION_OPUS_32K:
            case BAE_EDITOR_COMPRESSION_OPUS_48K:
            case BAE_EDITOR_COMPRESSION_OPUS_64K:
            case BAE_EDITOR_COMPRESSION_OPUS_96K:
            case BAE_EDITOR_COMPRESSION_OPUS_128K:
            case BAE_EDITOR_COMPRESSION_OPUS_256K:
                samplePath = sampleBasePath + ".opus";
                break;
            case BAE_EDITOR_COMPRESSION_FLAC:
                samplePath = sampleBasePath + ".flac";
                break;
            case BAE_EDITOR_COMPRESSION_ADPCM:
                samplePath = sampleBasePath + ".aif";
                break;
            default:
                samplePath = sampleBasePath + ".wav";
                break;
        }

        utf8SamplePath = samplePath.utf8_str();
        result = BAERmfEditorDocument_ExportSampleToFile(tempDoc,
                                                         sampleIndex,
                                                         const_cast<char *>(utf8SamplePath.data()));
        BAERmfEditorDocument_Delete(tempDoc);

        if (wxFileExists(sampleBasePath)) {
            wxRemoveFile(sampleBasePath);
        }
        if (result != BAE_NO_ERROR) {
            if (wxFileExists(samplePath)) {
                wxRemoveFile(samplePath);
            }
            return false;
        }

        *outPath = samplePath;
        return true;
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

    wxString FormatLoopTimeUsec(uint64_t usec) const {
        uint64_t totalSeconds;
        uint64_t minutes;
        uint64_t seconds;
        uint64_t millis;

        totalSeconds = usec / 1000000ULL;
        minutes = totalSeconds / 60ULL;
        seconds = totalSeconds % 60ULL;
        millis = (usec % 1000000ULL) / 1000ULL;
        return wxString::Format("%llu:%02llu.%03llu",
                                static_cast<unsigned long long>(minutes),
                                static_cast<unsigned long long>(seconds),
                                static_cast<unsigned long long>(millis));
    }

    bool ParseLoopTimeToUsec(wxString const &text, uint64_t *outUsec) const {
        wxString trimmed;
        wxString minutesPart;
        wxString secondsPart;
        long minutes;
        double secondsValue;
        double totalSeconds;
        int colonPos;
        size_t i;
        uint64_t usec;

        if (!outUsec) {
            return false;
        }
        trimmed = text;
        trimmed.Trim(true);
        trimmed.Trim(false);
        if (trimmed.empty()) {
            return false;
        }

        colonPos = trimmed.Find(':');
        if (colonPos != wxNOT_FOUND) {
            if (trimmed.Mid(static_cast<size_t>(colonPos + 1)).Find(':') != wxNOT_FOUND) {
                return false;
            }
            minutesPart = trimmed.SubString(0, colonPos - 1);
            secondsPart = trimmed.SubString(colonPos + 1, static_cast<int>(trimmed.length()) - 1);
            if (minutesPart.empty() || secondsPart.empty()) {
                return false;
            }
            for (i = 0; i < minutesPart.length(); ++i) {
                if (minutesPart[i] < '0' || minutesPart[i] > '9') {
                    return false;
                }
            }
            {
                int dotPos;

                dotPos = secondsPart.Find('.');
                if (dotPos != wxNOT_FOUND && secondsPart.Mid(static_cast<size_t>(dotPos + 1)).Find('.') != wxNOT_FOUND) {
                    return false;
                }
            }
            for (i = 0; i < secondsPart.length(); ++i) {
                if (!((secondsPart[i] >= '0' && secondsPart[i] <= '9') || secondsPart[i] == '.')) {
                    return false;
                }
            }
            if (!minutesPart.ToLong(&minutes) || !secondsPart.ToDouble(&secondsValue)) {
                return false;
            }
            if (minutes < 0 || secondsValue < 0.0 || secondsValue >= 60.0) {
                return false;
            }
            usec = static_cast<uint64_t>(std::llround((static_cast<double>(minutes) * 60.0 + secondsValue) * 1000000.0));
            if (usec > 0xFFFFFFFFULL) {
                return false;
            }
            *outUsec = usec;
            return true;
        } else {
            for (i = 0; i < trimmed.length(); ++i) {
                char c;

                c = static_cast<char>(trimmed[i]);
                if (!((c >= '0' && c <= '9') || c == '.')) {
                    return false;
                }
            }
            if (!trimmed.ToDouble(&totalSeconds)) {
                return false;
            }
            if (totalSeconds < 0.0) {
                return false;
            }
            usec = static_cast<uint64_t>(std::llround(totalSeconds * 1000000.0));
            if (usec > 0xFFFFFFFFULL) {
                return false;
            }
            *outUsec = usec;
            return true;
        }
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
        {
            BAERmfEditorMidiStorageType storageType;

            if (BAERmfEditorDocument_GetMidiStorageType(m_document, &storageType) == BAE_NO_ERROR) {
                SetSelectedMidiStorageType(storageType);
            }
        }
        InvalidatePianoRollPreviewSong();
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
        RefreshMidiLoopControlsFromDocument();
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
                            "Open RMF, MIDI, or Session",
                            wxEmptyString,
                            wxEmptyString,
                            "All supported files (*.rmf;*.zmf;*.mid;*.midi;*.kar;*.rmi;*.nbs)|*.rmf;*.zmf;*.mid;*.midi;*.kar;*.rmi;*.nbs|NeoBAE Session (*.nbs)|*.nbs|RMF files (*.rmf;*.zmf)|*.rmf;*.zmf|MIDI files (*.mid;*.midi;*.kar;*.rmi)|*.mid;*.midi;*.kar;*.rmi|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            wxString selectedPath = dialog.GetPath();
            if (wxFileName(selectedPath).GetExt().Lower() == "nbs") {
                LoadSession(selectedPath);
            } else {
                LoadDocument(selectedPath);
            }
        }
    }

    void OnSaveAs(wxCommandEvent &) {
        BAERmfEditorDocument *saveDoc;
        bool saveCurrentTrack;
        int selectedTrack;
        bool requiresZmf;
        bool canSaveAsMidi;

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
          canSaveAsMidi = (BAERmfEditorDocument_CanSaveAsMidi(saveDoc) != 0);
        BAERmfEditorDocument_SetMidiStorageType(saveDoc, GetSelectedMidiStorageType());

        wxFileDialog dialog(this,
                            requiresZmf ? "Save As ZMF (required by FLAC/Vorbis/Opus samples)"
                                                     : (canSaveAsMidi
                                                         ? "Save As RMF/ZMF/MIDI"
                                                         : "Save As RMF/ZMF (MIDI disabled: custom instruments/samples)"),
                            wxEmptyString,
                            wxEmptyString,
                            requiresZmf ? "ZMF files (*.zmf)|*.zmf"
                                                     : (canSaveAsMidi
                                                         ? "RMF, ZMF, and MIDI files (*.rmf;*.zmf;*.mid;*.midi)|*.rmf;*.zmf;*.mid;*.midi|RMF and ZMF files (*.rmf;*.zmf)|*.rmf;*.zmf|MIDI files (*.mid;*.midi)|*.mid;*.midi"
                                                         : "RMF and ZMF files (*.rmf;*.zmf)|*.rmf;*.zmf"),
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) {
            if (saveDoc != m_document) {
                BAERmfEditorDocument_Delete(saveDoc);
            }
            return;
        }
        {
            wxString targetPath = dialog.GetPath();
            BAEResult saveResult;

            if (IsMidiSaveExtension(targetPath)) {
                wxFileName fn(targetPath);
                if (!canSaveAsMidi) {
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Cannot save as MIDI when the document contains custom instruments or samples.",
                                 "MIDI Save Disabled",
                                 wxOK | wxICON_INFORMATION,
                                 this);
                    return;
                }
                if (fn.GetExt().Lower() != "mid" && fn.GetExt().Lower() != "midi") {
                    fn.SetExt("mid");
                    targetPath = fn.GetFullPath();
                }
                wxScopedCharBuffer utf8TargetPath = targetPath.utf8_str();
                saveResult = BAERmfEditorDocument_SaveAsMidi(saveDoc, const_cast<char *>(utf8TargetPath.data()));
                if (saveResult != BAE_NO_ERROR) {
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Failed to save MIDI file.", "Save Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                if (saveDoc != m_document) {
                    BAERmfEditorDocument_Delete(saveDoc);
                }
                SetStatusText(targetPath, 1);
                return;
            }

            /* Enforce .zmf extension when document contains FLAC/Vorbis/Opus samples */
            if (requiresZmf) {
                wxFileName fn(targetPath);
                if (fn.GetExt().Lower() != "zmf") {
                    fn.SetExt("zmf");
                    targetPath = fn.GetFullPath();
                }
            }
            wxScopedCharBuffer utf8TargetPath = targetPath.utf8_str();
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

    bool WriteSessionToFile(wxString const &path) {
        std::vector<unsigned char> rmfBlob;
        std::vector<unsigned char> payload;
        NbsSessionSettings settings;
        uLongf compBound;
        uLongf compLen;
        std::vector<unsigned char> compressed;
        std::vector<unsigned char> header;
        wxFile file;

        if (!CaptureSerializedDocument(&rmfBlob)) {
            wxMessageBox("Failed to serialize document.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        memset(&settings, 0, sizeof(settings));
        settings.settingsVersion = 1;
        settings.previewVolume = m_previewVolumeSlider ? m_previewVolumeSlider->GetValue() : 100;
        settings.previewReverbType = static_cast<int32_t>(GetSelectedPreviewReverbType());
        settings.previewLoopEnabled = IsPreviewLoopEnabled() ? 1 : 0;
        settings.midiStorageMode = m_midiStorageChoice ? m_midiStorageChoice->GetSelection() : 1;
        settings.selectedTrack = GetSelectedTrack();

        /* Build TLV payload: RMF blob */
        AppendLE16(payload, kNbsFieldRmfBlob);
        AppendLE32(payload, static_cast<uint32_t>(rmfBlob.size()));
        payload.insert(payload.end(), rmfBlob.begin(), rmfBlob.end());

        /* Build TLV payload: settings */
        AppendLE16(payload, kNbsFieldSettings);
        AppendLE32(payload, static_cast<uint32_t>(sizeof(settings)));
        {
            unsigned char const *p = reinterpret_cast<unsigned char const *>(&settings);
            payload.insert(payload.end(), p, p + sizeof(settings));
        }

        /* Build TLV payload: original path */
        if (!m_currentPath.empty()) {
            wxScopedCharBuffer utf8 = m_currentPath.utf8_str();
            size_t len = strlen(utf8.data());
            AppendLE16(payload, kNbsFieldOriginalPath);
            AppendLE32(payload, static_cast<uint32_t>(len));
            payload.insert(payload.end(),
                           reinterpret_cast<unsigned char const *>(utf8.data()),
                           reinterpret_cast<unsigned char const *>(utf8.data()) + len);
        }

        /* Compress */
        compBound = compressBound(static_cast<uLong>(payload.size()));
        compressed.resize(static_cast<size_t>(compBound));
        compLen = compBound;
        if (compress(compressed.data(), &compLen, payload.data(), static_cast<uLong>(payload.size())) != Z_OK) {
            wxMessageBox("Compression failed.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        compressed.resize(static_cast<size_t>(compLen));

        /* Write file: 10-byte header + compressed data */
        if (!file.Open(path, wxFile::write)) {
            wxMessageBox("Could not open file for writing.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        header.insert(header.end(), kNbsMagic, kNbsMagic + 4);
        AppendLE16(header, kNbsVersion);
        AppendLE32(header, static_cast<uint32_t>(payload.size()));

        if (file.Write(header.data(), header.size()) != static_cast<wxFileOffset>(header.size()) ||
            file.Write(compressed.data(), compressed.size()) != static_cast<wxFileOffset>(compressed.size())) {
            file.Close();
            wxMessageBox("Failed to write session file.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        file.Close();

        m_sessionPath = path;
        SetStatusText(path, 1);
        return true;
    }

    void OnSaveSession(wxCommandEvent &evt) {
        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save Session", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (m_sessionPath.empty()) {
            OnSaveSessionAs(evt);
            return;
        }
        WriteSessionToFile(m_sessionPath);
    }

    void OnSaveSessionAs(wxCommandEvent &) {
        wxFileDialog dialog(this,
                            "Save Session",
                            wxEmptyString,
                            wxEmptyString,
                            "NeoBAE Session (*.nbs)|*.nbs",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save Session", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (dialog.ShowModal() == wxID_OK) {
            wxString path = dialog.GetPath();
            if (wxFileName(path).GetExt().IsEmpty()) {
                path += ".nbs";
            }
            WriteSessionToFile(path);
        }
    }

    bool LoadSession(wxString const &path) {
        wxFile file;
        wxFileOffset fileLen;
        std::vector<unsigned char> fileData;
        uint32_t uncompressedSize;
        std::vector<unsigned char> payload;
        uLongf destLen;
        std::vector<unsigned char> rmfBlob;
        NbsSessionSettings settings;
        bool haveSettings;
        size_t offset;

        memset(&settings, 0, sizeof(settings));
        settings.selectedTrack = -1;
        haveSettings = false;

        if (!file.Open(path, wxFile::read)) {
            wxMessageBox("Could not open session file.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        fileLen = file.Length();
        if (fileLen < 10) {
            file.Close();
            wxMessageBox("File is too small to be a valid session.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        fileData.resize(static_cast<size_t>(fileLen));
        if (file.Read(fileData.data(), static_cast<size_t>(fileLen)) != fileLen) {
            file.Close();
            wxMessageBox("Failed to read session file.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        file.Close();

        /* Validate header */
        if (memcmp(fileData.data(), kNbsMagic, 4) != 0) {
            wxMessageBox("Not a valid NeoBAE session file.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        {
            uint16_t fileVersion = ReadLE16(fileData.data() + 4);
            if (fileVersion > kNbsVersion) {
                wxMessageBox("Session file version is newer than this editor supports.", "Open Session", wxOK | wxICON_ERROR, this);
                return false;
            }
        }
        uncompressedSize = ReadLE32(fileData.data() + 6);

        /* Decompress */
        payload.resize(uncompressedSize);
        destLen = static_cast<uLongf>(uncompressedSize);
        if (uncompress(payload.data(), &destLen, fileData.data() + 10,
                       static_cast<uLong>(fileData.size() - 10)) != Z_OK) {
            wxMessageBox("Failed to decompress session data.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        /* Parse TLV fields */
        offset = 0;
        while (offset + 6 <= payload.size()) {
            uint16_t fieldId = ReadLE16(payload.data() + offset);
            uint32_t fieldLen = ReadLE32(payload.data() + offset + 2);
            offset += 6;
            if (offset + fieldLen > payload.size()) {
                break;
            }

            if (fieldId == kNbsFieldRmfBlob) {
                rmfBlob.assign(payload.data() + offset, payload.data() + offset + fieldLen);
            } else if (fieldId == kNbsFieldSettings) {
                if (fieldLen >= sizeof(NbsSessionSettings)) {
                    memcpy(&settings, payload.data() + offset, sizeof(NbsSessionSettings));
                    haveSettings = true;
                }
            }
            /* Unknown fields are silently skipped */
            offset += fieldLen;
        }

        if (rmfBlob.empty()) {
            wxMessageBox("Session file contains no document data.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        /* Restore document */
        if (!RestoreSerializedDocument(rmfBlob)) {
            wxMessageBox("Failed to restore document from session.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        /* Apply settings */
        if (haveSettings) {
            if (m_previewVolumeSlider) {
                m_previewVolumeSlider->SetValue(std::clamp(settings.previewVolume, 0, 100));
            }
            SetSelectedPreviewReverbType(static_cast<BAEReverbType>(settings.previewReverbType));
            if (m_previewLoopCheck) {
                m_previewLoopCheck->SetValue(settings.previewLoopEnabled != 0);
            }
            if (m_midiStorageChoice) {
                m_midiStorageChoice->SetSelection(std::clamp(settings.midiStorageMode, 0, 3));
            }
        }

        /* Post-load UI refresh (mirrors LoadDocument) */
        {
            BAERmfEditorMidiStorageType storageType;
            if (BAERmfEditorDocument_GetMidiStorageType(m_document, &storageType) == BAE_NO_ERROR) {
                SetSelectedMidiStorageType(storageType);
            }
        }
        InvalidatePianoRollPreviewSong();
        ClearUndoHistory();
        m_currentPath = path;
        m_sessionPath = path;
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        UpdatePositionLabelFromDocumentTick(0);
        StopPlayback(true);
        PopulateTrackList();
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();

        /* Restore selected track from session */
        if (haveSettings && settings.selectedTrack >= 0 &&
            settings.selectedTrack < static_cast<int32_t>(m_trackList->GetCount())) {
            m_trackList->SetSelection(settings.selectedTrack);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, settings.selectedTrack);
            UpdateControlsFromSelection();
        }

        UpdateFrameTitle();
        SetStatusText(path, 1);
        return true;
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

    void OnTrackSettingsChanged(wxCommandEvent &event) {
        BAERmfEditorTrackInfo trackInfo;
        BAERmfEditorNoteInfo selectedNote;
        int selectedTrack;
        wxObject *source;
        bool bankProgramControl;
        bool defaultInstrumentUpdated;

        if (!m_document || m_updatingControls) {
            return;
        }
        source = event.GetEventObject();
        bankProgramControl = (source == m_bankSpin || source == m_programSpin);
        selectedTrack = GetSelectedTrack();
        if (selectedTrack < 0 || BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) != BAE_NO_ERROR) {
            return;
        }
        if (bankProgramControl && PianoRollPanel_GetSelectedNoteInfo(m_pianoRoll, &selectedNote)) {
            selectedNote.bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
            selectedNote.program = static_cast<unsigned char>(m_programSpin->GetValue());
            PianoRollPanel_SetSelectedNoteInstrument(m_pianoRoll, selectedNote.bank, selectedNote.program);
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, selectedNote.bank, selectedNote.program);
            InvalidatePianoRollPreviewSong();
            return;
        }
        defaultInstrumentUpdated = false;
        if (bankProgramControl) {
            uint16_t bank;
            unsigned char program;

            bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
            program = static_cast<unsigned char>(m_programSpin->GetValue());
            if (BAERmfEditorDocument_SetTrackDefaultInstrument(m_document,
                                                               static_cast<uint16_t>(selectedTrack),
                                                               bank,
                                                               program) == BAE_NO_ERROR) {
                trackInfo.bank = bank;
                trackInfo.program = program;
                defaultInstrumentUpdated = true;
            }
        }
        trackInfo.transpose = static_cast<int16_t>(m_transposeSpin->GetValue());
        trackInfo.pan = static_cast<unsigned char>(m_panSpin->GetValue());
        trackInfo.volume = static_cast<unsigned char>(m_volumeSpin->GetValue());
        if (!bankProgramControl && BAERmfEditorDocument_SetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) == BAE_NO_ERROR) {
            if (selectedTrack >= 0 && selectedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetString(static_cast<unsigned>(selectedTrack), BuildTrackLabel(selectedTrack, trackInfo));
            }
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, trackInfo.bank, trackInfo.program);
            PianoRollPanel_Refresh(m_pianoRoll);
        } else if (defaultInstrumentUpdated) {
            if (selectedTrack >= 0 && selectedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetString(static_cast<unsigned>(selectedTrack), BuildTrackLabel(selectedTrack, trackInfo));
            }
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, trackInfo.bank, trackInfo.program);
            PianoRollPanel_Refresh(m_pianoRoll);
        }
        if (bankProgramControl) {
            InvalidatePianoRollPreviewSong();
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
        SampleTreeItemData *selectedData;

        selectedData = GetSelectedSampleTreeData();
        if (selectedData) {
            menu.Append(ID_InstrumentEdit, "Edit Instrument...");
            menu.AppendSeparator();
            if (selectedData->IsAssetNode()) {
                menu.Append(ID_CompressInstrument, "Compress Instrument");
                menu.Append(ID_SampleDelete, "Delete Sample Asset");
            } else {
                menu.Append(ID_CompressInstrument, "Compress Instrument");
                menu.Append(ID_SampleDelete, "Delete Instrument");
            }
            menu.AppendSeparator();
            menu.Append(ID_CompressAllInstruments, "Compress All Instruments");
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

            loopResult = BAESong_SetLoops(m_playbackSong, IsPreviewLoopEnabled() ? 32767 : 0);
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
        ApplyPreviewReverbToMixer();
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

    void OnPreviewReverbChanged(wxCommandEvent &) {
        ApplyPreviewReverbToMixer();
    }

    void OnPreviewLoopChanged(wxCommandEvent &) {
        ApplyPreviewLoopSettingToSong();
    }

    void RefreshPlaybackAtCurrentPosition() {
        uint32_t posUsec;
        BAE_BOOL wasPaused;

        if (!m_playbackSong || !m_playButton) {
            return;
        }
        posUsec = 0;
        BAESong_GetMicrosecondPosition(m_playbackSong, &posUsec);
        wasPaused = FALSE;
        BAESong_IsPaused(m_playbackSong, &wasPaused);

        m_pendingLoopHotReloadPosUsec = posUsec;
        m_pendingLoopHotReloadPaused = (wasPaused != FALSE);
        if (m_pendingLoopHotReload) {
            return;
        }
        m_pendingLoopHotReload = true;

        /* Marker loop changes require rebuilding/reloading song data; restarting the same
           BAESong instance does not re-parse updated marker metadata. Queue this to run
           after the current text/change event completes so it mirrors an actual Play click. */
        CallAfter([this]() {
            wxCommandEvent playEvent(wxEVT_BUTTON, m_playButton ? m_playButton->GetId() : wxID_ANY);
            uint32_t restorePosUsec = m_pendingLoopHotReloadPosUsec;
            bool restorePaused = m_pendingLoopHotReloadPaused;
            XBOOL loopEnabled;
            uint32_t loopStartTick;
            uint32_t loopEndTick;
            int32_t loopCount;

            m_pendingLoopHotReload = false;
            OnPlay(playEvent);
            if (!m_playbackSong) {
                return;
            }
            loopEnabled = FALSE;
            loopStartTick = 0;
            loopEndTick = 0;
            loopCount = -1;
            if (BAERmfEditorDocument_GetMidiLoopMarkers(m_document,
                                                        &loopEnabled,
                                                        &loopStartTick,
                                                        &loopEndTick,
                                                        &loopCount) == BAE_NO_ERROR &&
                loopEnabled && loopEndTick > loopStartTick) {
                uint32_t loopStartUsec;

                loopStartUsec = static_cast<uint32_t>(TicksToMicroseconds(loopStartTick));
                if (restorePosUsec > loopStartUsec) {
                    /* Ensure transport passes loopstart after reload so marker-based
                       looping is armed before we continue playback. */
                    restorePosUsec = loopStartUsec;
                }
            }
            BAESong_SetMicrosecondPosition(m_playbackSong, restorePosUsec);
            if (restorePaused) {
                BAESong_Pause(m_playbackSong);
            }
        });
    }

    void OnMidiLoopMarkersChanged(wxCommandEvent &) {
        XBOOL enabled;
        uint32_t startTick;
        uint32_t endTick;
        uint64_t startUsec;
        uint64_t endUsec;

        if (m_loadingLoopControls || !m_document) {
            return;
        }
        enabled = (m_midiLoopEnableCheck && m_midiLoopEnableCheck->GetValue()) ? TRUE : FALSE;

        if (enabled) {
            if (!m_midiLoopStartText || !m_midiLoopEndText) {
                return;
            }
            if (!ParseLoopTimeToUsec(m_midiLoopStartText->GetValue(), &startUsec) ||
                !ParseLoopTimeToUsec(m_midiLoopEndText->GetValue(), &endUsec)) {
                return;
            }
            startTick = MicrosecondsToTicks(static_cast<uint32_t>(std::min<uint64_t>(startUsec, 0xFFFFFFFFULL)));
            endTick = MicrosecondsToTicks(static_cast<uint32_t>(std::min<uint64_t>(endUsec, 0xFFFFFFFFULL)));
        } else {
            startTick = 0;
            endTick = 0;
        }

        if (enabled && endTick <= startTick) {
            endTick = startTick + 1;
            if (m_midiLoopEndText) {
                m_loadingLoopControls = true;
                m_midiLoopEndText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(endTick)));
                m_loadingLoopControls = false;
            }
        }

        if (BAERmfEditorDocument_SetMidiLoopMarkers(m_document,
                                                    enabled,
                                                    startTick,
                                                    endTick,
                                                    -1) == BAE_NO_ERROR) {
            InvalidatePianoRollPreviewSong();
            RefreshPlaybackAtCurrentPosition();
        }

        if (m_midiLoopStartText) m_midiLoopStartText->Enable(enabled != FALSE);
        if (m_midiLoopEndText) m_midiLoopEndText->Enable(enabled != FALSE);
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

    void OnReloadInternalBank(wxCommandEvent &) {
        if (!m_playbackMixer) {
            return;
        }
        StopPlayback(true);
        BAEMixer_UnloadBanks(m_playbackMixer);
        m_bankToken = nullptr;
        m_bankLoaded = false;
        m_bankTokens.clear();
#ifdef _BUILT_IN_PATCHES
        {
            BAEResult bankResult = BAEMixer_LoadBuiltinBank(m_playbackMixer, &m_bankToken);
            if (bankResult == BAE_NO_ERROR) {
                m_bankLoaded = true;
                m_bankTokens.push_back(m_bankToken);
                SetStatusText("Internal bank reloaded", 0);
            } else {
                SetStatusText("Failed to reload internal bank", 0);
            }
        }
#else
        SetStatusText("All banks unloaded", 0);
#endif
    }

    void OnCloneFromBank(wxCommandEvent &) {
        if (!m_document) {
            wxMessageBox("Open or create a document first.", "Clone from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }
        /* Ensure engine + built-in bank are loaded */
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize playback engine.", "Clone from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        if (m_bankTokens.empty()) {
            wxMessageBox("No banks loaded.", "Clone from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        /* Enumerate instruments from ALL loaded banks */
        struct BankInstEntry {
            BAEBankToken token;
            uint32_t bankIndex;     /* index within the bank */
        };
        wxArrayString choices;
        std::vector<BankInstEntry> entries;

        for (size_t bk = 0; bk < m_bankTokens.size(); ++bk) {
            BAEBankToken token = m_bankTokens[bk];
            uint32_t instCount = 0;
            if (BAERmfEditorBank_GetInstrumentCount(token, &instCount) != BAE_NO_ERROR) {
                continue;
            }
            /* Get bank friendly name for display */
            char friendlyBuf[128];
            wxString bankLabel;
            if (BAE_GetBankFriendlyName(m_playbackMixer, token, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
                bankLabel = wxString::FromUTF8(friendlyBuf);
            } else if (m_bankTokens.size() > 1) {
                bankLabel = wxString::Format("Bank %u", static_cast<unsigned>(bk + 1));
            }

            for (uint32_t i = 0; i < instCount; ++i) {
                BAERmfEditorBankInstrumentInfo info;
                if (BAERmfEditorBank_GetInstrumentInfo(token, i, &info) != BAE_NO_ERROR) {
                    continue;
                }
                wxString instLabel;
                if (info.name[0]) {
                    instLabel = wxString::Format("P%u B%u: %s (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 wxString::FromUTF8(info.name),
                                                 static_cast<int>(info.keySplitCount));
                } else {
                    instLabel = wxString::Format("P%u B%u: (unnamed) (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 static_cast<int>(info.keySplitCount));
                }
                if (!bankLabel.empty()) {
                    instLabel = wxString::Format("[%s] %s", bankLabel, instLabel);
                }
                choices.Add(instLabel);
                entries.push_back({token, i});
            }
        }
        if (choices.IsEmpty()) {
            wxMessageBox("No instruments found in loaded banks.", "Clone from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        /* Show instrument picker */
        wxSingleChoiceDialog pickDlg(this,
                                     "Select an instrument to clone from the bank:",
                                     "Clone Instrument from Bank",
                                     choices);
        if (pickDlg.ShowModal() != wxID_OK) {
            return;
        }
        int selection = pickDlg.GetSelection();
        if (selection < 0 || static_cast<size_t>(selection) >= entries.size()) {
            return;
        }
        BankInstEntry const &chosen = entries[static_cast<size_t>(selection)];
        BAEBankToken chosenBank = chosen.token;
        uint32_t bankInstIndex = chosen.bankIndex;

        /* Find first unused program number */
        bool usedPrograms[128];
        long defaultProgram = 0;
        memset(usedPrograms, 0, sizeof(usedPrograms));
        {
            uint32_t sampleCount = 0;
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo sInfo;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &sInfo) == BAE_NO_ERROR &&
                    sInfo.program < 128) {
                    usedPrograms[sInfo.program] = true;
                }
            }
        }
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                defaultProgram = i;
                break;
            }
        }

        long targetProgram = wxGetNumberFromUser(
            "MIDI program number for cloned instrument (0-127):",
            "Program",
            "Clone Instrument from Bank",
            defaultProgram,
            0,
            127,
            this);
        if (targetProgram < 0) {
            return;
        }

        /* Perform clone */
        BeginUndoAction("Clone Instrument from Bank");
        BAEResult result = BAERmfEditorDocument_CloneInstrumentFromBank(
            m_document,
            chosenBank,
            bankInstIndex,
            static_cast<unsigned char>(targetProgram));
        if (result != BAE_NO_ERROR) {
            CancelUndoAction();
            wxMessageBox("Failed to clone instrument from bank.", "Clone from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        CommitUndoAction("Clone Instrument from Bank");

        PopulateSampleList();
        /* Select the last added sample */
        {
            uint32_t sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) == BAE_NO_ERROR && sampleCount > 0) {
                SelectTreeItemForSample(sampleCount - 1);
            }
        }
        SetStatusText(wxString::Format("Cloned instrument to program %ld", targetProgram));
    }

    void OnAliasFromBank(wxCommandEvent &) {
        if (!m_document) {
            wxMessageBox("Open or create a document first.", "Alias from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize playback engine.", "Alias from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        if (m_bankTokens.empty()) {
            wxMessageBox("No banks loaded.", "Alias from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        struct BankInstEntry {
            BAEBankToken token;
            uint32_t bankIndex;
        };
        wxArrayString choices;
        std::vector<BankInstEntry> entries;

        for (size_t bk = 0; bk < m_bankTokens.size(); ++bk) {
            BAEBankToken token = m_bankTokens[bk];
            uint32_t instCount = 0;
            if (BAERmfEditorBank_GetInstrumentCount(token, &instCount) != BAE_NO_ERROR) {
                continue;
            }
            char friendlyBuf[128];
            wxString bankLabel;
            if (BAE_GetBankFriendlyName(m_playbackMixer, token, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
                bankLabel = wxString::FromUTF8(friendlyBuf);
            } else if (m_bankTokens.size() > 1) {
                bankLabel = wxString::Format("Bank %u", static_cast<unsigned>(bk + 1));
            }

            for (uint32_t i = 0; i < instCount; ++i) {
                BAERmfEditorBankInstrumentInfo info;
                if (BAERmfEditorBank_GetInstrumentInfo(token, i, &info) != BAE_NO_ERROR) {
                    continue;
                }
                wxString instLabel;
                if (info.name[0]) {
                    instLabel = wxString::Format("P%u B%u: %s (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 wxString::FromUTF8(info.name),
                                                 static_cast<int>(info.keySplitCount));
                } else {
                    instLabel = wxString::Format("P%u B%u: (unnamed) (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 static_cast<int>(info.keySplitCount));
                }
                if (!bankLabel.empty()) {
                    instLabel = wxString::Format("[%s] %s", bankLabel, instLabel);
                }
                choices.Add(instLabel);
                entries.push_back({token, i});
            }
        }
        if (choices.IsEmpty()) {
            wxMessageBox("No instruments found in loaded banks.", "Alias from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxSingleChoiceDialog pickDlg(this,
                                     "Select an instrument to alias from the bank:\n"
                                     "(Samples will reference the bank, not be embedded)",
                                     "Alias Instrument from Bank",
                                     choices);
        if (pickDlg.ShowModal() != wxID_OK) {
            return;
        }
        int selection = pickDlg.GetSelection();
        if (selection < 0 || static_cast<size_t>(selection) >= entries.size()) {
            return;
        }
        BankInstEntry const &chosen = entries[static_cast<size_t>(selection)];
        BAEBankToken chosenBank = chosen.token;
        uint32_t bankInstIndex = chosen.bankIndex;

        bool usedPrograms[128];
        long defaultProgram = 0;
        memset(usedPrograms, 0, sizeof(usedPrograms));
        {
            uint32_t sampleCount = 0;
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo sInfo;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &sInfo) == BAE_NO_ERROR &&
                    sInfo.program < 128) {
                    usedPrograms[sInfo.program] = true;
                }
            }
        }
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                defaultProgram = i;
                break;
            }
        }

        long targetProgram = wxGetNumberFromUser(
            "MIDI program number for aliased instrument (0-127):",
            "Program",
            "Alias Instrument from Bank",
            defaultProgram,
            0,
            127,
            this);
        if (targetProgram < 0) {
            return;
        }

        BeginUndoAction("Alias Instrument from Bank");
        BAEResult result = BAERmfEditorDocument_AliasInstrumentFromBank(
            m_document,
            chosenBank,
            bankInstIndex,
            static_cast<unsigned char>(targetProgram));
        if (result != BAE_NO_ERROR) {
            CancelUndoAction();
            wxMessageBox("Failed to alias instrument from bank.", "Alias from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        CommitUndoAction("Alias Instrument from Bank");

        PopulateSampleList();
        {
            uint32_t sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) == BAE_NO_ERROR && sampleCount > 0) {
                SelectTreeItemForSample(sampleCount - 1);
            }
        }
        SetStatusText(wxString::Format("Aliased instrument to program %ld", targetProgram));
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
        {
            uint32_t sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) == BAE_NO_ERROR && sampleCount > 0) {
                SelectTreeItemForSample(sampleCount - 1);
            }
        }
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
        SelectTreeItemForSample(newSampleIndex);
        OpenInstrumentExtEditor();

        {
            uint32_t checkIndex;
            if (GetSelectedSampleIndexFromTree(&checkIndex)) {
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
        SampleTreeItemData *selectedData;
        std::vector<uint32_t> toDelete;

        if (!m_document) {
            return;
        }
        selectedData = GetSelectedSampleTreeData();
        if (!selectedData) {
            return;
        }

        if (selectedData->IsAssetNode()) {
            uint32_t selectedAssetID;
            uint32_t usageCount;

            selectedAssetID = selectedData->GetAssetID();
            if (BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, selectedAssetID, &usageCount) != BAE_NO_ERROR) {
                return;
            }
            for (uint32_t usageIndex = 0; usageIndex < usageCount; ++usageIndex) {
                uint32_t sampleIndex;
                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                                   selectedAssetID,
                                                                   usageIndex,
                                                                   &sampleIndex) == BAE_NO_ERROR) {
                    toDelete.push_back(sampleIndex);
                }
            }
            {
                wxString msg = toDelete.size() > 1
                    ? wxString::Format("Delete sample asset A%u and all %u usages?",
                                       static_cast<unsigned>(selectedAssetID),
                                       static_cast<unsigned>(toDelete.size()))
                    : wxString::Format("Delete sample asset A%u?", static_cast<unsigned>(selectedAssetID));
                if (wxMessageBox(msg, "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
                    return;
                }
            }
        } else {
            uint32_t firstSampleIndex;
            BAERmfEditorSampleInfo firstInfo;
            uint32_t sampleCount;
            unsigned char program;

            firstSampleIndex = selectedData->GetSampleIndex();
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
            {
                wxString msg = toDelete.size() > 1
                    ? wxString::Format("Delete instrument P%u and all %u of its splits?",
                                       static_cast<unsigned>(program),
                                       static_cast<unsigned>(toDelete.size()))
                    : wxString::Format("Delete instrument P%u?", static_cast<unsigned>(program));
                if (wxMessageBox(msg, "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
                    return;
                }
            }
        }

        /* Delete in reverse order so earlier indices stay valid. */
        for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            BAERmfEditorDocument_DeleteSample(m_document, *it);
        }
        PopulateSampleList();
    }

    void OnCompressInstrument(wxCommandEvent &) {
        SampleTreeItemData *selectedData;
        std::vector<uint32_t> sampleIndices;

        if (!m_document) {
            return;
        }
        selectedData = GetSelectedSampleTreeData();
        if (!selectedData) {
            return;
        }

        // Collect all samples for the selected instrument/asset
        if (selectedData->IsAssetNode()) {
            uint32_t selectedAssetID;
            uint32_t usageCount;

            selectedAssetID = selectedData->GetAssetID();
            if (BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, selectedAssetID, &usageCount) != BAE_NO_ERROR) {
                return;
            }
            for (uint32_t usageIndex = 0; usageIndex < usageCount; ++usageIndex) {
                uint32_t sampleIndex;
                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                                   selectedAssetID,
                                                                   usageIndex,
                                                                   &sampleIndex) == BAE_NO_ERROR) {
                    sampleIndices.push_back(sampleIndex);
                }
            }
        } else {
            uint32_t firstSampleIndex;
            BAERmfEditorSampleInfo firstInfo;
            uint32_t sampleCount;
            unsigned char program;

            firstSampleIndex = selectedData->GetSampleIndex();
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
                    sampleIndices.push_back(i);
                }
            }
        }

        if (sampleIndices.empty()) {
            return;
        }

        // Show compression dialog
        BatchCompressDialog dlg(this, sampleIndices, false);
        if (dlg.ShowModal() == wxID_OK) {
            BAERmfEditorCompressionType compressionType = dlg.GetSelectedCompressionType();
            BAERmfEditorOpusMode opusMode = dlg.GetSelectedOpusMode();
            // Apply compression to all samples
            for (uint32_t sampleIndex : sampleIndices) {
                BAERmfEditorSampleInfo info;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &info) == BAE_NO_ERROR) {
                    info.compressionType = compressionType;
                    info.opusMode = opusMode;
                    BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndex, &info);
                }
            }
            PopulateSampleList();
        }
    }

    void OnCompressAllInstruments(wxCommandEvent &) {
        std::vector<uint32_t> allSampleIndices;
        uint32_t sampleCount = 0;

        if (!m_document) {
            return;
        }

        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        for (uint32_t i = 0; i < sampleCount; ++i) {
            allSampleIndices.push_back(i);
        }

        if (allSampleIndices.empty()) {
            wxMessageBox("No samples to compress.", "No Samples", wxOK | wxICON_INFORMATION, this);
            return;
        }

        // Show compression dialog
        BatchCompressDialog dlg(this, allSampleIndices, true);
        if (dlg.ShowModal() == wxID_OK) {
            BAERmfEditorCompressionType compressionType = dlg.GetSelectedCompressionType();
            BAERmfEditorOpusMode opusMode = dlg.GetSelectedOpusMode();
            // Apply compression to all samples
            for (uint32_t sampleIndex : allSampleIndices) {
                BAERmfEditorSampleInfo info;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &info) == BAE_NO_ERROR) {
                    info.compressionType = compressionType;
                    info.opusMode = opusMode;
                    BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndex, &info);
                }
            }
            PopulateSampleList();
        }
    }

    void OnInstrumentEdit(wxCommandEvent &) {
        OpenInstrumentExtEditor();
    }

    void OnInstrumentEditDblClick(wxTreeEvent &) {
        OpenInstrumentExtEditor();
    }

    void OpenInstrumentExtEditor() {
        uint32_t primarySampleIndex;
        uint32_t selectedAssetID;
        uint32_t instID;
        BAERmfEditorInstrumentExtInfo extInfo;
        std::vector<uint32_t> deletedSampleIndices;
        std::vector<uint32_t> sampleIndices;
        std::vector<InstrumentEditorEditedSample> editedSamples;
        bool accepted;

        if (!m_document) {
            return;
        }
        if (!GetSelectedSampleIndexFromTree(&primarySampleIndex)) {
            return;
        }
        if (BAERmfEditorDocument_GetSampleAssetIDForSample(m_document, primarySampleIndex, &selectedAssetID) != BAE_NO_ERROR) {
            selectedAssetID = 0;
        }

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
            [this](wxString const &label) { BeginUndoAction(label); },
            [this](wxString const &label) { CommitUndoAction(label); },
            [this]() { CancelUndoAction(); },
            [this](uint32_t sampleIndex, int key, BAESampleInfo const *overrideInfo, int16_t splitVolumeOverride, unsigned char rootKeyOverride, BAERmfEditorCompressionType compressionOverride) {
                if (!PreviewSampleAtKey(sampleIndex, key, overrideInfo, splitVolumeOverride, rootKeyOverride, compressionOverride)) {
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
            std::vector<uint32_t> affectAllAssets;
            std::vector<uint32_t> cloneAssets;

            for (size_t i = 0; i < sampleIndices.size() && i < editedSamples.size(); ++i) {
                BAERmfEditorCompressionType oldCompression;

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

                oldCompression = BAE_EDITOR_COMPRESSION_PCM;
                {
                    BAERmfEditorSampleInfo oldInfo;
                    if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndices[i], &oldInfo) == BAE_NO_ERROR) {
                        oldCompression = oldInfo.compressionType;
                    }
                }

                if (oldCompression != editedSamples[i].compressionType) {
                    uint32_t assetID;
                    uint32_t usageCount;

                    assetID = 0;
                    usageCount = 0;
                    if (BAERmfEditorDocument_GetSampleAssetIDForSample(m_document, sampleIndices[i], &assetID) == BAE_NO_ERROR &&
                        BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, assetID, &usageCount) == BAE_NO_ERROR &&
                        usageCount > 1 &&
                        std::find(affectAllAssets.begin(), affectAllAssets.end(), assetID) == affectAllAssets.end()) {
                        if (std::find(cloneAssets.begin(), cloneAssets.end(), assetID) != cloneAssets.end()) {
                            if (BAERmfEditorDocument_CloneSampleAssetForSample(m_document, sampleIndices[i], NULL) != BAE_NO_ERROR) {
                                ok = false;
                                break;
                            }
                        } else {
                            wxMessageDialog choiceDialog(
                                this,
                                wxString::Format("Sample asset A%u is used by %u instrument usages.\n\n"
                                                 "Choose how to apply this compression change.",
                                                 static_cast<unsigned>(assetID),
                                                 static_cast<unsigned>(usageCount)),
                                "Shared Sample Asset",
                                wxYES_NO | wxCANCEL | wxICON_QUESTION | wxYES_DEFAULT);
                            choiceDialog.SetYesNoLabels("This Instrument Only", "All Shared Uses");
                            int choice = choiceDialog.ShowModal();
                            if (choice == wxCANCEL) {
                                ok = false;
                                break;
                            }
                            if (choice == wxYES) {
                                cloneAssets.push_back(assetID);
                                if (BAERmfEditorDocument_CloneSampleAssetForSample(m_document, sampleIndices[i], NULL) != BAE_NO_ERROR) {
                                    ok = false;
                                    break;
                                }
                            } else {
                                affectAllAssets.push_back(assetID);
                            }
                        }
                    }
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
        if (!sampleIndices.empty() && sampleIndices[0] != static_cast<uint32_t>(-1)) {
            SelectTreeItemForSample(sampleIndices[0]);
        } else if (selectedAssetID != 0) {
            uint32_t usageCount = 0;
            if (BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, selectedAssetID, &usageCount) == BAE_NO_ERROR && usageCount > 0) {
                uint32_t sampleIndex;
                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document, selectedAssetID, 0, &sampleIndex) == BAE_NO_ERROR) {
                    SelectTreeItemForSample(sampleIndex);
                }
            }
        }

    }
};

}  // namespace

class EditorApp final : public wxApp {
public:
    bool OnInit() override {
        auto *frame = new MainFrame();
        frame->Show(true);

        if (argc > 1 && (argv[1].IsEmpty() == false)) {
            wxString startupPath(argv[1]);
            frame->CallAfter([frame, startupPath]() {
                frame->OpenPathFromCli(startupPath);
            });
        }

        return true;
    }
};

wxIMPLEMENT_APP(EditorApp);