/*
 * Skyrim SE/AE ESP/ESM binary writer.
 *
 * Supports writing TES4, ARMO, ARMA records.
 */

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include "ESPReader.h"

namespace esp {

class ESPWriter {
public:
	ESPWriter();
	~ESPWriter() {}

	bool Load(const std::string& filepath);
	void AddMaster(const std::string& masterName);
	
	// Adds or updates a record.
	uint32_t AddRecord(const Record& rec);

	// Creates a copy of a record with a new FormID.
	static Record CloneRecord(const Record& src, uint32_t newFormId);

	const Record* GetRecordByEditorId(const std::string& edid) const;

	bool Save(const std::string& filepath);

	// Helpers for creating records from scratch
	static Record CreateARMO(uint32_t formId, const std::string& editorId, const std::string& fullName);
	static Record CreateARMA(uint32_t formId, const std::string& editorId, const std::string& modelPath, uint32_t bodySlotFlags);

private:
	std::vector<std::string> masters;
	std::unordered_map<uint32_t, Record> recordMap;
	uint32_t nextFormId = 0x800; // Starting FormID for new records in the ESP

	void WriteRecord(std::ofstream& f, const Record& rec);
	void WriteSubrecord(std::ofstream& f, const Subrecord& sr);
	
	template<typename T>
	void WriteLE(std::ofstream& f, T val) {
		f.write(reinterpret_cast<const char*>(&val), sizeof(T));
	}
};

} // namespace esp
