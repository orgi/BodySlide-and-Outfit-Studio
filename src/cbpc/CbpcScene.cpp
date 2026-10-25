/*
 * CbpcScene — the engine around CBPC for the leveled list previewer (see
 * CbpcScene.h). What CBPC's main.cpp and hookD3D.cpp do in game is done here:
 * the plugin load and SKSE's messages in Initialise, the frame hook in Step.
 */

#include "CbpcScene.h"

#define NOMINMAX
#include "common/IPrefix.h"

#include "StandIns.h"

#include "SimObj.h"
#include "config.h"

#include <algorithm>
#include <cstdio>
#include <set>

// scan.cpp: CBPC's actors by FormID and its frame update.
extern concurrency::concurrent_unordered_map<UInt32, SimObj> actors;
void updateActors(bool gamePaused);

namespace cbpc {

namespace {

// The game addresses CBPC reads through RelocPtr, in the code paths its
// project's SKSE version (skse_version.h) chooses (scan.cpp).
constexpr uintptr_t kProcessManagerAddress = 0x0219E570; // AIProcessManager::GetSingleton
constexpr uintptr_t kFrameIntervalAddress = 0x03275608;	 // GetFrameIntervalTimeTick: GameStepTimer_SlowTime

// The player's base form (Skyrim.esm).
constexpr UInt32 kPlayerBaseFormId = 0x00000007;

// The game state CBPC reads besides the actor: the process manager listing the
// loaded actors, the player in an interior cell and its camera, the frame time.
struct Engine {
	AIProcessManager processManager{};
	AIProcessManager* processManagerPointer = &processManager;
	std::vector<UInt32> highActors;
	float frameInterval = 1.0f / 60.0f;

	TESObjectCELL cell;
	PlayerCharacter player;
	TESObjectREFR::LoadedState playerState;
	NiNode playerRoot;

	EventDispatcher<SKSEModCallbackEvent> modEvents;
	EventDispatcher<TESEquipEvent> equipEvents;
};

Engine& GetEngine() {
	static Engine engine;
	return engine;
}

std::string FormIdText(UInt32 formId) {
	char text[16];
	std::snprintf(text, sizeof(text), "%08X", formId);
	return text;
}

NiTransform ToNi(const nifly::MatTransform& transform) {
	NiTransform ret;
	for (int r = 0; r < 3; r++) {
		ret.rot.data[r][0] = transform.rotation[r].x;
		ret.rot.data[r][1] = transform.rotation[r].y;
		ret.rot.data[r][2] = transform.rotation[r].z;
	}
	ret.pos = NiPoint3(transform.translation.x, transform.translation.y, transform.translation.z);
	ret.scale = transform.scale;
	return ret;
}

nifly::MatTransform ToNifly(const NiTransform& transform) {
	nifly::MatTransform ret;
	for (int r = 0; r < 3; r++)
		ret.rotation[r] = nifly::Vector3(transform.rot.data[r][0], transform.rot.data[r][1], transform.rot.data[r][2]);
	ret.translation = nifly::Vector3(transform.pos.x, transform.pos.y, transform.pos.z);
	ret.scale = transform.scale;
	return ret;
}

} // namespace

bool Available() {
	return true;
}

std::string Version() {
	return versionStr;
}

void Initialise(GameData data, LogSink sink, const std::string& gameFolder) {
	SetLogSink(sink);
	SetGameFolder(gameFolder);
	SetGameData(std::move(data));
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;

	Engine& engine = GetEngine();
	RegisterGameAddress(kProcessManagerAddress, &engine.processManagerPointer);
	RegisterGameAddress(kFrameIntervalAddress, &engine.frameInterval);
	engine.processManager.actorsHigh.Assign(engine.highActors);

	// The player stands where the previewer places the actor, in an interior
	// cell. The previewer doesn't show the player: its 3D is a root without
	// bones, and the camera is the player's.
	engine.player.formID = 0x14;
	engine.player.formType = kFormType_Character;
	engine.player.parentCell = &engine.cell;
	auto* playerBase = dynamic_cast<TESNPC*>(LookupFormByID(kPlayerBaseFormId));
	if (playerBase && playerBase->race) {
		engine.player.baseForm = playerBase;
		engine.player.race = playerBase->race;
		engine.player.weight = playerBase->weight;
		engine.playerState.node = &engine.playerRoot;
		engine.player.loadedState = &engine.playerState;
	}
	else
		Report(LogLevel::Error, "the load order has no player record (NPC_ " + FormIdText(kPlayerBaseFormId) + ") with a race; without the player's 3D, CBPC updates no actor");
	*g_thePlayer = &engine.player;
	PlayerCamera::GetSingleton()->cameraNode = &engine.playerRoot;

	// SKSEPlugin_Load (main.cpp)
	LOG_ERR("CBPC Physics SKSE Plugin: %s", versionStr.c_str());
	g_modEventDispatcher = &engine.modEvents;

	// kMessage_InputLoaded (main.cpp: SetupReceptors)
	g_TESEquipEventDispatcher = &engine.equipEvents;
	g_TESEquipEventDispatcher->AddEventSink(&g_TESEquipEventHandler);

	// kMessage_DataLoaded (main.cpp)
	GameLoad();
	std::string runtimeDirectory = GetRuntimeDirectory();
	if (!runtimeDirectory.empty()) {
		std::string configPath = runtimeDirectory + "Data\\SKSE\\Plugins\\";

		auto configList = get_all_files_names_within_folder(configPath.c_str());
		LOG_ERR("Loading Configs");
		loadSystemConfig();
		LOG_ERR("Loaded System config");
		loadMasterConfig(configPath, configList);
		LOG_ERR("Loaded MasterConfig");
		loadConfig(configPath, configList);
		LOG_ERR("Loaded Bounce configs");
		loadBounceInterpolationConfig(configPath, configList);
		LOG_ERR("Loaded Bounce Interpolation config");
		loadCollisionConfig();
		LOG_ERR("Loaded Collision config");
		loadExtraCollisionConfig(configPath, configList);
		LOG_ERR("Loaded Extra Collision configs");
		loadCollisionInterpolationConfig(configPath, configList);
		LOG_ERR("Loaded Collision Interpolation config");
		LoadPlayerCollisionEventConfig();
		LOG_ERR("Loaded Player Collision Event config");
	}
}

// ---------------------------------------------------------------------------
// The actor
// ---------------------------------------------------------------------------

struct Actor::Impl {
	::Actor actor;
	TESObjectREFR::LoadedState loadedState;
	UInt32 handle = 0;

	std::vector<std::unique_ptr<NiAVObject>> objects; // every object the actor's 3D has had
	std::vector<NiAVObject*> scene;					  // the 3D, in the order of SetScene
	NiNode* root = nullptr;

	bool inventorySet = false;
	bool bindingReported = false;
};

Actor::Actor(const ActorInfo& info) :
	impl(std::make_unique<Impl>()) {
	static UInt32 nextReference = 0xFF000800;

	::Actor& actor = impl->actor;
	actor.formID = nextReference++;
	actor.formType = kFormType_Character;
	actor.parentCell = &GetEngine().cell;
	actor.weight = info.weight;
	actor.baseScale = 1.0f;

	auto* npc = dynamic_cast<TESNPC*>(LookupFormByID(info.npcFormId));
	if (!npc) {
		Report(LogLevel::Error, "the load order has no NPC_ " + FormIdText(info.npcFormId) + "; CBPC doesn't see the actor");
		return;
	}
	if (!npc->race) {
		Report(LogLevel::Error, "NPC_ " + FormIdText(info.npcFormId) + " has no race in the load order; CBPC doesn't see the actor");
		return;
	}
	actor.baseForm = npc;
	actor.race = npc->race;

	Engine& engine = GetEngine();
	impl->handle = RegisterReference(&actor);
	engine.highActors.push_back(impl->handle);
	engine.processManager.actorsHigh.Assign(engine.highActors);

	// kMessage_PostLoadGame (main.cpp): CBPC loads its configs again and forgets its actors on the next frame.
	consoleConfigReload.store(true);
	ActorNodeStoppedPhysicsMap.clear();
}

Actor::~Actor() {
	if (!impl->handle)
		return;
	Engine& engine = GetEngine();
	engine.highActors.erase(std::remove(engine.highActors.begin(), engine.highActors.end(), impl->handle), engine.highActors.end());
	engine.processManager.actorsHigh.Assign(engine.highActors);
	UnregisterReference(impl->handle);
}

void Actor::SetScene(const std::vector<SceneNode>& nodes) {
	std::vector<NiAVObject*> next(nodes.size(), nullptr);
	std::set<NiAVObject*> claimed;

	for (size_t i = 0; i < nodes.size(); i++) {
		const SceneNode& node = nodes[i];
		NiNode* parent = nullptr;
		if (node.parent >= 0) {
			parent = static_cast<size_t>(node.parent) < i && next[node.parent] ? next[node.parent]->GetAsNiNode() : nullptr;
			if (!parent) {
				Report(LogLevel::Error, "the actor's object '" + node.name + "' comes before its parent or its parent is no node; CBPC doesn't see it");
				continue;
			}
		}
		else if (i != 0 || !node.isNode) {
			Report(LogLevel::Error, "the actor's 3D has no root node; CBPC doesn't see the actor");
			next.clear();
			break;
		}

		// The object of this name and kind the actor's 3D had in this place.
		const char* name = BSFixedString(node.name.c_str()).data;
		NiAVObject* found = nullptr;
		auto matches = [&](NiAVObject* object) {
			return object && object->m_name == name && (object->GetAsNiNode() != nullptr) == node.isNode && !claimed.count(object);
		};
		if (!parent) {
			if (matches(impl->root))
				found = impl->root;
		}
		else {
			for (UInt16 c = 0; c < parent->m_children.m_emptyRunStart && !found; c++) {
				if (matches(parent->m_children.m_data[c]))
					found = parent->m_children.m_data[c];
			}
		}
		if (!found) {
			std::unique_ptr<NiAVObject> object;
			if (node.isNode)
				object = std::make_unique<NiNode>();
			else
				object = std::make_unique<BSTriShape>();
			object->m_name = name;
			found = object.get();
			impl->objects.push_back(std::move(object));
			if (parent)
				AttachChild(parent, found);
		}
		claimed.insert(found);
		next[i] = found;
	}

	// What the 3D no longer has is detached, like the engine removes it.
	for (NiAVObject* object : impl->scene) {
		if (object && !claimed.count(object) && object->m_parent)
			DetachChild(object->m_parent, object);
	}

	impl->scene = std::move(next);
	impl->root = impl->scene.empty() || !impl->scene[0] ? nullptr : impl->scene[0]->GetAsNiNode();
	impl->loadedState.node = impl->root;
	impl->actor.loadedState = impl->root && impl->actor.baseForm ? &impl->loadedState : nullptr;
}

void Actor::SetWornItems(const std::vector<uint32_t>& armors) {
	std::vector<TESForm*> worn;
	for (uint32_t formId : armors) {
		TESForm* form = LookupFormByID(formId);
		if (!form) {
			Report(LogLevel::Error, "the load order has no ARMO " + FormIdText(formId) + "; it is not in the actor's inventory for CBPC");
			continue;
		}
		if (std::find(worn.begin(), worn.end(), form) == worn.end())
			worn.push_back(form);
	}

	::Actor& actor = impl->actor;
	if (!impl->inventorySet) {
		actor.wornItems = worn;
		impl->inventorySet = true;
		return;
	}

	// The engine equips and unequips one item at a time; each sends an equip event.
	std::vector<TESForm*> removed;
	for (TESForm* form : actor.wornItems) {
		if (std::find(worn.begin(), worn.end(), form) == worn.end())
			removed.push_back(form);
	}
	for (TESForm* form : removed) {
		actor.wornItems.erase(std::find(actor.wornItems.begin(), actor.wornItems.end(), form));
		TESEquipEvent event{&actor, form->formID, 0, 0, false};
		if (g_TESEquipEventDispatcher)
			g_TESEquipEventDispatcher->SendEvent(&event);
	}
	for (TESForm* form : worn) {
		if (std::find(actor.wornItems.begin(), actor.wornItems.end(), form) != actor.wornItems.end())
			continue;
		actor.wornItems.push_back(form);
		TESEquipEvent event{&actor, form->formID, 0, 0, true};
		if (g_TESEquipEventDispatcher)
			g_TESEquipEventDispatcher->SendEvent(&event);
	}
}

void Actor::Step(float seconds, std::vector<NodeTransforms>& transforms) {
	if (transforms.size() != impl->scene.size()) {
		Report(LogLevel::Error, "the frame's transforms don't match the actor's 3D");
		return;
	}
	for (size_t i = 0; i < transforms.size(); i++) {
		if (NiAVObject* object = impl->scene[i]) {
			object->m_localTransform = ToNi(transforms[i].local);
			object->m_worldTransform = ToNi(transforms[i].world);
		}
	}

	// The frame hook (hookD3D.cpp) at the start of the game's main update.
	GetEngine().frameInterval = seconds;
	updateActors(false);

	// What CBPC simulates once it has bound the actor.
	if (!impl->bindingReported) {
		auto bound = actors.find(impl->actor.formID);
		if (bound != actors.end() && bound->second.isBound()) {
			impl->bindingReported = true;
			std::string bones;
			for (auto& group : bound->second.things) {
				for (auto& thing : group.second)
					bones += (bones.empty() ? "" : ", ") + std::string(thing.first);
			}
			std::string colliders;
			for (auto& collider : bound->second.actorColliders)
				colliders += (colliders.empty() ? "" : ", ") + collider.first;
			Report(LogLevel::Info, "the actor's bones with physics: " + (bones.empty() ? std::string("none") : bones) + "; its colliders: " + (colliders.empty() ? std::string("none") : colliders));
		}
	}

	for (size_t i = 0; i < transforms.size(); i++) {
		if (NiAVObject* object = impl->scene[i]) {
			transforms[i].local = ToNifly(object->m_localTransform);
			transforms[i].world = ToNifly(object->m_worldTransform);
		}
	}
}

} // namespace cbpc
