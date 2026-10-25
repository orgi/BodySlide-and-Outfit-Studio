/*
 * FsmpScene — runs FSMP (lib/hdtSMP64, compiled unmodified) on the actor the
 * leveled list previewer shows.
 *
 * FSMP sees the actor the way it sees one in game: the race skeleton NIF is
 * the actor's 3D, worn models go through the armor attach events, the FaceGen
 * head through the head skinning events, and every displayed frame sends one
 * frame event and one frame sync event. The engine objects FSMP works on are
 * the stand-ins in include/, filled from nifly.
 *
 * This header is free of FSMP, Bullet and the stand-ins so callers can stay on
 * the project's C++ standard.
 */

#pragma once

#include <Object3d.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nifly {
class NifFile;
} // namespace nifly

namespace fsmp {

enum class LogLevel { Trace, Debug, Info, Warning, Error, Critical };

using LogSink = void (*)(LogLevel level, const std::string& message);

// Reads a game data file by its path relative to the Data folder ("meshes/...",
// forward slashes), loose files first, then archives. False if it doesn't exist.
using FileReader = std::function<bool(const std::string& relativePath, std::vector<char>& data)>;

// FSMP's plugin load: reads FSMP's configs.xml and registers FSMP's event
// handlers. `configsFile` is the loose file
// Data/SKSE/Plugins/hdtSkinnedMeshConfigs/configs.xml (empty if there is none;
// FSMP's defaults apply then, as in game). Runs once; later calls only replace
// the reader and the sink.
void Initialise(FileReader readDataFile, LogSink sink, const std::string& configsFile);

// A NIF loaded as engine objects, like the engine's model cache keeps it.
// Attaching it to an actor attaches a clone.
class Model;
std::shared_ptr<Model> LoadModel(nifly::NifFile& nif, const std::string& path);
const std::string& ModelPath(const Model& model);

// Replaces the vertex positions (NIF coordinates) of the model's shape, for
// example with preset morphs applied. Affects later attaches only.
bool SetShapeVertices(Model& model, const std::string& shapeName, const std::vector<nifly::Vector3>& positions);

// The actor's records, as far as FSMP reads them.
struct ActorInfo {
	uint32_t npcFormId = 0;		 // NPC_ record, 0 without one
	std::string name;			 // NPC full name
	std::string raceSkeleton[2]; // RACE ANAM (male, female) as written in the record
	std::string faceGenPath;	 // data-relative FaceGen NIF of the NPC, empty without one
};

// An item the actor wears, as far as FSMP reads it from the actor's inventory.
struct WornItem {
	uint32_t formId = 0;	 // ARMO record
	std::string name;		 // full name
	uint32_t bipedSlots = 0; // bit n: biped slot 30 + n
};

// A bone of the actor's Havok rig with its reference pose.
struct RigBone {
	std::string name;
	float translation[3] = {0.0f, 0.0f, 0.0f};
	float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
	float scale = 1.0f;
};

// A node in the actor's scene graph.
class Node;

class Actor {
public:
	// Places the actor (3D: a clone of `skeleton`) in the scene.
	Actor(const Model& skeleton, const ActorInfo& info);
	// Removes the actor from the scene; FSMP lets go of it like of an unloaded actor.
	~Actor();

	Actor(const Actor&) = delete;
	Actor& operator=(const Actor&) = delete;

	// The rig of the actor's behavior graph; FSMP builds physics systems in its reference pose.
	void SetAnimationSkeleton(const std::vector<RigBone>& bones);
	// The equipped items in the actor's inventory. Set before attaching models.
	void SetWornItems(const std::vector<WornItem>& items);

	// The engine equipping a model: armor attach event, the model's skin bound
	// to the skeleton by bone name, attach event again.
	void AttachArmor(const Model& model);
	// The engine loading the head from the NPC's FaceGen NIF: head skinning events.
	void AttachHead(const Model& faceGen);

	// Case-insensitive, like the engine's name lookup. Null if there is no such node.
	Node* FindNode(const std::string& name) const;
	static void SetLocal(Node* node, const nifly::MatTransform& local);

	// Updates the world transforms of the whole actor from the local ones.
	void UpdateWorld();
	// One displayed frame for FSMP.
	void Step(float seconds);

	// World transforms of the bones the attached clone of `shapeName` (from
	// `model`) is skinned to, in the order of the shape's NIF bone list.
	bool GetSkinBoneWorlds(const Model& model, const std::string& shapeName, std::vector<nifly::MatTransform>& worlds) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace fsmp
