/*
 * Implementation of the engine stand-ins in include/ (RE, REL, SKSE, spdlog,
 * Detours): string pool, scene graph behavior, data file streams, singletons
 * and logging. Engine functions the previewer can't provide (hooks, SKSE
 * interfaces) log an error when FSMP calls them.
 */

#include "StandIns.h"

#include "dhdtOverrideManager.h"

#include <detours/detours.h>

#include <mutex>
#include <unordered_map>

namespace
{
	std::string ToLower(std::string_view a_string)
	{
		std::string lower(a_string);
		for (auto& c : lower)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return lower;
	}

	std::mutex& FunctionLock()
	{
		static std::mutex lock;
		return lock;
	}

	std::unordered_map<std::uint64_t, std::uintptr_t>& Functions()
	{
		static std::unordered_map<std::uint64_t, std::uintptr_t> functions;
		return functions;
	}

	std::atomic<spdlog::level::level_enum> g_level{ spdlog::level::trace };
	std::atomic<SKSE::log::Sink> g_sink{ nullptr };

	fsmp::standin::FileReader& DataFileReader()
	{
		static fsmp::standin::FileReader reader;
		return reader;
	}

	std::unordered_map<RE::FormID, RE::TESForm*>& Forms()
	{
		static std::unordered_map<RE::FormID, RE::TESForm*> forms;
		return forms;
	}
}

// ---------------------------------------------------------------------------
// REL
// ---------------------------------------------------------------------------

std::uintptr_t REL::LookupFunction(std::uint64_t a_seID)
{
	std::lock_guard lock(FunctionLock());
	auto it = Functions().find(a_seID);
	return it != Functions().end() ? it->second : 0;
}

void REL::RegisterFunction(std::uint64_t a_seID, std::uintptr_t a_function)
{
	std::lock_guard lock(FunctionLock());
	Functions()[a_seID] = a_function;
}

void REL::ReportMissingFunction(std::uint64_t a_seID)
{
	SKSE::log::error("Engine function {} is not available outside of the game", a_seID);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void spdlog::set_level(level::level_enum a_level)
{
	g_level = a_level;
}

void spdlog::flush_on(level::level_enum)
{}

spdlog::level::level_enum spdlog::get_level()
{
	return g_level;
}

bool spdlog::should_log(level::level_enum a_level)
{
	return a_level >= g_level && a_level < level::off;
}

void SKSE::log::SetSink(Sink a_sink)
{
	g_sink = a_sink;
}

void SKSE::log::Write(spdlog::level::level_enum a_level, std::string_view a_message)
{
	if (auto sink = g_sink.load())
		sink(a_level, a_message);
}

void SKSE::stl::report_and_fail(std::string_view a_message)
{
	log::critical("{}", a_message);
	std::terminate();
}

// ---------------------------------------------------------------------------
// SKSE interfaces
// ---------------------------------------------------------------------------

bool SKSE::MessagingInterface::Dispatch(std::uint32_t a_messageType, void*, std::uint32_t, const char*) const
{
	log::error("SKSE messaging (message {}) is not available outside of the game", a_messageType);
	return false;
}

SKSE::PluginHandle SKSE::LoadInterface::GetPluginHandle() const
{
	log::error("SKSE plugin handles are not available outside of the game");
	return 0;
}

void* SKSE::LoadInterface::QueryInterface(std::uint32_t a_id) const
{
	log::error("SKSE interface {} is not available outside of the game", a_id);
	return nullptr;
}

bool SKSE::SerializationInterface::OpenRecord(std::uint32_t, std::uint32_t) const
{
	log::error("SKSE serialization is not available outside of the game");
	return false;
}

bool SKSE::SerializationInterface::WriteRecordData(const void*, std::uint32_t) const
{
	log::error("SKSE serialization is not available outside of the game");
	return false;
}

bool SKSE::SerializationInterface::GetNextRecordInfo(std::uint32_t&, std::uint32_t&, std::uint32_t&) const
{
	log::error("SKSE serialization is not available outside of the game");
	return false;
}

std::uint32_t SKSE::SerializationInterface::ReadRecordData(void*, std::uint32_t) const
{
	log::error("SKSE serialization is not available outside of the game");
	return 0;
}

std::uintptr_t SKSE::Trampoline::Unavailable(std::uintptr_t a_src)
{
	log::error("Code patches ({:X}) are not available outside of the game", a_src);
	return 0;
}

SKSE::Trampoline& SKSE::GetTrampoline()
{
	static Trampoline trampoline;
	return trampoline;
}

void SKSE::AllocTrampoline(std::size_t, bool)
{
	log::error("Code patches are not available outside of the game");
}

long DetourAttach(PVOID*, PVOID)
{
	SKSE::log::error("Function hooks are not available outside of the game");
	return 1;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

const char* RE::detail::PoolString(std::string_view a_string)
{
	if (a_string.empty())
		return nullptr;

	static std::mutex lock;
	static std::unordered_map<std::string, std::unique_ptr<char[]>> pool;

	std::lock_guard guard(lock);
	auto& entry = pool[ToLower(a_string)];
	if (!entry) {
		entry = std::make_unique<char[]>(a_string.size() + 1);
		std::memcpy(entry.get(), a_string.data(), a_string.size());
		entry[a_string.size()] = '\0';
	}
	return entry.get();
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

RE::NiObject* RE::NiObject::CreateClone(NiCloningProcess&)
{
	return nullptr;
}

void RE::NiObject::ProcessClone(NiCloningProcess& a_cloning)
{
	auto it = a_cloning.cloneMap.find(this);
	if (it == a_cloning.cloneMap.end())
		return;

	// Skin instances of cloned geometry follow the clone's own bones.
	if (auto* source = dynamic_cast<BSGeometry*>(this)) {
		auto* clone = static_cast<BSGeometry*>(it->second);
		auto& skin = source->GetGeometryRuntimeData().skinInstance;
		if (skin) {
			auto* cloneSkin = new NiSkinInstance();
			cloneSkin->skinData = skin->skinData;
			cloneSkin->skinPartition = skin->skinPartition;
			const auto count = static_cast<std::uint32_t>(skin->boneStore.size());
			cloneSkin->SetBoneCount(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				NiAVObject* bone = skin->bones[i];
				auto boneClone = a_cloning.cloneMap.find(bone);
				cloneSkin->bones[i] = boneClone != a_cloning.cloneMap.end() ? static_cast<NiAVObject*>(boneClone->second) : bone;
				cloneSkin->boneWorldTransforms[i] = cloneSkin->bones[i] ? &cloneSkin->bones[i]->world : nullptr;
			}
			auto rootClone = a_cloning.cloneMap.find(skin->rootParent);
			cloneSkin->rootParent = rootClone != a_cloning.cloneMap.end() ? static_cast<NiAVObject*>(rootClone->second) : skin->rootParent;
			clone->GetGeometryRuntimeData().skinInstance.reset(cloneSkin);
		}
	}

	if (auto* node = AsNode()) {
		for (auto& child : node->GetChildren()) {
			if (child)
				child->ProcessClone(a_cloning);
		}
	}
}

void RE::NiObjectNET::AddExtraData(NiExtraData* a_extra)
{
	if (!a_extra)
		return;
	_extraStore.emplace_back(a_extra);
	_extraList.push_back(a_extra);
	extra = _extraList.data();
	extraDataSize = static_cast<std::uint16_t>(_extraList.size());
}

RE::NiExtraData* RE::NiObjectNET::GetExtraData(const BSFixedString& a_name) const
{
	for (auto* data : _extraList) {
		if (data && data->name == a_name)
			return data;
	}
	return nullptr;
}

void RE::NiObjectNET::CopyMembers(const NiObjectNET& a_source)
{
	name = a_source.name;
	// Clones share their extra data.
	for (auto* data : a_source._extraList)
		AddExtraData(data);
}

RE::NiAVObject* RE::NiAVObject::GetObjectByName(const BSFixedString& a_name)
{
	return name == a_name ? this : nullptr;
}

void RE::NiAVObject::UpdateWorldData(NiUpdateData*)
{
	world = parent ? parent->world * local : local;
}

void RE::NiAVObject::UpdateDownwardPass(NiUpdateData& a_data)
{
	UpdateWorldData(std::addressof(a_data));
}

void RE::NiAVObject::CopyMembers(const NiAVObject& a_source)
{
	NiObjectNET::CopyMembers(a_source);
	local = a_source.local;
	world = a_source.world;
	userData = a_source.userData;
}

RE::NiObject* RE::NiAVObject::CreateClone(NiCloningProcess& a_cloning)
{
	auto* clone = new NiAVObject();
	clone->CopyMembers(*this);
	a_cloning.cloneMap[this] = clone;
	return clone;
}

RE::NiAVObject* RE::NiNode::GetObjectByName(const BSFixedString& a_name)
{
	if (name == a_name)
		return this;
	for (auto& child : children) {
		if (!child)
			continue;
		if (auto* found = child->GetObjectByName(a_name))
			return found;
	}
	return nullptr;
}

void RE::NiNode::UpdateDownwardPass(NiUpdateData& a_data)
{
	UpdateWorldData(std::addressof(a_data));
	for (auto& child : children) {
		if (child)
			child->UpdateDownwardPass(a_data);
	}
}

void RE::NiNode::CopyChildren(const NiNode& a_source, NiCloningProcess& a_cloning)
{
	for (auto& child : a_source.children) {
		if (!child) {
			children.push_back(nullptr);
			continue;
		}
		auto* childClone = static_cast<NiAVObject*>(child->CreateClone(a_cloning));
		if (childClone) {
			childClone->parent = this;
			children.push_back(NiPointer<NiAVObject>(childClone));
		} else
			children.push_back(nullptr);
	}
}

RE::NiObject* RE::NiNode::CreateClone(NiCloningProcess& a_cloning)
{
	auto* clone = new NiNode();
	clone->NiAVObject::CopyMembers(*this);
	a_cloning.cloneMap[this] = clone;
	clone->CopyChildren(*this, a_cloning);
	return clone;
}

RE::NiObject* RE::BSFadeNode::CreateClone(NiCloningProcess& a_cloning)
{
	auto* clone = new BSFadeNode();
	clone->NiAVObject::CopyMembers(*this);
	a_cloning.cloneMap[this] = clone;
	clone->CopyChildren(*this, a_cloning);
	return clone;
}

RE::NiObject* RE::BSFaceGenNiNode::CreateClone(NiCloningProcess& a_cloning)
{
	auto* clone = new BSFaceGenNiNode();
	clone->NiAVObject::CopyMembers(*this);
	a_cloning.cloneMap[this] = clone;
	clone->CopyChildren(*this, a_cloning);
	return clone;
}

void RE::NiNode::AttachChild(NiAVObject* a_child, bool a_firstAvail)
{
	if (!a_child)
		return;

	NiPointer<NiAVObject> hold(a_child);
	if (a_child->parent)
		a_child->parent->DetachChild(a_child);

	a_child->parent = this;
	if (a_firstAvail) {
		for (auto& slot : children) {
			if (!slot) {
				slot = hold;
				return;
			}
		}
	}
	children.push_back(hold);
}

void RE::NiNode::DetachChild(NiAVObject* a_child)
{
	NiPointer<NiAVObject> released;
	DetachChild(a_child, released);
}

void RE::NiNode::DetachChild(NiAVObject* a_child, NiPointer<NiAVObject>& a_childOut)
{
	for (auto& slot : children) {
		if (slot.get() == a_child) {
			// The node keeps the empty slot, like the engine's child array.
			a_childOut = slot;
			a_child->parent = nullptr;
			slot = nullptr;
			return;
		}
	}
	a_childOut = nullptr;
}

void RE::NiNode::DetachChildAt(std::uint32_t a_idx)
{
	if (a_idx >= children.size() || !children[a_idx])
		return;
	children[a_idx]->parent = nullptr;
	children[a_idx] = nullptr;
}

void RE::BSTriShape::CopyMembers(const BSTriShape& a_source)
{
	NiAVObject::CopyMembers(a_source);
	triangleCount = a_source.triangleCount;
	vertexCount = a_source.vertexCount;
	// The skin instance is cloned in ProcessClone, once the bones are cloned too.
	geometryRuntimeData.skinInstance = a_source.geometryRuntimeData.skinInstance;
}

RE::NiObject* RE::BSTriShape::CreateClone(NiCloningProcess& a_cloning)
{
	auto* clone = new BSTriShape();
	clone->CopyMembers(*this);
	a_cloning.cloneMap[this] = clone;
	return clone;
}

RE::NiObject* RE::BSDynamicTriShape::CreateClone(NiCloningProcess& a_cloning)
{
	auto* clone = new BSDynamicTriShape();
	clone->CopyMembers(*this);
	clone->dynamicStore = dynamicStore;
	clone->dynamicRuntimeData.dynamicData = clone->dynamicStore.data();
	a_cloning.cloneMap[this] = clone;
	return clone;
}

bool RE::NiCamera::NodeInFrustum(NiAVObject*) const
{
	// The previewer's camera always looks at the actor.
	return true;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

void fsmp::standin::SetFileReader(FileReader a_reader)
{
	DataFileReader() = std::move(a_reader);
}

bool fsmp::standin::ReadDataFile(const std::string& a_path, std::vector<char>& a_data)
{
	auto& reader = DataFileReader();
	if (!reader) {
		SKSE::log::error("No game data reader is set; '{}' can't be read", a_path);
		return false;
	}

	std::string path = a_path;
	std::replace(path.begin(), path.end(), '\\', '/');
	while (!path.empty() && path.front() == '/')
		path.erase(path.begin());
	// Paths are relative to the Data folder; a leading "Data/" names that folder.
	if (path.size() > 5 && ToLower(path.substr(0, 5)) == "data/") {
		SKSE::log::warn("Game data path '{}' starts with the Data folder; it is read relative to it", a_path);
		path.erase(0, 5);
	}
	return reader(path, a_data);
}

RE::BSResourceNiBinaryStream::BSResourceNiBinaryStream(const char* a_path) :
	_path(a_path ? a_path : "")
{
	if (a_path && fsmp::standin::ReadDataFile(a_path, _bytes)) {
		_stream.totalSize = static_cast<std::uint32_t>(_bytes.size());
		stream = &_stream;
		_good = true;
	} else
		SKSE::log::error("Game data file '{}' not found", a_path ? a_path : "");
}

void RE::BSResourceNiBinaryStream::read(void* a_buffer, std::uint32_t a_bytes)
{
	const auto available = std::min<std::size_t>(a_bytes, _bytes.size() - _position);
	std::memcpy(a_buffer, _bytes.data() + _position, available);
	_position += available;
}

// ---------------------------------------------------------------------------
// Forms
// ---------------------------------------------------------------------------

RE::TESForm* RE::TESForm::LookupByID(FormID a_formID)
{
	auto it = Forms().find(a_formID);
	return it != Forms().end() ? it->second : nullptr;
}

void RE::RegisterForm(TESForm* a_form)
{
	if (a_form)
		Forms()[a_form->formID] = a_form;
}

void RE::UnregisterForm(TESForm* a_form)
{
	if (!a_form)
		return;
	auto it = Forms().find(a_form->formID);
	if (it != Forms().end() && it->second == a_form)
		Forms().erase(it);
}

RE::NiAVObject* RE::TESObjectREFR::GetNodeByName(const BSFixedString& a_name) const
{
	return thirdPerson3D ? thirdPerson3D->GetObjectByName(a_name) : nullptr;
}

RE::PlayerCharacter* RE::PlayerCharacter::GetSingleton()
{
	// The previewer shows an NPC. The player exists, without 3D.
	static PlayerCharacter player;
	return &player;
}

// ---------------------------------------------------------------------------
// Singletons
// ---------------------------------------------------------------------------

RE::PlayerCamera* RE::PlayerCamera::GetSingleton()
{
	static PlayerCamera camera;
	return &camera;
}

RE::Main* RE::Main::GetSingleton()
{
	static Main main;
	return &main;
}

RE::NiCamera* RE::Main::WorldRootCamera()
{
	static NiPointer<NiCamera> camera(new NiCamera());
	return camera.get();
}

RE::UI* RE::UI::GetSingleton()
{
	static UI ui;
	return &ui;
}

RE::BSTimer* RE::BSTimer::GetSingleton()
{
	static BSTimer timer;
	return &timer;
}

float RE::GetSecondsSinceLastFrame()
{
	return BSTimer::GetSingleton()->delta;
}

RE::ConsoleLog* RE::ConsoleLog::GetSingleton()
{
	static ConsoleLog console;
	return &console;
}

void RE::ConsoleLog::Print(const char* a_fmt, ...)
{
	if (!a_fmt)
		return;
	va_list args;
	va_start(args, a_fmt);
	va_list copy;
	va_copy(copy, args);
	const int length = std::vsnprintf(nullptr, 0, a_fmt, copy);
	va_end(copy);
	std::string message(length > 0 ? static_cast<std::size_t>(length) : 0, '\0');
	if (length > 0)
		std::vsnprintf(message.data(), message.size() + 1, a_fmt, args);
	va_end(args);
	SKSE::log::info("Console: {}", message);
}

// ---------------------------------------------------------------------------
// FSMP parts that depend on game services: Dynamic HDT overrides are set by
// Papyrus scripts and save games (dhdtOverrideManager.cpp,
// dhdtPapyrusFunctions.cpp), neither exists outside of the game.
// ---------------------------------------------------------------------------

bool g_hasPapyrusExtension = false;

hdt::Override::OverrideManager* hdt::Override::OverrideManager::GetSingleton()
{
	static OverrideManager overrideManager;
	return &overrideManager;
}

std::string hdt::Override::OverrideManager::checkOverride(uint32_t a_actorFormID, std::string a_oldFilePath)
{
	auto actor = m_ActorPhysicsFileSwapList.find(a_actorFormID);
	if (actor != m_ActorPhysicsFileSwapList.end()) {
		auto file = actor->second.find(a_oldFilePath);
		if (file != actor->second.end())
			return file->second;
	}
	return std::string();
}

std::stringstream hdt::Override::OverrideManager::Serialize()
{
	return std::stringstream();
}

void hdt::Override::OverrideManager::Deserialize(std::stringstream&)
{}
