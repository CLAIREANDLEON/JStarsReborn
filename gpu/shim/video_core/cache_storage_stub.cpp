// JStarsReborn: the on-disk shader/pipeline blob archive (video_core/cache_storage.cpp, miniz zip) is
// not built in this checkpoint. EmulatorSettings::IsPipelineCacheEnabled() is false, so nothing is
// stored or loaded; Open() leaves the archive closed and Save() reports that nothing was stored.
#include "video_core/cache_storage.h"
namespace Storage {
void DataBase::Open() { opened = false; }
void DataBase::Close() {}
void DataBase::FinishPreload() {}
bool DataBase::Save(BlobType, const std::string&, std::vector<u8>&&) { return false; }
bool DataBase::Save(BlobType, const std::string&, std::vector<u32>&&) { return false; }
void DataBase::Load(BlobType, const std::string&, std::vector<u8>&) {}
void DataBase::Load(BlobType, const std::string&, std::vector<u32>&) {}
void DataBase::ForEachBlob(BlobType, const std::function<void(std::vector<u8>&&)>&) {}
} // namespace Storage
