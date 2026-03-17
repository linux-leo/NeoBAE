#include "batch_compress_dialog.h"

BatchCompressDialog::BatchCompressDialog(wxWindow *parent, const std::vector<uint32_t> &sampleIndices, bool compressAll)
    : wxDialog(parent, wxID_ANY, compressAll ? "Compress All Instruments" : "Compress Instrument",
               wxDefaultPosition, wxSize(450, 320), wxDEFAULT_DIALOG_STYLE),
      m_sampleIndices(sampleIndices),
      m_compressAll(compressAll)
{
    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Title/Description
    wxString descText = compressAll 
        ? wxString::Format("Compress all %zu samples", m_sampleIndices.size())
        : wxString::Format("Compress %zu sample(s)", m_sampleIndices.size());
    m_samplesLabel = new wxStaticText(this, wxID_ANY, descText);
    mainSizer->Add(m_samplesLabel, 0, wxALL | wxEXPAND, 10);

    // Codec selection
    wxBoxSizer *codecSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *codecLabel = new wxStaticText(this, wxID_ANY, "Codec:");
    m_codecChoice = new wxChoice(this, wxID_ANY);
    m_codecChoice->Append("Original");
    m_codecChoice->Append("RAW PCM");
    m_codecChoice->Append("ADPCM");
    m_codecChoice->Append("MP3");
    m_codecChoice->Append("VORBIS");
    m_codecChoice->Append("FLAC");
    m_codecChoice->Append("OPUS");
    m_codecChoice->SetSelection(DEFAULT_CODEC); // Default to OPUS
    codecSizer->Add(codecLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    codecSizer->Add(m_codecChoice, 1, wxALL | wxEXPAND, 5);
    mainSizer->Add(codecSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // Bitrate selection
    wxBoxSizer *bitrateSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *bitrateLabel = new wxStaticText(this, wxID_ANY, "Bitrate:");
    m_bitrateChoice = new wxChoice(this, wxID_ANY);
    bitrateSizer->Add(bitrateLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    bitrateSizer->Add(m_bitrateChoice, 1, wxALL | wxEXPAND, 5);
    mainSizer->Add(bitrateSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    UpdateBitrateChoice(DEFAULT_CODEC);

    // Buttons
    wxBoxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *okBtn = new wxButton(this, wxID_OK, "OK");
    wxButton *cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
    buttonSizer->Add(okBtn, 0, wxALL, 5);
    buttonSizer->Add(cancelBtn, 0, wxALL, 5);
    mainSizer->Add(buttonSizer, 0, wxALIGN_CENTER | wxALL, 10);

    SetSizer(mainSizer);

    // Bind events
    m_codecChoice->Bind(wxEVT_CHOICE, &BatchCompressDialog::OnCodecSelected, this);

    Centre();
}

BatchCompressDialog::~BatchCompressDialog()
{
}

void BatchCompressDialog::OnCodecSelected(wxCommandEvent &event)
{
    int codecIdx = m_codecChoice->GetSelection();
    UpdateBitrateChoice(codecIdx);
}

void BatchCompressDialog::UpdateBitrateChoice(int codecIdx)
{
    m_bitrateChoice->Clear();

    switch (codecIdx)
    {
    case 0: // Original
    case 1: // RAW PCM
    case 2: // ADPCM
    case 5: // FLAC
        m_bitrateChoice->Append("---");
        m_bitrateChoice->SetSelection(0);
        m_bitrateChoice->Enable(false);
        break;

    case 3: // MP3
        m_bitrateChoice->Append("32k");
        m_bitrateChoice->Append("48k");
        m_bitrateChoice->Append("64k");
        m_bitrateChoice->Append("96k");
        m_bitrateChoice->Append("128k");
        m_bitrateChoice->Append("192k");
        m_bitrateChoice->Append("256k");
        m_bitrateChoice->Append("320k");
        m_bitrateChoice->SetSelection(4); // 128k default
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
        m_bitrateChoice->SetSelection(5); // 128k default
        m_bitrateChoice->Enable(true);
        break;

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
        m_bitrateChoice->SetSelection(7); // 128k default
        m_bitrateChoice->Enable(true);
        break;

    default:
        m_bitrateChoice->Append("---");
        m_bitrateChoice->SetSelection(0);
        m_bitrateChoice->Enable(false);
    }
}

BAERmfEditorCompressionType BatchCompressDialog::GetSelectedCompressionType() const
{
    int codecIdx = m_codecChoice->GetSelection();
    int bitrateIdx = m_bitrateChoice->GetSelection();

    switch (codecIdx)
    {
    case 0: return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    case 1: return BAE_EDITOR_COMPRESSION_PCM;
    case 2: return BAE_EDITOR_COMPRESSION_ADPCM;
    case 3: // MP3
    {
        switch (bitrateIdx)
        {
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
    }
    case 4: // VORBIS
    {
        switch (bitrateIdx)
        {
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
    }
    case 5: return BAE_EDITOR_COMPRESSION_FLAC;
    case 6: // OPUS
    {
        switch (bitrateIdx)
        {
        case 0: return BAE_EDITOR_COMPRESSION_OPUS_12K;
        case 1: return BAE_EDITOR_COMPRESSION_OPUS_16K;
        case 2: return BAE_EDITOR_COMPRESSION_OPUS_24K;
        case 3: return BAE_EDITOR_COMPRESSION_OPUS_32K;
        case 4: return BAE_EDITOR_COMPRESSION_OPUS_48K;
        case 5: return BAE_EDITOR_COMPRESSION_OPUS_64K;
        case 6: return BAE_EDITOR_COMPRESSION_OPUS_96K;
        case 7: return BAE_EDITOR_COMPRESSION_OPUS_128K;
        case 8: return BAE_EDITOR_COMPRESSION_OPUS_256K;
        default: return BAE_EDITOR_COMPRESSION_OPUS_128K;
        }
    }
    default: return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    }
}
