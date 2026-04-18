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
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#	include <windows.h>
#else
#	include <dirent.h>
#endif

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

template<typename T>
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
	if (n > 200 * 1024 * 1024) return false; // Hard 200MB limit
	try {
		buf.resize(n);
	} catch (const std::exception&) {
		return false;
	}
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
			pos += 6;	// skip next subrecord's type+size header

			// Sanity check for huge sizes (100MB)
			if (actualSize > 100 * 1024 * 1024 || pos + actualSize > len)
				break;

			Subrecord sr;
			sr.type = srType;
			try {
				sr.data.assign(data + pos, data + pos + actualSize);
			} catch (const std::exception&) {
				break;
			}
			pos += actualSize;
			result.push_back(std::move(sr));
			continue;
		}

		if (pos + srSize > len)
			break;

		Subrecord sr;
		sr.type = srType;
		try {
			sr.data.assign(data + pos, data + pos + srSize);
		} catch (const std::exception&) {
			break;
		}
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
	// LSTRING: if exactly 4 bytes, it's a string table index. Without the string
	// table we can only return a placeholder — callers that have an ESPReader should
	// use ESPReader::ResolveFullName() instead.
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

	// Sanity check for huge records (100MB)
	if (out.record.dataSize > 100 * 1024 * 1024) {
		out.result = ReadResult::Error;
		return out;
	}

	// Read record data
	std::vector<uint8_t> rawData;
	if (!ReadExact(f, rawData, out.record.dataSize)) {
		out.result = ReadResult::Error;
		return out;
	}

	// Handle compressed records
	if (out.record.IsCompressed() && rawData.size() >= 4) {
		uint32_t decompSize = ReadLE<uint32_t>(rawData.data());
		
		// Sanity check for huge decompressed size (100MB)
		if (decompSize > 100 * 1024 * 1024) {
			out.result = ReadResult::Error;
			return out;
		}

		std::vector<uint8_t> decompressed;
		try {
			decompressed.resize(decompSize);
		} catch (const std::exception&) {
			out.result = ReadResult::Error;
			return out;
		}

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
		
		// Sanity check for string length
		if (nameLen > 1024 || off + nameLen + 8 > sr.data.size())
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

static NPCRecord ParseNPC(const Record& rec) {
	NPCRecord npc;
	npc.formId = rec.formId;
	npc.editorId = rec.EditorId();
	npc.fullName = rec.FullName();

	// WNAM: skin/worn armor FormID
	if (auto* wnam = rec.GetSubrecord("WNAM")) {
		if (wnam->data.size() >= 4)
			npc.wnamFormId = ReadLE<uint32_t>(wnam->data.data());
	}

	// RNAM: race FormID
	if (auto* rnam = rec.GetSubrecord("RNAM")) {
		if (rnam->data.size() >= 4)
			npc.raceFormId = ReadLE<uint32_t>(rnam->data.data());
	}

	// QNAM: 3 floats (R, G, B) — texture lighting / face tint color
	if (auto* qnam = rec.GetSubrecord("QNAM")) {
		if (qnam->data.size() >= 12) {
			npc.qnamR = ReadLE<float>(qnam->data.data() + 0);
			npc.qnamG = ReadLE<float>(qnam->data.data() + 4);
			npc.qnamB = ReadLE<float>(qnam->data.data() + 8);
			npc.hasQnam = true;
		}
	}

	return npc;
}

static RaceRecord ParseRace(const Record& rec) {
	RaceRecord r;
	r.formId = rec.formId;
	r.editorId = rec.EditorId();

	// WNAM: default skin ARMO for this race
	if (auto* wnam = rec.GetSubrecord("WNAM")) {
		if (wnam->data.size() >= 4)
			r.skinFormId = ReadLE<uint32_t>(wnam->data.data());
	}

	return r;
}

// ---------------------------------------------------------------------------
// STRINGS file loader
//
// Localized Skyrim plugins store FULL/DESC/shortname strings in external files
// under Data/Strings/{PluginBaseName}_{Language}.STRINGS (names),
// .DLSTRINGS (long dialog) and .ILSTRINGS (item/mid). FULL for NPC_ and ARMO
// is always in .STRINGS.
//
// Format (little-endian):
//   uint32 count
//   uint32 dataSize (bytes of the string data block)
//   directory: count * { uint32 stringId; uint32 offset; }
//   data block: dataSize bytes
//     .STRINGS: null-terminated UTF-8
//     .DLSTRINGS/.ILSTRINGS: uint32 length + data (length includes NUL)
// ---------------------------------------------------------------------------

static bool FileExistsCI(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	return f.is_open();
}

// Resolve a case-insensitive file in a given directory. Returns empty string on failure.
// Linux needs this because Skyrim data is case-sensitive on ext4 but shipped mixed-case.
static std::string ResolveCIFileInDir(const std::string& dir, const std::string& fileName) {
	// Try exact path first
	std::string exact = dir + "/" + fileName;
	if (FileExistsCI(exact))
		return exact;

	// Walk the directory looking for a case-insensitive match
	std::string lowerTarget = fileName;
	for (auto& c : lowerTarget) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

#ifdef _WIN32
	return exact; // Windows is case-insensitive — if exact failed, it doesn't exist
#else
	DIR* d = opendir(dir.c_str());
	if (!d) return {};
	std::string found;
	while (auto* e = readdir(d)) {
		std::string name = e->d_name;
		std::string lower = name;
		for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (lower == lowerTarget) {
			found = dir + "/" + name;
			break;
		}
	}
	closedir(d);
	return found;
#endif
}

std::unordered_map<uint32_t, std::string> ESPReader::ParseStringsBuffer(const uint8_t* buf, size_t fileSize, bool lengthPrefixed) {
	std::unordered_map<uint32_t, std::string> out;
	if (!buf || fileSize < 8)
		return out;

	uint32_t count = ReadLE<uint32_t>(buf + 0);
	uint32_t dataSize = ReadLE<uint32_t>(buf + 4);

	size_t dirSize = size_t(count) * 8;
	if (8 + dirSize + dataSize > fileSize)
		return out;

	const uint8_t* dir = buf + 8;
	const uint8_t* dataBlock = buf + 8 + dirSize;

	for (uint32_t i = 0; i < count; ++i) {
		uint32_t stringId = ReadLE<uint32_t>(dir + i * 8);
		uint32_t offset = ReadLE<uint32_t>(dir + i * 8 + 4);
		if (offset >= dataSize)
			continue;
		const uint8_t* p = dataBlock + offset;
		size_t remaining = dataSize - offset;

		if (lengthPrefixed) {
			if (remaining < 4) continue;
			uint32_t len = ReadLE<uint32_t>(p);
			p += 4;
			remaining -= 4;
			if (len > remaining) continue;
			size_t textLen = (len > 0 && p[len - 1] == 0) ? len - 1 : len;
			out[stringId] = std::string(reinterpret_cast<const char*>(p), textLen);
		} else {
			size_t textLen = 0;
			while (textLen < remaining && p[textLen] != 0) ++textLen;
			out[stringId] = std::string(reinterpret_cast<const char*>(p), textLen);
		}
	}
	return out;
}

static std::unordered_map<uint32_t, std::string> LoadStringsFile(const std::string& path, bool lengthPrefixed) {
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open())
		return {};
	f.seekg(0, std::ios::end);
	auto pos = f.tellg();
	if (pos <= 0 || pos > 100 * 1024 * 1024) // 100MB limit for strings
		return {};

	size_t fileSize = static_cast<size_t>(pos);
	f.seekg(0, std::ios::beg);
	std::vector<uint8_t> buf;
	try {
		buf.resize(fileSize);
	} catch (const std::exception&) {
		return {};
	}

	if (!ReadExact(f, buf.data(), fileSize))
		return {};
	return ESPReader::ParseStringsBuffer(buf.data(), buf.size(), lengthPrefixed);
}

// Try to load the .STRINGS file matching a plugin. Tries English (the de-facto
// default) and, if that fails, any _*.STRINGS file that exists.
static std::unordered_map<uint32_t, std::string> LoadPluginStrings(const std::string& espFilepath) {
	std::unordered_map<uint32_t, std::string> table;

	auto slashPos = espFilepath.find_last_of("/\\");
	std::string dir = (slashPos != std::string::npos) ? espFilepath.substr(0, slashPos) : ".";
	std::string file = (slashPos != std::string::npos) ? espFilepath.substr(slashPos + 1) : espFilepath;

	// Strip extension
	auto dotPos = file.find_last_of('.');
	std::string base = (dotPos != std::string::npos) ? file.substr(0, dotPos) : file;

	std::string stringsDir = dir + "/Strings";
	std::string stringsDirCI = ResolveCIFileInDir(dir, "Strings");
	if (!stringsDirCI.empty())
		stringsDir = stringsDirCI;

	static const char* langs[] = {"English", "French", "German", "Italian", "Spanish", "Polish", "Russian", nullptr};

	for (int i = 0; langs[i]; ++i) {
		std::string target = base + "_" + langs[i] + ".STRINGS";
		std::string full = ResolveCIFileInDir(stringsDir, target);
		if (!full.empty()) {
			table = LoadStringsFile(full, /*lengthPrefixed*/ false);
			if (!table.empty())
				return table;
		}
	}
	return table;
}

std::string ESPReader::ResolveFullName(const Record& rec) const {
	auto* sr = rec.GetSubrecord("FULL");
	if (!sr)
		return {};
	if (sr->data.size() == 4) {
		uint32_t idx = ReadLE<uint32_t>(sr->data.data());
		auto it = stringTable.find(idx);
		if (it != stringTable.end())
			return it->second;
		return {}; // Unresolved localized string — treat as no name
	}
	return ReadString(sr->data);
}

// ---------------------------------------------------------------------------
// ESPReader
// ---------------------------------------------------------------------------

bool ESPReader::Load(const std::string& filepath, const std::set<std::string>& recordTypes) {
	records.clear();
	recordsByType.clear();
	masters.clear();
	stringTable.clear();
	localized = false;

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

	// Detect Localized flag (TES4 record flag 0x80). When set, FULL/DESC use
	// 4-byte lstring indices into the companion .STRINGS file.
	localized = (headerOut.record.flags & 0x00000080) != 0;
	if (localized)
		stringTable = LoadPluginStrings(filepath);

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

std::vector<NPCRecord> ESPReader::GetNPCs() const {
	std::vector<NPCRecord> result;
	auto it = recordsByType.find("NPC_");
	if (it == recordsByType.end())
		return result;
	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end()) {
			auto npc = ParseNPC(rit->second);
			// Override with lstring-resolved name when this ESP is localized.
			npc.fullName = ResolveFullName(rit->second);
			result.push_back(std::move(npc));
		}
	}
	return result;
}

std::vector<RaceRecord> ESPReader::GetRaces() const {
	std::vector<RaceRecord> result;
	auto it = recordsByType.find("RACE");
	if (it == recordsByType.end())
		return result;
	for (uint32_t fid : it->second) {
		auto rit = records.find(fid);
		if (rit != records.end())
			result.push_back(ParseRace(rit->second));
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
