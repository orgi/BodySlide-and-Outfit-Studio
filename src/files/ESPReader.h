/*
 * Skyrim SE/AE ESP/ESM binary reader.
 *
 * Reads ARMO, LVLI, OTFT records from Skyrim mod ESPs.
 * Supports compressed records (zlib) used by Skyrim SE/AE.
 *
 * Ported from modular-leveled-lists/molli/esp_reader.py.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace esp {

// ---------------------------------------------------------------------------
// Low-level record structures
// ---------------------------------------------------------------------------

struct Subrecord {
	std::string type; // 4-char type code
	std::vector<uint8_t> data;
};

struct Record {
	std::string type;
	uint32_t dataSize = 0;
	uint32_t flags = 0;
	uint32_t formId = 0;
	uint16_t timestamp = 0;
	uint16_t versionControl = 0;
	uint16_t internalVersion = 0;
	std::vector<Subrecord> subrecords;

	bool IsCompressed() const { return (flags & 0x00040000) != 0; }

	std::string EditorId() const;
	std::string FullName() const;

	std::vector<const Subrecord*> GetSubrecords(const std::string& srType) const;
	const Subrecord* GetSubrecord(const std::string& srType) const;
};

struct GroupHeader {
	uint8_t label[4]{};
	uint32_t groupSize = 0;
	uint32_t groupType = 0;
	uint16_t timestamp = 0;
	uint16_t versionControl = 0;
};

// ---------------------------------------------------------------------------
// Parsed record types
// ---------------------------------------------------------------------------

struct ArmorRecord {
	uint32_t formId = 0;
	std::string editorId;
	std::string fullName;
	float armorRating = 0.0f;
	std::string armorType; // "Light Armor", "Heavy Armor", "Clothing"
	std::optional<uint32_t> enchantmentId;
	uint16_t enchantAmount = 0;
	std::optional<uint32_t> templateId;
	uint32_t bodySlotFlags = 0;
	std::vector<uint32_t> keywords;
	uint32_t value = 0;
	float weight = 0.0f;
	std::string modelFemale; // MOD2 path
	std::string modelMale;   // MOD4 path
	std::vector<uint32_t> armatureIds;
	uint32_t raceId = 0x00019; // DefaultRace

	std::vector<int> BodySlots() const;
};

struct AlternateTexture {
	std::string shapeName;   // 3D shape name in the NIF
	uint32_t texSetFormId = 0; // TXST FormID
	uint32_t index3D = 0;    // 3D index
};

struct ArmorAddonRecord {
	uint32_t formId = 0;
	std::string editorId;
	std::string modelMale;    // MOD2 — male 3rd person worn mesh
	std::string modelFemale;  // MOD3 — female 3rd person worn mesh
	uint32_t bodySlotFlags = 0;
	uint32_t raceId = 0x00019;
	std::vector<AlternateTexture> altTexFemale;  // MO3S — female model alternate textures
	std::vector<AlternateTexture> altTexMale;    // MO2S — male model alternate textures
};

struct TextureSetRecord {
	uint32_t formId = 0;
	std::string editorId;
	std::string textures[8]; // TX00-TX07: diffuse, normal, glow, parallax, env, envMask, multilayer, specular
};

struct LeveledItemEntry {
	uint32_t reference = 0;
	uint16_t level = 0;
	uint16_t count = 0;
};

struct LeveledItemRecord {
	uint32_t formId = 0;
	std::string editorId;
	uint8_t flags = 0;
	uint8_t chanceNone = 0;
	std::optional<uint32_t> globalId;
	std::vector<LeveledItemEntry> entries;
};

struct OutfitRecord {
	uint32_t formId = 0;
	std::string editorId;
	std::vector<uint32_t> items;
};

struct NPCRecord {
	uint32_t formId = 0;
	std::string editorId;
	std::string fullName;
};

// ---------------------------------------------------------------------------
// ESP Reader
// ---------------------------------------------------------------------------

class ESPReader {
public:
	bool Load(const std::string& filepath, const std::set<std::string>& recordTypes = {});

	std::vector<ArmorRecord> GetArmors() const;
	std::vector<ArmorAddonRecord> GetArmorAddons() const;
	std::vector<TextureSetRecord> GetTextureSets() const;
	std::vector<LeveledItemRecord> GetLeveledItems() const;
	std::vector<OutfitRecord> GetOutfits() const;
	std::vector<NPCRecord> GetNPCs() const;

	const Record* GetRecordByFormId(uint32_t formId) const;
	std::pair<std::string, uint32_t> ResolveFormId(uint32_t localFormId) const;

	const std::vector<std::string>& GetMasters() const { return masters; }
	const std::string& GetFilename() const { return filename; }

private:
	std::unordered_map<uint32_t, Record> records;
	std::unordered_map<std::string, std::vector<uint32_t>> recordsByType; // type -> formId list
	std::vector<std::string> masters;
	std::string filename;
};

} // namespace esp
