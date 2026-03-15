#pragma once

#include <stdint.h>

#include <functional>
#include <vector>

#include <wx/string.h>

extern "C" {
#include "NeoBAE.h"
}

class wxWindow;

struct InstrumentEditorEditedSample {
    wxString displayName;
    wxString sourcePath;
    unsigned char program;
    unsigned char rootKey;
    unsigned char lowKey;
    unsigned char highKey;
    BAESampleInfo sampleInfo;
    BAERmfEditorCompressionType compressionType;
    bool hasOriginalData;
};

bool ShowInstrumentEditorDialog(wxWindow *parent,
                                BAERmfEditorDocument const *document,
                                uint32_t primarySampleIndex,
                                std::function<void(uint32_t, int, BAESampleInfo const *)> playCallback,
                                std::function<void()> stopCallback,
                                std::function<bool(uint32_t, wxString const &)> replaceCallback,
                                std::function<bool(uint32_t, wxString const &)> exportCallback,
                                std::vector<uint32_t> *outSampleIndices,
                                std::vector<InstrumentEditorEditedSample> *outEditedSamples);
