#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>

#include "editor_pianoroll_panel.h"

constexpr int kPianoRollLeftGutter = 64;
constexpr int kPianoRollTopGutter = 30;
constexpr int kNoteHeight = 12;
constexpr int kBasePixelsPerQuarter = 96;
constexpr int kResizeHandlePixels = 6;
constexpr int kAutomationLaneHeight = 44;
constexpr int kAutomationLaneGap = 6;
constexpr uint32_t kDefaultNoteDuration = 480;
constexpr int kTempoLaneIndex = 0;

enum class DragMode {
    None,
    Move,
    ResizeLeft,
    ResizeRight,
    SelectBox,
};

enum {
    ID_PianoRollEdit = wxID_HIGHEST + 2000,
    ID_PianoRollDelete,
};

enum class PianoRollSelectionKind {
    None,
    Note,
    Automation,
};

enum class AutomationLaneKind {
    Tempo,
    Controller,
    PitchBend,
};

struct AutomationLaneDescriptor {
    wxString label;
    AutomationLaneKind kind;
    unsigned char controller;
    wxColour color;
    int maxValue;
    bool bipolar;
};

struct AutomationHitInfo {
    int laneIndex;
    uint32_t eventIndex;
    uint32_t tick;
    uint32_t endTick;
    int laneTop;
    int laneBottom;
    int leftX;
    int rightX;
    int value;
    bool hasFollowingEvent;
};

static wxString GetControllerLaneLabel(unsigned char controller) {
    switch (controller) {
        case 1: return "Mod Wheel (CC1)";
        case 2: return "Breath (CC2)";
        case 4: return "Foot Ctrl (CC4)";
        case 5: return "Portamento Time (CC5)";
        case 6: return "Data Entry (CC6)";
        case 7: return "Volume (CC7)";
        case 8: return "Balance (CC8)";
        case 10: return "Pan (CC10)";
        case 11: return "Expression (CC11)";
        case 64: return "Sustain (CC64)";
        case 65: return "Portamento (CC65)";
        case 66: return "Sostenuto (CC66)";
        case 67: return "Soft Pedal (CC67)";
        case 71: return "Resonance (CC71)";
        case 72: return "Release (CC72)";
        case 73: return "Attack (CC73)";
        case 74: return "Brightness (CC74)";
        case 91: return "Reverb Send (CC91)";
        case 93: return "Chorus Send (CC93)";
        default:
            return wxString::Format("CC%u", static_cast<unsigned>(controller));
    }
}

static std::vector<AutomationLaneDescriptor> const &GetAutomationLanes() {
    static std::vector<AutomationLaneDescriptor> const lanes = {
        { "Tempo", AutomationLaneKind::Tempo, 0, wxColour(202, 128, 52), 240, false },
        { "Mod Wheel (CC1)", AutomationLaneKind::Controller, 1, wxColour(74, 136, 208), 127, false },
        { "Breath (CC2)", AutomationLaneKind::Controller, 2, wxColour(166, 96, 196), 127, false },
        { "Foot Ctrl (CC4)", AutomationLaneKind::Controller, 4, wxColour(76, 170, 92), 127, false },
        { "Portamento Time (CC5)", AutomationLaneKind::Controller, 5, wxColour(228, 148, 56), 127, false },
        { "Data Entry (CC6)", AutomationLaneKind::Controller, 6, wxColour(82, 172, 170), 127, false },
        { "Volume (CC7)", AutomationLaneKind::Controller, 7, wxColour(74, 136, 208), 127, false },
        { "Balance (CC8)", AutomationLaneKind::Controller, 8, wxColour(166, 96, 196), 127, true },
        { "Pan (CC10)", AutomationLaneKind::Controller, 10, wxColour(76, 170, 92), 127, true },
        { "Expression (CC11)", AutomationLaneKind::Controller, 11, wxColour(228, 148, 56), 127, false },
        { "Sustain (CC64)", AutomationLaneKind::Controller, 64, wxColour(82, 172, 170), 127, false },
        { "Portamento (CC65)", AutomationLaneKind::Controller, 65, wxColour(210, 96, 118), 127, false },
        { "Sostenuto (CC66)", AutomationLaneKind::Controller, 66, wxColour(74, 136, 208), 127, false },
        { "Soft Pedal (CC67)", AutomationLaneKind::Controller, 67, wxColour(166, 96, 196), 127, false },
        { "Resonance (CC71)", AutomationLaneKind::Controller, 71, wxColour(76, 170, 92), 127, false },
        { "Release (CC72)", AutomationLaneKind::Controller, 72, wxColour(228, 148, 56), 127, false },
        { "Attack (CC73)", AutomationLaneKind::Controller, 73, wxColour(82, 172, 170), 127, false },
        { "Brightness (CC74)", AutomationLaneKind::Controller, 74, wxColour(210, 96, 118), 127, false },
        { "Reverb Send (CC91)", AutomationLaneKind::Controller, 91, wxColour(74, 136, 208), 127, false },
        { "Chorus Send (CC93)", AutomationLaneKind::Controller, 93, wxColour(166, 96, 196), 127, false },
        { "Pitch Bend", AutomationLaneKind::PitchBend, 0, wxColour(188, 120, 214), 16383, true },
    };

    return lanes;
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

class NoteEditDialog final : public wxDialog {
public:
    explicit NoteEditDialog(wxWindow *parent,
                            BAERmfEditorNoteInfo const &noteInfo,
                            bool hasResonance,
                            unsigned char resonanceValue,
                            bool hasBrightness,
                            unsigned char brightnessValue)
        : wxDialog(parent, wxID_ANY, "Edit Note", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *gridSizer = new wxFlexGridSizer(0, 2, 8, 8);

        m_startTickText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(noteInfo.startTick)));
        m_durationText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(noteInfo.durationTicks)));
        m_noteSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.note);
        m_velocitySpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.velocity);
        m_channelSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 16, static_cast<int>(noteInfo.channel) + 1);
        m_bankSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, DisplayBankFromInternal(noteInfo.bank));
        m_programSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.program);
        m_resonanceEnable = new wxCheckBox(this, wxID_ANY, "Set Resonance (CC71)");
        m_resonanceEnable->SetValue(hasResonance);
        m_resonanceSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, resonanceValue);
        m_resonanceSpin->Enable(hasResonance);
        m_brightnessEnable = new wxCheckBox(this, wxID_ANY, "Set Brightness (CC74)");
        m_brightnessEnable->SetValue(hasBrightness);
        m_brightnessSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, brightnessValue);
        m_brightnessSpin->Enable(hasBrightness);

        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Start Tick"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_startTickText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Duration"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_durationText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Note"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_noteSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Velocity"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_velocitySpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Channel"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_channelSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Bank"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_bankSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_programSpin, 1, wxEXPAND);
        gridSizer->Add(m_resonanceEnable, 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_resonanceSpin, 1, wxEXPAND);
        gridSizer->Add(m_brightnessEnable, 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_brightnessSpin, 1, wxEXPAND);
        gridSizer->AddGrowableCol(1, 1);

        m_resonanceEnable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            if (m_resonanceSpin) {
                m_resonanceSpin->Enable(m_resonanceEnable->GetValue());
            }
        });
        m_brightnessEnable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            if (m_brightnessSpin) {
                m_brightnessSpin->Enable(m_brightnessEnable->GetValue());
            }
        });

        rootSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 12);
        rootSizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(rootSizer);
    }

    bool GetNoteInfo(BAERmfEditorNoteInfo *outNoteInfo) const {
        unsigned long long startTick;
        unsigned long long durationTicks;

        if (!outNoteInfo) {
            return false;
        }
        if (!m_startTickText->GetValue().ToULongLong(&startTick) || !m_durationText->GetValue().ToULongLong(&durationTicks) || durationTicks == 0) {
            return false;
        }
        outNoteInfo->startTick = static_cast<uint32_t>(startTick);
        outNoteInfo->durationTicks = static_cast<uint32_t>(durationTicks);
        outNoteInfo->note = static_cast<unsigned char>(m_noteSpin->GetValue());
        outNoteInfo->velocity = static_cast<unsigned char>(m_velocitySpin->GetValue());
        outNoteInfo->channel = static_cast<unsigned char>(std::clamp(m_channelSpin->GetValue() - 1, 0, 15));
        outNoteInfo->bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
        outNoteInfo->program = static_cast<unsigned char>(m_programSpin->GetValue());
        return true;
    }

    void GetFilterSettings(bool *outSetResonance,
                           unsigned char *outResonance,
                           bool *outSetBrightness,
                           unsigned char *outBrightness) const {
        if (outSetResonance) {
            *outSetResonance = m_resonanceEnable && m_resonanceEnable->GetValue();
        }
        if (outResonance) {
            *outResonance = static_cast<unsigned char>(m_resonanceSpin ? m_resonanceSpin->GetValue() : 0);
        }
        if (outSetBrightness) {
            *outSetBrightness = m_brightnessEnable && m_brightnessEnable->GetValue();
        }
        if (outBrightness) {
            *outBrightness = static_cast<unsigned char>(m_brightnessSpin ? m_brightnessSpin->GetValue() : 0);
        }
    }

private:
    wxTextCtrl *m_startTickText;
    wxTextCtrl *m_durationText;
    wxSpinCtrl *m_noteSpin;
    wxSpinCtrl *m_velocitySpin;
    wxSpinCtrl *m_channelSpin;
    wxSpinCtrl *m_bankSpin;
    wxSpinCtrl *m_programSpin;
    wxCheckBox *m_resonanceEnable;
    wxSpinCtrl *m_resonanceSpin;
    wxCheckBox *m_brightnessEnable;
    wxSpinCtrl *m_brightnessSpin;
};

class AutomationEditDialog final : public wxDialog {
public:
    AutomationEditDialog(wxWindow *parent,
                         wxString const &title,
                         wxString const &valueLabel,
                         uint32_t tick,
                         int value,
                         int maxValue,
                         uint32_t durationTicks,
                         bool allowDurationEdit)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *gridSizer = new wxFlexGridSizer(2, 6, 8, 8);

        m_tickText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(tick)));
        m_valueSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, maxValue, value);
        m_durationText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(durationTicks)));
        m_durationText->Enable(allowDurationEdit);

        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Tick"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_tickText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, valueLabel), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_valueSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Duration"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_durationText, 1, wxEXPAND);
        gridSizer->AddGrowableCol(1, 1);

        rootSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 12);
        rootSizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(rootSizer);
    }

    bool GetValues(uint32_t *outTick, int *outValue, uint32_t *outDurationTicks) const {
        unsigned long long tick;
        unsigned long long durationTicks;

        if (!outTick || !outValue || !outDurationTicks) {
            return false;
        }
        if (!m_tickText->GetValue().ToULongLong(&tick) ||
            !m_durationText->GetValue().ToULongLong(&durationTicks) ||
            durationTicks == 0) {
            return false;
        }
        *outTick = static_cast<uint32_t>(tick);
        *outValue = m_valueSpin->GetValue();
        *outDurationTicks = static_cast<uint32_t>(durationTicks);
        return true;
    }

private:
    wxTextCtrl *m_tickText;
    wxSpinCtrl *m_valueSpin;
    wxTextCtrl *m_durationText;
};

class PianoRollPanel final : public wxScrolledWindow {
public:
    explicit PianoRollPanel(wxWindow *parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxBORDER_SIMPLE | wxVSCROLL | wxHSCROLL),
          m_document(nullptr),
          m_selectedTrack(-1),
          m_selectedNote(-1),
          m_dragging(false),
          m_dragMode(DragMode::None),
          m_showPlayhead(false),
          m_playheadTick(0),
          m_selectedItemKind(PianoRollSelectionKind::None),
          m_selectedAutomationLane(-1),
          m_selectedAutomationEvent(-1),
          m_dragAutomationValid(false),
          m_dragStartTick(0),
          m_dragStartNote(0),
          m_newNoteBank(0),
                    m_newNoteProgram(0),
                    m_zoomScale(1.0f),
          m_lastPreviewDragNote(-1) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetScrollRate(16, 16);
        Bind(wxEVT_PAINT, &PianoRollPanel::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &PianoRollPanel::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &PianoRollPanel::OnLeftUp, this);
        Bind(wxEVT_LEFT_DCLICK, &PianoRollPanel::OnLeftDoubleClick, this);
        Bind(wxEVT_RIGHT_DOWN, &PianoRollPanel::OnRightDown, this);
        Bind(wxEVT_MOTION, &PianoRollPanel::OnMotion, this);
        Bind(wxEVT_LEAVE_WINDOW, &PianoRollPanel::OnMouseLeave, this);
        Bind(wxEVT_MOUSEWHEEL, &PianoRollPanel::OnMouseWheel, this);
        Bind(wxEVT_CHAR_HOOK, &PianoRollPanel::OnCharHook, this);
        Bind(wxEVT_MENU, &PianoRollPanel::OnEditSelectedItem, this, ID_PianoRollEdit);
        Bind(wxEVT_MENU, &PianoRollPanel::OnDeleteSelectedItem, this, ID_PianoRollDelete);
        UpdateVirtualSize();
    }

    void SetDocument(BAERmfEditorDocument *document) {
        m_document = document;
        m_selectedTrack = -1;
        m_selectedNote = -1;
        m_selectedNotes.clear();
        m_selectedItemKind = PianoRollSelectionKind::None;
        m_selectedAutomationLane = -1;
        m_selectedAutomationEvent = -1;
        m_dragAutomationValid = false;
        m_dragging = false;
        m_dragMode = DragMode::None;
        m_selectBoxActive = false;
        m_dragOriginalNoteIndices.clear();
        m_dragOriginalNotes.clear();
        UpdateVirtualSize();
        ScrollToMidiContentCenter();
        Refresh();
    }

    void SetSelectedTrack(int trackIndex) {
        m_selectedTrack = trackIndex;
        m_selectedNote = -1;
        m_selectedNotes.clear();
        m_selectedItemKind = PianoRollSelectionKind::None;
        m_selectedAutomationLane = -1;
        m_selectedAutomationEvent = -1;
        m_dragAutomationValid = false;
        m_dragging = false;
        m_dragMode = DragMode::None;
        m_selectBoxActive = false;
        m_dragOriginalNoteIndices.clear();
        m_dragOriginalNotes.clear();
        UpdateVirtualSize();
        ScrollToMidiContentCenter();
        Refresh();
    }

    void SetSelectionChangedCallback(std::function<void()> callback) {
        m_selectionChangedCallback = std::move(callback);
    }

    void SetSeekRequestedCallback(std::function<void(uint32_t)> callback) {
        m_seekRequestedCallback = std::move(callback);
    }

    void SetNotePreviewRequestedCallback(std::function<void(uint16_t,
                                                            unsigned char,
                                                            unsigned char,
                                                            uint32_t,
                                                            int)> callback) {
        m_notePreviewRequestedCallback = std::move(callback);
    }

    void SetNotePreviewStopRequestedCallback(std::function<void()> callback) {
        m_notePreviewStopRequestedCallback = std::move(callback);
    }

    void SetUndoCallbacks(std::function<void(wxString const &)> beginCallback,
                          std::function<void(wxString const &)> commitCallback,
                          std::function<void()> cancelCallback) {
        m_beginUndoCallback = std::move(beginCallback);
        m_commitUndoCallback = std::move(commitCallback);
        m_cancelUndoCallback = std::move(cancelCallback);
    }

    void RefreshFromDocument(bool clearSelection) {
        if (clearSelection) {
            m_selectedItemKind = PianoRollSelectionKind::None;
            m_selectedNote = -1;
            m_selectedNotes.clear();
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
        }
        m_dragging = false;
        m_dragAutomationValid = false;
        m_dragMode = DragMode::None;
        m_selectBoxActive = false;
        m_dragOriginalNoteIndices.clear();
        m_dragOriginalNotes.clear();
        m_dragUndoActive = false;
        m_dragUndoLabel.clear();
        UpdateVirtualSize();
        Refresh();
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
    }

    bool GetSelectedNoteInfo(BAERmfEditorNoteInfo *outNoteInfo) const {
        long noteIndex;

        if (!outNoteInfo || !HasTrack()) {
            return false;
        }
        noteIndex = m_selectedNote;
        if (noteIndex < 0 && !m_selectedNotes.empty()) {
            noteIndex = m_selectedNotes.front();
        }
        if (noteIndex < 0) {
            return false;
        }
        return BAERmfEditorDocument_GetNoteInfo(m_document,
                                                static_cast<uint16_t>(m_selectedTrack),
                                                static_cast<uint32_t>(noteIndex),
                                                outNoteInfo) == BAE_NO_ERROR;
    }

    void SetSelectedNoteInstrument(uint16_t bank, unsigned char program) {
        std::vector<long> noteIndices;
        bool anyChanged;

        if (!HasTrack()) {
            return;
        }
        noteIndices = m_selectedNotes;
        if (noteIndices.empty() && m_selectedNote >= 0) {
            noteIndices.push_back(m_selectedNote);
        }
        if (noteIndices.empty()) {
            return;
        }
        BeginUndoAction("Change Note Instrument");
        anyChanged = false;
        for (long noteIndex : noteIndices) {
            BAERmfEditorNoteInfo noteInfo;

            if (noteIndex < 0) {
                continue;
            }
            if (BAERmfEditorDocument_GetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(noteIndex),
                                                 &noteInfo) != BAE_NO_ERROR) {
                continue;
            }
            noteInfo.bank = bank;
            noteInfo.program = program;
            if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(noteIndex),
                                                 &noteInfo) == BAE_NO_ERROR) {
                anyChanged = true;
            }
        }
        if (anyChanged) {
            CommitUndoAction("Change Note Instrument");
            Refresh();
        } else {
            CancelUndoAction();
        }
    }

    void SetNewNoteInstrument(uint16_t bank, unsigned char program) {
        m_newNoteBank = bank;
        m_newNoteProgram = program;
    }

    void SetPlayheadTick(uint32_t tick) {
        m_showPlayhead = true;
        m_playheadTick = tick;
        Refresh();
    }

    void EnsurePlayheadVisible(uint32_t tick) {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int viewLeft;
        int visibleWidth;
        int playheadX;
        int followPadding;
        int followBoundary;
        int newViewLeft;
        int maxViewLeft;
        wxSize virtualSize;

        scrollPixelsX = 0;
        scrollPixelsY = 0;
        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        viewLeft = viewUnitsX * scrollPixelsX;
        visibleWidth = GetClientSize().GetWidth();
        if (visibleWidth <= 0) {
            return;
        }

        playheadX = TickToX(tick);
        followPadding = std::max(48, visibleWidth / 5);
        followBoundary = viewLeft + visibleWidth - followPadding;
        if (playheadX <= followBoundary) {
            return;
        }

        virtualSize = GetVirtualSize();
        maxViewLeft = std::max(0, virtualSize.GetWidth() - visibleWidth);
        newViewLeft = std::min(maxViewLeft, playheadX - (visibleWidth - followPadding));
        Scroll(newViewLeft / scrollPixelsX, viewUnitsY);
    }

    void JumpToTick(uint32_t tick) {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int visibleWidth;
        int targetX;
        int targetViewLeft;
        wxSize virtualSize;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        visibleWidth = GetClientSize().GetWidth();
        if (visibleWidth <= 0) {
            return;
        }
        targetX = TickToX(tick);
        targetViewLeft = targetX - visibleWidth / 4;
        virtualSize = GetVirtualSize();
        targetViewLeft = std::clamp(targetViewLeft, 0, std::max(0, virtualSize.GetWidth() - visibleWidth));
        /* Preserve vertical scroll position; only change horizontal. */
        Scroll(targetViewLeft / scrollPixelsX, viewUnitsY);
    }

    void ClearPlayhead() {
        m_showPlayhead = false;
        Refresh();
    }

    uint32_t GetVisibleDocumentEndTick() const {
        return GetDocumentEndTick();
    }

private:
    BAERmfEditorDocument *m_document;
    int m_selectedTrack;
    long m_selectedNote;
    std::vector<long> m_selectedNotes;
    bool m_dragging;
    DragMode m_dragMode;
    bool m_showPlayhead;
    uint32_t m_playheadTick;
    PianoRollSelectionKind m_selectedItemKind;
    int m_selectedAutomationLane;
    long m_selectedAutomationEvent;
    bool m_dragAutomationValid;
    AutomationHitInfo m_dragAutomationHit;
    BAERmfEditorNoteInfo m_dragOriginalNote;
    uint32_t m_dragStartTick;
    int m_dragStartNote;
    uint16_t m_newNoteBank;
    unsigned char m_newNoteProgram;
    std::function<void()> m_selectionChangedCallback;
    std::function<void(uint32_t)> m_seekRequestedCallback;
    std::function<void(uint16_t, unsigned char, unsigned char, uint32_t, int)> m_notePreviewRequestedCallback;
    std::function<void()> m_notePreviewStopRequestedCallback;
    std::function<void(wxString const &)> m_beginUndoCallback;
    std::function<void(wxString const &)> m_commitUndoCallback;
    std::function<void()> m_cancelUndoCallback;
    bool m_dragUndoActive = false;
    wxString m_dragUndoLabel;
    float m_zoomScale;
    bool m_selectBoxActive = false;
    wxPoint m_selectBoxStart;
    wxPoint m_selectBoxCurrent;
    std::vector<long> m_dragOriginalNoteIndices;
    std::vector<BAERmfEditorNoteInfo> m_dragOriginalNotes;
    int m_lastPreviewDragNote;

    bool ScrollHorizontallyWithWheel(wxMouseEvent const &event, int stepMultiplier) {
        int wheelDelta;
        int wheelRotation;
        int steps;
        int linesPerStep;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int currentLeft;
        int clientWidth;
        int maxLeft;
        int newLeft;
        wxSize virtualSize;

        wheelDelta = event.GetWheelDelta();
        wheelRotation = event.GetWheelRotation();
        if (wheelDelta == 0 || wheelRotation == 0) {
            return false;
        }
        steps = wheelRotation / wheelDelta;
        if (steps == 0) {
            steps = (wheelRotation > 0) ? 1 : -1;
        }
        linesPerStep = std::max(1, event.GetLinesPerAction());
        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            return false;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        currentLeft = viewUnitsX * scrollPixelsX;
        clientWidth = std::max(1, GetClientSize().GetWidth());
        virtualSize = GetVirtualSize();
        maxLeft = std::max(0, virtualSize.GetWidth() - clientWidth);
        newLeft = currentLeft - (steps * linesPerStep * scrollPixelsX * std::max(1, stepMultiplier));
        newLeft = std::clamp(newLeft, 0, maxLeft);
        Scroll(newLeft / scrollPixelsX, viewUnitsY);
        return true;
    }

    void RequestSingleNotePreview(BAERmfEditorNoteInfo const &noteInfo) {
        if (!m_notePreviewRequestedCallback || !HasTrack()) {
            return;
        }
        m_notePreviewRequestedCallback(noteInfo.bank,
                                       noteInfo.program,
                                       noteInfo.note,
                                       noteInfo.durationTicks,
                                       m_selectedTrack);
    }

    void RequestStopNotePreview() {
        if (m_notePreviewStopRequestedCallback) {
            m_notePreviewStopRequestedCallback();
        }
    }

    static wxRect NormalizeRect(wxPoint const &a, wxPoint const &b) {
        int left;
        int top;
        int right;
        int bottom;

        left = std::min(a.x, b.x);
        top = std::min(a.y, b.y);
        right = std::max(a.x, b.x);
        bottom = std::max(a.y, b.y);
        return wxRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
    }

    bool IsNoteSelected(long noteIndex) const {
        return std::find(m_selectedNotes.begin(), m_selectedNotes.end(), noteIndex) != m_selectedNotes.end();
    }

    void SelectSingleNote(long noteIndex) {
        m_selectedNotes.clear();
        if (noteIndex >= 0) {
            m_selectedNotes.push_back(noteIndex);
            m_selectedNote = noteIndex;
            m_selectedItemKind = PianoRollSelectionKind::Note;
        } else {
            m_selectedNote = -1;
            m_selectedItemKind = PianoRollSelectionKind::None;
        }
    }

    void UpdatePrimarySelectedNote() {
        if (!m_selectedNotes.empty()) {
            m_selectedNote = m_selectedNotes.front();
            m_selectedItemKind = PianoRollSelectionKind::Note;
        } else {
            m_selectedNote = -1;
            if (m_selectedItemKind == PianoRollSelectionKind::Note) {
                m_selectedItemKind = PianoRollSelectionKind::None;
            }
        }
    }

    void UpdateAreaSelection(wxRect const &selectionRect) {
        uint32_t noteCount;

        m_selectedNotes.clear();
        if (!HasTrack()) {
            UpdatePrimarySelectedNote();
            return;
        }
        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) != BAE_NO_ERROR) {
            UpdatePrimarySelectedNote();
            return;
        }
        for (uint32_t noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
            BAERmfEditorNoteInfo noteInfo;
            wxRect noteRect;

            if (!GetNoteInfo(noteIndex, &noteInfo)) {
                continue;
            }
            noteRect = BuildNoteRect(noteInfo);
            if (noteRect.Intersects(selectionRect)) {
                m_selectedNotes.push_back(static_cast<long>(noteIndex));
            }
        }
        UpdatePrimarySelectedNote();
    }

    void BeginUndoAction(wxString const &label) {
        if (m_beginUndoCallback) {
            m_beginUndoCallback(label);
        }
    }

    void CommitUndoAction(wxString const &label) {
        if (m_commitUndoCallback) {
            m_commitUndoCallback(label);
        }
    }

    void CancelUndoAction() {
        if (m_cancelUndoCallback) {
            m_cancelUndoCallback();
        }
    }

    void ApplyZoomStep(int wheelRotation, wxPoint clientPoint) {
        double oldScale;
        uint32_t anchorTick;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int oldViewLeft;
        int oldViewTop;
        int logicalAnchorX;
        int newAnchorX;
        int newViewLeft;
        int maxViewLeft;
        int clientWidth;
        wxSize virtualSize;

        oldScale = m_zoomScale;
        if (wheelRotation > 0) {
            m_zoomScale = std::min(8.0f, m_zoomScale * 1.15f);
        } else {
            m_zoomScale = std::max(0.2f, m_zoomScale / 1.15f);
        }
        if (std::abs(m_zoomScale - oldScale) < 0.0001f) {
            return;
        }

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            scrollPixelsX = 1;
        }
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        oldViewLeft = viewUnitsX * scrollPixelsX;
        oldViewTop = viewUnitsY * scrollPixelsY;
        logicalAnchorX = oldViewLeft + clientPoint.x;
        anchorTick = XToTick(logicalAnchorX);

        UpdateVirtualSize();
        newAnchorX = TickToX(anchorTick);
        newViewLeft = std::max(0, newAnchorX - clientPoint.x);
        clientWidth = GetClientSize().GetWidth();
        virtualSize = GetVirtualSize();
        maxViewLeft = std::max(0, virtualSize.GetWidth() - std::max(1, clientWidth));
        newViewLeft = std::clamp(newViewLeft, 0, maxViewLeft);
        Scroll(newViewLeft / scrollPixelsX, oldViewTop / scrollPixelsY);
        Refresh();
    }

    wxRect BuildNoteRect(BAERmfEditorNoteInfo const &noteInfo) const {
        return wxRect(TickToX(noteInfo.startTick),
                      NoteToY(noteInfo.note),
                      std::max(8, TickToX(noteInfo.startTick + noteInfo.durationTicks) - TickToX(noteInfo.startTick)),
                      kNoteHeight - 1);
    }

    DragMode GetDragModeForPoint(wxRect const &noteRect, wxPoint point) const {
        if (std::abs(point.x - noteRect.GetLeft()) <= kResizeHandlePixels) {
            return DragMode::ResizeLeft;
        }
        if (std::abs(point.x - noteRect.GetRight()) <= kResizeHandlePixels) {
            return DragMode::ResizeRight;
        }
        return DragMode::Move;
    }

    void UpdateHoverCursor(wxPoint point) {
        BAERmfEditorNoteInfo noteInfo;
        AutomationHitInfo automationHit;
        long hitNote;

        if (!HasTrack()) {
            SetCursor(wxNullCursor);
            return;
        }
        hitNote = HitTestNote(point, &noteInfo);
        if (hitNote < 0) {
            if (HitTestAutomation(point, &automationHit)) {
                DragMode mode;

                mode = GetDragModeForAutomation(automationHit, point);
                if (mode == DragMode::ResizeLeft || mode == DragMode::ResizeRight) {
                    SetCursor(wxCursor(wxCURSOR_SIZEWE));
                } else {
                    SetCursor(wxCursor(wxCURSOR_HAND));
                }
                return;
            }
            SetCursor(wxNullCursor);
            return;
        }
        wxRect noteRect = BuildNoteRect(noteInfo);
        DragMode mode = GetDragModeForPoint(noteRect, point);
        if (mode == DragMode::ResizeLeft || mode == DragMode::ResizeRight) {
            SetCursor(wxCursor(wxCURSOR_SIZEWE));
        } else {
            SetCursor(wxCursor(wxCURSOR_HAND));
        }
    }


    uint16_t GetTicksPerQuarter() const {
        uint16_t ticksPerQuarter;

        ticksPerQuarter = 480;
        if (m_document) {
            BAERmfEditorDocument_GetTicksPerQuarter(m_document, &ticksPerQuarter);
        }
        return ticksPerQuarter ? ticksPerQuarter : 480;
    }

    int GetPixelsPerQuarter() const {
        int pixels;

        /* Keep grid spacing tied to musical time (ticks), not playback tempo. */
        pixels = static_cast<int>(static_cast<double>(kBasePixelsPerQuarter) * m_zoomScale);
        return std::clamp(pixels, 16, 768);
    }

    int TickToX(uint32_t tick) const {
        return kPianoRollLeftGutter + static_cast<int>((static_cast<uint64_t>(tick) * GetPixelsPerQuarter()) / GetTicksPerQuarter());
    }

    uint32_t XToTick(int x) const {
        int localX;

        localX = std::max(0, x - kPianoRollLeftGutter);
        return static_cast<uint32_t>((static_cast<uint64_t>(localX) * GetTicksPerQuarter()) / GetPixelsPerQuarter());
    }

    int NoteToY(int note) const {
        return kPianoRollTopGutter + (127 - note) * kNoteHeight;
    }

    int YToNote(int y) const {
        int index;

        index = (y - kPianoRollTopGutter) / kNoteHeight;
        index = std::clamp(index, 0, 127);
        return 127 - index;
    }

    uint32_t SnapTick(uint32_t tick) const {
        uint32_t snapTicks;

        snapTicks = GetSnapTicks();
        return (tick / snapTicks) * snapTicks;
    }

    uint32_t GetSnapTicks() const {
        uint16_t ticksPerQuarter;

        ticksPerQuarter = GetTicksPerQuarter();
        return std::max<uint32_t>(1, static_cast<uint32_t>(ticksPerQuarter) / 4);
    }

    bool HasTrack() const {
        return m_document && m_selectedTrack >= 0;
    }

    bool GetNoteInfo(uint32_t noteIndex, BAERmfEditorNoteInfo *noteInfo) const {
        if (!HasTrack() || !noteInfo) {
            return false;
        }
        return BAERmfEditorDocument_GetNoteInfo(m_document, static_cast<uint16_t>(m_selectedTrack), noteIndex, noteInfo) == BAE_NO_ERROR;
    }

    bool IsAutomationLaneVisible(int laneIndex) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        return m_document && laneIndex >= 0 && laneIndex < static_cast<int>(lanes.size()) && (laneIndex == kTempoLaneIndex || HasTrack());
    }

    int AutomationValueFromY(int laneIndex, int y) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        AutomationLaneDescriptor const &lane = lanes[laneIndex];
        int laneTop;
        int laneBottom;
        int value;

        laneTop = GetAutomationLaneY(laneIndex);
        laneBottom = laneTop + kAutomationLaneHeight;
        y = std::clamp(y, laneTop, laneBottom - 1);
        value = ((laneBottom - 1 - y) * lane.maxValue) / std::max(1, kAutomationLaneHeight - 1);
        return std::clamp(value, 0, lane.maxValue);
    }

    bool HitTestAutomation(wxPoint point, AutomationHitInfo *outHitInfo = nullptr) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        int laneIndex;

        if (!m_document || point.y < GetAutomationAreaTop()) {
            return false;
        }
        for (laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            AutomationLaneDescriptor const &lane = lanes[laneIndex];
            uint32_t eventCount;
            uint32_t eventIndex;
            int laneTop;
            int laneBottom;

            if (!IsAutomationLaneVisible(laneIndex)) {
                continue;
            }
            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            if (point.y < laneTop || point.y >= laneBottom) {
                continue;
            }
            if (lane.kind == AutomationLaneKind::Tempo) {
                eventCount = 0;
                if (BAERmfEditorDocument_GetTempoEventCount(m_document, &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                if (eventCount == 0) {
                    /* Default tempo bar spans the whole document – make it selectable. */
                    if (point.x >= kPianoRollLeftGutter) {
                        if (outHitInfo) {
                            uint32_t defaultBpm = 120;
                            BAERmfEditorDocument_GetTempoBPM(m_document, &defaultBpm);
                            outHitInfo->laneIndex = laneIndex;
                            outHitInfo->eventIndex = 0;
                            outHitInfo->tick = 0;
                            outHitInfo->endTick = GetDocumentEndTick();
                            outHitInfo->laneTop = laneTop;
                            outHitInfo->laneBottom = laneBottom;
                            outHitInfo->leftX = kPianoRollLeftGutter;
                            outHitInfo->rightX = TickToX(GetDocumentEndTick());
                            outHitInfo->value = static_cast<int>(defaultBpm);
                            outHitInfo->hasFollowingEvent = false;
                        }
                        return true;
                    }
                    continue;
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;
                    uint32_t microsecondsPerQuarter;
                    uint32_t nextTick;
                    int leftX;
                    int rightX;

                    if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, &tick, &microsecondsPerQuarter) != BAE_NO_ERROR) {
                        continue;
                    }
                    nextTick = GetDocumentEndTick();
                    if (eventIndex + 1 < eventCount) {
                        uint32_t nextMicrosecondsPerQuarter;

                        BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex + 1, &nextTick, &nextMicrosecondsPerQuarter);
                    }
                    leftX = TickToX(tick);
                    rightX = TickToX(std::max(tick + 1, nextTick));
                    if (point.x >= leftX && point.x < rightX) {
                        if (outHitInfo) {
                            outHitInfo->laneIndex = laneIndex;
                            outHitInfo->eventIndex = eventIndex;
                            outHitInfo->tick = tick;
                            outHitInfo->endTick = nextTick;
                            outHitInfo->laneTop = laneTop;
                            outHitInfo->laneBottom = laneBottom;
                            outHitInfo->leftX = leftX;
                            outHitInfo->rightX = rightX;
                            outHitInfo->value = microsecondsPerQuarter ? static_cast<int>(60000000UL / microsecondsPerQuarter) : 120;
                            outHitInfo->hasFollowingEvent = (eventIndex + 1) < eventCount;
                        }
                        return true;
                    }
                }
            } else if (lane.kind == AutomationLaneKind::Controller) {
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                              static_cast<uint16_t>(m_selectedTrack),
                                                              lane.controller,
                                                              &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;
                    unsigned char value;
                    uint32_t nextTick;
                    int leftX;
                    int rightX;

                    if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                             static_cast<uint16_t>(m_selectedTrack),
                                                             lane.controller,
                                                             eventIndex,
                                                             &tick,
                                                             &value) != BAE_NO_ERROR) {
                        continue;
                    }
                    nextTick = GetDocumentEndTick();
                    if (eventIndex + 1 < eventCount) {
                        unsigned char nextValue;

                        BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                             static_cast<uint16_t>(m_selectedTrack),
                                                             lane.controller,
                                                             eventIndex + 1,
                                                             &nextTick,
                                                             &nextValue);
                    }
                    leftX = TickToX(tick);
                    rightX = TickToX(std::max(tick + 1, nextTick));
                    if (point.x >= leftX && point.x < rightX) {
                        if (outHitInfo) {
                            outHitInfo->laneIndex = laneIndex;
                            outHitInfo->eventIndex = eventIndex;
                            outHitInfo->tick = tick;
                            outHitInfo->endTick = nextTick;
                            outHitInfo->laneTop = laneTop;
                            outHitInfo->laneBottom = laneBottom;
                            outHitInfo->leftX = leftX;
                            outHitInfo->rightX = rightX;
                            outHitInfo->value = value;
                            outHitInfo->hasFollowingEvent = (eventIndex + 1) < eventCount;
                        }
                        return true;
                    }
                }
            } else {
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document,
                                                                     static_cast<uint16_t>(m_selectedTrack),
                                                                     &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;
                    uint16_t value;
                    uint32_t nextTick;
                    int leftX;
                    int rightX;

                    if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                                    static_cast<uint16_t>(m_selectedTrack),
                                                                    eventIndex,
                                                                    &tick,
                                                                    &value) != BAE_NO_ERROR) {
                        continue;
                    }
                    nextTick = GetDocumentEndTick();
                    if (eventIndex + 1 < eventCount) {
                        uint16_t nextValue;

                        BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                                    static_cast<uint16_t>(m_selectedTrack),
                                                                    eventIndex + 1,
                                                                    &nextTick,
                                                                    &nextValue);
                    }
                    leftX = TickToX(tick);
                    rightX = TickToX(std::max(tick + 1, nextTick));
                    if (point.x >= leftX && point.x < rightX) {
                        if (outHitInfo) {
                            outHitInfo->laneIndex = laneIndex;
                            outHitInfo->eventIndex = eventIndex;
                            outHitInfo->tick = tick;
                            outHitInfo->endTick = nextTick;
                            outHitInfo->laneTop = laneTop;
                            outHitInfo->laneBottom = laneBottom;
                            outHitInfo->leftX = leftX;
                            outHitInfo->rightX = rightX;
                            outHitInfo->value = static_cast<int>(value);
                            outHitInfo->hasFollowingEvent = (eventIndex + 1) < eventCount;
                        }
                        return true;
                    }
                }
            }
        }
        return false;
    }

    int GetAutomationLaneIndexFromY(int y) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        int laneIndex;

        for (laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            int laneTop;
            int laneBottom;

            if (!IsAutomationLaneVisible(laneIndex)) {
                continue;
            }
            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            if (y >= laneTop && y < laneBottom) {
                return laneIndex;
            }
        }
        return -1;
    }

    bool GetAutomationEventCount(int laneIndex, uint32_t *outCount) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (!outCount || !m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            return BAERmfEditorDocument_GetTempoEventCount(m_document, outCount) == BAE_NO_ERROR;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            return BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                             static_cast<uint16_t>(m_selectedTrack),
                                                             lanes[laneIndex].controller,
                                                             outCount) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document,
                                                                static_cast<uint16_t>(m_selectedTrack),
                                                                outCount) == BAE_NO_ERROR;
    }

    bool GetAutomationEvent(int laneIndex, uint32_t eventIndex, uint32_t *outTick, int *outValue) const {
        if (!outTick || !outValue || !m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            uint32_t microsecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, outTick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                return false;
            }
            *outValue = static_cast<int>(60000000UL / microsecondsPerQuarter);
            return true;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            unsigned char value;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lanes[laneIndex].controller,
                                                     eventIndex,
                                                     outTick,
                                                     &value) != BAE_NO_ERROR) {
                return false;
            }
            *outValue = value;
            return true;
        }
        {
            uint16_t value;

            if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            eventIndex,
                                                            outTick,
                                                            &value) != BAE_NO_ERROR) {
                return false;
            }
            *outValue = static_cast<int>(value);
            return true;
        }
    }

    bool AddAutomationEvent(int laneIndex, uint32_t tick, int value) {
        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            return BAERmfEditorDocument_AddTempoEvent(m_document,
                                                      tick,
                                                      static_cast<uint32_t>(60000000UL / std::max(1, value))) == BAE_NO_ERROR;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            return BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                        static_cast<uint16_t>(m_selectedTrack),
                                                        lanes[laneIndex].controller,
                                                        tick,
                                                        static_cast<unsigned char>(value)) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_AddTrackPitchBendEvent(m_document,
                                                           static_cast<uint16_t>(m_selectedTrack),
                                                           tick,
                                                           static_cast<uint16_t>(value)) == BAE_NO_ERROR;
    }

    bool SetAutomationEvent(int laneIndex, uint32_t eventIndex, uint32_t tick, int value) {
        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            return BAERmfEditorDocument_SetTempoEvent(m_document,
                                                      eventIndex,
                                                      tick,
                                                      static_cast<uint32_t>(60000000UL / std::max(1, value))) == BAE_NO_ERROR;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            return BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                        static_cast<uint16_t>(m_selectedTrack),
                                                        lanes[laneIndex].controller,
                                                        eventIndex,
                                                        tick,
                                                        static_cast<unsigned char>(value)) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_SetTrackPitchBendEvent(m_document,
                                                           static_cast<uint16_t>(m_selectedTrack),
                                                           eventIndex,
                                                           tick,
                                                           static_cast<uint16_t>(value)) == BAE_NO_ERROR;
    }

    bool GetTrackCCValueAtTick(unsigned char cc, uint32_t tick, unsigned char *outValue) const {
        uint32_t count;

        if (!m_document || !HasTrack()) {
            return false;
        }
        count = 0;
        if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                      static_cast<uint16_t>(m_selectedTrack),
                                                      cc,
                                                      &count) != BAE_NO_ERROR) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t eventTick;
            unsigned char eventValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     cc,
                                                     i,
                                                     &eventTick,
                                                     &eventValue) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick == tick) {
                if (outValue) {
                    *outValue = eventValue;
                }
                return true;
            }
        }
        return false;
    }

    bool UpsertTrackCCAtTick(unsigned char cc, uint32_t tick, unsigned char value) {
        uint32_t count;

        if (!m_document || !HasTrack()) {
            return false;
        }
        count = 0;
        if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                      static_cast<uint16_t>(m_selectedTrack),
                                                      cc,
                                                      &count) != BAE_NO_ERROR) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t eventTick;
            unsigned char eventValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     cc,
                                                     i,
                                                     &eventTick,
                                                     &eventValue) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick == tick) {
                return BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            cc,
                                                            i,
                                                            tick,
                                                            value) == BAE_NO_ERROR;
            }
        }
        return BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                    static_cast<uint16_t>(m_selectedTrack),
                                                    cc,
                                                    tick,
                                                    value) == BAE_NO_ERROR;
    }

    DragMode GetDragModeForAutomation(AutomationHitInfo const &hitInfo, wxPoint point) const {
        if (std::abs(point.x - hitInfo.leftX) <= kResizeHandlePixels) {
            return DragMode::ResizeLeft;
        }
        if (hitInfo.hasFollowingEvent && std::abs(point.x - hitInfo.rightX) <= kResizeHandlePixels) {
            return DragMode::ResizeRight;
        }
        return DragMode::Move;
    }

    bool EditSelectedNote() {
        BAERmfEditorNoteInfo noteInfo;
        BAERmfEditorTrackInfo trackInfo;
        bool hasResonance;
        unsigned char resonanceValue;
        bool hasBrightness;
        unsigned char brightnessValue;
        bool setResonance;
        bool setBrightness;

        if (!GetSelectedNoteInfo(&noteInfo)) {
            return false;
        }
        hasResonance = GetTrackCCValueAtTick(71, noteInfo.startTick, &resonanceValue);
        if (!hasResonance) {
            resonanceValue = 64;
        }
        hasBrightness = GetTrackCCValueAtTick(74, noteInfo.startTick, &brightnessValue);
        if (!hasBrightness) {
            brightnessValue = 64;
        }
        NoteEditDialog dialog(this,
                              noteInfo,
                              hasResonance,
                              resonanceValue,
                              hasBrightness,
                              brightnessValue);
        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }
        if (!dialog.GetNoteInfo(&noteInfo)) {
            wxMessageBox("Invalid note values.", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        dialog.GetFilterSettings(&setResonance, &resonanceValue, &setBrightness, &brightnessValue);
        if ((setResonance || setBrightness) &&
            BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(m_selectedTrack), &trackInfo) == BAE_NO_ERROR &&
            trackInfo.channel != noteInfo.channel) {
            int answer;

            answer = wxMessageBox(wxString::Format("This note is on channel %u but the track channel is %u.\n"
                                                   "Filter CC events are track-channel events and will be applied to channel %u at this note tick.\n\n"
                                                   "Continue?",
                                                   static_cast<unsigned>(noteInfo.channel + 1),
                                                   static_cast<unsigned>(trackInfo.channel + 1),
                                                   static_cast<unsigned>(trackInfo.channel + 1)),
                                  "Edit Note",
                                  wxYES_NO | wxICON_WARNING,
                                  this);
            if (answer != wxYES) {
                return false;
            }
        }
        BeginUndoAction("Edit Note");
        if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_selectedTrack),
                                             static_cast<uint32_t>(m_selectedNote),
                                             &noteInfo) != BAE_NO_ERROR) {
            CancelUndoAction();
            wxMessageBox("Failed to update note.", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        if (setResonance && !UpsertTrackCCAtTick(71, noteInfo.startTick, resonanceValue)) {
            CancelUndoAction();
            wxMessageBox("Failed to set resonance automation (CC71).", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        if (setBrightness && !UpsertTrackCCAtTick(74, noteInfo.startTick, brightnessValue)) {
            CancelUndoAction();
            wxMessageBox("Failed to set brightness automation (CC74).", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        CommitUndoAction("Edit Note");
        UpdateVirtualSize();
        Refresh();
        return true;
    }

    bool EditAutomationEvent(int laneIndex, uint32_t eventIndex) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        AutomationLaneDescriptor const &lane = lanes[laneIndex];
        uint32_t eventCount;
        uint32_t durationTicks;
        bool hasFollowingEvent;
        uint32_t tick;
        int value;
        wxString valueLabel;
        wxString title;

        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        eventCount = 0;
        durationTicks = GetSnapTicks();
        hasFollowingEvent = false;
        GetAutomationEventCount(laneIndex, &eventCount);
        if (lane.kind == AutomationLaneKind::Tempo) {
            uint32_t microsecondsPerQuarter;

            if (eventCount == 0) {
                /* No explicit tempo events: edit the implicit default tempo.
                 * On confirmation this converts it to an explicit tick-0 event. */
                uint32_t defaultBpm = 120;
                uint32_t newTick;
                int newBpm;
                uint32_t newDuration;

                BAERmfEditorDocument_GetTempoBPM(m_document, &defaultBpm);
                AutomationEditDialog dialog(this,
                                            "Edit Default Tempo",
                                            "BPM",
                                            0,
                                            static_cast<int>(defaultBpm),
                                            lane.maxValue,
                                            GetDocumentEndTick(),
                                            false);
                if (dialog.ShowModal() != wxID_OK) {
                    return false;
                }
                if (!dialog.GetValues(&newTick, &newBpm, &newDuration)) {
                    wxMessageBox("Invalid tempo values.", "Edit Default Tempo", wxOK | wxICON_ERROR, this);
                    return false;
                }
                BeginUndoAction("Edit Default Tempo");
                if (BAERmfEditorDocument_AddTempoEvent(m_document,
                                                       newTick,
                                                       static_cast<uint32_t>(60000000UL / std::max(1, newBpm))) == BAE_NO_ERROR) {
                    CommitUndoAction("Edit Default Tempo");
                    m_selectedAutomationEvent = 0;
                    if (m_selectionChangedCallback) {
                        m_selectionChangedCallback();
                    }
                    UpdateVirtualSize();
                    Refresh();
                    return true;
                }
                CancelUndoAction();
                wxMessageBox("Failed to set default tempo.", "Edit Default Tempo", wxOK | wxICON_ERROR, this);
                return false;
            }

            if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, &tick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                return false;
            }
            value = static_cast<int>(60000000UL / microsecondsPerQuarter);
            valueLabel = "BPM";
            title = "Edit Tempo Event";
        } else if (lane.kind == AutomationLaneKind::Controller) {
            unsigned char controllerValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lane.controller,
                                                     eventIndex,
                                                     &tick,
                                                     &controllerValue) != BAE_NO_ERROR) {
                return false;
            }
            value = controllerValue;
            valueLabel = "Value";
            title = wxString::Format("Edit %s Event", lane.label);
        } else {
            uint16_t bendValue;

            if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            eventIndex,
                                                            &tick,
                                                            &bendValue) != BAE_NO_ERROR) {
                return false;
            }
            value = static_cast<int>(bendValue);
            valueLabel = "Value";
            title = "Edit Pitch Bend Event";
        }
        if (eventIndex + 1 < eventCount) {
            uint32_t nextTick;
            int nextValue;

            if (GetAutomationEvent(laneIndex, eventIndex + 1, &nextTick, &nextValue) && nextTick > tick) {
                durationTicks = nextTick - tick;
                hasFollowingEvent = true;
            }
        }
        AutomationEditDialog dialog(this, title, valueLabel, tick, value, lane.maxValue, durationTicks, hasFollowingEvent);
        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }
        if (!dialog.GetValues(&tick, &value, &durationTicks)) {
            wxMessageBox("Invalid automation values.", title, wxOK | wxICON_ERROR, this);
            return false;
        }
        BeginUndoAction(title);
        if (lane.kind == AutomationLaneKind::Tempo) {
            if (BAERmfEditorDocument_SetTempoEvent(m_document,
                                                   eventIndex,
                                                   tick,
                                                   static_cast<uint32_t>(60000000UL / std::max(1, value))) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update tempo event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        } else if (lane.kind == AutomationLaneKind::Controller) {
            if (BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lane.controller,
                                                     eventIndex,
                                                     tick,
                                                     static_cast<unsigned char>(value)) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update automation event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        } else {
            if (BAERmfEditorDocument_SetTrackPitchBendEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            eventIndex,
                                                            tick,
                                                            static_cast<uint16_t>(value)) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update pitch bend event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        }
        if (hasFollowingEvent) {
            uint32_t nextTickCurrent;
            uint32_t nextNextTick;
            uint32_t minNextTick;
            uint32_t maxNextTick;
            uint32_t requestedNextTick;
            int nextValueCurrent;

            if (!GetAutomationEvent(laneIndex, eventIndex + 1, &nextTickCurrent, &nextValueCurrent)) {
                CancelUndoAction();
                wxMessageBox("Failed to read following event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }

            nextNextTick = GetDocumentEndTick();
            if (eventIndex + 2 < eventCount) {
                int nextNextValue;
                GetAutomationEvent(laneIndex, eventIndex + 2, &nextNextTick, &nextNextValue);
            }

            uint32_t snapTicks;

            snapTicks = GetSnapTicks();
            minNextTick = tick + snapTicks;
            maxNextTick = minNextTick;
            if (nextNextTick > snapTicks) {
                maxNextTick = std::max(minNextTick, nextNextTick - snapTicks);
            }
            requestedNextTick = tick + std::max<uint32_t>(snapTicks, durationTicks);
            requestedNextTick = std::clamp(requestedNextTick, minNextTick, maxNextTick);

            if (!SetAutomationEvent(laneIndex, eventIndex + 1, requestedNextTick, nextValueCurrent)) {
                CancelUndoAction();
                wxMessageBox("Failed to update event duration.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        }
        CommitUndoAction(title);
        UpdateVirtualSize();
        Refresh();
        return true;
    }

    void DeleteCurrentSelection() {
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            DeleteSelectedNote();
            return;
        }
        if (m_selectedItemKind == PianoRollSelectionKind::Automation && m_selectedAutomationLane >= 0 && m_selectedAutomationEvent >= 0) {
            BAEResult result;

            BeginUndoAction("Delete Event");
            std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

            if (lanes[m_selectedAutomationLane].kind == AutomationLaneKind::Tempo) {
                uint32_t tempoCount = 0;
                BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount);
                if (tempoCount == 0) {
                    CancelUndoAction();
                    return; /* Cannot delete the implicit default tempo. */
                }
                result = BAERmfEditorDocument_DeleteTempoEvent(m_document, static_cast<uint32_t>(m_selectedAutomationEvent));
            } else if (lanes[m_selectedAutomationLane].kind == AutomationLaneKind::Controller) {
                result = BAERmfEditorDocument_DeleteTrackCCEvent(m_document,
                                                                 static_cast<uint16_t>(m_selectedTrack),
                                                                 lanes[m_selectedAutomationLane].controller,
                                                                 static_cast<uint32_t>(m_selectedAutomationEvent));
            } else {
                result = BAERmfEditorDocument_DeleteTrackPitchBendEvent(m_document,
                                                                        static_cast<uint16_t>(m_selectedTrack),
                                                                        static_cast<uint32_t>(m_selectedAutomationEvent));
            }
            if (result == BAE_NO_ERROR) {
                CommitUndoAction("Delete Event");
                m_selectedItemKind = PianoRollSelectionKind::None;
                m_selectedAutomationLane = -1;
                m_selectedAutomationEvent = -1;
                UpdateVirtualSize();
                Refresh();
            } else {
                CancelUndoAction();
            }
        }
    }

    void OnEditSelectedItem(wxCommandEvent &) {
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            EditSelectedNote();
        } else if (m_selectedItemKind == PianoRollSelectionKind::Automation && m_selectedAutomationLane >= 0 && m_selectedAutomationEvent >= 0) {
            EditAutomationEvent(m_selectedAutomationLane, static_cast<uint32_t>(m_selectedAutomationEvent));
        }
    }

    void OnDeleteSelectedItem(wxCommandEvent &) {
        DeleteCurrentSelection();
    }

    int GetAutomationAreaTop() const {
        return kPianoRollTopGutter + (128 * kNoteHeight) + 10;
    }

    int GetAutomationLaneY(int laneIndex) const {
        return GetAutomationAreaTop() + laneIndex * (kAutomationLaneHeight + kAutomationLaneGap);
    }

    void ScrollToBottom() {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int clientHeight;
        int targetTop;
        wxSize virtualSize;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        clientHeight = GetClientSize().GetHeight();
        if (clientHeight <= 0) {
            return;
        }
        virtualSize = GetVirtualSize();
        targetTop = std::max(0, virtualSize.GetHeight() - clientHeight);
        Scroll(0, targetTop / scrollPixelsY);
    }

    void ScrollToMidiContentCenter() {
        uint32_t noteCount;
        int minNote;
        int maxNote;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int clientHeight;
        int contentTop;
        int contentBottom;
        int contentCenter;
        int targetTop;
        wxSize virtualSize;

        if (!HasTrack()) {
            ScrollToBottom();
            return;
        }

        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) != BAE_NO_ERROR || noteCount == 0) {
            ScrollToBottom();
            return;
        }

        minNote = 127;
        maxNote = 0;
        for (uint32_t i = 0; i < noteCount; ++i) {
            BAERmfEditorNoteInfo noteInfo;
            if (GetNoteInfo(i, &noteInfo)) {
                minNote = std::min(minNote, static_cast<int>(noteInfo.note));
                maxNote = std::max(maxNote, static_cast<int>(noteInfo.note));
            }
        }

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        clientHeight = GetClientSize().GetHeight();
        if (clientHeight <= 0) {
            return;
        }

        contentTop = NoteToY(maxNote);
        contentBottom = NoteToY(minNote) + kNoteHeight;
        contentCenter = (contentTop + contentBottom) / 2;
        targetTop = contentCenter - (clientHeight / 2);

        virtualSize = GetVirtualSize();
        targetTop = std::clamp(targetTop, 0, std::max(0, virtualSize.GetHeight() - clientHeight));
        Scroll(viewUnitsX, targetTop / scrollPixelsY);
    }

    uint32_t GetDocumentEndTick() const {
        uint32_t lastTick;

        lastTick = GetTicksPerQuarter() * 8;
        if (!m_document) {
            return lastTick;
        }
        if (HasTrack()) {
            uint32_t noteCount;
            uint32_t noteIndex;

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) == BAE_NO_ERROR) {
                for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                    BAERmfEditorNoteInfo noteInfo;

                    if (GetNoteInfo(noteIndex, &noteInfo)) {
                        lastTick = std::max(lastTick, noteInfo.startTick + noteInfo.durationTicks + GetTicksPerQuarter());
                    }
                }
            }
            std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

            for (AutomationLaneDescriptor const &lane : lanes) {
                uint32_t eventCount;
                uint32_t eventIndex;

                if (lane.kind == AutomationLaneKind::Tempo) {
                    continue;
                }
                eventCount = 0;
                if (lane.kind == AutomationLaneKind::Controller) {
                    if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                                  static_cast<uint16_t>(m_selectedTrack),
                                                                  lane.controller,
                                                                  &eventCount) != BAE_NO_ERROR) {
                        continue;
                    }
                } else {
                    if (BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document,
                                                                         static_cast<uint16_t>(m_selectedTrack),
                                                                         &eventCount) != BAE_NO_ERROR) {
                        continue;
                    }
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;

                    if (lane.kind == AutomationLaneKind::Controller) {
                        unsigned char value;

                        if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                                 static_cast<uint16_t>(m_selectedTrack),
                                                                 lane.controller,
                                                                 eventIndex,
                                                                 &tick,
                                                                 &value) == BAE_NO_ERROR) {
                            lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
                        }
                    } else {
                        uint16_t value;

                        if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                                        static_cast<uint16_t>(m_selectedTrack),
                                                                        eventIndex,
                                                                        &tick,
                                                                        &value) == BAE_NO_ERROR) {
                            lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
                        }
                    }
                }
            }
        }
        {
            uint32_t tempoCount;
            uint32_t tempoIndex;

            tempoCount = 0;
            if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) == BAE_NO_ERROR) {
                for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
                    uint32_t tick;
                    uint32_t microsecondsPerQuarter;

                    if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &tick, &microsecondsPerQuarter) == BAE_NO_ERROR) {
                        lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
                    }
                }
            }
        }
        return lastTick;
    }

    double TickToSeconds(uint32_t tick) const {
        uint16_t ticksPerQuarter;
        uint32_t tempoCount;
        uint32_t baseBpm;
        uint32_t currentTick;
        uint32_t currentMicrosecondsPerQuarter;
        double seconds;
        uint32_t tempoIndex;

        ticksPerQuarter = GetTicksPerQuarter();
        if (ticksPerQuarter == 0) {
            return 0.0;
        }
        baseBpm = 120;
        if (m_document) {
            BAERmfEditorDocument_GetTempoBPM(m_document, &baseBpm);
        }
        if (baseBpm == 0) {
            baseBpm = 120;
        }
        currentTick = 0;
        currentMicrosecondsPerQuarter = 60000000UL / baseBpm;
        seconds = 0.0;
        tempoCount = 0;
        if (!m_document || BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return (static_cast<double>(tick) * static_cast<double>(currentMicrosecondsPerQuarter)) /
                   (static_cast<double>(ticksPerQuarter) * 1000000.0);
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
                seconds += (static_cast<double>(eventTick - currentTick) * static_cast<double>(currentMicrosecondsPerQuarter)) /
                           (static_cast<double>(ticksPerQuarter) * 1000000.0);
            }
            currentTick = eventTick;
            if (eventMicrosecondsPerQuarter > 0) {
                currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
            }
        }
        if (tick > currentTick) {
            seconds += (static_cast<double>(tick - currentTick) * static_cast<double>(currentMicrosecondsPerQuarter)) /
                       (static_cast<double>(ticksPerQuarter) * 1000000.0);
        }
        return seconds;
    }

    void DrawAutomationLanes(wxDC &dc, wxSize const &virtualSize, wxRect const &visibleRect) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        int laneIndex;
        int visibleLeft;
        int visibleRight;

        visibleLeft = std::max(0, visibleRect.GetLeft());
        visibleRight = std::min(virtualSize.GetWidth(), visibleRect.GetRight());

        for (laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            AutomationLaneDescriptor const &lane = lanes[laneIndex];
            int laneTop;
            int laneBottom;
            int laneVisibleLeft;
            int laneVisibleWidth;

            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            if (laneBottom < visibleRect.GetTop() || laneTop > visibleRect.GetBottom()) {
                continue;
            }
            laneVisibleLeft = std::max(kPianoRollLeftGutter, visibleLeft);
            laneVisibleWidth = std::max(0, visibleRight - laneVisibleLeft);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(32, 34, 38)));
            if (laneVisibleWidth > 0) {
                dc.DrawRectangle(laneVisibleLeft, laneTop, laneVisibleWidth, kAutomationLaneHeight);
            }
            dc.SetBrush(wxBrush(wxColour(42, 44, 50)));
            dc.DrawRectangle(0, laneTop, kPianoRollLeftGutter, kAutomationLaneHeight);
            dc.SetPen(wxPen(wxColour(68, 72, 78)));
            dc.DrawLine(0, laneBottom, visibleRight, laneBottom);
            dc.SetTextForeground(wxColour(214, 217, 222));
            dc.DrawText(lane.label, 8, laneTop + (kAutomationLaneHeight / 2) - 8);
            if (lane.bipolar) {
                dc.SetPen(wxPen(wxColour(84, 88, 96)));
                dc.DrawLine(kPianoRollLeftGutter, laneTop + (kAutomationLaneHeight / 2), visibleRight, laneTop + (kAutomationLaneHeight / 2));
            }
            if (!m_document) {
                continue;
            }
            if (lane.kind == AutomationLaneKind::Tempo) {
                uint32_t tempoCount;
                uint32_t tempoIndex;

                tempoCount = 0;
                if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR || tempoCount == 0) {
                    uint32_t bpm;
                    int barHeight;

                    bpm = 120;
                    BAERmfEditorDocument_GetTempoBPM(m_document, &bpm);
                    barHeight = std::clamp((static_cast<int>(bpm) * kAutomationLaneHeight) / 240, 2, kAutomationLaneHeight);
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(lane.color));
                    dc.DrawRectangle(kPianoRollLeftGutter, laneBottom - barHeight, virtualSize.GetWidth() - kPianoRollLeftGutter, barHeight);
                } else {
                    for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
                        uint32_t tick;
                        uint32_t microsecondsPerQuarter;
                        uint32_t nextTick;
                        uint32_t bpm;
                        int x0;
                        int x1;
                        int barHeight;

                        if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &tick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                            continue;
                        }
                        nextTick = GetDocumentEndTick();
                        if (tempoIndex + 1 < tempoCount) {
                            uint32_t nextMicrosecondsPerQuarter;

                            BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex + 1, &nextTick, &nextMicrosecondsPerQuarter);
                        }
                        bpm = 60000000UL / microsecondsPerQuarter;
                        x0 = TickToX(tick);
                        x1 = TickToX(std::max(tick + 1, nextTick));
                        if (x1 < visibleLeft || x0 > visibleRight) {
                            continue;
                        }
                        barHeight = std::clamp((static_cast<int>(bpm) * kAutomationLaneHeight) / 240, 2, kAutomationLaneHeight);
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.SetBrush(wxBrush(lane.color));
                        dc.DrawRectangle(x0, laneBottom - barHeight, std::max(1, x1 - x0), barHeight);
                        if (m_selectedItemKind == PianoRollSelectionKind::Automation &&
                            m_selectedAutomationLane == laneIndex &&
                            m_selectedAutomationEvent == static_cast<long>(tempoIndex)) {
                            dc.SetBrush(*wxTRANSPARENT_BRUSH);
                            dc.SetPen(wxPen(wxColour(255, 232, 190), 2));
                            dc.DrawRectangle(x0, laneTop + 1, std::max(2, x1 - x0), kAutomationLaneHeight - 2);
                        }
                    }
                }
            } else if (HasTrack() && lane.kind == AutomationLaneKind::Controller) {
                uint32_t eventCount;
                uint32_t eventIndex;
                unsigned char controller;

                controller = lane.controller;
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, static_cast<uint16_t>(m_selectedTrack), controller, &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;
                    unsigned char value;
                    uint32_t nextTick;
                    int x0;
                    int x1;

                    if (BAERmfEditorDocument_GetTrackCCEvent(m_document, static_cast<uint16_t>(m_selectedTrack), controller, eventIndex, &tick, &value) != BAE_NO_ERROR) {
                        continue;
                    }
                    nextTick = GetDocumentEndTick();
                    if (eventIndex + 1 < eventCount) {
                        unsigned char nextValue;

                        BAERmfEditorDocument_GetTrackCCEvent(m_document, static_cast<uint16_t>(m_selectedTrack), controller, eventIndex + 1, &nextTick, &nextValue);
                    }
                    x0 = TickToX(tick);
                    x1 = TickToX(std::max(tick + 1, nextTick));
                    if (x1 < visibleLeft || x0 > visibleRight) {
                        continue;
                    }
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(lane.color));
                    if (lane.bipolar) {
                        int centerY;
                        int offsetY;
                        int centerValue;

                        centerY = laneTop + (kAutomationLaneHeight / 2);
                        centerValue = (lane.maxValue + 1) / 2;
                        offsetY = ((static_cast<int>(value) - centerValue) * (kAutomationLaneHeight / 2 - 2)) / std::max(1, centerValue);
                        if (offsetY >= 0) {
                            dc.DrawRectangle(x0, centerY - offsetY, std::max(1, x1 - x0), std::max(1, offsetY));
                        } else {
                            dc.DrawRectangle(x0, centerY, std::max(1, x1 - x0), std::max(1, -offsetY));
                        }
                    } else {
                        int barHeight;

                        barHeight = (static_cast<int>(value) * kAutomationLaneHeight) / 127;
                        barHeight = std::clamp(barHeight, 1, kAutomationLaneHeight);
                        dc.DrawRectangle(x0, laneBottom - barHeight, std::max(1, x1 - x0), barHeight);
                    }
                    if (m_selectedItemKind == PianoRollSelectionKind::Automation &&
                        m_selectedAutomationLane == laneIndex &&
                        m_selectedAutomationEvent == static_cast<long>(eventIndex)) {
                        dc.SetBrush(*wxTRANSPARENT_BRUSH);
                        dc.SetPen(wxPen(wxColour(238, 240, 244), 2));
                        dc.DrawRectangle(x0, laneTop + 1, std::max(2, x1 - x0), kAutomationLaneHeight - 2);
                    }
                }
            } else if (HasTrack() && lane.kind == AutomationLaneKind::PitchBend) {
                uint32_t eventCount;
                uint32_t eventIndex;

                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document,
                                                                     static_cast<uint16_t>(m_selectedTrack),
                                                                     &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;
                    uint16_t value;
                    uint32_t nextTick;
                    int x0;
                    int x1;
                    int centerY;
                    int offsetY;
                    int centerValue;

                    if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                                    static_cast<uint16_t>(m_selectedTrack),
                                                                    eventIndex,
                                                                    &tick,
                                                                    &value) != BAE_NO_ERROR) {
                        continue;
                    }
                    nextTick = GetDocumentEndTick();
                    if (eventIndex + 1 < eventCount) {
                        uint16_t nextValue;

                        BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                                    static_cast<uint16_t>(m_selectedTrack),
                                                                    eventIndex + 1,
                                                                    &nextTick,
                                                                    &nextValue);
                    }
                    x0 = TickToX(tick);
                    x1 = TickToX(std::max(tick + 1, nextTick));
                    if (x1 < visibleLeft || x0 > visibleRight) {
                        continue;
                    }
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(lane.color));
                    centerY = laneTop + (kAutomationLaneHeight / 2);
                    centerValue = (lane.maxValue + 1) / 2;
                    offsetY = ((static_cast<int>(value) - centerValue) * (kAutomationLaneHeight / 2 - 2)) / std::max(1, centerValue);
                    if (offsetY >= 0) {
                        dc.DrawRectangle(x0, centerY - offsetY, std::max(1, x1 - x0), std::max(1, offsetY));
                    } else {
                        dc.DrawRectangle(x0, centerY, std::max(1, x1 - x0), std::max(1, -offsetY));
                    }
                    if (m_selectedItemKind == PianoRollSelectionKind::Automation &&
                        m_selectedAutomationLane == laneIndex &&
                        m_selectedAutomationEvent == static_cast<long>(eventIndex)) {
                        dc.SetBrush(*wxTRANSPARENT_BRUSH);
                        dc.SetPen(wxPen(wxColour(238, 240, 244), 2));
                        dc.DrawRectangle(x0, laneTop + 1, std::max(2, x1 - x0), kAutomationLaneHeight - 2);
                    }
                }
            }
        }
    }

    void UpdateVirtualSize() {
        int width;
        int height;
        uint32_t lastTick;
        int laneCount;
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        width = 2400;
        laneCount = 0;
        for (int laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            if (IsAutomationLaneVisible(laneIndex)) {
                laneCount++;
            }
        }
        height = GetAutomationAreaTop() + (laneCount * kAutomationLaneHeight) + ((std::max(0, laneCount - 1)) * kAutomationLaneGap) + kPianoRollTopGutter;
        lastTick = GetDocumentEndTick();
        width = TickToX(lastTick) + 200;
        SetVirtualSize(width, height);
    }

    void DrawStickyRuler(wxDC &dc) {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int scrollX;
        int scrollY;
        int screenW;
        int rulerH;
        int rulerTop;
        int rulerBot;
        int relLeft;
        int relRight;
        uint32_t tick;
        uint32_t quarterTicks;
        uint32_t stepTicks;
        uint32_t tickStart;
        uint32_t tickEnd;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) scrollPixelsX = 1;
        if (scrollPixelsY <= 0) scrollPixelsY = 1;
        GetViewStart(&viewUnitsX, &viewUnitsY);
        scrollX = viewUnitsX * scrollPixelsX;
        scrollY = viewUnitsY * scrollPixelsY;
        screenW = GetClientSize().GetWidth();

        rulerH  = kPianoRollTopGutter;
        rulerTop = scrollY;
        rulerBot = scrollY + rulerH;

        // Ruler background
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(42, 44, 50)));
        dc.DrawRectangle(scrollX, rulerTop, screenW, rulerH);

        // Left gutter corner (darker)
        dc.SetBrush(wxBrush(wxColour(30, 32, 36)));
        dc.DrawRectangle(scrollX, rulerTop, kPianoRollLeftGutter, rulerH);

        // Gutter labels pinned to screen-left
        dc.SetTextForeground(wxColour(130, 135, 140));
        dc.DrawText("Beat", scrollX + 3, rulerTop + 1);
        dc.SetTextForeground(wxColour(100, 105, 110));
        dc.DrawText("Time", scrollX + 3, rulerTop + rulerH / 2);

        relLeft  = std::max(0, scrollX - kPianoRollLeftGutter);
        relRight = scrollX + screenW - kPianoRollLeftGutter;
        quarterTicks = GetTicksPerQuarter();
        stepTicks = std::max<uint32_t>(1, quarterTicks / 4);
        tickStart = (XToTick(kPianoRollLeftGutter + relLeft) / stepTicks) * stepTicks;
        tickEnd = XToTick(kPianoRollLeftGutter + relRight);

        // Sub-beat (16th-note) small ticks
        for (tick = tickStart; tick <= tickEnd; tick += stepTicks) {
            int vx;

            if ((tick % quarterTicks) == 0) continue;
            vx = TickToX(tick);
            dc.SetPen(wxPen(wxColour(70, 75, 82)));
            dc.DrawLine(vx, rulerBot - 5, vx, rulerBot - 1);
            if (tickEnd - tick < stepTicks) {
                break;
            }
        }

        // Quarter-note beat labels and taller ticks
        tickStart = (tickStart / quarterTicks) * quarterTicks;
        for (tick = tickStart; tick <= tickEnd; tick += quarterTicks) {
            int vx;
            int beatNum;
            double seconds;
            int mm;
            int ss;
            int ds;
            bool isBar;

            vx = TickToX(tick);
            beatNum = static_cast<int>(tick / quarterTicks) + 1;
            isBar = ((tick % (quarterTicks * 4)) == 0);
            seconds = TickToSeconds(tick);
            mm = static_cast<int>(seconds / 60.0);
            ss = static_cast<int>(seconds) % 60;
            ds = static_cast<int>((seconds - std::floor(seconds)) * 10.0);

            // Tick mark: taller at bar starts
            dc.SetPen(wxPen(isBar ? wxColour(190, 195, 200) : wxColour(130, 135, 140)));
            dc.DrawLine(vx, rulerTop + (isBar ? 2 : rulerH / 2), vx, rulerBot - 1);

            // Beat number (top row)
            dc.SetTextForeground(isBar ? wxColour(240, 242, 245) : wxColour(195, 198, 202));
            dc.DrawText(wxString::Format("%d", beatNum), vx + 2, rulerTop + 1);

            // Time code (bottom row)
            dc.SetTextForeground(wxColour(140, 145, 150));
            dc.DrawText(wxString::Format("%d:%02d.%d", mm, ss, ds), vx + 2, rulerTop + rulerH / 2);
            if (tickEnd - tick < quarterTicks) {
                break;
            }
        }

        // Bottom border
        dc.SetPen(wxPen(wxColour(70, 75, 82)));
        dc.DrawLine(scrollX, rulerBot - 1, scrollX + screenW, rulerBot - 1);

        // Left gutter right-edge (re-draw on top of ruler)
        dc.SetPen(wxPen(wxColour(100, 105, 110)));
        dc.DrawLine(scrollX + kPianoRollLeftGutter, rulerTop, scrollX + kPianoRollLeftGutter, rulerBot);

    }

    bool IsPointInStickyRuler(wxPoint logicalPoint) const {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int scrollY;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        scrollY = viewUnitsY * scrollPixelsY;
        return logicalPoint.y >= scrollY && logicalPoint.y < (scrollY + kPianoRollTopGutter);
    }

    long HitTestNote(wxPoint point, BAERmfEditorNoteInfo *noteInfoOut = nullptr) const {
        uint32_t noteCount;
        uint32_t noteIndex;

        if (!HasTrack()) {
            return -1;
        }
        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) != BAE_NO_ERROR) {
            return -1;
        }
        for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
            BAERmfEditorNoteInfo noteInfo;
            wxRect noteRect;

            if (!GetNoteInfo(noteIndex, &noteInfo)) {
                continue;
            }
            noteRect = wxRect(TickToX(noteInfo.startTick),
                              NoteToY(noteInfo.note),
                              std::max(8, TickToX(noteInfo.startTick + noteInfo.durationTicks) - TickToX(noteInfo.startTick)),
                              kNoteHeight - 1);
            if (noteRect.Contains(point)) {
                if (noteInfoOut) {
                    *noteInfoOut = noteInfo;
                }
                return static_cast<long>(noteIndex);
            }
        }
        return -1;
    }

    void DeleteSelectedNote() {
        std::vector<long> noteIndices;

        if (!HasTrack()) {
            return;
        }
        noteIndices = m_selectedNotes;
        if (noteIndices.empty() && m_selectedNote >= 0) {
            noteIndices.push_back(m_selectedNote);
        }
        if (noteIndices.empty()) {
            return;
        }
        std::sort(noteIndices.begin(), noteIndices.end());
        noteIndices.erase(std::unique(noteIndices.begin(), noteIndices.end()), noteIndices.end());
        BeginUndoAction("Delete Note");
        bool anyDeleted = false;
        for (auto it = noteIndices.rbegin(); it != noteIndices.rend(); ++it) {
            if (*it < 0) {
                continue;
            }
            if (BAERmfEditorDocument_DeleteNote(m_document,
                                                static_cast<uint16_t>(m_selectedTrack),
                                                static_cast<uint32_t>(*it)) == BAE_NO_ERROR) {
                anyDeleted = true;
            }
        }
        if (anyDeleted) {
            CommitUndoAction("Delete Note");
            m_selectedNotes.clear();
            m_selectedNote = -1;
            UpdateVirtualSize();
            Refresh();
        } else {
            CancelUndoAction();
        }
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize clientSize;
        wxRect visibleRect;
        wxRect notesVisibleRect;
        wxSize windowSize;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int viewLeft;
        int viewTop;
        int viewRight;
        int viewBottom;
        int note;
        int noteY;
        int gridTop;
        int gridBottom;
        uint32_t gridTick;
        uint32_t quarterTicks;
        uint32_t stepTicks;
        uint32_t tickStart;
        uint32_t tickEnd;

        PrepareDC(dc);
        clientSize = GetVirtualSize();
        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            scrollPixelsX = 1;
        }
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        windowSize = GetClientSize();
        viewLeft = viewUnitsX * scrollPixelsX;
        viewTop = viewUnitsY * scrollPixelsY;
        viewRight = std::min(clientSize.GetWidth(), viewLeft + std::max(1, windowSize.GetWidth()));
        viewBottom = std::min(clientSize.GetHeight(), viewTop + std::max(1, windowSize.GetHeight()));
        visibleRect = wxRect(viewLeft, viewTop, std::max(1, viewRight - viewLeft), std::max(1, viewBottom - viewTop));
        notesVisibleRect = wxRect(std::max(kPianoRollLeftGutter, visibleRect.GetLeft()),
                                  visibleRect.GetTop(),
                                  std::max(1, visibleRect.GetRight() - std::max(kPianoRollLeftGutter, visibleRect.GetLeft())),
                                  visibleRect.GetHeight());
        dc.SetBackground(wxBrush(wxColour(248, 248, 246)));
        dc.Clear();

        for (note = 0; note <= 127; ++note) {
            bool blackKey;
            wxColour fillColor;

            noteY = NoteToY(note);
            if (noteY + kNoteHeight < visibleRect.GetTop() || noteY > visibleRect.GetBottom()) {
                continue;
            }
            blackKey = (note % 12 == 1) || (note % 12 == 3) || (note % 12 == 6) || (note % 12 == 8) || (note % 12 == 10);
            fillColor = blackKey ? wxColour(235, 238, 240) : wxColour(248, 248, 246);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(fillColor));
            dc.DrawRectangle(visibleRect.GetLeft(), noteY, visibleRect.GetWidth(), kNoteHeight);
            dc.SetPen(wxPen(wxColour(220, 224, 226)));
            dc.DrawLine(kPianoRollLeftGutter, noteY, visibleRect.GetRight(), noteY);
            if (note % 12 == 0) {
                dc.SetTextForeground(wxColour(90, 90, 90));
                dc.DrawText(wxString::Format("C%d", note / 12), 6, noteY - 1);
            }
        }

        quarterTicks = GetTicksPerQuarter();
        stepTicks = std::max<uint32_t>(1, quarterTicks / 4);
        tickStart = (XToTick(visibleRect.GetLeft()) / stepTicks) * stepTicks;
        tickEnd = XToTick(visibleRect.GetRight());
        gridTop = std::max(kPianoRollTopGutter, visibleRect.GetTop());
        gridBottom = visibleRect.GetBottom();
        for (gridTick = tickStart; gridTick <= tickEnd; gridTick += stepTicks) {
            int x;
            bool major;

            x = TickToX(gridTick);
            major = ((gridTick % quarterTicks) == 0);
            dc.SetPen(wxPen(major ? wxColour(180, 186, 190) : wxColour(222, 226, 228)));
            dc.DrawLine(x, gridTop, x, gridBottom);
            if (tickEnd - gridTick < stepTicks) {
                break;
            }
        }

        dc.SetPen(wxPen(wxColour(60, 60, 60)));
        dc.DrawLine(kPianoRollLeftGutter, visibleRect.GetTop(), kPianoRollLeftGutter, visibleRect.GetBottom());

        if (HasTrack()) {
            uint32_t noteCount;
            uint32_t noteIndex;

            noteCount = 0;
            BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount);
            for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                BAERmfEditorNoteInfo noteInfo;
                wxRect noteRect;
                bool selected;

                if (!GetNoteInfo(noteIndex, &noteInfo)) {
                    continue;
                }
                noteRect = wxRect(TickToX(noteInfo.startTick),
                                  NoteToY(noteInfo.note),
                                  std::max(8, TickToX(noteInfo.startTick + noteInfo.durationTicks) - TickToX(noteInfo.startTick)),
                                  kNoteHeight - 1);
                if (!noteRect.Intersects(notesVisibleRect)) {
                    continue;
                }
                selected = IsNoteSelected(static_cast<long>(noteIndex));
                dc.SetBrush(wxBrush(selected ? wxColour(216, 106, 58) : wxColour(73, 135, 210)));
                dc.SetPen(wxPen(selected ? wxColour(110, 42, 17) : wxColour(34, 72, 120)));
                dc.DrawRectangle(noteRect);
                if (selected && static_cast<long>(noteIndex) == m_selectedNote) {
                    int handleTop;

                    handleTop = noteRect.GetTop() + 1;
                    dc.SetBrush(wxBrush(wxColour(242, 225, 214)));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.DrawRectangle(noteRect.GetLeft(), handleTop, 3, std::max(2, noteRect.GetHeight() - 2));
                    dc.DrawRectangle(noteRect.GetRight() - 2, handleTop, 3, std::max(2, noteRect.GetHeight() - 2));
                }
            }
        }
        DrawAutomationLanes(dc, clientSize, visibleRect);
        if (m_showPlayhead) {
            int playheadX;

            playheadX = TickToX(m_playheadTick);
            if (playheadX >= visibleRect.GetLeft() - 2 && playheadX <= visibleRect.GetRight() + 2) {
                dc.SetPen(wxPen(wxColour(210, 40, 40), 2));
                dc.DrawLine(playheadX, kPianoRollTopGutter, playheadX, clientSize.GetHeight());
            }
        }
        if (m_selectBoxActive) {
            wxRect selectionRect;

            selectionRect = NormalizeRect(m_selectBoxStart, m_selectBoxCurrent);
            dc.SetBrush(wxBrush(wxColour(73, 135, 210, 48)));
            dc.SetPen(wxPen(wxColour(45, 98, 168), 1));
            dc.DrawRectangle(selectionRect);
        }
        DrawStickyRuler(dc);
    }

    void OnLeftDown(wxMouseEvent &event) {
        wxPoint logicalPoint;
        AutomationHitInfo automationHit;
        BAERmfEditorNoteInfo noteInfo;
        long hitNote;

        SetFocus();
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        if (IsPointInStickyRuler(logicalPoint) && logicalPoint.x >= kPianoRollLeftGutter) {
            uint32_t tick;

            tick = XToTick(logicalPoint.x);
            if (m_seekRequestedCallback) {
                m_seekRequestedCallback(tick);
            } else {
                SetPlayheadTick(tick);
            }
            return;
        }
        hitNote = HitTestNote(logicalPoint, &noteInfo);
        if (hitNote >= 0) {
            bool clickedAlreadySelected;
            bool shouldPreviewSingle;

            clickedAlreadySelected = IsNoteSelected(hitNote);
            m_dragMode = GetDragModeForPoint(BuildNoteRect(noteInfo), logicalPoint);
            if (!clickedAlreadySelected || m_dragMode != DragMode::Move) {
                SelectSingleNote(hitNote);
            }
            UpdatePrimarySelectedNote();
            m_selectedItemKind = PianoRollSelectionKind::Note;
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
            m_dragging = true;
            m_selectBoxActive = false;
            m_dragAutomationValid = false;
            m_dragStartTick = SnapTick(XToTick(logicalPoint.x));
            m_dragStartNote = YToNote(logicalPoint.y);
            m_dragOriginalNoteIndices.clear();
            m_dragOriginalNotes.clear();
            if (m_dragMode == DragMode::Move && !m_selectedNotes.empty()) {
                for (long selectedIndex : m_selectedNotes) {
                    BAERmfEditorNoteInfo selectedNoteInfo;

                    if (selectedIndex < 0) {
                        continue;
                    }
                    if (GetNoteInfo(static_cast<uint32_t>(selectedIndex), &selectedNoteInfo)) {
                        m_dragOriginalNoteIndices.push_back(selectedIndex);
                        m_dragOriginalNotes.push_back(selectedNoteInfo);
                    }
                }
            }
            if (m_dragOriginalNotes.empty()) {
                m_dragOriginalNote = noteInfo;
                m_dragOriginalNoteIndices.push_back(hitNote);
                m_dragOriginalNotes.push_back(noteInfo);
            } else {
                m_dragOriginalNote = m_dragOriginalNotes.front();
            }
            shouldPreviewSingle = (m_selectedNotes.size() == 1 && m_dragOriginalNotes.size() == 1);
            if (shouldPreviewSingle) {
                RequestSingleNotePreview(m_dragOriginalNotes.front());
                m_lastPreviewDragNote = static_cast<int>(m_dragOriginalNotes.front().note);
            } else {
                m_lastPreviewDragNote = -1;
            }
            m_dragUndoLabel = (m_dragMode == DragMode::Move) ? "Move Note" : "Resize Note";
            BeginUndoAction(m_dragUndoLabel);
            m_dragUndoActive = true;
            CaptureMouse();
        } else if (HitTestAutomation(logicalPoint, &automationHit)) {
            m_selectedNotes.clear();
            m_selectedItemKind = PianoRollSelectionKind::Automation;
            m_selectedNote = -1;
            m_selectedAutomationLane = automationHit.laneIndex;
            m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
            m_dragging = true;
            m_selectBoxActive = false;
            m_dragAutomationValid = true;
            m_dragAutomationHit = automationHit;
            m_dragMode = GetDragModeForAutomation(automationHit, logicalPoint);
            m_dragUndoLabel = (m_dragMode == DragMode::ResizeRight) ? "Resize Event" : "Edit Event";
            BeginUndoAction(m_dragUndoLabel);
            m_dragUndoActive = true;
            m_lastPreviewDragNote = -1;
            CaptureMouse();
        } else {
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
            m_dragging = true;
            m_dragAutomationValid = false;
            m_dragMode = DragMode::SelectBox;
            m_dragUndoActive = false;
            m_dragUndoLabel.clear();
            m_selectBoxActive = true;
            m_selectBoxStart = logicalPoint;
            m_selectBoxCurrent = logicalPoint;
            m_selectedNotes.clear();
            m_selectedNote = -1;
            m_selectedItemKind = PianoRollSelectionKind::None;
            m_lastPreviewDragNote = -1;
            if (HasCapture()) {
                ReleaseMouse();
            }
            CaptureMouse();
        }
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
        Refresh();
    }

    void OnLeftUp(wxMouseEvent &) {
        RequestStopNotePreview();
        if (m_dragging && HasCapture()) {
            ReleaseMouse();
        }
        if (m_dragMode == DragMode::SelectBox && m_selectBoxActive) {
            wxRect selectionRect;

            selectionRect = NormalizeRect(m_selectBoxStart, m_selectBoxCurrent);
            UpdateAreaSelection(selectionRect);
            m_selectBoxActive = false;
            if (m_selectionChangedCallback) {
                m_selectionChangedCallback();
            }
            Refresh();
        } else if (m_dragUndoActive) {
            CommitUndoAction(m_dragUndoLabel);
        }
        m_dragging = false;
        m_dragAutomationValid = false;
        m_dragMode = DragMode::None;
        m_dragUndoActive = false;
        m_dragUndoLabel.clear();
        m_lastPreviewDragNote = -1;
    }

    void OnLeftDoubleClick(wxMouseEvent &event) {
        wxPoint logicalPoint;
        int laneIndex;
        uint32_t startTick;
        int eventValue;
        unsigned char noteValue;

        if (!m_document) {
            return;
        }
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        if (HitTestNote(logicalPoint) >= 0) {
            return;
        }
        laneIndex = GetAutomationLaneIndexFromY(logicalPoint.y);
        if (laneIndex >= 0) {
            std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
            startTick = SnapTick(XToTick(logicalPoint.x));
            eventValue = AutomationValueFromY(laneIndex, logicalPoint.y);
            m_dragUndoLabel = wxString::Format("Add %s Event", lanes[laneIndex].label);
            BeginUndoAction(m_dragUndoLabel);
            if (AddAutomationEvent(laneIndex, startTick, eventValue)) {
                AutomationHitInfo automationHit;

                if (HitTestAutomation(logicalPoint, &automationHit)) {
                    m_selectedItemKind = PianoRollSelectionKind::Automation;
                    m_selectedAutomationLane = automationHit.laneIndex;
                    m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
                    m_selectedNote = -1;
                }
                CommitUndoAction(m_dragUndoLabel);
                UpdateVirtualSize();
                Refresh();
            } else {
                CancelUndoAction();
            }
            return;
        }
        if (!HasTrack()) {
            return;
        }
        startTick = SnapTick(XToTick(logicalPoint.x));
        noteValue = static_cast<unsigned char>(YToNote(logicalPoint.y));
        BeginUndoAction("Add Note");
        if (BAERmfEditorDocument_AddNote(m_document,
                                         static_cast<uint16_t>(m_selectedTrack),
                                         startTick,
                                         kDefaultNoteDuration,
                                         noteValue,
                                         100) == BAE_NO_ERROR) {
            uint32_t noteCount;

            noteCount = 0;
            BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount);
            if (noteCount > 0) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     noteCount - 1,
                                                     &noteInfo) == BAE_NO_ERROR) {
                    noteInfo.bank = m_newNoteBank;
                    noteInfo.program = m_newNoteProgram;
                    BAERmfEditorDocument_SetNoteInfo(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     noteCount - 1,
                                                     &noteInfo);
                    m_selectedItemKind = PianoRollSelectionKind::Note;
                    m_selectedNote = static_cast<long>(noteCount - 1);
                    m_selectedNotes.clear();
                    m_selectedNotes.push_back(m_selectedNote);
                    m_selectedAutomationLane = -1;
                    m_selectedAutomationEvent = -1;
                }
            }
            CommitUndoAction("Add Note");
            UpdateVirtualSize();
            Refresh();
        } else {
            CancelUndoAction();
        }
    }

    void OnRightDown(wxMouseEvent &event) {
        wxPoint logicalPoint;
        AutomationHitInfo automationHit;
        long hitNote;
        wxMenu menu;

        if (!m_document) {
            return;
        }
        if (event.LeftIsDown() || m_dragging || HasCapture()) {
            return;
        }
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        hitNote = HitTestNote(logicalPoint);
        if (hitNote >= 0) {
            if (!IsNoteSelected(hitNote)) {
                SelectSingleNote(hitNote);
            } else {
                UpdatePrimarySelectedNote();
            }
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
        } else if (HitTestAutomation(logicalPoint, &automationHit)) {
            m_selectedNotes.clear();
            m_selectedItemKind = PianoRollSelectionKind::Automation;
            m_selectedNote = -1;
            m_selectedAutomationLane = automationHit.laneIndex;
            m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
        } else {
            return;
        }
        menu.Append(ID_PianoRollEdit, "Edit");
        menu.Append(ID_PianoRollDelete, "Delete");
        PopupMenu(&menu, event.GetPosition());
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
        Refresh();
    }

    void OnMotion(wxMouseEvent &event) {
        wxPoint logicalPoint;
        int automationValue;
        uint32_t automationTick;
        uint32_t snappedTick;
        int snappedNote;
        BAERmfEditorNoteInfo updatedNote;

        if (!m_document) {
            return;
        }
        if (!m_dragging) {
            UpdateHoverCursor(CalcUnscrolledPosition(event.GetPosition()));
            return;
        }
        if (!event.Dragging() || !event.LeftIsDown()) {
            return;
        }
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        if (m_dragMode == DragMode::SelectBox && m_selectBoxActive) {
            wxRect selectionRect;

            m_selectBoxCurrent = logicalPoint;
            selectionRect = NormalizeRect(m_selectBoxStart, m_selectBoxCurrent);
            UpdateAreaSelection(selectionRect);
            Refresh();
            return;
        }
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            if (m_selectedNote < 0 || !HasTrack() || m_dragOriginalNoteIndices.empty() || m_dragOriginalNotes.empty()) {
                return;
            }
            if (m_dragMode == DragMode::Move) {
                snappedTick = SnapTick(XToTick(logicalPoint.x));
                snappedNote = YToNote(logicalPoint.y);
                for (size_t i = 0; i < m_dragOriginalNotes.size(); ++i) {
                    BAERmfEditorNoteInfo movedNote;
                    long noteIndex;

                    movedNote = m_dragOriginalNotes[i];
                    noteIndex = m_dragOriginalNoteIndices[i];
                    if (noteIndex < 0) {
                        continue;
                    }
                    if (snappedTick >= m_dragStartTick) {
                        movedNote.startTick = m_dragOriginalNotes[i].startTick + (snappedTick - m_dragStartTick);
                    } else {
                        uint32_t deltaTicks;

                        deltaTicks = m_dragStartTick - snappedTick;
                        movedNote.startTick = (deltaTicks > m_dragOriginalNotes[i].startTick) ? 0 : (m_dragOriginalNotes[i].startTick - deltaTicks);
                    }
                    movedNote.note = static_cast<unsigned char>(std::clamp(static_cast<int>(m_dragOriginalNotes[i].note) + (snappedNote - m_dragStartNote), 0, 127));
                    BAERmfEditorDocument_SetNoteInfo(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     static_cast<uint32_t>(noteIndex),
                                                     &movedNote);
                }
                if (m_selectedNotes.size() == 1 && !m_dragOriginalNotes.empty()) {
                    BAERmfEditorNoteInfo previewNote;

                    previewNote = m_dragOriginalNotes.front();
                    if (snappedTick >= m_dragStartTick) {
                        previewNote.startTick = m_dragOriginalNotes.front().startTick + (snappedTick - m_dragStartTick);
                    } else {
                        uint32_t deltaTicks;

                        deltaTicks = m_dragStartTick - snappedTick;
                        previewNote.startTick = (deltaTicks > m_dragOriginalNotes.front().startTick)
                                                    ? 0
                                                    : (m_dragOriginalNotes.front().startTick - deltaTicks);
                    }
                    previewNote.note = static_cast<unsigned char>(std::clamp(static_cast<int>(m_dragOriginalNotes.front().note) + (snappedNote - m_dragStartNote), 0, 127));
                    if (static_cast<int>(previewNote.note) != m_lastPreviewDragNote) {
                        RequestSingleNotePreview(previewNote);
                        m_lastPreviewDragNote = static_cast<int>(previewNote.note);
                    }
                }
            } else if (m_dragMode == DragMode::ResizeLeft) {
                uint32_t originalEndTick;
                uint32_t snappedStartTick;
                uint32_t maxStartTick;
                uint32_t snapTicks;

                updatedNote = m_dragOriginalNote;
                snapTicks = GetSnapTicks();
                originalEndTick = m_dragOriginalNote.startTick + m_dragOriginalNote.durationTicks;
                snappedStartTick = SnapTick(XToTick(logicalPoint.x));
                maxStartTick = (originalEndTick > snapTicks) ? (originalEndTick - snapTicks) : 0;
                updatedNote.startTick = std::min(snappedStartTick, maxStartTick);
                updatedNote.durationTicks = std::max<uint32_t>(snapTicks, originalEndTick - updatedNote.startTick);
                BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(m_selectedNote),
                                                 &updatedNote);
            } else if (m_dragMode == DragMode::ResizeRight) {
                uint32_t snappedEndTick;
                uint32_t snapTicks;

                updatedNote = m_dragOriginalNote;
                snapTicks = GetSnapTicks();
                snappedEndTick = SnapTick(XToTick(logicalPoint.x));
                if (snappedEndTick <= m_dragOriginalNote.startTick + snapTicks) {
                    snappedEndTick = m_dragOriginalNote.startTick + snapTicks;
                }
                updatedNote.durationTicks = snappedEndTick - m_dragOriginalNote.startTick;
                BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(m_selectedNote),
                                                 &updatedNote);
            }
            UpdateVirtualSize();
            Refresh();
            return;
        }
        if (m_selectedItemKind != PianoRollSelectionKind::Automation || !m_dragAutomationValid || m_selectedAutomationLane < 0 || m_selectedAutomationEvent < 0) {
            return;
        }
        automationTick = SnapTick(XToTick(logicalPoint.x));
        automationValue = AutomationValueFromY(m_selectedAutomationLane, logicalPoint.y);
        if (m_dragMode == DragMode::ResizeRight && m_dragAutomationHit.hasFollowingEvent) {
            uint32_t currentTick;
            uint32_t nextTickCurrent;
            int currentValue;
            int nextValueCurrent;
            uint32_t nextNextTick;
            uint32_t eventCount;
            uint32_t snapTicks;

            if (!GetAutomationEvent(m_selectedAutomationLane, static_cast<uint32_t>(m_selectedAutomationEvent), &currentTick, &currentValue)) {
                return;
            }
            if (!GetAutomationEvent(m_selectedAutomationLane,
                                    static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                    &nextTickCurrent,
                                    &nextValueCurrent)) {
                return;
            }
            nextNextTick = GetDocumentEndTick();
            eventCount = 0;
            GetAutomationEventCount(m_selectedAutomationLane, &eventCount);
            if (static_cast<uint32_t>(m_selectedAutomationEvent) + 2 < eventCount) {
                int nextNextValue;

                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 2,
                                   &nextNextTick,
                                   &nextNextValue);
            }
              snapTicks = GetSnapTicks();
            automationTick = std::clamp(automationTick,
                                    currentTick + snapTicks,
                                    nextNextTick > snapTicks ? (nextNextTick - snapTicks) : currentTick + snapTicks);
            if (SetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                   automationTick,
                                   nextValueCurrent)) {
                UpdateVirtualSize();
                Refresh();
            }
            return;
        }
        {
            uint32_t previousTick;
            uint32_t nextTick;
            uint32_t eventCount;
            int previousValue;
            int nextValue;
            uint32_t snapTicks;

            previousTick = 0;
            nextTick = GetDocumentEndTick();
            eventCount = 0;
            snapTicks = GetSnapTicks();
            GetAutomationEventCount(m_selectedAutomationLane, &eventCount);
            if (m_selectedAutomationEvent > 0) {
                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) - 1,
                                   &previousTick,
                                   &previousValue);
                previousTick += snapTicks;
            }
            if (static_cast<uint32_t>(m_selectedAutomationEvent) + 1 < eventCount) {
                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                   &nextTick,
                                   &nextValue);
                if (nextTick > snapTicks) {
                    nextTick -= snapTicks;
                }
            }
            automationTick = std::clamp(automationTick, previousTick, nextTick);
            if (SetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent),
                                   automationTick,
                                   automationValue)) {
                UpdateVirtualSize();
                Refresh();
            }
        }
    }

    void OnCharHook(wxKeyEvent &event) {
        if (event.GetKeyCode() == WXK_DELETE || event.GetKeyCode() == WXK_BACK) {
            DeleteCurrentSelection();
            return;
        }
        event.Skip();
    }

    void OnMouseWheel(wxMouseEvent &event) {
        wxPoint logicalPoint;
        bool wheelOnRuler;

        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        wheelOnRuler = IsPointInStickyRuler(logicalPoint) && logicalPoint.x >= kPianoRollLeftGutter;
        if ((wheelOnRuler || event.AltDown()) && !event.ControlDown()) {
            if (ScrollHorizontallyWithWheel(event, 2)) {
                return;
            }
        }
        if (wheelOnRuler && !event.ControlDown()) {
            if (ScrollHorizontallyWithWheel(event, 1)) {
                return;
            }
        }
        if (event.ControlDown()) {
            ApplyZoomStep(event.GetWheelRotation(), event.GetPosition());
            return;
        }
        event.Skip();
    }

    void OnMouseLeave(wxMouseEvent &) {
        if (!m_dragging) {
            SetCursor(wxNullCursor);
        }
    }
};


PianoRollPanel *CreatePianoRollPanel(wxWindow *parent) {
    return new PianoRollPanel(parent);
}

wxWindow *PianoRollPanel_AsWindow(PianoRollPanel *panel) {
    return panel;
}

void PianoRollPanel_SetDocument(PianoRollPanel *panel, BAERmfEditorDocument *document) {
    if (panel) {
        panel->SetDocument(document);
    }
}

void PianoRollPanel_SetSelectedTrack(PianoRollPanel *panel, int trackIndex) {
    if (panel) {
        panel->SetSelectedTrack(trackIndex);
    }
}

void PianoRollPanel_SetSelectionChangedCallback(PianoRollPanel *panel, std::function<void()> callback) {
    if (panel) {
        panel->SetSelectionChangedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetSeekRequestedCallback(PianoRollPanel *panel, std::function<void(uint32_t)> callback) {
    if (panel) {
        panel->SetSeekRequestedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetNotePreviewRequestedCallback(PianoRollPanel *panel,
                                                    std::function<void(uint16_t,
                                                                       unsigned char,
                                                                       unsigned char,
                                                                       uint32_t,
                                                                       int)> callback) {
    if (panel) {
        panel->SetNotePreviewRequestedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetNotePreviewStopRequestedCallback(PianoRollPanel *panel,
                                                        std::function<void()> callback) {
    if (panel) {
        panel->SetNotePreviewStopRequestedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetUndoCallbacks(PianoRollPanel *panel,
                                     std::function<void(wxString const &)> beginCallback,
                                     std::function<void(wxString const &)> commitCallback,
                                     std::function<void()> cancelCallback) {
    if (panel) {
        panel->SetUndoCallbacks(std::move(beginCallback), std::move(commitCallback), std::move(cancelCallback));
    }
}

void PianoRollPanel_RefreshFromDocument(PianoRollPanel *panel, bool clearSelection) {
    if (panel) {
        panel->RefreshFromDocument(clearSelection);
    }
}

bool PianoRollPanel_GetSelectedNoteInfo(PianoRollPanel *panel, BAERmfEditorNoteInfo *outNoteInfo) {
    if (!panel) {
        return false;
    }
    return panel->GetSelectedNoteInfo(outNoteInfo);
}

void PianoRollPanel_SetSelectedNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program) {
    if (panel) {
        panel->SetSelectedNoteInstrument(bank, program);
    }
}

void PianoRollPanel_SetNewNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program) {
    if (panel) {
        panel->SetNewNoteInstrument(bank, program);
    }
}

void PianoRollPanel_SetPlayheadTick(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->SetPlayheadTick(tick);
    }
}

void PianoRollPanel_EnsurePlayheadVisible(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->EnsurePlayheadVisible(tick);
    }
}

void PianoRollPanel_JumpToTick(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->JumpToTick(tick);
    }
}

uint32_t PianoRollPanel_GetDocumentEndTick(PianoRollPanel *panel) {
    if (!panel) {
        return 0;
    }
    return panel->GetVisibleDocumentEndTick();
}

void PianoRollPanel_ClearPlayhead(PianoRollPanel *panel) {
    if (panel) {
        panel->ClearPlayhead();
    }
}

void PianoRollPanel_Refresh(PianoRollPanel *panel) {
    if (panel) {
        panel->Refresh();
    }
}
