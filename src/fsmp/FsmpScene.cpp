/*
 * FsmpScene — Implementation. Everything here stands in for the game engine
 * around FSMP: loading NIFs into engine objects, equipping, the FaceGen head,
 * the frame loop and FSMP's plugin load. FSMP itself runs unmodified.
 */

#include "FsmpScene.h"
#include "StandIns.h"

#include "ActorManager.h"
#include "Events.h"
#include "config.h"
#include "hdtSkyrimPhysicsWorld.h"

#include <NifFile.hpp>

#include <filesystem>
#include <functional>
#include <random>
#include <sstream>
#include <system_error>

namespace fsmp {

class Model {
public:
	std::string path;
	RE::NiPointer<RE::NiAVObject> root;
};

namespace {

constexpr const char* kFaceGenNodeName = "BSFaceGenNiNodeSkinned";

// Address library IDs (SE) of the engine functions ActorManager.cpp calls.
constexpr std::uint64_t kActorCalculateLOS = 36754;
constexpr std::uint64_t kTESNPCGetFaceGeomPath = 24222;
constexpr std::uint64_t kNiStreamConstructor = 68971;
constexpr std::uint64_t kNiStreamDestructor = 68972;

// FSMP lays the per-vertex data of a skin partition out like the engine:
// position (three floats and the bitangent's x) followed by the bone data.
constexpr std::uint32_t kVertexPositionSize = 16;
constexpr std::uint32_t kVertexSize = kVertexPositionSize + sizeof(hdt::SkyrimSystem::BoneData);

LogSink g_sink = nullptr;
std::unordered_map<const RE::TESNPC*, std::string> g_faceGenPaths;

std::string ToLower(std::string_view a_string) {
	std::string lower(a_string);
	for (auto& c : lower)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return lower;
}

void ForwardLog(spdlog::level::level_enum a_level, std::string_view a_message) {
	if (!g_sink)
		return;
	LogLevel level = LogLevel::Info;
	switch (a_level) {
		case spdlog::level::trace: level = LogLevel::Trace; break;
		case spdlog::level::debug: level = LogLevel::Debug; break;
		case spdlog::level::info: level = LogLevel::Info; break;
		case spdlog::level::warn: level = LogLevel::Warning; break;
		case spdlog::level::err: level = LogLevel::Error; break;
		default: level = LogLevel::Critical; break;
	}
	g_sink(level, std::string(a_message));
}

void Report(LogLevel a_level, const std::string& a_message) {
	if (g_sink)
		g_sink(a_level, a_message);
}

RE::NiTransform ToNi(const nifly::MatTransform& a_transform) {
	RE::NiTransform ret;
	for (int r = 0; r < 3; r++) {
		ret.rotate.entry[r][0] = a_transform.rotation[r].x;
		ret.rotate.entry[r][1] = a_transform.rotation[r].y;
		ret.rotate.entry[r][2] = a_transform.rotation[r].z;
	}
	ret.translate = {a_transform.translation.x, a_transform.translation.y, a_transform.translation.z};
	ret.scale = a_transform.scale;
	return ret;
}

nifly::MatTransform ToNifly(const RE::NiTransform& a_transform) {
	nifly::MatTransform ret;
	for (int r = 0; r < 3; r++)
		ret.rotation[r] = nifly::Vector3(a_transform.rotate.entry[r][0], a_transform.rotate.entry[r][1], a_transform.rotate.entry[r][2]);
	ret.translation = nifly::Vector3(a_transform.translate.x, a_transform.translate.y, a_transform.translate.z);
	ret.scale = a_transform.scale;
	return ret;
}

// IEEE half precision with round-to-nearest-even, the format of the engine's bone weights.
std::uint16_t ToHalf(float a_value) {
	std::uint32_t bits;
	std::memcpy(&bits, &a_value, sizeof(bits));
	const std::uint32_t sign = (bits >> 16) & 0x8000u;
	std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
	std::uint32_t mantissa = bits & 0x7FFFFFu;

	if (((bits >> 23) & 0xFFu) == 0xFFu)
		return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
	if (exponent >= 31)
		return static_cast<std::uint16_t>(sign | 0x7C00u);
	if (exponent <= 0) {
		if (exponent < -10)
			return static_cast<std::uint16_t>(sign);
		mantissa |= 0x800000u;
		const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
		std::uint32_t half = mantissa >> shift;
		const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
		const std::uint32_t halfway = 1u << (shift - 1u);
		if (rest > halfway || (rest == halfway && (half & 1u)))
			half++;
		return static_cast<std::uint16_t>(sign | half);
	}
	std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
	const std::uint32_t rest = mantissa & 0x1FFFu;
	if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
		half++;
	return static_cast<std::uint16_t>(sign | half);
}

// ---------------------------------------------------------------------------
// NIF → engine objects
// ---------------------------------------------------------------------------

class Converter {
public:
	Converter(nifly::NifFile& a_nif, const std::string& a_path) :
		nif(a_nif), path(a_path) {}

	RE::NiAVObject* Convert(nifly::NiAVObject* a_source) {
		RE::NiAVObject* object = Create(a_source);
		object->name = a_source->name.get();
		object->local = ToNi(a_source->GetTransformToParent());
		converted[a_source] = object;

		for (auto& ref : a_source->extraDataRefs) {
			auto* extra = nif.GetHeader().GetBlock<nifly::NiExtraData>(ref);
			if (!extra)
				continue;
			if (auto* stringExtra = dynamic_cast<nifly::NiStringExtraData*>(extra)) {
				auto* data = new RE::NiStringExtraData();
				data->name = stringExtra->name.get();
				data->SetValue(stringExtra->stringData.get());
				object->AddExtraData(data);
			}
			else {
				auto* data = new RE::NiExtraData();
				data->name = extra->name.get();
				object->AddExtraData(data);
			}
		}

		if (auto* node = dynamic_cast<nifly::NiNode*>(a_source)) {
			auto* target = object->AsNode();
			for (auto& ref : node->childRefs) {
				auto* child = nif.GetHeader().GetBlock<nifly::NiAVObject>(ref);
				if (child)
					target->AttachChild(Convert(child));
			}
		}
		return object;
	}

	// Skin instances reference nodes anywhere in the file, so they are linked
	// once every node exists, like NiStream links objects after loading them.
	void LinkSkins() {
		for (auto& [source, geometry] : skinned)
			LinkSkin(source, geometry);
	}

private:
	RE::NiAVObject* Create(nifly::NiAVObject* a_source) {
		if (dynamic_cast<nifly::BSFadeNode*>(a_source))
			return new RE::BSFadeNode();
		if (dynamic_cast<nifly::NiNode*>(a_source)) {
			// The engine turns the FaceGen head's skinned node into its head node type.
			if (ToLower(a_source->name.get()) == ToLower(kFaceGenNodeName))
				return new RE::BSFaceGenNiNode();
			return new RE::NiNode();
		}
		if (auto* dynamicShape = dynamic_cast<nifly::BSDynamicTriShape*>(a_source)) {
			auto* shape = new RE::BSDynamicTriShape();
			shape->dynamicStore.resize(dynamicShape->dynamicData.size() * 4);
			for (size_t v = 0; v < dynamicShape->dynamicData.size(); v++) {
				shape->dynamicStore[v * 4 + 0] = dynamicShape->dynamicData[v].x;
				shape->dynamicStore[v * 4 + 1] = dynamicShape->dynamicData[v].y;
				shape->dynamicStore[v * 4 + 2] = dynamicShape->dynamicData[v].z;
				shape->dynamicStore[v * 4 + 3] = dynamicShape->dynamicData[v].w;
			}
			shape->GetDynamicTrishapeRuntimeData().dynamicData = shape->dynamicStore.data();
			FillTriShape(dynamicShape, shape);
			return shape;
		}
		if (auto* triShape = dynamic_cast<nifly::BSTriShape*>(a_source)) {
			auto* shape = new RE::BSTriShape();
			FillTriShape(triShape, shape);
			return shape;
		}
		if (dynamic_cast<nifly::NiGeometry*>(a_source))
			return new RE::NiGeometry();
		return new RE::NiAVObject();
	}

	void FillTriShape(nifly::BSTriShape* a_source, RE::BSTriShape* a_shape) {
		a_shape->vertexCount = a_source->GetNumVertices();
		a_shape->triangleCount = static_cast<std::uint16_t>(a_source->GetNumTriangles());
		if (a_source->IsSkinned() || !a_source->SkinInstanceRef()->IsEmpty())
			skinned.emplace_back(a_source, a_shape);
	}

	void LinkSkin(nifly::BSTriShape* a_source, RE::BSGeometry* a_geometry) {
		const std::string shapeName = a_source->name.get();
		auto* skinInstance = nif.GetHeader().GetBlock<nifly::NiSkinInstance>(a_source->SkinInstanceRef());
		if (!skinInstance) {
			Report(LogLevel::Error, "'" + path + "': skinned shape '" + shapeName + "' has no skin instance");
			return;
		}
		auto* skinData = nif.GetHeader().GetBlock<nifly::NiSkinData>(skinInstance->dataRef);
		auto* skinPartition = nif.GetHeader().GetBlock<nifly::NiSkinPartition>(skinInstance->skinPartitionRef);
		if (!skinData || !skinPartition) {
			Report(LogLevel::Error, "'" + path + "': shape '" + shapeName + "' lacks skin data or a skin partition; the engine can't skin it");
			return;
		}

		const auto boneCount = static_cast<std::uint32_t>(skinInstance->boneRefs.GetSize());
		if (skinData->bones.size() != boneCount) {
			Report(LogLevel::Error, "'" + path + "': shape '" + shapeName + "' lists " + std::to_string(boneCount) + " bones but has skin data for " + std::to_string(skinData->bones.size()));
			return;
		}

		RE::NiPointer<RE::NiSkinInstance> skin(new RE::NiSkinInstance());
		skin->SetBoneCount(boneCount);
		std::uint32_t b = 0;
		for (auto& boneRef : skinInstance->boneRefs) {
			auto* bone = nif.GetHeader().GetBlock<nifly::NiNode>(boneRef);
			auto it = bone ? converted.find(bone) : converted.end();
			if (it == converted.end())
				Report(LogLevel::Error, "'" + path + "': bone " + std::to_string(b) + " of shape '" + shapeName + "' is no node of the file");
			else {
				skin->bones[b] = it->second;
				skin->boneWorldTransforms[b] = &it->second->world;
			}
			b++;
		}
		if (auto* target = nif.GetHeader().GetBlock<nifly::NiNode>(skinInstance->targetRef)) {
			auto it = converted.find(target);
			if (it != converted.end())
				skin->rootParent = it->second;
		}

		RE::NiPointer<RE::NiSkinData> data(new RE::NiSkinData());
		data->rootParentToSkin = ToNi(skinData->skinTransform);
		data->boneStore.resize(boneCount);
		for (std::uint32_t i = 0; i < boneCount; i++) {
			data->boneStore[i].skinToBone = ToNi(skinData->bones[i].boneTransform);
			const auto& bounds = skinData->bones[i].bounds;
			data->boneStore[i].bound.center = {bounds.center.x, bounds.center.y, bounds.center.z};
			data->boneStore[i].bound.radius = bounds.radius;
		}
		data->boneData = data->boneStore.data();
		data->bones = boneCount;

		RE::NiPointer<RE::NiSkinPartition> partition(new RE::NiSkinPartition());
		const auto& verts = a_source->vertData;
		partition->vertexCount = static_cast<std::uint32_t>(verts.size());
		partition->vertexStore.assign(static_cast<size_t>(partition->vertexCount) * kVertexSize, 0);
		for (std::uint32_t v = 0; v < partition->vertexCount; v++) {
			std::uint8_t* out = partition->vertexStore.data() + static_cast<size_t>(v) * kVertexSize;
			const float position[4] = {verts[v].vert.x, verts[v].vert.y, verts[v].vert.z, verts[v].bitangentX};
			std::memcpy(out, position, sizeof(position));
			hdt::SkyrimSystem::BoneData bones{};
			for (int k = 0; k < 4; k++) {
				bones.boneWeights[k] = ToHalf(verts[v].weights[k]);
				bones.boneIndices[k] = verts[v].weightBones[k];
			}
			std::memcpy(out + kVertexPositionSize, &bones, sizeof(bones));
		}
		partition->bufferData.rawVertexData = partition->vertexStore.data();

		skinPartition->PrepareTrueTriangles();
		partition->numPartitions = static_cast<std::uint32_t>(skinPartition->partitions.size());
		for (auto& sourcePartition : skinPartition->partitions) {
			auto& target = partition->partitions.emplace_back();
			target.vertexDesc.flags = static_cast<RE::BSGraphics::Vertex::Flags>(RE::BSGraphics::Vertex::VF_VERTEX | RE::BSGraphics::Vertex::VF_SKINNED | RE::BSGraphics::Vertex::VF_FULLPREC);
			target.vertexDesc.size = kVertexSize;
			target.vertices = static_cast<std::uint16_t>(sourcePartition.numVertices);
			target.bones = static_cast<std::uint16_t>(sourcePartition.bones.size());
			target.bonesPerVertex = sourcePartition.numWeightsPerVertex;
			target.triangles = static_cast<std::uint16_t>(sourcePartition.trueTriangles.size());
			for (auto& tri : sourcePartition.trueTriangles) {
				target.triStore.push_back(tri.p1);
				target.triStore.push_back(tri.p2);
				target.triStore.push_back(tri.p3);
			}
			target.buffData = &partition->bufferData;
		}
		for (auto& target : partition->partitions)
			target.triList = target.triStore.data();
		if (partition->partitions.empty()) {
			Report(LogLevel::Error, "'" + path + "': shape '" + shapeName + "' has an empty skin partition; the engine can't skin it");
			return;
		}

		skin->skinData = data;
		skin->skinPartition = partition;
		a_geometry->GetGeometryRuntimeData().skinInstance = skin;
	}

	nifly::NifFile& nif;
	const std::string& path;
	std::unordered_map<nifly::NiObject*, RE::NiAVObject*> converted;
	std::vector<std::pair<nifly::BSTriShape*, RE::BSGeometry*>> skinned;
};

RE::NiPointer<RE::NiAVObject> ConvertNif(nifly::NifFile& a_nif, const std::string& a_path) {
	auto* root = a_nif.GetRootNode();
	if (!root) {
		Report(LogLevel::Error, "'" + a_path + "' has no root node");
		return nullptr;
	}
	Converter converter(a_nif, a_path);
	RE::NiPointer<RE::NiAVObject> converted(converter.Convert(root));
	converter.LinkSkins();
	RE::NiUpdateData update;
	converted->Update(update);
	return converted;
}

// The engine clones a model from its cache for every reference that shows it.
RE::NiPointer<RE::NiNode> CloneModel(const Model& a_model) {
	RE::NiCloningProcess cloning;
	cloning.copyType = 1;
	RE::NiPointer<RE::NiAVObject> clone(static_cast<RE::NiAVObject*>(a_model.root->CreateClone(cloning)));
	a_model.root->ProcessClone(cloning);
	if (!clone || !clone->AsNode()) {
		Report(LogLevel::Error, "'" + a_model.path + "': the model root is no node");
		return nullptr;
	}
	return RE::NiPointer<RE::NiNode>(clone->AsNode());
}

void ForEachGeometry(RE::NiAVObject* a_object, const std::function<void(RE::BSGeometry*)>& a_function) {
	if (!a_object)
		return;
	if (auto* geometry = a_object->AsGeometry())
		a_function(geometry);
	if (auto* node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			ForEachGeometry(child.get(), a_function);
	}
}

// ---------------------------------------------------------------------------
// Engine functions ActorManager.cpp calls
// ---------------------------------------------------------------------------

RE::NiAVObject* ActorCalculateLOS(RE::Actor*, RE::NiPoint3*, RE::NiPoint3*, float) {
	// The previewer shows the actor alone: nothing blocks the line of sight.
	return nullptr;
}

bool TESNPCGetFaceGeomPath(RE::TESNPC* a_npc, char* a_buffer) {
	auto it = g_faceGenPaths.find(a_npc);
	if (it == g_faceGenPaths.end() || it->second.empty() || it->second.size() >= MAX_PATH)
		return false;
	std::memcpy(a_buffer, it->second.c_str(), it->second.size() + 1);
	return true;
}

RE::NiStream* NiStreamConstructor(RE::NiStream* a_stream) {
	return new (a_stream) RE::NiStream();
}

RE::NiStream* NiStreamDestructor(RE::NiStream* a_stream) {
	a_stream->~NiStream();
	return a_stream;
}

void LoadFsmpConfig(const std::string& a_configsFile) {
	// FSMP reads its configuration from data/skse/plugins/hdtSkinnedMeshConfigs/configs.xml
	// relative to the game folder, which is the working directory in game.
	// That folder is set up as the working directory while FSMP reads it.
	namespace fs = std::filesystem;
	std::error_code ec;
	std::random_device random;
	const fs::path folder = fs::temp_directory_path(ec) / ("bodyslide-fsmp-" + std::to_string(random()));
	if (ec) {
		Report(LogLevel::Error, "no temporary folder for reading configs.xml: " + ec.message());
		return;
	}
	const fs::path configs = folder / "data" / "skse" / "plugins" / "hdtSkinnedMeshConfigs" / "configs.xml";
	fs::create_directories(configs.parent_path(), ec);
	if (!ec && !a_configsFile.empty())
		fs::copy_file(fs::u8path(a_configsFile), configs, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		Report(LogLevel::Error, "can't prepare configs.xml for reading: " + ec.message());
		fs::remove_all(folder, ec);
		return;
	}

	const fs::path previous = fs::current_path(ec);
	if (!ec)
		fs::current_path(folder, ec);
	if (ec)
		Report(LogLevel::Error, "can't read configs.xml: " + ec.message());
	else {
		hdt::loadConfig();
		fs::current_path(previous, ec);
		if (ec)
			Report(LogLevel::Error, "can't restore the working directory: " + ec.message());
	}
	fs::remove_all(folder, ec);
}

} // namespace

// ---------------------------------------------------------------------------
// Plugin load
// ---------------------------------------------------------------------------

void Initialise(FileReader readDataFile, LogSink sink, const std::string& configsFile) {
	g_sink = sink;
	SKSE::log::SetSink(&ForwardLog);
	standin::SetFileReader(std::move(readDataFile));

	static bool initialised = false;
	if (initialised)
		return;
	initialised = true;

	REL::RegisterFunction(kActorCalculateLOS, reinterpret_cast<std::uintptr_t>(&ActorCalculateLOS));
	REL::RegisterFunction(kTESNPCGetFaceGeomPath, reinterpret_cast<std::uintptr_t>(&TESNPCGetFaceGeomPath));
	REL::RegisterFunction(kNiStreamConstructor, reinterpret_cast<std::uintptr_t>(&NiStreamConstructor));
	REL::RegisterFunction(kNiStreamDestructor, reinterpret_cast<std::uintptr_t>(&NiStreamDestructor));

	// The player camera looks at the actor from the actor's position.
	auto* camera = RE::PlayerCamera::GetSingleton();
	camera->cameraRoot.reset(new RE::NiNode());
	camera->cameraRoot->name = "CameraRoot";
	auto firstPerson = RE::make_smart<RE::TESCameraState>();
	firstPerson->id = RE::CameraState::kFirstPerson;
	auto thirdPerson = RE::make_smart<RE::TESCameraState>();
	thirdPerson->id = RE::CameraState::kThirdPerson;
	camera->GetRuntimeData().cameraStates[static_cast<size_t>(RE::CameraState::kFirstPerson)] = firstPerson;
	camera->GetRuntimeData().cameraStates[static_cast<size_t>(RE::CameraState::kThirdPerson)] = thirdPerson;
	camera->currentState = thirdPerson;

	// SKSEPlugin_Load (main.cpp)
	if (configsFile.empty())
		Report(LogLevel::Info, "no configs.xml; FSMP's defaults apply");
	LoadFsmpConfig(configsFile);
	spdlog::set_level(static_cast<spdlog::level::level_enum>(hdt::g_logLevel));
	Report(LogLevel::Info, std::format("{} v{} ({}), log level {}", Plugin::NAME, Plugin::VERSION.string(), Plugin::AVX_VARIANT, 5 - hdt::g_logLevel));
	hdt::logConfig();

	Events::Sources::FrameEventSource::GetSingleton()->AddEventSink(hdt::ActorManager::instance());
	Events::Sources::FrameEventSource::GetSingleton()->AddEventSink(hdt::SkyrimPhysicsWorld::get());
	Events::Sources::FrameSyncEventSource::GetSingleton()->AddEventSink(hdt::SkyrimPhysicsWorld::get());
	Events::Sources::ShutdownEventEventSource::GetSingleton()->AddEventSink(hdt::ActorManager::instance());
	Events::Sources::ShutdownEventEventSource::GetSingleton()->AddEventSink(hdt::SkyrimPhysicsWorld::get());
	Events::Sources::ArmorAttachEventSource::GetSingleton()->AddEventSink(hdt::ActorManager::instance());
	Events::Sources::ArmorDetachEventSource::GetSingleton()->AddEventSink(hdt::ActorManager::instance());
	Events::Sources::SkinSingleHeadGeometryEventSource::GetSingleton()->AddEventSink(hdt::ActorManager::instance());
	Events::Sources::SkinAllHeadGeometryEventSource::GetSingleton()->AddEventSink(hdt::ActorManager::instance());
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

std::shared_ptr<Model> LoadModel(nifly::NifFile& nif, const std::string& path) {
	auto root = ConvertNif(nif, path);
	if (!root)
		return nullptr;
	auto model = std::make_shared<Model>();
	model->path = path;
	model->root = root;
	return model;
}

const std::string& ModelPath(const Model& model) {
	return model.path;
}

bool SetShapeVertices(Model& model, const std::string& shapeName, const std::vector<nifly::Vector3>& positions) {
	auto* object = model.root->GetObjectByName(RE::BSFixedString(shapeName));
	auto* shape = object ? object->AsTriShape() : nullptr;
	if (!shape) {
		Report(LogLevel::Error, "'" + model.path + "' has no shape '" + shapeName + "'");
		return false;
	}

	if (auto* dynamicShape = shape->AsDynamicTriShape()) {
		if (dynamicShape->dynamicStore.size() != positions.size() * 4) {
			Report(LogLevel::Error, "'" + model.path + "': shape '" + shapeName + "' has " + std::to_string(dynamicShape->dynamicStore.size() / 4) + " vertices, " + std::to_string(positions.size()) + " given");
			return false;
		}
		for (size_t v = 0; v < positions.size(); v++) {
			dynamicShape->dynamicStore[v * 4 + 0] = positions[v].x;
			dynamicShape->dynamicStore[v * 4 + 1] = positions[v].y;
			dynamicShape->dynamicStore[v * 4 + 2] = positions[v].z;
		}
	}

	auto& skin = shape->GetGeometryRuntimeData().skinInstance;
	if (skin && skin->skinPartition) {
		auto& partition = *skin->skinPartition;
		if (partition.vertexCount != positions.size()) {
			Report(LogLevel::Error, "'" + model.path + "': shape '" + shapeName + "' has " + std::to_string(partition.vertexCount) + " vertices, " + std::to_string(positions.size()) + " given");
			return false;
		}
		for (size_t v = 0; v < positions.size(); v++) {
			const float position[3] = {positions[v].x, positions[v].y, positions[v].z};
			std::memcpy(partition.vertexStore.data() + v * kVertexSize, position, sizeof(position));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Actor
// ---------------------------------------------------------------------------

struct Actor::Impl {
	struct Attachment {
		const Model* model = nullptr;
		RE::NiPointer<RE::NiNode> clone;		 // the model's clone, kept for the nodes its skin uses
		RE::NiPointer<RE::NiNode> shapes;		 // where the clone's shapes are in the actor's scene graph
	};

	RE::NiPointer<RE::NiNode> worldRoot;
	RE::NiPointer<RE::NiNode> cell;
	RE::NiPointer<RE::NiNode> root;

	RE::TESRace race;
	RE::TESNPC npc;
	RE::Actor actor;

	RE::hkaSkeleton rig;
	RE::hkbCharacterSetup characterSetup;

	// Inventory: one entry per equipped item, its extra data marking it as worn.
	struct InventoryItem {
		std::unique_ptr<RE::TESObjectARMO> armor;
		std::unique_ptr<RE::ExtraDataList> extra;
		RE::BSSimpleList<RE::ExtraDataList*> extraLists;
		RE::InventoryEntryData entry;
	};
	std::vector<std::unique_ptr<InventoryItem>> inventory;
	RE::BSSimpleList<RE::InventoryEntryData*> entryList;
	RE::InventoryChanges changes;

	std::vector<Attachment> attachments;
};

Actor::Actor(const Model& skeleton, const ActorInfo& info) :
	impl(std::make_unique<Impl>()) {
	static RE::FormID nextReference = 0xFF000800;

	impl->race.skeletonModels[0].model = info.raceSkeleton[0];
	impl->race.skeletonModels[1].model = info.raceSkeleton[1];
	impl->npc.formID = info.npcFormId;
	impl->npc.fullName = info.name;
	impl->npc.race = &impl->race;
	impl->actor.formID = nextReference++;
	impl->actor.data.objectReference = &impl->npc;
	RE::RegisterForm(&impl->actor);
	if (info.npcFormId != 0)
		RE::RegisterForm(&impl->npc);
	g_faceGenPaths[&impl->npc] = info.faceGenPath;

	// The actor's 3D sits in a loaded cell of the world's scene graph.
	impl->worldRoot.reset(new RE::NiNode());
	impl->worldRoot->name = "WorldRoot Node";
	RE::NiPointer<RE::NiNode> shadowScene(new RE::NiNode());
	shadowScene->name = "shadow scene node";
	impl->worldRoot->AttachChild(shadowScene.get());
	impl->cell.reset(new RE::NiNode());
	impl->cell->name = "ObjectLODRoot";
	shadowScene->AttachChild(impl->cell.get());

	impl->root = CloneModel(skeleton);
	if (!impl->root)
		return;
	impl->root->userData = &impl->actor;
	impl->actor.thirdPerson3D = impl->root.get();
	impl->cell->AttachChild(impl->root.get());
	UpdateWorld();
}

Actor::~Actor() {
	if (impl->root) {
		impl->attachments.clear();
		impl->cell->DetachChild(impl->root.get());
		impl->root.reset();
		// FSMP drops a skeleton once nothing but FSMP holds it, on the next frame.
		RE::BSTimer::GetSingleton()->delta = 0.0f;
		RE::BSTimer::GetSingleton()->realTimeDelta = 0.0f;
		Events::FrameEvent frame;
		frame.gamePaused = false;
		Events::Sources::FrameEventSource::GetSingleton()->SendEvent(&frame);
	}
	g_faceGenPaths.erase(&impl->npc);
	RE::UnregisterForm(&impl->actor);
	RE::UnregisterForm(&impl->npc);
}

void Actor::SetAnimationSkeleton(const std::vector<RigBone>& bones) {
	impl->rig.bones.clear();
	impl->rig.referencePose.clear();
	for (auto& bone : bones) {
		impl->rig.bones.push_back(RE::hkaBone{RE::hkStringPtr(bone.name), false});
		RE::hkQsTransform pose;
		for (int i = 0; i < 3; i++)
			pose.translation.quad.m128_f32[i] = bone.translation[i];
		for (int i = 0; i < 4; i++) {
			pose.rotation.vec.quad.m128_f32[i] = bone.rotation[i];
			pose.scale.quad.m128_f32[i] = i < 3 ? bone.scale : 0.0f;
		}
		impl->rig.referencePose.push_back(pose);
	}
	impl->characterSetup.animationSkeleton = RE::hkRefPtr<RE::hkaSkeleton>(&impl->rig);
	auto graph = RE::make_smart<RE::BShkbAnimationGraph>();
	graph->characterInstance.setup = RE::hkRefPtr<RE::hkbCharacterSetup>(&impl->characterSetup);
	auto manager = RE::make_smart<RE::BSAnimationGraphManager>();
	manager->graphs.push_back(graph);
	impl->actor.animationGraphManager = manager;
}

void Actor::SetWornItems(const std::vector<WornItem>& items) {
	if (impl->actor.extraList.HasType<RE::ExtraContainerChanges>()) {
		Report(LogLevel::Error, "the actor's inventory is set already");
		return;
	}
	for (auto& item : items) {
		auto entry = std::make_unique<Impl::InventoryItem>();
		entry->armor = std::make_unique<RE::TESObjectARMO>();
		entry->armor->formID = item.formId;
		entry->armor->fullName = item.name;
		entry->armor->bipedModelData.bipedObjectSlots = static_cast<RE::BIPED_MODEL::BipedObjectSlot>(item.bipedSlots);
		entry->extra = std::make_unique<RE::ExtraDataList>();
		entry->extra->Add(std::make_unique<RE::ExtraWorn>());
		entry->extraLists.push_back(entry->extra.get());
		entry->entry.object = entry->armor.get();
		entry->entry.extraLists = &entry->extraLists;
		impl->entryList.push_back(&entry->entry);
		impl->inventory.push_back(std::move(entry));
	}
	impl->changes.entryList = &impl->entryList;
	auto containerChanges = std::make_unique<RE::ExtraContainerChanges>();
	containerChanges->changes = &impl->changes;
	impl->actor.extraList.Add(std::move(containerChanges));
}

void Actor::AttachArmor(const Model& model) {
	if (!impl->root)
		return;
	auto armor = CloneModel(model);
	if (!armor)
		return;
	auto* skeleton = impl->root.get();

	// BipedAnimHooks::func (Hooks.cpp): the event before and after the engine attaches the model.
	Events::ArmorAttachEvent event;
	event.armorModel = armor.get();
	event.skeleton = skeleton;
	event.attachedNode = nullptr;
	event.hasAttached = false;
	Events::Sources::ArmorAttachEventSource::GetSingleton()->SendEvent(&event);

	// The engine binds the model's skin to the skeleton's nodes by bone name.
	ForEachGeometry(armor.get(), [&](RE::BSGeometry* geometry) {
		auto& skin = geometry->GetGeometryRuntimeData().skinInstance;
		if (!skin)
			return;
		for (size_t b = 0; b < skin->boneStore.size(); b++) {
			if (!skin->bones[b])
				continue;
			auto* node = skeleton->GetObjectByName(skin->bones[b]->name);
			if (!node) {
				Report(LogLevel::Error, "'" + model.path + "': bone '" + std::string(skin->bones[b]->name.c_str()) + "' of shape '" + std::string(geometry->name.c_str()) + "' is not in the skeleton; it keeps the model's own node");
				continue;
			}
			skin->bones[b] = node;
			skin->boneWorldTransforms[b] = &node->world;
		}
		skin->rootParent = skeleton;
	});
	skeleton->AttachChild(armor.get());
	RE::NiUpdateData update;
	armor->Update(update);

	event.attachedNode = armor.get();
	event.hasAttached = true;
	Events::Sources::ArmorAttachEventSource::GetSingleton()->SendEvent(&event);

	impl->attachments.push_back({&model, armor, armor});
}

void Actor::AttachHead(const Model& faceGen) {
	if (!impl->root)
		return;
	auto head = CloneModel(faceGen);
	if (!head)
		return;
	auto* skeleton = impl->root.get();

	auto* faceGenNode = head->GetObjectByName(RE::BSFixedString(kFaceGenNodeName));
	auto* headNode = faceGenNode ? dynamic_cast<RE::BSFaceGenNiNode*>(faceGenNode) : nullptr;
	if (!headNode) {
		Report(LogLevel::Error, "'" + faceGen.path + "' has no " + kFaceGenNodeName + " node; the head is not attached");
		return;
	}
	RE::NiPointer<RE::NiNode> headHold(headNode);
	if (headNode->parent)
		headNode->parent->DetachChild(headNode);
	skeleton->AttachChild(headNode);
	RE::NiUpdateData update;
	headNode->Update(update);

	// BSFaceGenNiNodeHooks::SkinAllGeometry__Hook and SkinAllGeometry (Hooks.cpp).
	Events::SkinAllHeadGeometryEvent all;
	all.skeleton = skeleton;
	all.headNode = headNode;
	all.hasSkinned = false;
	Events::Sources::SkinAllHeadGeometryEventSource::GetSingleton()->SendEvent(&all);

	const auto& children = headNode->GetChildren();
	for (std::uint16_t i = 0; i < children.size(); i++) {
		auto child = children[i];
		auto* triShape = child ? child->AsTriShape() : nullptr;
		if (!triShape)
			continue;
		Events::SkinSingleHeadGeometryEvent single;
		single.headNode = headNode;
		single.skeleton = skeleton;
		single.geometry = triShape;
		Events::Sources::SkinSingleHeadGeometryEventSource::GetSingleton()->SendEvent(&single);
	}

	all.hasSkinned = true;
	Events::Sources::SkinAllHeadGeometryEventSource::GetSingleton()->SendEvent(&all);

	impl->attachments.push_back({&faceGen, head, headHold});
}

Node* Actor::FindNode(const std::string& name) const {
	if (!impl->root)
		return nullptr;
	return reinterpret_cast<Node*>(impl->root->GetObjectByName(RE::BSFixedString(name)));
}

void Actor::SetLocal(Node* node, const nifly::MatTransform& local) {
	if (node)
		reinterpret_cast<RE::NiAVObject*>(node)->local = ToNi(local);
}

std::vector<Actor::SceneObject> Actor::SceneGraph() const {
	std::vector<SceneObject> objects;
	if (!impl->root)
		return objects;
	std::function<void(RE::NiAVObject*, int)> visit = [&](RE::NiAVObject* object, int parent) {
		const int index = static_cast<int>(objects.size());
		auto* node = object->AsNode();
		objects.push_back({reinterpret_cast<Node*>(object), parent, object->name.c_str(), node != nullptr});
		if (!node)
			return;
		for (auto& child : node->GetChildren()) {
			if (child)
				visit(child.get(), index);
		}
	};
	visit(impl->root.get(), -1);
	return objects;
}

void Actor::GetTransforms(const Node* object, nifly::MatTransform& local, nifly::MatTransform& world) {
	auto* av = reinterpret_cast<const RE::NiAVObject*>(object);
	local = ToNifly(av->local);
	world = ToNifly(av->world);
}

void Actor::SetTransforms(Node* object, const nifly::MatTransform& local, const nifly::MatTransform& world) {
	auto* av = reinterpret_cast<RE::NiAVObject*>(object);
	av->local = ToNi(local);
	av->world = ToNi(world);
}

void Actor::UpdateWorld() {
	if (!impl->root)
		return;
	RE::NiUpdateData update;
	impl->root->Update(update);
}

void Actor::Step(float seconds) {
	if (!impl->root)
		return;
	// MainHooks::Update and MainHooks::Unk_sub (Hooks.cpp).
	auto* timer = RE::BSTimer::GetSingleton();
	timer->delta = seconds;
	timer->realTimeDelta = seconds;
	Events::FrameEvent frame;
	frame.gamePaused = false;
	Events::Sources::FrameEventSource::GetSingleton()->SendEvent(&frame);
	Events::FrameSyncEvent frameSync;
	Events::Sources::FrameSyncEventSource::GetSingleton()->SendEvent(&frameSync);
}

bool Actor::GetSkinBoneWorlds(const Model& model, const std::string& shapeName, std::vector<nifly::MatTransform>& worlds) const {
	worlds.clear();
	for (auto& attachment : impl->attachments) {
		if (attachment.model != &model)
			continue;
		auto* object = attachment.shapes->GetObjectByName(RE::BSFixedString(shapeName));
		auto* geometry = object ? object->AsGeometry() : nullptr;
		if (!geometry || !geometry->GetGeometryRuntimeData().skinInstance)
			return false;
		auto& skin = geometry->GetGeometryRuntimeData().skinInstance;
		worlds.reserve(skin->boneStore.size());
		for (size_t b = 0; b < skin->boneStore.size(); b++)
			worlds.push_back(skin->bones[b] ? ToNifly(skin->bones[b]->world) : nifly::MatTransform());
		return true;
	}
	return false;
}

} // namespace fsmp

// ---------------------------------------------------------------------------
// NiStream (stand-in): loads a NIF into engine objects.
// ---------------------------------------------------------------------------

bool RE::NiStream::Load1(BSResourceNiBinaryStream* a_stream) {
	topObjects.clear();
	topObjects.push_back(nullptr);
	if (!a_stream || !a_stream->good())
		return false;

	const auto& bytes = a_stream->bytes();
	std::istringstream stream(std::string(bytes.data(), bytes.size()), std::istringstream::binary);
	nifly::NifFile nif;
	if (nif.Load(stream) != 0) {
		SKSE::log::error("'{}' is no readable NIF file", a_stream->path());
		return false;
	}
	auto root = fsmp::ConvertNif(nif, a_stream->path());
	if (!root)
		return false;
	topObjects[0] = RE::NiPointer<RE::NiObject>(root.get());
	return true;
}
