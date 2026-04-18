#include "ESPWriter.h"
#include <algorithm>

namespace esp {

ESPWriter::ESPWriter() {}

bool ESPWriter::Load(const std::string& filepath) {
	ESPReader reader;
	if (!reader.Load(filepath))
		return false;

	masters = reader.GetMasters();
	recordMap = reader.GetRecords();
	
	// Update nextFormId to be higher than any existing record in the patch
	for (auto const& [fid, rec] : recordMap) {
		if ((fid & 0xFF000000) == 0x01000000) { // If it's a record in our ESP slot
			uint32_t localId = fid & 0x00FFFFFF;
			if (localId >= nextFormId) {
				nextFormId = localId + 1;
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
		// Try to find if record already exists by EditorID
		std::string edid = newRec.EditorId();
		if (!edid.empty()) {
			const Record* existing = GetRecordByEditorId(edid);
			if (existing) {
				newRec.formId = existing->formId;
			}
		}

		if (newRec.formId == 0) {
			newRec.formId = 0x01000000 | nextFormId++;
		}
	}
	recordMap[newRec.formId] = newRec;
	return newRec.formId;
}

const Record* ESPWriter::GetRecordByEditorId(const std::string& edid) const {
	for (auto const& [fid, rec] : recordMap) {
		if (rec.EditorId() == edid) {
			return &rec;
		}
	}
	return nullptr;
}

bool ESPWriter::Save(const std::string& filepath) {
	std::ofstream f(filepath, std::ios::binary);
	if (!f.is_open()) return false;

	// Write TES4 Header
	Record header;
	header.type = "TES4";
	header.flags = 0;
	header.formId = 0;
	
	// HEDR subrecord
	Subrecord hedr;
	hedr.type = "HEDR";
	hedr.data.resize(12, 0);
	*reinterpret_cast<float*>(hedr.data.data()) = 1.7f; // Version
	*reinterpret_cast<uint32_t*>(hedr.data.data() + 4) = recordMap.size(); // Number of records
	*reinterpret_cast<uint32_t*>(hedr.data.data() + 8) = nextFormId; // Next FormID
	header.subrecords.push_back(hedr);

	// MAST and DATA subrecords for masters
	for (const auto& master : masters) {
		Subrecord mast;
		mast.type = "MAST";
		mast.data.assign(master.begin(), master.end());
		mast.data.push_back('\0');
		header.subrecords.push_back(mast);

		Subrecord data;
		data.type = "DATA";
		data.data.resize(8, 0); 
		header.subrecords.push_back(data);
	}

	WriteRecord(f, header);

	// Write all records
	// Sort by FormID for consistency
	std::vector<uint32_t> formIds;
	for (auto const& [fid, rec] : recordMap) formIds.push_back(fid);
	std::sort(formIds.begin(), formIds.end());

	for (uint32_t fid : formIds) {
		WriteRecord(f, recordMap[fid]);
	}

	f.close();
	return true;
}

void ESPWriter::WriteRecord(std::ofstream& f, const Record& rec) {
	f.write(rec.type.c_str(), 4);
	
	// Calculate data size (sum of subrecords)
	uint32_t dataSize = 0;
	for (const auto& sr : rec.subrecords) {
		dataSize += 4 + 2 + sr.data.size();
	}

	WriteLE(f, dataSize);
	WriteLE(f, rec.flags);
	WriteLE(f, rec.formId);
	WriteLE(f, rec.timestamp);
	WriteLE(f, rec.versionControl);
	WriteLE(f, (uint16_t)0); // Internal version

	for (const auto& sr : rec.subrecords) {
		WriteSubrecord(f, sr);
	}
}

void ESPWriter::WriteSubrecord(std::ofstream& f, const Subrecord& sr) {
	f.write(sr.type.c_str(), 4);
	WriteLE(f, (uint16_t)sr.data.size());
	f.write(reinterpret_cast<const char*>(sr.data.data()), sr.data.size());
}

Record ESPWriter::CreateARMO(uint32_t formId, const std::string& editorId, const std::string& fullName) {
	Record r;
	r.type = "ARMO";
	r.formId = formId;

	Subrecord edid;
	edid.type = "EDID";
	edid.data.assign(editorId.begin(), editorId.end());
	edid.data.push_back('\0');
	r.subrecords.push_back(edid);

	Subrecord full;
	full.type = "FULL";
	full.data.assign(fullName.begin(), fullName.end());
	full.data.push_back('\0');
	r.subrecords.push_back(full);

	return r;
}

Record ESPWriter::CreateARMA(uint32_t formId, const std::string& editorId, const std::string& modelPath, uint32_t bodySlotFlags) {
	Record r;
	r.type = "ARMA";
	r.formId = formId;

	Subrecord edid;
	edid.type = "EDID";
	edid.data.assign(editorId.begin(), editorId.end());
	edid.data.push_back('\0');
	r.subrecords.push_back(edid);

	Subrecord mod2;
	mod2.type = "MOD2";
	mod2.data.assign(modelPath.begin(), modelPath.end());
	mod2.data.push_back('\0');
	r.subrecords.push_back(mod2);

	return r;
}

} // namespace esp
