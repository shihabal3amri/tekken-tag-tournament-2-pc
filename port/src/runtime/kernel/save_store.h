#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using SaveContentRecord = std::array<uint8_t, 308>;
// <save root>/<signed-in profile XUID> (Win32 error when nobody is signed in).
uint32_t SaveUserRoot(std::filesystem::path& root);
uint32_t SaveListContent(std::vector<SaveContentRecord>& records);
uint32_t SaveCreateContent(const std::string& root, const SaveContentRecord& record,
                           uint32_t mode, uint32_t& disposition);
uint32_t SaveCloseContent(const std::string& root, bool close);
uint32_t SaveDeleteContent(const SaveContentRecord& record);
uint32_t SaveContentCreator(const SaveContentRecord& record, uint64_t& creator);
// true means this is a save path (including invalid/unmounted paths); never fall
// back to the disc resolver when true. error is a Win32 error, or zero on success.
bool ResolveSaveFile(const std::string& guest, std::filesystem::path& host,
                     std::filesystem::path& container, uint32_t& error);
// File handles retain host ownership across close/read races. Implemented in I/O.
uint32_t FlushSaveFiles(const std::filesystem::path& container, bool requireClosed);
// Online rollback (save_store.cpp): a container opened in a frame a rewind may still re-simulate is
// journaled first (its file list; a file's bytes right before the save changes it); a rewind to that
// frame or before puts the files back; confirmed frames drop the journal.
void SaveJournalCapture(const SaveContentRecord& record, int32_t frame);
// kernel_io.cpp: `file` in `container` is about to be written, truncated, deleted or replaced.
void SaveJournalBeforeChange(const std::filesystem::path& container, const std::filesystem::path& file);
int SaveJournalUndo(int32_t fromFrame);        // the number of saves undone
void SaveJournalPrune(int32_t confirmedFrame);
void SaveJournalClear();
bool SaveJournalHas(int32_t frame);            // a save of that frame is journaled (TTT2_RB_KOREPLAY's line)
