/*
 * Skyrim SE/AE ESP/ESM binary reader.
 *
 * Reads ARMO, LVLI, OTFT records from Skyrim mod ESPs.
 * Supports compressed records (zlib) used by Skyrim SE/AE.
 *
 * Ported from modular-leveled-lists/molli/esp_reader.py.
 */

#include "ESPReader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <zlib.h>

namespace esp {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ReadString(const std::vector<uint8_t>& data) {
	// Strip trailing NUL
	size_t len = data.size();
	while (len > 0 && data[len - 1] == 0)
		--len;
	return std::string(reinterpret_cast<const char*>(data.data()), len);
}

template <typename T>
static T ReadLE(const uint8_t* p) {
	T val;
	std::memcpy(&val, p, sizeof(T));
	return val;
}

static bool ReadExact(std::ifstream& f, uint8_t* buf, size_t n) {
	f.read(reinterpret_cast<char*>(buf), n);
	return static_cast<size_t>(f.gcount()) == n;
}

static bool ReadExact(std::ifstream& f, std::vector<uint8_t>& buf, size_t n) {
	buf.resize(n);
	return ReadExact(f, buf.data(), n);
}

// ---------------------------------------------------------------------------
// Subrecord parsing
// ---------------------------------------------------------------------------

static std::vector<Subrecord> ParseSubrecords(const uint8_t* data, size_t len) {
	std::vector<Subrecord> result;
	size_t pos = 0;

	while (pos < len) {
		if (pos + 6 > len)
			break;

		std::string srType(reinterpret_cast<const char*>(data + pos), 4);
		uint16_t srSize = ReadLE<uint16_t>(data + pos + 4);
		pos += 6;

		// Handle XXXX extended size marker
		if (srType == "XXXX") {
			if (pos + 4 > len)
				break;
			uint32_t actualSize = ReadLE<uint32_t>(data + pos);
			pos += srSize;

			if (pos + 6 > len)
				break;
			srType = std::string(reinterpret_cast<const char*>(data + pos), 4);
			pos += 6; // skip next subrecord's type+size header
			srSize = 0; // unused — actualSize replaces it
			(void)srSize;

			if (pos + actualSize > len)
				break;

			Subrecord sr;
			sr.type = srType;
			sr.data.assign(data + pos, data + pos + actualSize);
			pos += actualSize;
			result.push_back(std::move(sr));
			continue;
		}

		if (pos + srSize > len)
			break;

		Subrecord sr;
		sr.type = srType;
		sr.data.assign(data + pos, data + pos + srSize);
		pos += srSize;
		result.push_back(std::move(sr));
	}

	return result;
}

// ---------------------------------------------------------------------------
// Record methods
// ---------------------------------------------------------------------------

std::string Record::EditorId() const {
	auto* sr = GetSubrecord("EDID");
	if (!sr)
		return {};
	return ReadString(sr->data);
}

std::string Record::FullName() const {
	auto* sr = GetSubrecord("FULL");
	if (!sr)
		return {};
	// LSTRING: if exactly 4 bytes, it's a string table index
	if (sr->data.size() == 4) {
		uint32_t idx = ReadLE<uint32_t>(sr->data.data());
		char buf[20];
		snprintf(buf, sizeof(buf), "[LSTRING:%08X]", idx);
		return buf;
	}
	return ReadString(sr->data);
}

std::vector<const Subrecord*> Record::GetSubrecords(const std::string& srType) const {
	std::vector<const Subrecord*> result;
	for (auto& sr : subrecords) {
		if (sr.type == srType)
			result.push_back(&sr);
	}
	return result;
}

const Subrecord* Record::GetSubrecord(const std::string& srType) const {
	for (auto& sr : subrecords) {
		if (sr.type == srType)
			return &sr;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// ArmorRecord::BodySlots
// ---------------------------------------------------------------------------

std::vector<int> ArmorRecord::BodySlots() const {
	std::vector<int> slots;
	for (int bit = 0; bit < 32; ++bit) {
		if (bodySlotFlags & (1u << bit))
			slots.push_back(30 + bit);
	}
	return slots;
}

// ---------------------------------------------------------------------------
// Record reading (internal)
// ---------------------------------------------------------------------------

enum class ReadResult { Record, Group, Eof, Error };

struct ReadOutput {
	ReadResult result;
	Record record;
	GroupHeader group;
};

static ReadOutput ReadRecord(std::ifstream& f) {
	ReadOutput out{};

	uint8_t typeBytes[4];
	if (!ReadExact(f, typeBytes, 4)) {
		out.result = ReadResult::Eof;
		return out;
	}

	std::string recType(reinterpret_cast<const char*>(typeBytes), 4);

	if (recType == "GRUP") {
		uint8_t rest[20];
		if (!ReadExact(f, rest, 20)) {
			out.result = ReadResult::Error;
			return out;
		}
		out.result = ReadResult::Group;
		out.group.groupSize = ReadLE<uint32_t>(rest + 0);
		std::memcpy(out.group.label, rest + 4, 4);
		out.group.groupType = ReadLE<uint32_t>(rest + 8);
		out.group.timestamp = ReadLE<uint16_t>(rest + 12);
		out.group.versionControl = ReadLE<uint16_t>(rest + 14);
		return out;
	}

	// Regular record: 20 bytes after type
	uint8_t rest[20];
	if (!ReadExact(f, rest, 20)) {
		out.result = ReadResult::Error;
		return out;
	}

	out.result = ReadResult::Record;
	out.record.type = recType;
	out.record.dataSize = ReadLE<uint32_t>(rest + 0);
	out.record.flags = ReadLE<uint32_t>(rest + 4);
	out.record.formId = ReadLE<uint32_t>(rest + 8);
	out.record.timestamp = ReadLE<uint16_t>(rest + 12);
	out.record.versionControl = ReadLE<uint16_t>(rest + 14);
	out.record.internalVersion = ReadLE<uint16_t>(rest + 16);

	// Read record data
	std::vector<uint8_t> rawData;
	if (!ReadExact(f, rawData, out.record.dataSize)) {
		out.result = ReadResult::Error;
		return out;
	}

	// Handle compressed records
	if (out.record.IsCompressed() && rawData.size() >= 4) {
		uint32_t decompSize = ReadLE<uint32_t>(rawData.data());
		std::vector<uint8_t> decompressed(decompSize);

		uLongf destLen = decompSize;
		int ret = uncompress(decompressed.data(), &destLen, rawData.data() + 4, static_cast<uLong>(rawData.size() - 4));
		if (ret == Z_OK) {
			decompressed.resize(destLen);
			rawData = std::move(decompressed);
		}
		// If decompression fails, keep raw data
	}

	out.record.subrecords = ParseSubrecords(rawData.data(), rawData.size());
	return out;
}

// ---------------------------------------------------------------------------
// Parse helpers
// ---------------------------------------------------------------------------

static ArmorRecord ParseArmor(const Record& rec) {
	ArmorRecord ar;
	ar.formId = rec.formId;
	ar.editorId = rec.EditorId();
	ar.fullName = rec.FullName();

	// DNAM: armor rating (uint32, value * 100 in Skyrim SE/AE)
	if (auto* dnam = rec.GetSubrecord("DNAM")) {
		if (dnam->data.size() >= 4)
			ar.armorRating = ReadLE<uint32_t>(dnam->data.data()) / 100.0f;
	}

	// BOD2: body template (first 4 bytes = body part flags, next 4 = armor type)
	uint32_t armorTypeVal = 0;
	if (auto* bod2 = rec.GetSubrecord("BOD2")) {
		if (bod2->data.size() >= 8) {
			ar.bodySlotFlags = ReadLE<uint32_t>(bod2->data.data());
			armorTypeVal = ReadLE<uint32_t>(bod2->data.data() + 4);
		}
	}
	switch (armorTypeVal) {
		case 0: ar.armorType = "Light Armor"; break;
		case 1: ar.armorType = "Heavy Armor"; break;
		default: ar.armorType = "Clothing"; break;
	}

	// EITM: enchantment FormID
	if (auto* eitm = rec.GetSubrecord("EITM")) {
		if (eitm->data.size() >= 4)
			ar.enchantmentId = ReadLE<uint32_t>(eitm->data.data());
	}

	// EAMT: enchantment amount
	if (auto* eamt = rec.GetSubrecord("EAMT")) {
		if (eamt->data.size() >= 2)
			ar.enchantAmount = ReadLE<uint16_t>(eamt->data.data());
	}

	// TNAM: template armor FormID
	if (auto* tnam = rec.GetSubrecord("TNAM")) {
		if (tnam->data.size() >= 4)
			ar.templateId = ReadLE<uint32_t>(tnam->data.data());
	}

	// KWDA: keywords
	if (auto* kwda = rec.GetSubrecord("KWDA")) {
		size_t count = kwda->data.size() / 4;
		ar.keywords.reserve(count);
		for (size_t i = 0; i < count; ++i)
			ar.keywords.push_back(ReadLE<uint32_t>(kwda->data.data() + i * 4));
	}

	// DATA: value (4 bytes uint) + weight (4 bytes float)
	if (auto* data = rec.GetSubrecord("DATA")) {
		if (data->data.size() >= 8) {
			ar.value = ReadLE<uint32_t>(data->data.data());
			ar.weight = ReadLE<float>(data->data.data() + 4);
		}
	}

	// MOD2: female model path
	if (auto* mod2 = rec.GetSubrecord("MOD2"))
		ar.modelFemale = ReadString(mod2->data);

	// MOD4: male model path
	if (auto* mod4 = rec.GetSubrecord("MOD4"))
		ar.modelMale = ReadString(mod4->data);

	// MODL: armature (ARMA) FormID references
	for (auto* modl : rec.GetSubrecords("MODL")) {
		if (modl->data.size() == 4)
			ar.armatureIds.push_back(ReadLE<uint32_t>(modl->data.data()));
	}

	// RNAM: race FormID
	if (auto* rnam = rec.GetSubrecord("RNAM")) {
		if (rnam->data.size() >= 4)
			ar.raceId = ReadLE<uint32_t>(rnam->data.data());
	}

	return ar;
}

/// Parse an alternate texture array (MO2S, MO3S, MO4S, MO5S).
/// Format: count (uint32), then count entries of:
///   name_len (uint32), name (char[name_len]), txst_formid (uint32), 3d_index (uint32)
static std::vector<AlternateTexture> ParseAlternateTextures(const Subrecord& sr) {
	std::vector<AlternateTexture> result;
	if (sr.data.size() < 4)
		return result;

	uint32_t count = ReadLE<uint32_t>(sr.data.data());
	size_t off = 4;
	for (uint32_t i = 0; i < count; ++i) {
		if (off + 4 > sr.data.size())
			break;
		uint32_t nameLen = ReadLE<uint32_t>(sr.data.data() + off);
		off += 4;
		if (off + nameLen + 8 > sr.data.size())
			break;

		AlternateTexture at;
		at.shapeName = std::string(reinterpret_cast<const char*>(sr.data.data() + off), nameLen);
		// Trim null terminators
		while (!at.shapeName.empty() && at.shapeName.back() == '\0')
			at.shapeName.pop_back();
		off += nameLen;
		at.texSetFormId = ReadLE<uint32_t>(sr.data.data() + off);
		at.index3D = ReadLE<uint32_t>(sr.data.data() + off + 4);
		off += 8;

		result.push_back(std::move(at));
	}
	return result;
}

static ArmorAddonRecord ParseArmorAddon(const Record& rec) {
	ArmorAddonRecord aa;
	aa.formId = rec.formId;
	aa.editorId = rec.EditorId();

	// BOD2: body slot flags
	if (auto* bod2 = rec.GetSubrecord("BOD2")) {
		if (bod2->data.size() >= 4)
			aa.bodySlotFlags = ReadLE<uint32_t>(bod2->data.data());
	}

	// MOD2: male 3rd person model
	if (auto* mod2 = rec.GetSubrecord("MOD2"))
		aa.modelMale = ReadString(mod2->data);

	// MOD3: female 3rd person model (this is the key one for outfit preview)
	if (auto* mod3 = rec.GetSubrecord("MOD3"))
		aa.modelFemale = ReadString(mod3->data);

	// MO2S: male model alternate textures
	if (auto* mo2s = rec.GetSubrecord("MO2S"))
		aa.altTexMale = ParseAlternateTextures(*mo2s);

	// MO3S: female model alternate textures
	if (auto* mo3s = rec.GetSubrecord("MO3S"))
		aa.altTexFemale = ParseAlternateTextures(*mo3s);

	// RNAM: race
	if (auto* rnam = rec.GetSubrecord("RNAM")) {
		if (rnam->data.size() >= 4)
			aa.raceId = ReadLE<uint32_t>(rnam->data.data());
	}

	return aa;
}

static TextureSetRecord ParseTextureSet(const Record& rec) {
	TextureSetRecord ts;
	ts.formId = rec.formId;
	ts.editorId = rec.EditorId();

	const char* txNames[] = {"TX00", "TX01", "TX02", "TX03", "TX04", "TX05", "TX06", "TX07"};
	for (int i = 0; i < 8; ++i) {
		if (auto* sr = rec.GetSubrecord(txNames[i]))
			ts.textures[i] = ReadString(sr->data);
	}
	return ts;
}

static LeveledItemRecord ParseLeveledItem(const Record& rec) {
	LeveledItemRecord li;
	li.formId = rec.formId;
	li.editorId = rec.EditorId();

	// LVLF: flags
	if (auto* lvlf = rec.GetSubrecord("LVLF")) {
		if (!lvlf->data.empty())
			li.flags = lvlf->data[0];
	}

	// LVLD: chance none
	if (auto* lvld = rec.GetSubrecord("LVLD")) {
		if (!lvld->data.empty())
			li.chanceNone = lvld->data[0];
	}

	// LVLG: global
	if (auto* lvlg = rec.GetSubrecord("LVLG")) {
		if (lvlg->data.size() >= 4)
			li.globalId = ReadLE<uint32_t>(lvlg->data.data());
	}

	// LVLO entries: level:2, padding:2, ref:4, count:2, padding:2
	for (auto* lvlo : rec.GetSubrecords("LVLO")) {
		if (lvlo->data.size() >= 12) {
			LeveledItemEntry entry;
			entry.level = ReadLE<uint16_t>(lvlo->data.data());
			entry.reference = ReadLE<uint32_t>(lvlo->data.data() + 4);
			entry.count = ReadLE<uint16_t>(lvlo->data.data() + 8);
			li.entries.push_back(entry);
		}
	}

	return li;
}

static OutfitRecord ParseOutfit(const Record& rec) {
	OutfitRecord outfit;
	outfit.formId = rec.formId;
	outfit.editorId = rec.EditorId();

	// INAM: list of FormIDs (4 bytes each)
	if (auto* inam = rec.GetSubrecord("INAM")) {
		size_t count = inam->data.size() / 4;
		outfit.items.reserve(count);
		for (size_t i = 0; i < count; ++i)
			outfit.items.push_back(ReadLE<uint32_t>(inam->data.data() + i * 4));
	}

	return outfit;
}

// ---------------------------------------------------------------------------
// ESPReader
// ---------------------------------------------------------------------------

bool ESPReader::Load(const std::string& filepath, const std::set<std::string>& recordTypes) {
	records.clear();
	recordsByType.clear();
	masters.clear();

	// Extract filename
	auto slashPos = filepath.find_last_of("/\\");
	filename = (slashPos != std::string::npos) ? filepath.substr(slashPos + 1) : filepath;

	std::ifstream f(filepath, std::ios::binary);
	if (!f.is_open())
		return false;

	// Get file size
	f.seekg(0, std::ios::end);
	auto fileSize = f.tellg();
	f.seekg(0, std::ios::beg);

	// Read TES4 header
	auto headerOut = ReadRecord(f);
	if (headerOut.result != ReadResult::Record || headerOut.record.type != "TES4")
		return false;

	// Extract master list from TES4 header
	for (auto* mast : headerOut.record.GetSubrecords("MAST")) {
		masters.push_back(ReadString(mast->data));
	}

	// Read all records
	while (f.tellg() < fileSize && f.good()) {
		auto out = ReadRecord(f);
		if (out.result == ReadResult::Eof)
			break;
		if (out.result == ReadResult::Error)
			break;
		if (out.result == ReadResult::Group)
			continue; // GRUP headers are containers; records follow inline

		// Filter by record type if requested
		if (!recordTypes.empty() && recordTypes.find(out.record.type) == recordTypes.end())
			continue;

		uint32_t fid = out.record.formId;
		std::string rtype = out.record.type;
		recordsByType[rtype].push_back(fid);
		records.emplace(fid, std::move(out.record));
	}

	return true;
}

std::vector<ArmorRecord> ESPReader::GetArmors() const {
	std::vector<ArmorRecord> result;
	auto it = recordsByType.find("ARMO");
	if (it == recordsByType.end())
		return result;

	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end())
			result.push_back(ParseArmor(rit->second));
	}
	return result;
}

std::vector<ArmorAddonRecord> ESPReader::GetArmorAddons() const {
	std::vector<ArmorAddonRecord> result;
	auto it = recordsByType.find("ARMA");
	if (it == recordsByType.end())
		return result;

	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end())
			result.push_back(ParseArmorAddon(rit->second));
	}
	return result;
}

std::vector<TextureSetRecord> ESPReader::GetTextureSets() const {
	std::vector<TextureSetRecord> result;
	auto it = recordsByType.find("TXST");
	if (it == recordsByType.end())
		return result;

	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end())
			result.push_back(ParseTextureSet(rit->second));
	}
	return result;
}

std::vector<LeveledItemRecord> ESPReader::GetLeveledItems() const {
	std::vector<LeveledItemRecord> result;
	auto it = recordsByType.find("LVLI");
	if (it == recordsByType.end())
		return result;

	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end())
			result.push_back(ParseLeveledItem(rit->second));
	}
	return result;
}

std::vector<OutfitRecord> ESPReader::GetOutfits() const {
	std::vector<OutfitRecord> result;
	auto it = recordsByType.find("OTFT");
	if (it == recordsByType.end())
		return result;

	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end())
			result.push_back(ParseOutfit(rit->second));
	}
	return result;
}

const Record* ESPReader::GetRecordByFormId(uint32_t formId) const {
	auto it = records.find(formId);
	return (it != records.end()) ? &it->second : nullptr;
}

std::pair<std::string, uint32_t> ESPReader::ResolveFormId(uint32_t localFormId) const {
	uint8_t masterIdx = (localFormId >> 24) & 0xFF;
	uint32_t baseId = localFormId & 0x00FFFFFF;

	if (masterIdx < masters.size())
		return {masters[masterIdx], baseId};
	else if (masterIdx == masters.size())
		return {filename, baseId};
	else {
		char buf[32];
		snprintf(buf, sizeof(buf), "Unknown[%u]", masterIdx);
		return {buf, baseId};
	}
}

} // namespace esp
