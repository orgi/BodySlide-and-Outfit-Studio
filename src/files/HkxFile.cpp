/*
 * Havok packfile reader for Skyrim Special Edition animation data.
 *
 * Class layouts are the hk_2010.2.0-r1 x64 layouts used by the game. The
 * spline track encoding follows the reverse-engineered description in
 * HavokLib (GPL-3.0, https://github.com/PredatorCZ/HavokLib).
 */

#include "HkxFile.h"

#include <algorithm>
#include <cmath>

namespace hkx {

namespace {

constexpr uint32_t kMagic0 = 0x57E0E057;
constexpr uint32_t kMagic1 = 0x10C0C010;
constexpr int32_t kFileVersion = 8;
constexpr const char* kContentsVersion = "hk_2010.2.0-r1";
constexpr uint32_t kHeaderSize = 64;
constexpr uint32_t kSectionHeaderSize = 48;
constexpr uint32_t kArraySize = 16; // hkArray: data pointer, size, capacity|flags
constexpr uint32_t kPointerSize = 8;

// Member offsets (hk_2010.2.0-r1, x64). Every listed class derives from
// hkReferencedObject (16 bytes: vtable, memSizeAndFlags, referenceCount).
namespace layout {
constexpr uint32_t containerSkeletons = 16;
constexpr uint32_t containerBindings = 48;

constexpr uint32_t skeletonName = 16;
constexpr uint32_t skeletonParentIndices = 24;
constexpr uint32_t skeletonBones = 40;
constexpr uint32_t skeletonReferencePose = 56;
constexpr uint32_t boneSize = 16;
constexpr uint32_t qsTransformSize = 48;

constexpr uint32_t bindingSkeletonName = 16;
constexpr uint32_t bindingAnimation = 24;
constexpr uint32_t bindingTrackToBone = 32;
constexpr uint32_t bindingBlendHint = 64;

constexpr uint32_t animType = 16;
constexpr uint32_t animDuration = 20;
constexpr uint32_t animNumTransformTracks = 24;
constexpr uint32_t animNumFloatTracks = 28;
constexpr uint32_t splineNumFrames = 56;
constexpr uint32_t splineNumBlocks = 60;
constexpr uint32_t splineMaxFramesPerBlock = 64;
constexpr uint32_t splineBlockDuration = 72;
constexpr uint32_t splineBlockInverseDuration = 76;
constexpr uint32_t splineFrameDuration = 80;
constexpr uint32_t splineBlockOffsets = 88;
constexpr uint32_t splineData = 152;
constexpr uint32_t splineSize = 176;

constexpr uint32_t projectCharacterFilenames = 48;

constexpr uint32_t characterAnimationNames = 48;
constexpr uint32_t characterName = 160;
constexpr uint32_t characterRigName = 168;
constexpr uint32_t characterBehaviorFilename = 184;
} // namespace layout

constexpr int32_t kAnimationTypeSplineCompressed = 5; // hkaAnimation::HK_SPLINE_COMPRESSED_ANIMATION
constexpr uint8_t kBlendHintAdditive = 1;

template<class T>
T ReadRaw(const std::vector<char>& buf, size_t offset) {
	T value;
	std::memcpy(&value, buf.data() + offset, sizeof(T));
	return value;
}

// Sequential reader over an animation data buffer. Alignment is relative to
// the start of the buffer, which Havok keeps 16-byte aligned.
struct Cursor {
	const uint8_t* data;
	size_t size;
	size_t pos;
	bool ok = true;

	bool Need(size_t n) {
		if (!ok || n > size || pos > size - n)
			ok = false;
		return ok;
	}
	uint8_t U8() {
		if (!Need(1))
			return 0;
		return data[pos++];
	}
	uint16_t U16() {
		if (!Need(2))
			return 0;
		uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
		pos += 2;
		return v;
	}
	uint32_t U32() {
		if (!Need(4))
			return 0;
		uint32_t v;
		std::memcpy(&v, data + pos, 4);
		pos += 4;
		return v;
	}
	uint64_t U64(size_t bytes) {
		if (!Need(bytes))
			return 0;
		uint64_t v = 0;
		for (size_t i = 0; i < bytes; i++)
			v |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
		pos += bytes;
		return v;
	}
	float F32() {
		uint32_t v = U32();
		float f;
		std::memcpy(&f, &v, 4);
		return f;
	}
	void Skip(size_t n) {
		if (Need(n))
			pos += n;
	}
	void Align(size_t a) {
		size_t rem = pos % a;
		if (rem)
			Skip(a - rem);
	}
};

enum RotationQuantization { kPolar32, kThreeComp40, kThreeComp48, kThreeComp24, kStraight16, kUncompressed };

void Normalize(std::array<float, 4>& q) {
	float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (len > 0.0f) {
		for (float& c : q)
			c /= len;
	}
	else
		q = {0.0f, 0.0f, 0.0f, 1.0f};
}

// Smallest-three encodings: three stored components plus the index of the
// largest component, which is reconstructed from the unit length.
std::array<float, 4> ExpandThreeComp(const float stored[3], int missingIdx, bool negateMissing) {
	float sq = 1.0f - (stored[0] * stored[0] + stored[1] * stored[1] + stored[2] * stored[2]);
	float missing = sq > 0.0f ? std::sqrt(sq) : 0.0f;
	if (negateMissing)
		missing = -missing;

	std::array<float, 4> q{};
	int s = 0;
	for (int i = 0; i < 4; i++)
		q[i] = (i == missingIdx) ? missing : stored[s++];
	return q;
}

bool ReadQuat(Cursor& c, int quantization, std::array<float, 4>& out) {
	switch (quantization) {
		case kThreeComp40: {
			uint64_t v = c.U64(5);
			constexpr float fraction = 0.000345436f; // (1 / sqrt(2)) / 2047
			float stored[3];
			for (int i = 0; i < 3; i++)
				stored[i] = (static_cast<int>((v >> (12 * i)) & 0xFFF) - 2047) * fraction;
			out = ExpandThreeComp(stored, static_cast<int>((v >> 36) & 3), ((v >> 38) & 1) != 0);
			return c.ok;
		}
		case kThreeComp48: {
			uint16_t x = c.U16();
			uint16_t y = c.U16();
			uint16_t z = c.U16();
			constexpr float fraction = 0.000043161f; // (1 / sqrt(2)) / 16383
			const uint16_t raw[3] = {x, y, z};
			float stored[3];
			for (int i = 0; i < 3; i++)
				stored[i] = (static_cast<int>(raw[i] & 0x7FFF) - 16383) * fraction;
			int missingIdx = ((y >> 14) & 2) | ((x >> 15) & 1);
			out = ExpandThreeComp(stored, missingIdx, (z >> 15) != 0);
			return c.ok;
		}
		case kPolar32: {
			uint32_t v = c.U32();
			constexpr float pi = 3.14159265f;
			constexpr float rFraction = 1.0f / 1023.0f;
			constexpr float phiFraction = (0.5f * pi) / 511.0f;

			float r = static_cast<float>((v >> 18) & 0x3FF) * rFraction;
			r = 1.0f - r * r;
			float phiTheta = static_cast<float>(v & 0x3FFFF);
			float phi = std::floor(std::sqrt(phiTheta));
			float theta = 0.0f;
			if (phi > 0.0f) {
				theta = (0.25f * pi) * (phiTheta - phi * phi) / phi;
				phi = phiFraction * phi;
			}
			float magnitude = std::sqrt(std::max(0.0f, 1.0f - r * r));
			out = {std::sin(phi) * std::cos(theta) * magnitude, std::sin(phi) * std::sin(theta) * magnitude, std::cos(phi) * magnitude, r};
			for (int i = 0; i < 4; i++) {
				if (v & (0x10000000u << i))
					out[i] = -out[i];
			}
			return c.ok;
		}
		case kUncompressed:
			for (int i = 0; i < 4; i++)
				out[i] = c.F32();
			return c.ok;
		default: return false;
	}
}

size_t QuatAlignment(int quantization) {
	switch (quantization) {
		case kThreeComp48:
		case kStraight16: return 2;
		case kPolar32:
		case kUncompressed: return 4;
		default: return 1;
	}
}

// Algorithm A2.1, The NURBS Book (2nd ed.).
int FindKnotSpan(int degree, float value, int numControlPoints, const std::vector<uint8_t>& knots) {
	if (value >= knots[numControlPoints])
		return numControlPoints - 1;
	if (value <= knots[degree])
		return degree;

	int low = degree;
	int high = numControlPoints;
	int mid = (low + high) / 2;
	for (int guard = 0; guard < 64 && (value < knots[mid] || value >= knots[mid + 1]); guard++) {
		if (value < knots[mid])
			high = mid;
		else
			low = mid;
		mid = (low + high) / 2;
	}
	return mid;
}

// Non-zero B-spline basis functions N[span - degree .. span] at `frame`.
void BasisFunctions(int span, int degree, float frame, const std::vector<uint8_t>& knots, float N[5]) {
	N[0] = 1.0f;
	for (int i = 1; i <= degree; i++)
		N[i] = 0.0f;

	for (int i = 1; i <= degree; i++) {
		for (int j = i - 1; j >= 0; j--) {
			float denom = static_cast<float>(knots[span + i - j]) - static_cast<float>(knots[span - j]);
			float a = denom != 0.0f ? (frame - knots[span - j]) / denom : 0.0f;
			float tmp = N[j] * a;
			N[j + 1] += N[j] - tmp;
			N[j] = tmp;
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Packfile
// ---------------------------------------------------------------------------

bool Packfile::Load(std::vector<char> data, std::string& error) {
	file = std::move(data);
	pointers.clear();
	objects.clear();
	objectOrder.clear();

	if (file.size() < kHeaderSize) {
		error = "file too small for a Havok packfile header";
		return false;
	}
	if (ReadRaw<uint32_t>(file, 0) != kMagic0 || ReadRaw<uint32_t>(file, 4) != kMagic1) {
		error = "not a Havok packfile (bad magic)";
		return false;
	}

	int32_t fileVersion = ReadRaw<int32_t>(file, 12);
	uint8_t pointerSize = static_cast<uint8_t>(file[16]);
	uint8_t littleEndian = static_cast<uint8_t>(file[17]);
	std::string contentsVersion(file.data() + 40, strnlen(file.data() + 40, 16));
	if (fileVersion != kFileVersion || contentsVersion != kContentsVersion) {
		error = "unsupported Havok version '" + contentsVersion + "' (file version " + std::to_string(fileVersion) + "), expected " + kContentsVersion;
		return false;
	}
	if (pointerSize != kPointerSize || littleEndian != 1) {
		error = "unsupported packfile layout (pointer size " + std::to_string(pointerSize) + ", little endian " + std::to_string(littleEndian)
			  + "); Skyrim SE requires 64-bit little-endian files";
		return false;
	}

	int32_t numSections = ReadRaw<int32_t>(file, 20);
	if (numSections <= 0 || numSections > 16 || kHeaderSize + static_cast<size_t>(numSections) * kSectionHeaderSize > file.size()) {
		error = "invalid section count " + std::to_string(numSections);
		return false;
	}

	struct Section {
		std::string tag;
		uint32_t start, local, global, virt, exports;
	};
	std::vector<Section> sections(numSections);
	for (int32_t i = 0; i < numSections; i++) {
		size_t h = kHeaderSize + static_cast<size_t>(i) * kSectionHeaderSize;
		Section& s = sections[i];
		s.tag.assign(file.data() + h, strnlen(file.data() + h, 19));
		s.start = ReadRaw<uint32_t>(file, h + 20);
		s.local = ReadRaw<uint32_t>(file, h + 24);
		s.global = ReadRaw<uint32_t>(file, h + 28);
		s.virt = ReadRaw<uint32_t>(file, h + 32);
		s.exports = ReadRaw<uint32_t>(file, h + 36);
		if (s.local > s.global || s.global > s.virt || s.virt > s.exports || s.start > file.size() || s.exports > file.size() - s.start) {
			error = "corrupt section header '" + s.tag + "'";
			return false;
		}
	}

	int dataIdx = -1;
	int classIdx = -1;
	for (int32_t i = 0; i < numSections; i++) {
		if (sections[i].tag == "__data__")
			dataIdx = i;
		else if (sections[i].tag == "__classnames__")
			classIdx = i;
	}
	if (dataIdx < 0 || classIdx < 0) {
		error = "packfile lacks __data__ or __classnames__ section";
		return false;
	}

	const Section& ds = sections[dataIdx];
	const Section& cs = sections[classIdx];
	dataStart = ds.start;
	dataSize = ds.local;

	for (uint32_t p = ds.local; p + 8 <= ds.global; p += 8) {
		uint32_t from = ReadRaw<uint32_t>(file, ds.start + p);
		uint32_t to = ReadRaw<uint32_t>(file, ds.start + p + 4);
		if (from == 0xFFFFFFFF)
			continue;
		if (from < dataSize && to < dataSize)
			pointers[from] = to;
	}

	for (uint32_t p = ds.global; p + 12 <= ds.virt; p += 12) {
		uint32_t from = ReadRaw<uint32_t>(file, ds.start + p);
		uint32_t section = ReadRaw<uint32_t>(file, ds.start + p + 4);
		uint32_t to = ReadRaw<uint32_t>(file, ds.start + p + 8);
		if (from == 0xFFFFFFFF)
			continue;
		if (section == static_cast<uint32_t>(dataIdx) && from < dataSize && to < dataSize)
			pointers[from] = to;
	}

	for (uint32_t p = ds.virt; p + 12 <= ds.exports; p += 12) {
		uint32_t from = ReadRaw<uint32_t>(file, ds.start + p);
		uint32_t section = ReadRaw<uint32_t>(file, ds.start + p + 4);
		uint32_t nameOffset = ReadRaw<uint32_t>(file, ds.start + p + 8);
		if (from == 0xFFFFFFFF)
			continue;
		if (section != static_cast<uint32_t>(classIdx) || nameOffset >= cs.local || from >= dataSize) {
			error = "corrupt virtual fixup";
			return false;
		}
		const char* name = file.data() + cs.start + nameOffset;
		objects[from] = std::string(name, strnlen(name, cs.local - nameOffset));
		objectOrder.push_back(from);
	}

	return true;
}

std::vector<uint32_t> Packfile::FindObjects(const std::string& className) const {
	std::vector<uint32_t> result;
	for (uint32_t offset : objectOrder) {
		if (objects.at(offset) == className)
			result.push_back(offset);
	}
	return result;
}

std::string Packfile::ClassOf(uint32_t objectOffset) const {
	auto it = objects.find(objectOffset);
	return it != objects.end() ? it->second : std::string();
}

bool Packfile::ResolvePointer(uint32_t fieldOffset, uint32_t& target) const {
	auto it = pointers.find(fieldOffset);
	if (it == pointers.end())
		return false;
	target = it->second;
	return true;
}

std::string Packfile::ReadStringPtr(uint32_t fieldOffset) const {
	uint32_t target;
	if (!ResolvePointer(fieldOffset, target))
		return {};
	const char* s = reinterpret_cast<const char*>(Bytes(target));
	return std::string(s, strnlen(s, dataSize - target));
}

bool Packfile::ReadArray(uint32_t fieldOffset, uint32_t elementSize, Array& out) const {
	if (!InRange(fieldOffset, kArraySize))
		return false;
	int32_t size = Read<int32_t>(fieldOffset + kPointerSize);
	out = {};
	if (size == 0)
		return true;
	if (size < 0)
		return false;
	if (!ResolvePointer(fieldOffset, out.offset))
		return false;
	out.size = static_cast<uint32_t>(size);
	return elementSize == 0 || (out.size <= dataSize / elementSize && InRange(out.offset, out.size * elementSize));
}

// ---------------------------------------------------------------------------
// Typed readers
// ---------------------------------------------------------------------------

static bool FindContainerEntry(const Packfile& pf, uint32_t arrayField, uint32_t& object, std::string& error) {
	auto containers = pf.FindObjects("hkaAnimationContainer");
	if (containers.empty()) {
		error = "no hkaAnimationContainer in file";
		return false;
	}
	Packfile::Array arr;
	if (!pf.ReadArray(containers[0] + arrayField, kPointerSize, arr) || arr.size == 0) {
		error = "hkaAnimationContainer has no entries for the requested type";
		return false;
	}
	if (!pf.ResolvePointer(arr.offset, object)) {
		error = "hkaAnimationContainer entry is a null pointer";
		return false;
	}
	return true;
}

static QsTransform ReadQsTransform(const Packfile& pf, uint32_t offset) {
	QsTransform t;
	for (int i = 0; i < 3; i++)
		t.translation[i] = pf.Read<float>(offset + 4 * i);
	for (int i = 0; i < 4; i++)
		t.rotation[i] = pf.Read<float>(offset + 16 + 4 * i);
	for (int i = 0; i < 3; i++)
		t.scale[i] = pf.Read<float>(offset + 32 + 4 * i);
	return t;
}

bool Skeleton::Load(const Packfile& pf, std::string& error) {
	uint32_t obj;
	if (!FindContainerEntry(pf, layout::containerSkeletons, obj, error))
		return false;
	if (pf.ClassOf(obj) != "hkaSkeleton" || !pf.InRange(obj, layout::skeletonReferencePose + kArraySize)) {
		error = "skeleton entry is not an hkaSkeleton";
		return false;
	}

	Packfile::Array parents, bones, pose;
	if (!pf.ReadArray(obj + layout::skeletonParentIndices, 2, parents) || !pf.ReadArray(obj + layout::skeletonBones, layout::boneSize, bones)
		|| !pf.ReadArray(obj + layout::skeletonReferencePose, layout::qsTransformSize, pose)) {
		error = "hkaSkeleton arrays are corrupt";
		return false;
	}
	if (parents.size != bones.size || pose.size != bones.size) {
		error = "hkaSkeleton array sizes disagree (" + std::to_string(bones.size) + " bones, " + std::to_string(parents.size) + " parents, " + std::to_string(pose.size)
			  + " reference transforms)";
		return false;
	}

	name = pf.ReadStringPtr(obj + layout::skeletonName);
	boneNames.resize(bones.size);
	parentIndices.resize(bones.size);
	referencePose.resize(bones.size);
	for (uint32_t i = 0; i < bones.size; i++) {
		boneNames[i] = pf.ReadStringPtr(bones.offset + i * layout::boneSize);
		parentIndices[i] = pf.Read<int16_t>(parents.offset + i * 2);
		referencePose[i] = ReadQsTransform(pf, pose.offset + i * layout::qsTransformSize);
		if (parentIndices[i] >= static_cast<int16_t>(i)) {
			error = "hkaSkeleton bone '" + boneNames[i] + "' has parent index " + std::to_string(parentIndices[i]) + " that does not precede it";
			return false;
		}
	}
	return true;
}

bool SplineAnimation::Load(const Packfile& pf, std::string& error) {
	uint32_t binding;
	if (!FindContainerEntry(pf, layout::containerBindings, binding, error))
		return false;
	if (pf.ClassOf(binding) != "hkaAnimationBinding" || !pf.InRange(binding, layout::bindingBlendHint + 1)) {
		error = "binding entry is not an hkaAnimationBinding";
		return false;
	}

	uint32_t anim;
	if (!pf.ResolvePointer(binding + layout::bindingAnimation, anim)) {
		error = "hkaAnimationBinding has no animation";
		return false;
	}
	std::string animClass = pf.ClassOf(anim);
	if (animClass != "hkaSplineCompressedAnimation" || !pf.InRange(anim, layout::splineSize)
		|| pf.Read<int32_t>(anim + layout::animType) != kAnimationTypeSplineCompressed) {
		error = "unsupported animation class '" + animClass + "' (only hkaSplineCompressedAnimation is supported)";
		return false;
	}
	if (pf.Read<uint8_t>(binding + layout::bindingBlendHint) == kBlendHintAdditive) {
		error = "additive animations can't be played on their own";
		return false;
	}

	skeletonName = pf.ReadStringPtr(binding + layout::bindingSkeletonName);
	Packfile::Array map;
	if (!pf.ReadArray(binding + layout::bindingTrackToBone, 2, map)) {
		error = "hkaAnimationBinding track-to-bone array is corrupt";
		return false;
	}
	trackToBone.resize(map.size);
	for (uint32_t i = 0; i < map.size; i++)
		trackToBone[i] = pf.Read<int16_t>(map.offset + i * 2);

	duration = pf.Read<float>(anim + layout::animDuration);
	numTracks = pf.Read<uint32_t>(anim + layout::animNumTransformTracks);
	uint32_t numFloatTracks = pf.Read<uint32_t>(anim + layout::animNumFloatTracks);
	uint32_t numFrames = pf.Read<uint32_t>(anim + layout::splineNumFrames);
	uint32_t numBlocks = pf.Read<uint32_t>(anim + layout::splineNumBlocks);
	uint32_t maxFramesPerBlock = pf.Read<uint32_t>(anim + layout::splineMaxFramesPerBlock);
	blockDuration = pf.Read<float>(anim + layout::splineBlockDuration);
	blockInverseDuration = pf.Read<float>(anim + layout::splineBlockInverseDuration);
	frameDuration = pf.Read<float>(anim + layout::splineFrameDuration);

	if (!(duration > 0.0f) || !(frameDuration > 0.0f) || !(blockDuration > 0.0f) || numFrames == 0 || maxFramesPerBlock < 2 || numTracks > 4096) {
		error = "implausible spline animation header (duration " + std::to_string(duration) + ", frames " + std::to_string(numFrames) + ", tracks "
			  + std::to_string(numTracks) + ")";
		return false;
	}
	if (!trackToBone.empty() && trackToBone.size() != numTracks) {
		error = "track-to-bone map has " + std::to_string(trackToBone.size()) + " entries for " + std::to_string(numTracks) + " tracks";
		return false;
	}

	Packfile::Array offsets, data;
	if (!pf.ReadArray(anim + layout::splineBlockOffsets, 4, offsets) || !pf.ReadArray(anim + layout::splineData, 1, data)) {
		error = "spline animation arrays are corrupt";
		return false;
	}
	if (offsets.size != numBlocks || numBlocks == 0) {
		error = "spline animation lists " + std::to_string(offsets.size) + " block offsets for " + std::to_string(numBlocks) + " blocks";
		return false;
	}

	blocks.assign(numBlocks, {});
	for (uint32_t b = 0; b < numBlocks; b++) {
		uint32_t blockStart = pf.Read<uint32_t>(offsets.offset + b * 4);
		if (!DecodeBlock(pf.Bytes(data.offset), data.size, blockStart, numFloatTracks, blocks[b], error)) {
			error = "block " + std::to_string(b) + ": " + error;
			return false;
		}
	}
	return true;
}

bool SplineAnimation::DecodeBlock(const uint8_t* data, size_t dataSize, size_t blockStart, size_t numFloatTracks, Block& block, std::string& error) const {
	Cursor c{data, dataSize, blockStart};
	if (blockStart > dataSize || !c.Need(numTracks * 4 + numFloatTracks)) {
		error = "block offset outside of animation data";
		return false;
	}

	struct Mask {
		uint8_t quantization, position, rotation, scale;
	};
	std::vector<Mask> masks(numTracks);
	for (auto& m : masks) {
		m.quantization = c.U8();
		m.position = c.U8();
		m.rotation = c.U8();
		m.scale = c.U8();
	}
	c.Skip(numFloatTracks);
	c.Align(4);

	auto readVector = [&](int quantization, uint8_t types, float defaultValue, VectorTrack& track) {
		bool isStatic[3], isSpline[3];
		bool anySpline = false;
		for (int a = 0; a < 3; a++) {
			isStatic[a] = (types & (1 << a)) != 0;
			isSpline[a] = !isStatic[a] && (types & (1 << (4 + a))) != 0;
			anySpline |= isSpline[a];
		}

		if (!anySpline) {
			for (int a = 0; a < 3; a++)
				track.controlPoints[a] = {isStatic[a] ? c.F32() : defaultValue};
			return;
		}

		uint16_t numItems = c.U16();
		track.degree = c.U8();
		size_t numKnots = static_cast<size_t>(numItems) + track.degree + 2;
		if (!c.Need(numKnots))
			return;
		track.knots.assign(c.data + c.pos, c.data + c.pos + numKnots);
		c.Skip(numKnots);
		c.Align(4);

		float minVal[3] = {}, maxVal[3] = {};
		for (int a = 0; a < 3; a++) {
			if (isSpline[a]) {
				minVal[a] = c.F32();
				maxVal[a] = c.F32();
				track.controlPoints[a].resize(static_cast<size_t>(numItems) + 1);
			}
			else
				track.controlPoints[a] = {isStatic[a] ? c.F32() : defaultValue};
		}

		for (size_t t = 0; t <= numItems; t++) {
			for (int a = 0; a < 3; a++) {
				if (!isSpline[a])
					continue;
				float f = quantization == 0 ? c.U8() / 255.0f : c.U16() / 65535.0f;
				track.controlPoints[a][t] = minVal[a] + (maxVal[a] - minVal[a]) * f;
			}
		}
		c.Align(4);
	};

	block.tracks.resize(numTracks);
	for (size_t t = 0; t < numTracks && c.ok; t++) {
		const Mask& m = masks[t];
		TransformTrack& track = block.tracks[t];

		readVector(m.quantization & 3, m.position, 0.0f, track.translation);

		int rotQuant = (m.quantization >> 2) & 0xF;
		if (rotQuant == kThreeComp24 || rotQuant == kStraight16 || rotQuant > kUncompressed) {
			error = "track " + std::to_string(t) + " uses unsupported rotation quantization " + std::to_string(rotQuant);
			return false;
		}

		if (m.rotation & 0xF0) {
			uint16_t numItems = c.U16();
			track.rotation.degree = c.U8();
			size_t numKnots = static_cast<size_t>(numItems) + track.rotation.degree + 2;
			if (!c.Need(numKnots))
				break;
			track.rotation.knots.assign(c.data + c.pos, c.data + c.pos + numKnots);
			c.Skip(numKnots);
			c.Align(QuatAlignment(rotQuant));
			track.rotation.controlPoints.resize(static_cast<size_t>(numItems) + 1);
			for (auto& q : track.rotation.controlPoints)
				ReadQuat(c, rotQuant, q);
		}
		else if (m.rotation & 0x0F) {
			track.rotation.controlPoints.resize(1);
			ReadQuat(c, rotQuant, track.rotation.controlPoints[0]);
		}
		else
			track.rotation.controlPoints = {{0.0f, 0.0f, 0.0f, 1.0f}};
		c.Align(4);

		readVector((m.quantization >> 6) & 3, m.scale, 1.0f, track.scale);

		// The basis evaluation supports degrees up to 4.
		if (track.translation.degree > 4 || track.rotation.degree > 4 || track.scale.degree > 4) {
			error = "track " + std::to_string(t) + " has an unsupported spline degree";
			return false;
		}
	}

	if (!c.ok) {
		error = "animation data ends inside a track";
		return false;
	}
	return true;
}

void SplineAnimation::Sample(float time, std::vector<QsTransform>& tracks) const {
	tracks.resize(numTracks);
	if (blocks.empty())
		return;

	time = std::clamp(time, 0.0f, duration);
	size_t blockIdx = std::min(static_cast<size_t>(time * blockInverseDuration), blocks.size() - 1);
	float localFrame = std::max(0.0f, time - static_cast<float>(blockIdx) * blockDuration) / frameDuration;
	const Block& block = blocks[blockIdx];

	auto evalAxis = [&](const VectorTrack& v, int axis) -> float {
		const auto& cp = v.controlPoints[axis];
		if (cp.size() == 1 || v.knots.empty())
			return cp.empty() ? 0.0f : cp[0];
		int n = static_cast<int>(cp.size());
		int span = FindKnotSpan(v.degree, localFrame, n, v.knots);
		float N[5];
		BasisFunctions(span, v.degree, localFrame, v.knots, N);
		float r = 0.0f;
		for (int i = 0; i <= v.degree; i++)
			r += cp[span - i] * N[i];
		return r;
	};

	for (size_t t = 0; t < numTracks && t < block.tracks.size(); t++) {
		const TransformTrack& tr = block.tracks[t];
		QsTransform& out = tracks[t];
		for (int a = 0; a < 3; a++) {
			out.translation[a] = evalAxis(tr.translation, a);
			out.scale[a] = evalAxis(tr.scale, a);
		}

		const QuatTrack& rq = tr.rotation;
		std::array<float, 4> q = rq.controlPoints[0];
		if (!rq.knots.empty() && rq.controlPoints.size() > 1) {
			int n = static_cast<int>(rq.controlPoints.size());
			int span = FindKnotSpan(rq.degree, localFrame, n, rq.knots);
			float N[5];
			BasisFunctions(span, rq.degree, localFrame, rq.knots, N);
			q = {0.0f, 0.0f, 0.0f, 0.0f};
			for (int i = 0; i <= rq.degree; i++) {
				for (int k = 0; k < 4; k++)
					q[k] += rq.controlPoints[span - i][k] * N[i];
			}
		}
		Normalize(q);
		for (int k = 0; k < 4; k++)
			out.rotation[k] = q[k];
	}
}

static bool ReadStringArray(const Packfile& pf, uint32_t field, std::vector<std::string>& out) {
	Packfile::Array arr;
	if (!pf.ReadArray(field, kPointerSize, arr))
		return false;
	out.resize(arr.size);
	for (uint32_t i = 0; i < arr.size; i++)
		out[i] = pf.ReadStringPtr(arr.offset + i * kPointerSize);
	return true;
}

bool ProjectStrings::Load(const Packfile& pf, std::string& error) {
	auto objs = pf.FindObjects("hkbProjectStringData");
	if (objs.empty()) {
		error = "no hkbProjectStringData in file";
		return false;
	}
	if (!ReadStringArray(pf, objs[0] + layout::projectCharacterFilenames, characterFilenames)) {
		error = "hkbProjectStringData character list is corrupt";
		return false;
	}
	return true;
}

bool CharacterStrings::Load(const Packfile& pf, std::string& error) {
	auto objs = pf.FindObjects("hkbCharacterStringData");
	if (objs.empty()) {
		error = "no hkbCharacterStringData in file";
		return false;
	}
	uint32_t obj = objs[0];
	if (!pf.InRange(obj, layout::characterBehaviorFilename + kPointerSize) || !ReadStringArray(pf, obj + layout::characterAnimationNames, animationNames)) {
		error = "hkbCharacterStringData is corrupt";
		return false;
	}
	name = pf.ReadStringPtr(obj + layout::characterName);
	rigName = pf.ReadStringPtr(obj + layout::characterRigName);
	behaviorFilename = pf.ReadStringPtr(obj + layout::characterBehaviorFilename);
	return true;
}

} // namespace hkx
