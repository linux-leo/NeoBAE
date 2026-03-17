#pragma once

#include <wx/wx.h>
#include <vector>
#include "NeoBAE.h"

class BatchCompressDialog : public wxDialog
{
public:
    BatchCompressDialog(wxWindow *parent, const std::vector<uint32_t> &sampleIndices, bool compressAll = false);
    ~BatchCompressDialog();

    BAERmfEditorCompressionType GetSelectedCompressionType() const;

private:
    wxChoice *m_codecChoice = nullptr;
    wxChoice *m_bitrateChoice = nullptr;
    wxStaticText *m_samplesLabel = nullptr;

    void OnCodecSelected(wxCommandEvent &event);
    void UpdateBitrateChoice(int codecIdx);
    
    std::vector<uint32_t> m_sampleIndices;
    bool m_compressAll;

    static const int CODEC_CHOICES_COUNT = 7;
    static const int DEFAULT_CODEC = 6; // OPUS
};
