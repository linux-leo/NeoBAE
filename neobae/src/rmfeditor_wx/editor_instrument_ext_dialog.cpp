#include <stdint.h>

#include <algorithm>
#include <climits>
#include <string>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/numdlg.h>
#include <wx/spinctrl.h>
#include <wx/timer.h>
#include <wx/wx.h>

#include "editor_instrument_ext_dialog.h"

/* ====================================================================== */
/*  FOUR_CHAR helpers for readable dropdown labels                        */
/* ====================================================================== */

namespace {

struct FourCharLabel {
    int32_t value;
    char const *label;
};

static FourCharLabel const kADSRFlagLabels[] = {
    {0,                                           "OFF"},
    {(int32_t)FOUR_CHAR('L','I','N','E'),         "LINEAR_RAMP"},
    {(int32_t)FOUR_CHAR('S','U','S','T'),         "SUSTAIN"},
    {(int32_t)FOUR_CHAR('L','A','S','T'),         "TERMINATE"},
    {(int32_t)FOUR_CHAR('G','O','T','O'),         "GOTO"},
    {(int32_t)FOUR_CHAR('G','O','S','T'),         "GOTO_COND"},
    {(int32_t)FOUR_CHAR('R','E','L','S'),         "RELEASE"},
};
static int const kADSRFlagCount = (int)(sizeof(kADSRFlagLabels) / sizeof(kADSRFlagLabels[0]));

static FourCharLabel const kLFODestLabels[] = {
    {(int32_t)FOUR_CHAR('V','O','L','U'),         "VOLUME"},
    {(int32_t)FOUR_CHAR('P','I','T','C'),         "PITCH"},
    {(int32_t)FOUR_CHAR('S','P','A','N'),         "STEREO_PAN"},
    {(int32_t)FOUR_CHAR('P','A','N',' '),         "STEREO_PAN_2"},
    {(int32_t)FOUR_CHAR('L','P','F','R'),         "LPF_FREQUENCY"},
    {(int32_t)FOUR_CHAR('L','P','R','E'),         "LPF_DEPTH"},
    {(int32_t)FOUR_CHAR('L','P','A','M'),         "LOW_PASS_AMOUNT"},
};
static int const kLFODestCount = (int)(sizeof(kLFODestLabels) / sizeof(kLFODestLabels[0]));

static FourCharLabel const kLFOShapeLabels[] = {
    {(int32_t)FOUR_CHAR('S','I','N','E'),         "SINE"},
    {(int32_t)FOUR_CHAR('T','R','I','A'),         "TRIANGLE"},
    {(int32_t)FOUR_CHAR('S','Q','U','A'),         "SQUARE"},
    {(int32_t)FOUR_CHAR('S','Q','U','2'),         "SQUARE_SYNC"},
    {(int32_t)FOUR_CHAR('S','A','W','T'),         "SAWTOOTH"},
    {(int32_t)FOUR_CHAR('S','A','W','2'),         "SAWTOOTH_SYNC"},
};
static int const kLFOShapeCount = (int)(sizeof(kLFOShapeLabels) / sizeof(kLFOShapeLabels[0]));

static int FindLabelIndex(FourCharLabel const *table, int count, int32_t value) {
    for (int i = 0; i < count; i++) {
        if (table[i].value == value) return i;
    }
    return 0;
}

static void FillChoice(wxChoice *choice, FourCharLabel const *table, int count) {
    for (int i = 0; i < count; i++) {
        choice->Append(table[i].label);
    }
}

/* ====================================================================== */
/*  Piano keyboard panel                                                  */
/* ====================================================================== */

class PianoKeyboardPanel final : public wxPanel {
public:
    explicit PianoKeyboardPanel(wxWindow *parent,
                                std::function<void(int)> noteOnCallback,
                                std::function<void()> noteOffCallback)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(600, 60), wxBORDER_SIMPLE),
          m_noteOn(std::move(noteOnCallback)),
          m_noteOff(std::move(noteOffCallback)),
          m_baseNote(48),  /* C3 */
          m_octaves(3),
                    m_pressedKey(-1),
                    m_noteIsOn(false),
                    m_releaseWatchdog(this) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PianoKeyboardPanel::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &PianoKeyboardPanel::OnMouseDown, this);
        Bind(wxEVT_LEFT_UP, &PianoKeyboardPanel::OnMouseUp, this);
        Bind(wxEVT_MOTION, &PianoKeyboardPanel::OnMouseMove, this);
        Bind(wxEVT_LEAVE_WINDOW, &PianoKeyboardPanel::OnMouseLeave, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &PianoKeyboardPanel::OnMouseCaptureLost, this);
        Bind(wxEVT_MOUSE_CAPTURE_CHANGED, &PianoKeyboardPanel::OnMouseCaptureChanged, this);
                Bind(wxEVT_TIMER, &PianoKeyboardPanel::OnReleaseWatchdog, this);
    }

    void ForceStop() {
        StopPressedNote();
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void SetBaseNote(int baseNote) {
        int maxBase = std::max(0, 127 - (m_octaves * 12));
        int clamped = std::clamp(baseNote, 0, maxBase);
        if (clamped != m_baseNote) {
            m_baseNote = clamped;
            Refresh();
        }
    }

private:
    std::function<void(int)> m_noteOn;
    std::function<void()> m_noteOff;
    int m_baseNote;
    int m_octaves;
    int m_pressedKey;
    bool m_noteIsOn;
    wxTimer m_releaseWatchdog;

    static bool IsBlackKey(int noteInOctave) {
        return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
               noteInOctave == 8 || noteInOctave == 10;
    }

    int VisibleEndNote() const {
        return std::min(127, m_baseNote + m_octaves * 12);
    }

    int WhiteIndexForNote(int note) const {
        int whiteIndex = 0;
        int end = std::min(note - 1, VisibleEndNote());
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) {
                ++whiteIndex;
            }
        }
        return whiteIndex;
    }

    bool WhiteKeyRectForNote(int note, wxSize const &size, float whiteKeyWidth, int *x1, int *x2) const {
        int end = VisibleEndNote();
        if (note < m_baseNote || note > end || IsBlackKey(note % 12)) {
            return false;
        }
        int whiteIndex = WhiteIndexForNote(note);
        if (x1) *x1 = (int)((float)whiteIndex * whiteKeyWidth);
        if (x2) *x2 = (int)((float)(whiteIndex + 1) * whiteKeyWidth);
        return true;
    }

    bool BlackKeyRectForNote(int note, wxSize const &size, float whiteKeyWidth, float blackKeyWidth, int *x, int *w) const {
        int end = VisibleEndNote();
        int prevWhite = note - 1;
        int nextWhite = note + 1;
        if (note < m_baseNote || note > end || !IsBlackKey(note % 12)) {
            return false;
        }
        if (prevWhite < m_baseNote || nextWhite > end || IsBlackKey(prevWhite % 12) || IsBlackKey(nextWhite % 12)) {
            return false;
        }
        {
            int prevIdx = WhiteIndexForNote(prevWhite);
            float boundary = (float)(prevIdx + 1) * whiteKeyWidth;
            if (x) *x = (int)(boundary - blackKeyWidth / 2.0f);
            if (w) *w = (int)blackKeyWidth;
        }
        return true;
    }

    int WhiteKeyCount() const {
        int count = 0;
        int end = VisibleEndNote();
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) {
                ++count;
            }
        }
        return std::max(1, count);
    }

    int NoteAtPosition(int x, int y) const {
        wxSize size = GetClientSize();
        if (x < 0 || y < 0 || x >= size.x || y >= size.y) {
            return -1;
        }
        int whiteKeys = WhiteKeyCount();
        float whiteKeyWidth = (float)size.x / (float)whiteKeys;
        float blackKeyWidth = whiteKeyWidth * 0.6f;
        int blackKeyHeight = size.y * 60 / 100;
        int endNote = VisibleEndNote();

        /* Check black keys first (they overlay white keys) */
        if (y < blackKeyHeight) {
            for (int note = m_baseNote; note <= endNote; ++note) {
                int bx;
                int bw;
                if (BlackKeyRectForNote(note, size, whiteKeyWidth, blackKeyWidth, &bx, &bw)) {
                    if (x >= bx && x < (bx + bw)) {
                        return note;
                    }
                }
            }
        }

        /* White keys */
        for (int note = m_baseNote; note <= endNote; ++note) {
            int x1;
            int x2;
            if (WhiteKeyRectForNote(note, size, whiteKeyWidth, &x1, &x2)) {
                if (x >= x1 && x < x2) {
                    return note;
                }
            }
        }

        return -1;
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        int whiteKeys = WhiteKeyCount();
        float whiteKeyWidth = (float)size.x / (float)whiteKeys;
        float blackKeyWidth = whiteKeyWidth * 0.6f;
        int blackKeyHeight = size.y * 60 / 100;
        int endNote = VisibleEndNote();

        dc.SetBackground(wxBrush(wxColour(30, 30, 30)));
        dc.Clear();

        /* Draw white keys */
        for (int note = m_baseNote; note <= endNote; ++note) {
            int x1;
            int x2;
            if (WhiteKeyRectForNote(note, size, whiteKeyWidth, &x1, &x2)) {
                dc.SetBrush(wxBrush(wxColour(240, 240, 235)));
                dc.SetPen(wxPen(wxColour(100, 100, 100), 1));
                dc.DrawRectangle(x1, 0, x2 - x1, size.y);
            }
        }

        /* Draw black keys */
        for (int note = m_baseNote; note <= endNote; ++note) {
            int bx;
            int bw;
            if (BlackKeyRectForNote(note, size, whiteKeyWidth, blackKeyWidth, &bx, &bw)) {
                dc.SetBrush(wxBrush(wxColour(25, 25, 25)));
                dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
                dc.DrawRectangle(bx, 0, bw, blackKeyHeight);
            }
        }

        /* Highlight pressed key */
        if (m_pressedKey >= 0) {
            /* Simple highlight: draw a colored indicator at bottom */
            dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
            dc.SetTextForeground(wxColour(40, 200, 40));
            dc.DrawText(wxString::Format("Note: %d", m_pressedKey), 4, size.y - 14);
        }
    }

    void OnMouseDown(wxMouseEvent &evt) {
        int note = NoteAtPosition(evt.GetX(), evt.GetY());
        if (note < 0) {
            StopPressedNote();
            return;
        }
        if (!HasCapture()) {
            CaptureMouse();
        }
        if (note != m_pressedKey) {
            if (m_noteIsOn && m_noteOff) {
                m_noteOff();
                m_noteIsOn = false;
            }
            m_pressedKey = note;
            if (m_noteOn) {
                m_noteOn(note);
                m_noteIsOn = true;
                if (!m_releaseWatchdog.IsRunning()) {
                    m_releaseWatchdog.Start(25);
                }
            }
            Refresh();
        }
    }

    void OnMouseUp(wxMouseEvent &) {
        StopPressedNote();
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void OnMouseMove(wxMouseEvent &evt) {
        if (!evt.LeftIsDown()) {
            if (m_pressedKey >= 0) {
                StopPressedNote();
            }
            return;
        }
        if (evt.LeftIsDown()) {
            int note = NoteAtPosition(evt.GetX(), evt.GetY());
            if (note < 0) {
                StopPressedNote();
                return;
            }
            if (note != m_pressedKey) {
                if (m_noteIsOn && m_noteOff) {
                    m_noteOff();
                    m_noteIsOn = false;
                }
                m_pressedKey = note;
                if (m_noteOn) {
                    m_noteOn(note);
                    m_noteIsOn = true;
                    if (!m_releaseWatchdog.IsRunning()) {
                        m_releaseWatchdog.Start(25);
                    }
                }
                Refresh();
            }
        }
    }

    void OnMouseLeave(wxMouseEvent &) {
        StopPressedNote();
    }

    void OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
        StopPressedNote();
    }

    void OnMouseCaptureChanged(wxMouseCaptureChangedEvent &) {
        StopPressedNote();
    }

    void OnReleaseWatchdog(wxTimerEvent &) {
        if (m_noteIsOn && !wxGetMouseState().LeftIsDown()) {
            StopPressedNote();
            if (HasCapture()) {
                ReleaseMouse();
            }
        }
    }

    void StopPressedNote() {
        if (m_noteIsOn && m_noteOff) {
            m_noteOff();
        }
        m_noteIsOn = false;
        if (m_releaseWatchdog.IsRunning()) {
            m_releaseWatchdog.Stop();
        }
        if (m_pressedKey >= 0) {
            m_pressedKey = -1;
            Refresh();
        }
    }
};

/* ====================================================================== */
/*  Waveform panel (copied from editor_instrument_dialog.cpp)             */
/* ====================================================================== */

class WaveformPanelExt final : public wxPanel {
public:
    explicit WaveformPanelExt(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 150), wxBORDER_SIMPLE),
          m_waveData(nullptr), m_frameCount(0), m_bitSize(16), m_channels(1),
          m_loopStart(0), m_loopEnd(0) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &WaveformPanelExt::OnPaint, this);
    }

    void SetWaveform(void const *waveData, uint32_t frameCount, uint16_t bitSize, uint16_t channels) {
        m_waveData = waveData; m_frameCount = frameCount; m_bitSize = bitSize; m_channels = channels;
        Refresh();
    }

    void SetLoopPoints(uint32_t loopStart, uint32_t loopEnd) {
        m_loopStart = loopStart; m_loopEnd = loopEnd; Refresh();
    }

private:
    void const *m_waveData;
    uint32_t m_frameCount;
    uint16_t m_bitSize, m_channels;
    uint32_t m_loopStart, m_loopEnd;

    float SampleAt(uint32_t frame) const {
        if (!m_waveData || m_frameCount == 0 || frame >= m_frameCount || m_channels == 0) return 0.0f;
        if (m_bitSize == 8) {
            unsigned char const *pcm = static_cast<unsigned char const *>(m_waveData);
            return (static_cast<float>(pcm[frame * m_channels]) - 128.0f) / 128.0f;
        }
        if (m_bitSize == 16) {
            int16_t const *pcm = static_cast<int16_t const *>(m_waveData);
            return static_cast<float>(pcm[frame * m_channels]) / 32768.0f;
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

        bool hasLoop = (m_loopEnd > m_loopStart && m_frameCount > 0 && size.x > 2);
        if (hasLoop) {
            int xS = (int)((int64_t)m_loopStart * size.x / m_frameCount);
            int xE = (int)((int64_t)m_loopEnd * size.x / m_frameCount);
            if (xE > xS) {
                dc.SetBrush(wxBrush(wxColour(0, 55, 22)));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(xS, 0, xE - xS, size.y);
            }
        }
        if (!m_waveData || m_frameCount == 0 || size.x <= 2 || size.y <= 2) {
            dc.SetTextForeground(wxColour(200, 200, 200));
            dc.DrawText("No waveform data", 10, 10);
            return;
        }
        dc.SetPen(wxPen(wxColour(98, 215, 255), 1));
        uint32_t framesPerPixel = std::max<uint32_t>(1, m_frameCount / (uint32_t)std::max(1, size.x));
        for (int x = 0; x < size.x; ++x) {
            uint32_t start = (uint32_t)x * framesPerPixel;
            uint32_t end = std::min<uint32_t>(m_frameCount, start + framesPerPixel);
            float minV = 1.0f, maxV = -1.0f;
            if (start >= m_frameCount) break;
            if (end <= start) end = std::min<uint32_t>(m_frameCount, start + 1);
            for (uint32_t f = start; f < end; f++) {
                float v = SampleAt(f);
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
            }
            int y1 = centerY - (int)(maxV * (centerY - 4));
            int y2 = centerY - (int)(minV * (centerY - 4));
            dc.DrawLine(x, y1, x, y2);
        }
        if (hasLoop) {
            int xS = (int)((int64_t)m_loopStart * size.x / m_frameCount);
            int xE = (int)((int64_t)m_loopEnd * size.x / m_frameCount);
            dc.SetPen(wxPen(wxColour(0, 220, 80), 2));
            dc.DrawLine(xS, 0, xS, size.y);
            dc.SetPen(wxPen(wxColour(255, 160, 0), 2));
            dc.DrawLine(xE, 0, xE, size.y);
        }
    }
};

/* ====================================================================== */
/*  InstrumentExtEditorDialog                                             */
/* ====================================================================== */

class InstrumentExtEditorDialog final : public wxDialog {
public:
    using EditedSample = InstrumentEditorEditedSample;

    InstrumentExtEditorDialog(wxWindow *parent,
                                                            BAERmfEditorDocument *document,
                              uint32_t primarySampleIndex,
                              BAERmfEditorInstrumentExtInfo const *initialExtInfo,
                              std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char)> playCallback,
                              std::function<void()> stopCallback,
                              std::function<bool(uint32_t, wxString const &)> replaceCallback,
                              std::function<bool(uint32_t, wxString const &)> exportCallback)
        : wxDialog(parent, wxID_ANY, "Edit Instrument", wxDefaultPosition, wxSize(700, 700),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_document(document),
                    m_instID(0),
          m_extInfo(*initialExtInfo),
          m_extInfoModified(false),
          m_currentLocalIndex(-1),
          m_savedLoopStart(0),
          m_savedLoopEnd(0),
          m_playCallback(std::move(playCallback)),
          m_stopCallback(std::move(stopCallback)),
          m_replaceCallback(std::move(replaceCallback)),
          m_exportCallback(std::move(exportCallback)) {

                BAERmfEditorDocument_GetInstIDForSample(m_document, primarySampleIndex, &m_instID);
                if (m_instID != 0) {
                        m_extInfo.instID = m_instID;
                }

        BuildSampleGroup(primarySampleIndex);

        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);

        /* Notebook with two tabs */
        m_notebook = new wxNotebook(this, wxID_ANY);
        BuildInstrumentTab();
        BuildSamplesTab();

        rootSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 5);

        /* Piano keyboard (always visible) */
        m_pianoPanel = new PianoKeyboardPanel(this,
            [this](int note) {
                if (!m_playCallback || m_sampleIndices.empty()) return;
                /* Find the sample whose key range contains the played note */
                int best = -1;
                for (size_t i = 0; i < m_samples.size(); i++) {
                    if (note >= (int)m_samples[i].lowKey && note <= (int)m_samples[i].highKey) {
                        best = (int)i;
                        break;
                    }
                }
                if (best < 0) return;
                m_playCallback(m_sampleIndices[(size_t)best], note,
                               &m_samples[(size_t)best].sampleInfo,
                               m_samples[(size_t)best].splitVolume,
                               m_samples[(size_t)best].rootKey);
            },
            [this]() {
                if (m_stopCallback) m_stopCallback();
            });
        rootSizer->Add(m_pianoPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

        /* Apply / OK / Cancel */
        {
            wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
            btnSizer->AddStretchSpacer();
            m_applyButton = new wxButton(this, wxID_APPLY, "Apply");
            btnSizer->Add(m_applyButton, 0, wxRIGHT, 5);
            btnSizer->Add(new wxButton(this, wxID_OK, "OK"), 0, wxRIGHT, 5);
            btnSizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
            rootSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);
        }

        SetSizerAndFit(rootSizer);

        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &evt) {
            if (m_pianoPanel) {
                m_pianoPanel->ForceStop();
            }
            evt.Skip();
        });

        Bind(wxEVT_BUTTON, &InstrumentExtEditorDialog::OnApply, this, wxID_APPLY);
        Bind(wxEVT_BUTTON, &InstrumentExtEditorDialog::OnOk, this, wxID_OK);

        /* Load initial sample data */
        if (!m_samples.empty()) {
            m_splitChoice->SetSelection(0);
            LoadLocalSample(0);
        }
    }

    BAERmfEditorInstrumentExtInfo const &GetExtInfo() {
        SaveExtInfoFromUI();
        return m_extInfo;
    }

    std::vector<uint32_t> const &GetDeletedSampleIndices() const { return m_deletedSampleIndices; }

    std::vector<uint32_t> const &GetSampleIndices() const { return m_sampleIndices; }

    std::vector<EditedSample> const &GetEditedSamples() {
        SaveCurrentSampleFromUI();
        return m_samples;
    }

private:
    BAERmfEditorDocument *m_document;
    uint32_t m_instID;
    BAERmfEditorInstrumentExtInfo m_extInfo;
    bool m_extInfoModified;
    std::vector<uint32_t> m_deletedSampleIndices;
    std::vector<uint32_t> m_sampleIndices;
    std::vector<EditedSample> m_samples;
    int m_currentLocalIndex;
    uint32_t m_savedLoopStart, m_savedLoopEnd;
    std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char)> m_playCallback;
    std::function<void()> m_stopCallback;
    std::function<bool(uint32_t, wxString const &)> m_replaceCallback;
    std::function<bool(uint32_t, wxString const &)> m_exportCallback;

    wxNotebook *m_notebook;
    PianoKeyboardPanel *m_pianoPanel;
    wxButton *m_applyButton;

    /* Instrument tab controls */
    wxTextCtrl *m_instNameText;
    wxSpinCtrl *m_instProgramSpin;
    wxCheckBox *m_chkInterpolate;
    wxCheckBox *m_chkAvoidReverb;
    wxCheckBox *m_chkSampleAndHold;
    wxCheckBox *m_chkPlayAtSampledFreq;
    wxCheckBox *m_chkNotPolyphonic;
    wxSpinCtrl *m_lpfFrequency;
    wxSpinCtrl *m_lpfResonance;
    wxSpinCtrl *m_lpfLowpassAmount;
    /* ADSR stages */
    wxSpinCtrl *m_adsrStageSpin;
    wxSpinCtrl *m_adsrLevel[BAE_EDITOR_MAX_ADSR_STAGES];
    wxSpinCtrl *m_adsrTime[BAE_EDITOR_MAX_ADSR_STAGES];
    wxChoice *m_adsrFlags[BAE_EDITOR_MAX_ADSR_STAGES];
    wxStaticText *m_adsrRowLabels[BAE_EDITOR_MAX_ADSR_STAGES];
    /* LFO controls */
    wxSpinCtrl *m_lfoCountSpin;
    wxChoice *m_lfoSelector;
    wxChoice *m_lfoDest;
    wxChoice *m_lfoShape;
    wxSpinCtrl *m_lfoPeriod;
    wxSpinCtrl *m_lfoDCFeed;
    wxSpinCtrl *m_lfoLevel;

    /* Samples tab controls */
    wxChoice *m_splitChoice;
    wxTextCtrl *m_nameText;
    wxSpinCtrl *m_rootSpin;
    wxSpinCtrl *m_lowSpin;
    wxSpinCtrl *m_highSpin;
    wxSpinCtrl *m_splitVolumeSpin;
    wxCheckBox *m_loopEnableCheck;
    wxSpinCtrl *m_loopStartSpin;
    wxSpinCtrl *m_loopEndSpin;
    wxStaticText *m_loopInfoLabel;
    wxChoice *m_compressionChoice;
    wxStaticText *m_codecLabel;
    WaveformPanelExt *m_waveformPanel;
    wxButton *m_deleteSampleButton;

    /* ---- Instrument Tab ---- */
    void BuildInstrumentTab() {
        wxPanel *page = new wxPanel(m_notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        /* Name + Program */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(page, wxID_ANY, "Name"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_instNameText = new wxTextCtrl(page, wxID_ANY, "");
            if (m_extInfo.displayName && m_extInfo.displayName[0]) {
                m_instNameText->SetValue(SanitizeDisplayName(wxString::FromUTF8(m_extInfo.displayName)));
            } else if (!m_samples.empty()) {
                m_instNameText->SetValue(m_samples[0].displayName);
            }
            row->Add(m_instNameText, 1, wxRIGHT, 15);
            row->Add(new wxStaticText(page, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_instProgramSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                               wxSP_ARROW_KEYS, 0, 127, m_samples.empty() ? 0 : m_samples[0].program);
            row->Add(m_instProgramSpin, 0);
            sizer->Add(row, 0, wxEXPAND | wxALL, 8);
        }

        /* Flags */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, page, "Flags");
            wxBoxSizer *row1 = new wxBoxSizer(wxHORIZONTAL);
            m_chkInterpolate = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Interpolate");
            m_chkAvoidReverb = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Avoid Reverb");
            m_chkSampleAndHold = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Sample && Hold");
            row1->Add(m_chkInterpolate, 0, wxRIGHT, 15);
            row1->Add(m_chkAvoidReverb, 0, wxRIGHT, 15);
            row1->Add(m_chkSampleAndHold, 0);
            box->Add(row1, 0, wxALL, 4);
            wxBoxSizer *row2 = new wxBoxSizer(wxHORIZONTAL);
            m_chkPlayAtSampledFreq = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Play at Sampled Freq");
            m_chkNotPolyphonic = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Not Polyphonic");
            row2->Add(m_chkPlayAtSampledFreq, 0, wxRIGHT, 15);
            row2->Add(m_chkNotPolyphonic, 0);
            box->Add(row2, 0, wxALL, 4);
            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

            /* Populate from ext info */
            m_chkInterpolate->SetValue((m_extInfo.flags1 & 0x80) != 0);
            m_chkAvoidReverb->SetValue((m_extInfo.flags1 & 0x01) != 0);
            m_chkSampleAndHold->SetValue((m_extInfo.flags1 & 0x04) != 0);
            m_chkPlayAtSampledFreq->SetValue((m_extInfo.flags2 & 0x40) != 0);
            m_chkNotPolyphonic->SetValue((m_extInfo.flags2 & 0x04) != 0);
        }

        /* LPF */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxHORIZONTAL, page, "Low-Pass Filter");
            box->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Frequency"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfFrequency = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, -100000, 100000, m_extInfo.LPF_frequency);
            box->Add(m_lpfFrequency, 0, wxRIGHT, 10);
            box->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Resonance"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfResonance = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, -100000, 100000, m_extInfo.LPF_resonance);
            box->Add(m_lpfResonance, 0, wxRIGHT, 10);
            box->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Amount"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfLowpassAmount = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                                wxSP_ARROW_KEYS, -100000, 100000, m_extInfo.LPF_lowpassAmount);
            box->Add(m_lpfLowpassAmount, 0);
            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        }

        /* Volume ADSR */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, page, "Volume ADSR");
            wxBoxSizer *countRow = new wxBoxSizer(wxHORIZONTAL);
            countRow->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Stages:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_adsrStageSpin = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                             wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_ADSR_STAGES, (int)m_extInfo.volumeADSR.stageCount);
            countRow->Add(m_adsrStageSpin, 0);
            box->Add(countRow, 0, wxALL, 4);

            /* Header */
            wxFlexGridSizer *grid = new wxFlexGridSizer(0, 4, 2, 6);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "#"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Time"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Type"), 0, wxALIGN_CENTER);

            for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
                m_adsrRowLabels[i] = new wxStaticText(box->GetStaticBox(), wxID_ANY, wxString::Format("%d", i));
                grid->Add(m_adsrRowLabels[i], 0, wxALIGN_CENTER_VERTICAL);
                int lvl = (i < (int)m_extInfo.volumeADSR.stageCount) ? m_extInfo.volumeADSR.stages[i].level : 0;
                int tm = (i < (int)m_extInfo.volumeADSR.stageCount) ? m_extInfo.volumeADSR.stages[i].time : 0;
                int fl = (i < (int)m_extInfo.volumeADSR.stageCount) ? m_extInfo.volumeADSR.stages[i].flags : 0;
                m_adsrLevel[i] = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                                wxSP_ARROW_KEYS, -1000000, 1000000, lvl);
                grid->Add(m_adsrLevel[i], 0);
                m_adsrTime[i] = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                               wxSP_ARROW_KEYS, -1000000, 1000000, tm);
                grid->Add(m_adsrTime[i], 0);
                m_adsrFlags[i] = new wxChoice(box->GetStaticBox(), wxID_ANY);
                FillChoice(m_adsrFlags[i], kADSRFlagLabels, kADSRFlagCount);
                m_adsrFlags[i]->SetSelection(FindLabelIndex(kADSRFlagLabels, kADSRFlagCount, fl));
                grid->Add(m_adsrFlags[i], 0);

                bool visible = (i < (int)m_extInfo.volumeADSR.stageCount);
                m_adsrRowLabels[i]->Show(visible);
                m_adsrLevel[i]->Show(visible);
                m_adsrTime[i]->Show(visible);
                m_adsrFlags[i]->Show(visible);
            }
            box->Add(grid, 0, wxALL, 4);
            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

            m_adsrStageSpin->Bind(wxEVT_SPINCTRL, [this, box](wxCommandEvent &) {
                int count = m_adsrStageSpin->GetValue();
                for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
                    bool vis = (i < count);
                    m_adsrRowLabels[i]->Show(vis);
                    m_adsrLevel[i]->Show(vis);
                    m_adsrTime[i]->Show(vis);
                    m_adsrFlags[i]->Show(vis);
                }
                box->GetStaticBox()->GetParent()->Layout();
            });
        }

        /* LFO section */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, page, "LFOs");
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Count:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_lfoCountSpin = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                            wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_LFOS, (int)m_extInfo.lfoCount);
            row->Add(m_lfoCountSpin, 0, wxRIGHT, 15);
            row->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Edit LFO:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_lfoSelector = new wxChoice(box->GetStaticBox(), wxID_ANY);
            for (int i = 0; i < BAE_EDITOR_MAX_LFOS; i++) m_lfoSelector->Append(wxString::Format("LFO %d", i));
            if (m_extInfo.lfoCount > 0) m_lfoSelector->SetSelection(0);
            row->Add(m_lfoSelector, 0);
            box->Add(row, 0, wxALL, 4);

            wxFlexGridSizer *grid = new wxFlexGridSizer(2, 4, 6);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Dest"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoDest = new wxChoice(box->GetStaticBox(), wxID_ANY);
            FillChoice(m_lfoDest, kLFODestLabels, kLFODestCount);
            grid->Add(m_lfoDest, 0);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoShape = new wxChoice(box->GetStaticBox(), wxID_ANY);
            FillChoice(m_lfoShape, kLFOShapeLabels, kLFOShapeCount);
            grid->Add(m_lfoShape, 0);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Period"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoPeriod = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                         wxSP_ARROW_KEYS, -1000000, 10000000, 0);
            grid->Add(m_lfoPeriod, 0);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "DC Feed"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoDCFeed = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                         wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(m_lfoDCFeed, 0);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoLevel = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                        wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(m_lfoLevel, 0);
            box->Add(grid, 0, wxALL, 4);
            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

            if (m_extInfo.lfoCount > 0) LoadLFOIntoUI(0);

            m_lfoSelector->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
                int sel = m_lfoSelector->GetSelection();
                if (sel >= 0) LoadLFOIntoUI(sel);
            });
        }

        page->SetSizerAndFit(sizer);
        m_notebook->AddPage(page, "Instrument");
    }

    void LoadLFOIntoUI(int idx) {
        if (idx < 0 || idx >= (int)m_extInfo.lfoCount) {
            m_lfoDest->SetSelection(0);
            m_lfoShape->SetSelection(0);
            m_lfoPeriod->SetValue(0);
            m_lfoDCFeed->SetValue(0);
            m_lfoLevel->SetValue(0);
            return;
        }
        BAERmfEditorLFOInfo const &lfo = m_extInfo.lfos[idx];
        m_lfoDest->SetSelection(FindLabelIndex(kLFODestLabels, kLFODestCount, lfo.destination));
        m_lfoShape->SetSelection(FindLabelIndex(kLFOShapeLabels, kLFOShapeCount, lfo.waveShape));
        m_lfoPeriod->SetValue(lfo.period);
        m_lfoDCFeed->SetValue(lfo.DC_feed);
        m_lfoLevel->SetValue(lfo.level);
    }

    void SaveLFOFromUI(int idx) {
        if (idx < 0 || idx >= BAE_EDITOR_MAX_LFOS) return;
        BAERmfEditorLFOInfo &lfo = m_extInfo.lfos[idx];
        int destSel = m_lfoDest->GetSelection();
        if (destSel >= 0 && destSel < kLFODestCount) lfo.destination = kLFODestLabels[destSel].value;
        int shapeSel = m_lfoShape->GetSelection();
        if (shapeSel >= 0 && shapeSel < kLFOShapeCount) lfo.waveShape = kLFOShapeLabels[shapeSel].value;
        lfo.period = m_lfoPeriod->GetValue();
        lfo.DC_feed = m_lfoDCFeed->GetValue();
        lfo.level = m_lfoLevel->GetValue();
    }

    wxString GetInstrumentDisplayName() const {
        return SanitizeDisplayName(m_instNameText->GetValue());
    }

    void SaveExtInfoFromUI() {
        if (m_instID != 0) {
            m_extInfo.instID = m_instID;
        }

        /* Flags */
        m_extInfo.flags1 &= ~(0x80 | 0x04 | 0x01);  /* clear bits we manage */
        if (m_chkInterpolate->GetValue()) m_extInfo.flags1 |= 0x80;
        if (m_chkAvoidReverb->GetValue()) m_extInfo.flags1 |= 0x01;
        if (m_chkSampleAndHold->GetValue()) m_extInfo.flags1 |= 0x04;
        m_extInfo.flags2 &= ~(0x40 | 0x04);
        if (m_chkPlayAtSampledFreq->GetValue()) m_extInfo.flags2 |= 0x40;
        if (m_chkNotPolyphonic->GetValue()) m_extInfo.flags2 |= 0x04;

        /* LPF */
        m_extInfo.LPF_frequency = m_lpfFrequency->GetValue();
        m_extInfo.LPF_resonance = m_lpfResonance->GetValue();
        m_extInfo.LPF_lowpassAmount = m_lpfLowpassAmount->GetValue();

        /* ADSR */
        m_extInfo.volumeADSR.stageCount = (uint32_t)m_adsrStageSpin->GetValue();
        for (int i = 0; i < (int)m_extInfo.volumeADSR.stageCount && i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            m_extInfo.volumeADSR.stages[i].level = m_adsrLevel[i]->GetValue();
            m_extInfo.volumeADSR.stages[i].time = m_adsrTime[i]->GetValue();
            int sel = m_adsrFlags[i]->GetSelection();
            m_extInfo.volumeADSR.stages[i].flags = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
        }

        /* LFOs - save current one being edited */
        int lfoSel = m_lfoSelector->GetSelection();
        if (lfoSel >= 0) SaveLFOFromUI(lfoSel);
        m_extInfo.lfoCount = (uint32_t)m_lfoCountSpin->GetValue();

        /* Mark as having extended data if anything non-default is set */
        m_extInfo.hasExtendedData = TRUE;
        m_extInfoModified = true;
    }

    bool ApplyInstrumentChanges(bool showError) {
        BAERmfEditorInstrumentExtInfo infoToWrite;
        BAEResult result;
        wxString instName;

        SaveExtInfoFromUI();
        if (!m_document) {
            if (showError) {
                wxMessageBox("Failed to access instrument document.",
                             "Edit Instrument", wxOK | wxICON_ERROR, this);
            }
            return false;
        }
        infoToWrite = m_extInfo;
        instName = GetInstrumentDisplayName();
        wxScopedCharBuffer instNameUtf8 = instName.utf8_str();
        infoToWrite.displayName = instNameUtf8.data();
        result = BAERmfEditorDocument_SetInstrumentExtInfo(m_document, infoToWrite.instID, &infoToWrite);
        if (result == BAE_NO_ERROR) {
            return true;
        }
        if (showError) {
            wxMessageBox(wxString::Format("Failed to apply instrument extended data (error %d).",
                                          static_cast<int>(result)),
                         "Edit Instrument", wxOK | wxICON_ERROR, this);
        }
        return false;
    }

    /* ---- Samples Tab ---- */
    void BuildSamplesTab() {
        wxPanel *page = new wxPanel(m_notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        /* Sample selector */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(page, wxID_ANY, "Sample"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_splitChoice = new wxChoice(page, wxID_ANY);
            for (size_t i = 0; i < m_samples.size(); i++) {
                m_splitChoice->Append(BuildSplitLabel((int)i));
            }
            row->Add(m_splitChoice, 1, wxEXPAND);
            wxButton *newSampleBtn = new wxButton(page, wxID_ANY, "New Sample");
            row->Add(newSampleBtn, 0, wxLEFT, 8);
            newSampleBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnNewSample(); });
            sizer->Add(row, 0, wxEXPAND | wxALL, 8);
        }

        /* Sample properties grid */
        wxFlexGridSizer *grid = new wxFlexGridSizer(2, 4, 6);
        grid->Add(new wxStaticText(page, wxID_ANY, "Title"), 0, wxALIGN_CENTER_VERTICAL);
        m_nameText = new wxTextCtrl(page, wxID_ANY, "");
        grid->Add(m_nameText, 1, wxEXPAND);
        grid->Add(new wxStaticText(page, wxID_ANY, "Root Key"), 0, wxALIGN_CENTER_VERTICAL);
        m_rootSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 60);
        grid->Add(m_rootSpin, 0);
        grid->Add(new wxStaticText(page, wxID_ANY, "Low Key"), 0, wxALIGN_CENTER_VERTICAL);
        m_lowSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        grid->Add(m_lowSpin, 0);
        grid->Add(new wxStaticText(page, wxID_ANY, "High Key"), 0, wxALIGN_CENTER_VERTICAL);
        m_highSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 127);
        grid->Add(m_highSpin, 0);
        grid->Add(new wxStaticText(page, wxID_ANY, "Split Vol (0=default)"), 0, wxALIGN_CENTER_VERTICAL);
        m_splitVolumeSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 32767, 100);
        grid->Add(m_splitVolumeSpin, 0);
        grid->AddGrowableCol(1, 1);
        sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

        /* Loop controls */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            m_loopEnableCheck = new wxCheckBox(page, wxID_ANY, "Loop");
            row->Add(m_loopEnableCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
            row->Add(new wxStaticText(page, wxID_ANY, "Start"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_loopStartSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, INT_MAX, 0);
            row->Add(m_loopStartSpin, 0, wxRIGHT, 10);
            row->Add(new wxStaticText(page, wxID_ANY, "End"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_loopEndSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, INT_MAX, 0);
            row->Add(m_loopEndSpin, 0, wxRIGHT, 10);
            m_loopInfoLabel = new wxStaticText(page, wxID_ANY, "");
            row->Add(m_loopInfoLabel, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(row, 0, wxALL, 8);
        }

        /* Compression */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(page, wxID_ANY, "Compression"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_compressionChoice = new wxChoice(page, wxID_ANY);
            m_compressionChoice->Append("Don't Change");
            m_compressionChoice->Append("RAW PCM");
            m_compressionChoice->Append("ADPCM");
            m_compressionChoice->Append("MP3 32k");
            m_compressionChoice->Append("MP3 64k");
            m_compressionChoice->Append("MP3 96k");
            m_compressionChoice->Append("VORBIS 32k");
            m_compressionChoice->Append("VORBIS 64k");
            m_compressionChoice->Append("VORBIS 96k");
            m_compressionChoice->Append("FLAC (level 9)");
            m_compressionChoice->Append("OPUS 16k");
            m_compressionChoice->Append("OPUS 32k");
            m_compressionChoice->Append("OPUS 64k");
            m_compressionChoice->Append("OPUS 96k");
            m_compressionChoice->Append("OPUS 128k");
            m_compressionChoice->Append("OPUS 256k");
            row->Add(m_compressionChoice, 0);
            m_codecLabel = new wxStaticText(page, wxID_ANY, "");
            row->Add(m_codecLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
            sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
        }

        /* Action buttons */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            wxButton *replaceBtn = new wxButton(page, wxID_ANY, "Replace");
            wxButton *exportBtn = new wxButton(page, wxID_ANY, "Save As...");
            m_deleteSampleButton = new wxButton(page, wxID_ANY, "Delete Sample");
            row->Add(replaceBtn, 0, wxRIGHT, 8);
            row->Add(exportBtn, 0);
            row->AddStretchSpacer();
            row->Add(m_deleteSampleButton, 0, wxLEFT, 8);
            sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            replaceBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnReplaceSample(); });
            exportBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnExportSample(); });
            m_deleteSampleButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnDeleteSample(); });
        }

        /* Waveform */
        sizer->Add(new wxStaticText(page, wxID_ANY, "Waveform"), 0, wxLEFT | wxRIGHT, 8);
        m_waveformPanel = new WaveformPanelExt(page);
        sizer->Add(m_waveformPanel, 1, wxEXPAND | wxALL, 8);

        page->SetSizerAndFit(sizer);
        m_notebook->AddPage(page, "Samples");

        /* Bind events */
        m_splitChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
            SaveCurrentSampleFromUI();
            int sel = m_splitChoice->GetSelection();
            if (sel >= 0) LoadLocalSample(sel);
        });
        m_loopEnableCheck->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { OnLoopEnableChanged(); });
        m_loopStartSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_loopEndSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_lowSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { ClampRange(); });
        m_highSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { ClampRange(); });
        UpdateSampleButtonState();
    }

    void OnNewSample() {
        SaveCurrentSampleFromUI();

        unsigned char prog = m_samples.empty() ? 0 : m_samples[0].program;
        if (m_instProgramSpin) prog = (unsigned char)m_instProgramSpin->GetValue();

        long root = wxGetNumberFromUser("Root key (0-127)", "Root Key", "New Sample", 60, 0, 127, this);
        if (root < 0) return;

        EditedSample s;
        s.displayName = "New Sample";
        s.sourcePath = wxString();
        s.program = prog;
        s.rootKey = (unsigned char)root;
        s.lowKey = 0;
        s.highKey = 127;
        s.splitVolume = 0;
        memset(&s.sampleInfo, 0, sizeof(s.sampleInfo));
        s.compressionType = BAE_EDITOR_COMPRESSION_PCM;
        s.hasOriginalData = false;

        m_samples.push_back(s);
        m_sampleIndices.push_back(static_cast<uint32_t>(-1));

        int newIdx = (int)m_samples.size() - 1;
        m_splitChoice->Append(BuildSplitLabel(newIdx));
        m_splitChoice->SetSelection(newIdx);
        LoadLocalSample(newIdx);
        UpdateSampleButtonState();
    }

    void OnDeleteSample() {
        int localIndex;
        uint32_t sampleIndex;
        wxString confirmMessage;

        if (m_samples.size() <= 1) {
            wxMessageBox("Cannot delete the last sample from this editor. Use Delete Instrument instead.",
                         "Delete Sample",
                         wxOK | wxICON_INFORMATION,
                         this);
            return;
        }
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) {
            return;
        }

        SaveCurrentSampleFromUI();
        localIndex = m_currentLocalIndex;
        sampleIndex = m_sampleIndices[(size_t)localIndex];
        confirmMessage = wxString::Format("Delete sample '%s' (%d-%d)?",
                                          m_samples[(size_t)localIndex].displayName,
                                          (int)m_samples[(size_t)localIndex].lowKey,
                                          (int)m_samples[(size_t)localIndex].highKey);
        if (wxMessageBox(confirmMessage, "Delete Sample", wxYES_NO | wxICON_WARNING, this) != wxYES) {
            return;
        }

        if (sampleIndex != static_cast<uint32_t>(-1)) {
            m_deletedSampleIndices.push_back(sampleIndex);
        }
        m_samples.erase(m_samples.begin() + localIndex);
        m_sampleIndices.erase(m_sampleIndices.begin() + localIndex);
        m_splitChoice->Delete(localIndex);

        if (localIndex >= (int)m_samples.size()) {
            localIndex = (int)m_samples.size() - 1;
        }
        m_splitChoice->SetSelection(localIndex);
        LoadLocalSample(localIndex);
        UpdateSampleButtonState();
    }

    /* ---- Sample data management (mirrors existing dialog logic) ---- */

    static wxString SanitizeDisplayName(wxString const &name) {
        wxString clean;
        for (wxUniChar ch : name) {
            if (ch >= 32 && ch != 127) clean.Append(ch);
        }
        return clean.IsEmpty() ? wxString("Embedded Sample") : clean;
    }

    void UpdateSampleButtonState() {
        if (m_deleteSampleButton) {
            m_deleteSampleButton->Enable(m_samples.size() > 1 &&
                                         m_currentLocalIndex >= 0 &&
                                         m_currentLocalIndex < (int)m_samples.size());
        }
    }

    void BuildSampleGroup(uint32_t primarySampleIndex) {
        uint32_t sampleCount = 0;
        BAERmfEditorSampleInfo primaryInfo;
        if (!m_document ||
            BAERmfEditorDocument_GetSampleInfo(m_document, primarySampleIndex, &primaryInfo) != BAE_NO_ERROR ||
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) != BAE_NO_ERROR) return;

        for (uint32_t i = 0; i < sampleCount; i++) {
            BAERmfEditorSampleInfo info;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR) continue;
            if (info.program == primaryInfo.program) {
                EditedSample edited;
                edited.displayName = SanitizeDisplayName(info.displayName ? wxString::FromUTF8(info.displayName) : wxString());
                edited.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
                edited.program = info.program;
                edited.rootKey = info.rootKey;
                edited.lowKey = info.lowKey;
                edited.highKey = info.highKey;
                edited.splitVolume = info.splitVolume;
                edited.sampleInfo = info.sampleInfo;
                edited.compressionType = info.compressionType;
                edited.hasOriginalData = (info.hasOriginalData == TRUE);
                m_sampleIndices.push_back(i);
                m_samples.push_back(edited);
            }
        }
    }

    wxString BuildSplitLabel(int localIndex) const {
        EditedSample const &s = m_samples[(size_t)localIndex];
        return wxString::Format("%d-%d: %s", (int)s.lowKey, (int)s.highKey, s.displayName);
    }

    static int CompressionTypeToChoiceIndex(BAERmfEditorCompressionType t) {
        switch (t) {
            case BAE_EDITOR_COMPRESSION_DONT_CHANGE: return 0;
            case BAE_EDITOR_COMPRESSION_PCM:         return 1;
            case BAE_EDITOR_COMPRESSION_ADPCM:       return 2;
            case BAE_EDITOR_COMPRESSION_MP3_32K:     return 3;
            case BAE_EDITOR_COMPRESSION_MP3_64K:     return 4;
            case BAE_EDITOR_COMPRESSION_MP3_96K:     return 5;
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:  return 6;
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:  return 7;
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:  return 8;
            case BAE_EDITOR_COMPRESSION_FLAC:        return 9;
            case BAE_EDITOR_COMPRESSION_OPUS_16K:    return 10;
            case BAE_EDITOR_COMPRESSION_OPUS_32K:    return 11;
            case BAE_EDITOR_COMPRESSION_OPUS_64K:    return 12;
            case BAE_EDITOR_COMPRESSION_OPUS_96K:    return 13;
            case BAE_EDITOR_COMPRESSION_OPUS_128K:   return 14;
            case BAE_EDITOR_COMPRESSION_OPUS_256K:   return 15;
            default:                                  return 1;
        }
    }

    static BAERmfEditorCompressionType ChoiceIndexToCompressionType(int idx) {
        switch (idx) {
            case 0:  return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
            case 1:  return BAE_EDITOR_COMPRESSION_PCM;
            case 2:  return BAE_EDITOR_COMPRESSION_ADPCM;
            case 3:  return BAE_EDITOR_COMPRESSION_MP3_32K;
            case 4:  return BAE_EDITOR_COMPRESSION_MP3_64K;
            case 5:  return BAE_EDITOR_COMPRESSION_MP3_96K;
            case 6:  return BAE_EDITOR_COMPRESSION_VORBIS_32K;
            case 7:  return BAE_EDITOR_COMPRESSION_VORBIS_64K;
            case 8:  return BAE_EDITOR_COMPRESSION_VORBIS_96K;
            case 9:  return BAE_EDITOR_COMPRESSION_FLAC;
            case 10: return BAE_EDITOR_COMPRESSION_OPUS_16K;
            case 11: return BAE_EDITOR_COMPRESSION_OPUS_32K;
            case 12: return BAE_EDITOR_COMPRESSION_OPUS_64K;
            case 13: return BAE_EDITOR_COMPRESSION_OPUS_96K;
            case 14: return BAE_EDITOR_COMPRESSION_OPUS_128K;
            case 15: return BAE_EDITOR_COMPRESSION_OPUS_256K;
            default: return BAE_EDITOR_COMPRESSION_PCM;
        }
    }

    void SaveCurrentSampleFromUI() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) return;
        EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
        s.displayName = SanitizeDisplayName(m_nameText->GetValue());
        s.rootKey = (unsigned char)m_rootSpin->GetValue();
        s.lowKey = (unsigned char)m_lowSpin->GetValue();
        s.highKey = (unsigned char)m_highSpin->GetValue();
        s.splitVolume = (int16_t)m_splitVolumeSpin->GetValue();
        /* Program comes from the instrument tab */
        s.program = (unsigned char)m_instProgramSpin->GetValue();
        int idx = m_compressionChoice->GetSelection();
        BAERmfEditorCompressionType chosen = ChoiceIndexToCompressionType(idx);
        if (chosen == BAE_EDITOR_COMPRESSION_DONT_CHANGE && !s.hasOriginalData) chosen = BAE_EDITOR_COMPRESSION_PCM;
        s.compressionType = chosen;
        if (m_splitChoice && m_currentLocalIndex < (int)m_splitChoice->GetCount()) {
            m_splitChoice->SetString(m_currentLocalIndex, BuildSplitLabel(m_currentLocalIndex));
        }
    }

    void LoadLocalSample(int localIndex) {
        if (localIndex < 0 || localIndex >= (int)m_samples.size()) return;
        m_currentLocalIndex = localIndex;
        EditedSample const &s = m_samples[(size_t)localIndex];
        if (m_pianoPanel) {
            m_pianoPanel->SetBaseNote(std::max(0, (int)s.rootKey - 12));
        }
        m_nameText->SetValue(s.displayName);
        m_rootSpin->SetValue(s.rootKey);
        m_lowSpin->SetValue(s.lowKey);
        m_highSpin->SetValue(s.highKey);
        m_splitVolumeSpin->SetValue(std::clamp<int>(s.splitVolume, 0, 32767));
        {
            bool canKeep = s.hasOriginalData;
            BAERmfEditorCompressionType effectiveComp = s.compressionType;
            if (!canKeep && effectiveComp == BAE_EDITOR_COMPRESSION_DONT_CHANGE) effectiveComp = BAE_EDITOR_COMPRESSION_PCM;
            m_compressionChoice->SetSelection(CompressionTypeToChoiceIndex(effectiveComp));
            m_compressionChoice->SetString(0, canKeep ? "Don't Change" : "Don't Change (N/A)");
        }
        {
            char codecBuf[64] = {};
            uint32_t si = m_sampleIndices[(size_t)localIndex];
            if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, si, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
                m_codecLabel->SetLabel(wxString::Format("(source: %s)", wxString::FromUTF8(codecBuf)));
            } else {
                m_codecLabel->SetLabel(wxString());
            }
        }
        {
            bool loopEnabled = (s.sampleInfo.startLoop < s.sampleInfo.endLoop);
            int maxFrame = (int)(s.sampleInfo.waveFrames > 0 ? s.sampleInfo.waveFrames : INT_MAX);
            m_loopEnableCheck->SetValue(loopEnabled);
            m_loopStartSpin->SetRange(0, maxFrame);
            m_loopEndSpin->SetRange(0, maxFrame);
            m_loopStartSpin->SetValue((int)s.sampleInfo.startLoop);
            m_loopEndSpin->SetValue((int)s.sampleInfo.endLoop);
            m_loopStartSpin->Enable(loopEnabled);
            m_loopEndSpin->Enable(loopEnabled);
            m_savedLoopStart = s.sampleInfo.startLoop;
            m_savedLoopEnd = s.sampleInfo.endLoop;
            UpdateLoopInfoLabel(s);
        }
        RefreshWaveform();
        UpdateSampleButtonState();
    }

    void ClampRange() {
        if (m_lowSpin->GetValue() > m_highSpin->GetValue()) m_highSpin->SetValue(m_lowSpin->GetValue());
    }

    void OnLoopEnableChanged() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) return;
        EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
        bool en = m_loopEnableCheck->GetValue();
        m_loopStartSpin->Enable(en);
        m_loopEndSpin->Enable(en);
        if (!en) {
            m_savedLoopStart = (uint32_t)m_loopStartSpin->GetValue();
            m_savedLoopEnd = (uint32_t)m_loopEndSpin->GetValue();
            s.sampleInfo.startLoop = 0;
            s.sampleInfo.endLoop = 0;
            m_waveformPanel->SetLoopPoints(0, 0);
            m_loopInfoLabel->SetLabel("");
        } else {
            uint32_t rs = m_savedLoopStart, re = m_savedLoopEnd;
            if (rs == 0 && re == 0) re = s.sampleInfo.waveFrames;
            m_loopStartSpin->SetValue((int)rs);
            m_loopEndSpin->SetValue((int)re);
            ApplyLoopFromUI();
        }
    }

    void OnLoopPointChanged() {
        if (m_loopEnableCheck->GetValue()) ApplyLoopFromUI();
    }

    void ApplyLoopFromUI() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) return;
        EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
        uint32_t start = (uint32_t)std::max(0, m_loopStartSpin->GetValue());
        uint32_t end = (uint32_t)std::max(0, m_loopEndSpin->GetValue());
        if (s.sampleInfo.waveFrames > 0 && end > s.sampleInfo.waveFrames) end = s.sampleInfo.waveFrames;
        if (start >= end && end > 0) start = end - 1;
        s.sampleInfo.startLoop = start;
        s.sampleInfo.endLoop = end;
        m_waveformPanel->SetLoopPoints(start, end);
        UpdateLoopInfoLabel(s);
    }

    void UpdateLoopInfoLabel(EditedSample const &s) {
        if (s.sampleInfo.startLoop < s.sampleInfo.endLoop) {
            uint32_t frames = s.sampleInfo.endLoop - s.sampleInfo.startLoop;
            uint32_t rateHz = (s.sampleInfo.sampledRate >> 16);
            if (rateHz > 0) {
                double ms = frames * 1000.0 / rateHz;
                m_loopInfoLabel->SetLabel(wxString::Format("%u frames (%.0f ms)", frames, ms));
            } else {
                m_loopInfoLabel->SetLabel(wxString::Format("start: %u  end: %u", s.sampleInfo.startLoop, s.sampleInfo.endLoop));
            }
        } else {
            m_loopInfoLabel->SetLabel("");
        }
    }

    void RefreshWaveform() {
        void const *waveData = nullptr;
        uint32_t frameCount = 0;
        uint16_t bitSize = 16, channels = 1;
        BAE_UNSIGNED_FIXED sampleRate = 0;
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_sampleIndices.size()) return;
        uint32_t si = m_sampleIndices[(size_t)m_currentLocalIndex];
        if (BAERmfEditorDocument_GetSampleWaveformData(m_document, si, &waveData, &frameCount, &bitSize, &channels, &sampleRate) == BAE_NO_ERROR) {
            m_waveformPanel->SetWaveform(waveData, frameCount, bitSize, channels);
        }
        if (m_currentLocalIndex >= 0 && m_currentLocalIndex < (int)m_samples.size()) {
            EditedSample const &s = m_samples[(size_t)m_currentLocalIndex];
            m_waveformPanel->SetLoopPoints(s.sampleInfo.startLoop, s.sampleInfo.endLoop);
        }
    }

    void OnReplaceSample() {
        if (!m_replaceCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_sampleIndices.size()) return;
        wxFileDialog dialog(this, "Replace Embedded Sample", wxEmptyString, wxEmptyString,
            "Supported audio (*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus)|*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        uint32_t si = m_sampleIndices[(size_t)m_currentLocalIndex];
        if (!m_replaceCallback(si, dialog.GetPath())) return;
        BAERmfEditorSampleInfo info;
        if (BAERmfEditorDocument_GetSampleInfo(m_document, si, &info) == BAE_NO_ERROR) {
            EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
            s.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
            s.sampleInfo = info.sampleInfo;
            s.compressionType = info.compressionType;
            s.hasOriginalData = (info.hasOriginalData == TRUE);
        }
        LoadLocalSample(m_currentLocalIndex);
    }

    void OnExportSample() {
        if (!m_exportCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_sampleIndices.size()) return;
        uint32_t si = m_sampleIndices[(size_t)m_currentLocalIndex];
        wxString codec;
        char codecBuf[64];
        wxString suggestedName;
        wxString defaultExt;
        wxString filter;

        codecBuf[0] = 0;
        if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, si, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
            codec = wxString::FromUTF8(codecBuf);
        }

        {
            wxString codecLower = codec.Lower();
            if (codecLower.Contains("flac")) {
                defaultExt = "flac";
                filter = "FLAC files (*.flac)|*.flac|All files (*.*)|*.*";
            } else if (codecLower.Contains("opus")) {
                defaultExt = "opus";
                filter = "Ogg Opus files (*.opus)|*.opus|All files (*.*)|*.*";
            } else if (codecLower.Contains("vorbis")) {
                defaultExt = "ogg";
                filter = "Ogg Vorbis files (*.ogg)|*.ogg|All files (*.*)|*.*";
            } else if (codecLower.Contains("mpeg")) {
                defaultExt = "mp3";
                filter = "MP3 files (*.mp3)|*.mp3|All files (*.*)|*.*";
            } else if (codecLower.Contains("ima")) {
                defaultExt = "aif";
                filter = "AIFF files (*.aif)|*.aif|WAV files (*.wav)|*.wav|All files (*.*)|*.*";
            } else {
                defaultExt = "wav";
                filter = "WAV files (*.wav)|*.wav|AIFF files (*.aif)|*.aif|All files (*.*)|*.*";
            }
        }

        suggestedName = SanitizeDisplayName(m_nameText->GetValue()) + "." + defaultExt;
        wxFileDialog dialog(this, "Save Embedded Sample", wxEmptyString, suggestedName,
            filter, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return;
        if (!m_exportCallback(si, dialog.GetPath())) {
            wxMessageBox("Failed to save sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
        }
    }

    void OnApply(wxCommandEvent &) {
        if (!ApplyInstrumentChanges(true)) {
            return;
        }
        SaveCurrentSampleFromUI();
        /* Update program for all samples in the group */
        unsigned char prog = (unsigned char)m_instProgramSpin->GetValue();
        for (auto &s : m_samples) s.program = prog;
    }

    void OnOk(wxCommandEvent &evt) {
        SaveCurrentSampleFromUI();
        unsigned char prog = (unsigned char)m_instProgramSpin->GetValue();
        for (auto &s : m_samples) s.program = prog;
        if (!ApplyInstrumentChanges(true)) {
            return;
        }
        evt.Skip();
    }
};

}  // namespace

bool ShowInstrumentExtEditorDialog(
    wxWindow *parent,
    BAERmfEditorDocument *document,
    uint32_t primarySampleIndex,
    BAERmfEditorInstrumentExtInfo const *initialExtInfo,
    std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char)> playCallback,
    std::function<void()> stopCallback,
    std::function<bool(uint32_t, wxString const &)> replaceCallback,
    std::function<bool(uint32_t, wxString const &)> exportCallback,
    std::vector<uint32_t> *outDeletedSampleIndices,
    std::vector<uint32_t> *outSampleIndices,
    std::vector<InstrumentEditorEditedSample> *outEditedSamples) {

    InstrumentExtEditorDialog dialog(parent, document, primarySampleIndex,
                                     initialExtInfo,
                                     std::move(playCallback),
                                     std::move(stopCallback),
                                     std::move(replaceCallback),
                                     std::move(exportCallback));

    int result = dialog.ShowModal();
    if (result != wxID_OK && result != wxID_APPLY) {
        return false;
    }

    if (outDeletedSampleIndices) *outDeletedSampleIndices = dialog.GetDeletedSampleIndices();
    if (outSampleIndices) *outSampleIndices = dialog.GetSampleIndices();
    if (outEditedSamples) *outEditedSamples = dialog.GetEditedSamples();
    return true;
}
