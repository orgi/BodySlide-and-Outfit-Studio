/*
 * ActorAnimation — puts the actor shown by the leveled list previewer into
 * motion: an animation (by default its idle) and FSMP's physics (cloth,
 * hair, body).
 *
 * Animations are resolved the way the game does it: race behavior project
 * (e.g. Actors\Character\DefaultFemale.hkx) → character file → rig
 * (skeleton.hkx) and the character's animations, MT_Idle by default.
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
#include <optional>
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

	// Loads the skeleton NIF and the rig and animations of the behavior
	// project's character. Both paths are data-relative. Plays `preferredClip`
	// (a name from ClipNames(), matched by path, then by file name), MT_Idle
	// otherwise. Logs every failure. Returns false if the skeleton can't be
	// loaded; a missing or broken animation only disables the animation. The
	// previous actor is discarded either way.
	bool LoadActor(const std::string& skeletonNifPath, const std::string& behaviorProjectPath, const fsmp::ActorInfo& info, const std::string& preferredClip, const FileReader& readFile);
	void ClearActor();
	bool IsLoaded() const { return skeleton != nullptr; }
	bool HasClip() const { return clipLoaded; }

	// Animations of the actor's character (paths relative to the behavior project's folder).
	const std::vector<std::string>& ClipNames() const { return clipNames; }
	const std::string& ClipName() const { return clipName; }
	// Switches to another animation of ClipNames(). Logs and returns false if it can't be played.
	bool LoadClip(const std::string& name);
	// Holds the animation at its current time; physics keeps running.
	void SetPaused(bool pause) { paused = pause; }

	// One animation of a sequence played instead of the chosen one.
	struct SequenceStep {
		std::string fileName; // in the character's animations, e.g. "MT_Jump.hkx"
		float blend = 0.0f;	  // seconds it blends in over the pose before it, like the game's transitions
		bool sync = false;	  // starts at the phase the animation before it had reached
	};
	// Plays `steps` instead of the chosen animation, each once, also while
	// paused. Then the last step loops, or with `loopLast` false the chosen
	// animation blends back in over `returnBlend` seconds where it left off.
	// Without steps the chosen animation blends back in right away. Logs file
	// names the character doesn't list; those steps are left out.
	void PlaySequence(const std::vector<SequenceStep>& steps, bool loopLast, float returnBlend);

	// Hands the NIF that the meshes in `shapes` were loaded from to the actor.
	// Skinned shapes are posed; unskinned ones stay static.
	// `item` is the inventory item that equips the model (outfit pieces).
	void RegisterModel(nifly::NifFile& nif, const std::string& nifPath, ModelKind kind, const std::vector<std::pair<nifly::NiShape*, Mesh*>>& shapes, WornCheck worn, const std::optional<fsmp::WornItem>& item = std::nullopt);
	void ForgetModels();
	// The rest pose of registered meshes changed (preset morphs, weight); the
	// actor re-equips its models with it.
	void RestPoseChanged() { sceneDirty = true; }

	// Whether the next Update() with these arguments sets the actor up again
	// (the worn models, the switches or the rest pose changed), which takes a
	// moment.
	bool NeedsSetup(bool animate, bool simulate, const MeshLookup& getMesh);

	// Advances the actor by `dt` seconds: the animation if `animate`, FSMP if
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
		std::optional<fsmp::WornItem> item;
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

	// Reads one of the character's animations and checks it against the rig. Logs every failure.
	bool ReadClip(const std::string& name, hkx::SplineAnimation& out);
	bool Animates() const { return clipLoaded || sequenceActive; }

	// An animation at a point in time; times past the end of a track that
	// doesn't loop hold its last frame. No clip: the rig's reference pose.
	struct Track {
		const hkx::SplineAnimation* clip = nullptr;
		float time = 0.0f;
		bool loop = true;
	};
	const hkx::SplineAnimation* SequenceClip(const std::string& fileName);
	Track CurrentTrack() const;
	// Blends from `from` (which keeps playing) to what plays next; a blend in
	// progress is frozen at its current pose instead.
	void BeginBlend(const Track& from, float duration);
	void Advance(float dt);
	void SamplePose(const Track& track, std::vector<hkx::QsTransform>& out);
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

	FileReader readGameFile;
	std::string projectDir;
	std::string rigPath;
	std::vector<std::string> clipNames;
	std::string clipName;
	bool clipLoaded = false;
	bool paused = false;
	std::string clipPath;
	hkx::Skeleton rig;
	hkx::SplineAnimation clip;
	float clock = 0.0f;
	std::unordered_map<std::string, std::string> clipByFileName; // lower-case file name → name in clipNames
	std::unordered_map<std::string, hkx::SplineAnimation> sequenceCache; // by name in clipNames
	std::vector<std::pair<SequenceStep, const hkx::SplineAnimation*>> sequence;
	bool sequenceActive = false; // a step plays instead of the chosen animation
	size_t sequenceIndex = 0;
	float stepTime = 0.0f;
	bool sequenceLoopLast = true;
	float sequenceReturnBlend = 0.0f;
	Track blendFrom;
	bool blendFromFrozen = false;
	std::vector<hkx::QsTransform> blendFromPose;
	float blendTime = 0.0f;
	float blendDuration = 0.0f; // 0: no blend
	std::vector<hkx::QsTransform> pose; // per rig bone, last posed
	std::vector<hkx::QsTransform> targetPose, fromPose;
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
