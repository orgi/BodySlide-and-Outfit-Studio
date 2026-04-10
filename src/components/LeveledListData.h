/*
 * LeveledListData — data model for the Leveled List Previewer.
 *
 * Loads a generated ESP and resolves the LVLI → OTFT → ARMO → NIF chain
 * into a browseable list of outfits with their constituent armor pieces.
 * Supports filtering by player level and searching by name.
 */

#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lldata {

/// One NPC from a vanilla ESM, for the head preview selector.
struct NPCEntry {
	std::string displayName; // "EditorID" or "Full Name [EditorID]"
	std::string editorId;
	uint32_t formId = 0;
	std::string plugin;		 // e.g. "Skyrim.esm"
	uint32_t wnamFormId = 0; // WNAM: skin/worn armor FormID (0 = use race default)
};

/// Per-shape texture override from ARMA MO3S alternate textures.
struct TextureOverride {
	std::string shapeName;
	std::string textures[8]; // TX00-TX07 from TXST, empty = use NIF default
};

struct OutfitPiece {
	std::string name;	   // ARMO full name
	uint32_t formId = 0;   // ARMO FormID
	std::string nifPath;   // MOD2 (female model) path, relative to Data/
	std::string armorType; // "Light Armor", "Heavy Armor", "Clothing"
	std::vector<int> bodySlots;
	std::vector<TextureOverride> textureOverrides; // per-shape texture swaps from ARMA MO3S
};

struct OutfitEntry {
	std::string name;	   // OTFT editor ID (display name)
	uint32_t formId = 0;   // OTFT FormID
	std::string editorId;  // OTFT editor ID
	uint16_t minLevel = 1; // Minimum player level for this variant
	std::vector<OutfitPiece> pieces;
};

class LeveledListData {
public:
	/// Load a generated ESP file and its masters. Returns true on success.
	bool LoadESP(const std::string& filepath);

	/// Get diagnostic info about the last load.
	const std::string& GetLoadInfo() const { return loadInfo; }

	/// Set base data path for NIF resolution (Config["GameDataPath"]).
	void SetBaseDataPath(const std::string& path) { baseDataPath = path; }

	/// Get all loaded outfit entries.
	const std::vector<OutfitEntry>& GetOutfits() const { return outfits; }

	/// Load NPC_ records from vanilla ESMs found in baseDataPath.
	/// Call after SetBaseDataPath(). Results are sorted by displayName.
	void LoadNPCs();

	/// Get the loaded NPC list.
	const std::vector<NPCEntry>& GetNPCs() const { return npcs; }

	/// Get outfits filtered by player level range.
	std::vector<const OutfitEntry*> FilterByLevel(uint16_t minLevel, uint16_t maxLevel) const;

	/// Search outfits by name (case-insensitive substring match).
	std::vector<const OutfitEntry*> SearchByName(const std::string& query) const;

	/// Resolve a relative NIF path to an absolute filesystem path.
	/// Checks loose files first, then BSA/BA2 archives via FSManager.
	std::string ResolveNifPath(const std::string& relativePath) const;

	/// Case-insensitive file resolution for Linux.
	/// Walks path components from baseDir and finds case-insensitive matches on disk.
	static std::string ResolveCaseInsensitive(const std::string& baseDir, const std::string& relativePath);

	/// Get the loaded ESP filename.
	const std::string& GetFilename() const { return espFilename; }

	/// Resolve body skin textures for an NPC's WNAM (skin armor FormID).
	/// Returns an array of 8 texture paths (TX00-TX07), empty strings for unresolved slots.
	std::array<std::string, 8> ResolveSkinTextures(uint32_t wnamFormId) const;

	/// Get NPC skin cache (editorId -> remapped WNAM FormID from ESP masters).
	const std::unordered_map<std::string, uint32_t>& GetNpcSkinCache() const { return npcSkinCache; }

	/// Info about an NPC's skin override found by scanning all plugins.
	struct NpcSkinInfo {
		uint32_t remappedWnam = 0; // WNAM remapped to espMasters index space (when !selfDefined)
		uint32_t wnamRaw = 0;	   // WNAM raw FormID from source ESP (when selfDefined)
		std::string sourcePlugin;  // Full path to the ESP that defines this NPC/WNAM
		bool selfDefined = false;  // true = WNAM is self-defined in a non-master ESP
	};

	/// Scan ALL plugins in the Data directory for NPC_ WNAM overrides.
	/// Populates npcSkinOverrides. Call after LoadESP() and LoadNPCs().
	void ScanAllPluginsForNpcSkins();

	/// Resolve skin textures for a specific NPC by editorId.
	/// Uses the pre-scanned plugin data for on-demand ESP loading when needed.
	std::array<std::string, 8> ResolveSkinTexturesForNPC(const std::string& npcEditorId);

	/// Get NPC skin overrides map (editorId -> skin info from all plugins).
	const std::unordered_map<std::string, NpcSkinInfo>& GetNpcSkinOverrides() const { return npcSkinOverrides; }

private:
	/// Load records from a single ESP/ESM into the caches.
	/// masterIndex: the index of this file in the main ESP's master list.
	/// mainMasters: the master list of the main ESP, used to remap cross-references.
	void LoadRecordsFromESP(const std::string& filepath, const std::set<std::string>& types, uint8_t masterIndex, const std::vector<std::string>& mainMasters);

	/// Resolve LVLI entries recursively to collect referenced FormIDs and their levels.
	/// Collects both resolved ARMOs and unresolved FormIDs.
	void ResolveLVLI(uint32_t formId, uint16_t parentLevel, std::vector<std::pair<uint32_t, uint16_t>>& armoRefs, std::vector<std::pair<uint32_t, uint16_t>>& unresolvedRefs) const;

	/// Create a stub piece for an unresolved ARMO FormID.
	static OutfitPiece MakeStubPiece(uint32_t formId);

	std::vector<OutfitEntry> outfits;
	std::vector<NPCEntry> npcs;
	std::string baseDataPath;
	std::string espFilename;
	std::string espDirectory;
	std::string loadInfo;

	// Cached parsed records from ESP
	std::unordered_map<uint32_t, struct CachedArmo> armoCache;
	std::unordered_map<uint32_t, struct CachedARMA> armaCache;
	std::unordered_map<uint32_t, struct CachedTXST> txstCache;
	std::unordered_map<uint32_t, struct CachedLVLI> lvliCache;
	std::unordered_map<uint32_t, struct CachedOTFT> otftCache;
	std::unordered_map<std::string, uint32_t> npcSkinCache;		   // editorId → remapped WNAM FormID
	std::unordered_map<std::string, NpcSkinInfo> npcSkinOverrides; // editorId → skin info from all plugins
	std::vector<std::string> espMasters;						   // Master list of the loaded generated ESP
	bool npcSkinsScanned = false;
};

// Internal cache structs (defined in .cpp)
struct CachedArmo {
	std::string fullName;
	std::string editorId;
	std::string armorType;
	uint32_t bodySlotFlags = 0;
	std::string modelFemale;		   // MOD2 — world/ground model (NOT worn mesh)
	std::string modelMale;			   // MOD4 ��� world/ground model
	uint32_t templateId = 0;		   // TNAM — template armor FormID (0 = none)
	std::vector<uint32_t> armatureIds; // MODL — ARMA references for worn meshes
};

struct CachedAlternateTexture {
	std::string shapeName;
	uint32_t txstFormId = 0;
	uint32_t index3D = 0;
};

struct CachedARMA {
	std::string editorId;
	std::string modelMale;	 // MOD2 — male 3rd person worn mesh
	std::string modelFemale; // MOD3 — female 3rd person worn mesh
	uint32_t bodySlotFlags = 0;
	std::vector<CachedAlternateTexture> altTexFemale; // MO3S
	std::vector<CachedAlternateTexture> altTexMale;	  // MO2S
};

struct CachedTXST {
	std::string editorId;
	std::string textures[8]; // TX00-TX07
};

struct CachedLVLI {
	std::string editorId;
	std::vector<std::pair<uint32_t, uint16_t>> entries; // (reference, level)
};

struct CachedOTFT {
	std::string editorId;
	std::vector<uint32_t> items;
};

} // namespace lldata
