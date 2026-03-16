#include <stdint.h>

#include <algorithm>
#include <climits>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>

#include "editor_instrument_dialog.h"

namespace {

class WaveformPanel final : public wxPanel {
public:
    explicit WaveformPanel(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 180), wxBORDER_SIMPLE),
          m_waveData(nullptr),
          m_frameCount(0),
          m_bitSize(16),
          m_channels(1),
          m_loopStart(0),
          m_loopEnd(0) {
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

    void SetLoopPoints(uint32_t loopStart, uint32_t loopEnd) {
        m_loopStart = loopStart;
        m_loopEnd = loopEnd;
        Refresh();
    }

private:
    void const *m_waveData;
    uint32_t m_frameCount;
    uint16_t m_bitSize;
    uint16_t m_channels;
    uint32_t m_loopStart;
    uint32_t m_loopEnd;

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

        /* Draw loop region fill before waveform so signal renders on top. */
        bool hasLoop = (m_loopEnd > m_loopStart && m_frameCount > 0 && size.x > 2);
        if (hasLoop) {
            int xLoopStart = static_cast<int>(static_cast<int64_t>(m_loopStart) * size.x / m_frameCount);
            int xLoopEnd = static_cast<int>(static_cast<int64_t>(m_loopEnd) * size.x / m_frameCount);
            if (xLoopEnd > xLoopStart) {
                dc.SetBrush(wxBrush(wxColour(0, 55, 22)));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(xLoopStart, 0, xLoopEnd - xLoopStart, size.y);
            }
        }

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

        /* Draw loop start / end markers on top of waveform. */
        if (hasLoop) {
            int xLoopStart = static_cast<int>(static_cast<int64_t>(m_loopStart) * size.x / m_frameCount);
            int xLoopEnd = static_cast<int>(static_cast<int64_t>(m_loopEnd) * size.x / m_frameCount);

            dc.SetPen(wxPen(wxColour(0, 220, 80), 2));
            dc.DrawLine(xLoopStart, 0, xLoopStart, size.y);
            dc.SetPen(wxPen(wxColour(255, 160, 0), 2));
            dc.DrawLine(xLoopEnd, 0, xLoopEnd, size.y);

            dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
            dc.SetTextForeground(wxColour(0, 220, 80));
            dc.DrawText("S", xLoopStart + 3, 2);
            dc.SetTextForeground(wxColour(255, 160, 0));
            {
                int labelX = xLoopEnd + 3;
                if (labelX + 14 > size.x) {
                    labelX = std::max(0, xLoopEnd - 16);
                }
                dc.DrawText("E", labelX, 2);
            }
        }
    }
};

class InstrumentEditorDialog final : public wxDialog {
public:
    using EditedSample = InstrumentEditorEditedSample;

    InstrumentEditorDialog(wxWindow *parent,
                           BAERmfEditorDocument const *document,
                           uint32_t primarySampleIndex,
                           std::function<void(uint32_t, int, BAESampleInfo const *)> playCallback,
                           std::function<void()> stopCallback,
                           std::function<bool(uint32_t, wxString const &)> replaceCallback,
                           std::function<bool(uint32_t, wxString const &)> exportCallback)
        : wxDialog(parent, wxID_ANY, "Embedded Instrument", wxDefaultPosition, wxSize(640, 460), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_document(document),
          m_currentLocalIndex(-1),
          m_savedLoopStart(0),
          m_savedLoopEnd(0),
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
        m_loopEnableCheck = new wxCheckBox(this, wxID_ANY, "Loop");
        m_loopStartSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, INT_MAX, 0);
        m_loopEndSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, INT_MAX, 0);
        m_loopInfoLabel = new wxStaticText(this, wxID_ANY, "");
        m_playButton = new wxButton(this, wxID_ANY, "Play");
        m_stopButton = new wxButton(this, wxID_ANY, "Stop");
        m_replaceButton = new wxButton(this, wxID_ANY, "Replace");
        m_exportButton = new wxButton(this, wxID_ANY, "Save As...");
        m_rangeLabel = new wxStaticText(this, wxID_ANY, "");
        m_waveformPanel = new WaveformPanel(this);
        m_codecLabel = new wxStaticText(this, wxID_ANY, wxString());
        m_compressionChoice = new wxChoice(this, wxID_ANY);
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

        wxBoxSizer *loopSizer = new wxBoxSizer(wxHORIZONTAL);
        loopSizer->Add(m_loopEnableCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        loopSizer->Add(new wxStaticText(this, wxID_ANY, "Start"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        loopSizer->Add(m_loopStartSpin, 0, wxRIGHT, 10);
        loopSizer->Add(new wxStaticText(this, wxID_ANY, "End"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        loopSizer->Add(m_loopEndSpin, 0, wxRIGHT, 10);
        loopSizer->Add(m_loopInfoLabel, 0, wxALIGN_CENTER_VERTICAL);

        triggerSizer->Add(new wxStaticText(this, wxID_ANY, "Preview Key"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        triggerSizer->Add(m_triggerSpin, 0, wxRIGHT, 8);
        triggerSizer->Add(m_playButton, 0, wxRIGHT, 8);
        triggerSizer->Add(m_stopButton, 0, wxRIGHT, 8);
        triggerSizer->Add(m_replaceButton, 0, wxRIGHT, 8);
        triggerSizer->Add(m_exportButton, 0, wxRIGHT, 8);

        rootSizer->Add(instrumentSizer, 0, wxEXPAND | wxALL, 10);
        rootSizer->Add(grid, 0, wxEXPAND | wxALL, 10);
        rootSizer->Add(loopSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        {
            wxBoxSizer *compressionSizer = new wxBoxSizer(wxHORIZONTAL);
            compressionSizer->Add(new wxStaticText(this, wxID_ANY, "Compression"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            compressionSizer->Add(m_compressionChoice, 0);
            compressionSizer->Add(m_codecLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
            rootSizer->Add(compressionSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        }
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
        Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { OnLoopEnableChanged(); }, m_loopEnableCheck->GetId());
        Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { OnLoopPointChanged(); }, m_loopStartSpin->GetId());
        Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { OnLoopPointChanged(); }, m_loopEndSpin->GetId());
        Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { OnCompressionChanged(); }, m_compressionChoice->GetId());
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
    std::function<void(uint32_t, int, BAESampleInfo const *)> m_playCallback;
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
    wxCheckBox *m_loopEnableCheck;
    wxSpinCtrl *m_loopStartSpin;
    wxSpinCtrl *m_loopEndSpin;
    wxStaticText *m_loopInfoLabel;
    uint32_t m_savedLoopStart;
    uint32_t m_savedLoopEnd;
    wxButton *m_playButton;
    wxButton *m_stopButton;
    wxButton *m_replaceButton;
    wxButton *m_exportButton;
    wxStaticText *m_rangeLabel;
    WaveformPanel *m_waveformPanel;
    wxChoice *m_compressionChoice;
    wxStaticText *m_codecLabel;

    static int CompressionTypeToChoiceIndex(BAERmfEditorCompressionType t) {
        switch (t) {
            case BAE_EDITOR_COMPRESSION_DONT_CHANGE:  return 0;
            case BAE_EDITOR_COMPRESSION_PCM:          return 1;
            case BAE_EDITOR_COMPRESSION_ADPCM:        return 2;
            case BAE_EDITOR_COMPRESSION_MP3_32K:      return 3;
            case BAE_EDITOR_COMPRESSION_MP3_64K:      return 4;
            case BAE_EDITOR_COMPRESSION_MP3_96K:      return 5;
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:   return 6;
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:   return 7;
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:   return 8;
            case BAE_EDITOR_COMPRESSION_FLAC:         return 9;
            case BAE_EDITOR_COMPRESSION_OPUS_16K:     return 10;
            case BAE_EDITOR_COMPRESSION_OPUS_32K:     return 11;
            case BAE_EDITOR_COMPRESSION_OPUS_64K:     return 12;
            case BAE_EDITOR_COMPRESSION_OPUS_96K:     return 13;
            case BAE_EDITOR_COMPRESSION_OPUS_128K:    return 14;
            case BAE_EDITOR_COMPRESSION_OPUS_256K:    return 15;
            default:                                   return 1;
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
                edited.compressionType = info.compressionType;
                edited.hasOriginalData = (info.hasOriginalData == TRUE);
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
        {
            int idx = m_compressionChoice->GetSelection();
            BAERmfEditorCompressionType chosen = ChoiceIndexToCompressionType(idx);
            /* Guard: if user somehow has "Don't Change" selected without original data, fall back. */
            if (chosen == BAE_EDITOR_COMPRESSION_DONT_CHANGE && !sample.hasOriginalData) {
                chosen = BAE_EDITOR_COMPRESSION_PCM;
            }
            sample.compressionType = chosen;
        }
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
        {
            /* "Don't Change" (index 0) is only valid when original data is present. */
            bool canKeep = sample.hasOriginalData;
            m_compressionChoice->Enable(true);
            /* wxChoice doesn't support per-item enable; keep "Don't Change" selectable
             * only when canKeep, and if !canKeep and it was selected, bump to PCM. */
            BAERmfEditorCompressionType effectiveComp = sample.compressionType;
            if (!canKeep && effectiveComp == BAE_EDITOR_COMPRESSION_DONT_CHANGE) {
                effectiveComp = BAE_EDITOR_COMPRESSION_PCM;
            }
            m_compressionChoice->SetSelection(CompressionTypeToChoiceIndex(effectiveComp));
            /* Disable the "Don't Change" item text visually via choice label when unavailable.
             * We can't grey individual items in wxChoice portably, so rename it. */
            m_compressionChoice->SetString(0, canKeep ? "Don't Change" : "Don't Change (N/A)");
        }
        {
            char codecBuf[64] = {};
            uint32_t sampleIndex = m_sampleIndices[static_cast<size_t>(localIndex)];
            if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, sampleIndex, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
                m_codecLabel->SetLabel(wxString::Format("(source: %s)", wxString::FromUTF8(codecBuf)));
            } else {
                m_codecLabel->SetLabel(wxString());
            }
        }
        {
            bool loopEnabled = (sample.sampleInfo.startLoop < sample.sampleInfo.endLoop);
            int maxFrame = static_cast<int>(sample.sampleInfo.waveFrames > 0 ? sample.sampleInfo.waveFrames : INT_MAX);
            m_loopEnableCheck->SetValue(loopEnabled);
            m_loopStartSpin->SetRange(0, maxFrame);
            m_loopEndSpin->SetRange(0, maxFrame);
            m_loopStartSpin->SetValue(static_cast<int>(sample.sampleInfo.startLoop));
            m_loopEndSpin->SetValue(static_cast<int>(sample.sampleInfo.endLoop));
            m_loopStartSpin->Enable(loopEnabled);
            m_loopEndSpin->Enable(loopEnabled);
            /* Seed stash so disable->re-enable restores these values. */
            m_savedLoopStart = sample.sampleInfo.startLoop;
            m_savedLoopEnd = sample.sampleInfo.endLoop;
            UpdateLoopInfoLabel(sample);
        }
        RefreshWaveform();
    }

    void ClampRange() {
        if (m_lowSpin->GetValue() > m_highSpin->GetValue()) {
            m_highSpin->SetValue(m_lowSpin->GetValue());
        }
    }

    void OnLoopEnableChanged() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_samples.size())) {
            return;
        }
        EditedSample &sample = m_samples[static_cast<size_t>(m_currentLocalIndex)];
        bool loopEnabled = m_loopEnableCheck->GetValue();
        m_loopStartSpin->Enable(loopEnabled);
        m_loopEndSpin->Enable(loopEnabled);
        if (!loopEnabled) {
            /* Stash current values so re-enabling can restore them. */
            m_savedLoopStart = static_cast<uint32_t>(m_loopStartSpin->GetValue());
            m_savedLoopEnd = static_cast<uint32_t>(m_loopEndSpin->GetValue());
            sample.sampleInfo.startLoop = 0;
            sample.sampleInfo.endLoop = 0;
            m_waveformPanel->SetLoopPoints(0, 0);
            m_loopInfoLabel->SetLabel("");
        } else {
            /* Restore previously saved values; fall back to full range only if
               nothing was saved (first-ever enable with no prior loop data). */
            uint32_t restoreStart = m_savedLoopStart;
            uint32_t restoreEnd = m_savedLoopEnd;
            if (restoreStart == 0 && restoreEnd == 0) {
                restoreEnd = sample.sampleInfo.waveFrames;
            }
            m_loopStartSpin->SetValue(static_cast<int>(restoreStart));
            m_loopEndSpin->SetValue(static_cast<int>(restoreEnd));
            ApplyLoopFromUI();
        }
    }

    void OnLoopPointChanged() {
        if (m_loopEnableCheck->GetValue()) {
            ApplyLoopFromUI();
        }
    }

    void OnCompressionChanged() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_samples.size())) {
            return;
        }
        EditedSample const &sample = m_samples[static_cast<size_t>(m_currentLocalIndex)];
        int idx = m_compressionChoice->GetSelection();
        if (idx == 0 && !sample.hasOriginalData) {
            /* "Don't Change" not allowed; revert to PCM */
            m_compressionChoice->SetSelection(CompressionTypeToChoiceIndex(BAE_EDITOR_COMPRESSION_PCM));
            wxMessageBox("\"Don't Change\" is not available for new or replaced samples. "
                         "Falling back to RAW PCM.",
                         "Compression", wxOK | wxICON_INFORMATION, this);
        }
    }

    void ApplyLoopFromUI() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_samples.size())) {
            return;
        }
        EditedSample &sample = m_samples[static_cast<size_t>(m_currentLocalIndex)];
        uint32_t start = static_cast<uint32_t>(std::max(0, m_loopStartSpin->GetValue()));
        uint32_t end = static_cast<uint32_t>(std::max(0, m_loopEndSpin->GetValue()));
        if (sample.sampleInfo.waveFrames > 0 && end > sample.sampleInfo.waveFrames) {
            end = sample.sampleInfo.waveFrames;
        }
        if (start >= end && end > 0) {
            start = end - 1;
        }
        sample.sampleInfo.startLoop = start;
        sample.sampleInfo.endLoop = end;
        m_waveformPanel->SetLoopPoints(start, end);
        UpdateLoopInfoLabel(sample);
    }

    void UpdateLoopInfoLabel(EditedSample const &sample) {
        if (sample.sampleInfo.startLoop < sample.sampleInfo.endLoop) {
            uint32_t loopFrames = sample.sampleInfo.endLoop - sample.sampleInfo.startLoop;
            uint32_t rateHz = (sample.sampleInfo.sampledRate >> 16);
            if (rateHz > 0) {
                double loopMs = loopFrames * 1000.0 / rateHz;
                m_loopInfoLabel->SetLabel(wxString::Format("%u frames (%.0f ms)", loopFrames, loopMs));
            } else {
                m_loopInfoLabel->SetLabel(wxString::Format("start: %u  end: %u",
                                                           sample.sampleInfo.startLoop,
                                                           sample.sampleInfo.endLoop));
            }
        } else {
            m_loopInfoLabel->SetLabel("");
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
        if (m_currentLocalIndex >= 0 && m_currentLocalIndex < static_cast<int>(m_samples.size())) {
            EditedSample const &sample = m_samples[static_cast<size_t>(m_currentLocalIndex)];
            m_waveformPanel->SetLoopPoints(sample.sampleInfo.startLoop, sample.sampleInfo.endLoop);
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
        m_playCallback(sampleIndex, m_triggerSpin->GetValue(), &m_samples[static_cast<size_t>(m_currentLocalIndex)].sampleInfo);
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
                sample.compressionType = BAE_EDITOR_COMPRESSION_PCM;
                sample.hasOriginalData = false;
                /* Update the compression UI: "Don't Change" now unavailable */
                m_compressionChoice->SetString(0, "Don't Change (N/A)");
                m_compressionChoice->SetSelection(CompressionTypeToChoiceIndex(BAE_EDITOR_COMPRESSION_PCM));
                m_codecLabel->SetLabel("(source: no compression)");
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
        wxString filter;

        if (!m_exportCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= static_cast<int>(m_sampleIndices.size())) {
            return;
        }
        sampleIndex = m_sampleIndices[static_cast<size_t>(m_currentLocalIndex)];
        codecBuf[0] = 0;
        if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, sampleIndex, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
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
        wxFileDialog dialog(this,
                            "Save Embedded Sample",
                            wxEmptyString,
                            suggestedName,
                            filter,
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        if (!m_exportCallback(sampleIndex, dialog.GetPath())) {
            wxMessageBox("Failed to save sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
        }
    }
};

}  // namespace

bool ShowInstrumentEditorDialog(wxWindow *parent,
                                BAERmfEditorDocument const *document,
                                uint32_t primarySampleIndex,
                                std::function<void(uint32_t, int, BAESampleInfo const *)> playCallback,
                                std::function<void()> stopCallback,
                                std::function<bool(uint32_t, wxString const &)> replaceCallback,
                                std::function<bool(uint32_t, wxString const &)> exportCallback,
                                std::vector<uint32_t> *outSampleIndices,
                                std::vector<InstrumentEditorEditedSample> *outEditedSamples) {
    InstrumentEditorDialog dialog(parent,
                                  document,
                                  primarySampleIndex,
                                  std::move(playCallback),
                                  std::move(stopCallback),
                                  std::move(replaceCallback),
                                  std::move(exportCallback));
    if (dialog.ShowModal() != wxID_OK) {
        return false;
    }
    if (outSampleIndices) {
        *outSampleIndices = dialog.GetSampleIndices();
    }
    if (outEditedSamples) {
        *outEditedSamples = dialog.GetEditedSamples();
    }
    return true;
}
