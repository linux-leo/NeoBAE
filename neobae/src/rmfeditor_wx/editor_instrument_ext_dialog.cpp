#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <exception>
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

static unsigned char NormalizeRootKeyForSingleKeySplit(unsigned char rootKey,
                                                        unsigned char lowKey,
                                                        unsigned char highKey) {
    if (rootKey == 0 && lowKey <= 127 && highKey <= 127 && lowKey == highKey) {
        return lowKey;
    }
    return rootKey;
}

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
                    m_octaves(4),
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
        Bind(wxEVT_SIZE, &PianoKeyboardPanel::OnSize, this);
        UpdateVisibleRangeForWidth(GetClientSize().x);
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

    void SetExternalPressedNote(int note) {
        int clamped = std::clamp(note, 0, 127);
        if (m_externalPressedKey != clamped) {
            m_externalPressedKey = clamped;
            Refresh();
        }
    }

    void ClearExternalPressedNote() {
        if (m_externalPressedKey >= 0) {
            m_externalPressedKey = -1;
            Refresh();
        }
    }

    void UpdateVisibleRangeForWidth(int width) {
        int desiredWhiteKeys;
        int desiredOctaves;

        if (width <= 0) {
            return;
        }
        desiredWhiteKeys = std::clamp(width / 26, 21, 42);
        desiredOctaves = std::clamp(desiredWhiteKeys / 7, 3, 6);
        if (desiredOctaves != m_octaves) {
            m_octaves = desiredOctaves;
            m_baseNote = std::clamp(m_baseNote, 0, std::max(0, 127 - (m_octaves * 12)));
            Refresh();
        }
    }

private:
    std::function<void(int)> m_noteOn;
    std::function<void()> m_noteOff;
    int m_baseNote;
    int m_octaves;
    int m_pressedKey;
    int m_externalPressedKey = -1;
    bool m_noteIsOn;
    wxTimer m_releaseWatchdog;

    static bool IsBlackKey(int noteInOctave) {
        return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
               noteInOctave == 8 || noteInOctave == 10;
    }

    int VisibleEndNote() const {
        return std::min(127, m_baseNote + m_octaves * 12);
    }

    bool SafeInvokeNoteOn(int note) {
        try {
            if (m_noteOn) {
                m_noteOn(note);
            }
            return true;
        } catch (std::exception const &ex) {
            fprintf(stderr, "[nbstudio] piano preview note-on exception: %s\n", ex.what());
        } catch (...) {
            fprintf(stderr, "[nbstudio] piano preview note-on unknown exception\n");
        }

        if (m_releaseWatchdog.IsRunning()) {
            m_releaseWatchdog.Stop();
        }
        m_noteIsOn = false;
        if (HasCapture()) {
            ReleaseMouse();
        }
        return false;
    }

    void SafeInvokeNoteOff() {
        try {
            if (m_noteOff) {
                m_noteOff();
            }
        } catch (std::exception const &ex) {
            fprintf(stderr, "[nbstudio] piano preview note-off exception: %s\n", ex.what());
        } catch (...) {
            fprintf(stderr, "[nbstudio] piano preview note-off unknown exception\n");
        }
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
        int highlightedNote = (m_pressedKey >= 0) ? m_pressedKey : m_externalPressedKey;

        dc.SetBackground(wxBrush(wxColour(30, 30, 30)));
        dc.Clear();

        /* Draw white keys */
        for (int note = m_baseNote; note <= endNote; ++note) {
            int x1;
            int x2;
            if (WhiteKeyRectForNote(note, size, whiteKeyWidth, &x1, &x2)) {
                if (note == highlightedNote) {
                    dc.SetBrush(wxBrush(wxColour(170, 205, 165)));
                    dc.SetPen(wxPen(wxColour(95, 150, 95), 1));
                } else {
                    dc.SetBrush(wxBrush(wxColour(240, 240, 235)));
                    dc.SetPen(wxPen(wxColour(100, 100, 100), 1));
                }
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
        if (highlightedNote >= 0) {
            /* Tint the actual pressed key so the feedback is visible while dragging. */
            if (IsBlackKey(highlightedNote % 12)) {
                int bx;
                int bw;
                if (BlackKeyRectForNote(highlightedNote, size, whiteKeyWidth, blackKeyWidth, &bx, &bw)) {
                    dc.SetBrush(wxBrush(wxColour(70, 110, 70)));
                    dc.SetPen(wxPen(wxColour(160, 220, 160), 1));
                    dc.DrawRectangle(bx, 0, bw, blackKeyHeight);
                }
            }
            dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
            dc.SetTextForeground(wxColour(40, 200, 40));
            dc.DrawText(wxString::Format("Note: %d", highlightedNote), 4, size.y - 14);
        }
    }

    void OnSize(wxSizeEvent &event) {
        UpdateVisibleRangeForWidth(event.GetSize().x);
        event.Skip();
    }

    void OnMouseDown(wxMouseEvent &evt) {
        int note = NoteAtPosition(evt.GetX(), evt.GetY());

        /* Clicking the piano should restore keyboard focus for musical typing. */
        SetFocus();
        if (note < 0) {
            StopPressedNote();
            return;
        }
        if (!HasCapture()) {
            CaptureMouse();
        }
        if (note != m_pressedKey) {
            if (m_noteIsOn && m_noteOff) {
                SafeInvokeNoteOff();
                m_noteIsOn = false;
            }
            m_pressedKey = note;
            if (m_noteOn) {
                if (SafeInvokeNoteOn(note)) {
                    m_noteIsOn = true;
                    if (!m_releaseWatchdog.IsRunning()) {
                        m_releaseWatchdog.Start(25);
                    }
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
                    SafeInvokeNoteOff();
                    m_noteIsOn = false;
                }
                m_pressedKey = note;
                if (m_noteOn) {
                    if (SafeInvokeNoteOn(note)) {
                        m_noteIsOn = true;
                        if (!m_releaseWatchdog.IsRunning()) {
                            m_releaseWatchdog.Start(25);
                        }
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
            SafeInvokeNoteOff();
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
          m_loopStart(0), m_loopEnd(0), m_dragMode(DragMode::None) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &WaveformPanelExt::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &WaveformPanelExt::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &WaveformPanelExt::OnLeftUp, this);
        Bind(wxEVT_MOTION, &WaveformPanelExt::OnMouseMove, this);
        Bind(wxEVT_RIGHT_DOWN, &WaveformPanelExt::OnRightDown, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &WaveformPanelExt::OnMouseCaptureLost, this);
    }

    void SetWaveform(void const *waveData, uint32_t frameCount, uint16_t bitSize, uint16_t channels) {
        m_waveData = waveData;
        m_frameCount = frameCount;
        m_bitSize = bitSize;
        m_channels = std::max<uint16_t>(1, channels);
        ClampLoopToFrameCount();
        Refresh();
    }

    void SetLoopPoints(uint32_t loopStart, uint32_t loopEnd) {
        m_loopStart = loopStart;
        m_loopEnd = loopEnd;
        ClampLoopToFrameCount();
        Refresh();
    }

    void SetLoopChangedCallback(std::function<void(uint32_t, uint32_t)> callback) {
        m_loopChanged = std::move(callback);
    }

private:
    enum class DragMode {
        None,
        Start,
        End,
    };

    void const *m_waveData;
    uint32_t m_frameCount;
    uint16_t m_bitSize, m_channels;
    uint32_t m_loopStart, m_loopEnd;
    DragMode m_dragMode;
    std::function<void(uint32_t, uint32_t)> m_loopChanged;

    float SampleAt(uint32_t frame, int channel) const {
        if (!m_waveData || m_frameCount == 0 || frame >= m_frameCount || m_channels == 0) return 0.0f;
        int clampedChannel = std::clamp(channel, 0, (int)m_channels - 1);
        uint32_t sampleIndex = frame * (uint32_t)m_channels + (uint32_t)clampedChannel;
        if (m_bitSize == 8) {
            unsigned char const *pcm = static_cast<unsigned char const *>(m_waveData);
            return (static_cast<float>(pcm[sampleIndex]) - 128.0f) / 128.0f;
        }
        if (m_bitSize == 16) {
            int16_t const *pcm = static_cast<int16_t const *>(m_waveData);
            return static_cast<float>(pcm[sampleIndex]) / 32768.0f;
        }
        return 0.0f;
    }

    bool HasLoop() const {
        return m_frameCount > 0 && m_loopEnd > m_loopStart;
    }

    void ClampLoopToFrameCount() {
        if (m_frameCount == 0) {
            m_loopStart = 0;
            m_loopEnd = 0;
            return;
        }
        m_loopStart = std::min(m_loopStart, m_frameCount - 1);
        m_loopEnd = std::min(m_loopEnd, m_frameCount);
        if (m_loopEnd <= m_loopStart) {
            m_loopStart = 0;
            m_loopEnd = 0;
        }
    }

    uint32_t FrameFromX(int x) const {
        int width = std::max(1, GetClientSize().x - 1);
        int clampedX = std::clamp(x, 0, width);
        if (m_frameCount == 0) {
            return 0;
        }
        return (uint32_t)(((uint64_t)clampedX * (uint64_t)m_frameCount) / (uint64_t)width);
    }

    int XFromFrame(uint32_t frame) const {
        int width = std::max(1, GetClientSize().x - 1);
        if (m_frameCount == 0) {
            return 0;
        }
        uint32_t clampedFrame = std::min(frame, m_frameCount);
        return (int)(((uint64_t)clampedFrame * (uint64_t)width) / (uint64_t)m_frameCount);
    }

    void NotifyLoopChanged() {
        if (m_loopChanged) {
            m_loopChanged(m_loopStart, m_loopEnd);
        }
    }

    void DrawChannel(wxDC &dc,
                     wxSize const &size,
                     int channel,
                     int top,
                     int bottom,
                     wxColour const &waveColour,
                     wxColour const &centerColour) const {
        int laneHeight = std::max(1, bottom - top);
        int centerY = top + laneHeight / 2;
        dc.SetPen(wxPen(centerColour, 1));
        dc.DrawLine(0, centerY, size.x, centerY);

        dc.SetPen(wxPen(waveColour, 1));
        uint32_t framesPerPixel = std::max<uint32_t>(1, m_frameCount / (uint32_t)std::max(1, size.x));
        for (int x = 0; x < size.x; ++x) {
            uint32_t start = (uint32_t)x * framesPerPixel;
            uint32_t end = std::min<uint32_t>(m_frameCount, start + framesPerPixel);
            float minV = 1.0f;
            float maxV = -1.0f;

            if (start >= m_frameCount) {
                break;
            }
            if (end <= start) {
                end = std::min<uint32_t>(m_frameCount, start + 1);
            }
            for (uint32_t f = start; f < end; f++) {
                float v = SampleAt(f, channel);
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
            }
            int amp = std::max(1, (laneHeight / 2) - 3);
            int y1 = centerY - (int)(maxV * amp);
            int y2 = centerY - (int)(minV * amp);
            dc.DrawLine(x, y1, x, y2);
        }
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();

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
        if (m_channels >= 2) {
            int splitY = size.y / 2;
            dc.SetPen(wxPen(wxColour(45, 45, 45), 1));
            dc.DrawLine(0, splitY, size.x, splitY);
            DrawChannel(dc, size, 0, 0, splitY, wxColour(98, 215, 255), wxColour(70, 70, 70));
            DrawChannel(dc, size, 1, splitY, size.y, wxColour(255, 188, 98), wxColour(70, 70, 70));
        } else {
            DrawChannel(dc, size, 0, 0, size.y, wxColour(98, 215, 255), wxColour(70, 70, 70));
        }
        if (hasLoop) {
            int xS = (int)((int64_t)m_loopStart * size.x / m_frameCount);
            int xE = (int)((int64_t)m_loopEnd * size.x / m_frameCount);
            dc.SetPen(wxPen(wxColour(0, 220, 80), 2));
            dc.DrawLine(xS, 0, xS, size.y);
            dc.SetPen(wxPen(wxColour(255, 160, 0), 2));
            dc.DrawLine(xE, 0, xE, size.y);

            dc.SetBrush(wxBrush(wxColour(0, 220, 80)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawCircle(xS, 8, 4);
            dc.SetBrush(wxBrush(wxColour(255, 160, 0)));
            dc.DrawCircle(xE, 8, 4);
        }
    }

    void OnLeftDown(wxMouseEvent &event) {
        int x;
        int startX;
        int endX;

        if (!HasLoop()) {
            event.Skip();
            return;
        }
        x = event.GetX();
        startX = XFromFrame(m_loopStart);
        endX = XFromFrame(m_loopEnd);

        if (std::abs(x - startX) <= 6) {
            m_dragMode = DragMode::Start;
        } else if (std::abs(x - endX) <= 6) {
            m_dragMode = DragMode::End;
        } else {
            event.Skip();
            return;
        }
        if (!HasCapture()) {
            CaptureMouse();
        }
    }

    void OnLeftUp(wxMouseEvent &) {
        m_dragMode = DragMode::None;
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void OnMouseMove(wxMouseEvent &event) {
        uint32_t frame;

        if (m_dragMode == DragMode::None || !event.LeftIsDown() || m_frameCount == 0) {
            event.Skip();
            return;
        }
        frame = FrameFromX(event.GetX());
        if (m_dragMode == DragMode::Start) {
            uint32_t newStart = std::min(frame, (m_loopEnd > 0) ? (m_loopEnd - 1) : 0);
            if (newStart != m_loopStart) {
                m_loopStart = newStart;
                NotifyLoopChanged();
                Refresh();
            }
        } else if (m_dragMode == DragMode::End) {
            uint32_t newEnd = std::max(frame, m_loopStart + 1);
            newEnd = std::min(newEnd, m_frameCount);
            if (newEnd != m_loopEnd) {
                m_loopEnd = newEnd;
                NotifyLoopChanged();
                Refresh();
            }
        }
    }

    void OnRightDown(wxMouseEvent &event) {
        wxMenu menu;
        int idSetStart = wxWindow::NewControlId();
        int idSetEnd = wxWindow::NewControlId();
        int idClearLoop = wxWindow::NewControlId();
        int selectedId;
        uint32_t frame;

        if (m_frameCount == 0) {
            return;
        }
        frame = FrameFromX(event.GetX());
        menu.Append(idSetStart, "Set Loop Start Here");
        menu.Append(idSetEnd, "Set Loop End Here");
        menu.AppendSeparator();
        menu.Append(idClearLoop, "Clear Loop");
        menu.Enable(idClearLoop, HasLoop());

        selectedId = GetPopupMenuSelectionFromUser(menu, event.GetPosition());
        if (selectedId == idSetStart) {
            uint32_t end = HasLoop() ? m_loopEnd : std::min<uint32_t>(m_frameCount, frame + 1);
            m_loopStart = std::min(frame, (end > 0) ? (end - 1) : 0);
            m_loopEnd = std::max(end, m_loopStart + 1);
            m_loopEnd = std::min(m_loopEnd, m_frameCount);
            NotifyLoopChanged();
            Refresh();
        } else if (selectedId == idSetEnd) {
            uint32_t start = HasLoop() ? m_loopStart : std::min<uint32_t>(frame, m_frameCount - 1);
            uint32_t end = std::max(frame, start + 1);
            m_loopStart = start;
            m_loopEnd = std::min(end, m_frameCount);
            NotifyLoopChanged();
            Refresh();
        } else if (selectedId == idClearLoop) {
            m_loopStart = 0;
            m_loopEnd = 0;
            NotifyLoopChanged();
            Refresh();
        }
    }

    void OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
        m_dragMode = DragMode::None;
    }
};

/* ====================================================================== */
/*  ADSR Envelope graph panel                                             */
/* ====================================================================== */

class ADSRGraphPanel final : public wxPanel {
public:
    explicit ADSRGraphPanel(wxWindow *parent)
                : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 96), wxBORDER_SIMPLE),
          m_stageCount(0) {
        memset(m_levels, 0, sizeof(m_levels));
        memset(m_times, 0, sizeof(m_times));
        memset(m_flags, 0, sizeof(m_flags));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ADSRGraphPanel::OnPaint, this);
    }

    void SetEnvelope(int stageCount, int32_t const *levels, int32_t const *times, int32_t const *flags) {
        m_stageCount = std::clamp(stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
        for (int i = 0; i < m_stageCount; i++) {
            m_levels[i] = levels[i];
            m_times[i] = times[i];
            m_flags[i] = flags[i];
        }
        Refresh();
    }

private:
    int m_stageCount;
    int32_t m_levels[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t m_times[BAE_EDITOR_MAX_ADSR_STAGES];   /* microseconds */
    int32_t m_flags[BAE_EDITOR_MAX_ADSR_STAGES];

    static wxColour StageColour(int32_t flags) {
        if (flags == (int32_t)FOUR_CHAR('S','U','S','T')) return wxColour(80, 200, 80);
        if (flags == (int32_t)FOUR_CHAR('R','E','L','S')) return wxColour(200, 80, 80);
        if (flags == (int32_t)FOUR_CHAR('L','A','S','T')) return wxColour(200, 200, 60);
        if (flags == (int32_t)FOUR_CHAR('G','O','T','O')) return wxColour(140, 100, 220);
        if (flags == (int32_t)FOUR_CHAR('G','O','S','T')) return wxColour(100, 140, 220);
        return wxColour(98, 215, 255);  /* LINEAR_RAMP / default */
    }

    static char const *StageName(int32_t flags) {
        if (flags == (int32_t)FOUR_CHAR('L','I','N','E')) return "LIN";
        if (flags == (int32_t)FOUR_CHAR('S','U','S','T')) return "SUS";
        if (flags == (int32_t)FOUR_CHAR('L','A','S','T')) return "END";
        if (flags == (int32_t)FOUR_CHAR('G','O','T','O')) return "GOTO";
        if (flags == (int32_t)FOUR_CHAR('G','O','S','T')) return "GOST";
        if (flags == (int32_t)FOUR_CHAR('R','E','L','S')) return "REL";
        return "OFF";
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        int w = size.x;
        int h = size.y;

        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();

        if (m_stageCount <= 0 || w < 20 || h < 20) {
            dc.SetTextForeground(wxColour(120, 120, 120));
            dc.DrawText("No ADSR stages", 8, h / 2 - 6);
            return;
        }

        int const margin = 6;
        int const graphL = margin + 30;
        int const graphR = w - margin;
        int const graphT = margin;
        int const graphB = h - margin - 14;  /* leave room for labels */
        int const graphW = graphR - graphL;
        int const graphH = graphB - graphT;

        if (graphW < 10 || graphH < 10) return;

        /* Find level range and total time for scaling */
        int32_t minLevel = 0;
        int32_t maxLevel = 0;
        int64_t totalTime = 0;
        for (int i = 0; i < m_stageCount; i++) {
            if (m_levels[i] < minLevel) minLevel = m_levels[i];
            if (m_levels[i] > maxLevel) maxLevel = m_levels[i];
            totalTime += std::abs((int64_t)m_times[i]);
        }
        if (maxLevel <= minLevel) maxLevel = minLevel + 1;
        if (totalTime <= 0) totalTime = 1;

        /* Y-axis helper: level -> pixel y */
        auto levelToY = [&](int32_t level) -> int {
            double norm = (double)(level - minLevel) / (double)(maxLevel - minLevel);
            return graphB - (int)(norm * graphH);
        };

        /* Draw gridlines */
        dc.SetPen(wxPen(wxColour(50, 50, 50), 1));
        dc.DrawLine(graphL, graphT, graphL, graphB);
        dc.DrawLine(graphL, graphB, graphR, graphB);
        /* Zero level line */
        if (minLevel < 0 && maxLevel > 0) {
            int y0 = levelToY(0);
            dc.SetPen(wxPen(wxColour(60, 60, 60), 1, wxPENSTYLE_DOT));
            dc.DrawLine(graphL, y0, graphR, y0);
        }

        /* Y axis labels */
        dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        dc.SetTextForeground(wxColour(140, 140, 140));
        dc.DrawText(wxString::Format("%d", maxLevel), margin, graphT - 2);
        dc.DrawText(wxString::Format("%d", minLevel), margin, graphB - 8);

        /* Draw envelope segments */
        int32_t prevLevel = 0;  /* starts at zero before first stage */
        int64_t cumulativeTime = 0;

        for (int i = 0; i < m_stageCount; i++) {
            int64_t stageTime = std::abs((int64_t)m_times[i]);
            int x1 = graphL + (int)((double)cumulativeTime / (double)totalTime * graphW);
            int x2 = graphL + (int)((double)(cumulativeTime + stageTime) / (double)totalTime * graphW);
            int y1 = levelToY(prevLevel);
            int y2 = levelToY(m_levels[i]);

            wxColour colour = StageColour(m_flags[i]);

            /* Stage background band */
            dc.SetBrush(wxBrush(wxColour(colour.Red() / 6, colour.Green() / 6, colour.Blue() / 6)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x1, graphT, std::max(1, x2 - x1), graphH);

            /* Envelope line */
            dc.SetPen(wxPen(colour, 2));
            if (m_flags[i] == (int32_t)FOUR_CHAR('S','U','S','T') ||
                m_flags[i] == 0) {
                /* Hold at level (sustain or OFF) */
                int yHold = levelToY(m_levels[i]);
                dc.DrawLine(x1, y1, x1, yHold);
                dc.DrawLine(x1, yHold, x2, yHold);
            } else {
                /* Ramp from previous level to target */
                dc.DrawLine(x1, y1, x2, y2);
            }

            /* Stage label at bottom */
            dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
            dc.SetTextForeground(colour);
            int labelX = (x1 + x2) / 2 - 8;
            dc.DrawText(StageName(m_flags[i]), std::max(graphL, labelX), graphB + 2);

            cumulativeTime += stageTime;
            prevLevel = m_levels[i];
        }

        /* Draw stage boundary ticks */
        cumulativeTime = 0;
        dc.SetPen(wxPen(wxColour(80, 80, 80), 1, wxPENSTYLE_DOT));
        for (int i = 0; i < m_stageCount - 1; i++) {
            cumulativeTime += std::abs((int64_t)m_times[i]);
            int x = graphL + (int)((double)cumulativeTime / (double)totalTime * graphW);
            dc.DrawLine(x, graphT, x, graphB);
        }
    }
};

/* ====================================================================== */
/*  LFO waveform graph panel                                              */
/* ====================================================================== */

class LFOWaveformGraphPanel final : public wxPanel {
public:
    explicit LFOWaveformGraphPanel(wxWindow *parent)
                : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 72), wxBORDER_SIMPLE),
          m_waveShape((int32_t)FOUR_CHAR('S','I','N','E')),
          m_period(0),
          m_dcFeed(0),
          m_level(0) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &LFOWaveformGraphPanel::OnPaint, this);
    }

    void SetLFO(int32_t waveShape, int32_t period, int32_t dcFeed, int32_t level) {
        m_waveShape = waveShape;
        m_period = period;
        m_dcFeed = dcFeed;
        m_level = level;
        Refresh();
    }

private:
    int32_t m_waveShape;
    int32_t m_period;
    int32_t m_dcFeed;
    int32_t m_level;

    static double EvaluateWave(int32_t waveShape, double phase) {
        phase -= std::floor(phase);
        if (phase < 0.0) {
            phase += 1.0;
        }

        switch (waveShape) {
            case (int32_t)FOUR_CHAR('T','R','I','A'):
            {
                double tri = 4.0 * std::fabs(phase - 0.5) - 1.0;
                return -tri;
            }
            case (int32_t)FOUR_CHAR('S','Q','U','A'):
                return (phase < 0.5) ? 1.0 : -1.0;
            case (int32_t)FOUR_CHAR('S','Q','U','2'):
                return (phase < 0.25) ? 1.0 : -1.0;
            case (int32_t)FOUR_CHAR('S','A','W','T'):
                return (2.0 * phase) - 1.0;
            case (int32_t)FOUR_CHAR('S','A','W','2'):
                return 1.0 - (2.0 * phase);
            case (int32_t)FOUR_CHAR('S','I','N','E'):
            default:
                return std::sin(phase * 2.0 * 3.14159265358979323846);
        }
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        int w = size.x;
        int h = size.y;

        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();

        if (w < 20 || h < 20) {
            return;
        }

        int const margin = 6;
        int const left = margin;
        int const right = w - margin;
        int const top = margin;
        int const bottom = h - margin;
        int const width = right - left;
        int const height = bottom - top;
        int const centerY = top + height / 2;

        dc.SetPen(wxPen(wxColour(55, 55, 55), 1));
        dc.DrawRectangle(left, top, width, height);
        dc.DrawLine(left, centerY, right, centerY);

        if (m_period == 0 && m_level == 0 && m_dcFeed == 0) {
            dc.SetTextForeground(wxColour(120, 120, 120));
            dc.DrawText("LFO preview", left + 8, centerY - 6);
            return;
        }

        int64_t span = std::max<int64_t>(1, std::llabs((int64_t)m_level) + std::llabs((int64_t)m_dcFeed));

        dc.SetPen(wxPen(wxColour(98, 215, 255), 2));
        wxPoint prev;
        bool hasPrev = false;

        for (int x = 0; x < width; ++x) {
            double phase = (width > 1) ? ((double)x / (double)(width - 1)) : 0.0;
            double wave = EvaluateWave(m_waveShape, phase);
            double value = wave * (double)m_level + (double)m_dcFeed;
            double normalized = value / (double)span;
            normalized = std::clamp(normalized, -1.0, 1.0);
            int y = centerY - (int)(normalized * (height / 2 - 2));
            wxPoint p(left + x, y);

            if (hasPrev) {
                dc.DrawLine(prev, p);
            }
            prev = p;
            hasPrev = true;
        }

        dc.SetTextForeground(wxColour(150, 150, 150));
        dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        dc.DrawText(wxString::Format("period=%d  level=%d  dc=%d", m_period, m_level, m_dcFeed), left + 6, top + 4);
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
                              std::function<void(wxString const &)> beginUndoCallback,
                              std::function<void(wxString const &)> commitUndoCallback,
                              std::function<void()> cancelUndoCallback,
                              std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char, BAERmfEditorCompressionType, bool)> playCallback,
                              std::function<void()> stopCallback,
                              std::function<bool(uint32_t, wxString const &)> replaceCallback,
                              std::function<bool(uint32_t, wxString const &)> exportCallback)
        : wxDialog(parent, wxID_ANY, "Edit Instrument", wxDefaultPosition, wxSize(1180, 560),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_document(document),
                    m_instID(0),
          m_extInfo(*initialExtInfo),
          m_extInfoModified(false),
          m_currentLocalIndex(-1),
          m_currentLfoIndex(-1),
          m_savedLoopStart(0),
          m_savedLoopEnd(0),
          m_beginUndoCallback(std::move(beginUndoCallback)),
          m_commitUndoCallback(std::move(commitUndoCallback)),
          m_cancelUndoCallback(std::move(cancelUndoCallback)),
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
                SaveCurrentSampleFromUI();
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
                               m_samples[(size_t)best].rootKey,
                               m_samples[(size_t)best].compressionType,
                               m_samples[(size_t)best].opusRoundTripResample);
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
        SetSize(wxSize(1180, 560));

        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &evt) {
            if (m_pianoPanel) {
                m_pianoPanel->ForceStop();
            }
            StopKeyboardPreviewIfActive();
            evt.Skip();
        });

        Bind(wxEVT_BUTTON, &InstrumentExtEditorDialog::OnApply, this, wxID_APPLY);
        Bind(wxEVT_BUTTON, &InstrumentExtEditorDialog::OnOk, this, wxID_OK);
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) {
            StopKeyboardPreviewIfActive();
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
            StopKeyboardPreviewIfActive();
            event.Skip();
        });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
            int keyCode = event.GetKeyCode();
            bool ctrlDown = event.ControlDown() || event.CmdDown();
            wxWindow *focus = wxWindow::FindFocus();

            if (keyCode == WXK_ESCAPE && IsTextEditingFocus(focus)) {
                FocusKeyboardPreviewTarget();
                return;
            }

            if (ctrlDown && !event.ShiftDown() && !event.AltDown() &&
                (keyCode == 'Z' || keyCode == 'z')) {
                wxTextCtrl *textCtrl = wxDynamicCast(focus, wxTextCtrl);
                if (textCtrl && textCtrl->CanUndo()) {
                    textCtrl->Undo();
                    return;
                }
                wxCommandEvent cmd(wxEVT_MENU, wxID_UNDO);
                wxWindow *target = wxGetTopLevelParent(this);
                if (target && target != this) {
                    if (target->GetEventHandler()->ProcessEvent(cmd)) {
                        return;
                    }
                }
            }
            if (ctrlDown && !event.AltDown() &&
                (keyCode == 'Y' || keyCode == 'y' ||
                 (event.ShiftDown() && (keyCode == 'Z' || keyCode == 'z')))) {
                wxTextCtrl *textCtrl = wxDynamicCast(focus, wxTextCtrl);
                if (textCtrl && textCtrl->CanRedo()) {
                    textCtrl->Redo();
                    return;
                }
                wxCommandEvent cmd(wxEVT_MENU, wxID_REDO);
                wxWindow *target = wxGetTopLevelParent(this);
                if (target && target != this) {
                    if (target->GetEventHandler()->ProcessEvent(cmd)) {
                        return;
                    }
                }
            }
            if (!ctrlDown && !event.AltDown() && StartKeyboardPreviewForKey(keyCode)) {
                return;
            }
            event.Skip();
        });
        BindKeyboardReleaseHandlers(this);
        BindBackgroundFocusHandlers(this);
        if (m_notebook) {
            m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent &event) {
                StopKeyboardPreviewIfActive();
                event.Skip();
            });
        }
        Bind(wxEVT_SHOW, [this](wxShowEvent &event) {
            if (event.IsShown() && m_notebook) {
                m_notebook->SetFocus();
            }
            event.Skip();
        });

        /* Load initial sample data */
        if (!m_samples.empty()) {
            m_splitChoice->SetSelection(0);
            LoadLocalSample(0);
        }

        auto instantApply = [this](wxCommandEvent &event) {
            if (!m_loadingUiValues) {
                wxCommandEvent evt;
                OnApply(evt);
            }
            event.Skip();
        };
        /* Keep instrument name text undo-friendly by not applying on every keystroke. */
        if (m_instProgramSpin) {
            m_instProgramSpin->Bind(wxEVT_SPINCTRL, instantApply);
            m_instProgramSpin->Bind(wxEVT_TEXT, instantApply);
        }
        if (m_nameText) m_nameText->Bind(wxEVT_TEXT, instantApply);
        if (m_rootSpin) {
            m_rootSpin->Bind(wxEVT_SPINCTRL, instantApply);
            m_rootSpin->Bind(wxEVT_TEXT, instantApply);
        }
        if (m_lowSpin) {
            m_lowSpin->Bind(wxEVT_SPINCTRL, instantApply);
            m_lowSpin->Bind(wxEVT_TEXT, instantApply);
        }
        if (m_highSpin) {
            m_highSpin->Bind(wxEVT_SPINCTRL, instantApply);
            m_highSpin->Bind(wxEVT_TEXT, instantApply);
        }
        if (m_sampleRateSpin) {
            m_sampleRateSpin->Bind(wxEVT_SPINCTRL, instantApply);
            m_sampleRateSpin->Bind(wxEVT_TEXT, instantApply);
        }
        if (m_splitVolumeSpin) {
            m_splitVolumeSpin->Bind(wxEVT_SPINCTRL, instantApply);
            m_splitVolumeSpin->Bind(wxEVT_TEXT, instantApply);
        }
        if (m_codecChoice) m_codecChoice->Bind(wxEVT_CHOICE, instantApply);
        if (m_bitrateChoice) m_bitrateChoice->Bind(wxEVT_CHOICE, instantApply);
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
    int m_currentLfoIndex;
    uint32_t m_savedLoopStart, m_savedLoopEnd;
    std::function<void(wxString const &)> m_beginUndoCallback;
    std::function<void(wxString const &)> m_commitUndoCallback;
    std::function<void()> m_cancelUndoCallback;
    std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char, BAERmfEditorCompressionType, bool)> m_playCallback;
    std::function<void()> m_stopCallback;
    std::function<bool(uint32_t, wxString const &)> m_replaceCallback;
    std::function<bool(uint32_t, wxString const &)> m_exportCallback;

    wxNotebook *m_notebook;
    PianoKeyboardPanel *m_pianoPanel;
    wxButton *m_applyButton;
    bool m_loadingUiValues = false;
    bool m_inInstantApply = false;

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
    wxSpinCtrlDouble *m_adsrTime[BAE_EDITOR_MAX_ADSR_STAGES];
    wxChoice *m_adsrFlags[BAE_EDITOR_MAX_ADSR_STAGES];
    wxStaticText *m_adsrRowLabels[BAE_EDITOR_MAX_ADSR_STAGES];
    ADSRGraphPanel *m_adsrGraph;
    /* LFO controls */
    wxSpinCtrl *m_lfoCountSpin;
    wxChoice *m_lfoSelector;
    wxChoice *m_lfoDest;
    wxChoice *m_lfoShape;
    wxSpinCtrl *m_lfoPeriod;
    wxSpinCtrl *m_lfoDCFeed;
    wxSpinCtrl *m_lfoLevel;
    LFOWaveformGraphPanel *m_lfoGraph;
    ADSRGraphPanel *m_lfoAdsrGraph;
    ADSRGraphPanel *m_filterGraph;

    /* Samples tab controls */
    wxChoice *m_splitChoice;
    wxTextCtrl *m_nameText;
    wxSpinCtrl *m_rootSpin;
    wxSpinCtrl *m_lowSpin;
    wxSpinCtrl *m_highSpin;
    wxSpinCtrl *m_sampleRateSpin;
    wxSpinCtrl *m_splitVolumeSpin;
    wxCheckBox *m_loopEnableCheck;
    wxSpinCtrl *m_loopStartSpin;
    wxSpinCtrl *m_loopEndSpin;
    wxStaticText *m_loopInfoLabel;
    wxChoice *m_codecChoice;
    wxChoice *m_bitrateChoice;
    wxChoice *m_opusModeChoice;
    wxStaticText *m_codecLabel;
    wxChoice *m_sndStorageChoice;
    WaveformPanelExt *m_waveformPanel;
    wxButton *m_deleteSampleButton;
    int m_keyboardPreviewKeyCode = -1;
    int m_keyboardPreviewNote = -1;

    static int NormalizeAsciiKeyCode(int keyCode) {
        if (keyCode >= 'a' && keyCode <= 'z') {
            return keyCode - ('a' - 'A');
        }
        return keyCode;
    }

    static int KeyCodeToSemitoneOffset(int keyCode) {
        switch (NormalizeAsciiKeyCode(keyCode)) {
            /* Match GUI keyboard sequence: a w s e d f t g y h u j k o */
            case 'A': return 0;
            case 'W': return 1;
            case 'S': return 2;
            case 'E': return 3;
            case 'D': return 4;
            case 'F': return 5;
            case 'T': return 6;
            case 'G': return 7;
            case 'Y': return 8;
            case 'H': return 9;
            case 'U': return 10;
            case 'J': return 11;
            case 'K': return 12;
            case 'O': return 13;
            default: return -1;
        }
    }

    static bool IsTextEditingFocus(wxWindow *focus) {
        return wxDynamicCast(focus, wxTextCtrl) != nullptr ||
               wxDynamicCast(focus, wxSpinCtrl) != nullptr ||
               wxDynamicCast(focus, wxSpinCtrlDouble) != nullptr;
    }

    void FocusKeyboardPreviewTarget() {
        if (m_pianoPanel && m_pianoPanel->IsShown()) {
            m_pianoPanel->SetFocus();
            return;
        }
        if (m_notebook) {
            m_notebook->SetFocus();
            return;
        }
        SetFocus();
    }

    bool ShouldReturnFocusFromClickTarget(wxWindow *target) const {
        if (!target) {
            return false;
        }
        if (target == m_pianoPanel || target == m_waveformPanel) {
            return false;
        }
        if (wxDynamicCast(target, wxTextCtrl) != nullptr ||
            wxDynamicCast(target, wxSpinCtrl) != nullptr ||
            wxDynamicCast(target, wxSpinCtrlDouble) != nullptr ||
            wxDynamicCast(target, wxChoice) != nullptr ||
            wxDynamicCast(target, wxCheckBox) != nullptr ||
            wxDynamicCast(target, wxButton) != nullptr ||
            wxDynamicCast(target, wxNotebook) != nullptr) {
            return false;
        }
        return target == this ||
               wxDynamicCast(target, wxPanel) != nullptr ||
               wxDynamicCast(target, wxStaticText) != nullptr ||
               wxDynamicCast(target, wxStaticBox) != nullptr;
    }

    void HandleBackgroundLeftDown(wxMouseEvent &event) {
        wxWindow *target = wxDynamicCast((wxObject *)event.GetEventObject(), wxWindow);
        if (ShouldReturnFocusFromClickTarget(target)) {
            FocusKeyboardPreviewTarget();
        }
        event.Skip();
    }

    void BindBackgroundFocusHandlers(wxWindow *window) {
        wxWindowList::compatibility_iterator node;

        if (!window) {
            return;
        }
        window->Bind(wxEVT_LEFT_DOWN, &InstrumentExtEditorDialog::HandleBackgroundLeftDown, this);
        node = window->GetChildren().GetFirst();
        while (node) {
            BindBackgroundFocusHandlers(node->GetData());
            node = node->GetNext();
        }
    }

    void BindKeyboardReleaseHandlers(wxWindow *window) {
        wxWindowList::compatibility_iterator node;

        if (!window) {
            return;
        }
        window->Bind(wxEVT_KEY_UP, &InstrumentExtEditorDialog::HandleKeyboardPreviewKeyUp, this);
        node = window->GetChildren().GetFirst();
        while (node) {
            BindKeyboardReleaseHandlers(node->GetData());
            node = node->GetNext();
        }
    }

    void StopKeyboardPreviewIfActive() {
        if (m_keyboardPreviewNote >= 0 && m_stopCallback) {
            m_stopCallback();
        }
        if (m_pianoPanel) {
            m_pianoPanel->ClearExternalPressedNote();
        }
        m_keyboardPreviewKeyCode = -1;
        m_keyboardPreviewNote = -1;
    }

    bool StartKeyboardPreviewForKey(int keyCode) {
        int semitoneOffset;
        int root;
        int base;
        int note;

        if (!m_playCallback || m_sampleIndices.empty() || IsTextEditingFocus(wxWindow::FindFocus())) {
            return false;
        }
        semitoneOffset = KeyCodeToSemitoneOffset(keyCode);
        if (semitoneOffset < 0) {
            return false;
        }

        root = m_rootSpin ? m_rootSpin->GetValue() : 60;
        /* Keep keyboard note sequence centered on the split root key, same as GUI typing mode. */
        base = std::max(0, root);
        note = std::clamp(base + semitoneOffset, 0, 127);
        keyCode = NormalizeAsciiKeyCode(keyCode);
        if (keyCode == m_keyboardPreviewKeyCode && note == m_keyboardPreviewNote) {
            return true;
        }

        StopKeyboardPreviewIfActive();
        SaveCurrentSampleFromUI();

        {
            int best = -1;
            for (size_t i = 0; i < m_samples.size(); i++) {
                if (note >= (int)m_samples[i].lowKey && note <= (int)m_samples[i].highKey) {
                    best = (int)i;
                    break;
                }
            }
            if (best < 0) {
                return true;
            }
            m_playCallback(m_sampleIndices[(size_t)best], note,
                           &m_samples[(size_t)best].sampleInfo,
                           m_samples[(size_t)best].splitVolume,
                           m_samples[(size_t)best].rootKey,
                           m_samples[(size_t)best].compressionType,
                           m_samples[(size_t)best].opusRoundTripResample);
        }

        m_keyboardPreviewKeyCode = keyCode;
        m_keyboardPreviewNote = note;
        if (m_pianoPanel) {
            m_pianoPanel->SetExternalPressedNote(note);
        }
        return true;
    }

    void HandleKeyboardPreviewKeyUp(wxKeyEvent &event) {
        int keyCode = NormalizeAsciiKeyCode(event.GetKeyCode());
        if (keyCode == m_keyboardPreviewKeyCode) {
            StopKeyboardPreviewIfActive();
            return;
        }
        event.Skip();
    }

    /* ---- Instrument Tab ---- */
    void BuildInstrumentTab() {
        wxPanel *page = new wxPanel(m_notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        wxStaticBoxSizer *lpfBox = nullptr;
        wxStaticBoxSizer *lfoBox = nullptr;
        wxStaticBoxSizer *adsrBox = nullptr;

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
            lpfBox = new wxStaticBoxSizer(wxHORIZONTAL, page, "Low-Pass Filter");
            wxBoxSizer *controlsRow = new wxBoxSizer(wxHORIZONTAL);

            controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Frequency"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfFrequency = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, -100000, 100000, m_extInfo.LPF_frequency);
            controlsRow->Add(m_lpfFrequency, 0, wxRIGHT, 12);
            controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Resonance"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfResonance = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, -100000, 100000, m_extInfo.LPF_resonance);
            controlsRow->Add(m_lpfResonance, 0, wxRIGHT, 12);
            controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Amount"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfLowpassAmount = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                                wxSP_ARROW_KEYS, -100000, 100000, m_extInfo.LPF_lowpassAmount);
            controlsRow->Add(m_lpfLowpassAmount, 0);
            lpfBox->Add(controlsRow, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

            wxBoxSizer *previewCol = new wxBoxSizer(wxVERTICAL);
            previewCol->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Filter Envelope Preview"), 0, wxBOTTOM, 2);
            m_filterGraph = new ADSRGraphPanel(lpfBox->GetStaticBox());
            m_filterGraph->SetMinSize(wxSize(-1, 60));
            previewCol->Add(m_filterGraph, 1, wxEXPAND);
            lpfBox->Add(previewCol, 1, wxEXPAND | wxALL, 4);

            m_lpfFrequency->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshFilterEnvelopeGraph(); });
            m_lpfResonance->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshFilterEnvelopeGraph(); });
            m_lpfLowpassAmount->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshFilterEnvelopeGraph(); });

        }

        /* Volume ADSR */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, page, "Volume ADSR");
            adsrBox = box;
            wxBoxSizer *countRow = new wxBoxSizer(wxHORIZONTAL);
            countRow->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Stages:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_adsrStageSpin = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                             wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_ADSR_STAGES, (int)m_extInfo.volumeADSR.stageCount);
            countRow->Add(m_adsrStageSpin, 0);
            box->Add(countRow, 0, wxALL, 4);

            /* Envelope graph */
            m_adsrGraph = new ADSRGraphPanel(box->GetStaticBox());
            m_adsrGraph->SetMinSize(wxSize(-1, 84));
            box->Add(m_adsrGraph, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

            /* Header */
            wxFlexGridSizer *grid = new wxFlexGridSizer(0, 4, 2, 6);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "#"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Time (s)"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Type"), 0, wxALIGN_CENTER);

            for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
                m_adsrRowLabels[i] = new wxStaticText(box->GetStaticBox(), wxID_ANY, wxString::Format("%d", i));
                grid->Add(m_adsrRowLabels[i], 0, wxALIGN_CENTER_VERTICAL);
                int lvl = (i < (int)m_extInfo.volumeADSR.stageCount) ? m_extInfo.volumeADSR.stages[i].level : 0;
                int32_t tmRaw = (i < (int)m_extInfo.volumeADSR.stageCount) ? m_extInfo.volumeADSR.stages[i].time : 0;
                int fl = (i < (int)m_extInfo.volumeADSR.stageCount) ? m_extInfo.volumeADSR.stages[i].flags : 0;
                m_adsrLevel[i] = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                                wxSP_ARROW_KEYS, -1000000, 1000000, lvl);
                grid->Add(m_adsrLevel[i], 0);
                m_adsrTime[i] = new wxSpinCtrlDouble(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                                     wxSP_ARROW_KEYS, -16.0, 16.0, (double)tmRaw / 1000000.0, 0.001);
                m_adsrTime[i]->SetDigits(3);
                grid->Add(m_adsrTime[i], 0);
                m_adsrFlags[i] = new wxChoice(box->GetStaticBox(), wxID_ANY);
                FillChoice(m_adsrFlags[i], kADSRFlagLabels, kADSRFlagCount);
                m_adsrFlags[i]->SetSelection(FindLabelIndex(kADSRFlagLabels, kADSRFlagCount, fl));
                grid->Add(m_adsrFlags[i], 0);

                /* Bind change events to refresh the graph */
                m_adsrLevel[i]->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshADSRGraph(); });
                m_adsrTime[i]->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &) { RefreshADSRGraph(); });
                m_adsrFlags[i]->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { RefreshADSRGraph(); });

                bool visible = (i < (int)m_extInfo.volumeADSR.stageCount);
                m_adsrRowLabels[i]->Show(visible);
                m_adsrLevel[i]->Show(visible);
                m_adsrTime[i]->Show(visible);
                m_adsrFlags[i]->Show(visible);
            }
            box->Add(grid, 0, wxALL, 4);

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
                RefreshADSRGraph();
            });

            RefreshADSRGraph();
            RefreshFilterEnvelopeGraph();
        }

        /* LFO section */
        {
            lfoBox = new wxStaticBoxSizer(wxVERTICAL, page, "LFOs");
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Count:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_lfoCountSpin = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                            wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_LFOS, (int)m_extInfo.lfoCount);
            row->Add(m_lfoCountSpin, 0, wxRIGHT, 15);
            row->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Edit LFO:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_lfoSelector = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
            for (int i = 0; i < BAE_EDITOR_MAX_LFOS; i++) m_lfoSelector->Append(wxString::Format("LFO %d", i));
            if (m_extInfo.lfoCount > 0) m_lfoSelector->SetSelection(0);
            row->Add(m_lfoSelector, 0);
            lfoBox->Add(row, 0, wxALL, 4);

            wxFlexGridSizer *grid = new wxFlexGridSizer(2, 4, 6);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Dest"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoDest = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
            FillChoice(m_lfoDest, kLFODestLabels, kLFODestCount);
            grid->Add(m_lfoDest, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoShape = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
            FillChoice(m_lfoShape, kLFOShapeLabels, kLFOShapeCount);
            grid->Add(m_lfoShape, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Period"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoPeriod = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                         wxSP_ARROW_KEYS, -1000000, 10000000, 0);
            grid->Add(m_lfoPeriod, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "DC Feed"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoDCFeed = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                         wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(m_lfoDCFeed, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoLevel = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                        wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(m_lfoLevel, 0);
            lfoBox->Add(grid, 0, wxALL, 4);

            m_lfoGraph = new LFOWaveformGraphPanel(lfoBox->GetStaticBox());
            lfoBox->Add(m_lfoGraph, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

            lfoBox->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "LFO Envelope (selected LFO)"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
            m_lfoAdsrGraph = new ADSRGraphPanel(lfoBox->GetStaticBox());
            m_lfoAdsrGraph->SetMinSize(wxSize(-1, 72));
            lfoBox->Add(m_lfoAdsrGraph, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

            if (m_extInfo.lfoCount > 0) {
                m_currentLfoIndex = 0;
                LoadLFOIntoUI(0);
            }
            RefreshLFOGraph();
            RefreshLFOEnvelopeGraph();

            m_lfoSelector->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
                int sel = m_lfoSelector->GetSelection();
                if (m_currentLfoIndex >= 0) {
                    SaveLFOFromUI(m_currentLfoIndex);
                }
                m_currentLfoIndex = sel;
                if (sel >= 0) LoadLFOIntoUI(sel);
                RefreshLFOGraph();
                RefreshLFOEnvelopeGraph();
                RefreshFilterEnvelopeGraph();
            });

            m_lfoCountSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) {
                int count = m_lfoCountSpin->GetValue();
                if (count <= 0) {
                    m_currentLfoIndex = -1;
                    m_lfoSelector->SetSelection(wxNOT_FOUND);
                } else {
                    int sel = m_lfoSelector->GetSelection();
                    if (sel < 0 || sel >= count) {
                        sel = 0;
                        m_lfoSelector->SetSelection(sel);
                    }
                    m_currentLfoIndex = sel;
                    LoadLFOIntoUI(sel);
                }
                RefreshLFOGraph();
                RefreshLFOEnvelopeGraph();
                RefreshFilterEnvelopeGraph();
            });

            auto lfoChanged = [this](wxCommandEvent &) {
                int sel = (m_currentLfoIndex >= 0) ? m_currentLfoIndex : m_lfoSelector->GetSelection();
                if (sel >= 0) {
                    SaveLFOFromUI(sel);
                }
                RefreshLFOGraph();
            };
            m_lfoDest->Bind(wxEVT_CHOICE, lfoChanged);
            m_lfoShape->Bind(wxEVT_CHOICE, lfoChanged);
            m_lfoPeriod->Bind(wxEVT_SPINCTRL, lfoChanged);
            m_lfoDCFeed->Bind(wxEVT_SPINCTRL, lfoChanged);
            m_lfoLevel->Bind(wxEVT_SPINCTRL, lfoChanged);
        }

        if (lpfBox) {
            sizer->Add(lpfBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        }
        if (lfoBox && adsrBox) {
            wxBoxSizer *modulationRow = new wxBoxSizer(wxHORIZONTAL);
            modulationRow->Add(lfoBox, 1, wxEXPAND | wxRIGHT, 6);
            modulationRow->Add(adsrBox, 1, wxEXPAND | wxLEFT, 6);
            sizer->Add(modulationRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        } else if (lfoBox) {
            sizer->Add(lfoBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        } else if (adsrBox) {
            sizer->Add(adsrBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
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
            RefreshLFOGraph();
            RefreshLFOEnvelopeGraph();
            return;
        }
        BAERmfEditorLFOInfo const &lfo = m_extInfo.lfos[idx];
        m_lfoDest->SetSelection(FindLabelIndex(kLFODestLabels, kLFODestCount, lfo.destination));
        m_lfoShape->SetSelection(FindLabelIndex(kLFOShapeLabels, kLFOShapeCount, lfo.waveShape));
        m_lfoPeriod->SetValue(lfo.period);
        m_lfoDCFeed->SetValue(lfo.DC_feed);
        m_lfoLevel->SetValue(lfo.level);
        RefreshLFOGraph();
        RefreshLFOEnvelopeGraph();
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

    void RefreshADSRGraph() {
        int count = m_adsrStageSpin->GetValue();
        int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
        for (int i = 0; i < count && i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            levels[i] = m_adsrLevel[i]->GetValue();
            times[i] = (int32_t)(m_adsrTime[i]->GetValue() * 1000000.0);
            int sel = m_adsrFlags[i]->GetSelection();
            flags[i] = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
        }
        m_adsrGraph->SetEnvelope(count, levels, times, flags);
        RefreshFilterEnvelopeGraph();
    }

    void RefreshLFOGraph() {
        int shapeSel;
        int32_t shape;

        if (!m_lfoGraph) {
            return;
        }
        shapeSel = m_lfoShape ? m_lfoShape->GetSelection() : 0;
        shape = (shapeSel >= 0 && shapeSel < kLFOShapeCount) ? kLFOShapeLabels[shapeSel].value : (int32_t)FOUR_CHAR('S','I','N','E');
        m_lfoGraph->SetLFO(shape,
                           m_lfoPeriod ? m_lfoPeriod->GetValue() : 0,
                           m_lfoDCFeed ? m_lfoDCFeed->GetValue() : 0,
                           m_lfoLevel ? m_lfoLevel->GetValue() : 0);
    }

    void RefreshLFOEnvelopeGraph() {
        int idx;
        int count;
        int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];

        if (!m_lfoAdsrGraph) {
            return;
        }
        idx = (m_currentLfoIndex >= 0) ? m_currentLfoIndex : (m_lfoSelector ? m_lfoSelector->GetSelection() : -1);
        if (idx < 0 || idx >= (int)m_extInfo.lfoCount) {
            m_lfoAdsrGraph->SetEnvelope(0, levels, times, flags);
            return;
        }

        count = std::clamp((int)m_extInfo.lfos[idx].adsr.stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
        for (int i = 0; i < count; ++i) {
            levels[i] = m_extInfo.lfos[idx].adsr.stages[i].level;
            times[i] = m_extInfo.lfos[idx].adsr.stages[i].time;
            flags[i] = m_extInfo.lfos[idx].adsr.stages[i].flags;
        }
        m_lfoAdsrGraph->SetEnvelope(count, levels, times, flags);
    }

    void RefreshFilterEnvelopeGraph() {
        int count;
        int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t baseFreq;
        int32_t amount;
        int64_t sourceLevels[BAE_EDITOR_MAX_ADSR_STAGES];
        int64_t maxAbsLevel;
        BAERmfEditorADSRInfo const *lpfSourceAdsr;

        if (!m_filterGraph || !m_adsrStageSpin) {
            return;
        }
        baseFreq = m_lpfFrequency ? m_lpfFrequency->GetValue() : 0;
        amount = m_lpfLowpassAmount ? m_lpfLowpassAmount->GetValue() : 0;
        lpfSourceAdsr = nullptr;

        for (uint32_t i = 0; i < m_extInfo.lfoCount; ++i) {
            int32_t dest = m_extInfo.lfos[i].destination;
            if ((dest == (int32_t)FOUR_CHAR('L','P','F','R') ||
                 dest == (int32_t)FOUR_CHAR('L','P','A','M')) &&
                m_extInfo.lfos[i].adsr.stageCount > 0) {
                lpfSourceAdsr = &m_extInfo.lfos[i].adsr;
                break;
            }
        }

        if (lpfSourceAdsr) {
            count = std::clamp((int)lpfSourceAdsr->stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
            for (int i = 0; i < count; ++i) {
                sourceLevels[i] = lpfSourceAdsr->stages[i].level;
                times[i] = lpfSourceAdsr->stages[i].time;
                flags[i] = lpfSourceAdsr->stages[i].flags;
            }
        } else {
            count = std::clamp(m_adsrStageSpin->GetValue(), 0, BAE_EDITOR_MAX_ADSR_STAGES);
            for (int i = 0; i < count; ++i) {
                sourceLevels[i] = m_adsrLevel[i] ? (int64_t)m_adsrLevel[i]->GetValue() : 0;
                times[i] = (int32_t)((m_adsrTime[i] ? m_adsrTime[i]->GetValue() : 0.0) * 1000000.0);
                {
                    int sel = m_adsrFlags[i] ? m_adsrFlags[i]->GetSelection() : 0;
                    flags[i] = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
                }
            }
        }

        maxAbsLevel = 0;
        for (int i = 0; i < count; ++i) {
            maxAbsLevel = std::max<int64_t>(maxAbsLevel, std::llabs(sourceLevels[i]));
        }
        if (maxAbsLevel == 0) {
            maxAbsLevel = 4096;
        }

        for (int i = 0; i < count; ++i) {
            int64_t cutoff = (int64_t)baseFreq + (sourceLevels[i] * (int64_t)amount) / maxAbsLevel;
            cutoff = std::clamp<int64_t>(cutoff, INT32_MIN, INT32_MAX);
            levels[i] = (int32_t)cutoff;
        }
        m_filterGraph->SetEnvelope(count, levels, times, flags);
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
            m_extInfo.volumeADSR.stages[i].time = (int32_t)(m_adsrTime[i]->GetValue() * 1000000.0);
            int sel = m_adsrFlags[i]->GetSelection();
            m_extInfo.volumeADSR.stages[i].flags = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
        }

        /* LFOs - save current one being edited */
        int lfoSel = (m_currentLfoIndex >= 0) ? m_currentLfoIndex : m_lfoSelector->GetSelection();
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
        grid->Add(new wxStaticText(page, wxID_ANY, "Sample Rate (Hz)"), 0, wxALIGN_CENTER_VERTICAL);
        m_sampleRateSpin = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22050);
        grid->Add(m_sampleRateSpin, 0);
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
            m_codecChoice = new wxChoice(page, wxID_ANY);
            m_codecChoice->Append("Don't Change");
            m_codecChoice->Append("RAW PCM");
            m_codecChoice->Append("ADPCM");
            m_codecChoice->Append("MP3");
            m_codecChoice->Append("VORBIS");
            m_codecChoice->Append("FLAC");
            m_codecChoice->Append("OPUS");
            m_codecChoice->Append("OPUS (Round-Trip)");
            row->Add(m_codecChoice, 0);
            row->Add(new wxStaticText(page, wxID_ANY, "Bitrate"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
            m_bitrateChoice = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(130, -1));
            m_bitrateChoice->Append("---");
            m_bitrateChoice->SetSelection(0);
            m_bitrateChoice->Enable(false);
            row->Add(m_bitrateChoice, 0);

            row->Add(new wxStaticText(page, wxID_ANY, "Opus Mode"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
            m_opusModeChoice = new wxChoice(page, wxID_ANY);
            m_opusModeChoice->Append("Audio");
            m_opusModeChoice->Append("Voice");
            m_opusModeChoice->SetSelection(0);
            m_opusModeChoice->Enable(false);
            row->Add(m_opusModeChoice, 0);

            row->AddStretchSpacer();
            m_codecLabel = new wxStaticText(page, wxID_ANY, "");
            row->Add(m_codecLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

            sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
            m_codecChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
                UpdateBitrateChoice(m_codecChoice->GetSelection());
                if (m_opusModeChoice) {
                    int opusSel = m_codecChoice->GetSelection();
                    m_opusModeChoice->Enable(opusSel == 6 || opusSel == 7);
                }

            });
        }

        /* Storage type (ESND/CSND/SND) */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(page, wxID_ANY, "Storage"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_sndStorageChoice = new wxChoice(page, wxID_ANY);
            m_sndStorageChoice->Append("ESND (encrypted)");
            m_sndStorageChoice->Append("CSND (LZSS compressed)");
            m_sndStorageChoice->Append("SND (plain)");
            m_sndStorageChoice->SetSelection(0);
            row->Add(m_sndStorageChoice, 0);
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
        m_waveformPanel->SetLoopChangedCallback([this](uint32_t start, uint32_t end) {
            if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) {
                return;
            }

            m_loadingUiValues = true;
            m_loopEnableCheck->SetValue(end > start);
            m_loopStartSpin->Enable(end > start);
            m_loopEndSpin->Enable(end > start);
            m_loopStartSpin->SetValue((int)start);
            m_loopEndSpin->SetValue((int)end);
            m_loadingUiValues = false;

            if (end > start) {
                ApplyLoopFromUI();
            } else {
                EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
                s.sampleInfo.startLoop = 0;
                s.sampleInfo.endLoop = 0;
                UpdateLoopInfoLabel(s);
            }
            CommitLoopChangeToUndo();
        });
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
        m_loopStartSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_loopEndSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_rootSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshPianoRangeFromRootUI(); });
        m_rootSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { RefreshPianoRangeFromRootUI(); });
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
        s.sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)(22050u << 16);
        s.compressionType = BAE_EDITOR_COMPRESSION_PCM;
        s.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        s.opusRoundTripResample = false;
        s.sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
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
        uint32_t primaryInstID = 0;
        BAERmfEditorSampleInfo primaryInfo;
        if (!m_document ||
            BAERmfEditorDocument_GetSampleInfo(m_document, primarySampleIndex, &primaryInfo) != BAE_NO_ERROR ||
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) != BAE_NO_ERROR) return;

        BAERmfEditorDocument_GetInstIDForSample(m_document, primarySampleIndex, &primaryInstID);

        for (uint32_t i = 0; i < sampleCount; i++) {
            BAERmfEditorSampleInfo info;
            uint32_t instID = 0;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR) continue;
            BAERmfEditorDocument_GetInstIDForSample(m_document, i, &instID);
            if ((primaryInstID != 0 && instID == primaryInstID) ||
                (primaryInstID == 0 && info.program == primaryInfo.program)) {
                EditedSample edited;
                edited.displayName = SanitizeDisplayName(info.displayName ? wxString::FromUTF8(info.displayName) : wxString());
                edited.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
                edited.program = info.program;
                edited.rootKey = NormalizeRootKeyForSingleKeySplit(info.rootKey, info.lowKey, info.highKey);
                edited.lowKey = info.lowKey;
                edited.highKey = info.highKey;
                edited.splitVolume = info.splitVolume;
                edited.sampleInfo = info.sampleInfo;
                edited.compressionType = info.compressionType;
                edited.hasOriginalData = (info.hasOriginalData == TRUE);
                edited.sndStorageType = info.sndStorageType;
                edited.opusMode = info.opusMode;
                edited.opusRoundTripResample = (info.opusRoundTripResample == TRUE);
                m_sampleIndices.push_back(i);
                m_samples.push_back(edited);
            }
        }
    }

    wxString BuildSplitLabel(int localIndex) const {
        EditedSample const &s = m_samples[(size_t)localIndex];
        return wxString::Format("%d-%d: %s", (int)s.lowKey, (int)s.highKey, s.displayName);
    }

    /* Returns {codec_idx, bitrate_idx} for the two-dropdown UI */
    static std::pair<int,int> CompressionTypeToCodecBitrate(BAERmfEditorCompressionType t) {
        switch (t) {
            case BAE_EDITOR_COMPRESSION_DONT_CHANGE: return {0, 0};
            case BAE_EDITOR_COMPRESSION_PCM:         return {1, 0};
            case BAE_EDITOR_COMPRESSION_ADPCM:       return {2, 0};
            case BAE_EDITOR_COMPRESSION_MP3_32K:     return {3, 0};
            case BAE_EDITOR_COMPRESSION_MP3_48K:     return {3, 1};
            case BAE_EDITOR_COMPRESSION_MP3_64K:     return {3, 2};
            case BAE_EDITOR_COMPRESSION_MP3_96K:     return {3, 3};
            case BAE_EDITOR_COMPRESSION_MP3_128K:    return {3, 4};
            case BAE_EDITOR_COMPRESSION_MP3_192K:    return {3, 5};
            case BAE_EDITOR_COMPRESSION_MP3_256K:    return {3, 6};
            case BAE_EDITOR_COMPRESSION_MP3_320K:    return {3, 7};
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:  return {4, 0};
            case BAE_EDITOR_COMPRESSION_VORBIS_48K:  return {4, 1};
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:  return {4, 2};
            case BAE_EDITOR_COMPRESSION_VORBIS_80K:  return {4, 3};
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:  return {4, 4};
            case BAE_EDITOR_COMPRESSION_VORBIS_128K: return {4, 5};
            case BAE_EDITOR_COMPRESSION_VORBIS_160K: return {4, 6};
            case BAE_EDITOR_COMPRESSION_VORBIS_192K: return {4, 7};
            case BAE_EDITOR_COMPRESSION_VORBIS_256K: return {4, 8};
            case BAE_EDITOR_COMPRESSION_FLAC:        return {5, 0};
            case BAE_EDITOR_COMPRESSION_OPUS_12K:    return {6, 0};
            case BAE_EDITOR_COMPRESSION_OPUS_16K:    return {6, 1};
            case BAE_EDITOR_COMPRESSION_OPUS_24K:    return {6, 2};
            case BAE_EDITOR_COMPRESSION_OPUS_32K:    return {6, 3};
            case BAE_EDITOR_COMPRESSION_OPUS_48K:    return {6, 4};
            case BAE_EDITOR_COMPRESSION_OPUS_64K:    return {6, 5};
            case BAE_EDITOR_COMPRESSION_OPUS_96K:    return {6, 6};
            case BAE_EDITOR_COMPRESSION_OPUS_128K:   return {6, 7};
            case BAE_EDITOR_COMPRESSION_OPUS_256K:   return {6, 8};
            default:                                  return {1, 0};
        }
    }

    static BAERmfEditorCompressionType CodecBitrateToCompressionType(int codec, int bitrate) {
        switch (codec) {
            case 0: return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
            case 1: return BAE_EDITOR_COMPRESSION_PCM;
            case 2: return BAE_EDITOR_COMPRESSION_ADPCM;
            case 3: // MP3
                switch (bitrate) {
                    case 0: return BAE_EDITOR_COMPRESSION_MP3_32K;
                    case 1: return BAE_EDITOR_COMPRESSION_MP3_48K;
                    case 2: return BAE_EDITOR_COMPRESSION_MP3_64K;
                    case 3: return BAE_EDITOR_COMPRESSION_MP3_96K;
                    case 4: return BAE_EDITOR_COMPRESSION_MP3_128K;
                    case 5: return BAE_EDITOR_COMPRESSION_MP3_192K;
                    case 6: return BAE_EDITOR_COMPRESSION_MP3_256K;
                    case 7: return BAE_EDITOR_COMPRESSION_MP3_320K;
                    default: return BAE_EDITOR_COMPRESSION_MP3_128K;
                }
            case 4: // VORBIS
                switch (bitrate) {
                    case 0: return BAE_EDITOR_COMPRESSION_VORBIS_32K;
                    case 1: return BAE_EDITOR_COMPRESSION_VORBIS_48K;
                    case 2: return BAE_EDITOR_COMPRESSION_VORBIS_64K;
                    case 3: return BAE_EDITOR_COMPRESSION_VORBIS_80K;
                    case 4: return BAE_EDITOR_COMPRESSION_VORBIS_96K;
                    case 5: return BAE_EDITOR_COMPRESSION_VORBIS_128K;
                    case 6: return BAE_EDITOR_COMPRESSION_VORBIS_160K;
                    case 7: return BAE_EDITOR_COMPRESSION_VORBIS_192K;
                    case 8: return BAE_EDITOR_COMPRESSION_VORBIS_256K;
                    default: return BAE_EDITOR_COMPRESSION_VORBIS_128K;
                }
            case 5: return BAE_EDITOR_COMPRESSION_FLAC;
            case 7: // OPUS (Round-Trip) – same underlying compression types
            case 6: // OPUS
                switch (bitrate) {
                    case 0: return BAE_EDITOR_COMPRESSION_OPUS_12K;
                    case 1: return BAE_EDITOR_COMPRESSION_OPUS_16K;
                    case 2: return BAE_EDITOR_COMPRESSION_OPUS_24K;
                    case 3: return BAE_EDITOR_COMPRESSION_OPUS_32K;
                    case 4: return BAE_EDITOR_COMPRESSION_OPUS_48K;
                    case 5: return BAE_EDITOR_COMPRESSION_OPUS_64K;
                    case 6: return BAE_EDITOR_COMPRESSION_OPUS_96K;
                    case 7: return BAE_EDITOR_COMPRESSION_OPUS_128K;
                    case 8: return BAE_EDITOR_COMPRESSION_OPUS_256K;
                    default: return BAE_EDITOR_COMPRESSION_OPUS_48K;
                }
            default: return BAE_EDITOR_COMPRESSION_PCM;
        }
    }

    void UpdateBitrateChoice(int codecIdx) {
        m_bitrateChoice->Clear();
        switch (codecIdx) {
            case 3: // MP3
                m_bitrateChoice->Append("32k");
                m_bitrateChoice->Append("48k");
                m_bitrateChoice->Append("64k");
                m_bitrateChoice->Append("96k");
                m_bitrateChoice->Append("128k");
                m_bitrateChoice->Append("192k");
                m_bitrateChoice->Append("256k");
                m_bitrateChoice->Append("320k");
                m_bitrateChoice->SetSelection(0);
                m_bitrateChoice->Enable(true);
                break;
            case 4: // VORBIS
                m_bitrateChoice->Append("32k");
                m_bitrateChoice->Append("48k");
                m_bitrateChoice->Append("64k");
                m_bitrateChoice->Append("80k");
                m_bitrateChoice->Append("96k");
                m_bitrateChoice->Append("128k");
                m_bitrateChoice->Append("160k");
                m_bitrateChoice->Append("192k");
                m_bitrateChoice->Append("256k");
                m_bitrateChoice->SetSelection(0);
                m_bitrateChoice->Enable(true);
                break;
            case 7: // OPUS (Round-Trip) – same bitrates as OPUS
            case 6: // OPUS
                m_bitrateChoice->Append("12k");
                m_bitrateChoice->Append("16k");
                m_bitrateChoice->Append("24k");
                m_bitrateChoice->Append("32k");
                m_bitrateChoice->Append("48k");
                m_bitrateChoice->Append("64k");
                m_bitrateChoice->Append("96k");
                m_bitrateChoice->Append("128k");
                m_bitrateChoice->Append("256k");
                m_bitrateChoice->SetSelection(0);
                m_bitrateChoice->Enable(true);
                break;
            default: // Don't Change, PCM, ADPCM, FLAC – no bitrate
                m_bitrateChoice->Append("---");
                m_bitrateChoice->SetSelection(0);
                m_bitrateChoice->Enable(false);
                break;
        }
    }

    void SaveCurrentSampleFromUI() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) return;
        EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
        s.displayName = SanitizeDisplayName(m_nameText->GetValue());
        s.rootKey = (unsigned char)m_rootSpin->GetValue();
        if (m_pianoPanel) {
            m_pianoPanel->SetBaseNote(std::max(0, (int)s.rootKey - 12));
        }
        s.lowKey = (unsigned char)m_lowSpin->GetValue();
        s.highKey = (unsigned char)m_highSpin->GetValue();
        {
            uint32_t rateHz = (uint32_t)std::max(1, m_sampleRateSpin->GetValue());
            s.sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)(rateHz << 16);
        }
        s.splitVolume = (int16_t)m_splitVolumeSpin->GetValue();
        /* Program comes from the instrument tab */
        s.program = (unsigned char)m_instProgramSpin->GetValue();
        int codecIdx = m_codecChoice->GetSelection();
        int bitrateIdx = m_bitrateChoice->IsEnabled() ? m_bitrateChoice->GetSelection() : 0;
        BAERmfEditorCompressionType chosen = CodecBitrateToCompressionType(codecIdx, bitrateIdx);
        if (chosen == BAE_EDITOR_COMPRESSION_DONT_CHANGE && !s.hasOriginalData) chosen = BAE_EDITOR_COMPRESSION_PCM;
        s.compressionType = chosen;
        s.opusRoundTripResample = (codecIdx == 7);
        {
            int sel = m_opusModeChoice->GetSelection();
            if (sel == 1)      s.opusMode = BAE_EDITOR_OPUS_MODE_VOICE;
            else               s.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        }
        {
            int sel = m_sndStorageChoice->GetSelection();
            if (sel == 1)      s.sndStorageType = BAE_EDITOR_SND_STORAGE_CSND;
            else if (sel == 2) s.sndStorageType = BAE_EDITOR_SND_STORAGE_SND;
            else               s.sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
        }

        if (m_loopEnableCheck->GetValue()) {
            ApplyLoopFromUI();
        } else {
            s.sampleInfo.startLoop = 0;
            s.sampleInfo.endLoop = 0;
        }

        if (m_splitChoice && m_currentLocalIndex < (int)m_splitChoice->GetCount()) {
            m_splitChoice->SetString(m_currentLocalIndex, BuildSplitLabel(m_currentLocalIndex));
        }
    }

    void LoadLocalSample(int localIndex) {
        if (localIndex < 0 || localIndex >= (int)m_samples.size()) return;
        m_loadingUiValues = true;
        m_currentLocalIndex = localIndex;
        EditedSample const &s = m_samples[(size_t)localIndex];
        if (m_pianoPanel) {
            m_pianoPanel->SetBaseNote(std::max(0, (int)s.rootKey - 12));
        }
        m_nameText->SetValue(s.displayName);
        m_rootSpin->SetValue(s.rootKey);
        m_lowSpin->SetValue(s.lowKey);
        m_highSpin->SetValue(s.highKey);
        {
            int rateHz = (int)(s.sampleInfo.sampledRate >> 16);
            m_sampleRateSpin->SetValue(std::clamp(rateHz, 1, 65535));
        }
        m_splitVolumeSpin->SetValue(std::clamp<int>(s.splitVolume, 0, 32767));
        {
            bool canKeep = s.hasOriginalData;
            BAERmfEditorCompressionType effectiveComp = s.compressionType;
            if (!canKeep && effectiveComp == BAE_EDITOR_COMPRESSION_DONT_CHANGE) effectiveComp = BAE_EDITOR_COMPRESSION_PCM;
            auto [codecIdx, bitrateIdx] = CompressionTypeToCodecBitrate(effectiveComp);
            /* For Round-Trip Opus, promote codec idx from 6 to 7. */
            if (codecIdx == 6 && s.opusRoundTripResample) codecIdx = 7;
            m_codecChoice->SetSelection(codecIdx);
            m_codecChoice->SetString(0, canKeep ? "Don't Change" : "Don't Change (N/A)");
            UpdateBitrateChoice(codecIdx);
            if (bitrateIdx >= 0 && bitrateIdx < (int)m_bitrateChoice->GetCount()) {
                m_bitrateChoice->SetSelection(bitrateIdx);
            }
            if (m_opusModeChoice) {
                int opusModeSel = 0;
                if (s.opusMode == BAE_EDITOR_OPUS_MODE_AUDIO) opusModeSel = 0;
                else if (s.opusMode == BAE_EDITOR_OPUS_MODE_VOICE) opusModeSel = 1;
                m_opusModeChoice->SetSelection(opusModeSel);
                m_opusModeChoice->Enable(codecIdx == 6 || codecIdx == 7);
            }
        }
        {
            int sel = 0;
            if (s.sndStorageType == BAE_EDITOR_SND_STORAGE_CSND)      sel = 1;
            else if (s.sndStorageType == BAE_EDITOR_SND_STORAGE_SND)  sel = 2;
            m_sndStorageChoice->SetSelection(sel);
        }
        {
            char codecBuf[64] = {};
            uint32_t si = m_sampleIndices[(size_t)localIndex];
            if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, si, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
                m_codecLabel->SetLabel(wxString::Format("Source: %s", wxString::FromUTF8(codecBuf)));
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
        m_loadingUiValues = false;
    }

    void ClampRange() {
        if (m_lowSpin->GetValue() > m_highSpin->GetValue()) m_highSpin->SetValue(m_lowSpin->GetValue());
    }

    void RefreshPianoRangeFromRootUI() {
        if (!m_pianoPanel || !m_rootSpin) return;
        m_pianoPanel->SetBaseNote(std::max(0, m_rootSpin->GetValue() - 12));
    }

    void CommitLoopChangeToUndo() {
        if (m_loadingUiValues || m_inInstantApply) {
            return;
        }
        wxCommandEvent evt;
        OnApply(evt);
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
        CommitLoopChangeToUndo();
    }

    void OnLoopPointChanged() {
        if (m_loopEnableCheck->GetValue()) {
            ApplyLoopFromUI();
            CommitLoopChangeToUndo();
        }
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
            s.sndStorageType = info.sndStorageType;
            s.opusMode = info.opusMode;
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
        if (m_loadingUiValues || m_inInstantApply) {
            return;
        }
        m_inInstantApply = true;
        if (m_beginUndoCallback) {
            m_beginUndoCallback("Edit Instrument");
        }
        if (!ApplyInstrumentChanges(true)) {
            if (m_cancelUndoCallback) {
                m_cancelUndoCallback();
            }
            m_inInstantApply = false;
            return;
        }
        SaveCurrentSampleFromUI();
        /* Update program for all samples in the group */
        unsigned char prog = (unsigned char)m_instProgramSpin->GetValue();
        for (auto &s : m_samples) s.program = prog;
        for (size_t i = 0; i < m_samples.size() && i < m_sampleIndices.size(); ++i) {
            BAERmfEditorSampleInfo info;
            uint32_t sampleIndex = m_sampleIndices[i];
            wxScopedCharBuffer nameUtf8 = m_samples[i].displayName.utf8_str();

            if (sampleIndex == static_cast<uint32_t>(-1)) {
                continue;
            }
            info.displayName = const_cast<char *>(nameUtf8.data());
            info.sourcePath = NULL;
            info.program = m_samples[i].program;
            info.rootKey = m_samples[i].rootKey;
            info.lowKey = m_samples[i].lowKey;
            info.highKey = m_samples[i].highKey;
            info.splitVolume = m_samples[i].splitVolume;
            info.sampleInfo = m_samples[i].sampleInfo;
            info.compressionType = m_samples[i].compressionType;
            info.hasOriginalData = m_samples[i].hasOriginalData ? TRUE : FALSE;
            info.sndStorageType = m_samples[i].sndStorageType;
            info.opusMode = m_samples[i].opusMode;
            info.opusRoundTripResample = m_samples[i].opusRoundTripResample ? TRUE : FALSE;
            if (BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndex, &info) != BAE_NO_ERROR) {
                if (m_cancelUndoCallback) {
                    m_cancelUndoCallback();
                }
                m_inInstantApply = false;
                return;
            }
        }
        if (m_commitUndoCallback) {
            m_commitUndoCallback("Edit Instrument");
        }
        m_inInstantApply = false;
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
    std::function<void(wxString const &)> beginUndoCallback,
    std::function<void(wxString const &)> commitUndoCallback,
    std::function<void()> cancelUndoCallback,
    std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char, BAERmfEditorCompressionType, bool)> playCallback,
    std::function<void()> stopCallback,
    std::function<bool(uint32_t, wxString const &)> replaceCallback,
    std::function<bool(uint32_t, wxString const &)> exportCallback,
    std::vector<uint32_t> *outDeletedSampleIndices,
    std::vector<uint32_t> *outSampleIndices,
    std::vector<InstrumentEditorEditedSample> *outEditedSamples) {

    InstrumentExtEditorDialog dialog(parent, document, primarySampleIndex,
                                     initialExtInfo,
                                     std::move(beginUndoCallback),
                                     std::move(commitUndoCallback),
                                     std::move(cancelUndoCallback),
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
