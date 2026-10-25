/*
 * CbpcScene — runs CBPC (its source from lib/CBPC, see cbpc.cmake) on the
 * actor the leveled list previewer shows.
 *
 * CBPC sees the actor the way it sees one in game: a reference next to the
 * player, its 3D the scene graph of the FSMP scene (mirrored into the
 * stand-ins), its base form, race and inventory from the plugins, and every
 * displayed frame runs CBPC's frame update, which the game runs at the start
 * of its main update, before the animation and FSMP.
 *
 * This header is free of CBPC and the stand-ins.
 */

#pragma once

#include <Object3d.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cbpc {

enum class LogLevel { Info, Error };

using LogSink = void (*)(LogLevel level, const std::string& message);

// Whether the previewer is built with CBPC (its source in lib/CBPC).
bool Available();
// CBPC's version, empty without CBPC.
std::string Version();

// A plugin of the game's load order, with the index the engine gives it.
struct Plugin {
	std::string name;
	bool light = false;		 // light plugins share index 0xFE and have a light index
	uint8_t index = 0xFF;	 // 0xFE for light plugins
	uint16_t lightIndex = 0; // light plugins only
};

// A record as the engine has it after loading the plugins. FormIDs are the
// ones the engine assigns for the load order.
struct Form {
	enum class Type { Other, Keyword, Faction, Class, CombatStyle, VoiceType, Race, Npc, Armor };

	Type type = Type::Other;
	uint32_t formId = 0;
	std::string editorId;
	std::string fullName;
	std::vector<uint32_t> keywords; // KWDA: race, NPC, armor
	uint32_t flags = 0;				// RACE: DATA flags; NPC_: ACBS flags
	uint32_t bipedSlots = 0;		// ARMO: bit n is biped slot 30 + n
	uint32_t race = 0;				// NPC_ RNAM
	uint32_t voiceType = 0;			// NPC_ VTCK
	uint32_t combatStyle = 0;		// NPC_ ZNAM
	uint32_t npcClass = 0;			// NPC_ CNAM
	float weight = 0.0f;			// NPC_ NAM7
	std::vector<std::pair<uint32_t, int8_t>> factions; // NPC_ SNAM: faction, rank
};

// The game's data, as far as CBPC reads it.
struct GameData {
	std::vector<Plugin> loadOrder;
	// The record of a FormID; false if the load order has none.
	std::function<bool(uint32_t formId, Form& form)> findForm;
	// The FormID of the keyword with this editor ID (any case); 0 if there is none.
	std::function<uint32_t(const std::string& editorId)> findKeyword;
};

// CBPC's plugin load and SKSE's data loaded message: CBPC loads its configs
// from Data/SKSE/Plugins of `gameFolder` (the folder holding Data) and
// registers its event sinks. Runs once; later calls only replace the data,
// the sink and the folder (the forms CBPC has read stay, like in game, where
// the load order doesn't change while the game runs).
void Initialise(GameData data, LogSink sink, const std::string& gameFolder);

// An object of the actor's 3D. A list of them is in depth-first order,
// parents before their children.
struct SceneNode {
	std::string name;
	int parent = -1;	 // index in the list, -1 for the actor's root
	bool isNode = true;	 // a node (can have children); otherwise geometry
};

struct NodeTransforms {
	nifly::MatTransform local;
	nifly::MatTransform world;
};

struct ActorInfo {
	uint32_t npcFormId = 0; // NPC_ record
	float weight = 0.0f;	// 0 to 100, the weight the actor is shown with

	bool operator==(const ActorInfo& other) const { return npcFormId == other.npcFormId && weight == other.weight; }
	bool operator!=(const ActorInfo& other) const { return !(*this == other); }
};

class Actor {
public:
	// Places the actor next to the player, like a reference the game creates at
	// runtime, and starts CBPC on it the way loading a save game does: CBPC
	// loads its configs again and forgets the actors it knew.
	explicit Actor(const ActorInfo& info);
	// The actor is gone; CBPC no longer finds it among the loaded actors.
	~Actor();

	Actor(const Actor&) = delete;
	Actor& operator=(const Actor&) = delete;

	// The actor's 3D after it was loaded or the engine attached or removed
	// models. Objects CBPC knows keep their identity by name and parent.
	void SetScene(const std::vector<SceneNode>& nodes);
	// The worn armors (FormIDs, inventory order). The first call fills the
	// inventory the actor is loaded with; later changes are equipped or
	// unequipped with the engine's equip events.
	void SetWornItems(const std::vector<uint32_t>& armors);

	// One displayed frame for CBPC: its frame update with the transforms of the
	// objects (order of SetScene) the frame starts with; returns them as CBPC
	// leaves them.
	void Step(float seconds, std::vector<NodeTransforms>& transforms);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace cbpc
