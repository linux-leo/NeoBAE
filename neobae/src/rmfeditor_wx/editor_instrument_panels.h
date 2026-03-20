/****************************************************************************
 *
 * editor_instrument_panels.h
 *
 * Reusable instrument editor panel widgets shared between the instrument
 * editor dialog and the bank editor panel.
 *
 * Contains:
 *   - FourCharLabel helpers and ADSR/LFO label tables
 *   - ADSRGraphPanel       (envelope visualisation)
 *   - LFOWaveformGraphPanel (LFO waveform visualisation)
 *   - WaveformPanelExt     (sample waveform display with loop editing)
 *   - PianoKeyboardPanel   (interactive piano keyboard for preview)
 *
 ****************************************************************************/

#ifndef EDITOR_INSTRUMENT_PANELS_H
#define EDITOR_INSTRUMENT_PANELS_H

#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <functional>
#include <set>

#include <wx/dcbuffer.h>
#include <wx/timer.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
}

/* ====================================================================== */
/*  FOUR_CHAR helpers for readable dropdown labels                        */
/* ====================================================================== */

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

static inline int FindLabelIndex(FourCharLabel const *table, int count, int32_t value) {
    for (int i = 0; i < count; i++) {
        if (table[i].value == value) return i;
    }
    return 0;
}

static inline void FillChoice(wxChoice *choice, FourCharLabel const *table, int count) {
    for (int i = 0; i < count; i++) {
        choice->Append(table[i].label);
    }
}

static inline char const *FourCharFlagName(int32_t flags) {
    if (flags == (int32_t)FOUR_CHAR('L','I','N','E')) return "LINEAR_RAMP";
    if (flags == (int32_t)FOUR_CHAR('S','U','S','T')) return "SUSTAIN";
    if (flags == (int32_t)FOUR_CHAR('L','A','S','T')) return "TERMINATE";
    if (flags == (int32_t)FOUR_CHAR('G','O','T','O')) return "GOTO";
    if (flags == (int32_t)FOUR_CHAR('G','O','S','T')) return "GOTO_COND";
    if (flags == (int32_t)FOUR_CHAR('R','E','L','S')) return "RELEASE";
    return "OFF";
}

static inline char const *LFODestName(int32_t dest) {
    for (int i = 0; i < kLFODestCount; i++) {
        if (kLFODestLabels[i].value == dest) return kLFODestLabels[i].label;
    }
    return "UNKNOWN";
}

static inline char const *LFOShapeName(int32_t shape) {
    for (int i = 0; i < kLFOShapeCount; i++) {
        if (kLFOShapeLabels[i].value == shape) return kLFOShapeLabels[i].label;
    }
    return "UNKNOWN";
}

/* ====================================================================== */
/*  WaveformPanelExt                                                       */
/* ====================================================================== */

class WaveformPanelExt : public wxPanel {
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
/*  ADSR Envelope graph panel                                              */
/* ====================================================================== */

class ADSRGraphPanel : public wxPanel {
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
/*  LFO waveform graph panel                                               */
/* ====================================================================== */

class LFOWaveformGraphPanel : public wxPanel {
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
/*  Piano keyboard panel                                                   */
/* ====================================================================== */

class PianoKeyboardPanel : public wxPanel {
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
        m_externalPressedKeys = {clamped};
        Refresh();
    }

    void ClearExternalPressedNote() {
        if (!m_externalPressedKeys.empty()) {
            m_externalPressedKeys.clear();
            Refresh();
        }
    }

    void SetExternalPressedNotes(std::set<int> const &notes) {
        m_externalPressedKeys = notes;
        Refresh();
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
    std::set<int> m_externalPressedKeys;
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
        if (note < m_baseNote || note > end || IsBlackKey(note % 12)) return false;
        int wi = WhiteIndexForNote(note);
        *x1 = (int)(wi * whiteKeyWidth);
        *x2 = (int)((wi + 1) * whiteKeyWidth);
        return true;
    }

    void StopPressedNote() {
        if (m_noteIsOn) {
            SafeInvokeNoteOff();
            m_noteIsOn = false;
        }
        if (m_releaseWatchdog.IsRunning()) {
            m_releaseWatchdog.Stop();
        }
        m_pressedKey = -1;
    }

    int NoteFromPosition(wxPoint const &pos) const {
        wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return -1;
        int end = VisibleEndNote();
        int whiteCount = 0;
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) ++whiteCount;
        }
        if (whiteCount <= 0) return -1;
        float whiteKeyWidth = (float)size.x / (float)whiteCount;
        int blackKeyHeight = (int)(size.y * 0.6f);

        /* Check black keys first (they overlap white keys) */
        if (pos.y < blackKeyHeight) {
            for (int n = m_baseNote; n <= end; ++n) {
                if (!IsBlackKey(n % 12)) continue;
                int prevWhite = WhiteIndexForNote(n);
                float bx = prevWhite * whiteKeyWidth + whiteKeyWidth * 0.65f;
                float bw = whiteKeyWidth * 0.7f;
                if (pos.x >= (int)bx && pos.x < (int)(bx + bw)) return n;
            }
        }
        /* White keys */
        for (int n = m_baseNote; n <= end; ++n) {
            if (IsBlackKey(n % 12)) continue;
            int wx1, wx2;
            if (WhiteKeyRectForNote(n, size, whiteKeyWidth, &wx1, &wx2)) {
                if (pos.x >= wx1 && pos.x < wx2) return n;
            }
        }
        return -1;
    }

    void OnMouseDown(wxMouseEvent &event) {
        int note = NoteFromPosition(event.GetPosition());
        if (note < 0) return;
        StopPressedNote();
        m_pressedKey = note;
        if (SafeInvokeNoteOn(note)) {
            m_noteIsOn = true;
            m_releaseWatchdog.StartOnce(5000);
        }
        if (!HasCapture()) CaptureMouse();
        Refresh();
    }

    void OnMouseUp(wxMouseEvent &) {
        StopPressedNote();
        if (HasCapture()) ReleaseMouse();
        Refresh();
    }

    void OnMouseMove(wxMouseEvent &event) {
        if (!event.LeftIsDown() || m_pressedKey < 0) return;
        int note = NoteFromPosition(event.GetPosition());
        if (note >= 0 && note != m_pressedKey) {
            StopPressedNote();
            m_pressedKey = note;
            if (SafeInvokeNoteOn(note)) {
                m_noteIsOn = true;
                m_releaseWatchdog.StartOnce(5000);
            }
            Refresh();
        }
    }

    void OnMouseLeave(wxMouseEvent &) {
        if (m_pressedKey >= 0 && !HasCapture()) {
            StopPressedNote();
            Refresh();
        }
    }

    void OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
        StopPressedNote();
        Refresh();
    }

    void OnMouseCaptureChanged(wxMouseCaptureChangedEvent &) {
        StopPressedNote();
        Refresh();
    }

    void OnReleaseWatchdog(wxTimerEvent &) {
        StopPressedNote();
        if (HasCapture()) ReleaseMouse();
        Refresh();
    }

    void OnSize(wxSizeEvent &event) {
        UpdateVisibleRangeForWidth(event.GetSize().x);
        event.Skip();
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(wxColour(40, 40, 40)));
        dc.Clear();
        if (size.x <= 0 || size.y <= 0) return;

        int end = VisibleEndNote();
        int whiteCount = 0;
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) ++whiteCount;
        }
        if (whiteCount <= 0) return;
        float whiteKeyWidth = (float)size.x / (float)whiteCount;
        int blackKeyHeight = (int)(size.y * 0.6f);

        /* Draw white keys */
        for (int n = m_baseNote; n <= end; ++n) {
            if (IsBlackKey(n % 12)) continue;
            int wx1, wx2;
            if (!WhiteKeyRectForNote(n, size, whiteKeyWidth, &wx1, &wx2)) continue;
            bool pressed = (n == m_pressedKey) || (m_externalPressedKeys.count(n) > 0);
            dc.SetBrush(wxBrush(pressed ? wxColour(180, 220, 255) : wxColour(245, 245, 245)));
            dc.SetPen(wxPen(wxColour(100, 100, 100), 1));
            dc.DrawRectangle(wx1, 0, wx2 - wx1, size.y);
        }
        /* Draw black keys */
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) continue;
            int prevWhite = WhiteIndexForNote(n);
            float bx = prevWhite * whiteKeyWidth + whiteKeyWidth * 0.65f;
            float bw = whiteKeyWidth * 0.7f;
            bool pressed = (n == m_pressedKey) || (m_externalPressedKeys.count(n) > 0);
            dc.SetBrush(wxBrush(pressed ? wxColour(100, 140, 200) : wxColour(30, 30, 30)));
            dc.SetPen(wxPen(wxColour(20, 20, 20), 1));
            dc.DrawRectangle((int)bx, 0, (int)bw, blackKeyHeight);
        }
    }
};

#endif /* EDITOR_INSTRUMENT_PANELS_H */
