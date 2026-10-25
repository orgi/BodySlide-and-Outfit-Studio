/*
 * ActorAnimation — Implementation
 */

#include "ActorAnimation.h"
#include "Mesh.h"

#include <NifFile.hpp>
#include <wx/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

using namespace nifly;

namespace {

std::string ToLower(std::string s) {
	for (auto& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// Havok and plugin paths use backslashes; game data lookups use forward slashes.
std::string NormalizePath(std::string p) {
	std::replace(p.begin(), p.end(), '\\', '/');
	return p;
}

std::string DirectoryOf(const std::string& path) {
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string FileNameOf(const std::string& path) {
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

MatTransform ToMatTransform(const hkx::QsTransform& t) {
	const float x = t.rotation[0], y = t.rotation[1], z = t.rotation[2], w = t.rotation[3];
	MatTransform m;
	m.translation = Vector3(t.translation[0], t.translation[1], t.translation[2]);
	m.rotation = Matrix3(1.0f - 2.0f * (y * y + z * z),
						 2.0f * (x * y - z * w),
						 2.0f * (x * z + y * w),
						 2.0f * (x * y + z * w),
						 1.0f - 2.0f * (x * x + z * z),
						 2.0f * (y * z - x * w),
						 2.0f * (x * z - y * w),
						 2.0f * (y * z + x * w),
						 1.0f - 2.0f * (x * x + y * y));
	// NIF nodes carry a uniform scale only.
	m.scale = t.scale[0];
	return m;
}

// Vertices skinned per work item when shapes are posed in parallel.
constexpr int kSkinChunk = 8192;

// Affine transform with scale folded into the 3x3 part.
struct Affine {
	float m[3][3];
	float t[3];

	explicit Affine(const MatTransform& x) {
		for (int r = 0; r < 3; r++) {
			m[r][0] = x.rotation[r].x * x.scale;
			m[r][1] = x.rotation[r].y * x.scale;
			m[r][2] = x.rotation[r].z * x.scale;
		}
		t[0] = x.translation.x;
		t[1] = x.translation.y;
		t[2] = x.translation.z;
	}
};

void AccumulatePoint(const Affine& a, const Vector3& v, float w, float out[3]) {
	for (int r = 0; r < 3; r++)
		out[r] += w * (a.m[r][0] * v.x + a.m[r][1] * v.y + a.m[r][2] * v.z + a.t[r]);
}

void AccumulateDir(const Affine& a, const Vector3& v, float w, float out[3]) {
	for (int r = 0; r < 3; r++)
		out[r] += w * (a.m[r][0] * v.x + a.m[r][1] * v.y + a.m[r][2] * v.z);
}

Vector3 NormalizedOr(const float v[3], const Vector3& fallback) {
	float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len <= 0.0f)
		return fallback;
	return Vector3(v[0] / len, v[1] / len, v[2] / len);
}

bool LoadPackfile(const ActorAnimation::FileReader& readFile, const std::string& path, hkx::Packfile& pf, std::string& error) {
	std::vector<char> data;
	if (!readFile(path, data)) {
		error = "file not found";
		return false;
	}
	return pf.Load(std::move(data), error);
}

void LogFsmp(fsmp::LogLevel level, const std::string& message) {
	switch (level) {
		case fsmp::LogLevel::Trace:
		case fsmp::LogLevel::Debug: wxLogVerbose("FSMP: %s", message); break;
		case fsmp::LogLevel::Info: wxLogMessage("FSMP: %s", message); break;
		case fsmp::LogLevel::Warning: wxLogWarning("FSMP: %s", message); break;
		case fsmp::LogLevel::Error:
		case fsmp::LogLevel::Critical: wxLogError("FSMP: %s", message); break;
	}
}

} // namespace

void ActorAnimation::InitialisePhysics(const FileReader& readFile, const std::string& configsFile) {
	fsmp::Initialise(readFile, &LogFsmp, configsFile);
}

// ---------------------------------------------------------------------------
// Actor loading
// ---------------------------------------------------------------------------

void ActorAnimation::ClearActor() {
	actor.reset();
	rigToNode.clear();
	attached.clear();
	skeleton.reset();
	skeletonPath.clear();
	actorInfo = {};
	projectDir.clear();
	rigPath.clear();
	clipNames.clear();
	clipName.clear();
	clipLoaded = false;
	clipPath.clear();
	rig = {};
	clip = {};
	clock = 0.0f;
	sceneDirty = true;
}

bool ActorAnimation::LoadActor(const std::string& skeletonNifPath, const std::string& behaviorProjectPath, const fsmp::ActorInfo& info, const std::string& preferredClip, const FileReader& readFile) {
	ClearActor();
	readGameFile = readFile;

	const std::string nifPath = NormalizePath(skeletonNifPath);
	std::vector<char> nifData;
	if (!readFile(nifPath, nifData)) {
		wxLogError("ActorAnimation: Skeleton NIF '%s' not found", nifPath);
		return false;
	}
	NifFile skeletonNif;
	std::istringstream nifStream(std::string(nifData.data(), nifData.size()), std::istringstream::binary);
	if (skeletonNif.Load(nifStream) != 0) {
		wxLogError("ActorAnimation: Failed to load skeleton NIF '%s'", nifPath);
		return false;
	}
	skeleton = fsmp::LoadModel(skeletonNif, nifPath);
	if (!skeleton)
		return false;
	skeletonPath = nifPath;
	actorInfo = info;

	const std::string projectPath = NormalizePath(behaviorProjectPath);
	projectDir = DirectoryOf(projectPath);
	std::string error;

	hkx::Packfile projectFile;
	hkx::ProjectStrings project;
	if (!LoadPackfile(readFile, projectPath, projectFile, error) || !project.Load(projectFile, error)) {
		wxLogError("ActorAnimation: Behavior project '%s': %s", projectPath, error);
		return true;
	}
	if (project.characterFilenames.empty()) {
		wxLogError("ActorAnimation: Behavior project '%s' lists no character file", projectPath);
		return true;
	}

	const std::string characterPath = projectDir + NormalizePath(project.characterFilenames[0]);
	hkx::Packfile characterFile;
	hkx::CharacterStrings character;
	if (!LoadPackfile(readFile, characterPath, characterFile, error) || !character.Load(characterFile, error)) {
		wxLogError("ActorAnimation: Character file '%s': %s", characterPath, error);
		return true;
	}

	rigPath = projectDir + NormalizePath(character.rigName);
	hkx::Packfile rigFile;
	if (!LoadPackfile(readFile, rigPath, rigFile, error) || !rig.Load(rigFile, error)) {
		wxLogError("ActorAnimation: Rig '%s': %s", rigPath, error);
		rig = {};
		return true;
	}

	clipNames = character.animationNames;
	std::sort(clipNames.begin(), clipNames.end(), [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); });

	// The preferred clip by its path in the character file, then by file name; MT_Idle otherwise.
	std::string chosen;
	for (int pass = 0; pass < 3 && chosen.empty(); pass++) {
		for (auto& name : clipNames) {
			const std::string candidate = ToLower(NormalizePath(name));
			if ((pass == 0 && !preferredClip.empty() && candidate == ToLower(NormalizePath(preferredClip))) ||
				(pass == 1 && !preferredClip.empty() && FileNameOf(candidate) == ToLower(FileNameOf(NormalizePath(preferredClip)))) ||
				(pass == 2 && FileNameOf(candidate) == "mt_idle.hkx")) {
				chosen = name;
				break;
			}
		}
	}
	if (!preferredClip.empty() && FileNameOf(ToLower(NormalizePath(chosen))) != FileNameOf(ToLower(NormalizePath(preferredClip))))
		wxLogMessage("ActorAnimation: Character '%s' has no animation '%s'; playing '%s'", character.name, preferredClip, chosen);
	if (chosen.empty()) {
		wxLogError("ActorAnimation: Character '%s' (%s) has no MT_Idle.hkx animation", character.name, characterPath);
		return true;
	}
	LoadClip(chosen);
	return true;
}

bool ActorAnimation::LoadClip(const std::string& name) {
	clipLoaded = false;
	clip = {};
	clock = 0.0f;
	if (rig.boneNames.empty() || !readGameFile)
		return false;

	const std::string path = projectDir + NormalizePath(name);
	std::string error;
	hkx::Packfile clipFile;
	if (!LoadPackfile(readGameFile, path, clipFile, error) || !clip.Load(clipFile, error)) {
		wxLogError("ActorAnimation: Animation '%s': %s", path, error);
		clip = {};
		return false;
	}

	const auto& trackToBone = clip.TrackToBone();
	if (trackToBone.empty() && clip.TrackCount() > rig.boneNames.size()) {
		wxLogError("ActorAnimation: Animation '%s' has %zu tracks but rig '%s' only %zu bones", path, clip.TrackCount(), rigPath, rig.boneNames.size());
		clip = {};
		return false;
	}
	for (int16_t bone : trackToBone) {
		if (bone < 0 || static_cast<size_t>(bone) >= rig.boneNames.size()) {
			wxLogError("ActorAnimation: Animation '%s' binds a track to bone %d, rig '%s' has %zu bones", path, bone, rigPath, rig.boneNames.size());
			clip = {};
			return false;
		}
	}
	if (ToLower(clip.SkeletonName()) != ToLower(rig.name))
		wxLogWarning("ActorAnimation: Animation '%s' was bound to skeleton '%s', rig is '%s'", path, clip.SkeletonName(), rig.name);

	clipName = name;
	clipPath = path;
	clipLoaded = true;
	wxLogMessage("ActorAnimation: Loaded animation '%s' (%.2fs, %zu tracks) on rig '%s' (%zu bones), skeleton '%s'",
				 path,
				 clip.Duration(),
				 clip.TrackCount(),
				 rigPath,
				 rig.boneNames.size(),
				 skeletonPath);
	return true;
}

// ---------------------------------------------------------------------------
// Model registration
// ---------------------------------------------------------------------------

void ActorAnimation::RegisterModel(NifFile& nif, const std::string& nifPath, ModelKind kind, const std::vector<std::pair<NiShape*, Mesh*>>& meshes, WornCheck worn, const std::optional<fsmp::WornItem>& item) {
	auto model = std::make_unique<Model>();
	model->scene = fsmp::LoadModel(nif, nifPath);
	if (!model->scene)
		return;
	model->kind = kind;
	model->worn = std::move(worn);
	model->item = item;

	for (auto& [shape, mesh] : meshes) {
		if (!shape || !mesh)
			continue;
		model->meshes.emplace_back(mesh->shapeName, mesh);
		if (!shape->IsSkinned())
			continue;

		Shape s;
		s.mesh = mesh;
		s.nVerts = mesh->nVerts;
		s.model = model.get();
		s.nifName = shape->name.get();

		std::vector<std::string> boneNames;
		std::vector<int> boneIds;
		nif.GetShapeBoneList(shape, boneNames);
		nif.GetShapeBoneIDList(shape, boneIds);
		if (boneNames.empty() || boneIds.size() != boneNames.size()) {
			wxLogWarning("ActorAnimation: Skinned shape '%s' has no usable bone list; it stays static", mesh->shapeName);
			continue;
		}
		if (boneNames.size() > 0xFFFF) {
			wxLogError("ActorAnimation: Shape '%s' references %zu bones; it stays static", mesh->shapeName, boneNames.size());
			continue;
		}

		s.skinToBone.resize(boneNames.size());
		s.influences.resize(s.nVerts);
		size_t truncated = 0;
		for (uint32_t b = 0; b < boneNames.size(); b++) {
			nif.GetShapeTransformSkinToBone(shape, b, s.skinToBone[b]);

			std::unordered_map<uint16_t, float> weights;
			nif.GetShapeBoneWeights(shape, b, weights);
			for (auto& [vertex, weight] : weights) {
				if (vertex >= s.nVerts || weight <= 0.0f)
					continue;
				// The game renders at most four influences per vertex; keep the strongest.
				Influence& inf = s.influences[vertex];
				int slot = static_cast<int>(std::min_element(inf.weight, inf.weight + 4) - inf.weight);
				if (inf.weight[slot] > 0.0f) {
					truncated++;
					if (inf.weight[slot] >= weight)
						continue;
				}
				inf.bone[slot] = static_cast<uint16_t>(b);
				inf.weight[slot] = weight;
			}
		}
		if (truncated > 0)
			wxLogWarning("ActorAnimation: Shape '%s' has %zu bone influences beyond the four per vertex the game renders; the weakest were dropped", mesh->shapeName, truncated);

		size_t unweighted = 0;
		for (auto& inf : s.influences) {
			if (inf.weight[0] + inf.weight[1] + inf.weight[2] + inf.weight[3] <= 0.0f)
				unweighted++;
		}
		if (unweighted > 0)
			wxLogWarning("ActorAnimation: Shape '%s' has %zu vertices without bone weights; they collapse to the origin like in game", mesh->shapeName, unweighted);

		shapes[mesh->shapeName] = std::move(s);
	}

	models.push_back(std::move(model));
	sceneDirty = true;
}

void ActorAnimation::ForgetModels() {
	actor.reset();
	rigToNode.clear();
	attached.clear();
	shapes.clear();
	models.clear();
	sceneDirty = true;
}

void ActorAnimation::PruneMeshes(const MeshLookup& getMesh) {
	for (auto it = shapes.begin(); it != shapes.end();) {
		Mesh* m = getMesh(it->first);
		if (m != it->second.mesh || !m || m->nVerts != it->second.nVerts) {
			it = shapes.erase(it);
			sceneDirty = true;
		}
		else
			++it;
	}

	// A model is gone once the previewer has dropped all of its meshes.
	for (auto it = models.begin(); it != models.end();) {
		auto& meshes = (*it)->meshes;
		meshes.erase(std::remove_if(meshes.begin(), meshes.end(), [&](const auto& entry) { return getMesh(entry.first) != entry.second; }), meshes.end());
		if (meshes.empty()) {
			if (actor) {
				// The actor still wears the model's clone; take it off with the actor.
				actor.reset();
				rigToNode.clear();
			}
			it = models.erase(it);
			sceneDirty = true;
		}
		else
			++it;
	}
}

std::vector<bool> ActorAnimation::WornModels() const {
	std::vector<bool> worn(models.size());
	for (size_t i = 0; i < models.size(); i++)
		worn[i] = !models[i]->worn || models[i]->worn();
	return worn;
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

void ActorAnimation::BuildScene(const MeshLookup& getMesh, bool simulate) {
	actor.reset();
	rigToNode.clear();
	attached = WornModels();

	fsmp::ActorInfo info = actorInfo;
	for (size_t i = 0; i < models.size(); i++) {
		if (attached[i] && models[i]->kind == ModelKind::FaceGenHead) {
			info.faceGenPath = fsmp::ModelPath(*models[i]->scene);
			break;
		}
	}
	actor = std::make_unique<fsmp::Actor>(*skeleton, info);
	if (!rig.boneNames.empty()) {
		std::vector<fsmp::RigBone> rigBones(rig.boneNames.size());
		for (size_t b = 0; b < rig.boneNames.size(); b++) {
			const auto& pose = rig.referencePose[b];
			rigBones[b].name = rig.boneNames[b];
			std::copy(pose.translation, pose.translation + 3, rigBones[b].translation);
			std::copy(pose.rotation, pose.rotation + 4, rigBones[b].rotation);
			rigBones[b].scale = pose.scale[0];
		}
		actor->SetAnimationSkeleton(rigBones);
	}

	// The worn models carry the meshes' current rest pose, preset morphs included.
	for (auto& [name, s] : shapes) {
		Mesh* m = getMesh(name);
		if (m != s.mesh || !m)
			continue;
		std::vector<Vector3> positions(m->nVerts);
		for (int v = 0; v < m->nVerts; v++)
			positions[v] = Mesh::TransformPosMeshToNif(m->verts[v]);
		fsmp::SetShapeVertices(*s.model->scene, s.nifName, positions);
	}

	// The equipped armors are in the actor's inventory, once per record.
	std::vector<fsmp::WornItem> items;
	for (size_t i = 0; i < models.size(); i++) {
		const auto& item = models[i]->item;
		if (!attached[i] || !item)
			continue;
		if (std::none_of(items.begin(), items.end(), [&](const fsmp::WornItem& known) { return known.formId == item->formId; }))
			items.push_back(*item);
	}
	actor->SetWornItems(items);

	// Worn items first, then the head, the order the engine sets up an actor's 3D in.
	size_t worn = 0;
	for (size_t i = 0; i < models.size(); i++) {
		if (attached[i] && models[i]->kind == ModelKind::Armor) {
			actor->AttachArmor(*models[i]->scene);
			worn++;
		}
	}
	for (size_t i = 0; i < models.size(); i++) {
		if (attached[i] && models[i]->kind == ModelKind::FaceGenHead) {
			actor->AttachHead(*models[i]->scene);
			worn++;
		}
	}

	if (!rig.boneNames.empty()) {
		rigToNode.assign(rig.boneNames.size(), nullptr);
		std::string unmapped;
		for (size_t b = 0; b < rig.boneNames.size(); b++) {
			rigToNode[b] = actor->FindNode(rig.boneNames[b]);
			if (!rigToNode[b])
				unmapped += (unmapped.empty() ? "'" : ", '") + rig.boneNames[b] + "'";
		}
		if (!unmapped.empty())
			wxLogMessage("ActorAnimation: Rig bones without a node in '%s' (not displayed): %s", skeletonPath, unmapped);
	}

	actor->UpdateWorld();
	wxLogMessage("ActorAnimation: Actor set up with %zu of %zu models%s", worn, models.size(), simulate ? ", FSMP physics running" : "");
}

void ActorAnimation::PoseSkeleton() {
	// Bones without a track keep the rig's reference pose, as Havok samples them.
	for (size_t b = 0; b < rig.boneNames.size(); b++) {
		if (rigToNode[b])
			fsmp::Actor::SetLocal(rigToNode[b], ToMatTransform(rig.referencePose[b]));
	}

	clip.Sample(clock, samples);
	const auto& trackToBone = clip.TrackToBone();
	for (size_t t = 0; t < samples.size(); t++) {
		size_t bone = trackToBone.empty() ? t : static_cast<size_t>(trackToBone[t]);
		if (bone < rigToNode.size() && rigToNode[bone])
			fsmp::Actor::SetLocal(rigToNode[bone], ToMatTransform(samples[t]));
	}
}

// ---------------------------------------------------------------------------
// Posing
// ---------------------------------------------------------------------------

void ActorAnimation::PrepareSkin(Shape& s, const std::vector<MatTransform>& worlds) {
	Mesh* m = s.mesh;
	const int n = s.nVerts;

	s.restVerts.assign(m->verts.get(), m->verts.get() + n);
	if (m->norms)
		s.restNorms.assign(m->norms.get(), m->norms.get() + n);
	if (m->tangents)
		s.restTangents.assign(m->tangents.get(), m->tangents.get() + n);
	if (m->bitangents)
		s.restBitangents.assign(m->bitangents.get(), m->bitangents.get() + n);

	// Mesh-space skinning matrix per bone: back to NIF units, skin → bone →
	// actor world, then into the mesh's model space.
	const MatTransform worldToMesh = m->xformModelToMesh.ComposeTransforms(Mesh::xformNifToMesh);
	s.skinning.resize(worlds.size());
	for (size_t b = 0; b < worlds.size(); b++)
		s.skinning[b] = worldToMesh.ComposeTransforms(worlds[b].ComposeTransforms(s.skinToBone[b].ComposeTransforms(Mesh::xformMeshToNif)));
}

void ActorAnimation::SkinRange(Shape& s, int begin, int end) {
	Mesh* m = s.mesh;
	std::vector<Affine> bones(s.skinning.begin(), s.skinning.end());

	for (int v = begin; v < end; v++) {
		const Influence& inf = s.influences[v];
		float p[3] = {0.0f, 0.0f, 0.0f};
		float nn[3] = {0.0f, 0.0f, 0.0f};
		float tn[3] = {0.0f, 0.0f, 0.0f};
		float bn[3] = {0.0f, 0.0f, 0.0f};
		for (int k = 0; k < 4; k++) {
			if (inf.weight[k] <= 0.0f)
				continue;
			const Affine& a = bones[inf.bone[k]];
			AccumulatePoint(a, s.restVerts[v], inf.weight[k], p);
			if (m->norms)
				AccumulateDir(a, s.restNorms[v], inf.weight[k], nn);
			if (m->tangents)
				AccumulateDir(a, s.restTangents[v], inf.weight[k], tn);
			if (m->bitangents)
				AccumulateDir(a, s.restBitangents[v], inf.weight[k], bn);
		}
		m->verts[v] = Vector3(p[0], p[1], p[2]);
		if (m->norms)
			m->norms[v] = NormalizedOr(nn, s.restNorms[v]);
		if (m->tangents)
			m->tangents[v] = NormalizedOr(tn, s.restTangents[v]);
		if (m->bitangents)
			m->bitangents[v] = NormalizedOr(bn, s.restBitangents[v]);
	}
}

void ActorAnimation::FinishSkin(Shape& s) {
	Mesh* m = s.mesh;
	m->QueueUpdate(Mesh::UpdateType::Position);
	if (m->norms)
		m->QueueUpdate(Mesh::UpdateType::Normals);
	if (m->tangents)
		m->QueueUpdate(Mesh::UpdateType::Tangents);
	if (m->bitangents)
		m->QueueUpdate(Mesh::UpdateType::Bitangents);
	s.posed = true;
}

void ActorAnimation::Update(float dt, bool animate, bool simulate, const MeshLookup& getMesh) {
	if (!skeleton)
		return;
	animate = animate && clipLoaded;
	dt = std::max(0.0f, dt);

	PruneMeshes(getMesh);
	if (WornModels() != attached || animate != sceneAnimated || simulate != sceneSimulated)
		sceneDirty = true;
	if (sceneDirty || !actor) {
		BuildScene(getMesh, simulate);
		sceneDirty = false;
		sceneAnimated = animate;
		sceneSimulated = simulate;
		firstFrame = true;
	}

	auto start = std::chrono::steady_clock::now();

	if (animate) {
		if (!paused)
			clock = clip.Duration() > 0.0f ? std::fmod(clock + dt, clip.Duration()) : 0.0f;
		PoseSkeleton();
	}
	actor->UpdateWorld();
	if (simulate)
		actor->Step(dt);

	// The bones are read from the actor first; the vertices are then skinned in parallel.
	std::vector<Shape*> posing;
	size_t posedVerts = 0;
	for (auto& [name, s] : shapes) {
		if (!s.mesh->bVisible)
			continue;
		auto model = std::find_if(models.begin(), models.end(), [&](const auto& entry) { return entry.get() == s.model; });
		if (model == models.end() || !attached[model - models.begin()])
			continue;
		if (!actor->GetSkinBoneWorlds(*s.model->scene, s.nifName, boneWorlds) || boneWorlds.size() != s.skinToBone.size()) {
			if (firstFrame)
				wxLogError("ActorAnimation: Shape '%s' (%s) has no skinned copy on the actor; it stays in its rest pose", name, s.nifName);
			continue;
		}
		PrepareSkin(s, boneWorlds);
		posing.push_back(&s);
		posedVerts += s.nVerts;
	}

	std::vector<std::pair<Shape*, int>> chunks;
	for (Shape* s : posing) {
		for (int begin = 0; begin < s->nVerts; begin += kSkinChunk)
			chunks.emplace_back(s, begin);
	}
	std::atomic<size_t> nextChunk{0};
	auto skinChunks = [&]() {
		for (size_t c = nextChunk++; c < chunks.size(); c = nextChunk++) {
			Shape* s = chunks[c].first;
			SkinRange(*s, chunks[c].second, std::min(s->nVerts, chunks[c].second + kSkinChunk));
		}
	};
	const size_t threadCount = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), chunks.size());
	std::vector<std::thread> workers;
	for (size_t t = 1; t < threadCount; t++)
		workers.emplace_back(skinChunks);
	skinChunks();
	for (auto& worker : workers)
		worker.join();

	for (Shape* s : posing)
		FinishSkin(*s);
	const size_t posedMeshes = posing.size();

	if (firstFrame) {
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		wxLogMessage("ActorAnimation: First frame posed %zu meshes (%zu vertices) in %.2f ms", posedMeshes, posedVerts, ms);
		firstFrame = false;
	}
}

void ActorAnimation::RestoreRestPose() {
	for (auto& [name, s] : shapes) {
		if (!s.posed)
			continue;
		Mesh* m = s.mesh;
		std::copy(s.restVerts.begin(), s.restVerts.end(), m->verts.get());
		if (m->norms)
			std::copy(s.restNorms.begin(), s.restNorms.end(), m->norms.get());
		if (m->tangents)
			std::copy(s.restTangents.begin(), s.restTangents.end(), m->tangents.get());
		if (m->bitangents)
			std::copy(s.restBitangents.begin(), s.restBitangents.end(), m->bitangents.get());
		s.posed = false;
	}
}

void ActorAnimation::QueueRestPoseUpload(const MeshLookup& getMesh) {
	for (auto& [name, s] : shapes) {
		Mesh* m = getMesh(name);
		if (m != s.mesh || !m)
			continue;
		m->QueueUpdate(Mesh::UpdateType::Position);
		if (m->norms)
			m->QueueUpdate(Mesh::UpdateType::Normals);
		if (m->tangents)
			m->QueueUpdate(Mesh::UpdateType::Tangents);
		if (m->bitangents)
			m->QueueUpdate(Mesh::UpdateType::Bitangents);
	}
}
