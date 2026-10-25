/*
 * Behavior of CBPC's stand-ins (include/CbpcGame.h, include/dirent.h, the
 * Win32 functions of include-nowin/Windows.h): what the engine and SKSE do for
 * the functions CBPC calls. Engine functions that can't exist outside of the
 * game (scripts, SKSE messaging, INI settings) log an error when called.
 */

#include "StandIns.h"
#include "dirent.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

std::mutex g_logLock;
cbpc::LogSink g_sink = nullptr;
std::string g_gameFolder;

std::recursive_mutex g_dataLock;
cbpc::GameData g_data;

std::string ToLower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

std::string ToUpper(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return text;
}

std::string Format(const char* fmt, va_list args) {
	char buffer[4096];
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	return buffer;
}

// ---------------------------------------------------------------------------
// Forms built from the records
// ---------------------------------------------------------------------------

struct FormEntry {
	std::unique_ptr<TESForm> form;
	std::vector<BGSKeyword*> keywords;
	std::vector<TESActorBaseData::FactionInfo> factions;
};

std::map<UInt32, std::unique_ptr<FormEntry>> g_forms; // FormID → form, null if the load order has none

template <class T>
T* Lookup(uint32_t formId) {
	return formId ? dynamic_cast<T*>(LookupFormByID(formId)) : nullptr;
}

void AssignKeywords(FormEntry& entry, BGSKeywordForm& keywords, const cbpc::Form& record) {
	for (uint32_t id : record.keywords) {
		if (auto* keyword = Lookup<BGSKeyword>(id))
			entry.keywords.push_back(keyword);
	}
	keywords.Assign(entry.keywords);
}

// Fills `entry` from `record`; references to other forms are looked up after
// the entry is in the table.
std::unique_ptr<TESForm> CreateForm(const cbpc::Form& record) {
	using Type = cbpc::Form::Type;
	std::unique_ptr<TESForm> form;
	switch (record.type) {
		case Type::Keyword: {
			auto keyword = std::make_unique<BGSKeyword>();
			keyword->formType = kFormType_Keyword;
			keyword->keyword = BSString(record.editorId.c_str());
			form = std::move(keyword);
			break;
		}
		case Type::Faction:
			form = std::make_unique<TESFaction>();
			form->formType = kFormType_Faction;
			break;
		case Type::Class:
			form = std::make_unique<TESClass>();
			form->formType = kFormType_Class;
			break;
		case Type::CombatStyle:
			form = std::make_unique<TESCombatStyle>();
			form->formType = kFormType_CombatStyle;
			break;
		case Type::VoiceType:
			form = std::make_unique<BGSVoiceType>();
			form->formType = kFormType_VoiceType;
			break;
		case Type::Race: {
			auto race = std::make_unique<TESRace>();
			race->formType = kFormType_Race;
			race->fullName.name = BSFixedString(record.fullName.c_str());
			race->data.raceFlags = record.flags;
			form = std::move(race);
			break;
		}
		case Type::Npc: {
			auto npc = std::make_unique<TESNPC>();
			npc->formType = kFormType_NPC;
			npc->fullName.name = BSFixedString(record.fullName.c_str());
			npc->actorData.flags = record.flags;
			npc->weight = record.weight;
			form = std::move(npc);
			break;
		}
		case Type::Armor: {
			auto armor = std::make_unique<TESObjectARMO>();
			armor->formType = kFormType_Armor;
			armor->fullName.name = BSFixedString(record.fullName.c_str());
			armor->bipedObject.data.parts = record.bipedSlots;
			form = std::move(armor);
			break;
		}
		case Type::Other:
			form = std::make_unique<TESForm>();
			break;
	}
	form->formID = record.formId;
	return form;
}

void ResolveReferences(FormEntry& entry, const cbpc::Form& record) {
	if (auto* race = dynamic_cast<TESRace*>(entry.form.get()))
		AssignKeywords(entry, race->keyword, record);
	else if (auto* armor = dynamic_cast<TESObjectARMO*>(entry.form.get()))
		AssignKeywords(entry, armor->keyword, record);
	else if (auto* npc = dynamic_cast<TESNPC*>(entry.form.get())) {
		AssignKeywords(entry, npc->keyword, record);
		npc->race = Lookup<TESRace>(record.race);
		npc->actorData.voiceType = Lookup<BGSVoiceType>(record.voiceType);
		npc->combatStyle = Lookup<TESCombatStyle>(record.combatStyle);
		npc->npcClass = Lookup<TESClass>(record.npcClass);
		for (auto& [factionId, rank] : record.factions) {
			if (auto* faction = Lookup<TESFaction>(factionId))
				entry.factions.push_back({faction, rank});
		}
		npc->actorData.factions.Assign(entry.factions);
	}
}

// ---------------------------------------------------------------------------
// References, game addresses
// ---------------------------------------------------------------------------

std::mutex g_referenceLock;
std::unordered_map<UInt32, TESObjectREFR*> g_references;
UInt32 g_nextHandle = 0x100000;

std::mutex g_addressLock;
std::unordered_map<uintptr_t, void*> g_addresses;
std::set<uintptr_t> g_unknownAddresses;
alignas(16) unsigned char g_zeroes[4096] = {};

// ---------------------------------------------------------------------------
// The engine's string cache
// ---------------------------------------------------------------------------

const char* InternString(const char* text) {
	static std::mutex lock;
	static std::unordered_map<std::string, std::string> pool; // lower case → first spelling
	std::lock_guard<std::mutex> guard(lock);
	const std::string value = text ? text : "";
	auto it = pool.find(ToLower(value));
	if (it == pool.end())
		it = pool.emplace(ToLower(value), value).first;
	return it->second.c_str();
}

NiRTTI g_rttiNiObject{"NiObject", nullptr};
NiRTTI g_rttiNiAVObject{"NiAVObject", &g_rttiNiObject};
NiRTTI g_rttiNiNode{"NiNode", &g_rttiNiAVObject};
NiRTTI g_rttiBSGeometry{"BSGeometry", &g_rttiNiAVObject};
NiRTTI g_rttiBSTriShape{"BSTriShape", &g_rttiBSGeometry};

PlayerCharacter* g_player = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// The previewer's side
// ---------------------------------------------------------------------------

void cbpc::SetLogSink(LogSink sink) {
	std::lock_guard<std::mutex> guard(g_logLock);
	g_sink = sink;
}

void cbpc::Report(LogLevel level, const std::string& message) {
	std::lock_guard<std::mutex> guard(g_logLock);
	if (g_sink)
		g_sink(level, message);
}

void cbpc::SetGameFolder(const std::string& folder) {
	g_gameFolder = folder;
	if (!g_gameFolder.empty() && g_gameFolder.back() != '/' && g_gameFolder.back() != '\\')
		g_gameFolder += '/';
}

void cbpc::SetGameData(GameData data) {
	std::lock_guard<std::recursive_mutex> guard(g_dataLock);
	g_data = std::move(data);

	auto* handler = DataHandler::GetSingleton();
	handler->modList.clear();
	for (auto& plugin : g_data.loadOrder) {
		ModInfo info;
		std::snprintf(info.name, sizeof(info.name), "%s", plugin.name.c_str());
		info.fileFlags = plugin.light ? ModInfo::kFileFlags_Light : 0;
		info.modIndex = plugin.index;
		info.lightIndex = plugin.lightIndex;
		handler->modList.push_back(info);
	}
}

void cbpc::RegisterGameAddress(uintptr_t offset, void* address) {
	std::lock_guard<std::mutex> guard(g_addressLock);
	g_addresses[offset] = address;
}

UInt32 cbpc::RegisterReference(TESObjectREFR* reference) {
	std::lock_guard<std::mutex> guard(g_referenceLock);
	const UInt32 handle = g_nextHandle++;
	g_references[handle] = reference;
	return handle;
}

void cbpc::UnregisterReference(UInt32 handle) {
	std::lock_guard<std::mutex> guard(g_referenceLock);
	g_references.erase(handle);
}

void cbpc::AttachChild(NiNode* parent, NiAVObject* child) {
	auto& children = parent->m_children;
	if (children.m_emptyRunStart == children.m_arrayBufLen) {
		const UInt16 size = static_cast<UInt16>(std::max<int>(4, children.m_arrayBufLen * 2));
		auto* data = new NiAVObject*[size]();
		std::copy(children.m_data, children.m_data + children.m_emptyRunStart, data);
		delete[] children.m_data;
		children.m_data = data;
		children.m_arrayBufLen = size;
	}
	children.m_data[children.m_emptyRunStart++] = child;
	children.m_size++;
	child->m_parent = parent;
}

void cbpc::DetachChild(NiNode* parent, NiAVObject* child) {
	auto& children = parent->m_children;
	for (UInt16 i = 0; i < children.m_emptyRunStart; i++) {
		if (children.m_data[i] == child) {
			children.m_data[i] = nullptr;
			children.m_size--;
			child->m_parent = nullptr;
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string cbpc::ResolveGamePath(const std::string& path) {
	std::string normalized = path;
	std::replace(normalized.begin(), normalized.end(), '\\', '/');

	std::string base;
	std::string rest = normalized;
	std::string folder = g_gameFolder;
	std::replace(folder.begin(), folder.end(), '\\', '/');
	if (!folder.empty() && normalized.compare(0, folder.size(), folder) == 0) {
		base = folder;
		rest = normalized.substr(folder.size());
	}
	else if (!normalized.empty() && normalized.front() == '/') {
		base = "/";
		rest = normalized.substr(1);
	}

	fs::path current = base.empty() ? fs::path(".") : fs::path(base);
	size_t start = 0;
	while (start <= rest.size()) {
		size_t end = rest.find('/', start);
		if (end == std::string::npos)
			end = rest.size();
		const std::string component = rest.substr(start, end - start);
		start = end + 1;
		if (component.empty())
			continue;

		std::error_code error;
		fs::path exact = current / component;
		if (component == "." || component == ".." || fs::exists(exact, error)) {
			current = exact;
			continue;
		}
		const std::string wanted = ToLower(component);
		bool found = false;
		for (fs::directory_iterator it(current, error), endIt; !error && it != endIt; it.increment(error)) {
			if (ToLower(it->path().filename().string()) == wanted) {
				current = it->path();
				found = true;
				break;
			}
		}
		if (!found)
			current = exact;
	}
	return base.empty() ? current.lexically_normal().string() : current.string();
}

DIR* opendir(const char* name) {
	const fs::path folder = cbpc::ResolveGamePath(name ? name : "");
	std::error_code error;
	if (!fs::is_directory(folder, error))
		return nullptr;

	auto* dir = new DIR();
	for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
		dir->names.push_back(it->path().filename().string());
	std::sort(dir->names.begin(), dir->names.end(), [](const std::string& a, const std::string& b) { return ToUpper(a) < ToUpper(b); });
	dir->names.insert(dir->names.begin(), {".", ".."});
	return dir;
}

dirent* readdir(DIR* dir) {
	if (!dir || dir->next >= dir->names.size())
		return nullptr;
	std::snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s", dir->names[dir->next++].c_str());
	return &dir->entry;
}

int closedir(DIR* dir) {
	if (!dir)
		return -1;
	delete dir;
	return 0;
}

void* FormHeap_Allocate(std::size_t size) {
	return std::malloc(size);
}

void FormHeap_Free(void* ptr) {
	std::free(ptr);
}

// Declared by common/IPrefix.h for its clock() macro.
namespace cbpc {
std::clock_t Clock() {
	static const auto start = std::chrono::steady_clock::now();
	return static_cast<std::clock_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
} // namespace cbpc

void cbpc::AssertionFailed(const char* file, unsigned long line, const char* desc) {
	Report(LogLevel::Error, std::string("assertion failed in ") + fs::path(file).filename().string() + "(" + std::to_string(line) + "): " + desc);
}

void* cbpc::GameAddress(uintptr_t offset) {
	std::lock_guard<std::mutex> guard(g_addressLock);
	auto it = g_addresses.find(offset);
	if (it != g_addresses.end())
		return it->second;
	if (g_unknownAddresses.insert(offset).second) {
		char text[128];
		std::snprintf(text, sizeof(text), "CBPC reads game address 0x%08llX, which the previewer doesn't emulate; it reads zeroes", static_cast<unsigned long long>(offset));
		Report(LogLevel::Error, text);
	}
	return g_zeroes;
}

// ---------------------------------------------------------------------------
// SKSE
// ---------------------------------------------------------------------------

void _MESSAGE(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const std::string message = Format(fmt, args);
	va_end(args);
	cbpc::Report(cbpc::LogLevel::Info, message);
}

void _ERROR(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const std::string message = Format(fmt, args);
	va_end(args);
	cbpc::Report(cbpc::LogLevel::Error, message);
}

void Console_Print(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const std::string message = Format(fmt, args);
	va_end(args);
	cbpc::Report(cbpc::LogLevel::Info, "console: " + message);
}

std::string GetRuntimeDirectory() {
	return g_gameFolder;
}

BSFixedString::BSFixedString() :
	data(InternString("")) {}

BSFixedString::BSFixedString(const char* buf) :
	data(InternString(buf)) {}

BSFixedString* BSFixedString::Set(const char* buf) {
	data = InternString(buf);
	return this;
}

template <typename T>
NiTArray<T>::NiTArray() :
	m_data(nullptr), m_arrayBufLen(0), m_emptyRunStart(0), m_size(0), m_growSize(0) {}

template <typename T>
NiTArray<T>::~NiTArray() {
	delete[] m_data;
}

template class NiTArray<NiAVObject*>;

NiRTTI* NiObject::GetRTTI() {
	return &g_rttiNiObject;
}

NiRTTI* NiAVObject::GetRTTI() {
	return &g_rttiNiAVObject;
}

NiRTTI* NiNode::GetRTTI() {
	return &g_rttiNiNode;
}

NiRTTI* BSGeometry::GetRTTI() {
	return &g_rttiBSGeometry;
}

NiRTTI* BSTriShape::GetRTTI() {
	return &g_rttiBSTriShape;
}

NiAVObject* NiAVObject::GetObjectByName(const char** name) {
	return m_name && *name == m_name ? this : nullptr;
}

void NiAVObject::UpdateWorldData(ControllerUpdateContext*) {
	m_worldTransform = m_parent ? m_parent->m_worldTransform * m_localTransform : m_localTransform;
}

NiAVObject* NiNode::GetObjectByName(const char** name) {
	if (m_name && *name == m_name)
		return this;
	for (UInt16 i = 0; i < m_children.m_emptyRunStart; i++) {
		NiAVObject* child = m_children.m_data[i];
		if (!child)
			continue;
		if (NiAVObject* found = child->GetObjectByName(name))
			return found;
	}
	return nullptr;
}

bool BGSKeywordForm::HasKeyword(BGSKeyword* keyword) const {
	for (UInt32 i = 0; i < numKeywords; i++) {
		if (keywords[i] == keyword)
			return true;
	}
	return false;
}

void BGSKeywordForm::Assign(std::vector<BGSKeyword*>& list) {
	keywords = list.empty() ? nullptr : list.data();
	numKeywords = static_cast<UInt32>(list.size());
}

const char* TESObjectREFR::GetReferenceName() const {
	if (auto* npc = dynamic_cast<TESNPC*>(baseForm))
		return npc->fullName.GetName();
	if (auto* armor = dynamic_cast<TESObjectARMO*>(baseForm))
		return armor->fullName.GetName();
	return "";
}

bool Actor::VisitFactions(FactionVisitor& visitor) {
	// SKSE's Actor::VisitFactions: the base's factions, then the reference's
	// faction changes, which a reference placed by the previewer has none of.
	TESNPC* npc = DYNAMIC_CAST(baseForm, TESForm, TESNPC);
	if (npc) {
		for (UInt32 i = 0; i < npc->actorData.factions.count; i++) {
			TESActorBaseData::FactionInfo info;
			npc->actorData.factions.GetNthItem(i, info);
			if (visitor.Accept(info.faction, info.rank))
				return true;
		}
	}
	return false;
}

PlayerCharacter** g_thePlayer = &g_player;

bool LookupREFRByHandle(UInt32& refHandle, NiPointer<TESObjectREFR>& refrOut) {
	std::lock_guard<std::mutex> guard(g_referenceLock);
	auto it = g_references.find(refHandle);
	refrOut = it != g_references.end() ? it->second : nullptr;
	return refrOut != nullptr;
}

TESForm* LookupFormByID(UInt32 formID) {
	std::lock_guard<std::recursive_mutex> guard(g_dataLock);
	auto it = g_forms.find(formID);
	if (it != g_forms.end())
		return it->second ? it->second->form.get() : nullptr;

	cbpc::Form record;
	if (!g_data.findForm || !g_data.findForm(formID, record)) {
		g_forms[formID] = nullptr;
		return nullptr;
	}
	auto entry = std::make_unique<FormEntry>();
	entry->form = CreateForm(record);
	FormEntry& added = *entry;
	g_forms[formID] = std::move(entry);
	ResolveReferences(added, record);
	return added.form.get();
}

PlayerCamera* PlayerCamera::GetSingleton() {
	static PlayerCamera camera;
	return &camera;
}

DataHandler* DataHandler::GetSingleton() {
	static DataHandler handler;
	return &handler;
}

const ModInfo* DataHandler::LookupModByName(const char* modName) {
	std::lock_guard<std::recursive_mutex> guard(g_dataLock);
	const std::string wanted = ToLower(modName ? modName : "");
	for (auto& info : modList) {
		if (ToLower(info.name) == wanted)
			return &info;
	}
	return nullptr;
}

bool Setting::GetDouble(double* out) const {
	*out = value;
	return true;
}

Setting* GetINISetting(const char* name) {
	cbpc::Report(cbpc::LogLevel::Error, std::string("CBPC reads the game's INI setting '") + (name ? name : "") + "', which the previewer doesn't have");
	return nullptr;
}

MenuManager* MenuManager::GetSingleton() {
	static MenuManager manager;
	return &manager;
}

bool SKSEMessagingInterface::Dispatch(PluginHandle, UInt32 messageType, void*, UInt32, const char* receiver) {
	char text[160];
	std::snprintf(text, sizeof(text), "CBPC sends SKSE message %08X to '%s'; there are no other SKSE plugins in the previewer", messageType, receiver ? receiver : "");
	cbpc::Report(cbpc::LogLevel::Error, text);
	return false;
}

void VMClassRegistry::RegisterFunction(IFunction* fn) {
	cbpc::Report(cbpc::LogLevel::Error, "CBPC registers a Papyrus function; there are no scripts in the previewer");
	delete fn;
}

TESForm* papyrusActor::GetWornForm(Actor* thisActor, UInt32 mask) {
	// SKSE's MatchBySlot over the inventory's worn items.
	for (TESForm* form : thisActor->wornItems) {
		auto* armor = dynamic_cast<TESObjectARMO*>(form);
		if (armor && (armor->bipedObject.data.parts & mask) != 0)
			return form;
	}
	return nullptr;
}

BGSKeyword* papyrusKeyword::GetKeyword(StaticFunctionTag*, BSFixedString keyword) {
	uint32_t formId = 0;
	{
		std::lock_guard<std::recursive_mutex> guard(g_dataLock);
		if (g_data.findKeyword)
			formId = g_data.findKeyword(keyword.c_str());
	}
	return formId ? dynamic_cast<BGSKeyword*>(LookupFormByID(formId)) : nullptr;
}
