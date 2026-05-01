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
// Load a plugin's .STRINGS file from BSA archives. Used when the game ships
// strings only in Skyrim - Interface.bsa / Patch.bsa (the default on English
// installs — there are no loose .STRINGS files on disk).
// Returns an empty map on failure.
// ---------------------------------------------------------------------------
static std::unordered_map<uint32_t, std::string> LoadStringsFromBSA(const std::string& pluginFilename) {
	// pluginFilename like "Skyrim.esm" → base "Skyrim"
	std::string base = pluginFilename;
	auto dot = base.find_last_of('.');
	if (dot != std::string::npos)
		base = base.substr(0, dot);

	static const char* langs[] = {"English", "French", "German", "Italian", "Spanish", "Polish", "Russian", nullptr};
	for (int i = 0; langs[i]; ++i) {
		std::string relPath = "strings/" + base + "_" + langs[i] + ".strings";
		for (FSArchiveFile* archive : FSManager::archiveList()) {
			if (!archive)
				continue;
			if (!archive->hasFile(relPath))
				continue;
			wxMemoryBuffer buf;
			if (!archive->fileContents(relPath, buf))
				continue;
			return esp::ESPReader::ParseStringsBuffer(reinterpret_cast<const uint8_t*>(buf.GetData()),
													  buf.GetDataLen(),
													  /*lengthPrefixed*/ false);
		}
	}
	return {};
}

// ---------------------------------------------------------------------------
// LoadNPCs — load NPC_ records from vanilla ESMs
// ---------------------------------------------------------------------------

void LeveledListData::LoadNPCs() {
	npcs.clear();
	if (baseDataPath.empty())
		return;

	static const char* vanillaESMs[] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm", nullptr};

	// Pass 1: read NPC_ + RACE from each vanilla ESM, remember enough context to
	// resolve cross-ESM race references in Pass 2.
	struct PendingNPC {
		NPCEntry entry;
		uint32_t rawRaceFormId = 0;		 // RNAM FormID in the source ESM's own space
		const char* sourceEsm = nullptr; // ESM that defined this NPC
	};
	struct EsmRaces {
		std::vector<std::string> masters;						  // source ESM's master list
		std::unordered_map<uint32_t, std::string> raceEdidByBase; // base FormID (no top byte) → editor id
	};

	std::vector<PendingNPC> pending;
	std::unordered_map<std::string, EsmRaces> esmRacesByName; // ESM name → races defined there
	// Track (nativePlugin, baseFormId) already added so later scans (which see the same
	// NPC as an override record) don't produce duplicate entries. Maps to the index
	// in `pending` so we can append override plugins to the existing entry.
	std::map<std::pair<std::string, uint32_t>, size_t> seenNpcs;

	for (int i = 0; vanillaESMs[i]; ++i) {
		std::string esmPath = baseDataPath + vanillaESMs[i];
		if (!wxFileName::FileExists(wxString::FromUTF8(esmPath))) {
			esmPath = ResolveCaseInsensitive(baseDataPath, vanillaESMs[i]);
			if (esmPath.empty())
				continue;
		}

		esp::ESPReader reader;
		if (!reader.Load(esmPath, {"NPC_", "RACE", "HDPT"})) {
			wxLogWarning("LeveledListData::LoadNPCs: failed to load %s", esmPath);
			continue;
		}

		// For localized vanilla ESMs the FULL names live in Data/Strings/*.STRINGS.
		// Those files are usually not loose on English installs — they're packed
		// inside the vanilla BSAs. If the reader couldn't find a loose file, pull
		// the strings out of the BSA archives that FSManager has already indexed.
		if (reader.IsLocalized() && reader.GetStringTable().empty()) {
			auto table = LoadStringsFromBSA(vanillaESMs[i]);
			if (!table.empty()) {
				wxLogMessage("LeveledListData::LoadNPCs: loaded %zu strings for %s from BSA", table.size(), vanillaESMs[i]);
				reader.SetStringTable(std::move(table));
			}
			else {
				wxLogWarning("LeveledListData::LoadNPCs: %s is localized but no .STRINGS file found (loose or BSA) — NPC names will be empty", vanillaESMs[i]);
			}
		}

		EsmRaces er;
		er.masters = reader.GetMasters();
		for (auto& r : reader.GetRaces()) {
			if (!r.editorId.empty())
				er.raceEdidByBase[r.formId & 0x00FFFFFF] = r.editorId;
		}
		esmRacesByName[vanillaESMs[i]] = std::move(er);

		// Cache HDPT records from this ESM
		for (auto& hp : reader.GetHeadParts()) {
			CachedHeadPart chp;
			chp.editorId = hp.editorId;
			chp.model = hp.model;
			chp.type = hp.type;
			// Remap native formId to main-ESP master index space if possible.
			// Vanilla ESMs are guaranteed to be in espMasters if the generated ESP is valid.
			// For LoadNPCs (vanilla scan), we can use a temporary remapping.
			uint32_t baseId = hp.formId & 0x00FFFFFF;
			uint32_t remappedFid = (static_cast<uint32_t>(i) << 24) | baseId;
			headPartCache[remappedFid] = std::move(chp);
		}

		size_t countBefore = npcs.size() + pending.size();
		auto& readerMasters = reader.GetMasters();
		for (auto& npc : reader.GetNPCs()) {
			if (npc.editorId.empty())
				continue;

			// Determine the NATIVE plugin for this NPC — the one that owns the FaceGen
			// NIF file. Top byte of the raw formId indexes the scanned ESM's master list
			// (len(masters) == self). If top byte < len(masters) this is an OVERRIDE of
			// an NPC defined in one of our masters, not a native record, and its FaceGen
			// file lives under that master's folder.
			uint8_t topByte = (npc.formId >> 24) & 0xFF;
			std::string nativePlugin;
			if (topByte < readerMasters.size())
				nativePlugin = readerMasters[topByte];
			else
				nativePlugin = vanillaESMs[i]; // native to the scanned ESM

			uint32_t baseFormId = npc.formId & 0x00FFFFFF;
			auto seenIt = seenNpcs.find({nativePlugin, baseFormId});
			if (seenIt != seenNpcs.end()) {
				// Already added from an earlier scan. Just append this plugin as
				// another override so LoadHeadMesh has all candidates.
				auto& existing = pending[seenIt->second];
				existing.entry.overrides.emplace_back(vanillaESMs[i], npc.formId);
				continue;
			}

			PendingNPC pn;
			pn.entry.editorId = npc.editorId;
			pn.entry.formId = npc.formId;
			pn.entry.plugin = nativePlugin;
			pn.entry.wnamFormId = npc.wnamFormId;
			pn.rawRaceFormId = npc.raceFormId;
			pn.sourceEsm = vanillaESMs[i];
			// Seed the override list with this plugin's native record.
			pn.entry.overrides.emplace_back(vanillaESMs[i], npc.formId);

			// Map head parts from the source ESM's local FormID space to our indexed master space.
			auto remapLocalFid = [&](uint32_t localFid) -> uint32_t {
				uint8_t tb = (localFid >> 24) & 0xFF;
				uint32_t bid = localFid & 0x00FFFFFF;
				if (tb == readerMasters.size())
					return (static_cast<uint32_t>(i) << 24) | bid;

				std::string mName = ToLower(readerMasters[tb]);
				for (int j = 0; vanillaESMs[j]; ++j) {
					if (ToLower(vanillaESMs[j]) == mName)
						return (static_cast<uint32_t>(j) << 24) | bid;
				}
				return 0;
			};
			for (uint32_t hpFid : npc.headParts) {
				uint32_t remapped = remapLocalFid(hpFid);
				if (remapped != 0)
					pn.entry.headParts.push_back(remapped);
			}

			// Full in-game name (may be empty, or an unresolved lstring marker like "[1234]")
			if (!npc.fullName.empty() && npc.fullName[0] != '[')
				pn.entry.fullName = npc.fullName;

			// Display: "In-game Name (plugin.esm, EditorID)" — fall back to editor id only
			// when the NPC has no readable FULL name. Autocomplete still matches on editor id
			// via the fallback branch in OnHeadEntered.
			const std::string& shownName = !pn.entry.fullName.empty() ? pn.entry.fullName : npc.editorId;
			pn.entry.displayName = shownName + " (" + vanillaESMs[i] + ", " + npc.editorId + ")";

			// QNAM face tint — quantize floats to 0-255 for shader use.
			if (npc.hasQnam) {
				auto clamp8 = [](float v) -> uint8_t {
					if (v < 0.0f)
						v = 0.0f;
					if (v > 1.0f)
						v = 1.0f;
					return static_cast<uint8_t>(v * 255.0f + 0.5f);
				};
				pn.entry.tintR = clamp8(npc.qnamR);
				pn.entry.tintG = clamp8(npc.qnamG);
				pn.entry.tintB = clamp8(npc.qnamB);
				pn.entry.hasTint = true;
			}

			seenNpcs[{nativePlugin, baseFormId}] = pending.size();
			pending.push_back(std::move(pn));
		}
		wxLogMessage("LeveledListData::LoadNPCs: %zu NPCs from %s", (npcs.size() + pending.size()) - countBefore, vanillaESMs[i]);
	}

	// Scan every non-vanilla plugin in Data/ for NPC_ override records so each
	// NPCEntry knows every plugin that touches it. Later, LoadHeadMesh walks the
	// override chain (last plugin first) to find the FaceGen NIF generated by
	// whichever plugin last modified the face.
	{
		std::set<std::string> vanillaLower;
		for (int i = 0; vanillaESMs[i]; ++i)
			vanillaLower.insert(ToLower(vanillaESMs[i]));

		wxArrayString allPlugins;
		wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &allPlugins, "*.esp", wxDIR_FILES);
		wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &allPlugins, "*.esm", wxDIR_FILES);
		wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &allPlugins, "*.esl", wxDIR_FILES);

		size_t scannedPlugins = 0;
		size_t appendedOverrides = 0;

		for (size_t pi = 0; pi < allPlugins.size(); ++pi) {
			std::string pluginPath = allPlugins[pi].ToStdString();
			wxFileName pluginFn(allPlugins[pi]);
			std::string pluginFile = pluginFn.GetFullName().ToStdString();
			if (vanillaLower.count(ToLower(pluginFile)))
				continue;

			esp::ESPReader reader;
			if (!reader.Load(pluginPath, {"NPC_"}))
				continue;

			++scannedPlugins;
			auto& readerMasters = reader.GetMasters();
			for (auto& npc : reader.GetNPCs()) {
				if (npc.editorId.empty())
					continue;

				// Only care about records that override an NPC defined in one of the
				// plugin's masters — those are the ones whose FaceGen this plugin may
				// have regenerated. Top byte == len(masters) means the NPC is native
				// to this plugin (i.e. a new NPC_ record), not relevant here.
				uint8_t topByte = (npc.formId >> 24) & 0xFF;
				if (topByte >= readerMasters.size())
					continue;

				const std::string& nativePlugin = readerMasters[topByte];
				uint32_t baseFormId = npc.formId & 0x00FFFFFF;

				auto it = seenNpcs.find({nativePlugin, baseFormId});
				if (it == seenNpcs.end())
					continue;

				auto& existing = pending[it->second];
				// Skip duplicates (this plugin is already in the chain).
				bool dup = false;
				for (auto& ov : existing.entry.overrides) {
					if (ToLower(ov.first) == ToLower(pluginFile) && ov.second == npc.formId) {
						dup = true;
						break;
					}
				}
				if (!dup) {
					existing.entry.overrides.emplace_back(pluginFile, npc.formId);
					++appendedOverrides;
				}
			}
		}
		wxLogMessage("LeveledListData::LoadNPCs: scanned %zu non-vanilla plugins, appended %zu override records", scannedPlugins, appendedOverrides);
	}

	// Pass 2: resolve each NPC's RNAM → race editor id.
	// The top byte of rawRaceFormId indexes the source ESM's own masters list; the base
	// FormID lives either in one of those masters (self-defined races in that ESM), or
	// in the NPC's own ESM if top == len(masters).
	for (auto& pn : pending) {
		if (pn.rawRaceFormId != 0 && pn.sourceEsm) {
			auto esmIt = esmRacesByName.find(pn.sourceEsm);
			if (esmIt != esmRacesByName.end()) {
				auto& er = esmIt->second;
				uint8_t topByte = (pn.rawRaceFormId >> 24) & 0xFF;
				uint32_t baseId = pn.rawRaceFormId & 0x00FFFFFF;

				const std::string* raceSourceEsm = nullptr;
				if (topByte < er.masters.size())
					raceSourceEsm = &er.masters[topByte];
				else if (topByte == er.masters.size())
					raceSourceEsm = nullptr; // self-defined in pn.sourceEsm

				const EsmRaces* targetEr = nullptr;
				if (raceSourceEsm) {
					auto it2 = esmRacesByName.find(*raceSourceEsm);
					if (it2 != esmRacesByName.end())
						targetEr = &it2->second;
				}
				else {
					targetEr = &er;
				}

				if (targetEr) {
					auto rIt = targetEr->raceEdidByBase.find(baseId);
					if (rIt != targetEr->raceEdidByBase.end())
						pn.entry.raceEditorId = rIt->second;
				}
			}
		}
		npcs.push_back(std::move(pn.entry));
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

	// Cache ARMO records. Overwrite: in Bethesda load order, later masters
	// override earlier ones for the same (remapped) FormID.
	for (auto& ar : reader.GetArmors()) {
		uint32_t remappedId = remapFid(ar.formId);
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
		armoCache[remappedId] = std::move(ca);
	}

	// Cache ARMA records. Overwrite (later master wins).
	for (auto& aa : reader.GetArmorAddons()) {
		uint32_t remappedId = remapFid(aa.formId);
		CachedARMA cam;
		cam.editorId = aa.editorId;
		cam.modelMale = aa.modelMale;
		cam.modelFemale = aa.modelFemale;
		cam.bodySlotFlags = aa.bodySlotFlags;
		cam.raceFormId = aa.raceId != 0 ? remapFid(aa.raceId) : 0;
		cam.skinTextureMale = aa.skinTextureMale != 0 ? remapFid(aa.skinTextureMale) : 0;
		cam.skinTextureFemale = aa.skinTextureFemale != 0 ? remapFid(aa.skinTextureFemale) : 0;
		for (uint32_t r : aa.additionalRaces) {
			if (r != 0)
				cam.additionalRaceFormIds.push_back(remapFid(r));
		}
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
		armaCache[remappedId] = std::move(cam);
	}

	// Cache TXST records. Overwrite (later master wins).
	for (auto& ts : reader.GetTextureSets()) {
		uint32_t remappedId = remapFid(ts.formId);
		CachedTXST ct;
		ct.editorId = ts.editorId;
		for (int i = 0; i < 8; ++i)
			ct.textures[i] = ts.textures[i];
		txstCache[remappedId] = std::move(ct);
	}

	// Cache NPC_ WNAM references (editorId → remapped WNAM FormID).
	// Always overwrite: later masters/plugins take priority.
	for (auto& npc : reader.GetNPCs()) {
		if (npc.editorId.empty() || npc.wnamFormId == 0)
			continue;
		npcSkinCache[npc.editorId] = remapFid(npc.wnamFormId);
	}

	// Cache RACE records (remapped FormID → race info). Overwrite (later master wins).
	// raceByEditorId is keyed by editor id; overwrite it too so it tracks the same winner.
	for (auto& r : reader.GetRaces()) {
		uint32_t remappedId = remapFid(r.formId);
		CachedRACE cr;
		cr.editorId = r.editorId;
		cr.skinFormId = r.skinFormId != 0 ? remapFid(r.skinFormId) : 0;
		raceCache[remappedId] = std::move(cr);
		if (!r.editorId.empty())
			raceByEditorId[r.editorId] = remappedId;
	}

	// Cache HDPTs
	for (auto& hp : reader.GetHeadParts()) {
		uint32_t remappedId = remapFid(hp.formId);
		CachedHeadPart chp;
		chp.editorId = hp.editorId;
		chp.model = hp.model;
		chp.type = hp.type;
		headPartCache[remappedId] = std::move(chp);
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
		const std::string& model = !it->second.modelFemale.empty() ? it->second.modelFemale : (femaleOnly ? std::string{} : it->second.modelMale);
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
		// Use ARMO-level body slot flags — the ARMO record is the authoritative declaration
		// of which body slots this armor piece occupies, matching how the game processes it.
		piece.bodySlots = DecodeBodySlots(armo.bodySlotFlags);
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
	raceCache.clear();
	raceByEditorId.clear();
	npcSkinCache.clear();
	headPartCache.clear();
	npcSkinOverrides.clear();
	npcSkinsScanned = false;
	espMasters.clear();
	loadInfo.clear();

	espDirectory = DirectoryOf(filepath);

	// Load the main ESP first
	esp::ESPReader mainReader;
	if (!mainReader.Load(filepath, {"ARMO", "ARMA", "TXST", "LVLI", "OTFT", "HDPT"}))
		return false;

	espFilename = mainReader.GetFilename();

	// Load master files. The main ESP's TES4 master list is a flat dependency
	// declaration; its declared *order* is not authoritative for override
	// resolution (the game uses plugins.txt). To pick the correct override
	// winner without reading plugins.txt, we topologically sort the master list
	// using each master's own TES4 master list — if A is a master of B, A must
	// load before B. Pairs with no dependency relation fall back to declared
	// order (which is fine: neither can override the other for FormID-keyed
	// records since they live in different cache namespaces).
	//
	// Crucial: `masterIdx` passed to LoadRecordsFromESP stays the *declared*
	// index, because the main ESP's records reference masters by that index
	// for FormID remapping. Only the iteration order changes.
	auto masters = mainReader.GetMasters();
	espMasters = masters; // Store for FormID remapping in skin texture resolution

	// Resolve each master's full path on disk (same probing as the load below).
	auto resolveMasterPath = [&](const std::string& masterName) -> std::string {
		std::string p = espDirectory + masterName;
		if (wxFileName::FileExists(p))
			return p;
		if (!baseDataPath.empty()) {
			p = baseDataPath + masterName;
			if (wxFileName::FileExists(p))
				return p;
		}
		return {};
	};

	std::vector<std::string> masterPaths(masters.size());
	for (size_t mi = 0; mi < masters.size(); ++mi)
		masterPaths[mi] = resolveMasterPath(masters[mi]);

	// Index masters by lowercase name for dependency lookup.
	std::unordered_map<std::string, size_t> indexByName;
	indexByName.reserve(masters.size());
	for (size_t mi = 0; mi < masters.size(); ++mi)
		indexByName.emplace(ToLower(masters[mi]), mi);

	// For each master, read its own TES4 master list and record edges
	// (depIdx → mi) where depIdx is also part of this main ESP's master list.
	std::vector<std::vector<size_t>> outEdges(masters.size()); // outEdges[a] = b means a must load before b
	std::vector<int> inDegree(masters.size(), 0);
	for (size_t mi = 0; mi < masters.size(); ++mi) {
		if (masterPaths[mi].empty())
			continue;
		auto deps = esp::ESPReader::ReadPluginMasters(masterPaths[mi]);
		for (auto& depName : deps) {
			auto it = indexByName.find(ToLower(depName));
			if (it == indexByName.end())
				continue; // dep is not part of this main ESP's master list
			size_t depIdx = it->second;
			if (depIdx == mi)
				continue; // self-reference, ignore
			outEdges[depIdx].push_back(mi);
			++inDegree[mi];
		}
	}

	// Kahn's algorithm: pick zero-in-degree masters in declared order to make the
	// fallback deterministic and aligned with the original list.
	std::vector<size_t> loadOrder;
	loadOrder.reserve(masters.size());
	std::vector<bool> queued(masters.size(), false);
	auto enqueueReady = [&](size_t startFrom) {
		for (size_t i = startFrom; i < masters.size(); ++i) {
			if (!queued[i] && inDegree[i] == 0) {
				loadOrder.push_back(i);
				queued[i] = true;
			}
		}
	};
	enqueueReady(0);
	for (size_t cursor = 0; cursor < loadOrder.size(); ++cursor) {
		size_t cur = loadOrder[cursor];
		for (size_t next : outEdges[cur]) {
			if (--inDegree[next] == 0 && !queued[next]) {
				loadOrder.push_back(next);
				queued[next] = true;
			}
		}
	}
	// Cycle / unreachable safety net: append anything we missed in declared order.
	if (loadOrder.size() < masters.size()) {
		wxLogWarning("LeveledListData: master dependency graph has cycles or unreachable nodes; appending remainder in declared order");
		for (size_t i = 0; i < masters.size(); ++i)
			if (!queued[i])
				loadOrder.push_back(i);
	}

	int mastersLoaded = 0;
	for (size_t mi : loadOrder) {
		auto& masterName = masters[mi];
		uint8_t masterIdx = static_cast<uint8_t>(mi);
		const std::string& masterPath = masterPaths[mi];

		if (masterPath.empty()) {
			wxLogWarning("LeveledListData: Master not found: %s (index %zu)", wxString(masterName), mi);
			continue;
		}

		LoadRecordsFromESP(masterPath, {"ARMO", "ARMA", "TXST", "NPC_", "RACE", "HDPT"}, masterIdx, masters);
		++mastersLoaded;
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
		cam.raceFormId = aa.raceId;						// main-ESP FormIDs need no remap
		cam.additionalRaceFormIds = aa.additionalRaces; // main-ESP FormIDs need no remap
		cam.skinTextureMale = aa.skinTextureMale;
		cam.skinTextureFemale = aa.skinTextureFemale;
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

	for (auto& hp : mainReader.GetHeadParts()) {
		CachedHeadPart chp;
		chp.editorId = hp.editorId;
		chp.model = hp.model;
		chp.type = hp.type;
		headPartCache[hp.formId] = std::move(chp);
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

std::array<std::string, 8> LeveledListData::ResolveSkinTextures(uint32_t wnamFormId, uint32_t npcRaceFormId) const {
	std::array<std::string, 8> textures{};
	if (wnamFormId == 0)
		return textures;

	auto armoIt = armoCache.find(wnamFormId);
	if (armoIt == armoCache.end()) {
		wxLogMessage("ResolveSkinTextures: WNAM ARMO %08X not in cache", wnamFormId);
		return textures;
	}

	auto& armo = armoIt->second;

	// Try race-matching ARMAs first, then any. Game semantics: SkinNaked holds one ARMA
	// per race; the engine picks the one whose RNAM matches the NPC's race.
	auto tryArmas = [&](bool requireRaceMatch) -> bool {
		for (uint32_t armaId : armo.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end())
				continue;

			auto& arma = armaIt->second;
			if (!(arma.bodySlotFlags & (1u << 2))) // bit 2 = slot 32 = Body
				continue;
			if (requireRaceMatch && npcRaceFormId != 0) {
				bool match = arma.raceFormId == npcRaceFormId;
				if (!match) {
					for (uint32_t r : arma.additionalRaceFormIds) {
						if (r == npcRaceFormId) {
							match = true;
							break;
						}
					}
				}
				if (!match)
					continue;
			}

			// Use female alternate textures (fallback to male)
			auto& altTexList = arma.altTexFemale.empty() ? arma.altTexMale : arma.altTexFemale;
			if (altTexList.empty()) {
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
				wxLogMessage("ResolveSkinTextures: Found body textures from ARMA %08X (%s, race %08X), TXST %08X (%s): diffuse='%s'",
							 armaId,
							 arma.editorId,
							 arma.raceFormId,
							 at.txstFormId,
							 txstIt->second.editorId,
							 textures[0]);
				return true;
			}
		}
		return false;
	};

	if (npcRaceFormId != 0 && tryArmas(true))
		return textures;
	if (tryArmas(false))
		return textures;

	wxLogMessage("ResolveSkinTextures: No body ARMA with textures found for WNAM %08X (race hint %08X)", wnamFormId, npcRaceFormId);
	return textures;
}

// ---------------------------------------------------------------------------
// ResolveBodyNifPaths — get body/hands/feet NIF paths from NPC skin ARMO
// ---------------------------------------------------------------------------

LeveledListData::BodyNifPaths LeveledListData::ResolveBodyNifPaths(uint32_t wnamFormId, bool highWeight, uint32_t npcRaceFormId) const {
	BodyNifPaths result;
	if (wnamFormId == 0)
		return result;

	auto armoIt = armoCache.find(wnamFormId);
	if (armoIt == armoCache.end()) {
		wxLogMessage("ResolveBodyNifPaths: WNAM ARMO %08X not in cache", wnamFormId);
		return result;
	}

	const std::string suffix = highWeight ? "_1.nif" : "_0.nif";

	// Fill one pass worth of ARMAs with optional race-match requirement. Returns true
	// if all three slots are filled after this pass.
	auto pass = [&](bool requireRaceMatch) -> bool {
		for (uint32_t armaId : armoIt->second.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end())
				continue;

			auto& arma = armaIt->second;
			if (requireRaceMatch && npcRaceFormId != 0) {
				bool match = arma.raceFormId == npcRaceFormId;
				if (!match) {
					for (uint32_t r : arma.additionalRaceFormIds) {
						if (r == npcRaceFormId) {
							match = true;
							break;
						}
					}
				}
				if (!match)
					continue;
			}

			const std::string& model = arma.modelFemale.empty() ? arma.modelMale : arma.modelFemale;
			if (model.empty())
				continue;

			std::string path = NormalizeMeshPath(model);
			// ARMA model paths can come in two forms:
			// 1. Already weighted: "...femalebody_1.nif" — swap suffix to match
			//    the requested weight variant.
			// 2. Suffix-less: "...femalebody.nif" — the engine appends _0/_1
			//    automatically; do the same here.
			if (path.size() >= 6 && (path.substr(path.size() - 6) == "_1.nif" || path.substr(path.size() - 6) == "_0.nif")) {
				path = path.substr(0, path.size() - 6) + suffix;
			}
			else if (path.size() >= 4 && path.substr(path.size() - 4) == ".nif") {
				path = path.substr(0, path.size() - 4) + suffix;
			}

			if ((arma.bodySlotFlags & (1u << 2)) && result.body.empty()) {
				result.body = path;
				wxLogMessage("ResolveBodyNifPaths: body  -> %s (ARMA %08X race %08X)", path, armaId, arma.raceFormId);
			}
			if ((arma.bodySlotFlags & (1u << 3)) && result.hands.empty()) {
				result.hands = path;
				wxLogMessage("ResolveBodyNifPaths: hands -> %s (ARMA %08X race %08X)", path, armaId, arma.raceFormId);
			}
			if ((arma.bodySlotFlags & (1u << 7)) && result.feet.empty()) {
				result.feet = path;
				wxLogMessage("ResolveBodyNifPaths: feet  -> %s (ARMA %08X race %08X)", path, armaId, arma.raceFormId);
			}
		}
		return !result.body.empty() && !result.hands.empty() && !result.feet.empty();
	};

	// Pass 1: race-matching ARMAs only (matches in-game behavior).
	// Pass 2: any ARMA to fill remaining slots.
	if (npcRaceFormId != 0)
		pass(true);
	pass(false);

	return result;
}

// ---------------------------------------------------------------------------
// Race skin ARMO + NPC body NIF resolution (with race fallback)
// ---------------------------------------------------------------------------

uint32_t LeveledListData::GetRaceSkinArmo(const std::string& raceEditorId) const {
	if (raceEditorId.empty())
		return 0;
	auto it = raceByEditorId.find(raceEditorId);
	if (it == raceByEditorId.end())
		return 0;
	auto rit = raceCache.find(it->second);
	if (rit == raceCache.end())
		return 0;
	return rit->second.skinFormId;
}

LeveledListData::EffectiveRace LeveledListData::GetEffectiveNpcRace(const std::string& npcEditorId) {
	EffectiveRace er;
	if (npcEditorId.empty())
		return er;

	if (!npcSkinsScanned)
		ScanAllPluginsForNpcSkins();

	// Step 1: prefer the latest plugin's RNAM override if we managed to remap it
	// into the main-ESP cache space.
	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt != npcSkinOverrides.end() && ovIt->second.remappedRace != 0) {
		uint32_t fid = ovIt->second.remappedRace;
		auto cacheIt = raceCache.find(fid);
		if (cacheIt != raceCache.end()) {
			er.formId = fid;
			er.editorId = cacheIt->second.editorId;
			er.skinArmoFormId = cacheIt->second.skinFormId;
			wxLogMessage("GetEffectiveNpcRace: NPC '%s' override race %08X ('%s') skinArmo=%08X", npcEditorId, fid, er.editorId, er.skinArmoFormId);
			return er;
		}
		wxLogMessage("GetEffectiveNpcRace: NPC '%s' override race %08X not in raceCache, falling back to vanilla", npcEditorId, fid);
	}

	// Step 2: vanilla race recorded during LoadNPCs.
	for (auto& n : npcs) {
		if (n.editorId == npcEditorId) {
			er.editorId = n.raceEditorId;
			break;
		}
	}
	if (!er.editorId.empty()) {
		auto raceIt = raceByEditorId.find(er.editorId);
		if (raceIt != raceByEditorId.end()) {
			er.formId = raceIt->second;
			auto cacheIt = raceCache.find(er.formId);
			if (cacheIt != raceCache.end())
				er.skinArmoFormId = cacheIt->second.skinFormId;
		}
	}
	return er;
}

std::string LeveledListData::ResolveHeadPartNif(uint32_t headPartFormId) const {
	auto it = headPartCache.find(headPartFormId);
	if (it == headPartCache.end())
		return {};
	return it->second.model;
}

LeveledListData::BodyNifPaths LeveledListData::ResolveNpcBodyNifPaths(const std::string& npcEditorId, bool highWeight) {
	BodyNifPaths result;
	if (npcEditorId.empty())
		return result;

	// Effective race honors RNAM overrides from any plugin, not just vanilla data.
	EffectiveRace effRace = GetEffectiveNpcRace(npcEditorId);
	const std::string& raceEdid = effRace.editorId;
	uint32_t npcRaceFid = effRace.formId;

	// Primary: NPC's own WNAM (prefer all-plugins override, fall back to master cache).
	uint32_t npcWnam = 0;
	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt != npcSkinOverrides.end() && !ovIt->second.selfDefined)
		npcWnam = ovIt->second.remappedWnam;
	if (npcWnam == 0) {
		auto scIt = npcSkinCache.find(npcEditorId);
		if (scIt != npcSkinCache.end())
			npcWnam = scIt->second;
	}
	if (npcWnam != 0)
		result = ResolveBodyNifPaths(npcWnam, highWeight, npcRaceFid);

	uint32_t raceWnam = 0;
	if (result.body.empty() || result.hands.empty() || result.feet.empty()) {
		raceWnam = GetRaceSkinArmo(raceEdid);
		if (raceWnam != 0) {
			BodyNifPaths rp = ResolveBodyNifPaths(raceWnam, highWeight, npcRaceFid);
			if (result.body.empty())
				result.body = rp.body;
			if (result.hands.empty())
				result.hands = rp.hands;
			if (result.feet.empty())
				result.feet = rp.feet;
		}
	}

	wxLogMessage("ResolveNpcBodyNifPaths: NPC '%s' race='%s' raceFID=%08X npcWNAM=%08X raceWNAM=%08X -> body='%s' hands='%s' feet='%s'",
				 npcEditorId,
				 raceEdid,
				 npcRaceFid,
				 npcWnam,
				 raceWnam,
				 result.body,
				 result.hands,
				 result.feet);

	return result;
}

// ---------------------------------------------------------------------------
// Per-slot skin texture resolution (body / hands / feet)
// ---------------------------------------------------------------------------

namespace {
// Fill `dst` from the first TXST in ARMA's female alt-texture list (fallback male).
// If MO3S/MO2S yield nothing, fall back to the per-gender skin TXST set via
// NAM1 (female) / NAM0 (male) — that's the simpler ESP/ESL skin override
// mechanism that mods commonly use to retexture an NPC's body.
bool FillFromArmaTextures(const CachedARMA& arma, const std::unordered_map<uint32_t, CachedTXST>& txstCache, std::array<std::string, 8>& dst) {
	auto& altTex = arma.altTexFemale.empty() ? arma.altTexMale : arma.altTexFemale;
	for (auto& at : altTex) {
		auto it = txstCache.find(at.txstFormId);
		if (it == txstCache.end())
			continue;
		for (int i = 0; i < 8; ++i)
			if (!it->second.textures[i].empty())
				dst[i] = it->second.textures[i];
		if (!dst[0].empty())
			return true;
	}

	uint32_t skinTxst = arma.skinTextureFemale != 0 ? arma.skinTextureFemale : arma.skinTextureMale;
	if (skinTxst != 0) {
		auto it = txstCache.find(skinTxst);
		if (it != txstCache.end()) {
			for (int i = 0; i < 8; ++i)
				if (!it->second.textures[i].empty())
					dst[i] = it->second.textures[i];
			if (!dst[0].empty())
				return true;
		}
	}
	return false;
}

// Walk an ARMO's ARMA list filling per-slot texture arrays. Prefers race-matching ARMAs.
void FillPartTexturesFromArmo(uint32_t armoFid,
							  uint32_t npcRaceFid,
							  const std::unordered_map<uint32_t, CachedArmo>& armoCache,
							  const std::unordered_map<uint32_t, CachedARMA>& armaCache,
							  const std::unordered_map<uint32_t, CachedTXST>& txstCache,
							  LeveledListData::BodyPartTextures& out) {
	if (armoFid == 0)
		return;
	auto armoIt = armoCache.find(armoFid);
	if (armoIt == armoCache.end()) {
		wxLogMessage("FillPartTexturesFromArmo: ARMO %08X not in cache", armoFid);
		return;
	}

	wxLogMessage("FillPartTexturesFromArmo: walking ARMO %08X (%zu ARMAs) raceHint=%08X",
				 armoFid, armoIt->second.armatureIds.size(), npcRaceFid);

	auto tryFill = [&](bool requireRaceMatch) {
		for (uint32_t armaId : armoIt->second.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end()) {
				wxLogMessage("  ARMA %08X not in cache", armaId);
				continue;
			}
			auto& arma = armaIt->second;
			if (requireRaceMatch && npcRaceFid != 0) {
				bool match = arma.raceFormId == npcRaceFid;
				if (!match) {
					for (uint32_t r : arma.additionalRaceFormIds) {
						if (r == npcRaceFid) {
							match = true;
							break;
						}
					}
				}
				if (!match)
					continue;
			}

			std::size_t altF = arma.altTexFemale.size();
			std::size_t altM = arma.altTexMale.size();
			wxLogMessage("  ARMA %08X race=%08X slotFlags=%08X altTexF=%zu altTexM=%zu skinTxstF=%08X skinTxstM=%08X",
						 armaId, arma.raceFormId, arma.bodySlotFlags, altF, altM,
						 arma.skinTextureFemale, arma.skinTextureMale);

			if ((arma.bodySlotFlags & (1u << 2)) && out.body[0].empty()) {
				if (FillFromArmaTextures(arma, txstCache, out.body))
					wxLogMessage("    body texture set: %s", out.body[0]);
			}
			if ((arma.bodySlotFlags & (1u << 3)) && out.hands[0].empty()) {
				if (FillFromArmaTextures(arma, txstCache, out.hands))
					wxLogMessage("    hands texture set: %s", out.hands[0]);
			}
			if ((arma.bodySlotFlags & (1u << 7)) && out.feet[0].empty()) {
				if (FillFromArmaTextures(arma, txstCache, out.feet))
					wxLogMessage("    feet texture set: %s", out.feet[0]);
			}
		}
	};

	if (npcRaceFid != 0)
		tryFill(true);
	tryFill(false);
}
} // namespace

LeveledListData::BodyPartTextures LeveledListData::ResolveNpcBodyPartTextures(const std::string& npcEditorId) {
	BodyPartTextures out;
	if (npcEditorId.empty())
		return out;

	// Use the effective race (RNAM override-aware) so a plugin that re-races
	// an NPC also redirects which ARMAs the texture chain walks.
	EffectiveRace effRace = GetEffectiveNpcRace(npcEditorId);
	const std::string& raceEdid = effRace.editorId;
	uint32_t npcRaceFid = effRace.formId;

	// Pass 1: NPC's own WNAM (from all-plugin scan, remapped to main cache).
	uint32_t npcWnam = 0;
	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt != npcSkinOverrides.end() && !ovIt->second.selfDefined)
		npcWnam = ovIt->second.remappedWnam;
	if (npcWnam == 0) {
		auto scIt = npcSkinCache.find(npcEditorId);
		if (scIt != npcSkinCache.end())
			npcWnam = scIt->second;
	}
	if (npcWnam != 0)
		FillPartTexturesFromArmo(npcWnam, npcRaceFid, armoCache, armaCache, txstCache, out);

	// Pass 2: race's default skin ARMO for any slots still empty.
	if (out.body[0].empty() || out.hands[0].empty() || out.feet[0].empty()) {
		uint32_t raceWnam = GetRaceSkinArmo(raceEdid);
		if (raceWnam != 0)
			FillPartTexturesFromArmo(raceWnam, npcRaceFid, armoCache, armaCache, txstCache, out);
	}

	wxLogMessage("ResolveNpcBodyPartTextures: NPC '%s' race='%s' raceFID=%08X npcWNAM=%08X -> body='%s' hands='%s' feet='%s'",
				 npcEditorId,
				 raceEdid,
				 npcRaceFid,
				 npcWnam,
				 out.body[0],
				 out.hands[0],
				 out.feet[0]);
	return out;
}

// ---------------------------------------------------------------------------
// EnsureDynamicMaster — load a non-master plugin's RACE/ARMO/ARMA/TXST records
// its WNAM at an ARMO it also defines) can be resolved.
// ---------------------------------------------------------------------------

uint8_t LeveledListData::EnsureDynamicMaster(const std::string& pluginPath) {
	for (size_t i = 0; i < dynamicMasters.size(); ++i) {
		if (dynamicMasters[i] == pluginPath)
			return static_cast<uint8_t>(0xFD - i);
	}
	if (dynamicMasters.size() > 0xF0 - 0x80) {
		wxLogWarning("EnsureDynamicMaster: too many dynamic masters, refusing to load %s", pluginPath);
		return 0;
	}
	uint8_t synthIdx = static_cast<uint8_t>(0xFD - dynamicMasters.size());
	wxLogMessage("EnsureDynamicMaster: loading '%s' at synthetic master index 0x%02X", pluginPath, synthIdx);
	dynamicMasters.push_back(pluginPath);
	LoadRecordsFromESP(pluginPath, {"RACE", "ARMO", "ARMA", "TXST"}, synthIdx, espMasters);
	return synthIdx;
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

	// Vanilla-ESM names. We never let a record from one of these clobber an
	// existing override from a non-vanilla plugin: scan order is alphabetical
	// (NOT load order), so the engine's "last writer wins" semantics need a
	// proxy. Real overrides live in mod ESPs/ESLs, so prefer them over reading
	// the original NPC record back from the vanilla ESM.
	auto isVanillaEsm = [](const std::string& pluginPath) {
		static const std::array<const char*, 5> vanilla = {"skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm"};
		auto sep = pluginPath.find_last_of("/\\");
		std::string base = (sep == std::string::npos) ? pluginPath : pluginPath.substr(sep + 1);
		std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return std::tolower(c); });
		for (auto* v : vanilla) {
			if (base == v)
				return true;
		}
		return false;
	};

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

		// Helper: remap a FormID from this plugin's master-index space into the
		// main ESP's master-index space (raceCache / armoCache lookup space).
		// Returns 0 if the FormID can't be remapped (top byte unknown).
		auto remapToMainSpace = [&](uint32_t fid) -> uint32_t {
			if (fid == 0)
				return 0;
			uint8_t tb = (fid >> 24) & 0xFF;
			uint32_t base = fid & 0x00FFFFFF;
			if (tb == selfIdx) {
				if (isMaster)
					return (static_cast<uint32_t>(selfMasterIdx) << 24) | base;
				return 0;
			}
			auto it = masterRemap.find(tb);
			if (it == masterRemap.end())
				return 0;
			return (static_cast<uint32_t>(it->second) << 24) | base;
		};

		for (auto& npc : npcs) {
			if (npc.editorId.empty())
				continue;
			// Skip records that override neither WNAM nor RNAM — they have nothing to contribute.
			if (npc.wnamFormId == 0 && npc.raceFormId == 0)
				continue;

			NpcSkinInfo info;
			info.sourcePlugin = pluginPath;
			info.remappedRace = remapToMainSpace(npc.raceFormId);

			if (npc.wnamFormId != 0) {
				uint8_t topByte = (npc.wnamFormId >> 24) & 0xFF;
				uint32_t baseId = npc.wnamFormId & 0x00FFFFFF;

				if (topByte == selfIdx) {
					if (isMaster) {
						info.remappedWnam = (static_cast<uint32_t>(selfMasterIdx) << 24) | baseId;
						info.selfDefined = false;
					}
					else {
						info.wnamRaw = npc.wnamFormId;
						info.selfDefined = true;
					}
				}
				else {
					auto remapIt = masterRemap.find(topByte);
					if (remapIt != masterRemap.end()) {
						info.remappedWnam = (static_cast<uint32_t>(remapIt->second) << 24) | baseId;
						info.selfDefined = false;
					}
					else {
						info.wnamRaw = npc.wnamFormId;
						info.selfDefined = true;
					}
				}
			}

			// Don't let a record from a vanilla ESM (Skyrim.esm, Update.esm, ...)
			// clobber an override we already recorded from a non-vanilla plugin.
			// Scan order is alphabetical, so an ESM revisit AFTER a mod ESP would
			// otherwise blow the override away — i.e. a mod's intentional
			// override would be reset to vanilla just because the ESM happens
			// to sort later in the directory listing.
			bool currentIsVanilla = isVanillaEsm(pluginPath);
			auto existing = npcSkinOverrides.find(npc.editorId);
			if (existing != npcSkinOverrides.end() && currentIsVanilla && !isVanillaEsm(existing->second.sourcePlugin)) {
				continue;
			}
			npcSkinOverrides[npc.editorId] = info;
		}
	}

	wxLogMessage("ScanAllPluginsForNpcSkins: scanned %zu plugins (%zu with NPCs), %zu NPC skin overrides", pluginFiles.size(), pluginsWithNpcs, npcSkinOverrides.size());

	// For each unresolvable override (RNAM and/or WNAM not in our caches), find
	// the plugin that ACTUALLY defines the referenced record — which can be the
	// override's source plugin itself (self-defined) OR one of the source's
	// masters that isn't one of our generated ESP's masters. Load that plugin
	// as a synthetic "dynamic master" and rewrite the NpcSkinInfo with FormIDs
	// that resolve in our caches. This lets both a mod that re-races an NPC
	// and points its WNAM at an ARMO it self-defines, and a patch ESP that
	// points an NPC override at an ARMO from a third plugin, drive the
	// NIF/texture chain.
	//
	// First, gather all (sourcePlugin → editorId) pairs that still need
	// resolution. Group by sourcePlugin so we re-read each source plugin once.
	std::unordered_map<std::string, std::vector<std::string>> needBySource;
	for (auto& [editorId, info] : npcSkinOverrides) {
		bool needRace = (info.remappedRace == 0);
		bool needWnam = (info.selfDefined && info.wnamRaw != 0);
		if (!needRace && !needWnam)
			continue;
		needBySource[info.sourcePlugin].push_back(editorId);
	}

	// Index every plugin file in the data directory by lowercased base name —
	// we'll need this to locate "the plugin named X" when an override points
	// into one of the source plugin's masters that isn't one of our masters.
	std::unordered_map<std::string, std::string> dataDirPluginByName;
	for (auto& wxPath : pluginFiles) {
		std::string p = wxPath.ToStdString();
		auto sep = p.find_last_of("/\\");
		std::string base = (sep == std::string::npos) ? p : p.substr(sep + 1);
		dataDirPluginByName[ToLower(base)] = p;
	}

	// Resolve a raw FormID (in the source plugin's master-index space) to the
	// plugin path that actually defines it. Returns empty string if the master
	// can't be located in the data directory.
	auto findDefiningPlugin = [&](const std::string& srcPath,
								  const std::vector<std::string>& srcMasters,
								  uint8_t srcSelfIdx,
								  uint32_t rawFid) -> std::string {
		uint8_t topByte = (rawFid >> 24) & 0xFF;
		if (topByte == srcSelfIdx)
			return srcPath;
		if (topByte < srcMasters.size()) {
			auto it = dataDirPluginByName.find(ToLower(srcMasters[topByte]));
			if (it != dataDirPluginByName.end())
				return it->second;
		}
		return {};
	};

	for (auto& [pluginPath, editorIds] : needBySource) {
		if (isVanillaEsm(pluginPath))
			continue;

		// Re-read the source plugin's NPC_ records to recover each override's
		// raw RNAM and the plugin's master list.
		esp::ESPReader reader;
		if (!reader.Load(pluginPath, {"NPC_"}))
			continue;
		auto& srcMasters = reader.GetMasters();
		uint8_t srcSelfIdx = static_cast<uint8_t>(srcMasters.size());

		std::unordered_map<std::string, uint32_t> rawRaceByEditorId;
		std::unordered_map<std::string, uint32_t> rawWnamByEditorId;
		for (auto& npc : reader.GetNPCs()) {
			if (npc.editorId.empty())
				continue;
			rawRaceByEditorId[npc.editorId] = npc.raceFormId;
			rawWnamByEditorId[npc.editorId] = npc.wnamFormId;
		}

		for (auto& editorId : editorIds) {
			auto ovIt = npcSkinOverrides.find(editorId);
			if (ovIt == npcSkinOverrides.end())
				continue;
			NpcSkinInfo& info = ovIt->second;

			// Resolve the RNAM if still unmapped.
			auto rIt = rawRaceByEditorId.find(editorId);
			uint32_t raceRaw = (rIt != rawRaceByEditorId.end()) ? rIt->second : 0;
			if (raceRaw != 0 && info.remappedRace == 0) {
				std::string defPath = findDefiningPlugin(pluginPath, srcMasters, srcSelfIdx, raceRaw);
				if (!defPath.empty()) {
					uint8_t synthIdx = EnsureDynamicMaster(defPath);
					if (synthIdx != 0)
						info.remappedRace = (static_cast<uint32_t>(synthIdx) << 24) | (raceRaw & 0x00FFFFFF);
				}
			}

			// Resolve the WNAM if still unmapped. Use whichever raw value is
			// available — info.wnamRaw is what we recorded earlier, and the
			// source plugin's record may have a different one if the override
			// chain is more complex.
			uint32_t wnamRaw = info.wnamRaw;
			if (wnamRaw == 0) {
				auto wIt = rawWnamByEditorId.find(editorId);
				if (wIt != rawWnamByEditorId.end())
					wnamRaw = wIt->second;
			}
			if (wnamRaw != 0 && info.remappedWnam == 0) {
				std::string defPath = findDefiningPlugin(pluginPath, srcMasters, srcSelfIdx, wnamRaw);
				if (!defPath.empty()) {
					uint8_t synthIdx = EnsureDynamicMaster(defPath);
					if (synthIdx != 0) {
						info.remappedWnam = (static_cast<uint32_t>(synthIdx) << 24) | (wnamRaw & 0x00FFFFFF);
						info.selfDefined = false;
					}
				}
			}

			wxLogMessage("ScanAllPluginsForNpcSkins: '%s' override resolution — remappedRace=%08X remappedWnam=%08X",
						 editorId, info.remappedRace, info.remappedWnam);
		}
	}
}

// ---------------------------------------------------------------------------
// ResolveSkinTexturesForNPC — on-demand skin texture resolution per NPC
// ---------------------------------------------------------------------------

std::array<std::string, 8> LeveledListData::ResolveSkinTexturesForNPC(const std::string& npcEditorId) {
	std::array<std::string, 8> textures{};

	// Lazy scan: first call triggers the all-plugins scan
	if (!npcSkinsScanned)
		ScanAllPluginsForNpcSkins();

	// Look up NPC's race so we can prefer race-matching ARMAs (SkinNaked has one ARMA per race).
	std::string raceEdid;
	for (auto& n : npcs) {
		if (n.editorId == npcEditorId) {
			raceEdid = n.raceEditorId;
			break;
		}
	}
	uint32_t npcRaceFid = 0;
	if (!raceEdid.empty()) {
		auto it = raceByEditorId.find(raceEdid);
		if (it != raceByEditorId.end())
			npcRaceFid = it->second;
	}

	// Helper: try the race's default skin ARMO as a last resort.
	auto tryRaceFallback = [&](std::array<std::string, 8>& tex) {
		if (!tex[0].empty())
			return;
		uint32_t raceWnam = GetRaceSkinArmo(raceEdid);
		if (raceWnam != 0) {
			tex = ResolveSkinTextures(raceWnam, npcRaceFid);
			if (!tex[0].empty())
				wxLogMessage("ResolveSkinTexturesForNPC: NPC '%s' race '%s' skin ARMO %08X provided textures: %s", npcEditorId, raceEdid, raceWnam, tex[0]);
		}
	};

	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt == npcSkinOverrides.end()) {
		// No WNAM override known for this NPC → go straight to race fallback.
		tryRaceFallback(textures);
		return textures;
	}

	auto& info = ovIt->second;

	// If WNAM was remapped to main cache space, use the existing cache-based resolver
	if (!info.selfDefined) {
		textures = ResolveSkinTextures(info.remappedWnam, npcRaceFid);
		if (!textures[0].empty())
			wxLogMessage("ResolveSkinTexturesForNPC: NPC '%s' resolved from cache (WNAM %08X, race %08X): %s", npcEditorId, info.remappedWnam, npcRaceFid, textures[0]);
		else
			tryRaceFallback(textures);
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

	// Self-defined chain yielded nothing — try race fallback.
	tryRaceFallback(textures);
	if (textures[0].empty())
		wxLogMessage("ResolveSkinTexturesForNPC: no body textures found for '%s'", npcEditorId);
	return textures;
}

} // namespace lldata
