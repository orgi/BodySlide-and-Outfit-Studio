/*
 * LeveledListData — resolves ESP records into browseable outfit data.
 */

#include "LeveledListData.h"
#include "../../lib/FSEngine/FSEngine.h"
#include "../../lib/FSEngine/FSManager.h"
#include "../files/ESPReader.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/utils.h>

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
	npcOutfitCache.clear();
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

	// Build a remap from a scanned plugin's master indices to espMasters indices, so
	// a DOFT FormID (in that plugin's own local space) can be translated into the
	// main-ESP otftCache key. Falls back to 0 ("unmapped") when the main ESP doesn't
	// include the referenced master. The returned closure copies the master list, so
	// the source reader's lifetime is irrelevant.
	auto buildLocalToEspMastersRemap = [this](std::vector<std::string> localMasters, std::string selfPlugin) {
		return [this, localMasters = std::move(localMasters), selfPlugin = std::move(selfPlugin)](uint32_t raw) -> uint32_t {
			if (raw == 0)
				return 0;
			uint8_t tb = (raw >> 24) & 0xFF;
			uint32_t baseId = raw & 0x00FFFFFF;
			const std::string* sourceName = nullptr;
			if (tb < localMasters.size())
				sourceName = &localMasters[tb];
			else if (tb == localMasters.size())
				sourceName = &selfPlugin;
			else
				return 0;
			for (size_t mi = 0; mi < espMasters.size(); ++mi) {
				if (ToLower(espMasters[mi]) == ToLower(*sourceName))
					return (static_cast<uint32_t>(mi) << 24) | baseId;
			}
			return 0;
		};
	};

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

		// Build a remap from reader's master indices to espMasters indices, so a
		// DOFT FormID (in this scanned ESM's local space) can be translated into
		// the main-ESP otftCache key.
		auto& readerMasters = reader.GetMasters();
		auto remapDoftLocal = buildLocalToEspMastersRemap(readerMasters, vanillaESMs[i]);

		size_t countBefore = npcs.size() + pending.size();
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
			pn.entry.isFemale = npc.isFemale;
			pn.rawRaceFormId = npc.raceFormId;
			pn.sourceEsm = vanillaESMs[i];
			// Capture default outfit (DOFT) and remember which plugin defined it,
			// so the previewer can lazily remap the raw FormID into otftCache.
			if (npc.defaultOutfitFormId != 0) {
				pn.entry.defaultOutfitFormIdRaw = npc.defaultOutfitFormId;
				pn.entry.defaultOutfitSourcePlugin = vanillaESMs[i];
				pn.entry.defaultOutfitFormId = remapDoftLocal(npc.defaultOutfitFormId);
			}
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
			auto remapDoftOverride = buildLocalToEspMastersRemap(readerMasters, pluginFile);
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

				// Engine "last writer wins" for DOFT: any override that defines
				// DOFT replaces the value captured from the native (vanilla) pass.
				// Scan order here is alphabetical, not strict load order, but for
				// the previewer this is a reasonable approximation that lets mods
				// like "Immersive Armors" actually swap a follower's default kit.
				if (npc.defaultOutfitFormId != 0) {
					existing.entry.defaultOutfitFormIdRaw = npc.defaultOutfitFormId;
					existing.entry.defaultOutfitSourcePlugin = pluginFile;
					existing.entry.defaultOutfitFormId = remapDoftOverride(npc.defaultOutfitFormId);
				}

				// ACBS — last writer wins, same as DOFT.
				existing.entry.isFemale = npc.isFemale;
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
// LoadAllPluginNativeNPCs — extend the NPC list with mod-native NPC_ records
// ---------------------------------------------------------------------------

void LeveledListData::LoadAllPluginNativeNPCs() {
	if (baseDataPath.empty())
		return;

	// Local remap factory (mirrors the one in LoadNPCs).
	auto buildLocalToEspMastersRemap = [this](std::vector<std::string> localMasters, std::string selfPlugin) {
		return [this, localMasters = std::move(localMasters), selfPlugin = std::move(selfPlugin)](uint32_t raw) -> uint32_t {
			if (raw == 0)
				return 0;
			uint8_t tb = (raw >> 24) & 0xFF;
			uint32_t baseId = raw & 0x00FFFFFF;
			const std::string* sourceName = nullptr;
			if (tb < localMasters.size())
				sourceName = &localMasters[tb];
			else if (tb == localMasters.size())
				sourceName = &selfPlugin;
			else
				return 0;
			for (size_t mi = 0; mi < espMasters.size(); ++mi) {
				if (ToLower(espMasters[mi]) == ToLower(*sourceName))
					return (static_cast<uint32_t>(mi) << 24) | baseId;
			}
			return 0;
		};
	};

	// Already-loaded NPCs are keyed by (nativePlugin, baseFormId). Build a set
	// to skip them — re-scanning them in this pass would produce duplicates.
	std::set<std::pair<std::string, uint32_t>> alreadyLoaded;
	for (auto& n : npcs)
		alreadyLoaded.insert({n.plugin, n.formId & 0x00FFFFFF});

	static const char* vanillaESMs[] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm", nullptr};
	std::set<std::string> vanillaLower;
	for (int i = 0; vanillaESMs[i]; ++i)
		vanillaLower.insert(ToLower(vanillaESMs[i]));

	wxArrayString allPlugins;
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &allPlugins, "*.esp", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &allPlugins, "*.esm", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &allPlugins, "*.esl", wxDIR_FILES);

	size_t scanned = 0;
	size_t added = 0;

	for (size_t pi = 0; pi < allPlugins.size(); ++pi) {
		std::string pluginPath = allPlugins[pi].ToStdString();
		wxFileName pluginFn(allPlugins[pi]);
		std::string pluginFile = pluginFn.GetFullName().ToStdString();
		if (vanillaLower.count(ToLower(pluginFile)))
			continue;

		esp::ESPReader reader;
		if (!reader.Load(pluginPath, {"NPC_"}))
			continue;
		++scanned;

		auto& readerMasters = reader.GetMasters();
		auto remapDoft = buildLocalToEspMastersRemap(readerMasters, pluginFile);

		for (auto& npc : reader.GetNPCs()) {
			if (npc.editorId.empty())
				continue;

			// Only native records — overrides are handled by LoadNPCs's
			// override-scan pass (they update an existing NPCEntry rather
			// than introducing a new one).
			uint8_t topByte = (npc.formId >> 24) & 0xFF;
			if (topByte < readerMasters.size())
				continue;

			uint32_t baseFormId = npc.formId & 0x00FFFFFF;
			if (alreadyLoaded.count({pluginFile, baseFormId}))
				continue;

			NPCEntry entry;
			entry.editorId = npc.editorId;
			entry.formId = npc.formId;
			entry.plugin = pluginFile;
			entry.wnamFormId = npc.wnamFormId;
			entry.isFemale = npc.isFemale;
			entry.overrides.emplace_back(pluginFile, npc.formId);

			if (npc.defaultOutfitFormId != 0) {
				entry.defaultOutfitFormIdRaw = npc.defaultOutfitFormId;
				entry.defaultOutfitSourcePlugin = pluginFile;
				entry.defaultOutfitFormId = remapDoft(npc.defaultOutfitFormId);
			}

			if (!npc.fullName.empty() && npc.fullName[0] != '[')
				entry.fullName = npc.fullName;
			const std::string& shownName = !entry.fullName.empty() ? entry.fullName : npc.editorId;
			entry.displayName = shownName + " (" + pluginFile + ", " + npc.editorId + ")";

			if (npc.hasQnam) {
				auto clamp8 = [](float v) -> uint8_t {
					if (v < 0.0f)
						v = 0.0f;
					if (v > 1.0f)
						v = 1.0f;
					return static_cast<uint8_t>(v * 255.0f + 0.5f);
				};
				entry.tintR = clamp8(npc.qnamR);
				entry.tintG = clamp8(npc.qnamG);
				entry.tintB = clamp8(npc.qnamB);
				entry.hasTint = true;
			}

			alreadyLoaded.insert({pluginFile, baseFormId});
			npcs.push_back(std::move(entry));
			++added;
		}
	}

	std::sort(npcs.begin(), npcs.end(), [](const NPCEntry& a, const NPCEntry& b) { return a.displayName < b.displayName; });

	wxLogMessage("LeveledListData::LoadAllPluginNativeNPCs: scanned %zu plugins, added %zu mod-native NPCs (total %zu)", scanned, added, npcs.size());
}

// ---------------------------------------------------------------------------
// LoadRecordsFromESP — load records from a single file into caches
// ---------------------------------------------------------------------------

void LeveledListData::LoadRecordsFromESP(
	const std::string& filepath, const std::set<std::string>& types, uint16_t globalPluginIdx, const std::vector<std::string>& mainMasters, int masterLoadDepth, bool noClobber) {
	esp::ESPReader reader;
	if (!reader.Load(filepath, types)) {
		wxLogWarning("LeveledListData: Failed to load %s", filepath);
		return;
	}

	// Build FormID remapping table.
	// This file's own records have top byte == len(its masters) → remap to globalPluginIdx.
	// Its masters' records need to be remapped to the main ESP's master indices.
	uint8_t selfIndex = static_cast<uint8_t>(reader.GetMasters().size());
	auto& fileMasters = reader.GetMasters();

	// Build a map: this file's master index (0..255, plugin-local) → resolved
	// 16-bit globalPluginIdx used in cache keys. First try the main ESP's master
	// list (matched case-insensitive by filename). Then fall back to any plugin
	// already loaded as a dynamic master — this is what lets an override
	// plugin's references to e.g. "Midnight Huntress.esp" be remapped to its
	// synthetic slot instead of falling through and colliding with an unrelated
	// main-ESP master. Without this, an OTFT that mixes references to mapped
	// and unmapped masters loses the unmapped entries silently.
	std::unordered_map<uint8_t, uint16_t> masterRemap;
	for (size_t fi = 0; fi < fileMasters.size(); ++fi) {
		std::string fileMasterLower = ToLower(fileMasters[fi]);
		bool mapped = false;
		for (size_t mi = 0; mi < mainMasters.size(); ++mi) {
			if (ToLower(mainMasters[mi]) == fileMasterLower) {
				masterRemap[static_cast<uint8_t>(fi)] = static_cast<uint16_t>(mi);
				mapped = true;
				break;
			}
		}
		if (mapped)
			continue;
		for (size_t di = 0; di < dynamicMasters.size(); ++di) {
			const std::string& dyn = dynamicMasters[di];
			auto slash = dyn.find_last_of("/\\");
			std::string dbase = ToLower(slash == std::string::npos ? dyn : dyn.substr(slash + 1));
			if (dbase == fileMasterLower) {
				masterRemap[static_cast<uint8_t>(fi)] = DynamicGlobalIdx(di);
				break;
			}
		}
	}

	// NB: We intentionally do not lazy-load this file's master plugins here.
	// Doing so during the main load path would recursively pull in entire
	// dependency chains (e.g. the user-opened combined ESP declares ~200+
	// masters), exhausting plugin slots before any override-only plugin can be
	// loaded. Scoped master expansion happens in
	// ScanAllPluginsForOutfitOverrides instead, only for plugins that
	// actually own overrides we want to surface.
	(void)masterLoadDepth;

	auto remapFid = [&](uint32_t fid) -> FormId {
		uint8_t topByte = (fid >> 24) & 0xFF;
		uint32_t baseId = fid & 0x00FFFFFF;
		if (topByte == selfIndex) {
			return EncodeFormId(globalPluginIdx, baseId);
		}
		auto it = masterRemap.find(topByte);
		if (it != masterRemap.end()) {
			return EncodeFormId(it->second, baseId);
		}
		// Unmapped: encode in the loaded file's own master-local space so the
		// raw bytes survive round-tripping, but using a synthetic high half
		// that cannot collide with any allocated globalPluginIdx. We pack the
		// raw top byte into the high 8 bits of the 16-bit slot, leaving the
		// low 8 bits 0xFF as a sentinel. This lets diagnostic logs identify
		// such entries while keeping the FormId unique.
		uint16_t synthSlot = static_cast<uint16_t>((static_cast<uint16_t>(topByte) << 8) | 0x00FFu);
		return EncodeFormId(synthSlot, baseId);
	};

	wxLogMessage("LeveledListData: Loaded %s (globalPluginIdx=%u, selfIndex=%d, %zu ARMOs, %zu ARMAs, %zu TXSTs, %zu OTFTs, %zu LVLIs)",
				 wxString(filepath),
				 static_cast<unsigned>(globalPluginIdx),
				 selfIndex,
				 reader.GetArmors().size(),
				 reader.GetArmorAddons().size(),
				 reader.GetTextureSets().size(),
				 reader.GetOutfits().size(),
				 reader.GetLeveledItems().size());

	// Cache ARMO records. Overwrite: in Bethesda load order, later masters
	// override earlier ones for the same (remapped) FormID.
	for (auto& ar : reader.GetArmors()) {
		FormId remappedId = remapFid(ar.formId);
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
		// Keep main-ESP-cached ARMO records authoritative. Many override
		// plugins re-state an ARMO only to tweak keywords/enchantments and
		// omit MOD2/MOD4 — letting them clobber would strip the model path
		// and the previewer would render nothing for that piece. OTFT/LVLI
		// records intentionally remain overridable so load-order edits to
		// outfit lists (e.g. MoLLi Clothing) take effect.
		if (mainEspLoaded && mainEspArmoIds.count(remappedId))
			continue;
		if (noClobber && armoCache.count(remappedId))
			continue; // dependency-fill load — keep the already-cached winner
		armoCache[remappedId] = std::move(ca);
	}

	// Cache ARMA records. Overwrite (later master wins).
	for (auto& aa : reader.GetArmorAddons()) {
		FormId remappedId = remapFid(aa.formId);
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
		if (mainEspLoaded && mainEspArmaIds.count(remappedId))
			continue;
		if (noClobber && armaCache.count(remappedId))
			continue;
		armaCache[remappedId] = std::move(cam);
	}

	// Cache TXST records. Overwrite (later master wins).
	for (auto& ts : reader.GetTextureSets()) {
		FormId remappedId = remapFid(ts.formId);
		CachedTXST ct;
		ct.editorId = ts.editorId;
		for (int i = 0; i < 8; ++i)
			ct.textures[i] = ts.textures[i];
		if (mainEspLoaded && mainEspTxstIds.count(remappedId))
			continue;
		if (noClobber && txstCache.count(remappedId))
			continue;
		txstCache[remappedId] = std::move(ct);
	}

	// Cache NPC_ WNAM references (editorId → remapped WNAM FormID).
	// Always overwrite: later masters/plugins take priority.
	for (auto& npc : reader.GetNPCs()) {
		if (npc.editorId.empty() || npc.wnamFormId == 0)
			continue;
		if (noClobber && npcSkinCache.count(npc.editorId))
			continue;
		npcSkinCache[npc.editorId] = remapFid(npc.wnamFormId);
	}

	// Cache RACE records (remapped FormID → race info). Overwrite (later master wins).
	// raceByEditorId is keyed by editor id; overwrite it too so it tracks the same winner.
	for (auto& r : reader.GetRaces()) {
		FormId remappedId = remapFid(r.formId);
		if (noClobber && raceCache.count(remappedId))
			continue;
		CachedRACE cr;
		cr.editorId = r.editorId;
		cr.skinFormId = r.skinFormId != 0 ? remapFid(r.skinFormId) : 0;
		raceCache[remappedId] = std::move(cr);
		if (!r.editorId.empty())
			raceByEditorId[r.editorId] = remappedId;
	}

	// Cache HDPTs
	for (auto& hp : reader.GetHeadParts()) {
		FormId remappedId = remapFid(hp.formId);
		CachedHeadPart chp;
		chp.editorId = hp.editorId;
		chp.model = hp.model;
		chp.type = hp.type;
		if (mainEspLoaded && mainEspHdptIds.count(remappedId))
			continue;
		if (noClobber && headPartCache.count(remappedId))
			continue;
		headPartCache[remappedId] = std::move(chp);
	}

	// Cache OTFT records. Overwrite (later master wins). Required so vanilla
	// NPC default outfits (DOFT) can be resolved even when the loaded main ESP
	// doesn't override the OTFT itself.
	// A remap is "safe" when every referenced top byte is the file's own
	// selfIndex or a master we successfully mapped to a main-ESP master.
	// References whose top byte falls through unchanged (override plugin has a
	// private master the main ESP doesn't list) would collide with whatever
	// main-ESP master happens to share that index and surface as a wrong
	// stub. When that happens we keep the previously cached OTFT/LVLI entry
	// (e.g. the main modular ESP's clean version) rather than letting a
	// broken override clobber it.
	auto remapIsSafe = [&](uint32_t fid) -> bool {
		uint8_t tb = (fid >> 24) & 0xFF;
		if (tb == selfIndex)
			return true;
		return masterRemap.find(tb) != masterRemap.end();
	};
	for (auto& ot : reader.GetOutfits()) {
		FormId remappedId = remapFid(ot.formId);
		CachedOTFT co;
		co.editorId = ot.editorId;
		co.sourcePlugin = wxFileName(wxString::FromUTF8(filepath)).GetFullName().ToStdString();
		co.items.reserve(ot.items.size());
		bool allSafe = true;
		for (uint32_t inam : ot.items) {
			if (!remapIsSafe(inam))
				allSafe = false;
			co.items.push_back(remapFid(inam));
		}
		if (noClobber && otftCache.count(remappedId))
			continue;
		if (!allSafe && otftCache.count(remappedId)) {
			wxLogMessage("  Skipping OTFT %08X override from '%s': contains unmappable master references", remappedId, filepath);
			continue;
		}
		otftCache[remappedId] = std::move(co);
	}

	// Cache LVLI records. Overwrite (later master wins). Needed so OTFTs that
	// reference leveled-item lists (variant outfits) can be expanded.
	for (auto& li : reader.GetLeveledItems()) {
		FormId remappedId = remapFid(li.formId);
		CachedLVLI cl;
		cl.editorId = li.editorId;
		cl.flags = li.flags;
		cl.entries.reserve(li.entries.size());
		bool allSafe = true;
		for (auto& e : li.entries) {
			if (!remapIsSafe(e.reference))
				allSafe = false;
			cl.entries.emplace_back(remapFid(e.reference), e.level);
		}
		if (noClobber && lvliCache.count(remappedId))
			continue;
		if (!allSafe && lvliCache.count(remappedId)) {
			wxLogMessage("  Skipping LVLI %08X override from '%s': contains unmappable master references", remappedId, filepath);
			continue;
		}
		lvliCache[remappedId] = std::move(cl);
	}
}

// ---------------------------------------------------------------------------
// MakeStubPiece — placeholder for unresolved ARMO references
// ---------------------------------------------------------------------------

OutfitPiece LeveledListData::MakeStubPiece(FormId formId) {
	OutfitPiece piece;
	piece.formId = formId;
	piece.name = "[" + FormIdToString(formId) + "]";
	piece.armorType = "Unknown";
	return piece;
}

// ---------------------------------------------------------------------------
// BuildOutfitFromOtft — assemble OutfitEntry from a cached OTFT
// ---------------------------------------------------------------------------

// Forward declaration; defined later in this TU.
static void AddPiecesFromArmo(lldata::FormId formId,
							  const CachedArmo& armo,
							  const std::unordered_map<lldata::FormId, CachedArmo>& armoCache,
							  const std::unordered_map<lldata::FormId, CachedARMA>& armaCache,
							  const std::unordered_map<lldata::FormId, CachedTXST>& txstCache,
							  std::vector<OutfitPiece>& outPieces);

OutfitEntry LeveledListData::BuildOutfitFromOtft(FormId otftId, int& groupCounter, int* outPieces, int* outUnresolved) const {
	OutfitEntry entry;
	auto otftIt = otftCache.find(otftId);
	if (otftIt == otftCache.end())
		return entry;

	const auto& otft = otftIt->second;
	entry.formId = otftId;
	entry.editorId = otft.editorId;
	entry.name = otft.editorId;
	entry.sourcePlugin = otft.sourcePlugin;
	entry.minLevel = 1;

	int totalPieces = 0;
	int unresolvedCount = 0;

	for (lldata::FormId itemId : otft.items) {
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

		// LVLI reference? Resolve with Use Any group tracking.
		auto lvliIt = lvliCache.find(itemId);
		if (lvliIt != lvliCache.end()) {
			std::vector<OutfitPiece> resolvedPieces;
			std::vector<std::pair<lldata::FormId, uint16_t>> unresolvedRefs;
			ResolveLVLIGrouped(itemId, 1, {}, groupCounter, resolvedPieces, unresolvedRefs, entry.useAnyGroupNames, entry.useAnyGroups);

			// Deduplicate ARMOs: within each gating path, keep first occurrence
			// of each (FormID, nifPath). Across different paths the same ARMO
			// is expected (variants share common pieces).
			{
				std::set<std::tuple<std::vector<std::pair<int, int>>, lldata::FormId, std::string>> seen;
				std::vector<OutfitPiece> deduped;
				for (auto& p : resolvedPieces) {
					auto key = std::make_tuple(p.useAnyPath, p.formId, p.nifPath);
					if (seen.insert(key).second)
						deduped.push_back(std::move(p));
				}
				resolvedPieces = std::move(deduped);
			}

			for (auto& piece : resolvedPieces) {
				if (!piece.useAnyPath.empty()) {
					std::string pathStr;
					for (auto& [g, v] : piece.useAnyPath) {
						if (!pathStr.empty())
							pathStr += " > ";
						pathStr += std::to_string(g) + "/" + std::to_string(v);
					}
					wxLogMessage("  LVLI piece '%s' [%08X]: path=%s", wxString(piece.name), piece.formId, pathStr);
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

	// Final dedup pass across the entire outfit. Some OTFTs list multiple
	// distinct ARMOs that share the same mesh path (e.g. SaltLemon Wine
	// Duchess has separate ARMO records 71000810 and 71000835 both pointing
	// at Nails_1.nif). These render identically and look like UI duplicates
	// to the user. Collapse them by (nifPath, useAnyPath); keep the first
	// occurrence so the original ordering and the per-gate dedup above are
	// preserved.
	{
		std::set<std::pair<std::string, std::vector<std::pair<int, int>>>> seenPieces;
		std::vector<OutfitPiece> deduped;
		deduped.reserve(entry.pieces.size());
		for (auto& p : entry.pieces) {
			// Don't collapse stubs (empty nifPath) — they carry distinct
			// FormIDs the user may want to see listed.
			if (p.nifPath.empty()) {
				deduped.push_back(std::move(p));
				continue;
			}
			auto key = std::make_pair(p.nifPath, p.useAnyPath);
			if (seenPieces.insert(key).second)
				deduped.push_back(std::move(p));
		}
		entry.pieces = std::move(deduped);
	}

	if (outPieces)
		*outPieces = static_cast<int>(entry.pieces.size()) - unresolvedCount;
	if (outUnresolved)
		*outUnresolved = unresolvedCount;
	return entry;
}

// ---------------------------------------------------------------------------
// ResolveNpcDefaultOutfit — DOFT → OTFT → OutfitEntry for NPC mode
// ---------------------------------------------------------------------------

const OutfitEntry* LeveledListData::ResolveNpcDefaultOutfit(const NPCEntry& npc) {
	auto cached = npcOutfitCache.find(npc.editorId);
	if (cached != npcOutfitCache.end())
		return &cached->second;

	// Lazy-scan the Data folder for non-master plugins that override OTFT/LVLI
	// records we already cache. Triggered on the first NPC outfit resolution so
	// the scan cost is only paid when the NPC tab is actually used. After this
	// runs, otftCache and lvliCache reflect overrides from mod plugins that are
	// not masters of the loaded main ESP.
	ScanAllPluginsForOutfitOverrides();

	if (npc.defaultOutfitFormId == 0) {
		if (npc.defaultOutfitFormIdRaw == 0) {
			// NPC has no DOFT at all — typical for creatures (e.g. Ice Wraith,
			// Wolf, Draugr animals). Nothing to render and nothing to warn about.
			wxLogMessage("ResolveNpcDefaultOutfit: NPC '%s' has no DOFT subrecord (creature or unequipped NPC)", npc.editorId.c_str());
			return nullptr;
		}

		// DOFT set but its source plugin isn't a master of the loaded ESP.
		// Lazy-load the defining plugin via EnsureDynamicMaster so the OTFT
		// (and any LVLI/ARMO it references) end up in the caches, then remap
		// the raw FormID into the synthetic master space.
		if (baseDataPath.empty() || npc.defaultOutfitSourcePlugin.empty()) {
			wxLogWarning("ResolveNpcDefaultOutfit: NPC '%s' DOFT unresolvable (raw=%08X, source='%s' — no baseDataPath or source plugin)",
						 npc.editorId.c_str(),
						 npc.defaultOutfitFormIdRaw,
						 npc.defaultOutfitSourcePlugin.c_str());
			return nullptr;
		}

		// Find the source plugin on disk (case-insensitive).
		std::string srcPath = baseDataPath + npc.defaultOutfitSourcePlugin;
		if (!wxFileName::FileExists(wxString::FromUTF8(srcPath))) {
			srcPath = ResolveCaseInsensitive(baseDataPath, npc.defaultOutfitSourcePlugin);
			if (srcPath.empty()) {
				wxLogWarning("ResolveNpcDefaultOutfit: NPC '%s' source plugin '%s' not found on disk", npc.editorId.c_str(), npc.defaultOutfitSourcePlugin.c_str());
				return nullptr;
			}
		}

		// Read the source plugin's master list so we can map the raw DOFT FormID
		// to whichever plugin actually defines the OTFT record.
		esp::ESPReader probe;
		if (!probe.Load(srcPath, {})) {
			wxLogWarning("ResolveNpcDefaultOutfit: failed to open source plugin '%s'", srcPath.c_str());
			return nullptr;
		}
		auto& srcMasters = probe.GetMasters();
		uint8_t srcSelfIdx = static_cast<uint8_t>(srcMasters.size());
		uint8_t doftTopByte = (npc.defaultOutfitFormIdRaw >> 24) & 0xFF;
		uint32_t doftBase = npc.defaultOutfitFormIdRaw & 0x00FFFFFF;

		std::string otftDefiningPlugin;
		if (doftTopByte == srcSelfIdx)
			otftDefiningPlugin = srcPath;
		else if (doftTopByte < srcMasters.size()) {
			std::string masterPath = baseDataPath + srcMasters[doftTopByte];
			if (!wxFileName::FileExists(wxString::FromUTF8(masterPath)))
				masterPath = ResolveCaseInsensitive(baseDataPath, srcMasters[doftTopByte]);
			otftDefiningPlugin = masterPath;
		}

		if (otftDefiningPlugin.empty()) {
			wxLogWarning("ResolveNpcDefaultOutfit: NPC '%s' OTFT %08X — defining plugin not located on disk", npc.editorId.c_str(), npc.defaultOutfitFormIdRaw);
			return nullptr;
		}

		uint16_t synthIdx = EnsureDynamicMaster(otftDefiningPlugin, /*masterLoadDepth*/ 0, /*noClobber*/ true);
		if (synthIdx == 0) {
			wxLogWarning("ResolveNpcDefaultOutfit: NPC '%s' lazy-load of '%s' failed", npc.editorId.c_str(), otftDefiningPlugin.c_str());
			return nullptr;
		}

		FormId lazyOtftId = EncodeFormId(synthIdx, doftBase);
		if (otftCache.find(lazyOtftId) == otftCache.end()) {
			wxLogWarning("ResolveNpcDefaultOutfit: NPC '%s' OTFT %08X not present in '%s' after lazy-load", npc.editorId.c_str(), lazyOtftId, otftDefiningPlugin.c_str());
			return nullptr;
		}

		// Mutate the NPC entry so subsequent calls / the cache hit the resolved
		// ID directly. The NPC list owns these entries by reference so this
		// edit is harmless and avoids re-running the probe on next click.
		const_cast<NPCEntry&>(npc).defaultOutfitFormId = lazyOtftId;
		wxLogMessage("ResolveNpcDefaultOutfit: NPC '%s' lazy-resolved DOFT raw=%08X -> %08X (defined in '%s')",
					 npc.editorId.c_str(),
					 npc.defaultOutfitFormIdRaw,
					 lazyOtftId,
					 otftDefiningPlugin.c_str());
	}

	if (otftCache.find(npc.defaultOutfitFormId) == otftCache.end()) {
		wxLogMessage("ResolveNpcDefaultOutfit: OTFT %08X for NPC '%s' not in otftCache", npc.defaultOutfitFormId, npc.editorId.c_str());
		return nullptr;
	}

	// Use a per-call group counter so group IDs don't collide with the main
	// outfit list. NPC-mode outfits live in their own cache and never coexist
	// with the main outfits in the previewer's useAnyGroups_ vector.
	int groupCounter = 100000 + static_cast<int>(npcOutfitCache.size());
	OutfitEntry entry = BuildOutfitFromOtft(npc.defaultOutfitFormId, groupCounter);
	if (entry.pieces.empty()) {
		wxLogMessage("ResolveNpcDefaultOutfit: OTFT %08X built empty for NPC '%s'", npc.defaultOutfitFormId, npc.editorId.c_str());
		return nullptr;
	}

	// Use the NPC's full name (or editor id) for the display name so the UI
	// can show "Lydia — default outfit" rather than the OTFT editor id.
	const std::string& shown = !npc.fullName.empty() ? npc.fullName : npc.editorId;
	entry.name = shown + " — " + entry.editorId;

	auto [it, inserted] = npcOutfitCache.emplace(npc.editorId, std::move(entry));
	(void)inserted;
	return &it->second;
}

// ---------------------------------------------------------------------------
// ResolveModelPath — follow TNAM template chain to find models
// ---------------------------------------------------------------------------

/// Resolve the worn mesh paths and ARMA records for an ARMO.
/// Returns a list of (mesh path, ARMA pointer) pairs.
static std::vector<std::pair<std::string, const CachedARMA*>> ResolveWornMeshes(const CachedArmo& armo,
																				const std::unordered_map<lldata::FormId, CachedArmo>& armoCache,
																				const std::unordered_map<lldata::FormId, CachedARMA>& armaCache) {
	std::vector<std::pair<std::string, const CachedARMA*>> results;
	std::set<std::string> seenNifs;

	// Add an ARMA's model to results. If femaleOnly is true, only use modelFemale.
	auto addArma = [&](lldata::FormId armaId, bool femaleOnly) {
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
	auto collectArmatures = [&](const std::vector<lldata::FormId>& ids) {
		for (lldata::FormId armaId : ids)
			addArma(armaId, true);
		if (results.empty()) {
			for (lldata::FormId armaId : ids)
				addArma(armaId, false);
		}
	};

	// Try this ARMO's ARMA references first
	collectArmatures(armo.armatureIds);

	// If no meshes found, follow template chain (enchanted copies inherit from base armor)
	if (results.empty()) {
		lldata::FormId tid = armo.templateId;
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
static void AddPiecesFromArmo(lldata::FormId formId,
							  const CachedArmo& armo,
							  const std::unordered_map<lldata::FormId, CachedArmo>& armoCache,
							  const std::unordered_map<lldata::FormId, CachedARMA>& armaCache,
							  const std::unordered_map<lldata::FormId, CachedTXST>& txstCache,
							  std::vector<OutfitPiece>& outPieces) {
	auto meshes = ResolveWornMeshes(armo, armoCache, armaCache);

	if (meshes.empty()) {
		// Add a single piece with no mesh so it's visible as "missing" in logs
		OutfitPiece piece;
		piece.formId = formId;
		piece.name = armo.fullName;
		piece.editorId = armo.editorId;
		piece.armorType = armo.armorType;
		piece.bodySlots = DecodeBodySlots(armo.bodySlotFlags);
		outPieces.push_back(std::move(piece));
		return;
	}

	for (auto& [modelPath, arma] : meshes) {
		OutfitPiece piece;
		piece.formId = formId;
		piece.name = armo.fullName;
		piece.editorId = armo.editorId;
		piece.armorType = armo.armorType;
		// Use ARMO-level body slot flags — the ARMO record is the authoritative declaration
		// of which body slots this armor piece occupies, matching how the game processes it.
		piece.bodySlots = DecodeBodySlots(armo.bodySlotFlags);
		piece.nifPath = NormalizeMeshPath(modelPath);

		// Resolve alternate textures from the matched ARMA
		if (arma) {
			// Capture race(s) targeted by this ARMA so the previewer can filter
			// out pieces whose ARMA doesn't apply to the selected NPC's race
			// (e.g. ARMOs that bundle one ARMA per race).
			piece.armaRaceFormId = arma->raceFormId;
			piece.armaAdditionalRaceFormIds = arma->additionalRaceFormIds;
			// Capture the ARMA's body slot flags. They are usually a superset
			// of the ARMO flags and list every dismember partition the NIF's
			// shapes are allowed to use — used by the previewer to hide shapes
			// (e.g. SMP/collision rigging meshes) whose partition isn't
			// covered by this ARMA.
			piece.armaBodySlots = DecodeBodySlots(arma->bodySlotFlags);

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
	npcOutfitCache.clear();
	npcSkinsScanned = false;
	// dynamicMasters MUST be cleared too: it tracks which plugins were
	// loaded into raceCache/armoCache via EnsureDynamicMaster. If left
	// populated across reloads, EnsureDynamicMaster() short-circuits and
	// returns the cached synthetic index without re-loading the records
	// into the just-cleared caches, leaving NPC race/skin overrides
	// unresolvable (e.g. body/hands/feet NIFs disappear after ESP reload).
	dynamicMasters.clear();
	espMasters.clear();
	mainEspArmoIds.clear();
	mainEspArmaIds.clear();
	mainEspTxstIds.clear();
	mainEspOtftIds.clear();
	mainEspLvliIds.clear();
	mainEspRaceIds.clear();
	mainEspHdptIds.clear();
	mainEspLoaded = false;
	loadInfo.clear();
	outfitOverridesScanned = false;

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

		LoadRecordsFromESP(masterPath, {"ARMO", "ARMA", "TXST", "NPC_", "RACE", "HDPT", "OTFT", "LVLI"}, masterIdx, masters);
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
		ca.armatureIds.assign(ar.armatureIds.begin(), ar.armatureIds.end()); // main-ESP-local FormIDs already have main-ESP top byte
		armoCache[ar.formId] = std::move(ca);								 // overwrite any master version
		mainEspArmoIds.insert(ar.formId);
	}

	for (auto& aa : mainReader.GetArmorAddons()) {
		CachedARMA cam;
		cam.editorId = aa.editorId;
		cam.modelMale = aa.modelMale;
		cam.modelFemale = aa.modelFemale;
		cam.bodySlotFlags = aa.bodySlotFlags;
		cam.raceFormId = aa.raceId;																// main-ESP FormIDs need no remap
		cam.additionalRaceFormIds.assign(aa.additionalRaces.begin(), aa.additionalRaces.end()); // main-ESP FormIDs need no remap
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
		mainEspArmaIds.insert(aa.formId);
	}

	for (auto& ts : mainReader.GetTextureSets()) {
		CachedTXST ct;
		ct.editorId = ts.editorId;
		for (int i = 0; i < 8; ++i)
			ct.textures[i] = ts.textures[i];
		txstCache[ts.formId] = std::move(ct);
		mainEspTxstIds.insert(ts.formId);
	}

	for (auto& li : mainReader.GetLeveledItems()) {
		CachedLVLI cl;
		cl.editorId = li.editorId;
		cl.flags = li.flags;
		for (auto& e : li.entries)
			cl.entries.emplace_back(e.reference, e.level);
		lvliCache[li.formId] = std::move(cl);
		mainEspLvliIds.insert(li.formId);
	}

	for (auto& ot : mainReader.GetOutfits()) {
		CachedOTFT co;
		co.editorId = ot.editorId;
		co.sourcePlugin = wxFileName(wxString::FromUTF8(espFilename)).GetFullName().ToStdString();
		co.items.assign(ot.items.begin(), ot.items.end());
		otftCache[ot.formId] = std::move(co);
		mainEspOtftIds.insert(ot.formId);
	}

	for (auto& hp : mainReader.GetHeadParts()) {
		CachedHeadPart chp;
		chp.editorId = hp.editorId;
		chp.model = hp.model;
		chp.type = hp.type;
		headPartCache[hp.formId] = std::move(chp);
		mainEspHdptIds.insert(hp.formId);
	}

	mainEspLoaded = true;

	// Eagerly apply non-master plugin overrides BEFORE building the outfit
	// list. Previously this scan ran lazily on the first NPC-outfit resolve,
	// which meant the outfit panel showed pre-override contents until the
	// user clicked into the NPC tab — and outfits whose pieces are only
	// resolvable via mod-side overrides (e.g. Octi/J3 Vampire variants) were
	// invisible on first load. Running it here also lets MoLLi-style
	// overrides that REMOVE entries from a vanilla LVLI take effect on the
	// initial outfit list rather than producing a stale set.
	ScanAllPluginsForOutfitOverrides();

	// Build outfit entries from OTFT records
	int totalPieces = 0;
	int unresolvedCount = 0;
	int groupCounter = 0;

	for (auto& [otftId, otft] : otftCache) {
		int builtPieces = 0;
		int builtUnresolved = 0;
		OutfitEntry entry = BuildOutfitFromOtft(otftId, groupCounter, &builtPieces, &builtUnresolved);
		totalPieces += builtPieces;
		unresolvedCount += builtUnresolved;
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

void LeveledListData::ResolveLVLI(FormId formId,
								  uint16_t parentLevel,
								  std::vector<std::pair<lldata::FormId, uint16_t>>& armoRefs,
								  std::vector<std::pair<lldata::FormId, uint16_t>>& unresolvedRefs) const {
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

void LeveledListData::ResolveLVLIGrouped(FormId formId,
										 uint16_t parentLevel,
										 const std::vector<std::pair<int, int>>& parentPath,
										 int& groupCounter,
										 std::vector<OutfitPiece>& pieces,
										 std::vector<std::pair<lldata::FormId, uint16_t>>& unresolvedRefs,
										 std::map<int, std::string>& groupNames,
										 std::vector<OutfitGroupInfo>& groups) const {
	auto it = lvliCache.find(formId);
	if (it == lvliCache.end())
		return;

	auto& lvli = it->second;
	// LVLF flag 0x04 = "Use All" — every entry is worn together. When unset
	// the game picks exactly one entry, which is the variant semantics the
	// previewer surfaces as a selectable group.
	bool isUseAll = (lvli.flags & 0x04) != 0;

	if (isUseAll) {
		// Use All: every entry contributes pieces. The path is inherited from
		// the surrounding scope verbatim; this LVLI does not add a new gate.
		for (auto& [ref, level] : lvli.entries) {
			uint16_t effectiveLevel = std::max(parentLevel, level);

			auto armoIt = armoCache.find(ref);
			if (armoIt != armoCache.end()) {
				std::vector<OutfitPiece> resolved;
				AddPiecesFromArmo(ref, armoIt->second, armoCache, armaCache, txstCache, resolved);
				for (auto& piece : resolved) {
					piece.useAnyPath = parentPath;
					pieces.push_back(std::move(piece));
				}
				continue;
			}

			auto lvliIt2 = lvliCache.find(ref);
			if (lvliIt2 != lvliCache.end()) {
				ResolveLVLIGrouped(ref, effectiveLevel, parentPath, groupCounter, pieces, unresolvedRefs, groupNames, groups);
				continue;
			}

			unresolvedRefs.emplace_back(ref, effectiveLevel);
		}
	}
	else {
		// Not Use All ⇒ Use Any: introduce a new gate. Every distinct entry
		// becomes a variant of this group. Pieces from nested LVLIs inherit
		// the extended path so a deeper Use Any nests under this group's
		// variant.
		//
		// LVLIs frequently repeat the same reference multiple times to bias
		// the random selection toward that entry (weighted RNG). The previewer
		// surfaces variants as a dropdown for the user — repeating the same
		// armor as five separate dropdown entries is just visual noise, so we
		// collapse consecutive duplicates by ref FormID here. The first
		// occurrence's level wins; later duplicates are skipped entirely.
		std::vector<std::pair<lldata::FormId, uint16_t>> uniqueEntries;
		{
			std::set<uint32_t> seenRefs;
			for (auto& [ref, level] : lvli.entries) {
				if (seenRefs.insert(ref).second)
					uniqueEntries.emplace_back(ref, level);
			}
		}

		int groupId = groupCounter++;
		groupNames[groupId] = lvli.editorId;

		// Pre-allocate the group info entry so every variant slot — including
		// stub/unresolved references — appears in the variant tree even when
		// it produces no pieces.
		OutfitGroupInfo gi;
		gi.groupId = groupId;
		gi.label = lvli.editorId;
		if (!parentPath.empty()) {
			gi.parentGroupId = parentPath.back().first;
			gi.parentVariantIdx = parentPath.back().second;
		}
		int variantIdx = 0;
		wxLogMessage("  LVLI '%s' [%08X] is Use Any → group %d with %zu variants (depth %zu, deduped from %zu raw entries)",
					 wxString(lvli.editorId),
					 formId,
					 groupId,
					 uniqueEntries.size(),
					 parentPath.size(),
					 lvli.entries.size());

		for (auto& [ref, level] : uniqueEntries) {
			uint16_t effectiveLevel = std::max(parentLevel, level);

			std::vector<std::pair<int, int>> childPath = parentPath;
			childPath.emplace_back(groupId, variantIdx);

			OutfitVariantInfo vi;
			vi.variantIdx = variantIdx;
			vi.refFormId = ref;

			auto armoIt = armoCache.find(ref);
			if (armoIt != armoCache.end()) {
				vi.label = !armoIt->second.fullName.empty() ? armoIt->second.fullName : armoIt->second.editorId;
				if (vi.label.empty()) {
					char buf[16];
					std::snprintf(buf, sizeof(buf), "[%08X]", ref);
					vi.label = buf;
				}
				gi.variants.push_back(std::move(vi));

				std::vector<OutfitPiece> resolved;
				AddPiecesFromArmo(ref, armoIt->second, armoCache, armaCache, txstCache, resolved);
				for (auto& piece : resolved) {
					piece.useAnyPath = childPath;
					pieces.push_back(std::move(piece));
				}
				++variantIdx;
				continue;
			}

			auto lvliIt2 = lvliCache.find(ref);
			if (lvliIt2 != lvliCache.end()) {
				vi.label = !lvliIt2->second.editorId.empty() ? lvliIt2->second.editorId : ([ref]() {
					char buf[16];
					std::snprintf(buf, sizeof(buf), "[%08X]", ref);
					return std::string(buf);
				})();
				vi.gatesSubLvli = true;
				gi.variants.push_back(std::move(vi));

				ResolveLVLIGrouped(ref, effectiveLevel, childPath, groupCounter, pieces, unresolvedRefs, groupNames, groups);
				++variantIdx;
				continue;
			}

			// Unresolved reference: still emit a variant slot so the tree UI
			// renders it (greyed-out in practice — the previewer will show the
			// stub label and selecting it just yields no pieces).
			char buf[24];
			std::snprintf(buf, sizeof(buf), "[%08X]", ref);
			vi.label = buf;
			gi.variants.push_back(std::move(vi));

			unresolvedRefs.emplace_back(ref, effectiveLevel);
			++variantIdx;
		}

		groups.push_back(std::move(gi));
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

std::array<std::string, 8> LeveledListData::ResolveSkinTextures(FormId wnamFormId, FormId npcRaceFormId) const {
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
		for (lldata::FormId armaId : armo.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end())
				continue;

			auto& arma = armaIt->second;
			if (!(arma.bodySlotFlags & (1u << 2))) // bit 2 = slot 32 = Body
				continue;
			if (requireRaceMatch && npcRaceFormId != 0) {
				bool match = arma.raceFormId == npcRaceFormId;
				if (!match) {
					for (lldata::FormId r : arma.additionalRaceFormIds) {
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

LeveledListData::BodyNifPaths LeveledListData::ResolveBodyNifPaths(FormId wnamFormId, bool highWeight, FormId npcRaceFormId) const {
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
		for (lldata::FormId armaId : armoIt->second.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end())
				continue;

			auto& arma = armaIt->second;
			if (requireRaceMatch && npcRaceFormId != 0) {
				bool match = arma.raceFormId == npcRaceFormId;
				if (!match) {
					for (lldata::FormId r : arma.additionalRaceFormIds) {
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

lldata::FormId LeveledListData::GetRaceSkinArmo(const std::string& raceEditorId) const {
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
	if (ovIt != npcSkinOverrides.end())
		ResolveLazyNpcSkin(ovIt->second);
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

std::set<lldata::FormId> LeveledListData::GetEffectiveArmorRaces(FormId npcRaceFormId) const {
	std::set<lldata::FormId> races;
	if (npcRaceFormId == 0)
		return races;
	races.insert(npcRaceFormId);

	auto raceIt = raceCache.find(npcRaceFormId);
	if (raceIt == raceCache.end() || raceIt->second.skinFormId == 0)
		return races;

	auto armoIt = armoCache.find(raceIt->second.skinFormId);
	if (armoIt == armoCache.end())
		return races;

	// Collect every race targeted by any ARMA hanging off the race's skin ARMO.
	// In Skyrim, this is what makes a custom child / variant race compatible
	// with vanilla armor: the skin ARMO references vanilla ARMAs whose RNAMs
	// list NordRace / DefaultRace / etc., so outfits whose ARMAs target those
	// same races are equippable on the custom race in-game.
	for (lldata::FormId armaId : armoIt->second.armatureIds) {
		auto armaIt = armaCache.find(armaId);
		if (armaIt == armaCache.end())
			continue;
		if (armaIt->second.raceFormId != 0)
			races.insert(armaIt->second.raceFormId);
		for (lldata::FormId r : armaIt->second.additionalRaceFormIds)
			races.insert(r);
	}

	// Walk template-armor chain too in case the skin ARMO is an enchanted
	// copy that inherits its ARMAs from a base.
	lldata::FormId tid = armoIt->second.templateId;
	for (int depth = 0; depth < 10 && tid != 0; ++depth) {
		auto tit = armoCache.find(tid);
		if (tit == armoCache.end())
			break;
		for (lldata::FormId armaId : tit->second.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end())
				continue;
			if (armaIt->second.raceFormId != 0)
				races.insert(armaIt->second.raceFormId);
			for (lldata::FormId r : armaIt->second.additionalRaceFormIds)
				races.insert(r);
		}
		tid = tit->second.templateId;
	}

	return races;
}

std::string LeveledListData::ResolveHeadPartNif(FormId headPartFormId) const {
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
	FormId npcRaceFid = effRace.formId;

	// Primary: NPC's own WNAM (prefer all-plugins override, fall back to master cache).
	FormId npcWnam = 0;
	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt != npcSkinOverrides.end())
		ResolveLazyNpcSkin(ovIt->second);
	if (ovIt != npcSkinOverrides.end() && !ovIt->second.selfDefined)
		npcWnam = ovIt->second.remappedWnam;
	if (npcWnam == 0) {
		auto scIt = npcSkinCache.find(npcEditorId);
		if (scIt != npcSkinCache.end())
			npcWnam = scIt->second;
	}
	if (npcWnam != 0)
		result = ResolveBodyNifPaths(npcWnam, highWeight, npcRaceFid);

	FormId raceWnam = 0;
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

	wxLogMessage("ResolveNpcBodyNifPaths: NPC '%s' race='%s' raceFID=%016llX npcWNAM=%016llX raceWNAM=%016llX -> body='%s' hands='%s' feet='%s'",
				 npcEditorId,
				 raceEdid,
				 static_cast<unsigned long long>(npcRaceFid),
				 static_cast<unsigned long long>(npcWnam),
				 static_cast<unsigned long long>(raceWnam),
				 result.body,
				 result.hands,
				 result.feet);

	return result;
}

// ---------------------------------------------------------------------------
// Per-slot skin texture resolution (body / hands / feet)
// ---------------------------------------------------------------------------

namespace {
// Fill `dst` from the ARMA's female skin TXST (NAM1). That is the TXST the
// game engine forces onto skin-tinted shapes for female bodies. MO3S/MO2S
// alt-textures are per-NiTriShape overrides for *non-skin* shapes in the
// worn mesh and must NOT be consulted here. NAM0 (male) is never used as a
// fallback — BodySlide renders female bodies, so a male TXST on a female
// mesh would be wrong.
bool FillFromArmaTextures(const CachedARMA& arma,
						  const std::unordered_map<lldata::FormId, CachedTXST>& txstCache,
						  std::array<std::string, 8>& dst) {
	if (arma.skinTextureFemale == 0)
		return false;
	auto it = txstCache.find(arma.skinTextureFemale);
	if (it == txstCache.end())
		return false;
	for (int i = 0; i < 8; ++i)
		if (!it->second.textures[i].empty())
			dst[i] = it->second.textures[i];
	return !dst[0].empty();
}

// Walk an ARMO's ARMA list filling per-slot texture arrays. Prefers race-matching ARMAs.
void FillPartTexturesFromArmo(lldata::FormId armoFid,
							  lldata::FormId npcRaceFid,
							  const std::unordered_map<lldata::FormId, CachedArmo>& armoCache,
							  const std::unordered_map<lldata::FormId, CachedARMA>& armaCache,
							  const std::unordered_map<lldata::FormId, CachedTXST>& txstCache,
							  LeveledListData::BodyPartTextures& out) {
	if (armoFid == 0)
		return;
	auto armoIt = armoCache.find(armoFid);
	if (armoIt == armoCache.end()) {
		wxLogMessage("FillPartTexturesFromArmo: ARMO %016llX not in cache", static_cast<unsigned long long>(armoFid));
		return;
	}

	wxLogMessage("FillPartTexturesFromArmo: walking ARMO %016llX (%zu ARMAs) raceHint=%016llX",
				 static_cast<unsigned long long>(armoFid),
				 armoIt->second.armatureIds.size(),
				 static_cast<unsigned long long>(npcRaceFid));

	auto tryFill = [&](bool requireRaceMatch) {
		for (lldata::FormId armaId : armoIt->second.armatureIds) {
			auto armaIt = armaCache.find(armaId);
			if (armaIt == armaCache.end()) {
				wxLogMessage("  ARMA %08X not in cache", armaId);
				continue;
			}
			auto& arma = armaIt->second;
			if (requireRaceMatch && npcRaceFid != 0) {
				bool match = arma.raceFormId == npcRaceFid;
				if (!match) {
					for (lldata::FormId r : arma.additionalRaceFormIds) {
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
						 armaId,
						 arma.raceFormId,
						 arma.bodySlotFlags,
						 altF,
						 altM,
						 arma.skinTextureFemale,
						 arma.skinTextureMale);

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
	FormId npcRaceFid = effRace.formId;

	// Pass 1: NPC's own WNAM (from all-plugin scan, remapped to main cache).
	FormId npcWnam = 0;
	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt != npcSkinOverrides.end())
		ResolveLazyNpcSkin(ovIt->second);
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
		FormId raceWnam = GetRaceSkinArmo(raceEdid);
		if (raceWnam != 0)
			FillPartTexturesFromArmo(raceWnam, npcRaceFid, armoCache, armaCache, txstCache, out);
	}

	wxLogMessage("ResolveNpcBodyPartTextures: NPC '%s' race='%s' raceFID=%016llX npcWNAM=%016llX -> body='%s' hands='%s' feet='%s'",
				 npcEditorId,
				 raceEdid,
				 static_cast<unsigned long long>(npcRaceFid),
				 static_cast<unsigned long long>(npcWnam),
				 out.body[0],
				 out.hands[0],
				 out.feet[0]);
	return out;
}

// ---------------------------------------------------------------------------
// EnsureDynamicMaster — load a non-master plugin's RACE/ARMO/ARMA/TXST records
// its WNAM at an ARMO it also defines) can be resolved.
// ---------------------------------------------------------------------------

uint16_t LeveledListData::EnsureDynamicMaster(const std::string& pluginPath, int masterLoadDepth, bool noClobber) {
	// If this plugin is already loaded as a main-ESP master, return its
	// existing globalPluginIdx so callers encode FormIDs against the slot
	// where the plugin's records are actually cached. Without this, an NPC
	// override whose WNAM points into a main-ESP master would get encoded
	// against a freshly-allocated dynamic slot whose cache is empty, and the
	// lookup would miss even though the record IS cached (just under a
	// different key).
	{
		auto sep = pluginPath.find_last_of("/\\");
		std::string base = (sep == std::string::npos) ? pluginPath : pluginPath.substr(sep + 1);
		std::string baseLower = ToLower(base);
		for (size_t i = 0; i < espMasters.size(); ++i) {
			if (ToLower(espMasters[i]) == baseLower)
				return static_cast<uint16_t>(i);
		}
	}
	for (size_t i = 0; i < dynamicMasters.size(); ++i) {
		if (dynamicMasters[i] == pluginPath)
			return DynamicGlobalIdx(i);
	}
	// Allocate a new globalPluginIdx slot. The 16-bit ordinal space (65535
	// plugins) is effectively unbounded for any realistic mod list, so the
	// old "synthetic master space exhausted" failure mode is gone.
	size_t nextSlot = dynamicMasters.size();
	uint16_t globalIdx = static_cast<uint16_t>(espMasters.size() + 1 + nextSlot);
	if (globalIdx == 0xFFFF) {
		wxLogWarning("EnsureDynamicMaster: 16-bit plugin space exhausted (%zu plugins loaded), refusing to load %s", static_cast<size_t>(globalIdx), pluginPath);
		return 0;
	}
	wxLogMessage("EnsureDynamicMaster: loading '%s' at globalPluginIdx=%u%s", pluginPath, static_cast<unsigned>(globalIdx), noClobber ? " (no-clobber)" : "");
	dynamicMasters.push_back(pluginPath);
	LoadRecordsFromESP(pluginPath, {"RACE", "ARMO", "ARMA", "TXST", "OTFT", "LVLI", "NPC_"}, globalIdx, espMasters, masterLoadDepth, noClobber);
	return globalIdx;
}

// ---------------------------------------------------------------------------
// FindSkyrimPluginsTxt — locate the user's Skyrim Plugins.txt to honour the
// real load order when lazy-loading override plugins. Without this we'd load
// alphabetically and a late-loading patch could clobber a mod whose override
// the user explicitly placed above it.
//   - Windows:   %LOCALAPPDATA%/Skyrim Special Edition/Plugins.txt
//   - Linux:     <steamapps>/compatdata/489830/pfx/drive_c/users/steamuser/
//                AppData/Local/Skyrim Special Edition/Plugins.txt
// Returns empty string if not found; caller falls back to alphabetical order.
// ---------------------------------------------------------------------------

static std::string FindSkyrimPluginsTxt(const std::string& baseDataPath) {
	auto tryPath = [](const std::string& p) -> std::string {
		if (!p.empty() && wxFileName::FileExists(wxString::FromUTF8(p)))
			return p;
		return {};
	};

#ifdef _WIN32
	wxString localAppData;
	if (wxGetEnv("LOCALAPPDATA", &localAppData)) {
		std::string p = localAppData.ToStdString() + "\\Skyrim Special Edition\\Plugins.txt";
		std::string r = tryPath(p);
		if (!r.empty())
			return r;
	}
#endif

	// Linux/Proton: Plugins.txt lives inside the Wine prefix under
	// compatdata/<appid>/pfx/drive_c/users/steamuser/AppData/Local/<game>.
	// Skyrim SE app id is 489830, Skyrim AE = 1716740, Skyrim LE = 72850.
	// We probe every common Steam install location plus an explicit env var
	// override (SKYRIM_PLUGINS_TXT) for users with non-standard setups.
	wxString explicitOverride;
	if (wxGetEnv("SKYRIM_PLUGINS_TXT", &explicitOverride)) {
		std::string r = tryPath(explicitOverride.ToStdString());
		if (!r.empty())
			return r;
	}

	wxString homeWx;
	std::string home;
	if (wxGetEnv("HOME", &homeWx))
		home = homeWx.ToStdString();

	std::vector<std::string> steamRoots;
	if (!home.empty()) {
		steamRoots.push_back(home + "/.steam/steam");
		steamRoots.push_back(home + "/.steam/debian-installation");
		steamRoots.push_back(home + "/.steam/root");
		steamRoots.push_back(home + "/.local/share/Steam");
		steamRoots.push_back(home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam");
	}

	static const std::array<const char*, 3> appIds = {"489830", "1716740", "72850"};
	static const std::array<const char*, 3> gameDirs = {"Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim"};

	for (auto& root : steamRoots) {
		for (auto* appId : appIds) {
			for (auto* game : gameDirs) {
				std::string p = root + "/steamapps/compatdata/" + appId + "/pfx/drive_c/users/steamuser/AppData/Local/" + game + "/Plugins.txt";
				std::string r = tryPath(p);
				if (!r.empty())
					return r;
			}
		}
	}

	// Fallback: walk up from baseDataPath to find a sibling compatdata path
	// (covers custom install dirs that still follow the standard layout).
	if (!baseDataPath.empty()) {
		std::string base = baseDataPath;
		while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
			base.pop_back();
		for (int strip = 0; strip < 5 && !base.empty(); ++strip) {
			auto pos = base.find_last_of("/\\");
			if (pos == std::string::npos)
				break;
			base = base.substr(0, pos);
			for (auto* appId : appIds) {
				for (auto* game : gameDirs) {
					std::string p = base + "/compatdata/" + appId + "/pfx/drive_c/users/steamuser/AppData/Local/" + game + "/Plugins.txt";
					std::string r = tryPath(p);
					if (!r.empty())
						return r;
				}
			}
		}
	}

	return {};
}

// ---------------------------------------------------------------------------
// ScanAllPluginsForOutfitOverrides — pre-load every ACTIVE plugin from the
// user's Skyrim load order into the dynamic master pool so all ARMO/ARMA/TXST/
// OTFT/LVLI/RACE/NPC_ records are resolvable from cache without any further
// lazy expansion. The 16-bit plugin pool removes the historical 256-slot cap,
// so we no longer need to be selective about which plugins to load.
//
// Load order: when Plugins.txt is available we iterate plugins in the user's
// real load order so "later load wins" matches the game. Without Plugins.txt
// we fall back to alphabetical filesystem order, which can pick the wrong
// override winner among multiple conflicting plugins.
//
// Inactive plugins (not listed in Plugins.txt with a leading '*') are skipped:
// they aren't part of the runtime load order so the game would never load
// them, and loading them here would waste RAM and could let an inactive
// override clobber an active winner.
// ---------------------------------------------------------------------------

void LeveledListData::ScanAllPluginsForOutfitOverrides() {
	if (outfitOverridesScanned)
		return;
	outfitOverridesScanned = true;

	if (baseDataPath.empty()) {
		wxLogMessage("ScanAllPluginsForOutfitOverrides: no baseDataPath set");
		return;
	}

	// Set of plugins already loaded (as masters or main ESP) — skip them.
	std::unordered_set<std::string> alreadyLoaded;
	for (auto& m : espMasters)
		alreadyLoaded.insert(ToLower(m));
	if (!espFilename.empty())
		alreadyLoaded.insert(ToLower(espFilename));

	// Parse Plugins.txt for the user's real load order. Active plugins are
	// prefixed with '*'; we only care about active ones since inactive plugins
	// aren't part of the runtime load order. Build a map: lowercase base name
	// → load order index.
	std::unordered_map<std::string, int> loadOrderRank;
	std::string pluginsTxt = FindSkyrimPluginsTxt(baseDataPath);
	if (!pluginsTxt.empty()) {
		std::ifstream f(pluginsTxt);
		std::string line;
		int rank = 0;
		while (std::getline(f, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line.front() != '*')
				continue;
			std::string name = line.substr(1);
			if (name.empty())
				continue;
			loadOrderRank[ToLower(name)] = rank++;
		}
		wxLogMessage("ScanAllPluginsForOutfitOverrides: parsed Plugins.txt '%s' with %zu active entries", pluginsTxt, loadOrderRank.size());
	}
	else {
		wxLogMessage("ScanAllPluginsForOutfitOverrides: no Plugins.txt found; falling back to alphabetical order");
	}

	wxArrayString pluginFiles;
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &pluginFiles, "*.esp", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &pluginFiles, "*.esm", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(baseDataPath), &pluginFiles, "*.esl", wxDIR_FILES);

	// Sort plugins by Plugins.txt load order. Plugins not present in
	// Plugins.txt are placed after all ordered plugins (still alphabetical
	// among themselves via stable sort). When Plugins.txt isn't available the
	// rank map is empty and the original alphabetical order is preserved.
	std::vector<std::string> orderedPlugins;
	orderedPlugins.reserve(pluginFiles.GetCount());
	for (auto& p : pluginFiles)
		orderedPlugins.push_back(p.ToStdString());

	if (!loadOrderRank.empty()) {
		std::stable_sort(orderedPlugins.begin(), orderedPlugins.end(), [&](const std::string& a, const std::string& b) {
			auto baseOf = [](const std::string& s) {
				auto pos = s.find_last_of("/\\");
				return ToLower(pos == std::string::npos ? s : s.substr(pos + 1));
			};
			auto ia = loadOrderRank.find(baseOf(a));
			auto ib = loadOrderRank.find(baseOf(b));
			int ra = (ia == loadOrderRank.end()) ? INT_MAX : ia->second;
			int rb = (ib == loadOrderRank.end()) ? INT_MAX : ib->second;
			return ra < rb;
		});
	}

	int considered = 0;
	int loaded = 0;
	int failed = 0;
	int skippedInactive = 0;

	// Eager load: every active plugin not already loaded, in load order.
	// EnsureDynamicMaster is idempotent — a plugin that some prior plugin
	// listed as a master may already be loaded by the time we reach it, in
	// which case the call is a no-op. Plugins.txt order guarantees masters
	// precede dependents (Bethesda load order rule enforced by mod managers),
	// so each plugin's own master list is already cached as dynamic masters
	// when remapFid runs, letting cross-references redirect cleanly.
	for (auto& pluginPath : orderedPlugins) {
		std::string base = wxFileName(wxString::FromUTF8(pluginPath)).GetFullName().ToStdString();
		if (alreadyLoaded.count(ToLower(base)))
			continue;
		if (!loadOrderRank.empty() && !loadOrderRank.count(ToLower(base))) {
			// Plugin file exists on disk but is not active in the user's
			// Skyrim load order — skip it. The game would never load it, so
			// neither should we.
			++skippedInactive;
			continue;
		}
		++considered;
		uint16_t idx = EnsureDynamicMaster(pluginPath, /*masterLoadDepth*/ 0, /*noClobber*/ true);
		if (idx != 0)
			++loaded;
		else
			++failed;
	}

	wxLogMessage("ScanAllPluginsForOutfitOverrides: considered %d active plugins (%d skipped inactive); loaded %d, failed %d", considered, skippedInactive, loaded, failed);
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

	// Parse Plugins.txt to honor the user's real Skyrim load order. Active
	// plugins are prefixed '*'. With this order, the last-write-wins semantic
	// on npcSkinOverrides correctly picks the highest-priority override —
	// matching the in-game winner. Without Plugins.txt order we'd pick an
	// arbitrary alphabetical winner and skin/race choices would not match
	// what the game actually loads.
	std::unordered_map<std::string, int> loadOrderRank;
	{
		std::string pluginsTxt = FindSkyrimPluginsTxt(baseDataPath);
		if (!pluginsTxt.empty()) {
			std::ifstream f(pluginsTxt);
			std::string line;
			int rank = 0;
			while (std::getline(f, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line.front() != '*')
					continue;
				std::string name = line.substr(1);
				if (name.empty())
					continue;
				loadOrderRank[ToLower(name)] = rank++;
			}
			wxLogMessage("ScanAllPluginsForNpcSkins: parsed Plugins.txt with %zu active entries", loadOrderRank.size());
		}
		else {
			wxLogMessage("ScanAllPluginsForNpcSkins: no Plugins.txt found; using alphabetical order");
		}
	}

	std::vector<std::string> orderedPlugins;
	orderedPlugins.reserve(pluginFiles.GetCount());
	for (auto& p : pluginFiles)
		orderedPlugins.push_back(p.ToStdString());

	if (!loadOrderRank.empty()) {
		std::stable_sort(orderedPlugins.begin(), orderedPlugins.end(), [&](const std::string& a, const std::string& b) {
			auto baseOf = [](const std::string& s) {
				auto pos = s.find_last_of("/\\");
				return ToLower(pos == std::string::npos ? s : s.substr(pos + 1));
			};
			auto ia = loadOrderRank.find(baseOf(a));
			auto ib = loadOrderRank.find(baseOf(b));
			int ra = (ia == loadOrderRank.end()) ? INT_MAX : ia->second;
			int rb = (ib == loadOrderRank.end()) ? INT_MAX : ib->second;
			return ra < rb;
		});
	}

	size_t pluginsWithNpcs = 0;
	size_t skippedInactive = 0;

	for (auto& pluginPath : orderedPlugins) {
		// Skip inactive plugins: not in Plugins.txt → game would never load them.
		if (!loadOrderRank.empty()) {
			auto sep = pluginPath.find_last_of("/\\");
			std::string base = (sep == std::string::npos) ? pluginPath : pluginPath.substr(sep + 1);
			if (!loadOrderRank.count(ToLower(base))) {
				++skippedInactive;
				continue;
			}
		}

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
		std::string pluginName = wxFileName(wxString::FromUTF8(pluginPath)).GetFullName().ToStdString();
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
			if (npc.editorId == "HousecarlWhiterun") {
				wxLogMessage("ScanAllPluginsForNpcSkins: 'HousecarlWhiterun' override from '%s' wnamRaw=%08X remappedWnam=%08X selfDefined=%d",
							 pluginPath,
							 npc.wnamFormId,
							 info.remappedWnam,
							 info.selfDefined ? 1 : 0);
			}
		}
	}

	wxLogMessage("ScanAllPluginsForNpcSkins: scanned %zu plugins (%zu with NPCs, %zu skipped inactive), %zu NPC skin overrides",
				 orderedPlugins.size(),
				 pluginsWithNpcs,
				 skippedInactive,
				 npcSkinOverrides.size());

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
	auto findDefiningPlugin = [&](const std::string& srcPath, const std::vector<std::string>& srcMasters, uint8_t srcSelfIdx, uint32_t rawFid) -> std::string {
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
					// Defer EnsureDynamicMaster — synthetic master space is
					// scarce and only the NPCs the user actually views need
					// resolution. ResolveLazyNpcSkin performs the load on
					// first consumption.
					info.rawRace = raceRaw;
					info.raceDefiningPlugin = defPath;
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
					info.rawWnamForResolve = wnamRaw;
					info.wnamDefiningPlugin = defPath;
				}
			}

			wxLogMessage("ScanAllPluginsForNpcSkins: '%s' override deferred — rawRace=%08X rawWnam=%08X (resolution on use)", editorId, info.rawRace, info.rawWnamForResolve);
		}
	}
}

// ---------------------------------------------------------------------------
// ResolveLazyNpcSkin — on first NPC use, lazy-load defining plugins and
// remap raw FormIDs. Idempotent: a fully-resolved info is left untouched.
// ---------------------------------------------------------------------------

void LeveledListData::ResolveLazyNpcSkin(NpcSkinInfo& info) {
	if (info.remappedRace == 0 && info.rawRace != 0 && !info.raceDefiningPlugin.empty()) {
		uint16_t synthIdx = EnsureDynamicMaster(info.raceDefiningPlugin, /*masterLoadDepth*/ 0, /*noClobber*/ true);
		if (synthIdx != 0)
			info.remappedRace = EncodeFormId(synthIdx, info.rawRace);
	}
	if (info.remappedWnam == 0 && info.rawWnamForResolve != 0 && !info.wnamDefiningPlugin.empty()) {
		uint16_t synthIdx = EnsureDynamicMaster(info.wnamDefiningPlugin, /*masterLoadDepth*/ 0, /*noClobber*/ true);
		if (synthIdx != 0) {
			info.remappedWnam = EncodeFormId(synthIdx, info.rawWnamForResolve);
			info.selfDefined = false;
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
	FormId npcRaceFid = 0;
	if (!raceEdid.empty()) {
		auto it = raceByEditorId.find(raceEdid);
		if (it != raceByEditorId.end())
			npcRaceFid = it->second;
	}

	// Helper: try the race's default skin ARMO as a last resort.
	auto tryRaceFallback = [&](std::array<std::string, 8>& tex) {
		if (!tex[0].empty())
			return;
		FormId raceWnam = GetRaceSkinArmo(raceEdid);
		if (raceWnam != 0) {
			tex = ResolveSkinTextures(raceWnam, npcRaceFid);
			if (!tex[0].empty())
				wxLogMessage("ResolveSkinTexturesForNPC: NPC '%s' race '%s' skin ARMO %016llX provided textures: %s",
							 npcEditorId,
							 raceEdid,
							 static_cast<unsigned long long>(raceWnam),
							 tex[0]);
		}
	};

	auto ovIt = npcSkinOverrides.find(npcEditorId);
	if (ovIt == npcSkinOverrides.end()) {
		// No WNAM override known for this NPC → go straight to race fallback.
		tryRaceFallback(textures);
		return textures;
	}

	auto& info = ovIt->second;
	ResolveLazyNpcSkin(info);

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
	auto toMainCache = [&](uint32_t fid) -> FormId {
		uint8_t top = (fid >> 24) & 0xFF;
		if (top == selfIdx)
			return 0; // self-defined, use local lookup
		uint32_t base = fid & 0x00FFFFFF;
		auto it = masterRemap.find(top);
		if (it != masterRemap.end())
			return EncodeFormId(it->second, base);
		return 0; // can't remap
	};

	// Step 1: Find WNAM ARMO
	uint32_t wnamFid = info.wnamRaw;
	const std::vector<uint32_t>* espArmaIds = nullptr;
	const std::vector<FormId>* cacheArmaIds = nullptr;

	auto lArmoIt = localArmos.find(wnamFid);
	if (lArmoIt != localArmos.end()) {
		espArmaIds = &armors[lArmoIt->second].armatureIds;
	}
	if (!espArmaIds || espArmaIds->empty()) {
		FormId remapped = toMainCache(wnamFid);
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
	std::vector<lldata::FormId> allArmaIds;
	if (espArmaIds && !espArmaIds->empty())
		allArmaIds.insert(allArmaIds.end(), espArmaIds->begin(), espArmaIds->end());
	if (cacheArmaIds && !cacheArmaIds->empty())
		allArmaIds.insert(allArmaIds.end(), cacheArmaIds->begin(), cacheArmaIds->end());

	// Step 2: Find body ARMA (slot 32) with alternate textures
	for (lldata::FormId armaId : allArmaIds) {
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
