#include "ESPWriter.h"
#include <algorithm>
#include <cstring>

namespace esp {

ESPWriter::ESPWriter() {}

bool ESPWriter::Load(const std::string& filepath, const std::set<std::string>& recordTypes) {
	ESPReader reader;
	if (!reader.Load(filepath, recordTypes))
		return false;

	masters = reader.GetMasters();
	const auto& srcRecords = reader.GetRecords();
	for (auto const& [fid, rec] : srcRecords) {
		recordMap[fid] = rec;
	}
	
	recordMap.erase(0); // Remove old header

	// Local Index is N (00=Master0, 01=Master1... Local=N)
	uint32_t localIndex = (uint32_t)masters.size();
	
	// Find true max ID to avoid collisions
	uint32_t maxId = 0x010000;
	for (auto const& [fid, rec] : recordMap) {
		if ((fid >> 24) == localIndex) {
			uint32_t id = fid & 0x00FFFFFF;
			if (id > maxId) maxId = id;
		}
	}
	nextFormId = maxId + 1;

	// De-localize: bake strings into the foundation
	if (reader.IsLocalized()) {
		for (auto& [fid, rec] : recordMap) {
			for (auto& sr : rec.subrecords) {
				if (sr.type == "FULL" && sr.data.size() == 4) {
					std::string res = reader.ResolveFullName(rec);
					if (!res.empty()) {
						sr.data.assign(res.begin(), res.end());
						sr.data.push_back('\0');
					}
				}
			}
		}
	}

	return true; 
}

void ESPWriter::AddMaster(const std::string& masterName) {
	std::string lowerMaster = masterName;
	std::transform(lowerMaster.begin(), lowerMaster.end(), lowerMaster.begin(), ::tolower);
	for (const auto& m : masters) {
		std::string mLower = m;
		std::transform(mLower.begin(), mLower.end(), mLower.begin(), ::tolower);
		if (mLower == lowerMaster) return;
	}
	masters.push_back(masterName);
}

Record ESPWriter::CloneRecord(const Record& src, uint32_t newFormId) {
	Record r = src;
	r.formId = newFormId;
	return r;
}

uint32_t ESPWriter::AddRecord(const Record& rec) {
	Record newRec = rec;
	if (newRec.formId == 0) {
		std::string edid = newRec.EditorId();
		if (!edid.empty()) {
			const Record* existing = GetRecordByEditorId(edid);
			if (existing) newRec.formId = existing->formId;
		}

		if (newRec.formId == 0) {
			uint32_t localIndex = (uint32_t)masters.size();
			newRec.formId = (localIndex << 24) | (nextFormId++ & 0x00FFFFFF);
		}
	}
	recordMap[newRec.formId] = newRec;
	return newRec.formId;
}

const Record* ESPWriter::GetRecordByEditorId(const std::string& edid) const {
	for (auto const& [fid, rec] : recordMap) if (rec.EditorId() == edid) return &rec;
	return nullptr;
}

bool ESPWriter::Save(const std::string& filepath) {
	std::ofstream f(filepath, std::ios::binary);
	if (!f.is_open()) return false;

	// 1. TES4 Header
	Record header;
	header.type = "TES4";
	header.flags = 0; // Localized flag 0x80 is cleared automatically in WriteRecord
	header.formId = 0;
	
	Subrecord hedr;
	hedr.type = "HEDR";
	hedr.data.resize(12, 0);
	*reinterpret_cast<float*>(hedr.data.data()) = 1.7f; 
	*reinterpret_cast<uint32_t*>(hedr.data.data() + 4) = recordMap.size() + 1; 
	*reinterpret_cast<uint32_t*>(hedr.data.data() + 8) = (uint32_t)masters.size() << 24 | (nextFormId & 0x00FFFFFF);
	header.subrecords.push_back(hedr);

	for (const auto& master : masters) {
		Subrecord mast; mast.type = "MAST"; mast.data.assign(master.begin(), master.end()); mast.data.push_back('\0');
		header.subrecords.push_back(mast);
		Subrecord data; data.type = "DATA"; data.data.resize(8, 0);
		header.subrecords.push_back(data);
	}

	WriteRecord(f, header);

	// 2. Grouped Records
	std::map<std::string, std::vector<uint32_t>> groups;
	for (auto const& [fid, rec] : recordMap) groups[rec.type].push_back(fid);

	for (auto& [type, fids] : groups) {
		std::sort(fids.begin(), fids.end());

		uint32_t groupContentSize = 0;
		for (uint32_t fid : fids) {
			const auto& rec = recordMap[fid];
			groupContentSize += 24;
			for (const auto& sr : rec.subrecords) {
				if (sr.data.size() > 65535) groupContentSize += 16 + (uint32_t)sr.data.size();
				else groupContentSize += 6 + (uint32_t)sr.data.size();
			}
		}

		f.write("GRUP", 4);
		WriteLE(f, groupContentSize + 24); 
		f.write(type.c_str(), 4);
		WriteLE(f, (uint32_t)0);
		WriteLE(f, (uint16_t)0);
		WriteLE(f, (uint16_t)0);
		WriteLE(f, (uint32_t)0);

		for (uint32_t fid : fids) WriteRecord(f, recordMap[fid]);
	}

	f.close();
	return true;
}

void ESPWriter::WriteRecord(std::ofstream& f, const Record& rec) {
	uint32_t dataSize = 0;
	for (const auto& sr : rec.subrecords) {
		if (sr.data.size() > 65535) dataSize += 16 + (uint32_t)sr.data.size();
		else dataSize += 6 + (uint32_t)sr.data.size();
	}

	f.write(rec.type.c_str(), 4);
	WriteLE(f, dataSize);
	uint32_t flags = rec.flags & ~0x00040080; // No compression, no localization
	WriteLE(f, flags);
	WriteLE(f, rec.formId);
	WriteLE(f, rec.timestamp);
	WriteLE(f, rec.versionControl);
	WriteLE(f, rec.internalVersion);
	WriteLE(f, (uint16_t)0); // Pad to 24

	for (const auto& sr : rec.subrecords) WriteSubrecord(f, sr);
}

void ESPWriter::WriteSubrecord(std::ofstream& f, const Subrecord& sr) {
	if (sr.data.size() > 65535) {
		f.write("XXXX", 4);
		WriteLE(f, (uint16_t)4);
		WriteLE(f, (uint32_t)sr.data.size());
		f.write(sr.type.c_str(), 4);
		WriteLE(f, (uint16_t)0); 
	} else {
		f.write(sr.type.c_str(), 4);
		WriteLE(f, (uint16_t)sr.data.size());
	}
	if (!sr.data.empty()) f.write(reinterpret_cast<const char*>(sr.data.data()), sr.data.size());
}

Record ESPWriter::CreateARMO(uint32_t formId, const std::string& editorId, const std::string& fullName) {
	Record r; r.type = "ARMO"; r.formId = formId;
	Subrecord edid; edid.type = "EDID"; edid.data.assign(editorId.begin(), editorId.end()); edid.data.push_back('\0');
	r.subrecords.push_back(edid);
	Subrecord full; full.type = "FULL"; full.data.assign(fullName.begin(), fullName.end()); full.data.push_back('\0');
	r.subrecords.push_back(full);
	return r;
}

Record ESPWriter::CreateARMA(uint32_t formId, const std::string& editorId, const std::string& modelPath, uint32_t bodySlotFlags) {
	Record r; r.type = "ARMA"; r.formId = formId;
	Subrecord edid; edid.type = "EDID"; edid.data.assign(editorId.begin(), editorId.end()); edid.data.push_back('\0');
	r.subrecords.push_back(edid);
	Subrecord mod2; mod2.type = "MOD2"; mod2.data.assign(modelPath.begin(), modelPath.end()); mod2.data.push_back('\0');
	r.subrecords.push_back(mod2);
	return r;
}

} // namespace esp
