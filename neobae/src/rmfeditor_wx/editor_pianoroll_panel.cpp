#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>

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
constexpr int kAutomationLaneCount = 4;
constexpr uint32_t kDefaultNoteDuration = 480;
constexpr uint32_t kSnapTicks = 120;

enum class DragMode {
    None,
    Move,
    ResizeLeft,
    ResizeRight,
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

struct AutomationLaneDescriptor {
    char const *label;
    unsigned char controller;
    wxColour color;
    int maxValue;
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
