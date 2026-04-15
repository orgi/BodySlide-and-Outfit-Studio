/*
 * LeveledListData — resolves ESP records into browseable outfit data.
 */

#include "LeveledListData.h"
#include "../../lib/FSEngine/FSEngine.h"
#include "../../lib/FSEngine/FSManager.h"
#include "../files/ESPReader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>

namespace lldata {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ToLower(const std::string& s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
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

/// Recursive helper: resolve components[idx..] starting from currentDir.
/// Tries exact match first, then all other case-insensitive matches (handles
/// duplicate dirs like Clothes/ and clothes/ on a case-sensitive filesystem).
static std::string ResolveCaseInsensitiveImpl(const wxString& currentDir, const std::vector<std::string>& components, size_t idx) {
	if (idx >= components.size())
		return {};

	wxString target = wxString::FromUTF8(components[idx]);
	wxString targetLower = target.Lower();
	bool isLast = (idx == components.size() - 1);

	// Try exact match first (fast path)
	wxString exact = currentDir + target;
	if (isLast) {
		if (wxFileName::FileExists(exact))
			return exact.ToStdString();
	}
	else {
		if (wxFileName::DirExists(exact)) {
			std::string result = ResolveCaseInsensitiveImpl(exact + "/", components, idx + 1);
			if (!result.empty())
				return result;
			// exact dir exists but path didn't resolve — fall through to try other case variants
		}
	}

	// Case-insensitive scan — try ALL matches (handles duplicates like Clothes/clothes)
	wxDir dir(currentDir);
	if (!dir.IsOpened())
		return {};

	wxString entry;
	bool hasEntry = dir.GetFirst(&entry, wxEmptyString, wxDIR_FILES | wxDIR_DIRS | wxDIR_HIDDEN);
	while (hasEntry) {
		if (entry.Lower() == targetLower && entry != target) { // skip exact (already tried)
			wxString candidate = currentDir + entry;
			if (isLast) {
				if (wxFileName::FileExists(candidate))
					return candidate.ToStdString();
			}
			else {
				if (wxFileName::DirExists(candidate)) {
					std::string result = ResolveCaseInsensitiveImpl(candidate + "/", components, idx + 1);
					if (!result.empty())
						return result;
				}
			}
		}
		hasEntry = dir.GetNext(&entry);
	}

	return {};
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

	wxString base = wxString::FromUTF8(baseDir);
	if (!base.EndsWith("/") && !base.EndsWith("\\"))
		base += "/";

	return ResolveCaseInsensitiveImpl(base, components, 0);
}

// ---------------------------------------------------------------------------
// LoadNPCs — load NPC_ records from vanilla ESMs
// ---------------------------------------------------------------------------

void LeveledListData::LoadNPCs() {
	npcs.clear();
	if (baseDataPath.empty())
		return;

	static const char* vanillaESMs[] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm", nullptr};

	for (int i = 0; vanillaESMs[i]; ++i) {
		std::string esmPath = baseDataPath + vanillaESMs[i];
		if (!wxFileName::FileExists(wxString::FromUTF8(esmPath))) {
			esmPath = ResolveCaseInsensitive(baseDataPath, vanillaESMs[i]);
			if (esmPath.empty())
				continue;
		}

		esp::ESPReader reader;
		if (!reader.Load(esmPath, {"NPC_"})) {
			wxLogWarning("LeveledListData::LoadNPCs: failed to load %s", esmPath);
			continue;
		}

		size_t countBefore = npcs.size();
		for (auto& npc : reader.GetNPCs()) {
			if (npc.editorId.empty())
				continue;

			NPCEntry entry;
			entry.editorId = npc.editorId;
			entry.formId = npc.formId;
			entry.plugin = vanillaESMs[i];
			entry.wnamFormId = npc.wnamFormId;

			// FULL is LSTRING for most vanilla NPCs; show inline name when available
			if (!npc.fullName.empty() && npc.fullName[0] != '[')
				entry.displayName = npc.fullName + " [" + npc.editorId + "]";
			else
				entry.displayName = npc.editorId;

			npcs.push_back(std::move(entry));
		}
		wxLogMessage("LeveledListData::LoadNPCs: %zu NPCs from %s", npcs.size() - countBefore, vanillaESMs[i]);
	}

	std::sort(npcs.begin(), npcs.end(), [](const NPCEntry& a, const NPCEntry& b) { return a.displayName < b.displayName; });

	wxLogMessage("LeveledListData::LoadNPCs: %zu NPCs total", npcs.size());
}

// ---------------------------------------------------------------------------
// LoadRecordsFromESP — load records from a single file into caches
// ---------------------------------------------------------------------------

void LeveledListData::LoadRecordsFromESP(const std::string& filepath, const std::set<std::string>& types, uint8_t masterIndex, const std::vector<std::string>& mainMasters) {
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

	wxLogMessage("LeveledListData: Loaded %s (masterIndex=%d, selfIndex=%d, %zu ARMOs, %zu ARMAs, %zu TXSTs)",
				 wxString(filepath),
				 masterIndex,
				 selfIndex,
				 reader.GetArmors().size(),
				 reader.GetArmorAddons().size(),
				 reader.GetTextureSets().size());

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
		cam.bodySlotFlags = aa.bodySlotFlags;
		for (auto& at : aa.altTexFemale) {
			CachedAlternateTexture cat;
			cat.shapeName = at.shapeName;
			cat.txstFormId = remapFid(at.texSetFormId);
			cat.index3D = at.index3D;
			cam.altTexFemale.push_back(std::move(cat));
		}
		for (auto& at : aa.altTexMale) {
			CachedAlternateTexture cat;
			cat.shapeName = at.shapeName;
			cat.txstFormId = remapFid(at.texSetFormId);
			cat.index3D = at.index3D;
			cam.altTexMale.push_back(std::move(cat));
		}
		armaCache.emplace(remappedId, std::move(cam));
	}

	// Cache TXST records
	for (auto& ts : reader.GetTextureSets()) {
		uint32_t remappedId = remapFid(ts.formId);
		if (txstCache.find(remappedId) != txstCache.end())
			continue;
		CachedTXST ct;
		ct.editorId = ts.editorId;
		for (int i = 0; i < 8; ++i)
			ct.textures[i] = ts.textures[i];
		txstCache.emplace(remappedId, std::move(ct));
	}

	// Cache NPC_ WNAM references (editorId → remapped WNAM FormID)
	for (auto& npc : reader.GetNPCs()) {
		if (npc.editorId.empty() || npc.wnamFormId == 0)
			continue;
		// Always overwrite: later masters/plugins take priority
		npcSkinCache[npc.editorId] = remapFid(npc.wnamFormId);
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
// ResolveModelPath — follow TNAM template chain to find models
// ---------------------------------------------------------------------------

/// Resolve the worn mesh paths and ARMA records for an ARMO.
/// Returns a list of (mesh path, ARMA pointer) pairs.
static std::vector<std::pair<std::string, const CachedARMA*>> ResolveWornMeshes(const CachedArmo& armo,
																				const std::unordered_map<uint32_t, CachedArmo>& armoCache,
																				const std::unordered_map<uint32_t, CachedARMA>& armaCache) {
	std::vector<std::pair<std::string, const CachedARMA*>> results;
	std::set<std::string> seenNifs;

	// Add an ARMA's model to results. If femaleOnly is true, only use modelFemale.
	auto addArma = [&](uint32_t armaId, bool femaleOnly) {
		auto it = armaCache.find(armaId);
		if (it == armaCache.end()) {
			if (!femaleOnly)
				wxLogWarning("  ARMA %08X not found in cache", armaId);
			return;
		}
		const std::string& model = !it->second.modelFemale.empty() ? it->second.modelFemale
		                         : (femaleOnly ? std::string{} : it->second.modelMale);
		if (!model.empty()) {
			std::string normalized = NormalizeMeshPath(model);
			if (seenNifs.insert(normalized).second)
				results.push_back({model, &it->second});
		}
	};

	// Two-pass collection for each set of ARMAs:
	// Pass 1 — female models only (avoids loading male-only race variants for female previews)
	// Pass 2 — male fallback only if pass 1 found nothing
	auto collectArmatures = [&](const std::vector<uint32_t>& ids) {
		for (uint32_t armaId : ids)
			addArma(armaId, true);
		if (results.empty()) {
			for (uint32_t armaId : ids)
				addArma(armaId, false);
		}
	};

	// Try this ARMO's ARMA references first
	collectArmatures(armo.armatureIds);

	// If no meshes found, follow template chain (enchanted copies inherit from base armor)
	if (results.empty()) {
		uint32_t tid = armo.templateId;
		for (int depth = 0; depth < 10 && tid != 0; ++depth) {
			auto tit = armoCache.find(tid);
			if (tit == armoCache.end())
				break;

			collectArmatures(tit->second.armatureIds);

			if (!results.empty())
				break;

			tid = tit->second.templateId;
		}
	}

	return results;
}

/// Build OutfitPiece(s) from a cached ARMO, resolving worn meshes through ARMA.
static void AddPiecesFromArmo(uint32_t formId,
							  const CachedArmo& armo,
							  const std::unordered_map<uint32_t, CachedArmo>& armoCache,
							  const std::unordered_map<uint32_t, CachedARMA>& armaCache,
							  const std::unordered_map<uint32_t, CachedTXST>& txstCache,
							  std::vector<OutfitPiece>& outPieces) {
	auto meshes = ResolveWornMeshes(armo, armoCache, armaCache);

	if (meshes.empty()) {
		// Add a single piece with no mesh so it's visible as "missing" in logs
		OutfitPiece piece;
		piece.formId = formId;
		piece.name = armo.fullName;
		piece.armorType = armo.armorType;
		piece.bodySlots = DecodeBodySlots(armo.bodySlotFlags);
		outPieces.push_back(std::move(piece));
		return;
	}

	for (auto& [modelPath, arma] : meshes) {
		OutfitPiece piece;
		piece.formId = formId;
		piece.name = armo.fullName;
		piece.armorType = armo.armorType;
		// Use ARMA-level body slot flags so each piece only claims the slots
		// its own mesh actually covers, rather than the broader ARMO flags.
		piece.bodySlots = DecodeBodySlots(arma ? arma->bodySlotFlags : armo.bodySlotFlags);
		piece.nifPath = NormalizeMeshPath(modelPath);

		// Resolve alternate textures from the matched ARMA
		if (arma) {
			auto& altTexList = arma->altTexFemale.empty() ? arma->altTexMale : arma->altTexFemale;
			for (auto& at : altTexList) {
				auto txstIt = txstCache.find(at.txstFormId);
				if (txstIt == txstCache.end())
					continue;
				TextureOverride ovr;
				ovr.shapeName = at.shapeName;
				for (int i = 0; i < 8; ++i)
					ovr.textures[i] = txstIt->second.textures[i];
				piece.textureOverrides.push_back(std::move(ovr));
			}
		}
		outPieces.push_back(std::move(piece));
	}
}

// ---------------------------------------------------------------------------
// LoadESP
// ---------------------------------------------------------------------------

bool LeveledListData::LoadESP(const std::string& filepath) {
	outfits.clear();
	armoCache.clear();
	armaCache.clear();
	txstCache.clear();
	lvliCache.clear();
	otftCache.clear();
	npcSkinCache.clear();
	npcSkinOverrides.clear();
	npcSkinsScanned = false;
	espMasters.clear();
	loadInfo.clear();

	espDirectory = DirectoryOf(filepath);

	// Load the main ESP first
	esp::ESPReader mainReader;
	if (!mainReader.Load(filepath, {"ARMO", "ARMA", "TXST", "LVLI", "OTFT"}))
		return false;

	espFilename = mainReader.GetFilename();

	// Load master files (in order, so FormID indices are correct)
	// Masters are loaded from the same directory as the main ESP,
	// or from the game data path.
	// Also load NPC_ records from masters for skin texture resolution.
	auto masters = mainReader.GetMasters();
	espMasters = masters; // Store for FormID remapping in skin texture resolution
	int mastersLoaded = 0;
	for (size_t mi = 0; mi < masters.size(); ++mi) {
		auto& masterName = masters[mi];
		uint8_t masterIdx = static_cast<uint8_t>(mi);

		// Try same directory as the ESP
		std::string masterPath = espDirectory + masterName;
		if (wxFileName::FileExists(masterPath)) {
			LoadRecordsFromESP(masterPath, {"ARMO", "ARMA", "TXST", "NPC_"}, masterIdx, masters);
			++mastersLoaded;
			continue;
		}

		// Try game data path
		if (!baseDataPath.empty()) {
			masterPath = baseDataPath + masterName;
			if (wxFileName::FileExists(masterPath)) {
				LoadRecordsFromESP(masterPath, {"ARMO", "ARMA", "TXST", "NPC_"}, masterIdx, masters);
				++mastersLoaded;
				continue;
			}
		}

		wxLogWarning("LeveledListData: Master not found: %s (index %zu)", wxString(masterName), mi);
	}

	wxLogMessage("LeveledListData: After loading %d/%zu masters: %zu ARMOs, %zu ARMAs in cache", mastersLoaded, masters.size(), armoCache.size(), armaCache.size());

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
		ca.armatureIds = ar.armatureIds;	  // no remapping needed — main ESP's own FormIDs
		armoCache[ar.formId] = std::move(ca); // overwrite any master version
	}

	for (auto& aa : mainReader.GetArmorAddons()) {
		CachedARMA cam;
		cam.editorId = aa.editorId;
		cam.modelMale = aa.modelMale;
		cam.modelFemale = aa.modelFemale;
		cam.bodySlotFlags = aa.bodySlotFlags;
		for (auto& at : aa.altTexFemale) {
			CachedAlternateTexture cat;
			cat.shapeName = at.shapeName;
			cat.txstFormId = at.texSetFormId;
			cat.index3D = at.index3D;
			cam.altTexFemale.push_back(std::move(cat));
		}
		for (auto& at : aa.altTexMale) {
			CachedAlternateTexture cat;
			cat.shapeName = at.shapeName;
			cat.txstFormId = at.texSetFormId;
			cat.index3D = at.index3D;
			cam.altTexMale.push_back(std::move(cat));
		}
		armaCache[aa.formId] = std::move(cam);
	}

	for (auto& ts : mainReader.GetTextureSets()) {
		CachedTXST ct;
		ct.editorId = ts.editorId;
		for (int i = 0; i < 8; ++i)
			ct.textures[i] = ts.textures[i];
		txstCache[ts.formId] = std::move(ct);
	}

	for (auto& li : mainReader.GetLeveledItems()) {
		CachedLVLI cl;
		cl.editorId = li.editorId;
		cl.flags = li.flags;
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
	int groupCounter = 0;

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
				std::vector<OutfitPiece> resolved;
				AddPiecesFromArmo(itemId, armoIt->second, armoCache, armaCache, txstCache, resolved);
				for (auto& p : resolved) {
					entry.pieces.push_back(std::move(p));
					++totalPieces;
				}
				continue;
			}

			// LVLI reference? Resolve with Use Any group tracking
			auto lvliIt = lvliCache.find(itemId);
			if (lvliIt != lvliCache.end()) {
				std::vector<OutfitPiece> resolvedPieces;
				std::vector<std::pair<uint32_t, uint16_t>> unresolvedRefs;
				ResolveLVLIGrouped(itemId, 1, -1, -1, groupCounter, resolvedPieces, unresolvedRefs);

				// Deduplicate ARMOs: within each (group, variant), keep first occurrence of each FormID.
				// Across different variants, the same ARMO is expected (variants share common pieces).
				{
					std::set<std::tuple<int, int, uint32_t, std::string>> seen;
					std::vector<OutfitPiece> deduped;
					for (auto& p : resolvedPieces) {
						auto key = std::make_tuple(p.useAnyGroup, p.useAnyVariant, p.formId, p.nifPath);
						if (seen.insert(key).second)
							deduped.push_back(std::move(p));
					}
					resolvedPieces = std::move(deduped);
				}

				for (auto& piece : resolvedPieces) {
					if (piece.useAnyGroup >= 0) {
						wxLogMessage("  LVLI piece '%s' [%08X]: group=%d, variant=%d", wxString(piece.name), piece.formId, piece.useAnyGroup, piece.useAnyVariant);
					}
					entry.pieces.push_back(std::move(piece));
					++totalPieces;
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
	std::sort(outfits.begin(), outfits.end(), [](const OutfitEntry& a, const OutfitEntry& b) { return a.name < b.name; });

	// Build load info string
	char buf[256];
	snprintf(buf,
			 sizeof(buf),
			 "%zu outfits, %d pieces (%d unresolved), %zu ARMOs, %zu ARMAs, "
			 "%zu LVLIs, %zu masters (%d loaded)",
			 outfits.size(),
			 totalPieces,
			 unresolvedCount,
			 armoCache.size(),
			 armaCache.size(),
			 lvliCache.size(),
			 masters.size(),
			 mastersLoaded);
	loadInfo = buf;
	wxLogMessage("LeveledListData: %s", loadInfo);

	return true;
}

// ---------------------------------------------------------------------------
// ResolveLVLI — recursive LVLI → ARMO resolution
// ---------------------------------------------------------------------------

void LeveledListData::ResolveLVLI(uint32_t formId,
								  uint16_t parentLevel,
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
// ResolveLVLIGrouped — recursive LVLI → ARMO resolution with Use Any tracking
// ---------------------------------------------------------------------------

void LeveledListData::ResolveLVLIGrouped(uint32_t formId,
										 uint16_t parentLevel,
										 int parentGroup,
										 int parentVariant,
										 int& groupCounter,
										 std::vector<OutfitPiece>& pieces,
										 std::vector<std::pair<uint32_t, uint16_t>>& unresolvedRefs) const {
	auto it = lvliCache.find(formId);
	if (it == lvliCache.end())
		return;

	auto& lvli = it->second;
	bool isUseAll = (lvli.flags & 0x04) != 0;

	if (isUseAll) {
		// Use All: include all entries, preserve parent group/variant
		for (auto& [ref, level] : lvli.entries) {
			uint16_t effectiveLevel = std::max(parentLevel, level);

			auto armoIt = armoCache.find(ref);
			if (armoIt != armoCache.end()) {
				std::vector<OutfitPiece> resolved;
				AddPiecesFromArmo(ref, armoIt->second, armoCache, armaCache, txstCache, resolved);
				for (auto& piece : resolved) {
					piece.useAnyGroup = parentGroup;
					piece.useAnyVariant = parentVariant;
					pieces.push_back(std::move(piece));
				}
				continue;
			}

			auto lvliIt2 = lvliCache.find(ref);
			if (lvliIt2 != lvliCache.end()) {
				ResolveLVLIGrouped(ref, effectiveLevel, parentGroup, parentVariant, groupCounter, pieces, unresolvedRefs);
				continue;
			}

			unresolvedRefs.emplace_back(ref, effectiveLevel);
		}
	}
	else {
		// Use Any: each entry is an alternative variant — assign a new group
		int groupId = groupCounter++;
		int variantIdx = 0;
		wxLogMessage("  LVLI '%s' [%08X] is Use Any → group %d with %zu variants", wxString(lvli.editorId), formId, groupId, lvli.entries.size());

		for (auto& [ref, level] : lvli.entries) {
			uint16_t effectiveLevel = std::max(parentLevel, level);

			auto armoIt = armoCache.find(ref);
			if (armoIt != armoCache.end()) {
				std::vector<OutfitPiece> resolved;
				AddPiecesFromArmo(ref, armoIt->second, armoCache, armaCache, txstCache, resolved);
				for (auto& piece : resolved) {
					piece.useAnyGroup = groupId;
					piece.useAnyVariant = variantIdx;
					pieces.push_back(std::move(piece));
				}
				++variantIdx;
				continue;
			}

			auto lvliIt2 = lvliCache.find(ref);
			if (lvliIt2 != lvliCache.end()) {
				// Recurse: the sub-LVLI's entries inherit this group + variant
				ResolveLVLIGrouped(ref, effectiveLevel, groupId, variantIdx, groupCounter, pieces, unresolvedRefs);
				++variantIdx;
				continue;
			}

			unresolvedRefs.emplace_back(ref, effectiveLevel);
			++variantIdx;
		}
	}
}

// ---------------------------------------------------------------------------
// Filtering and search
// ---------------------------------------------------------------------------

std::vector<const OutfitEntry*> LeveledListData::FilterByLevel(uint16_t minLevel, uint16_t maxLevel) const {
	std::vector<const OutfitEntry*> result;
	for (auto& entry : outfits) {
		if (entry.minLevel >= minLevel && entry.minLevel <= maxLevel)
			result.push_back(&entry);
	}
	return result;
}

std::vector<const OutfitEntry*> LeveledListData::SearchByName(const std::string& query) const {
	std::vector<const OutfitEntry*> result;
	if (query.empty()) {
		for (auto& entry : outfits)
			result.push_back(&entry);
		return result;
	}

	std::string lowerQuery = ToLower(query);
	for (auto& entry : outfits) {
		if (ToLower(entry.name).find(lowerQuery) != std::string::npos || ToLower(entry.editorId).find(lowerQuery) != std::string::npos) {
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

// ---------------------------------------------------------------------------
// Skin texture resolution for NPC body display
// ---------------------------------------------------------------------------

std::array<std::string, 8> LeveledListData::ResolveSkinTextures(uint32_t wnamFormId) const {
	std::array<std::string, 8> textures{};
	if (wnamFormId == 0)
		return textures;

	auto armoIt = armoCache.find(wnamFormId);
	if (armoIt == armoCache.end()) {
		wxLogMessage("ResolveSkinTextures: WNAM ARMO %08X not in cache", wnamFormId);
		return textures;
	}

	auto& armo = armoIt->second;

	// Find an ARMA covering body slot 32 (bit 2)
	for (uint32_t armaId : armo.armatureIds) {
		auto armaIt = armaCache.find(armaId);
		if (armaIt == armaCache.end())
			continue;

		auto& arma = armaIt->second;
		if (!(arma.bodySlotFlags & (1u << 2))) // bit 2 = slot 32 = Body
			continue;

		// Use female alternate textures (fallback to male)
		auto& altTexList = arma.altTexFemale.empty() ? arma.altTexMale : arma.altTexFemale;
		if (altTexList.empty()) {
			// No alternate textures — try the ARMA's own model NIF textures
			// This is the common case for vanilla skin ARMAs
			wxLogMessage("ResolveSkinTextures: ARMA %08X has body slot but no alt textures", armaId);
			continue;
		}

		for (auto& at : altTexList) {
			auto txstIt = txstCache.find(at.txstFormId);
			if (txstIt == txstCache.end())
				continue;

			for (int i = 0; i < 8; ++i) {
				if (!txstIt->second.textures[i].empty())
					textures[i] = txstIt->second.textures[i];
			}
			wxLogMessage("ResolveSkinTextures: Found body textures from ARMA %08X (%s), TXST %08X (%s): diffuse='%s'",
						 armaId,
						 arma.editorId,
						 at.txstFormId,
						 txstIt->second.editorId,
						 textures[0]);
			return textures;
		}
	}

	wxLogMessage("ResolveSkinTextures: No body ARMA with textures found for WNAM %08X", wnamFormId);
	return textures;
}

// ---------------------------------------------------------------------------
// ResolveBodyNifPaths — get body/hands/feet NIF paths from NPC skin ARMO
// ---------------------------------------------------------------------------

LeveledListData::BodyNifPaths LeveledListData::ResolveBodyNifPaths(uint32_t wnamFormId, bool highWeight) const {
	BodyNifPaths result;
	if (wnamFormId == 0)
		return result;

	auto armoIt = armoCache.find(wnamFormId);
	if (armoIt == armoCache.end()) {
		wxLogMessage("ResolveBodyNifPaths: WNAM ARMO %08X not in cache", wnamFormId);
		return result;
	}

	const std::string suffix = highWeight ? "_1.nif" : "_0.nif";

	for (uint32_t armaId : armoIt->second.armatureIds) {
		auto armaIt = armaCache.find(armaId);
		if (armaIt == armaCache.end())
			continue;

		auto& arma = armaIt->second;
		const std::string& model = arma.modelFemale.empty() ? arma.modelMale : arma.modelFemale;
		if (model.empty())
			continue;

		// Normalise model path: backslash → slash, ensure meshes/ prefix
		std::string path = NormalizeMeshPath(model);

		// Replace _1.nif / _0.nif suffix according to requested weight.
		// Models are usually stored as _1.nif in the ESP; swap if needed.
		if (path.size() >= 6) {
			if (path.substr(path.size() - 6) == "_1.nif" || path.substr(path.size() - 6) == "_0.nif")
				path = path.substr(0, path.size() - 6) + suffix;
		}

		// Assign to the right slot by body slot flags
		// bit 2 = slot 32 (body), bit 3 = slot 33 (hands), bit 7 = slot 37 (feet)
		if ((arma.bodySlotFlags & (1u << 2)) && result.body.empty()) {
			result.body = path;
			wxLogMessage("ResolveBodyNifPaths: body  → %s", path);
		}
		if ((arma.bodySlotFlags & (1u << 3)) && result.hands.empty()) {
			result.hands = path;
			wxLogMessage("ResolveBodyNifPaths: hands → %s", path);
		}
		if ((arma.bodySlotFlags & (1u << 7)) && result.feet.empty()) {
			result.feet = path;
			wxLogMessage("ResolveBodyNifPaths: feet  → %s", path);
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// ScanAllPluginsForNpcSkins — scan every ESP/ESM in Data dir for NPC_ WNAM
// ---------------------------------------------------------------------------

void LeveledListData::ScanAllPluginsForNpcSkins() {
	npcSkinOverrides.clear();
	npcSkinsScanned = true;

	if (baseDataPath.empty()) {
		wxLogMessage("ScanAllPluginsForNpcSkins: no baseDataPath set");
		return;
	}

	// Build lookup: lowercase master name → index in espMasters
	std::unordered_map<std::string, uint8_t> masterNameToIdx;
	for (size_t i = 0; i < espMasters.size(); ++i)
		masterNameToIdx[ToLower(espMasters[i])] = static_cast<uint8_t>(i);

	// Scan all plugin files in Data directory
	wxArrayString pluginFiles;
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &pluginFiles, "*.esp", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &pluginFiles, "*.esm", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &pluginFiles, "*.esl", wxDIR_FILES);

	size_t pluginsWithNpcs = 0;

	for (auto& pluginWx : pluginFiles) {
		std::string pluginPath = pluginWx.ToStdString();

		esp::ESPReader reader;
		if (!reader.Load(pluginPath, {"NPC_"}))
			continue;

		auto npcs = reader.GetNPCs();
		if (npcs.empty())
			continue;
		++pluginsWithNpcs;

		auto& srcMasters = reader.GetMasters();
		uint8_t selfIdx = static_cast<uint8_t>(srcMasters.size());

		// Check if this ESP is itself a master of the generated ESP
		std::string pluginName = wxFileName(pluginWx).GetFullName().ToStdString();
		auto selfMasterIt = masterNameToIdx.find(ToLower(pluginName));
		bool isMaster = (selfMasterIt != masterNameToIdx.end());
		uint8_t selfMasterIdx = isMaster ? selfMasterIt->second : 0;

		// Build remap: source ESP master idx → generated ESP master idx
		std::unordered_map<uint8_t, uint8_t> masterRemap;
		for (size_t i = 0; i < srcMasters.size(); ++i) {
			auto it = masterNameToIdx.find(ToLower(srcMasters[i]));
			if (it != masterNameToIdx.end())
				masterRemap[static_cast<uint8_t>(i)] = it->second;
		}

		for (auto& npc : npcs) {
			if (npc.editorId.empty() || npc.wnamFormId == 0)
				continue;

			uint8_t topByte = (npc.wnamFormId >> 24) & 0xFF;
			uint32_t baseId = npc.wnamFormId & 0x00FFFFFF;

			NpcSkinInfo info;
			info.sourcePlugin = pluginPath;

			if (topByte == selfIdx) {
				// Self-defined ARMO
				if (isMaster) {
					// This ESP is a master of the generated ESP → can remap
					info.remappedWnam = (static_cast<uint32_t>(selfMasterIdx) << 24) | baseId;
					info.selfDefined = false;
				}
				else {
					// Not a master → need on-demand loading
					info.wnamRaw = npc.wnamFormId;
					info.selfDefined = true;
				}
			}
			else {
				// References a master
				auto remapIt = masterRemap.find(topByte);
				if (remapIt != masterRemap.end()) {
					info.remappedWnam = (static_cast<uint32_t>(remapIt->second) << 24) | baseId;
					info.selfDefined = false;
				}
				else {
					// Can't remap — store raw and resolve on demand
					info.wnamRaw = npc.wnamFormId;
					info.selfDefined = true;
				}
			}

			// Last writer wins (later plugins override earlier ones)
			npcSkinOverrides[npc.editorId] = info;
		}
	}

	wxLogMessage("ScanAllPluginsForNpcSkins: scanned %zu plugins (%zu with NPCs), %zu NPC skin overrides", pluginFiles.size(), pluginsWithNpcs, npcSkinOverrides.size());
}

// ---------------------------------------------------------------------------
// ResolveSkinTexturesForNPC — on-demand skin texture resolution per NPC
// ---------------------------------------------------------------------------

std::array<std::string, 8> LeveledListData::ResolveSkinTexturesForNPC(const std::string& npcEditorId) {
	std::array<std::string, 8> textures{};

	// Lazy scan: first call triggers the all-plugins scan
	if (!npcSkinsScanned)
		ScanAllPluginsForNpcSkins();

	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt == npcSkinOverrides.end())
		return textures;

	auto& info = ovIt->second;

	// If WNAM was remapped to main cache space, use the existing cache-based resolver
	if (!info.selfDefined) {
		textures = ResolveSkinTextures(info.remappedWnam);
		if (!textures[0].empty())
			wxLogMessage("ResolveSkinTexturesForNPC: NPC '%s' resolved from cache (WNAM %08X): %s", npcEditorId, info.remappedWnam, textures[0]);
		return textures;
	}

	// Self-defined WNAM from a non-master ESP — load the source ESP on demand
	wxLogMessage("ResolveSkinTexturesForNPC: loading '%s' for NPC '%s' WNAM raw=%08X",
				 wxFileName(wxString::FromUTF8(info.sourcePlugin)).GetFullName().ToStdString(),
				 npcEditorId,
				 info.wnamRaw);

	esp::ESPReader reader;
	if (!reader.Load(info.sourcePlugin, {"ARMO", "ARMA", "TXST"})) {
		wxLogMessage("ResolveSkinTexturesForNPC: failed to load %s", info.sourcePlugin);
		return textures;
	}

	auto armors = reader.GetArmors();
	auto armorAddons = reader.GetArmorAddons();
	auto texSets = reader.GetTextureSets();

	// Build local lookups indexed by raw FormID
	std::unordered_map<uint32_t, size_t> localArmos, localArmas, localTxsts;
	for (size_t i = 0; i < armors.size(); ++i)
		localArmos[armors[i].formId] = i;
	for (size_t i = 0; i < armorAddons.size(); ++i)
		localArmas[armorAddons[i].formId] = i;
	for (size_t i = 0; i < texSets.size(); ++i)
		localTxsts[texSets[i].formId] = i;

	// Build remap for cross-references to masters (source ESP master → espMasters index)
	auto& srcMasters = reader.GetMasters();
	uint8_t selfIdx = static_cast<uint8_t>(srcMasters.size());

	std::unordered_map<std::string, uint8_t> masterNameToIdx;
	for (size_t i = 0; i < espMasters.size(); ++i)
		masterNameToIdx[ToLower(espMasters[i])] = static_cast<uint8_t>(i);

	std::unordered_map<uint8_t, uint8_t> masterRemap;
	for (size_t i = 0; i < srcMasters.size(); ++i) {
		auto it = masterNameToIdx.find(ToLower(srcMasters[i]));
		if (it != masterNameToIdx.end())
			masterRemap[static_cast<uint8_t>(i)] = it->second;
	}

	// Remap a raw FormID from source ESP space → main cache space.
	// Returns 0 for self-defined records (must use local lookup).
	auto toMainCache = [&](uint32_t fid) -> uint32_t {
		uint8_t top = (fid >> 24) & 0xFF;
		if (top == selfIdx)
			return 0; // self-defined, use local lookup
		uint32_t base = fid & 0x00FFFFFF;
		auto it = masterRemap.find(top);
		if (it != masterRemap.end())
			return (static_cast<uint32_t>(it->second) << 24) | base;
		return 0; // can't remap
	};

	// Step 1: Find WNAM ARMO
	uint32_t wnamFid = info.wnamRaw;
	const std::vector<uint32_t>* espArmaIds = nullptr;
	const std::vector<uint32_t>* cacheArmaIds = nullptr;

	auto lArmoIt = localArmos.find(wnamFid);
	if (lArmoIt != localArmos.end()) {
		espArmaIds = &armors[lArmoIt->second].armatureIds;
	}
	if (!espArmaIds || espArmaIds->empty()) {
		uint32_t remapped = toMainCache(wnamFid);
		if (remapped != 0) {
			auto cIt = armoCache.find(remapped);
			if (cIt != armoCache.end())
				cacheArmaIds = &cIt->second.armatureIds;
		}
	}

	if ((!espArmaIds || espArmaIds->empty()) && (!cacheArmaIds || cacheArmaIds->empty())) {
		wxLogMessage("ResolveSkinTexturesForNPC: ARMO not found for WNAM %08X", wnamFid);
		return textures;
	}

	// Collect all ARMA IDs to check
	std::vector<uint32_t> allArmaIds;
	if (espArmaIds && !espArmaIds->empty())
		allArmaIds.insert(allArmaIds.end(), espArmaIds->begin(), espArmaIds->end());
	if (cacheArmaIds && !cacheArmaIds->empty())
		allArmaIds.insert(allArmaIds.end(), cacheArmaIds->begin(), cacheArmaIds->end());

	// Step 2: Find body ARMA (slot 32) with alternate textures
	for (uint32_t armaId : allArmaIds) {
		// Try local ARMA first
		auto lArmaIt = localArmas.find(armaId);
		if (lArmaIt != localArmas.end()) {
			auto& arma = armorAddons[lArmaIt->second];
			if (!(arma.bodySlotFlags & (1u << 2)))
				continue;

			auto& altTex = arma.altTexFemale.empty() ? arma.altTexMale : arma.altTexFemale;
			if (altTex.empty())
				continue;

			for (auto& at : altTex) {
				// Try local TXST
				auto lTxIt = localTxsts.find(at.texSetFormId);
				if (lTxIt != localTxsts.end()) {
					auto& ts = texSets[lTxIt->second];
					for (int i = 0; i < 8; ++i)
						if (!ts.textures[i].empty())
							textures[i] = ts.textures[i];
					if (!textures[0].empty()) {
						wxLogMessage("ResolveSkinTexturesForNPC: found textures (local): %s", textures[0]);
						return textures;
					}
				}
				// Try main cache TXST
				uint32_t remapped = toMainCache(at.texSetFormId);
				if (remapped != 0) {
					auto cIt = txstCache.find(remapped);
					if (cIt != txstCache.end()) {
						for (int i = 0; i < 8; ++i)
							if (!cIt->second.textures[i].empty())
								textures[i] = cIt->second.textures[i];
						if (!textures[0].empty()) {
							wxLogMessage("ResolveSkinTexturesForNPC: found textures (cached TXST): %s", textures[0]);
							return textures;
						}
					}
				}
			}
			continue;
		}

		// Try main cache ARMA
		uint32_t remapped = toMainCache(armaId);
		if (remapped != 0) {
			auto cArmaIt = armaCache.find(remapped);
			if (cArmaIt != armaCache.end()) {
				auto& cachedArma = cArmaIt->second;
				if (!(cachedArma.bodySlotFlags & (1u << 2)))
					continue;

				auto& altTex = cachedArma.altTexFemale.empty() ? cachedArma.altTexMale : cachedArma.altTexFemale;
				if (altTex.empty())
					continue;

				for (auto& at : altTex) {
					auto tIt = txstCache.find(at.txstFormId);
					if (tIt != txstCache.end()) {
						for (int i = 0; i < 8; ++i)
							if (!tIt->second.textures[i].empty())
								textures[i] = tIt->second.textures[i];
						if (!textures[0].empty()) {
							wxLogMessage("ResolveSkinTexturesForNPC: found textures (cached ARMA+TXST): %s", textures[0]);
							return textures;
						}
					}
				}
			}
		}
	}

	wxLogMessage("ResolveSkinTexturesForNPC: no body textures found for '%s'", npcEditorId);
	return textures;
}

} // namespace lldata
