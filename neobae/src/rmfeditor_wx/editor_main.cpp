#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/button.h>
#include <wx/file.h>
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

namespace {

constexpr int kPianoRollLeftGutter = 64;
constexpr int kPianoRollTopGutter = 30;
constexpr int kNoteHeight = 12;
constexpr int kBasePixelsPerQuarter = 96;
constexpr int kResizeHandlePixels = 6;
constexpr int kAutomationLaneHeight = 44;
constexpr int kAutomationLaneGap = 6;
constexpr int kAutomationLaneCount = 4;
constexpr uint32_t kDefaultNoteDuration = 480;
constexpr uint32_t kSnapTicks = 120;
constexpr char const *kVersionString = "0.01 alpha";

enum class DragMode {
    None,
    Move,
    ResizeLeft,
    ResizeRight,
};

enum {
    ID_TrackAdd = wxID_HIGHEST + 200,
    ID_TrackDelete,
    ID_SampleAdd,
    ID_SampleNewInstrument,
    ID_SampleRename,
    ID_SampleDelete,
    ID_LoadBank,
    ID_UnloadBanks,
    ID_PianoRollEdit,
    ID_PianoRollDelete,
};

enum class PianoRollSelectionKind {
    None,
    Note,
    Automation,
};

struct AutomationLaneDescriptor {
    char const *label;
    unsigned char controller;
    wxColour color;
    int maxValue;
};

struct UndoTempoEventState {
    uint32_t tick;
    uint32_t microsecondsPerQuarter;
};

struct UndoCCEventState {
    uint32_t tick;
    unsigned char value;
};

struct UndoTrackState {
    std::vector<BAERmfEditorNoteInfo> notes;
    std::vector<UndoCCEventState> ccEvents[kAutomationLaneCount - 1];
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

static AutomationLaneDescriptor const kAutomationLanes[kAutomationLaneCount] = {
    { "Tempo", 0,  wxColour(202, 128, 52), 240 },
    { "Volume", 7, wxColour(74, 136, 208), 127 },
    { "Pan", 10, wxColour(166, 96, 196), 127 },
    { "Expr", 11, wxColour(76, 170, 92), 127 },
};

static unsigned char GetUndoCCController(int ccSlot) {
    return kAutomationLanes[ccSlot + 1].controller;
}

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
        for (int ccSlot = 0; ccSlot < kAutomationLaneCount - 1; ++ccSlot) {
            if (leftTrack.ccEvents[ccSlot].size() != rightTrack.ccEvents[ccSlot].size()) {
                return false;
            }
            for (size_t eventIndex = 0; eventIndex < leftTrack.ccEvents[ccSlot].size(); ++eventIndex) {
                if (leftTrack.ccEvents[ccSlot][eventIndex].tick != rightTrack.ccEvents[ccSlot][eventIndex].tick ||
                    leftTrack.ccEvents[ccSlot][eventIndex].value != rightTrack.ccEvents[ccSlot][eventIndex].value) {
                    return false;
                }
            }
        }
    }
    return true;
}

class NoteEditDialog final : public wxDialog {
public:
    explicit NoteEditDialog(wxWindow *parent, BAERmfEditorNoteInfo const &noteInfo)
        : wxDialog(parent, wxID_ANY, "Edit Note", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *gridSizer = new wxFlexGridSizer(2, 6, 8, 8);

        m_startTickText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(noteInfo.startTick)));
        m_durationText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(noteInfo.durationTicks)));
        m_noteSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.note);
        m_velocitySpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.velocity);
        m_bankSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 16383, noteInfo.bank);
        m_programSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.program);

        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Start Tick"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_startTickText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Duration"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_durationText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Note"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_noteSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Velocity"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_velocitySpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Bank"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_bankSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_programSpin, 1, wxEXPAND);
        gridSizer->AddGrowableCol(1, 1);

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
        outNoteInfo->bank = static_cast<uint16_t>(m_bankSpin->GetValue());
        outNoteInfo->program = static_cast<unsigned char>(m_programSpin->GetValue());
        return true;
    }

private:
    wxTextCtrl *m_startTickText;
    wxTextCtrl *m_durationText;
    wxSpinCtrl *m_noteSpin;
    wxSpinCtrl *m_velocitySpin;
    wxSpinCtrl *m_bankSpin;
    wxSpinCtrl *m_programSpin;
};

class AutomationEditDialog final : public wxDialog {
public:
    AutomationEditDialog(wxWindow *parent, wxString const &title, wxString const &valueLabel, uint32_t tick, int value, int maxValue)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *gridSizer = new wxFlexGridSizer(2, 4, 8, 8);

        m_tickText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(tick)));
        m_valueSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, maxValue, value);

        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Tick"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_tickText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, valueLabel), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_valueSpin, 1, wxEXPAND);
        gridSizer->AddGrowableCol(1, 1);

        rootSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 12);
        rootSizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(rootSizer);
    }

    bool GetValues(uint32_t *outTick, int *outValue) const {
        unsigned long long tick;

        if (!outTick || !outValue) {
            return false;
        }
        if (!m_tickText->GetValue().ToULongLong(&tick)) {
            return false;
        }
        *outTick = static_cast<uint32_t>(tick);
        *outValue = m_valueSpin->GetValue();
        return true;
    }

private:
    wxTextCtrl *m_tickText;
    wxSpinCtrl *m_valueSpin;
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
                    m_zoomScale(1.0f) {
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
        m_selectedItemKind = PianoRollSelectionKind::None;
        m_selectedAutomationLane = -1;
        m_selectedAutomationEvent = -1;
        m_dragAutomationValid = false;
        m_dragging = false;
        m_dragMode = DragMode::None;
        UpdateVirtualSize();
        ScrollToBottom();
        Refresh();
    }

    void SetSelectedTrack(int trackIndex) {
        m_selectedTrack = trackIndex;
        m_selectedNote = -1;
        m_selectedItemKind = PianoRollSelectionKind::None;
        m_selectedAutomationLane = -1;
        m_selectedAutomationEvent = -1;
        m_dragAutomationValid = false;
        m_dragging = false;
        m_dragMode = DragMode::None;
        UpdateVirtualSize();
        ScrollToBottom();
        Refresh();
    }

    void SetSelectionChangedCallback(std::function<void()> callback) {
        m_selectionChangedCallback = std::move(callback);
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
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
        }
        m_dragging = false;
        m_dragAutomationValid = false;
        m_dragMode = DragMode::None;
        m_dragUndoActive = false;
        m_dragUndoLabel.clear();
        UpdateVirtualSize();
        Refresh();
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
    }

    bool GetSelectedNoteInfo(BAERmfEditorNoteInfo *outNoteInfo) const {
        if (!outNoteInfo || !HasTrack() || m_selectedNote < 0) {
            return false;
        }
        return BAERmfEditorDocument_GetNoteInfo(m_document,
                                                static_cast<uint16_t>(m_selectedTrack),
                                                static_cast<uint32_t>(m_selectedNote),
                                                outNoteInfo) == BAE_NO_ERROR;
    }

    void SetSelectedNoteInstrument(uint16_t bank, unsigned char program) {
        BAERmfEditorNoteInfo noteInfo;

        if (!GetSelectedNoteInfo(&noteInfo)) {
            return;
        }
        noteInfo.bank = bank;
        noteInfo.program = program;
        BeginUndoAction("Change Note Instrument");
        if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_selectedTrack),
                                             static_cast<uint32_t>(m_selectedNote),
                                             &noteInfo) == BAE_NO_ERROR) {
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

private:
    BAERmfEditorDocument *m_document;
    int m_selectedTrack;
    long m_selectedNote;
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
    std::function<void(wxString const &)> m_beginUndoCallback;
    std::function<void(wxString const &)> m_commitUndoCallback;
    std::function<void()> m_cancelUndoCallback;
    bool m_dragUndoActive = false;
    wxString m_dragUndoLabel;
    float m_zoomScale;

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
        uint32_t bpm;
        int pixels;

        bpm = 120;
        if (m_document) {
            BAERmfEditorDocument_GetTempoBPM(m_document, &bpm);
        }
        if (bpm == 0) {
            bpm = 120;
        }
        pixels = static_cast<int>((static_cast<double>(kBasePixelsPerQuarter) * m_zoomScale * 120.0) / static_cast<double>(bpm));
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
        return (tick / kSnapTicks) * kSnapTicks;
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
        return m_document && (laneIndex == 0 || HasTrack());
    }

    int AutomationValueFromY(int laneIndex, int y) const {
        int laneTop;
        int laneBottom;
        int value;

        laneTop = GetAutomationLaneY(laneIndex);
        laneBottom = laneTop + kAutomationLaneHeight;
        y = std::clamp(y, laneTop, laneBottom - 1);
        if (kAutomationLanes[laneIndex].controller == 10) {
            value = 127 - ((y - laneTop) * 127) / std::max(1, kAutomationLaneHeight - 1);
        } else {
            value = ((laneBottom - 1 - y) * kAutomationLanes[laneIndex].maxValue) / std::max(1, kAutomationLaneHeight - 1);
        }
        return std::clamp(value, 0, kAutomationLanes[laneIndex].maxValue);
    }

    bool HitTestAutomation(wxPoint point, AutomationHitInfo *outHitInfo = nullptr) const {
        int laneIndex;

        if (!m_document || point.y < GetAutomationAreaTop()) {
            return false;
        }
        for (laneIndex = 0; laneIndex < kAutomationLaneCount; ++laneIndex) {
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
            if (laneIndex == 0) {
                eventCount = 0;
                if (BAERmfEditorDocument_GetTempoEventCount(m_document, &eventCount) != BAE_NO_ERROR || eventCount == 0) {
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
            } else {
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                              static_cast<uint16_t>(m_selectedTrack),
                                                              kAutomationLanes[laneIndex].controller,
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
                                                             kAutomationLanes[laneIndex].controller,
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
                                                             kAutomationLanes[laneIndex].controller,
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
            }
        }
        return false;
    }

    int GetAutomationLaneIndexFromY(int y) const {
        int laneIndex;

        for (laneIndex = 0; laneIndex < kAutomationLaneCount; ++laneIndex) {
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
        if (!outCount || !m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (laneIndex == 0) {
            return BAERmfEditorDocument_GetTempoEventCount(m_document, outCount) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                         static_cast<uint16_t>(m_selectedTrack),
                                                         kAutomationLanes[laneIndex].controller,
                                                         outCount) == BAE_NO_ERROR;
    }

    bool GetAutomationEvent(int laneIndex, uint32_t eventIndex, uint32_t *outTick, int *outValue) const {
        if (!outTick || !outValue || !m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (laneIndex == 0) {
            uint32_t microsecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, outTick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                return false;
            }
            *outValue = static_cast<int>(60000000UL / microsecondsPerQuarter);
            return true;
        }
        {
            unsigned char value;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     kAutomationLanes[laneIndex].controller,
                                                     eventIndex,
                                                     outTick,
                                                     &value) != BAE_NO_ERROR) {
                return false;
            }
            *outValue = value;
            return true;
        }
    }

    bool AddAutomationEvent(int laneIndex, uint32_t tick, int value) {
        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (laneIndex == 0) {
            return BAERmfEditorDocument_AddTempoEvent(m_document,
                                                      tick,
                                                      static_cast<uint32_t>(60000000UL / std::max(1, value))) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                    static_cast<uint16_t>(m_selectedTrack),
                                                    kAutomationLanes[laneIndex].controller,
                                                    tick,
                                                    static_cast<unsigned char>(value)) == BAE_NO_ERROR;
    }

    bool SetAutomationEvent(int laneIndex, uint32_t eventIndex, uint32_t tick, int value) {
        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (laneIndex == 0) {
            return BAERmfEditorDocument_SetTempoEvent(m_document,
                                                      eventIndex,
                                                      tick,
                                                      static_cast<uint32_t>(60000000UL / std::max(1, value))) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                    static_cast<uint16_t>(m_selectedTrack),
                                                    kAutomationLanes[laneIndex].controller,
                                                    eventIndex,
                                                    tick,
                                                    static_cast<unsigned char>(value)) == BAE_NO_ERROR;
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

        if (!GetSelectedNoteInfo(&noteInfo)) {
            return false;
        }
        NoteEditDialog dialog(this, noteInfo);
        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }
        if (!dialog.GetNoteInfo(&noteInfo)) {
            wxMessageBox("Invalid note values.", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
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
        CommitUndoAction("Edit Note");
        UpdateVirtualSize();
        Refresh();
        return true;
    }

    bool EditAutomationEvent(int laneIndex, uint32_t eventIndex) {
        uint32_t tick;
        int value;
        wxString valueLabel;
        wxString title;

        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (laneIndex == 0) {
            uint32_t microsecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, &tick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                return false;
            }
            value = static_cast<int>(60000000UL / microsecondsPerQuarter);
            valueLabel = "BPM";
            title = "Edit Tempo Event";
        } else {
            unsigned char controllerValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     kAutomationLanes[laneIndex].controller,
                                                     eventIndex,
                                                     &tick,
                                                     &controllerValue) != BAE_NO_ERROR) {
                return false;
            }
            value = controllerValue;
            valueLabel = "Value";
            title = wxString::Format("Edit %s Event", kAutomationLanes[laneIndex].label);
        }
        AutomationEditDialog dialog(this, title, valueLabel, tick, value, kAutomationLanes[laneIndex].maxValue);
        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }
        if (!dialog.GetValues(&tick, &value)) {
            wxMessageBox("Invalid automation values.", title, wxOK | wxICON_ERROR, this);
            return false;
        }
        BeginUndoAction(title);
        if (laneIndex == 0) {
            if (BAERmfEditorDocument_SetTempoEvent(m_document,
                                                   eventIndex,
                                                   tick,
                                                   static_cast<uint32_t>(60000000UL / std::max(1, value))) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update tempo event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        } else {
            if (BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     kAutomationLanes[laneIndex].controller,
                                                     eventIndex,
                                                     tick,
                                                     static_cast<unsigned char>(value)) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update automation event.", title, wxOK | wxICON_ERROR, this);
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
            if (m_selectedAutomationLane == 0) {
                result = BAERmfEditorDocument_DeleteTempoEvent(m_document, static_cast<uint32_t>(m_selectedAutomationEvent));
            } else {
                result = BAERmfEditorDocument_DeleteTrackCCEvent(m_document,
                                                                 static_cast<uint16_t>(m_selectedTrack),
                                                                 kAutomationLanes[m_selectedAutomationLane].controller,
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
            for (unsigned char cc : { static_cast<unsigned char>(7), static_cast<unsigned char>(10), static_cast<unsigned char>(11) }) {
                uint32_t eventCount;
                uint32_t eventIndex;

                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, static_cast<uint16_t>(m_selectedTrack), cc, &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;
                    unsigned char value;

                    if (BAERmfEditorDocument_GetTrackCCEvent(m_document, static_cast<uint16_t>(m_selectedTrack), cc, eventIndex, &tick, &value) == BAE_NO_ERROR) {
                        lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
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

    void DrawAutomationLanes(wxDC &dc, wxSize const &virtualSize) {
        static char const *const laneLabels[kAutomationLaneCount] = { "Tempo", "Volume", "Pan", "Expr" };
        static unsigned char const laneControllers[kAutomationLaneCount] = { 0, 7, 10, 11 };
        static wxColour const laneColors[kAutomationLaneCount] = {
            wxColour(202, 128, 52),
            wxColour(74, 136, 208),
            wxColour(166, 96, 196),
            wxColour(76, 170, 92)
        };
        int laneIndex;

        for (laneIndex = 0; laneIndex < kAutomationLaneCount; ++laneIndex) {
            int laneTop;
            int laneBottom;

            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(32, 34, 38)));
            dc.DrawRectangle(kPianoRollLeftGutter, laneTop, virtualSize.GetWidth() - kPianoRollLeftGutter, kAutomationLaneHeight);
            dc.SetBrush(wxBrush(wxColour(42, 44, 50)));
            dc.DrawRectangle(0, laneTop, kPianoRollLeftGutter, kAutomationLaneHeight);
            dc.SetPen(wxPen(wxColour(68, 72, 78)));
            dc.DrawLine(0, laneBottom, virtualSize.GetWidth(), laneBottom);
            dc.SetTextForeground(wxColour(214, 217, 222));
            dc.DrawText(laneLabels[laneIndex], 8, laneTop + (kAutomationLaneHeight / 2) - 8);
            if (laneControllers[laneIndex] == 10) {
                dc.SetPen(wxPen(wxColour(84, 88, 96)));
                dc.DrawLine(kPianoRollLeftGutter, laneTop + (kAutomationLaneHeight / 2), virtualSize.GetWidth(), laneTop + (kAutomationLaneHeight / 2));
            }
            if (!m_document) {
                continue;
            }
            if (laneIndex == 0) {
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
                    dc.SetBrush(wxBrush(laneColors[laneIndex]));
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
                        barHeight = std::clamp((static_cast<int>(bpm) * kAutomationLaneHeight) / 240, 2, kAutomationLaneHeight);
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.SetBrush(wxBrush(laneColors[laneIndex]));
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
            } else if (HasTrack()) {
                uint32_t eventCount;
                uint32_t eventIndex;
                unsigned char controller;

                controller = laneControllers[laneIndex];
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
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(laneColors[laneIndex]));
                    if (controller == 10) {
                        int centerY;
                        int offsetY;

                        centerY = laneTop + (kAutomationLaneHeight / 2);
                        offsetY = ((static_cast<int>(value) - 64) * (kAutomationLaneHeight / 2 - 2)) / 64;
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
            }
        }
    }

    void UpdateVirtualSize() {
        int width;
        int height;
        uint32_t lastTick;

        width = 2400;
        height = GetAutomationAreaTop() + (kAutomationLaneCount * kAutomationLaneHeight) + ((kAutomationLaneCount - 1) * kAutomationLaneGap) + kPianoRollTopGutter;
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
        int startOff;
        int beatOff;
        int pixelsPerQuarter;
        int pixelsPerStep;

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
        pixelsPerQuarter = GetPixelsPerQuarter();
        pixelsPerStep = std::max(1, pixelsPerQuarter / 4);
        startOff = (relLeft / pixelsPerQuarter) * pixelsPerQuarter;

        // Sub-beat (16th-note) small ticks
        for (beatOff = (startOff >= pixelsPerQuarter ? startOff - pixelsPerQuarter : 0);
             beatOff <= relRight;
             beatOff += pixelsPerStep) {
            int vx;

            if (beatOff < 0) continue;
            if ((beatOff % pixelsPerQuarter) == 0) continue;
            vx = kPianoRollLeftGutter + beatOff;
            dc.SetPen(wxPen(wxColour(70, 75, 82)));
            dc.DrawLine(vx, rulerBot - 5, vx, rulerBot - 1);
        }

        // Quarter-note beat labels and taller ticks
           for (beatOff = (startOff >= pixelsPerQuarter ? startOff - pixelsPerQuarter : 0);
             beatOff <= relRight;
               beatOff += pixelsPerQuarter) {
            int vx;
            int beatNum;
            uint32_t tick;
            double seconds;
            int mm;
            int ss;
            int ds;
            bool isBar;

            if (beatOff < 0) continue;
            vx      = kPianoRollLeftGutter + beatOff;
            beatNum = beatOff / pixelsPerQuarter + 1;
            isBar   = ((beatOff % (pixelsPerQuarter * 4)) == 0);

            tick = static_cast<uint32_t>((static_cast<uint64_t>(beatOff) * GetTicksPerQuarter()) / pixelsPerQuarter);
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
        }

        // Bottom border
        dc.SetPen(wxPen(wxColour(70, 75, 82)));
        dc.DrawLine(scrollX, rulerBot - 1, scrollX + screenW, rulerBot - 1);

        // Left gutter right-edge (re-draw on top of ruler)
        dc.SetPen(wxPen(wxColour(100, 105, 110)));
        dc.DrawLine(scrollX + kPianoRollLeftGutter, rulerTop, scrollX + kPianoRollLeftGutter, rulerBot);
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
        if (!HasTrack() || m_selectedNote < 0) {
            return;
        }
        BeginUndoAction("Delete Note");
        if (BAERmfEditorDocument_DeleteNote(m_document, static_cast<uint16_t>(m_selectedTrack), static_cast<uint32_t>(m_selectedNote)) == BAE_NO_ERROR) {
            CommitUndoAction("Delete Note");
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
        wxRect updateRect;
        int beat;
        int note;
        int pixelsPerQuarter;
        int pixelsPerStep;

        PrepareDC(dc);
        clientSize = GetVirtualSize();
        updateRect = GetUpdateRegion().GetBox();
        dc.SetBackground(wxBrush(wxColour(248, 248, 246)));
        dc.Clear();

        for (note = 0; note < 128; ++note) {
            int y;
            bool blackKey;
            wxColour fillColor;

            y = NoteToY(note);
            blackKey = (note % 12 == 1) || (note % 12 == 3) || (note % 12 == 6) || (note % 12 == 8) || (note % 12 == 10);
            fillColor = blackKey ? wxColour(235, 238, 240) : wxColour(248, 248, 246);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(fillColor));
            dc.DrawRectangle(0, y, clientSize.GetWidth(), kNoteHeight);
            dc.SetPen(wxPen(wxColour(220, 224, 226)));
            dc.DrawLine(kPianoRollLeftGutter, y, clientSize.GetWidth(), y);
            if (note % 12 == 0) {
                dc.SetTextForeground(wxColour(90, 90, 90));
                dc.DrawText(wxString::Format("C%d", note / 12), 6, y - 1);
            }
        }

        pixelsPerQuarter = GetPixelsPerQuarter();
        pixelsPerStep = std::max(1, pixelsPerQuarter / 4);
        for (beat = 0; beat < clientSize.GetWidth(); beat += pixelsPerStep) {
            int x;
            bool major;

            x = kPianoRollLeftGutter + beat;
            major = ((beat % pixelsPerQuarter) == 0);
            dc.SetPen(wxPen(major ? wxColour(180, 186, 190) : wxColour(222, 226, 228)));
            dc.DrawLine(x, kPianoRollTopGutter, x, clientSize.GetHeight());
        }

        dc.SetPen(wxPen(wxColour(60, 60, 60)));
        dc.DrawLine(kPianoRollLeftGutter, 0, kPianoRollLeftGutter, clientSize.GetHeight());

        if (m_showPlayhead) {
            int playheadX;

            playheadX = TickToX(m_playheadTick);
            dc.SetPen(wxPen(wxColour(210, 40, 40), 2));
            dc.DrawLine(playheadX, kPianoRollTopGutter, playheadX, clientSize.GetHeight());
        }

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
                selected = (static_cast<long>(noteIndex) == m_selectedNote);
                dc.SetBrush(wxBrush(selected ? wxColour(216, 106, 58) : wxColour(73, 135, 210)));
                dc.SetPen(wxPen(selected ? wxColour(110, 42, 17) : wxColour(34, 72, 120)));
                dc.DrawRectangle(noteRect);
                if (selected) {
                    int handleTop;

                    handleTop = noteRect.GetTop() + 1;
                    dc.SetBrush(wxBrush(wxColour(242, 225, 214)));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.DrawRectangle(noteRect.GetLeft(), handleTop, 3, std::max(2, noteRect.GetHeight() - 2));
                    dc.DrawRectangle(noteRect.GetRight() - 2, handleTop, 3, std::max(2, noteRect.GetHeight() - 2));
                }
            }
        }
        DrawAutomationLanes(dc, clientSize);
        DrawStickyRuler(dc);
    }

    void OnLeftDown(wxMouseEvent &event) {
        wxPoint logicalPoint;
        AutomationHitInfo automationHit;
        BAERmfEditorNoteInfo noteInfo;
        long hitNote;

        SetFocus();
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        hitNote = HitTestNote(logicalPoint, &noteInfo);
        if (hitNote >= 0) {
            m_selectedItemKind = PianoRollSelectionKind::Note;
            m_selectedNote = hitNote;
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
            m_dragging = true;
            m_dragAutomationValid = false;
            m_dragOriginalNote = noteInfo;
            m_dragStartTick = SnapTick(XToTick(logicalPoint.x));
            m_dragStartNote = YToNote(logicalPoint.y);
            m_dragMode = GetDragModeForPoint(BuildNoteRect(noteInfo), logicalPoint);
            m_dragUndoLabel = (m_dragMode == DragMode::Move) ? "Move Note" : "Resize Note";
            BeginUndoAction(m_dragUndoLabel);
            m_dragUndoActive = true;
            CaptureMouse();
        } else if (HitTestAutomation(logicalPoint, &automationHit)) {
            m_selectedItemKind = PianoRollSelectionKind::Automation;
            m_selectedNote = -1;
            m_selectedAutomationLane = automationHit.laneIndex;
            m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
            m_dragging = true;
            m_dragAutomationValid = true;
            m_dragAutomationHit = automationHit;
            m_dragMode = GetDragModeForAutomation(automationHit, logicalPoint);
            m_dragUndoLabel = (m_dragMode == DragMode::ResizeRight) ? "Resize Event" : "Edit Event";
            BeginUndoAction(m_dragUndoLabel);
            m_dragUndoActive = true;
            CaptureMouse();
        } else {
            m_selectedItemKind = PianoRollSelectionKind::None;
            m_selectedNote = -1;
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
            m_dragging = false;
            m_dragAutomationValid = false;
            m_dragMode = DragMode::None;
            m_dragUndoActive = false;
            m_dragUndoLabel.clear();
        }
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
        Refresh();
    }

    void OnLeftUp(wxMouseEvent &) {
        if (m_dragging && HasCapture()) {
            ReleaseMouse();
        }
        if (m_dragUndoActive) {
            CommitUndoAction(m_dragUndoLabel);
        }
        m_dragging = false;
        m_dragAutomationValid = false;
        m_dragMode = DragMode::None;
        m_dragUndoActive = false;
        m_dragUndoLabel.clear();
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
            startTick = SnapTick(XToTick(logicalPoint.x));
            eventValue = AutomationValueFromY(laneIndex, logicalPoint.y);
            m_dragUndoLabel = wxString::Format("Add %s Event", kAutomationLanes[laneIndex].label);
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
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        hitNote = HitTestNote(logicalPoint);
        if (hitNote >= 0) {
            m_selectedItemKind = PianoRollSelectionKind::Note;
            m_selectedNote = hitNote;
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
        } else if (HitTestAutomation(logicalPoint, &automationHit)) {
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
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            if (m_selectedNote < 0 || !HasTrack()) {
                return;
            }
            updatedNote = m_dragOriginalNote;
            if (m_dragMode == DragMode::Move) {
                snappedTick = SnapTick(XToTick(logicalPoint.x));
                snappedNote = YToNote(logicalPoint.y);
                if (snappedTick >= m_dragStartTick) {
                    updatedNote.startTick = m_dragOriginalNote.startTick + (snappedTick - m_dragStartTick);
                } else {
                    uint32_t deltaTicks;

                    deltaTicks = m_dragStartTick - snappedTick;
                    updatedNote.startTick = (deltaTicks > m_dragOriginalNote.startTick) ? 0 : (m_dragOriginalNote.startTick - deltaTicks);
                }
                updatedNote.note = static_cast<unsigned char>(std::clamp(static_cast<int>(m_dragOriginalNote.note) + (snappedNote - m_dragStartNote), 0, 127));
            } else if (m_dragMode == DragMode::ResizeLeft) {
                uint32_t originalEndTick;
                uint32_t snappedStartTick;
                uint32_t maxStartTick;

                originalEndTick = m_dragOriginalNote.startTick + m_dragOriginalNote.durationTicks;
                snappedStartTick = SnapTick(XToTick(logicalPoint.x));
                maxStartTick = (originalEndTick > kSnapTicks) ? (originalEndTick - kSnapTicks) : 0;
                updatedNote.startTick = std::min(snappedStartTick, maxStartTick);
                updatedNote.durationTicks = std::max<uint32_t>(kSnapTicks, originalEndTick - updatedNote.startTick);
            } else if (m_dragMode == DragMode::ResizeRight) {
                uint32_t snappedEndTick;

                snappedEndTick = SnapTick(XToTick(logicalPoint.x));
                if (snappedEndTick <= m_dragOriginalNote.startTick + kSnapTicks) {
                    snappedEndTick = m_dragOriginalNote.startTick + kSnapTicks;
                }
                updatedNote.durationTicks = snappedEndTick - m_dragOriginalNote.startTick;
            }
            if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(m_selectedNote),
                                                 &updatedNote) == BAE_NO_ERROR) {
                UpdateVirtualSize();
                Refresh();
            }
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
            automationTick = std::clamp(automationTick,
                                        currentTick + kSnapTicks,
                                        nextNextTick > kSnapTicks ? (nextNextTick - kSnapTicks) : currentTick + kSnapTicks);
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

            previousTick = 0;
            nextTick = GetDocumentEndTick();
            eventCount = 0;
            GetAutomationEventCount(m_selectedAutomationLane, &eventCount);
            if (m_selectedAutomationEvent > 0) {
                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) - 1,
                                   &previousTick,
                                   &previousValue);
                previousTick += kSnapTicks;
            }
            if (static_cast<uint32_t>(m_selectedAutomationEvent) + 1 < eventCount) {
                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                   &nextTick,
                                   &nextValue);
                if (nextTick > kSnapTicks) {
                    nextTick -= kSnapTicks;
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
        if (event.AltDown() && !event.ControlDown()) {
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
                return;
            }
            steps = wheelRotation / wheelDelta;
            if (steps == 0) {
                steps = (wheelRotation > 0) ? 1 : -1;
            }
            linesPerStep = std::max(1, event.GetLinesPerAction());
            GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
            if (scrollPixelsX <= 0) {
                return;
            }
            GetViewStart(&viewUnitsX, &viewUnitsY);
            currentLeft = viewUnitsX * scrollPixelsX;
            clientWidth = std::max(1, GetClientSize().GetWidth());
            virtualSize = GetVirtualSize();
            maxLeft = std::max(0, virtualSize.GetWidth() - clientWidth);
            newLeft = currentLeft - (steps * linesPerStep * scrollPixelsX * 2);
            newLeft = std::clamp(newLeft, 0, maxLeft);
            Scroll(newLeft / scrollPixelsX, viewUnitsY);
            return;
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

class WaveformPanel final : public wxPanel {
public:
    explicit WaveformPanel(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 180), wxBORDER_SIMPLE),
          m_waveData(nullptr),
          m_frameCount(0),
          m_bitSize(16),
          m_channels(1) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &WaveformPanel::OnPaint, this);
    }

    void SetWaveform(void const *waveData, uint32_t frameCount, uint16_t bitSize, uint16_t channels) {
        m_waveData = waveData;
        m_frameCount = frameCount;
        m_bitSize = bitSize;
        m_channels = channels;
        Refresh();
    }

private:
    void const *m_waveData;
    uint32_t m_frameCount;
    uint16_t m_bitSize;
    uint16_t m_channels;

    float SampleAt(uint32_t frame) const {
        if (!m_waveData || m_frameCount == 0 || frame >= m_frameCount || m_channels == 0) {
            return 0.0f;
        }
        if (m_bitSize == 8) {
            unsigned char const *pcm = static_cast<unsigned char const *>(m_waveData);
            uint32_t idx = frame * m_channels;
            int value = static_cast<int>(pcm[idx]) - 128;
            return static_cast<float>(value) / 128.0f;
        }
        if (m_bitSize == 16) {
            int16_t const *pcm = static_cast<int16_t const *>(m_waveData);
            uint32_t idx = frame * m_channels;
            return static_cast<float>(pcm[idx]) / 32768.0f;
        }
        return 0.0f;
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        int centerY = size.y / 2;

        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();
        dc.SetPen(wxPen(wxColour(70, 70, 70), 1));
        dc.DrawLine(0, centerY, size.x, centerY);

        if (!m_waveData || m_frameCount == 0 || size.x <= 2 || size.y <= 2) {
            dc.SetTextForeground(wxColour(200, 200, 200));
            dc.DrawText("No waveform data", 10, 10);
            return;
        }

        dc.SetPen(wxPen(wxColour(98, 215, 255), 1));
        uint32_t framesPerPixel = std::max<uint32_t>(1, m_frameCount / static_cast<uint32_t>(std::max(1, size.x)));
        for (int x = 0; x < size.x; ++x) {
            uint32_t start = static_cast<uint32_t>(x) * framesPerPixel;
            uint32_t end = std::min<uint32_t>(m_frameCount, start + framesPerPixel);
            float minV = 1.0f;
            float maxV = -1.0f;
            uint32_t frame;

            if (start >= m_frameCount) {
                break;
            }
            if (end <= start) {
                end = std::min<uint32_t>(m_frameCount, start + 1);
            }
            for (frame = start; frame < end; ++frame) {
                float v = SampleAt(frame);
                if (v < minV) {
                    minV = v;
                }
                if (v > maxV) {
                    maxV = v;
                }
            }
            int y1 = centerY - static_cast<int>(maxV * (centerY - 4));
            int y2 = centerY - static_cast<int>(minV * (centerY - 4));
            dc.DrawLine(x, y1, x, y2);
        }
    }
};

class InstrumentEditorDialog final : public wxDialog {
public:
    struct EditedSample {
        wxString displayName;
        wxString sourcePath;
        unsigned char program;
        unsigned char rootKey;
        unsigned char lowKey;
        unsigned char highKey;
        BAESampleInfo sampleInfo;
    };

    InstrumentEditorDialog(wxWindow *parent,
                           BAERmfEditorDocument const *document,
                           uint32_t primarySampleIndex,
                           std::function<void(uint32_t, int)> playCallback,
                                                     std::function<void()> stopCallback,
                                                     std::function<bool(uint32_t, wxString const &)> replaceCallback,
                                                     std::function<bool(uint32_t, wxString const &)> exportCallback)
        : wxDialog(parent, wxID_ANY, "Embedded Instrument", wxDefaultPosition, wxSize(640, 460), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_document(document),
          m_currentLocalIndex(-1),
          m_playCallback(std::move(playCallback)),
                    m_stopCallback(std::move(stopCallback)),
                    m_replaceCallback(std::move(replaceCallback)),
                    m_exportCallback(std::move(exportCallback)) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *grid = new wxFlexGridSizer(2, 6, 8, 8);
        wxBoxSizer *triggerSizer = new wxBoxSizer(wxHORIZONTAL);
        wxBoxSizer *instrumentSizer = new wxBoxSizer(wxHORIZONTAL);

        BuildSampleGroup(primarySampleIndex);
        if (m_sampleIndices.empty()) {
            return;
        }

        m_splitChoice = new wxChoice(this, wxID_ANY);
        m_nameText = new wxTextCtrl(this, wxID_ANY, "");
        m_programSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, m_samples[0].program);
        m_rootSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 60);
        m_lowSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        m_highSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 127);
        m_triggerSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS, 0, 127, 60);
        m_playButton = new wxButton(this, wxID_ANY, "Play");
        m_stopButton = new wxButton(this, wxID_ANY, "Stop");
        m_replaceButton = new wxButton(this, wxID_ANY, "Replace Sample...");
        m_exportButton = new wxButton(this, wxID_ANY, "Save Sample...");
        m_rangeLabel = new wxStaticText(this, wxID_ANY, "");
        m_waveformPanel = new WaveformPanel(this);

        for (size_t i = 0; i < m_samples.size(); ++i) {
            m_splitChoice->Append(BuildSplitLabel(static_cast<int>(i)));
        }

        instrumentSizer->Add(new wxStaticText(this, wxID_ANY, "Instrument Program"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        instrumentSizer->Add(m_programSpin, 0, wxRIGHT, 16);
        instrumentSizer->Add(new wxStaticText(this, wxID_ANY, "Sample"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        instrumentSizer->Add(m_splitChoice, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, "Title"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_nameText, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, "Key Range"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_rangeLabel, 1, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(this, wxID_ANY, "Root"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_rootSpin, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, "Low Key"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_lowSpin, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, "High Key"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_highSpin, 1, wxEXPAND);
        grid->AddGrowableCol(1, 1);
        grid->AddGrowableCol(3, 1);
        grid->AddGrowableCol(5, 1);

        triggerSizer->Add(new wxStaticText(this, wxID_ANY, "Preview Key"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        triggerSizer->Add(m_triggerSpin, 0, wxRIGHT, 8);
        triggerSizer->Add(m_playButton, 0, wxRIGHT, 8);
        triggerSizer->Add(m_stopButton, 0, wxRIGHT, 8);
        triggerSizer->Add(m_replaceButton, 0, wxRIGHT, 8);
        triggerSizer->Add(m_exportButton, 0, wxRIGHT, 8);

        rootSizer->Add(instrumentSizer, 0, wxEXPAND | wxALL, 10);
        rootSizer->Add(grid, 0, wxEXPAND | wxALL, 10);
        rootSizer->Add(triggerSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        rootSizer->Add(new wxStaticText(this, wxID_ANY, "Waveform"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        rootSizer->Add(m_waveformPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        rootSizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
        SetSizerAndFit(rootSizer);

        Bind(wxEVT_CHOICE, &InstrumentEditorDialog::OnSplitChanged, this, m_splitChoice->GetId());
        Bind(wxEVT_BUTTON, &InstrumentEditorDialog::OnPlay, this, m_playButton->GetId());
        Bind(wxEVT_BUTTON, &InstrumentEditorDialog::OnStop, this, m_stopButton->GetId());
        Bind(wxEVT_BUTTON, &InstrumentEditorDialog::OnReplace, this, m_replaceButton->GetId());
        Bind(wxEVT_BUTTON, &InstrumentEditorDialog::OnExport, this, m_exportButton->GetId());
        Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { ClampRange(); }, m_lowSpin->GetId());
        Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { ClampRange(); }, m_highSpin->GetId());
        m_splitChoice->SetSelection(0);
        LoadLocalSample(0);
    }

    std::vector<uint32_t> const &GetSampleIndices() const {
        return m_sampleIndices;
    }

    std::vector<EditedSample> const &GetEditedSamples() {
        SaveCurrentFromUI();
        return m_samples;
    }

private:
    static wxString SanitizeDisplayName(wxString const &name) {
        wxString clean;
        for (wxUniChar ch : name) {
            if (ch >= 32 && ch != 127) {
                clean.Append(ch);
            }
        }
        if (clean.IsEmpty()) {
            clean = "Embedded Sample";
        }
        return clean;
    }

    BAERmfEditorDocument const *m_document;
    std::vector<uint32_t> m_sampleIndices;
    std::vector<EditedSample> m_samples;
    int m_currentLocalIndex;
    std::function<void(uint32_t, int)> m_playCallback;
    std::function<void()> m_stopCallback;
    std::function<bool(uint32_t, wxString const &)> m_replaceCallback;
    std::function<bool(uint32_t, wxString const &)> m_exportCallback;
    wxChoice *m_splitChoice;
    wxTextCtrl *m_nameText;
    wxSpinCtrl *m_programSpin;
    wxSpinCtrl *m_rootSpin;
    wxSpinCtrl *m_lowSpin;
    wxSpinCtrl *m_highSpin;
    wxSpinCtrl *m_triggerSpin;
    wxButton *m_playButton;
    wxButton *m_stopButton;
    wxButton *m_replaceButton;
    wxButton *m_exportButton;
    wxStaticText *m_rangeLabel;
    WaveformPanel *m_waveformPanel;

    void BuildSampleGroup(uint32_t primarySampleIndex) {
        uint32_t sampleCount;
        BAERmfEditorSampleInfo primaryInfo;

        sampleCount = 0;
        if (!m_document ||
            BAERmfEditorDocument_GetSampleInfo(m_document, primarySampleIndex, &primaryInfo) != BAE_NO_ERROR ||
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) != BAE_NO_ERROR) {
            return;
        }
        for (uint32_t i = 0; i < sampleCount; ++i) {
            BAERmfEditorSampleInfo info;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR) {
                continue;
            }
            if (info.program == primaryInfo.program) {
                EditedSample edited;
                edited.displayName = SanitizeDisplayName(info.displayName ? wxString::FromUTF8(info.displayName) : wxString());
                edited.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
                edited.program = info.program;
                edited.rootKey = info.rootKey;
                edited.lowKey = info.lowKey;
                edited.highKey = info.highKey;
                edited.sampleInfo = info.sampleInfo;
                m_sampleIndices.push_back(i);
                m_samples.push_back(edited);
            }
        }
    }

    wxString BuildSplitLabel(int localIndex) const {
        EditedSample const &s = m_samples[localIndex];
        return wxString::Format("%d-%d: %s", static_cast<int>(s.lowKey), static_cast<int>(s.highKey), s.displayName);
    }

    void SaveCurrentFromUI() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_samples.size())) {
            return;
        }
        EditedSample &sample = m_samples[static_cast<size_t>(m_currentLocalIndex)];
        sample.displayName = SanitizeDisplayName(m_nameText->GetValue());
        sample.rootKey = static_cast<unsigned char>(m_rootSpin->GetValue());
        sample.lowKey = static_cast<unsigned char>(m_lowSpin->GetValue());
        sample.highKey = static_cast<unsigned char>(m_highSpin->GetValue());
        sample.program = static_cast<unsigned char>(m_programSpin->GetValue());
        m_rangeLabel->SetLabel(wxString::Format("%d-%d", static_cast<int>(sample.lowKey), static_cast<int>(sample.highKey)));
        if (m_splitChoice && m_currentLocalIndex < static_cast<int>(m_splitChoice->GetCount())) {
            m_splitChoice->SetString(m_currentLocalIndex, BuildSplitLabel(m_currentLocalIndex));
        }
    }

    void LoadLocalSample(int localIndex) {
        if (localIndex < 0 || localIndex >= static_cast<int>(m_samples.size())) {
            return;
        }
        m_currentLocalIndex = localIndex;
        EditedSample const &sample = m_samples[static_cast<size_t>(localIndex)];
        m_nameText->SetValue(sample.displayName);
        m_rangeLabel->SetLabel(wxString::Format("%d-%d", static_cast<int>(sample.lowKey), static_cast<int>(sample.highKey)));
        m_rootSpin->SetValue(sample.rootKey);
        m_lowSpin->SetValue(sample.lowKey);
        m_highSpin->SetValue(sample.highKey);
        m_programSpin->SetValue(sample.program);
        m_triggerSpin->SetValue(sample.rootKey);
        RefreshWaveform();
    }

    void ClampRange() {
        if (m_lowSpin->GetValue() > m_highSpin->GetValue()) {
            m_highSpin->SetValue(m_lowSpin->GetValue());
        }
    }

    void RefreshWaveform() {
        void const *waveData;
        uint32_t frameCount;
        uint16_t bitSize;
        uint16_t channels;
        BAE_UNSIGNED_FIXED sampleRate;
        uint32_t sampleIndex;

        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_sampleIndices.size())) {
            return;
        }
        sampleIndex = m_sampleIndices[static_cast<size_t>(m_currentLocalIndex)];
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
                                                       &sampleRate) == BAE_NO_ERROR) {
            m_waveformPanel->SetWaveform(waveData, frameCount, bitSize, channels);
        }
    }

    void OnSplitChanged(wxCommandEvent &) {
        int selection;
        SaveCurrentFromUI();
        selection = m_splitChoice->GetSelection();
        if (selection >= 0) {
            LoadLocalSample(selection);
        }
    }

    void OnPlay(wxCommandEvent &) {
        uint32_t sampleIndex;
        if (!m_playCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_sampleIndices.size())) {
            return;
        }
        sampleIndex = m_sampleIndices[static_cast<size_t>(m_currentLocalIndex)];
        m_playCallback(sampleIndex, m_triggerSpin->GetValue());
    }

    void OnStop(wxCommandEvent &) {
        if (m_stopCallback) {
            m_stopCallback();
        }
    }

    void OnReplace(wxCommandEvent &) {
        uint32_t sampleIndex;
        wxFileDialog dialog(this,
                            "Replace Embedded Sample",
                            wxEmptyString,
                            wxEmptyString,
                            "Audio files (*.wav;*.aif;*.aiff)|*.wav;*.aif;*.aiff|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (!m_replaceCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_sampleIndices.size())) {
            return;
        }
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        sampleIndex = m_sampleIndices[static_cast<size_t>(m_currentLocalIndex)];
        if (!m_replaceCallback(sampleIndex, dialog.GetPath())) {
            wxMessageBox("Failed to replace sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        {
            BAERmfEditorSampleInfo info;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &info) == BAE_NO_ERROR) {
                EditedSample &sample = m_samples[static_cast<size_t>(m_currentLocalIndex)];
                sample.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
                sample.sampleInfo = info.sampleInfo;
            }
        }
        RefreshWaveform();
    }

    void OnExport(wxCommandEvent &) {
        uint32_t sampleIndex;
        wxString codec;
        char codecBuf[64];
        wxString suggestedName;
        wxString defaultExt;
        wxFileDialog dialog(this,
                            "Save Embedded Sample",
                            wxEmptyString,
                            wxEmptyString,
                            "WAV files (*.wav)|*.wav|AIFF files (*.aif)|*.aif|All files (*.*)|*.*",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (!m_exportCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_sampleIndices.size())) {
            return;
        }
        sampleIndex = m_sampleIndices[static_cast<size_t>(m_currentLocalIndex)];
        codecBuf[0] = 0;
        if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, sampleIndex, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
            codec = wxString::FromUTF8(codecBuf);
        }
        defaultExt = (codec.Lower().Contains("ima")) ? "aif" : "wav";
        suggestedName = SanitizeDisplayName(m_nameText->GetValue()) + "." + defaultExt;
        dialog.SetFilename(suggestedName);

        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        if (!m_exportCallback(sampleIndex, dialog.GetPath())) {
            wxMessageBox("Failed to save sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        if (!codec.IsEmpty()) {
            wxMessageBox(wxString::Format("Saved sample. Source codec: %s", codec),
                         "Embedded Instruments",
                         wxOK | wxICON_INFORMATION,
                         this);
        }
    }
};

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
        m_playScopeChoice = new wxChoice(editorPanel, wxID_ANY);
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
        transportSizer->Add(m_playScopeChoice, 0, wxALIGN_CENTER_VERTICAL, 0);

        m_positionSlider = new wxSlider(editorPanel, wxID_ANY, 0, 0, 1000, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);

        m_pianoRoll = new PianoRollPanel(editorPanel);
        editorSizer->Add(controlsSizer, 0, wxEXPAND | wxALL, 10);
        editorSizer->Add(transportSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        editorSizer->Add(m_positionSlider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        editorSizer->Add(m_pianoRoll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        editorPanel->SetSizer(editorSizer);

        m_pianoRoll->SetSelectionChangedCallback([this]() { UpdateControlsFromSelection(); });
        m_pianoRoll->SetUndoCallbacks([this](wxString const &label) { BeginUndoAction(label); },
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
        m_positionSlider->Bind(wxEVT_SLIDER, &MainFrame::OnSeekSlider, this);
        Bind(wxEVT_MENU, &MainFrame::OnTrackAdd, this, ID_TrackAdd);
        Bind(wxEVT_MENU, &MainFrame::OnTrackDelete, this, ID_TrackDelete);
        Bind(wxEVT_MENU, &MainFrame::OnSampleAdd, this, ID_SampleAdd);
        Bind(wxEVT_BUTTON, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnSampleRename, this, ID_SampleRename);
        Bind(wxEVT_MENU, &MainFrame::OnSampleDelete, this, ID_SampleDelete);
        m_sampleList->Bind(wxEVT_LISTBOX_DCLICK, &MainFrame::OnSampleEdit, this);
        Bind(wxEVT_MENU, &MainFrame::OnLoadBank, this, ID_LoadBank);
        Bind(wxEVT_MENU, &MainFrame::OnUnloadBanks, this, ID_UnloadBanks);
        Bind(wxEVT_TIMER, &MainFrame::OnPlaybackTimer, this);
        UpdateUndoMenuState();

    }

    ~MainFrame() override {
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
    wxChoice *m_playScopeChoice;
    wxSlider *m_positionSlider;
    wxListBox *m_sampleList;
    wxButton *m_sampleAddButton;
    BAEMixer m_playbackMixer;
    BAESong m_playbackSong;
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
            for (int ccSlot = 0; ccSlot < kAutomationLaneCount - 1; ++ccSlot) {
                uint32_t eventCount;
                unsigned char controller;

                controller = GetUndoCCController(ccSlot);
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, controller, &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                trackState.ccEvents[ccSlot].reserve(eventCount);
                for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    UndoCCEventState eventState;

                    if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                             trackIndex,
                                                             controller,
                                                             eventIndex,
                                                             &eventState.tick,
                                                             &eventState.value) == BAE_NO_ERROR) {
                        trackState.ccEvents[ccSlot].push_back(eventState);
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
            for (int ccSlot = 0; ccSlot < kAutomationLaneCount - 1; ++ccSlot) {
                uint32_t eventCount;
                unsigned char controller;

                controller = GetUndoCCController(ccSlot);
                eventCount = 0;
                BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, controller, &eventCount);
                for (uint32_t eventIndex = eventCount; eventIndex > 0; --eventIndex) {
                    BAERmfEditorDocument_DeleteTrackCCEvent(m_document, trackIndex, controller, eventIndex - 1);
                }
                for (UndoCCEventState const &eventState : trackState.ccEvents[ccSlot]) {
                    if (BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                             trackIndex,
                                                             controller,
                                                             eventState.tick,
                                                             eventState.value) != BAE_NO_ERROR) {
                        return false;
                    }
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
        m_pianoRoll->RefreshFromDocument(true);
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
        m_pianoRoll->RefreshFromDocument(true);
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
        if (!m_document) {
            return;
        }
        sampleCount = 0;
        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        for (sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
            BAERmfEditorSampleInfo sampleInfo;
            wxString label;

            if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
                continue;
            }
            label = wxString::Format("P%u Root %u [%u-%u] %s",
                                     static_cast<unsigned>(sampleInfo.program),
                                     static_cast<unsigned>(sampleInfo.rootKey),
                                     static_cast<unsigned>(sampleInfo.lowKey),
                                     static_cast<unsigned>(sampleInfo.highKey),
                                     sampleInfo.displayName ? sampleInfo.displayName : "(unnamed)");
            m_sampleList->Append(label);
        }
    }

    bool PreviewSampleAtKey(uint32_t sampleIndex, int midiKey) {
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
        if (!m_previewSound) {
            m_previewSound = BAESound_New(m_playbackMixer);
            if (!m_previewSound) {
                return false;
            }
        }

        stopResult = BAESound_Stop(m_previewSound, FALSE);
        fprintf(stderr, "[nbstudio] Preview sample %u stop result=%d\n", static_cast<unsigned>(sampleIndex), static_cast<int>(stopResult));

        loadResult = BAE_GENERAL_ERR;

        if (sampleInfo.sourcePath && sampleInfo.sourcePath[0]) {
            BAEFileType sourceType;

            sourceType = X_DetermineFileTypeByPath(sampleInfo.sourcePath);
            if (sourceType == BAE_WAVE_TYPE || sourceType == BAE_AIFF_TYPE || sourceType == BAE_AU_TYPE) {
                loadResult = BAESound_LoadFileSample(m_previewSound,
                                                     const_cast<char *>(sampleInfo.sourcePath),
                                                     sourceType);
                fprintf(stderr,
                        "[nbstudio] Preview sample %u source='%s' type=%d load result=%d\n",
                        static_cast<unsigned>(sampleIndex),
                        sampleInfo.sourcePath,
                        static_cast<int>(sourceType),
                        static_cast<int>(loadResult));
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

            utf8Path = m_previewSampleTempPath.utf8_str();
            if (loadResult != BAE_NO_ERROR && BAERmfEditorDocument_ExportSampleToFile(m_document,
                                                        sampleIndex,
                                                        const_cast<char *>(utf8Path.data())) == BAE_NO_ERROR) {
                loadResult = BAESound_LoadFileSample(m_previewSound,
                                                     const_cast<char *>(utf8Path.data()),
                                                     BAE_WAVE_TYPE);
                fprintf(stderr,
                        "[nbstudio] Preview sample %u temp='%s' load result=%d\n",
                        static_cast<unsigned>(sampleIndex),
                        utf8Path.data(),
                        static_cast<int>(loadResult));
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
            if (loopStart >= frameCount || loopEnd > frameCount || loopEnd <= loopStart) {
                loopStart = 0;
                loopEnd = 0;
            }
            loadResult = BAESound_LoadCustomSample(m_previewSound,
                                                   const_cast<void *>(waveData),
                                                   frameCount,
                                                   bitSize,
                                                   channels,
                                                   sampleRate,
                                                   loopStart,
                                                   loopEnd);
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
        if (previewRoot < 0 || previewRoot > 127) {
            previewRoot = static_cast<int>(sampleInfo.sampleInfo.baseMidiPitch & 0x7F);
        }
        int semitones = midiKey - previewRoot;
        double ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);
        BAE_UNSIGNED_FIXED rate = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(static_cast<double>(baseSampleRate) * ratio,
                                             static_cast<double>(4000U << 16),
                                             static_cast<double>(192000U << 16)));
        BAE_UNSIGNED_FIXED volume = static_cast<BAE_UNSIGNED_FIXED>(0.7 * 65536.0);

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
        fprintf(stderr,
                "[nbstudio] Preview sample %u Start result=%d volume=%lu rate=%lu midiKey=%d root=%d\n",
                static_cast<unsigned>(sampleIndex),
                static_cast<int>(startResult),
                static_cast<unsigned long>(volume),
                static_cast<unsigned long>(rate),
                midiKey,
                previewRoot);
        if (startResult != BAE_NO_ERROR) {
            return false;
        }
        return true;
    }

    void StopPreviewSample() {
        if (m_previewSound) {
            BAESound_Stop(m_previewSound, TRUE);
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
        BAESampleInfo sampleInfo;
        wxScopedCharBuffer utf8Path;

        if (!m_document) {
            return false;
        }
        utf8Path = path.utf8_str();
        return BAERmfEditorDocument_ReplaceSampleFromFile(m_document,
                                                          sampleIndex,
                                                          const_cast<char *>(utf8Path.data()),
                                                          &sampleInfo) == BAE_NO_ERROR;
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
        BAERmfEditorTrackInfo trackInfo;
        BAERmfEditorTrackSetup setup;
        uint16_t addedTrackIndex;
        uint32_t noteCount;
        uint32_t noteIndex;
        uint32_t ccCount;
        uint32_t copiedSampleCount;
        unsigned char requiredPrograms[128];
        uint32_t tempo;
        uint16_t tpq;

        if (!m_document || trackIndex < 0) {
            return nullptr;
        }
        if (BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(trackIndex), &trackInfo) != BAE_NO_ERROR) {
            return nullptr;
        }
        playDoc = BAERmfEditorDocument_New();
        if (!playDoc) {
            return nullptr;
        }
        tempo = 120;
        tpq = 480;
        BAERmfEditorDocument_GetTempoBPM(m_document, &tempo);
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        BAERmfEditorDocument_SetTempoBPM(playDoc, tempo);
        BAERmfEditorDocument_SetTicksPerQuarter(playDoc, tpq);
        BAERmfEditorDocument_CopyTempoMapFrom(playDoc, m_document);
        setup.channel = trackInfo.channel;
        setup.bank = trackInfo.bank;
        setup.program = trackInfo.program;
        setup.name = const_cast<char *>(trackInfo.name);
        if (BAERmfEditorDocument_AddTrack(playDoc, &setup, &addedTrackIndex) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }
        BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(trackIndex), &noteCount);
        std::fill(requiredPrograms, requiredPrograms + 128, static_cast<unsigned char>(0));
        if (trackInfo.program < 128) {
            requiredPrograms[trackInfo.program] = 1;
        }
        for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
            BAERmfEditorNoteInfo noteInfo;

            if (BAERmfEditorDocument_GetNoteInfo(m_document, static_cast<uint16_t>(trackIndex), noteIndex, &noteInfo) == BAE_NO_ERROR) {
                if (noteInfo.program < 128) {
                    requiredPrograms[noteInfo.program] = 1;
                }
                BAERmfEditorDocument_AddNote(playDoc,
                                             addedTrackIndex,
                                             noteInfo.startTick,
                                             noteInfo.durationTicks,
                                             noteInfo.note,
                                             noteInfo.velocity);
                BAERmfEditorDocument_SetNoteInfo(playDoc,
                                                 addedTrackIndex,
                                                 noteIndex,
                                                 &noteInfo);
            }
        }
        fprintf(stderr,
                "[nbstudio] Single-track build srcTrack=%d notes=%u trackBankRaw=%u trackBankDisplay=%u trackProgram=%u\n",
                trackIndex,
                static_cast<unsigned>(noteCount),
                static_cast<unsigned>(trackInfo.bank),
                static_cast<unsigned>(DisplayBankFromInternal(trackInfo.bank)),
                static_cast<unsigned>(trackInfo.program));
        trackInfo.noteCount = noteCount;
        BAERmfEditorDocument_SetTrackInfo(playDoc, addedTrackIndex, &trackInfo);
        for (unsigned char cc : { static_cast<unsigned char>(7), static_cast<unsigned char>(10), static_cast<unsigned char>(11) }) {
            ccCount = 0;
            BAERmfEditorDocument_GetTrackCCEventCount(m_document, static_cast<uint16_t>(trackIndex), cc, &ccCount);
            for (uint32_t ccIndex = 0; ccIndex < ccCount; ++ccIndex) {
                uint32_t tick;
                unsigned char value;

                if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                         static_cast<uint16_t>(trackIndex),
                                                         cc,
                                                         ccIndex,
                                                         &tick,
                                                         &value) == BAE_NO_ERROR) {
                    BAERmfEditorDocument_AddTrackCCEvent(playDoc,
                                                         addedTrackIndex,
                                                         cc,
                                                         tick,
                                                         value);
                }
            }
        }
        copiedSampleCount = 0;
        if (BAERmfEditorDocument_CopySamplesForPrograms(playDoc,
                                                        m_document,
                                                        requiredPrograms,
                                                        &copiedSampleCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }
        fprintf(stderr,
                "[nbstudio] Single-track sample copy copied=%u\n",
                static_cast<unsigned>(copiedSampleCount));
        return playDoc;
    }

    void StopPlayback(bool releaseSong) {
        if (!m_playbackSong) {
            m_playbackTimer.Stop();
            return;
        }
        BAESong_Stop(m_playbackSong, FALSE);
        if (releaseSong) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            m_playbackTimer.Stop();
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

    uint32_t MicrosecondsToTicks(uint32_t usec) const {
        uint32_t tempo;
        uint16_t tpq;
        uint64_t ticks;

        if (!m_document) {
            return 0;
        }
        tempo = 120;
        tpq = 480;
        BAERmfEditorDocument_GetTempoBPM(m_document, &tempo);
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        ticks = (static_cast<uint64_t>(usec) * static_cast<uint64_t>(tempo) * static_cast<uint64_t>(tpq)) / 60000000ULL;
        return static_cast<uint32_t>(ticks);
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
        m_pianoRoll->SetDocument(m_document);
        m_pianoRoll->ClearPlayhead();
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
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
            m_pianoRoll->SetSelectedTrack(0);
        } else {
            m_pianoRoll->SetSelectedTrack(-1);
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

            if (m_pianoRoll->GetSelectedNoteInfo(&selectedNote)) {
                m_bankSpin->SetValue(DisplayBankFromInternal(selectedNote.bank));
                m_programSpin->SetValue(selectedNote.program);
            } else {
                m_bankSpin->SetValue(DisplayBankFromInternal(trackInfo.bank));
                m_programSpin->SetValue(trackInfo.program);
            }
            m_transposeSpin->SetValue(trackInfo.transpose);
            m_panSpin->SetValue(trackInfo.pan);
            m_volumeSpin->SetValue(trackInfo.volume);
            m_pianoRoll->SetNewNoteInstrument(InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue())), static_cast<unsigned char>(m_programSpin->GetValue()));
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
                            "RMF and MIDI files (*.rmf;*.mid;*.midi)|*.rmf;*.mid;*.midi|RMF files (*.rmf)|*.rmf|MIDI files (*.mid;*.midi)|*.mid;*.midi|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            LoadDocument(dialog.GetPath());
        }
    }

    void OnSaveAs(wxCommandEvent &) {
        wxFileDialog dialog(this,
                            "Save As RMF",
                            wxEmptyString,
                            wxEmptyString,
                            "RMF files (*.rmf)|*.rmf",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        BAERmfEditorDocument *saveDoc;
        bool saveCurrentTrack;
        int selectedTrack;

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save As RMF", wxOK | wxICON_INFORMATION, this);
            return;
        }
        saveDoc = m_document;
        saveCurrentTrack = (m_playScopeChoice && m_playScopeChoice->GetSelection() == 1);
        selectedTrack = GetSelectedTrack();
        if (saveCurrentTrack) {
            saveDoc = BuildSingleTrackPlaybackDocument(selectedTrack);
            if (!saveDoc) {
                wxMessageBox("Failed to build selected track for RMF save.", "Save Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            fprintf(stderr,
                    "[nbstudio] SaveAs using single-track document selectedTrack=%d\n",
                    selectedTrack);
        }
        if (dialog.ShowModal() != wxID_OK) {
            if (saveDoc != m_document) {
                BAERmfEditorDocument_Delete(saveDoc);
            }
            return;
        }
        {
            wxString targetPath = dialog.GetPath();
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
        m_pianoRoll->SetSelectedTrack(GetSelectedTrack());
        UpdateControlsFromSelection();
    }

    void OnTempoChanged(wxCommandEvent &) {
        if (!m_document || m_updatingControls) {
            return;
        }
        BeginUndoAction("Change Tempo");
        if (BAERmfEditorDocument_SetTempoBPM(m_document, static_cast<uint32_t>(m_tempoSpin->GetValue())) == BAE_NO_ERROR) {
            CommitUndoAction("Change Tempo");
            m_pianoRoll->RefreshFromDocument(false);
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
        if (m_pianoRoll->GetSelectedNoteInfo(&selectedNote)) {
            selectedNote.bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
            selectedNote.program = static_cast<unsigned char>(m_programSpin->GetValue());
            m_pianoRoll->SetSelectedNoteInstrument(selectedNote.bank, selectedNote.program);
            m_pianoRoll->SetNewNoteInstrument(selectedNote.bank, selectedNote.program);
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
            m_pianoRoll->SetNewNoteInstrument(trackInfo.bank, trackInfo.program);
            m_pianoRoll->Refresh();
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

        menu.Append(ID_SampleAdd, "Add Sample");

        if (m_sampleList->GetSelection() >= 0) {
            menu.Append(ID_SampleRename, "Rename Instrument...");
            menu.Append(ID_SampleDelete, "Delete Sample");
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
            m_pianoRoll->SetSelectedTrack(newTrackIndex);
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
        wxScopedCharBuffer utf8Path;
        wxScopedCharBuffer utf8CurrentPath;
        BAERmfEditorDocument *playDoc;
        bool singleTrackMode;
        bool sourceLooksMidi;
        bool sourceLooksRmf;
        bool exportAsMidi;
        int selectedTrack;
        uint16_t sourceTrackCount;
        bool runtimeMuteSingleTrack;
        BAEResult saveResult;
        BAEResult loadResult;

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Playback", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize audio engine.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        selectedTrack = GetSelectedTrack();
        sourceTrackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &sourceTrackCount);
        singleTrackMode = (m_playScopeChoice->GetSelection() == 1);
        runtimeMuteSingleTrack = false;
        fprintf(stderr,
            "[nbstudio] OnPlay scope=%d selectedTrack=%d currentPath='%s'\n",
            singleTrackMode ? 1 : 0,
            selectedTrack,
            m_currentPath.empty() ? "" : static_cast<char const *>(m_currentPath.utf8_str()));
        sourceLooksMidi = false;
        sourceLooksRmf = false;
        if (!m_currentPath.empty()) {
            utf8CurrentPath = m_currentPath.utf8_str();
            {
                BAEFileType sourceType = X_DetermineFileTypeByPath(utf8CurrentPath.data());
                sourceLooksMidi = (sourceType == BAE_MIDI_TYPE);
                sourceLooksRmf = (sourceType == BAE_RMF);
                {
                    wxFileName srcName(m_currentPath);
                    wxString ext = srcName.GetExt().Lower();

                    /* Some detectors report RMF as MIDI; extension must win for playback routing. */
                    if (ext == "rmf") {
                        sourceLooksRmf = true;
                        sourceLooksMidi = false;
                    } else if (!sourceLooksRmf) {
                        sourceLooksMidi = sourceLooksMidi || (ext == "mid" || ext == "midi" || ext == "rmi");
                    }
                }
            }
        }
        /* Preserve embedded resources for RMF sessions, including single-track playback. */
        exportAsMidi = sourceLooksMidi && !sourceLooksRmf;
        fprintf(stderr,
            "[nbstudio] OnPlay detected sourceLooksMidi=%d sourceLooksRmf=%d exportAsMidi=%d tempPath='%s'\n",
            sourceLooksMidi ? 1 : 0,
            sourceLooksRmf ? 1 : 0,
            exportAsMidi ? 1 : 0,
            static_cast<char const *>(m_playbackTempPath.utf8_str()));
        if (!EnsurePlaybackTempPath(exportAsMidi)) {
            wxMessageBox("Failed to create temporary playback file.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        playDoc = m_document;
        if (singleTrackMode) {
            if (selectedTrack < 0 || selectedTrack >= static_cast<int>(sourceTrackCount)) {
                wxMessageBox("No valid track selected for Current Track playback.", "Playback Error", wxOK | wxICON_ERROR, this);
                return;
            }
            /*
             * Full-document RMF playback is known-good for instrument loading.
             * For current-track audition, isolate by muting other tracks at runtime.
             */
            runtimeMuteSingleTrack = true;
        }
        utf8Path = m_playbackTempPath.utf8_str();
        if (exportAsMidi) {
            saveResult = BAERmfEditorDocument_SaveAsMidi(playDoc, const_cast<char *>(utf8Path.data()));
        } else {
            saveResult = BAERmfEditorDocument_SaveAsRmf(playDoc, const_cast<char *>(utf8Path.data()));
        }
        if (saveResult != BAE_NO_ERROR) {
            if (playDoc != m_document) {
                BAERmfEditorDocument_Delete(playDoc);
            }
            wxMessageBox(exportAsMidi ? "Failed to build playback MIDI." : "Failed to build playback RMF.",
                         "Playback Error",
                         wxOK | wxICON_ERROR,
                         this);
            return;
        }
        if (playDoc != m_document) {
            BAERmfEditorDocument_Delete(playDoc);
        }
        StopPlayback(true);
        m_playbackSong = BAESong_New(m_playbackMixer);
        if (!m_playbackSong) {
            wxMessageBox("Failed to allocate playback song.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        if (exportAsMidi) {
            fprintf(stderr, "[nbstudio] Loading MIDI from file: %s\n", utf8Path.data());
            loadResult = BAESong_LoadMidiFromFile(m_playbackSong, const_cast<char *>(utf8Path.data()), TRUE);
            fprintf(stderr, "[nbstudio] BAESong_LoadMidiFromFile result=%d\n", static_cast<int>(loadResult));
        } else {
            fprintf(stderr, "[nbstudio] Loading RMF from file: %s\n", utf8Path.data());
            loadResult = BAESong_LoadRmfFromFile(m_playbackSong, const_cast<char *>(utf8Path.data()), 0, TRUE);
            fprintf(stderr, "[nbstudio] BAESong_LoadRmfFromFile result=%d\n", static_cast<int>(loadResult));
        }
        if (loadResult != BAE_NO_ERROR) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            wxMessageBox("Failed to load playback song.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
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

            if (runtimeMuteSingleTrack) {
                uint16_t trackIndex;
                uint16_t engineSelectedTrack;

                engineSelectedTrack = static_cast<uint16_t>(selectedTrack + 1);

                for (trackIndex = 0; trackIndex < sourceTrackCount; ++trackIndex) {
                    BAEResult muteResult;
                    uint16_t engineTrackIndex;

                    engineTrackIndex = static_cast<uint16_t>(trackIndex + 1);

                    if (engineTrackIndex == engineSelectedTrack) {
                        muteResult = BAESong_UnmuteTrack(m_playbackSong, engineTrackIndex);
                        fprintf(stderr,
                                "[nbstudio] Runtime isolate uiTrack=%u engineTrack=%u: unmute result=%d\n",
                                static_cast<unsigned>(trackIndex),
                                static_cast<unsigned>(engineTrackIndex),
                                static_cast<int>(muteResult));
                    } else {
                        muteResult = BAESong_MuteTrack(m_playbackSong, engineTrackIndex);
                        fprintf(stderr,
                                "[nbstudio] Runtime isolate uiTrack=%u engineTrack=%u: mute result=%d\n",
                                static_cast<unsigned>(trackIndex),
                                static_cast<unsigned>(engineTrackIndex),
                                static_cast<int>(muteResult));
                    }
                }
                }
        }
        BAEResult startResult = BAESong_Start(m_playbackSong, 0);
        fprintf(stderr, "[nbstudio] BAESong_Start result=%d\n", static_cast<int>(startResult));
        if (startResult != BAE_NO_ERROR) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            wxMessageBox("Failed to start playback.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
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
        m_pianoRoll->SetPlayheadTick(0);
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

    void OnStop(wxCommandEvent &) {
        StopPlayback(true);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        m_pianoRoll->ClearPlayhead();
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
                {
                    uint32_t playheadTick;

                    playheadTick = MicrosecondsToTicks(posUsec);
                    m_pianoRoll->SetPlayheadTick(playheadTick);
                    if (m_autoFollowPlayhead) {
                        m_pianoRoll->EnsurePlayheadVisible(playheadTick);
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
            SetStatusText("Playback complete", 0);
        }
    }

    void OnSeekSlider(wxCommandEvent &) {
        uint32_t lenUsec;
        uint32_t targetUsec;

        if (m_ignoreSeekEvent || !m_playbackSong) {
            return;
        }
        lenUsec = 0;
        if (BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec) != BAE_NO_ERROR || lenUsec == 0) {
            return;
        }
        m_autoFollowPlayhead = false;
        targetUsec = static_cast<uint32_t>((static_cast<uint64_t>(lenUsec) * static_cast<uint64_t>(m_positionSlider->GetValue())) / 1000ULL);
        BAESong_SetMicrosecondPosition(m_playbackSong, targetUsec);
        {
            uint32_t seekTick = MicrosecondsToTicks(targetUsec);
            m_pianoRoll->SetPlayheadTick(seekTick);
            m_pianoRoll->JumpToTick(seekTick);
        }
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
                            "Audio files (*.wav;*.aif;*.aiff)|*.wav;*.aif;*.aiff|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        BAERmfEditorSampleSetup setup;
        BAESampleInfo info;
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
        if (BAERmfEditorDocument_AddSampleFromFile(m_document,
                                                   const_cast<char *>(utf8Path.data()),
                                                   &setup,
                                                   &info) != BAE_NO_ERROR) {
            wxMessageBox("Failed to add sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        PopulateSampleList();
    }

    void OnSampleNewInstrument(wxCommandEvent &) {
        BAERmfEditorSampleSetup setup;
        BAESampleInfo info;
        uint32_t newSampleIndex;
        long program;
        long root;

        if (!m_document) {
            return;
        }

        program = wxGetNumberFromUser("MIDI program for new instrument (0-127)",
                                      "Program",
                                      "New Instrument",
                                      0,
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
            m_sampleList->SetSelection(static_cast<int>(newSampleIndex));
        }

        {
            wxCommandEvent dummyEvent;
            OnSampleEdit(dummyEvent);
        }

        if (newSampleIndex < static_cast<uint32_t>(m_sampleList->GetCount())) {
            BAERmfEditorSampleInfo verifyInfo;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, newSampleIndex, &verifyInfo) == BAE_NO_ERROR &&
                verifyInfo.sourcePath == NULL && verifyInfo.sampleInfo.waveFrames <= 1) {
                /* User likely canceled without replacing; remove placeholder sample. */
                BAERmfEditorDocument_DeleteSample(m_document, newSampleIndex);
                PopulateSampleList();
            }
        }
    }

    void OnSampleDelete(wxCommandEvent &) {
        int selected;

        if (!m_document) {
            return;
        }
        selected = m_sampleList->GetSelection();
        if (selected < 0) {
            return;
        }
        if (wxMessageBox("Delete selected sample?", "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        if (BAERmfEditorDocument_DeleteSample(m_document, static_cast<uint32_t>(selected)) == BAE_NO_ERROR) {
            PopulateSampleList();
            if (selected >= static_cast<int>(m_sampleList->GetCount())) {
                selected = static_cast<int>(m_sampleList->GetCount()) - 1;
            }
            if (selected >= 0) {
                m_sampleList->SetSelection(selected);
            }
        }
    }

    void OnSampleRename(wxCommandEvent &) {
        int selected;
        BAERmfEditorSampleInfo selectedInfo;
        wxString currentName;
        wxString newName;

        if (!m_document) {
            return;
        }
        selected = m_sampleList->GetSelection();
        if (selected < 0) {
            return;
        }
        if (BAERmfEditorDocument_GetSampleInfo(m_document, static_cast<uint32_t>(selected), &selectedInfo) != BAE_NO_ERROR) {
            return;
        }

        currentName = selectedInfo.displayName ? wxString::FromUTF8(selectedInfo.displayName) : wxString("New Instrument");
        newName = wxGetTextFromUser("Instrument name", "Rename Instrument", currentName, this);
        if (newName.IsEmpty()) {
            return;
        }

        {
            uint32_t sampleCount = 0;
            uint32_t i;

            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo info;

                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR) {
                    continue;
                }
                if (info.program == selectedInfo.program) {
                    BAERmfEditorSampleInfo updated = info;
                    wxScopedCharBuffer utf8Name = newName.utf8_str();
                    updated.displayName = const_cast<char *>(utf8Name.data());
                    BAERmfEditorDocument_SetSampleInfo(m_document, i, &updated);
                }
            }
        }

        PopulateSampleList();
        if (selected < static_cast<int>(m_sampleList->GetCount())) {
            m_sampleList->SetSelection(selected);
        }
    }

    void OnSampleEdit(wxCommandEvent &) {
        int selected;

        if (!m_document) {
            return;
        }
        selected = m_sampleList->GetSelection();
        if (selected < 0) {
            return;
        }
        InstrumentEditorDialog dialog(this,
                                      m_document,
                                      static_cast<uint32_t>(selected),
                                      [this](uint32_t sampleIndex, int key) {
                                          if (!PreviewSampleAtKey(sampleIndex, key)) {
                                              wxMessageBox("Preview playback failed for this sample.", "Sample Preview", wxOK | wxICON_ERROR, this);
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
                                      });
        if (dialog.ShowModal() != wxID_OK) {
            StopPreviewSample();
            return;
        }
        StopPreviewSample();
        {
            std::vector<uint32_t> const &indices = dialog.GetSampleIndices();
            std::vector<InstrumentEditorDialog::EditedSample> const &editedSamples = dialog.GetEditedSamples();
            bool ok = true;

            for (size_t i = 0; i < indices.size() && i < editedSamples.size(); ++i) {
                BAERmfEditorSampleInfo info;
                wxScopedCharBuffer nameUtf8 = editedSamples[i].displayName.utf8_str();
                info.displayName = const_cast<char *>(nameUtf8.data());
                info.sourcePath = NULL;
                info.program = editedSamples[i].program;
                info.rootKey = editedSamples[i].rootKey;
                info.lowKey = editedSamples[i].lowKey;
                info.highKey = editedSamples[i].highKey;
                info.sampleInfo = editedSamples[i].sampleInfo;
                if (BAERmfEditorDocument_SetSampleInfo(m_document, indices[i], &info) != BAE_NO_ERROR) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                wxMessageBox("Failed to apply instrument sample edits.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
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