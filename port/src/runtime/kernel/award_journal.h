#pragma once
// Online rollback: the end-of-battle award's journal (award_journal.cpp).
#include <cstdint>

void AwardJournalUndo(int32_t p);        // rollback.cpp SessionSaveBeforeRestore: a restore to frame p follows
void AwardJournalSessionOn();            // rollback.cpp: a session starts (the journal and the detector start empty)
void AwardJournalSessionOver();          // rollback.cpp: the session ended (every award stands; the detector keeps the last)
int32_t AwardJournalNewestFrame();       // rollback.cpp SessionSelfTest: the newest journaled award's frame (-1: none)
