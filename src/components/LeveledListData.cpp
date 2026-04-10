/*
 * LeveledListData — resolves ESP records into browseable outfit data.
 */

#include "LeveledListData.h"
#include "../files/ESPReader.h"
#include "../../lib/FSEngine/FSEngine.h"
#include "../../lib/FSEngine/FSManager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>

namespace lldata {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ToLower(const std::string& s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	return out;
}

static std::vector<int> DecodeBodySlots(uint32_t flags) {
	std::vector<int> slots;
	for (int bit = 0; bit < 32; ++bit) {
		if (flags & (1u << bit))
			slots.push_back(30 + bit);
	}
	return slots;
}

/// Normalize a NIF path: backslash → forward slash, ensure meshes/ prefix.
static std::string NormalizeMeshPath(const std::string& path) {
	if (path.empty())
		return {};

	std::string p = path;
	std::replace(p.begin(), p.end(), '\\', '/');

	// Strip leading slashes
	while (!p.empty() && p[0] == '/')
		p = p.substr(1);

	// Ensure meshes/ prefix
	std::string lower = ToLower(p);
	auto meshesPos = lower.find("meshes/");
	if (meshesPos != std::string::npos && meshesPos > 0) {
		p = p.substr(meshesPos);
	}
	else if (meshesPos == std::string::npos) {
		p = "meshes/" + p;
	}

	return p;
}

static std::string FormIdToString(uint32_t fid) {
	char buf[12];
	snprintf(buf, sizeof(buf), "%08X", fid);
	return buf;
}

/// Extract directory from filepath.
static std::string DirectoryOf(const std::string& filepath) {
	auto pos = filepath.find_last_of("/\\");
	if (pos != std::string::npos)
		return filepath.substr(0, pos + 1);
	return "";
}

std::string LeveledListData::ResolveCaseInsensitive(const std::string& baseDir, const std::string& relativePath) {
	if (baseDir.empty() || relativePath.empty())
		return {};

	// Split relative path into components
	std::vector<std::string> components;
	std::string remaining = relativePath;
	std::replace(remaining.begin(), remaining.end(), '\\', '/');
	size_t pos = 0;
	while ((pos = remaining.find('/')) != std::string::npos) {
		std::string comp = remaining.substr(0, pos);
		if (!comp.empty())
			components.push_back(comp);
		remaining = remaining.substr(pos + 1);
	}
	if (!remaining.empty())
		components.push_back(remaining);

	// Walk from baseDir, matching each component case-insensitively
	wxString current = wxString::FromUTF8(baseDir);
	if (!current.EndsWith("/") && !current.EndsWith("\\"))
		current += "/";

	for (size_t i = 0; i < components.size(); ++i) {
		wxString target = wxString::FromUTF8(components[i]);
		wxString targetLower = target.Lower();
		bool isLast = (i == components.size() - 1);

		// Try exact match first (fast path)
		wxString exact = current + target;
		if (isLast ? wxFileName::FileExists(exact) : wxFileName::DirExists(exact)) {
			current = exact + (isLast ? "" : "/");
			continue;
		}

		// Case-insensitive scan of current directory
		bool found = false;
		wxDir dir(current);
		if (!dir.IsOpened())
			return {};

		wxString entry;
		// Check files and dirs
		bool hasEntry = dir.GetFirst(&entry, wxEmptyString, wxDIR_FILES | wxDIR_DIRS | wxDIR_HIDDEN);
		while (hasEntry) {
			if (entry.Lower() == targetLower) {
				current = current + entry + (isLast ? "" : "/");
				found = true;
				break;
			}
			hasEntry = dir.GetNext(&entry);
		}

		if (!found)
			return {};
	}

	return current.ToStdString();
}

// ---------------------------------------------------------------------------
// LoadRecordsFromESP — load records from a single file into caches
// ---------------------------------------------------------------------------

void LeveledListData::LoadRecordsFromESP(const std::string& filepath, const std::set<std::string>& types,
										  uint8_t masterIndex, const std::vector<std::string>& mainMasters) {
	esp::ESPReader reader;
	if (!reader.Load(filepath, types)) {
		wxLogWarning("LeveledListData: Failed to load %s", filepath);
		return;
	}

	// Build FormID remapping table.
	// This file's own records have top byte == len(its masters) → remap to masterIndex.
	// Its masters' records need to be remapped to the main ESP's master indices.
	uint8_t selfIndex = static_cast<uint8_t>(reader.GetMasters().size());
	auto& fileMasters = reader.GetMasters();

	// Build a map: this file's master index → main ESP's master index
	// by matching filenames (case-insensitive).
	std::unordered_map<uint8_t, uint8_t> masterRemap;
	for (size_t fi = 0; fi < fileMasters.size(); ++fi) {
		std::string fileMasterLower = ToLower(fileMasters[fi]);
		for (size_t mi = 0; mi < mainMasters.size(); ++mi) {
			if (ToLower(mainMasters[mi]) == fileMasterLower) {
				masterRemap[static_cast<uint8_t>(fi)] = static_cast<uint8_t>(mi);
				break;
			}
		}
	}

	auto remapFid = [&](uint32_t fid) -> uint32_t {
		uint8_t topByte = (fid >> 24) & 0xFF;
		uint32_t baseId = fid & 0x00FFFFFF;
		if (topByte == selfIndex) {
			return (static_cast<uint32_t>(masterIndex) << 24) | baseId;
		}
		auto it = masterRemap.find(topByte);
		if (it != masterRemap.end()) {
			return (static_cast<uint32_t>(it->second) << 24) | baseId;
		}
		return fid;
	};

	wxLogMessage("LeveledListData: Loaded %s (masterIndex=%d, selfIndex=%d, %zu ARMOs, %zu ARMAs)",
				 filepath, masterIndex, selfIndex, reader.GetArmors().size(), reader.GetArmorAddons().size());

	// Cache ARMO records (don't overwrite — main ESP takes priority)
	for (auto& ar : reader.GetArmors()) {
		uint32_t remappedId = remapFid(ar.formId);
		if (armoCache.find(remappedId) != armoCache.end())
			continue;
		CachedArmo ca;
		ca.fullName = ar.fullName;
		ca.editorId = ar.editorId;
		ca.armorType = ar.armorType;
		ca.bodySlotFlags = ar.bodySlotFlags;
		ca.modelFemale = ar.modelFemale;
		ca.modelMale = ar.modelMale;
		ca.templateId = ar.templateId.has_value() ? remapFid(ar.templateId.value()) : 0;
		// Remap armature IDs
		for (uint32_t aid : ar.armatureIds)
			ca.armatureIds.push_back(remapFid(aid));
		armoCache.emplace(remappedId, std::move(ca));
	}

	// Cache ARMA records
	for (auto& aa : reader.GetArmorAddons()) {
		uint32_t remappedId = remapFid(aa.formId);
		if (armaCache.find(remappedId) != armaCache.end())
			continue;
		CachedARMA cam;
		cam.editorId = aa.editorId;
		cam.modelMale = aa.modelMale;
		cam.modelFemale = aa.modelFemale;
		armaCache.emplace(remappedId, std::move(cam));
	}
}

// ---------------------------------------------------------------------------
// MakeStubPiece — placeholder for unresolved ARMO references
// ---------------------------------------------------------------------------

OutfitPiece LeveledListData::MakeStubPiece(uint32_t formId) {
	OutfitPiece piece;
	piece.formId = formId;
	piece.name = "[" + FormIdToString(formId) + "]";
	piece.armorType = "Unknown";
	return piece;
}

// ---------------------------------------------------------------------------
// ResolveModelPath — follow TNAM template chain to find a model
// ---------------------------------------------------------------------------

/// Resolve the worn mesh path for an ARMO by following ARMO → ARMA chain.
/// Falls back to template chain (TNAM) if the ARMO has no armature IDs.
/// Prefers female model (MOD3), falls back to male (MOD2).
static std::string ResolveWornMeshPath(
	const CachedArmo& armo,
	const std::unordered_map<uint32_t, CachedArmo>& armoCache,
	const std::unordered_map<uint32_t, CachedARMA>& armaCache) {

	// Try this ARMO's ARMA references first
	for (uint32_t armaId : armo.armatureIds) {
		auto it = armaCache.find(armaId);
		if (it == armaCache.end())
			continue;
		if (!it->second.modelFemale.empty())
			return it->second.modelFemale;
		if (!it->second.modelMale.empty())
			return it->second.modelMale;
	}

	// Follow template chain (enchanted copies inherit from base armor)
	uint32_t tid = armo.templateId;
	for (int depth = 0; depth < 10 && tid != 0; ++depth) {
		auto tit = armoCache.find(tid);
		if (tit == armoCache.end())
			break;
		auto& tmpl = tit->second;

		for (uint32_t armaId : tmpl.armatureIds) {
			auto ait = armaCache.find(armaId);
			if (ait == armaCache.end())
				continue;
			if (!ait->second.modelFemale.empty())
				return ait->second.modelFemale;
			if (!ait->second.modelMale.empty())
				return ait->second.modelMale;
		}

		tid = tmpl.templateId;
	}

	// Do NOT fall back to ARMO's MOD2/MOD4 — those are ground/world models,
	// not worn meshes. Return empty so the piece is skipped rather than
	// showing a ground object (e.g. GND.nif).
	return {};
}

/// Build an OutfitPiece from a cached ARMO, resolving worn mesh through ARMA.
static OutfitPiece MakePieceFromArmo(
	uint32_t formId, const CachedArmo& armo,
	const std::unordered_map<uint32_t, CachedArmo>& armoCache,
	const std::unordered_map<uint32_t, CachedARMA>& armaCache) {
	OutfitPiece piece;
	piece.formId = formId;
	piece.name = armo.fullName;
	piece.armorType = armo.armorType;
	piece.bodySlots = DecodeBodySlots(armo.bodySlotFlags);

	std::string modelPath = ResolveWornMeshPath(armo, armoCache, armaCache);
	piece.nifPath = NormalizeMeshPath(modelPath);
	return piece;
}

// ---------------------------------------------------------------------------
// LoadESP
// ---------------------------------------------------------------------------

bool LeveledListData::LoadESP(const std::string& filepath) {
	outfits.clear();
	armoCache.clear();
	armaCache.clear();
	lvliCache.clear();
	otftCache.clear();
	loadInfo.clear();

	espDirectory = DirectoryOf(filepath);

	// Load the main ESP first
	esp::ESPReader mainReader;
	if (!mainReader.Load(filepath, {"ARMO", "ARMA", "LVLI", "OTFT"}))
		return false;

	espFilename = mainReader.GetFilename();

	// Load master files (in order, so FormID indices are correct)
	// Masters are loaded from the same directory as the main ESP,
	// or from the game data path.
	auto masters = mainReader.GetMasters();
	int mastersLoaded = 0;
	for (size_t mi = 0; mi < masters.size(); ++mi) {
		auto& masterName = masters[mi];
		uint8_t masterIdx = static_cast<uint8_t>(mi);

		// Try same directory as the ESP
		std::string masterPath = espDirectory + masterName;
		if (wxFileName::FileExists(masterPath)) {
			LoadRecordsFromESP(masterPath, {"ARMO", "ARMA"}, masterIdx, masters);
			++mastersLoaded;
			continue;
		}

		// Try game data path
		if (!baseDataPath.empty()) {
			masterPath = baseDataPath + masterName;
			if (wxFileName::FileExists(masterPath)) {
				LoadRecordsFromESP(masterPath, {"ARMO", "ARMA"}, masterIdx, masters);
				++mastersLoaded;
				continue;
			}
		}

		wxLogWarning("LeveledListData: Master not found: %s (index %zu)", masterName, mi);
	}

	// Now load the main ESP records (these take priority over masters)
	// We load after masters so the main ESP's records overwrite master records
	// Actually, let's load main ESP records into caches now
	for (auto& ar : mainReader.GetArmors()) {
		CachedArmo ca;
		ca.fullName = ar.fullName;
		ca.editorId = ar.editorId;
		ca.armorType = ar.armorType;
		ca.bodySlotFlags = ar.bodySlotFlags;
		ca.modelFemale = ar.modelFemale;
		ca.modelMale = ar.modelMale;
		ca.templateId = ar.templateId.value_or(0);
		ca.armatureIds = ar.armatureIds; // no remapping needed — main ESP's own FormIDs
		armoCache[ar.formId] = std::move(ca); // overwrite any master version
	}

	for (auto& aa : mainReader.GetArmorAddons()) {
		CachedARMA cam;
		cam.editorId = aa.editorId;
		cam.modelMale = aa.modelMale;
		cam.modelFemale = aa.modelFemale;
		armaCache[aa.formId] = std::move(cam);
	}

	for (auto& li : mainReader.GetLeveledItems()) {
		CachedLVLI cl;
		cl.editorId = li.editorId;
		for (auto& e : li.entries)
			cl.entries.emplace_back(e.reference, e.level);
		lvliCache[li.formId] = std::move(cl);
	}

	for (auto& ot : mainReader.GetOutfits()) {
		CachedOTFT co;
		co.editorId = ot.editorId;
		co.items = ot.items;
		otftCache[ot.formId] = std::move(co);
	}

	// Build outfit entries from OTFT records
	int totalPieces = 0;
	int unresolvedCount = 0;

	for (auto& [otftId, otft] : otftCache) {
		OutfitEntry entry;
		entry.formId = otftId;
		entry.editorId = otft.editorId;
		entry.name = otft.editorId;
		entry.minLevel = 1;

		for (uint32_t itemId : otft.items) {
			// Direct ARMO reference?
			auto armoIt = armoCache.find(itemId);
			if (armoIt != armoCache.end()) {
				entry.pieces.push_back(MakePieceFromArmo(itemId, armoIt->second, armoCache, armaCache));
				++totalPieces;
				continue;
			}

			// LVLI reference? Resolve recursively
			auto lvliIt = lvliCache.find(itemId);
			if (lvliIt != lvliCache.end()) {
				std::vector<std::pair<uint32_t, uint16_t>> armoRefs;
				std::vector<std::pair<uint32_t, uint16_t>> unresolvedRefs;
				ResolveLVLI(itemId, 1, armoRefs, unresolvedRefs);

				for (auto& [armoId, level] : armoRefs) {
					auto ait = armoCache.find(armoId);
					if (ait == armoCache.end()) {
						unresolvedRefs.emplace_back(armoId, level);
						continue;
					}

					entry.pieces.push_back(MakePieceFromArmo(armoId, ait->second, armoCache, armaCache));
					++totalPieces;

					if (level > entry.minLevel)
						entry.minLevel = level;
				}

				// Create stub pieces for unresolved references
				for (auto& [fid, level] : unresolvedRefs) {
					entry.pieces.push_back(MakeStubPiece(fid));
					++unresolvedCount;

					if (level > entry.minLevel)
						entry.minLevel = level;
				}
				continue;
			}

			// Neither ARMO nor LVLI — create stub
			entry.pieces.push_back(MakeStubPiece(itemId));
			++unresolvedCount;
		}

		// Always add outfit, even if all pieces are stubs
		outfits.push_back(std::move(entry));
	}

	// Sort by name
	std::sort(outfits.begin(), outfits.end(),
			  [](const OutfitEntry& a, const OutfitEntry& b) {
				  return a.name < b.name;
			  });

	// Build load info string
	char buf[256];
	snprintf(buf, sizeof(buf),
			 "%zu outfits, %d pieces (%d unresolved), %zu ARMOs, %zu ARMAs, "
			 "%zu LVLIs, %zu masters (%d loaded)",
			 outfits.size(), totalPieces, unresolvedCount,
			 armoCache.size(), armaCache.size(), lvliCache.size(),
			 masters.size(), mastersLoaded);
	loadInfo = buf;
	wxLogMessage("LeveledListData: %s", loadInfo);

	return true;
}

// ---------------------------------------------------------------------------
// ResolveLVLI — recursive LVLI → ARMO resolution
// ---------------------------------------------------------------------------

void LeveledListData::ResolveLVLI(
	uint32_t formId, uint16_t parentLevel,
	std::vector<std::pair<uint32_t, uint16_t>>& armoRefs,
	std::vector<std::pair<uint32_t, uint16_t>>& unresolvedRefs) const {

	auto it = lvliCache.find(formId);
	if (it == lvliCache.end())
		return;

	for (auto& [ref, level] : it->second.entries) {
		uint16_t effectiveLevel = std::max(parentLevel, level);

		// Is this an ARMO?
		if (armoCache.find(ref) != armoCache.end()) {
			armoRefs.emplace_back(ref, effectiveLevel);
			continue;
		}

		// Is this a nested LVLI?
		if (lvliCache.find(ref) != lvliCache.end()) {
			ResolveLVLI(ref, effectiveLevel, armoRefs, unresolvedRefs);
			continue;
		}

		// Unresolved reference (ARMO in a master we couldn't load)
		unresolvedRefs.emplace_back(ref, effectiveLevel);
	}
}

// ---------------------------------------------------------------------------
// Filtering and search
// ---------------------------------------------------------------------------

std::vector<const OutfitEntry*> LeveledListData::FilterByLevel(
	uint16_t minLevel, uint16_t maxLevel) const {

	std::vector<const OutfitEntry*> result;
	for (auto& entry : outfits) {
		if (entry.minLevel >= minLevel && entry.minLevel <= maxLevel)
			result.push_back(&entry);
	}
	return result;
}

std::vector<const OutfitEntry*> LeveledListData::SearchByName(
	const std::string& query) const {

	std::vector<const OutfitEntry*> result;
	if (query.empty()) {
		for (auto& entry : outfits)
			result.push_back(&entry);
		return result;
	}

	std::string lowerQuery = ToLower(query);
	for (auto& entry : outfits) {
		if (ToLower(entry.name).find(lowerQuery) != std::string::npos ||
			ToLower(entry.editorId).find(lowerQuery) != std::string::npos) {
			result.push_back(&entry);
		}
	}
	return result;
}

// ---------------------------------------------------------------------------
// NIF path resolution
// ---------------------------------------------------------------------------

std::string LeveledListData::ResolveNifPath(const std::string& relativePath) const {
	if (relativePath.empty())
		return {};

	// Try loose file first (exact case)
	std::string fullPath = baseDataPath + relativePath;
	if (wxFileName::FileExists(fullPath))
		return fullPath;

	// Try loose file with case-insensitive resolution (needed on Linux)
	std::string resolved = ResolveCaseInsensitive(baseDataPath, relativePath);
	if (!resolved.empty())
		return resolved;

	// Try BSA/BA2 archives (already case-insensitive internally)
	for (FSArchiveFile* archive : FSManager::archiveList()) {
		if (archive && archive->hasFile(relativePath))
			return relativePath; // FSManager can load it from archive
	}

	return {};
}

} // namespace lldata
