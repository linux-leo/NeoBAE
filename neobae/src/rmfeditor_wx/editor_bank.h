/****************************************************************************
 *
 * editor_bank.h
 *
 * Bank Editor Panel for NeoBAE Studio.
 * Provides a wxPanel-based UI for browsing and editing instruments
 * and samples within a loaded bank file (HSB/ZSB).
 *
 * C-style interface matching the project pattern used by other editor panels.
 *
 ****************************************************************************/

#ifndef EDITOR_BANK_H
#define EDITOR_BANK_H

#include <wx/panel.h>

extern "C" {
#include "NeoBAE.h"
}

/* Opaque handle to the bank editor panel */
typedef struct BankEditorPanel BankEditorPanel;

/* Create a new BankEditorPanel as a child of the given parent window. */
BankEditorPanel *CreateBankEditorPanel(wxWindow *parent);

/* Return the underlying wxWindow* for layout purposes. */
wxWindow *BankEditorPanel_AsWindow(BankEditorPanel *panel);

/* Load a bank into the editor panel for display and editing.
 * bankToken must be a valid loaded bank token from BAEMixer_AddBankFromFile.
 * bankPath is the file path for display purposes. */
void BankEditorPanel_LoadBank(BankEditorPanel *panel,
                              BAEBankToken bankToken,
                              const char *bankPath);

/* Clear the bank editor panel (no bank loaded). */
void BankEditorPanel_Clear(BankEditorPanel *panel);

#endif /* EDITOR_BANK_H */
