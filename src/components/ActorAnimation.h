/*
 * ActorAnimation — puts the actor shown by the leveled list previewer into
 * motion: its default idle and FSMP's physics (cloth, hair, body).
 *
 * The idle is resolved the way the game does it: race behavior project
 * (e.g. Actors\Character\DefaultFemale.hkx) → character file → rig
 * (skeleton.hkx) and the character's MT_Idle clip.
 *
 * The actor lives in an FSMP scene (src/fsmp/FsmpScene.h): the race skeleton
 * NIF as its 3D, the worn models attached like the engine equips them and the
 * FaceGen head attached like the engine loads it. Each frame the clip poses
 * the skeleton, FSMP simulates, and every registered mesh is skinned on the
 * CPU from the bones of its attached copy.
 *
 * Posed vertices only live in a mesh between Update() and RestoreRestPose();
 * outside of that window every mesh holds its rest pose, so morphs and other
 * edits keep operating on unposed data.
 */

#pragma once

#include "../files/HkxFile.h"
#include "../fsmp/FsmpScene.h"

#include <Object3d.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nifly {
class NifFile;
class NiShape;
} // namespace nifly

class Mesh;

class ActorAnimation {
public:
	// Reads a game data file by data-relative path ("meshes/..."), loose
	// files first, then archives. Returns false if the file doesn't exist.
	using FileReader = std::function<bool(const std::string& relativePath, std::vector<char>& data)>;
	using MeshLookup = std::function<Mesh*(const std::string& name)>;
	// Whether the actor wears a registered model right now.
	using WornCheck = std::function<bool()>;

	enum class ModelKind {
		Armor,	   // equipped like a worn armor addon (body, hands, feet, outfit pieces)
		FaceGenHead // the NPC's FaceGen NIF
	};

	// FSMP's plugin load (once per process). `configsFile` is the loose
	// SKSE/Plugins/hdtSkinnedMeshConfigs/configs.xml, empty if there is none.
	static void InitialisePhysics(const FileReader& readFile, const std::string& configsFile);

	// Loads the skeleton NIF and resolves the idle clip through the behavior
	// project. Both paths are data-relative. Logs every failure. Returns false
	// if the skeleton can't be loaded; a missing or broken clip only disables
	// the idle. The previous actor is discarded either way.
	bool LoadActor(const std::string& skeletonNifPath, const std::string& behaviorProjectPath, const fsmp::ActorInfo& info, const FileReader& readFile);
	void ClearActor();
	bool IsLoaded() const { return skeleton != nullptr; }
	bool HasClip() const { return clipLoaded; }

	// Hands the NIF that the meshes in `shapes` were loaded from to the actor.
	// Skinned shapes are posed; unskinned ones stay static.
	void RegisterModel(nifly::NifFile& nif, const std::string& nifPath, ModelKind kind, const std::vector<std::pair<nifly::NiShape*, Mesh*>>& shapes, WornCheck worn);
	void ForgetModels();
	// The rest pose of registered meshes changed (preset morphs, weight); the
	// actor re-equips its models with it.
	void RestPoseChanged() { sceneDirty = true; }

	// Advances the actor by `dt` seconds: the idle if `animate`, FSMP if
	// `simulate`. Writes posed vertices, normals and tangents into every
	// visible registered mesh that `getMesh` still returns and queues them for
	// upload; render, then call RestoreRestPose().
	void Update(float dt, bool animate, bool simulate, const MeshLookup& getMesh);
	void RestoreRestPose();
	// Queues the rest pose of every registered mesh for upload to the GPU.
	void QueueRestPoseUpload(const MeshLookup& getMesh);

private:
	struct Influence {
		uint16_t bone[4] = {0, 0, 0, 0};
		float weight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	};

	struct Model {
		std::shared_ptr<fsmp::Model> scene;
		ModelKind kind = ModelKind::Armor;
		WornCheck worn;
		std::vector<std::pair<std::string, Mesh*>> meshes; // every mesh loaded from the model
	};

	struct Shape {
		Mesh* mesh = nullptr;
		int nVerts = 0;
		Model* model = nullptr;
		std::string nifName;
		std::vector<nifly::MatTransform> skinToBone;
		std::vector<Influence> influences;

		bool posed = false;
		std::vector<nifly::Vector3> restVerts, restNorms, restTangents, restBitangents;
		std::vector<nifly::MatTransform> skinning; // per bone: rest vertex → posed vertex, mesh space
	};

	void PruneMeshes(const MeshLookup& getMesh);
	std::vector<bool> WornModels() const;
	void BuildScene(const MeshLookup& getMesh, bool simulate);
	void PoseSkeleton();
	// Skinning of one shape: PrepareSkin, SkinRange over all vertices (any
	// thread, disjoint ranges), FinishSkin.
	void PrepareSkin(Shape& shape, const std::vector<nifly::MatTransform>& boneWorlds);
	void SkinRange(Shape& shape, int begin, int end);
	void FinishSkin(Shape& shape);

	std::shared_ptr<fsmp::Model> skeleton;
	fsmp::ActorInfo actorInfo;
	std::string skeletonPath;

	bool clipLoaded = false;
	std::string clipPath;
	hkx::Skeleton rig;
	hkx::SplineAnimation clip;
	float clock = 0.0f;
	std::vector<hkx::QsTransform> samples;

	std::vector<std::unique_ptr<Model>> models;
	std::unordered_map<std::string, Shape> shapes; // mesh name → skinning

	std::unique_ptr<fsmp::Actor> actor;
	std::vector<fsmp::Node*> rigToNode; // rig bone index → actor node (null: none)
	std::vector<bool> attached;			// models attached to `actor`
	bool sceneDirty = true;
	bool sceneAnimated = false;
	bool sceneSimulated = false;
	bool firstFrame = true;
	std::vector<nifly::MatTransform> boneWorlds;
};
