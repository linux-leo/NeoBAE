/****************************************************************************
 *
 * editor_bank.cpp
 *
 * Bank Editor Panel implementation for NeoBAE Studio.
 * Displays instruments organized by bank, samples per instrument,
 * and an embedded instrument editor with ADSR/LFO/LPF graphs.
 *
 ****************************************************************************/

#include <stdint.h>
#include <algorithm>
#include <vector>

#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
#include "X_API.h"
#include "X_Formats.h"
}

#include "editor_bank.h"
#include "editor_instrument_panels.h"

/* ------------------------------------------------------------------ */
/* Tree item data for instrument/sample nodes                         */
/* ------------------------------------------------------------------ */

class BankInstrumentItemData : public wxTreeItemData {
public:
    BankInstrumentItemData(uint32_t instrumentIndex, uint32_t instID)
        : m_instrumentIndex(instrumentIndex), m_instID(instID) {}
    uint32_t GetInstrumentIndex() const { return m_instrumentIndex; }
    uint32_t GetInstID() const { return m_instID; }
private:
    uint32_t m_instrumentIndex;
    uint32_t m_instID;
};

/* ------------------------------------------------------------------ */
/* Compression type name helper                                       */
/* ------------------------------------------------------------------ */

static const char *CompressionTypeName(uint32_t ct)
{
    switch (ct)
    {
        case 0:                                     return "PCM (raw)";
        case FOUR_CHAR('n','o','n','e'):            return "PCM";
        /* IMA ADPCM */
        case FOUR_CHAR('i','m','a','4'):            return "IMA4";
        case FOUR_CHAR('i','m','a','W'):            return "IMA4 (WAV)";
        case FOUR_CHAR('i','m','a','3'):            return "IMA3";
        /* MACE */
        case FOUR_CHAR('m','a','c','3'):            return "MACE3";
        case FOUR_CHAR('m','a','c','6'):            return "MACE6";
        /* uLaw / aLaw */
        case FOUR_CHAR('u','l','a','w'):            return "uLaw";
        case FOUR_CHAR('a','l','a','w'):            return "aLaw";
        /* FLAC */
        case FOUR_CHAR('f','L','a','C'):            return "FLAC";
        case FOUR_CHAR('F','L','A','C'):            return "FLAC";
        /* Vorbis */
        case FOUR_CHAR('O','g','g','V'):            return "Vorbis";
        case FOUR_CHAR('V','O','R','B'):            return "Vorbis";
        /* Opus */
        case FOUR_CHAR('O','g','g','O'):            return "Opus";
        case FOUR_CHAR('O','P','U','S'):            return "Opus";
        /* MPEG (MP3) - various bitrates */
        case FOUR_CHAR('m','p','g','n'):            return "MP3 32k";
        case FOUR_CHAR('m','p','g','a'):            return "MP3 40k";
        case FOUR_CHAR('m','p','g','b'):            return "MP3 48k";
        case FOUR_CHAR('m','p','g','c'):            return "MP3 56k";
        case FOUR_CHAR('m','p','g','d'):            return "MP3 64k";
        case FOUR_CHAR('m','p','g','e'):            return "MP3 80k";
        case FOUR_CHAR('m','p','g','f'):            return "MP3 96k";
        case FOUR_CHAR('m','p','g','g'):            return "MP3 112k";
        case FOUR_CHAR('m','p','g','h'):            return "MP3 128k";
        case FOUR_CHAR('m','p','g','i'):            return "MP3 160k";
        case FOUR_CHAR('m','p','g','j'):            return "MP3 192k";
        case FOUR_CHAR('m','p','g','k'):            return "MP3 224k";
        case FOUR_CHAR('m','p','g','l'):            return "MP3 256k";
        case FOUR_CHAR('m','p','g','m'):            return "MP3 320k";
        /* Legacy MPEG marker */
        case FOUR_CHAR('M','P','E','G'):            return "MPEG";
        default:
        {
            static char buf[32];
            if (ct > 0x20202020) {
                snprintf(buf, sizeof(buf), "'%c%c%c%c'",
                         (char)((ct >> 24) & 0xFF),
                         (char)((ct >> 16) & 0xFF),
                         (char)((ct >> 8) & 0xFF),
                         (char)(ct & 0xFF));
            } else {
                snprintf(buf, sizeof(buf), "0x%08X", ct);
            }
            return buf;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bank group helpers                                                 */
/* ------------------------------------------------------------------ */

struct BankGroupKey {
    int sortOrder;
    bool isPercussion;
};

static BankGroupKey GetBankGroupKey(uint32_t instID)
{
    BankGroupKey key;
    if (instID < 128) {
        key.sortOrder = 0;
        key.isPercussion = false;
    } else if (instID < 256) {
        key.sortOrder = 1;
        key.isPercussion = true;
    } else if (instID < 384) {
        key.sortOrder = 2;
        key.isPercussion = false;
    } else if (instID < 512) {
        key.sortOrder = 3;
        key.isPercussion = true;
    } else {
        key.sortOrder = 4;
        key.isPercussion = false;
    }
    return key;
}

static wxString GetBankGroupName(uint32_t instID)
{
    if (instID < 128) {
        return "GM Melodic (Bank 0)";
    } else if (instID < 256) {
        return "GM Percussion (Bank 0)";
    } else if (instID < 384) {
        return "NeoBAE Special Melodic (Bank 1)";
    } else if (instID < 512) {
        return "NeoBAE Special Percussion (Bank 1)";
    } else {
        uint32_t bank = instID / 128;
        return wxString::Format("NeoBAE Extra (Bank %u)", static_cast<unsigned>(bank));
    }
}

/* ------------------------------------------------------------------ */
/* BankEditorPanel implementation                                     */
/* ------------------------------------------------------------------ */

struct BankEditorPanel {
    wxPanel *panel;
    wxSplitterWindow *splitter;
    wxTreeCtrl *instrumentTree;
    wxListCtrl *sampleList;
    BAEBankToken bankToken;
    wxString bankPath;

    /* Right side: detail notebook with Instrument and Samples tabs */
    wxNotebook *detailNotebook;

    /* Instrument tab controls */
    wxPanel *instPage;
    wxStaticText *instHeaderLabel;
    wxTextCtrl *instNameText;
    wxSpinCtrl *instProgramSpin;
    wxCheckBox *chkInterpolate;
    wxCheckBox *chkAvoidReverb;
    wxCheckBox *chkSampleAndHold;
    wxCheckBox *chkPlayAtSampledFreq;
    wxCheckBox *chkNotPolyphonic;
    wxSpinCtrl *lpfFrequency;
    wxSpinCtrl *lpfResonance;
    wxSpinCtrl *lpfLowpassAmount;
    ADSRGraphPanel *filterGraph;
    wxSpinCtrl *adsrStageSpin;
    wxSpinCtrl *adsrLevel[BAE_EDITOR_MAX_ADSR_STAGES];
    wxSpinCtrlDouble *adsrTime[BAE_EDITOR_MAX_ADSR_STAGES];
    wxChoice *adsrFlags[BAE_EDITOR_MAX_ADSR_STAGES];
    wxStaticText *adsrRowLabels[BAE_EDITOR_MAX_ADSR_STAGES];
    ADSRGraphPanel *adsrGraph;
    wxSpinCtrl *lfoCountSpin;
    wxChoice *lfoSelector;
    wxChoice *lfoDest;
    wxChoice *lfoShape;
    wxSpinCtrl *lfoPeriod;
    wxSpinCtrl *lfoDCFeed;
    wxSpinCtrl *lfoLevel;
    LFOWaveformGraphPanel *lfoGraph;
    ADSRGraphPanel *lfoAdsrGraph;

    /* Samples tab controls */
    wxPanel *samplesPage;
    wxStaticText *sampleHeaderLabel;
    wxStaticText *sampleInfoLabel;
    wxStaticText *sampleLoopLabel;
    wxStaticText *sampleCodecLabel;

    /* State */
    uint32_t currentInstrumentIndex;
    BAERmfEditorInstrumentExtInfo currentExtInfo;
    int currentLfoIndex;
    bool hasInstrument;
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void PopulateInstrumentTree(BankEditorPanel *bp);
static void PopulateSampleList(BankEditorPanel *bp, uint32_t instrumentIndex);
static void ShowInstrumentDetail(BankEditorPanel *bp, uint32_t instrumentIndex);
static void ShowSampleDetail(BankEditorPanel *bp, uint32_t instrumentIndex, uint32_t sampleIndex);
static void OnInstrumentSelected(BankEditorPanel *bp, wxTreeEvent &event);
static void OnSampleSelected(BankEditorPanel *bp, wxListEvent &event);
static void RefreshADSRGraph(BankEditorPanel *bp);
static void RefreshLFOGraph(BankEditorPanel *bp);
static void RefreshLFOEnvelopeGraph(BankEditorPanel *bp);
static void RefreshFilterEnvelopeGraph(BankEditorPanel *bp);
static void LoadLFOIntoUI(BankEditorPanel *bp, int idx);
static void SaveLFOFromUI(BankEditorPanel *bp, int idx);
static void BuildInstrumentTab(BankEditorPanel *bp);
static void BuildSamplesTab(BankEditorPanel *bp);

/* ------------------------------------------------------------------ */
/* Instrument tree population                                         */
/* ------------------------------------------------------------------ */

static void PopulateInstrumentTree(BankEditorPanel *bp)
{
    uint32_t instCount = 0;
    wxTreeItemId root;

    bp->instrumentTree->DeleteAllItems();
    root = bp->instrumentTree->AddRoot("Instruments");

    if (!bp->bankToken) {
        fprintf(stderr, "[BankEditor] PopulateInstrumentTree: no bank token\n");
        bp->instrumentTree->Expand(root);
        return;
    }

    if (BAERmfEditorBank_GetInstrumentCount(bp->bankToken, &instCount) != BAE_NO_ERROR) {
        fprintf(stderr, "[BankEditor] PopulateInstrumentTree: GetInstrumentCount failed\n");
        bp->instrumentTree->Expand(root);
        return;
    }
    fprintf(stderr, "[BankEditor] PopulateInstrumentTree: %u instruments\n", instCount);

    struct BankGroup {
        int sortOrder;
        wxTreeItemId node;
    };
    std::vector<BankGroup> groups;

    auto findOrCreateGroup = [&](uint32_t instID) -> wxTreeItemId {
        BankGroupKey key = GetBankGroupKey(instID);
        for (auto &g : groups) {
            if (g.sortOrder == key.sortOrder) {
                return g.node;
            }
        }
        wxString groupName = GetBankGroupName(instID);
        wxTreeItemId node = bp->instrumentTree->AppendItem(root, groupName);
        groups.push_back({key.sortOrder, node});
        return node;
    };

    struct InstrumentEntry {
        uint32_t instrumentIndex;
        uint32_t displayInstID;
        BAERmfEditorBankInstrumentInfo info;
        bool isPercussion;
        bool isAlias;
    };
    std::vector<InstrumentEntry> entries;

    for (uint32_t i = 0; i < instCount; ++i) {
        InstrumentEntry entry;
        entry.instrumentIndex = i;
        entry.isAlias = false;
        if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, i, &entry.info) != BAE_NO_ERROR) {
            continue;
        }
        entry.displayInstID = entry.info.instID;
        entry.isPercussion = GetBankGroupKey(entry.info.instID).isPercussion;
        entries.push_back(entry);
    }

    /* Add aliased instruments */
    {
        XFILE bankFile = (XFILE)bp->bankToken;
        XAliasLinkResource *pAlias = XGetAliasLinkFromFile(bankFile);
        if (pAlias) {
            uint32_t aliasCount = (uint32_t)XGetLong(&pAlias->numberOfAliases);
            for (uint32_t a = 0; a < aliasCount; ++a) {
                XLongResourceID fromID = (XLongResourceID)XGetLong(&pAlias->list[a].aliasFrom);
                XLongResourceID toID = (XLongResourceID)XGetLong(&pAlias->list[a].aliasTo);

                for (auto const &e : entries) {
                    if (!e.isAlias && e.info.instID == (uint32_t)toID) {
                        InstrumentEntry aliasEntry;
                        aliasEntry.instrumentIndex = e.instrumentIndex;
                        aliasEntry.info = e.info;
                        aliasEntry.displayInstID = (uint32_t)fromID;
                        aliasEntry.info.instID = (uint32_t)fromID;
                        aliasEntry.info.bank = (uint16_t)(fromID / 128);
                        aliasEntry.info.program = (unsigned char)(fromID % 128);
                        aliasEntry.isPercussion = GetBankGroupKey((uint32_t)fromID).isPercussion;
                        aliasEntry.isAlias = true;
                        entries.push_back(aliasEntry);
                        break;
                    }
                }
            }
            XDisposePtr((XPTR)pAlias);
        }
    }

    std::sort(entries.begin(), entries.end(), [](InstrumentEntry const &a, InstrumentEntry const &b) {
        BankGroupKey ka = GetBankGroupKey(a.displayInstID);
        BankGroupKey kb = GetBankGroupKey(b.displayInstID);
        if (ka.sortOrder != kb.sortOrder) return ka.sortOrder < kb.sortOrder;
        return a.info.program < b.info.program;
    });

    for (auto const &entry : entries) {
        wxTreeItemId groupNode = findOrCreateGroup(entry.displayInstID);

        wxString label;
        wxString name = entry.info.name[0] ? wxString(entry.info.name) : wxString("(unnamed)");
        name.Replace("\r", "");
        name.Replace("\n", " ");
        name.Trim();
        if (entry.isAlias) {
            name += " (Alias)";
        }
        if (entry.isPercussion) {
            label = wxString::Format("%u: %s (Note %u)",
                                     static_cast<unsigned>(entry.info.program),
                                     name,
                                     static_cast<unsigned>(entry.info.program));
        } else {
            label = wxString::Format("%u: %s",
                                     static_cast<unsigned>(entry.info.program),
                                     name);
        }

        bp->instrumentTree->AppendItem(groupNode, label, -1, -1,
                                       new BankInstrumentItemData(entry.instrumentIndex, entry.displayInstID));
    }

    bp->instrumentTree->Expand(root);
    if (groups.size() <= 4) {
        for (auto &g : groups) {
            bp->instrumentTree->Expand(g.node);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sample list population                                             */
/* ------------------------------------------------------------------ */

static void PopulateSampleList(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    uint32_t sampleCount = 0;

    bp->sampleList->DeleteAllItems();

    if (!bp->bankToken) {
        return;
    }

    if (BAERmfEditorBank_GetInstrumentSampleCount(bp->bankToken, instrumentIndex, &sampleCount) != BAE_NO_ERROR) {
        return;
    }

    for (uint32_t s = 0; s < sampleCount; ++s) {
        BAERmfEditorBankSampleInfo sampleInfo;
        if (BAERmfEditorBank_GetInstrumentSampleInfo(bp->bankToken, instrumentIndex, s, &sampleInfo) != BAE_NO_ERROR) {
            continue;
        }

        wxString sndLabel = wxString::Format("SND %d", static_cast<int>(sampleInfo.sndResourceID));
        wxString keyRange = wxString::Format("%u-%u", sampleInfo.lowKey, sampleInfo.highKey);
        wxString rootKey = wxString::Format("%u", sampleInfo.rootKey);
        wxString rate = wxString::Format("%u Hz", sampleInfo.sampleRate);
        wxString frames = wxString::Format("%u", sampleInfo.frameCount);
        wxString codec = CompressionTypeName((uint32_t)sampleInfo.compressionType);
        wxString bits = wxString::Format("%d-bit %s",
                                         sampleInfo.bitDepth,
                                         sampleInfo.channels == 2 ? "stereo" : "mono");

        long idx = bp->sampleList->InsertItem(bp->sampleList->GetItemCount(), sndLabel);
        bp->sampleList->SetItem(idx, 1, keyRange);
        bp->sampleList->SetItem(idx, 2, rootKey);
        bp->sampleList->SetItem(idx, 3, rate);
        bp->sampleList->SetItem(idx, 4, bits);
        bp->sampleList->SetItem(idx, 5, codec);
        bp->sampleList->SetItem(idx, 6, frames);
    }
}

/* ------------------------------------------------------------------ */
/* ADSR / LFO / Filter graph refresh helpers                          */
/* ------------------------------------------------------------------ */

static void RefreshADSRGraph(BankEditorPanel *bp)
{
    int count = bp->adsrStageSpin->GetValue();
    int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
    for (int i = 0; i < count && i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
        levels[i] = bp->adsrLevel[i]->GetValue();
        times[i] = (int32_t)(bp->adsrTime[i]->GetValue() * 1000000.0);
        int sel = bp->adsrFlags[i]->GetSelection();
        flags[i] = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
    }
    bp->adsrGraph->SetEnvelope(count, levels, times, flags);
    RefreshFilterEnvelopeGraph(bp);
}

static void RefreshLFOGraph(BankEditorPanel *bp)
{
    int shapeSel;
    int32_t shape;

    if (!bp->lfoGraph) {
        return;
    }
    shapeSel = bp->lfoShape ? bp->lfoShape->GetSelection() : 0;
    shape = (shapeSel >= 0 && shapeSel < kLFOShapeCount) ? kLFOShapeLabels[shapeSel].value : (int32_t)FOUR_CHAR('S','I','N','E');
    bp->lfoGraph->SetLFO(shape,
                         bp->lfoPeriod ? bp->lfoPeriod->GetValue() : 0,
                         bp->lfoDCFeed ? bp->lfoDCFeed->GetValue() : 0,
                         bp->lfoLevel ? bp->lfoLevel->GetValue() : 0);
}

static void RefreshLFOEnvelopeGraph(BankEditorPanel *bp)
{
    int idx;
    int count;
    int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];

    if (!bp->lfoAdsrGraph) {
        return;
    }
    idx = bp->currentLfoIndex;
    if (idx < 0 || idx >= (int)bp->currentExtInfo.lfoCount) {
        bp->lfoAdsrGraph->SetEnvelope(0, levels, times, flags);
        return;
    }

    count = std::clamp((int)bp->currentExtInfo.lfos[idx].adsr.stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
    for (int i = 0; i < count; ++i) {
        levels[i] = bp->currentExtInfo.lfos[idx].adsr.stages[i].level;
        times[i] = bp->currentExtInfo.lfos[idx].adsr.stages[i].time;
        flags[i] = bp->currentExtInfo.lfos[idx].adsr.stages[i].flags;
    }
    bp->lfoAdsrGraph->SetEnvelope(count, levels, times, flags);
}

static void RefreshFilterEnvelopeGraph(BankEditorPanel *bp)
{
    int count;
    int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t baseFreq;
    int32_t amount;
    int64_t sourceLevels[BAE_EDITOR_MAX_ADSR_STAGES];
    int64_t maxAbsLevel;
    BAERmfEditorADSRInfo const *lpfSourceAdsr;

    if (!bp->filterGraph || !bp->adsrStageSpin) {
        return;
    }
    baseFreq = bp->lpfFrequency ? bp->lpfFrequency->GetValue() : 0;
    amount = bp->lpfLowpassAmount ? bp->lpfLowpassAmount->GetValue() : 0;
    lpfSourceAdsr = nullptr;

    for (uint32_t i = 0; i < bp->currentExtInfo.lfoCount; ++i) {
        int32_t dest = bp->currentExtInfo.lfos[i].destination;
        if ((dest == (int32_t)FOUR_CHAR('L','P','F','R') ||
             dest == (int32_t)FOUR_CHAR('L','P','A','M')) &&
            bp->currentExtInfo.lfos[i].adsr.stageCount > 0) {
            lpfSourceAdsr = &bp->currentExtInfo.lfos[i].adsr;
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
        count = std::clamp(bp->adsrStageSpin->GetValue(), 0, BAE_EDITOR_MAX_ADSR_STAGES);
        for (int i = 0; i < count; ++i) {
            sourceLevels[i] = bp->adsrLevel[i] ? (int64_t)bp->adsrLevel[i]->GetValue() : 0;
            times[i] = (int32_t)((bp->adsrTime[i] ? bp->adsrTime[i]->GetValue() : 0.0) * 1000000.0);
            {
                int sel = bp->adsrFlags[i] ? bp->adsrFlags[i]->GetSelection() : 0;
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
    bp->filterGraph->SetEnvelope(count, levels, times, flags);
}

static void LoadLFOIntoUI(BankEditorPanel *bp, int idx)
{
    if (idx < 0 || idx >= (int)bp->currentExtInfo.lfoCount) {
        bp->lfoDest->SetSelection(0);
        bp->lfoShape->SetSelection(0);
        bp->lfoPeriod->SetValue(0);
        bp->lfoDCFeed->SetValue(0);
        bp->lfoLevel->SetValue(0);
        RefreshLFOGraph(bp);
        RefreshLFOEnvelopeGraph(bp);
        return;
    }
    BAERmfEditorLFOInfo const &lfo = bp->currentExtInfo.lfos[idx];
    bp->lfoDest->SetSelection(FindLabelIndex(kLFODestLabels, kLFODestCount, lfo.destination));
    bp->lfoShape->SetSelection(FindLabelIndex(kLFOShapeLabels, kLFOShapeCount, lfo.waveShape));
    bp->lfoPeriod->SetValue(lfo.period);
    bp->lfoDCFeed->SetValue(lfo.DC_feed);
    bp->lfoLevel->SetValue(lfo.level);
    RefreshLFOGraph(bp);
    RefreshLFOEnvelopeGraph(bp);
}

static void SaveLFOFromUI(BankEditorPanel *bp, int idx)
{
    if (idx < 0 || idx >= BAE_EDITOR_MAX_LFOS) return;
    BAERmfEditorLFOInfo &lfo = bp->currentExtInfo.lfos[idx];
    int destSel = bp->lfoDest->GetSelection();
    if (destSel >= 0 && destSel < kLFODestCount) lfo.destination = kLFODestLabels[destSel].value;
    int shapeSel = bp->lfoShape->GetSelection();
    if (shapeSel >= 0 && shapeSel < kLFOShapeCount) lfo.waveShape = kLFOShapeLabels[shapeSel].value;
    lfo.period = bp->lfoPeriod->GetValue();
    lfo.DC_feed = bp->lfoDCFeed->GetValue();
    lfo.level = bp->lfoLevel->GetValue();
}

/* ------------------------------------------------------------------ */
/* Show instrument detail in the Instrument tab                       */
/* ------------------------------------------------------------------ */

static void ShowInstrumentDetail(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    BAERmfEditorInstrumentExtInfo extInfo;
    BAERmfEditorBankInstrumentInfo instInfo;

    if (!bp->bankToken) {
        bp->instHeaderLabel->SetLabel("No bank loaded.");
        bp->hasInstrument = false;
        return;
    }

    if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, instrumentIndex, &instInfo) != BAE_NO_ERROR) {
        bp->instHeaderLabel->SetLabel("Failed to read instrument info.");
        bp->hasInstrument = false;
        return;
    }

    bp->currentInstrumentIndex = instrumentIndex;
    bp->hasInstrument = true;

    {
        wxString instName = instInfo.name[0] ? wxString(instInfo.name) : wxString("(unnamed)");
        instName.Replace("\r", "");
        instName.Replace("\n", " ");
        instName.Trim();
        bp->instHeaderLabel->SetLabel(wxString::Format(
            "INST ID: %u  Bank: %u  Program: %u  Key Splits: %d",
            instInfo.instID, instInfo.bank, instInfo.program, instInfo.keySplitCount));
        bp->instNameText->SetValue(instName);
        bp->instProgramSpin->SetValue(instInfo.program);
    }

    /* Populate flags */
    bp->chkInterpolate->SetValue((instInfo.flags1 & 0x80) != 0);
    bp->chkAvoidReverb->SetValue((instInfo.flags1 & 0x01) != 0);
    bp->chkSampleAndHold->SetValue((instInfo.flags1 & 0x04) != 0);
    bp->chkPlayAtSampledFreq->SetValue((instInfo.flags2 & 0x40) != 0);
    bp->chkNotPolyphonic->SetValue((instInfo.flags2 & 0x04) != 0);

    /* Get extended info */
    memset(&extInfo, 0, sizeof(extInfo));
    if (BAERmfEditorBank_GetInstrumentExtInfo(bp->bankToken, instrumentIndex, &extInfo) == BAE_NO_ERROR) {
        bp->currentExtInfo = extInfo;

        /* LPF */
        bp->lpfFrequency->SetValue(extInfo.LPF_frequency);
        bp->lpfResonance->SetValue(extInfo.LPF_resonance);
        bp->lpfLowpassAmount->SetValue(extInfo.LPF_lowpassAmount);

        /* ADSR */
        bp->adsrStageSpin->SetValue((int)extInfo.volumeADSR.stageCount);
        for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            int lvl = (i < (int)extInfo.volumeADSR.stageCount) ? extInfo.volumeADSR.stages[i].level : 0;
            int32_t tmRaw = (i < (int)extInfo.volumeADSR.stageCount) ? extInfo.volumeADSR.stages[i].time : 0;
            int fl = (i < (int)extInfo.volumeADSR.stageCount) ? extInfo.volumeADSR.stages[i].flags : 0;
            bp->adsrLevel[i]->SetValue(lvl);
            bp->adsrTime[i]->SetValue((double)tmRaw / 1000000.0);
            bp->adsrFlags[i]->SetSelection(FindLabelIndex(kADSRFlagLabels, kADSRFlagCount, fl));
            bool visible = (i < (int)extInfo.volumeADSR.stageCount);
            bp->adsrRowLabels[i]->Show(visible);
            bp->adsrLevel[i]->Show(visible);
            bp->adsrTime[i]->Show(visible);
            bp->adsrFlags[i]->Show(visible);
        }

        /* LFO */
        bp->lfoCountSpin->SetValue((int)extInfo.lfoCount);
        if (extInfo.lfoCount > 0) {
            bp->currentLfoIndex = 0;
            bp->lfoSelector->SetSelection(0);
            LoadLFOIntoUI(bp, 0);
        } else {
            bp->currentLfoIndex = -1;
            bp->lfoSelector->SetSelection(wxNOT_FOUND);
            LoadLFOIntoUI(bp, -1);
        }

        RefreshADSRGraph(bp);
        RefreshFilterEnvelopeGraph(bp);
    } else {
        memset(&bp->currentExtInfo, 0, sizeof(bp->currentExtInfo));
        bp->lpfFrequency->SetValue(0);
        bp->lpfResonance->SetValue(0);
        bp->lpfLowpassAmount->SetValue(0);
        bp->adsrStageSpin->SetValue(0);
        for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            bp->adsrRowLabels[i]->Show(false);
            bp->adsrLevel[i]->Show(false);
            bp->adsrTime[i]->Show(false);
            bp->adsrFlags[i]->Show(false);
        }
        bp->lfoCountSpin->SetValue(0);
        bp->currentLfoIndex = -1;
        RefreshADSRGraph(bp);
        RefreshLFOGraph(bp);
        RefreshLFOEnvelopeGraph(bp);
        RefreshFilterEnvelopeGraph(bp);
    }

    bp->instPage->Layout();
    bp->detailNotebook->SetSelection(0);  /* Switch to Instrument tab */
}

/* ------------------------------------------------------------------ */
/* Show sample detail in the Samples tab                              */
/* ------------------------------------------------------------------ */

static void ShowSampleDetail(BankEditorPanel *bp, uint32_t instrumentIndex, uint32_t sampleIndex)
{
    BAERmfEditorBankSampleInfo sampleInfo;

    if (!bp->bankToken) {
        bp->sampleHeaderLabel->SetLabel("No bank loaded.");
        return;
    }

    if (BAERmfEditorBank_GetInstrumentSampleInfo(bp->bankToken, instrumentIndex, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
        bp->sampleHeaderLabel->SetLabel("Failed to read sample info.");
        return;
    }

    bp->sampleHeaderLabel->SetLabel(wxString::Format(
        "SND Resource: %d  |  Sample %u of instrument",
        static_cast<int>(sampleInfo.sndResourceID),
        static_cast<unsigned>(sampleIndex)));

    bp->sampleInfoLabel->SetLabel(wxString::Format(
        "Root Key: %u    Key Range: %u - %u    Split Volume: %d\n"
        "Sample Rate: %u Hz    Frames: %u    %d-bit %s\n"
        "Codec: %s",
        sampleInfo.rootKey,
        sampleInfo.lowKey, sampleInfo.highKey,
        static_cast<int>(sampleInfo.splitVolume),
        sampleInfo.sampleRate, sampleInfo.frameCount,
        sampleInfo.bitDepth,
        sampleInfo.channels == 2 ? "stereo" : "mono",
        CompressionTypeName((uint32_t)sampleInfo.compressionType)));

    if (sampleInfo.loopStart < sampleInfo.loopEnd) {
        uint32_t loopFrames = sampleInfo.loopEnd - sampleInfo.loopStart;
        double loopMs = (sampleInfo.sampleRate > 0) ? (loopFrames * 1000.0 / sampleInfo.sampleRate) : 0.0;
        bp->sampleLoopLabel->SetLabel(wxString::Format(
            "Loop: %u - %u  (%u frames, %.1f ms)",
            sampleInfo.loopStart, sampleInfo.loopEnd, loopFrames, loopMs));
    } else {
        bp->sampleLoopLabel->SetLabel("Loop: none");
    }

    bp->samplesPage->Layout();
    bp->detailNotebook->SetSelection(1);  /* Switch to Samples tab */
}

/* ------------------------------------------------------------------ */
/* Event handlers                                                     */
/* ------------------------------------------------------------------ */

static void OnInstrumentSelected(BankEditorPanel *bp, wxTreeEvent &event)
{
    wxTreeItemId item = event.GetItem();
    if (!item.IsOk()) {
        return;
    }

    BankInstrumentItemData *data = dynamic_cast<BankInstrumentItemData *>(
        bp->instrumentTree->GetItemData(item));
    if (!data) {
        /* Group node selected, not an instrument */
        bp->sampleList->DeleteAllItems();
        bp->instHeaderLabel->SetLabel("Select an instrument to view details.");
        bp->hasInstrument = false;
        return;
    }

    uint32_t instrumentIndex = data->GetInstrumentIndex();
    PopulateSampleList(bp, instrumentIndex);
    ShowInstrumentDetail(bp, instrumentIndex);
}

static void OnSampleSelected(BankEditorPanel *bp, wxListEvent &event)
{
    if (!bp->hasInstrument) {
        return;
    }
    long sel = event.GetIndex();
    if (sel < 0) {
        return;
    }
    ShowSampleDetail(bp, bp->currentInstrumentIndex, (uint32_t)sel);
}

/* ------------------------------------------------------------------ */
/* Build Instrument Tab                                               */
/* ------------------------------------------------------------------ */

static void BuildInstrumentTab(BankEditorPanel *bp)
{
    bp->instPage = new wxPanel(bp->detailNotebook);
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticBoxSizer *lpfBox = nullptr;
    wxStaticBoxSizer *lfoBox = nullptr;
    wxStaticBoxSizer *adsrBox = nullptr;

    /* Header */
    bp->instHeaderLabel = new wxStaticText(bp->instPage, wxID_ANY, "Select an instrument to view details.");
    sizer->Add(bp->instHeaderLabel, 0, wxALL, 8);

    /* Name + Program */
    {
        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(bp->instPage, wxID_ANY, "Name"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        bp->instNameText = new wxTextCtrl(bp->instPage, wxID_ANY, "");
        row->Add(bp->instNameText, 1, wxRIGHT, 15);
        row->Add(new wxStaticText(bp->instPage, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        bp->instProgramSpin = new wxSpinCtrl(bp->instPage, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                             wxSP_ARROW_KEYS, 0, 127, 0);
        row->Add(bp->instProgramSpin, 0);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    }

    /* Flags */
    {
        wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, bp->instPage, "Flags");
        wxBoxSizer *row1 = new wxBoxSizer(wxHORIZONTAL);
        bp->chkInterpolate = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Interpolate");
        bp->chkAvoidReverb = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Avoid Reverb");
        bp->chkSampleAndHold = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Sample && Hold");
        row1->Add(bp->chkInterpolate, 0, wxRIGHT, 15);
        row1->Add(bp->chkAvoidReverb, 0, wxRIGHT, 15);
        row1->Add(bp->chkSampleAndHold, 0);
        box->Add(row1, 0, wxALL, 4);
        wxBoxSizer *row2 = new wxBoxSizer(wxHORIZONTAL);
        bp->chkPlayAtSampledFreq = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Play at Sampled Freq");
        bp->chkNotPolyphonic = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Not Polyphonic");
        row2->Add(bp->chkPlayAtSampledFreq, 0, wxRIGHT, 15);
        row2->Add(bp->chkNotPolyphonic, 0);
        box->Add(row2, 0, wxALL, 4);
        sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    /* LPF */
    {
        lpfBox = new wxStaticBoxSizer(wxHORIZONTAL, bp->instPage, "Low-Pass Filter");
        wxBoxSizer *controlsRow = new wxBoxSizer(wxHORIZONTAL);

        controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Frequency"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        bp->lpfFrequency = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                          wxSP_ARROW_KEYS, -100000, 100000, 0);
        controlsRow->Add(bp->lpfFrequency, 0, wxRIGHT, 12);
        controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Resonance"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        bp->lpfResonance = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                          wxSP_ARROW_KEYS, -100000, 100000, 0);
        controlsRow->Add(bp->lpfResonance, 0, wxRIGHT, 12);
        controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Amount"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        bp->lpfLowpassAmount = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                              wxSP_ARROW_KEYS, -100000, 100000, 0);
        controlsRow->Add(bp->lpfLowpassAmount, 0);
        lpfBox->Add(controlsRow, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        wxBoxSizer *previewCol = new wxBoxSizer(wxVERTICAL);
        previewCol->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Filter Envelope Preview"), 0, wxBOTTOM, 2);
        bp->filterGraph = new ADSRGraphPanel(lpfBox->GetStaticBox());
        bp->filterGraph->SetMinSize(wxSize(-1, 60));
        previewCol->Add(bp->filterGraph, 1, wxEXPAND);
        lpfBox->Add(previewCol, 1, wxEXPAND | wxALL, 4);

        bp->lpfFrequency->Bind(wxEVT_SPINCTRL, [bp](wxCommandEvent &) { RefreshFilterEnvelopeGraph(bp); });
        bp->lpfResonance->Bind(wxEVT_SPINCTRL, [bp](wxCommandEvent &) { RefreshFilterEnvelopeGraph(bp); });
        bp->lpfLowpassAmount->Bind(wxEVT_SPINCTRL, [bp](wxCommandEvent &) { RefreshFilterEnvelopeGraph(bp); });
    }

    /* Volume ADSR */
    {
        wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, bp->instPage, "Volume ADSR");
        adsrBox = box;
        wxBoxSizer *countRow = new wxBoxSizer(wxHORIZONTAL);
        countRow->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Stages:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        bp->adsrStageSpin = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                           wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_ADSR_STAGES, 0);
        countRow->Add(bp->adsrStageSpin, 0);
        box->Add(countRow, 0, wxALL, 4);

        bp->adsrGraph = new ADSRGraphPanel(box->GetStaticBox());
        bp->adsrGraph->SetMinSize(wxSize(-1, 84));
        box->Add(bp->adsrGraph, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

        wxFlexGridSizer *grid = new wxFlexGridSizer(0, 4, 2, 6);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "#"), 0, wxALIGN_CENTER);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Time (s)"), 0, wxALIGN_CENTER);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Type"), 0, wxALIGN_CENTER);

        for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            bp->adsrRowLabels[i] = new wxStaticText(box->GetStaticBox(), wxID_ANY, wxString::Format("%d", i));
            grid->Add(bp->adsrRowLabels[i], 0, wxALIGN_CENTER_VERTICAL);
            bp->adsrLevel[i] = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                              wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(bp->adsrLevel[i], 0);
            bp->adsrTime[i] = new wxSpinCtrlDouble(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                                   wxSP_ARROW_KEYS, -16.0, 16.0, 0.0, 0.001);
            bp->adsrTime[i]->SetDigits(3);
            grid->Add(bp->adsrTime[i], 0);
            bp->adsrFlags[i] = new wxChoice(box->GetStaticBox(), wxID_ANY);
            FillChoice(bp->adsrFlags[i], kADSRFlagLabels, kADSRFlagCount);
            bp->adsrFlags[i]->SetSelection(0);
            grid->Add(bp->adsrFlags[i], 0);

            bp->adsrLevel[i]->Bind(wxEVT_SPINCTRL, [bp](wxCommandEvent &) { RefreshADSRGraph(bp); });
            bp->adsrTime[i]->Bind(wxEVT_SPINCTRLDOUBLE, [bp](wxSpinDoubleEvent &) { RefreshADSRGraph(bp); });
            bp->adsrFlags[i]->Bind(wxEVT_CHOICE, [bp](wxCommandEvent &) { RefreshADSRGraph(bp); });

            bp->adsrRowLabels[i]->Show(false);
            bp->adsrLevel[i]->Show(false);
            bp->adsrTime[i]->Show(false);
            bp->adsrFlags[i]->Show(false);
        }
        box->Add(grid, 0, wxALL, 4);

        bp->adsrStageSpin->Bind(wxEVT_SPINCTRL, [bp, box](wxCommandEvent &) {
            int count = bp->adsrStageSpin->GetValue();
            for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
                bool vis = (i < count);
                bp->adsrRowLabels[i]->Show(vis);
                bp->adsrLevel[i]->Show(vis);
                bp->adsrTime[i]->Show(vis);
                bp->adsrFlags[i]->Show(vis);
            }
            box->GetStaticBox()->GetParent()->Layout();
            RefreshADSRGraph(bp);
        });
    }

    /* LFO section */
    {
        lfoBox = new wxStaticBoxSizer(wxVERTICAL, bp->instPage, "LFOs");
        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Count:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        bp->lfoCountSpin = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                          wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_LFOS, 0);
        row->Add(bp->lfoCountSpin, 0, wxRIGHT, 15);
        row->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Edit LFO:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        bp->lfoSelector = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
        for (int i = 0; i < BAE_EDITOR_MAX_LFOS; i++) bp->lfoSelector->Append(wxString::Format("LFO %d", i));
        row->Add(bp->lfoSelector, 0);
        lfoBox->Add(row, 0, wxALL, 4);

        wxFlexGridSizer *grid = new wxFlexGridSizer(2, 4, 6);
        grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Dest"), 0, wxALIGN_CENTER_VERTICAL);
        bp->lfoDest = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
        FillChoice(bp->lfoDest, kLFODestLabels, kLFODestCount);
        grid->Add(bp->lfoDest, 0);
        grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL);
        bp->lfoShape = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
        FillChoice(bp->lfoShape, kLFOShapeLabels, kLFOShapeCount);
        grid->Add(bp->lfoShape, 0);
        grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Period"), 0, wxALIGN_CENTER_VERTICAL);
        bp->lfoPeriod = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                       wxSP_ARROW_KEYS, -1000000, 10000000, 0);
        grid->Add(bp->lfoPeriod, 0);
        grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "DC Feed"), 0, wxALIGN_CENTER_VERTICAL);
        bp->lfoDCFeed = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                       wxSP_ARROW_KEYS, -1000000, 1000000, 0);
        grid->Add(bp->lfoDCFeed, 0);
        grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER_VERTICAL);
        bp->lfoLevel = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                      wxSP_ARROW_KEYS, -1000000, 1000000, 0);
        grid->Add(bp->lfoLevel, 0);
        lfoBox->Add(grid, 0, wxALL, 4);

        bp->lfoGraph = new LFOWaveformGraphPanel(lfoBox->GetStaticBox());
        lfoBox->Add(bp->lfoGraph, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

        lfoBox->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "LFO Envelope (selected LFO)"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
        bp->lfoAdsrGraph = new ADSRGraphPanel(lfoBox->GetStaticBox());
        bp->lfoAdsrGraph->SetMinSize(wxSize(-1, 72));
        lfoBox->Add(bp->lfoAdsrGraph, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

        bp->lfoSelector->Bind(wxEVT_CHOICE, [bp](wxCommandEvent &) {
            int sel = bp->lfoSelector->GetSelection();
            if (bp->currentLfoIndex >= 0) {
                SaveLFOFromUI(bp, bp->currentLfoIndex);
            }
            bp->currentLfoIndex = sel;
            if (sel >= 0) LoadLFOIntoUI(bp, sel);
            RefreshLFOGraph(bp);
            RefreshLFOEnvelopeGraph(bp);
            RefreshFilterEnvelopeGraph(bp);
        });

        bp->lfoCountSpin->Bind(wxEVT_SPINCTRL, [bp](wxCommandEvent &) {
            int count = bp->lfoCountSpin->GetValue();
            if (count <= 0) {
                bp->currentLfoIndex = -1;
                bp->lfoSelector->SetSelection(wxNOT_FOUND);
            } else {
                int sel = bp->lfoSelector->GetSelection();
                if (sel < 0 || sel >= count) {
                    sel = 0;
                    bp->lfoSelector->SetSelection(sel);
                }
                bp->currentLfoIndex = sel;
                LoadLFOIntoUI(bp, sel);
            }
            RefreshLFOGraph(bp);
            RefreshLFOEnvelopeGraph(bp);
            RefreshFilterEnvelopeGraph(bp);
        });

        auto lfoChanged = [bp](wxCommandEvent &) {
            int sel = (bp->currentLfoIndex >= 0) ? bp->currentLfoIndex : bp->lfoSelector->GetSelection();
            if (sel >= 0) {
                SaveLFOFromUI(bp, sel);
            }
            RefreshLFOGraph(bp);
        };
        bp->lfoDest->Bind(wxEVT_CHOICE, lfoChanged);
        bp->lfoShape->Bind(wxEVT_CHOICE, lfoChanged);
        bp->lfoPeriod->Bind(wxEVT_SPINCTRL, lfoChanged);
        bp->lfoDCFeed->Bind(wxEVT_SPINCTRL, lfoChanged);
        bp->lfoLevel->Bind(wxEVT_SPINCTRL, lfoChanged);
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

    bp->instPage->SetSizerAndFit(sizer);
    bp->detailNotebook->AddPage(bp->instPage, "Instrument");
}

/* ------------------------------------------------------------------ */
/* Build Samples Tab                                                  */
/* ------------------------------------------------------------------ */

static void BuildSamplesTab(BankEditorPanel *bp)
{
    bp->samplesPage = new wxPanel(bp->detailNotebook);
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    bp->sampleHeaderLabel = new wxStaticText(bp->samplesPage, wxID_ANY, "Select a sample from the list to view details.");
    sizer->Add(bp->sampleHeaderLabel, 0, wxALL, 8);

    bp->sampleInfoLabel = new wxStaticText(bp->samplesPage, wxID_ANY, "",
                                           wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    sizer->Add(bp->sampleInfoLabel, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    bp->sampleLoopLabel = new wxStaticText(bp->samplesPage, wxID_ANY, "");
    sizer->Add(bp->sampleLoopLabel, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    bp->sampleCodecLabel = new wxStaticText(bp->samplesPage, wxID_ANY, "");
    sizer->Add(bp->sampleCodecLabel, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    /* TODO: Add waveform display when BAERmfEditorBank_GetSampleWaveformData API is available */

    bp->samplesPage->SetSizerAndFit(sizer);
    bp->detailNotebook->AddPage(bp->samplesPage, "Samples");
}

/* ------------------------------------------------------------------ */
/* Public C-style interface                                           */
/* ------------------------------------------------------------------ */

BankEditorPanel *CreateBankEditorPanel(wxWindow *parent)
{
    BankEditorPanel *bp = new BankEditorPanel();
    bp->bankToken = nullptr;
    bp->hasInstrument = false;
    bp->currentInstrumentIndex = 0;
    bp->currentLfoIndex = -1;
    memset(&bp->currentExtInfo, 0, sizeof(bp->currentExtInfo));

    bp->panel = new wxPanel(parent);
    bp->splitter = new wxSplitterWindow(bp->panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3D);

    /* Left side: instrument tree + sample list */
    wxPanel *leftPanel = new wxPanel(bp->splitter);
    wxBoxSizer *leftSizer = new wxBoxSizer(wxVERTICAL);

    leftSizer->Add(new wxStaticText(leftPanel, wxID_ANY, "Instruments"), 0, wxALL, 4);
    bp->instrumentTree = new wxTreeCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
    leftSizer->Add(bp->instrumentTree, 2, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    leftSizer->Add(new wxStaticText(leftPanel, wxID_ANY, "Samples / Key Splits"), 0, wxLEFT | wxRIGHT, 4);
    bp->sampleList = new wxListCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxLC_SINGLE_SEL);
    bp->sampleList->InsertColumn(0, "Sample", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(1, "Key Range", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(2, "Root", wxLIST_FORMAT_LEFT, 50);
    bp->sampleList->InsertColumn(3, "Rate", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(4, "Format", wxLIST_FORMAT_LEFT, 100);
    bp->sampleList->InsertColumn(5, "Codec", wxLIST_FORMAT_LEFT, 70);
    bp->sampleList->InsertColumn(6, "Frames", wxLIST_FORMAT_LEFT, 80);
    leftSizer->Add(bp->sampleList, 1, wxEXPAND | wxALL, 4);
    leftPanel->SetSizer(leftSizer);

    /* Right side: detail notebook with Instrument and Samples tabs */
    wxPanel *rightPanel = new wxPanel(bp->splitter);
    wxBoxSizer *rightSizer = new wxBoxSizer(wxVERTICAL);

    bp->detailNotebook = new wxNotebook(rightPanel, wxID_ANY);
    BuildInstrumentTab(bp);
    BuildSamplesTab(bp);

    rightSizer->Add(bp->detailNotebook, 1, wxEXPAND);
    rightPanel->SetSizer(rightSizer);

    bp->splitter->SplitVertically(leftPanel, rightPanel, 500);
    bp->splitter->SetMinimumPaneSize(200);

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(bp->splitter, 1, wxEXPAND);
    bp->panel->SetSizer(mainSizer);

    /* Bind events */
    bp->instrumentTree->Bind(wxEVT_TREE_SEL_CHANGED, [bp](wxTreeEvent &event) {
        OnInstrumentSelected(bp, event);
    });

    bp->sampleList->Bind(wxEVT_LIST_ITEM_SELECTED, [bp](wxListEvent &event) {
        OnSampleSelected(bp, event);
    });

    return bp;
}

wxWindow *BankEditorPanel_AsWindow(BankEditorPanel *panel)
{
    return panel ? panel->panel : nullptr;
}

void BankEditorPanel_LoadBank(BankEditorPanel *panel,
                              BAEBankToken bankToken,
                              const char *bankPath)
{
    if (!panel) {
        return;
    }
    panel->bankToken = bankToken;
    panel->bankPath = bankPath ? bankPath : "";
    panel->hasInstrument = false;
    PopulateInstrumentTree(panel);
    panel->sampleList->DeleteAllItems();
    panel->instHeaderLabel->SetLabel("Select an instrument to view details.");
    panel->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
}

void BankEditorPanel_Clear(BankEditorPanel *panel)
{
    if (!panel) {
        return;
    }
    panel->bankToken = nullptr;
    panel->bankPath.clear();
    panel->hasInstrument = false;
    panel->instrumentTree->DeleteAllItems();
    panel->instrumentTree->AddRoot("Instruments");
    panel->sampleList->DeleteAllItems();
    panel->instHeaderLabel->SetLabel("Open an HSB or ZSB file to begin editing.");
    panel->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
}
