/*
 * Skyrim SE/AE ESP/ESM binary writer.
 *
 * Supports writing TES4, ARMO, ARMA records with standards-compliant grouping.
 */

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include "ESPReader.h"

namespace esp {

class ESPWriter {
public:
	ESPWriter();
	~ESPWriter() {}

	bool Load(const std::string& filepath, const std::set<std::string>& recordTypes = {});
	void AddMaster(const std::string& masterName);
	
	uint32_t AddRecord(const Record& rec);
	static Record CloneRecord(const Record& src, uint32_t newFormId);

	const Record* GetRecordByEditorId(const std::string& edid) const;

	bool Save(const std::string& filepath);

	static Record CreateARMO(uint32_t formId, const std::string& editorId, const std::string& fullName);
	static Record CreateARMA(uint32_t formId, const std::string& editorId, const std::string& modelPath, uint32_t bodySlotFlags = 0x4);

private:
	std::vector<std::string> masters;
	std::map<uint32_t, Record> recordMap;
	uint32_t nextFormId = 0x800;

	void WriteRecord(std::ofstream& f, const Record& rec);
	void WriteSubrecord(std::ofstream& f, const Subrecord& sr);
	
	template<typename T>
	void WriteLE(std::ofstream& f, T val) {
		f.write(reinterpret_cast<const char*>(&val), sizeof(T));
	}
};

} // namespace esp
