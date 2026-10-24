/*
 * Havok packfile reader for Skyrim Special Edition animation data.
 *
 * Reads the 64-bit hk_2010.2.0-r1 packfiles shipped with the game:
 *   - hkaSkeleton (rig bone names, parents and reference pose)
 *   - hkaSplineCompressedAnimation + hkaAnimationBinding (animation clips)
 *   - hkbProjectStringData / hkbCharacterStringData (behavior project and
 *     character files that link a race to its rig and animation clips)
 *
 * Other Havok versions and pointer sizes are rejected: the game itself can't
 * load them either.
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hkx {

// Bone-local transform as stored by Havok. Applied as T + R * (S * v),
// rotation is a unit quaternion (x, y, z, w).
struct QsTransform {
	float translation[3] = {0.0f, 0.0f, 0.0f};
	float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	float scale[3] = {1.0f, 1.0f, 1.0f};
};

class Packfile {
public:
	// Parses the packfile in `data`. On failure returns false and sets `error`.
	bool Load(std::vector<char> data, std::string& error);

	// Data-section offsets of every object of the given class, in file order.
	std::vector<uint32_t> FindObjects(const std::string& className) const;
	// Class name of the object starting at `objectOffset` (empty if none).
	std::string ClassOf(uint32_t objectOffset) const;

	// Target of the pointer stored at `fieldOffset` in the data section.
	// Returns false for null pointers.
	bool ResolvePointer(uint32_t fieldOffset, uint32_t& target) const;
	// String referenced by the hkStringPtr stored at `fieldOffset`.
	std::string ReadStringPtr(uint32_t fieldOffset) const;

	struct Array {
		uint32_t offset = 0; // data-section offset of the first element
		uint32_t size = 0;
	};
	// Reads the hkArray stored at `fieldOffset`. Fails if a non-empty array's
	// elements can't be located inside the data section.
	bool ReadArray(uint32_t fieldOffset, uint32_t elementSize, Array& out) const;

	bool InRange(uint32_t offset, uint32_t size) const { return offset <= dataSize && size <= dataSize - offset; }

	// Caller must check InRange(offset, sizeof(T)) first.
	template<class T>
	T Read(uint32_t offset) const {
		T value;
		std::memcpy(&value, file.data() + dataStart + offset, sizeof(T));
		return value;
	}

	const uint8_t* Bytes(uint32_t offset) const { return reinterpret_cast<const uint8_t*>(file.data()) + dataStart + offset; }
	uint32_t DataSize() const { return dataSize; }

private:
	std::vector<char> file;
	uint32_t dataStart = 0;
	uint32_t dataSize = 0;
	std::unordered_map<uint32_t, uint32_t> pointers;   // pointer field offset → target offset
	std::unordered_map<uint32_t, std::string> objects; // object offset → class name
	std::vector<uint32_t> objectOrder;
};

// hkaSkeleton referenced by the file's hkaAnimationContainer.
struct Skeleton {
	std::string name;
	std::vector<std::string> boneNames;
	std::vector<int16_t> parentIndices;
	std::vector<QsTransform> referencePose;

	bool Load(const Packfile& pf, std::string& error);
};

// First animation binding of the file's hkaAnimationContainer, decoded from
// hkaSplineCompressedAnimation into per-block B-spline tracks.
class SplineAnimation {
public:
	bool Load(const Packfile& pf, std::string& error);

	float Duration() const { return duration; }
	size_t TrackCount() const { return numTracks; }
	// Track index → skeleton bone index. Empty means track i drives bone i.
	const std::vector<int16_t>& TrackToBone() const { return trackToBone; }
	const std::string& SkeletonName() const { return skeletonName; }

	// Samples every transform track at `time` (seconds, clamped to the clip).
	void Sample(float time, std::vector<QsTransform>& tracks) const;

private:
	struct VectorTrack {
		uint8_t degree = 0;
		std::vector<uint8_t> knots;			 // empty: every axis is constant
		std::vector<float> controlPoints[3]; // one value for constant axes
	};
	struct QuatTrack {
		uint8_t degree = 0;
		std::vector<uint8_t> knots; // empty: constant rotation
		std::vector<std::array<float, 4>> controlPoints;
	};
	struct TransformTrack {
		VectorTrack translation;
		QuatTrack rotation;
		VectorTrack scale;
	};
	struct Block {
		std::vector<TransformTrack> tracks;
	};

	bool DecodeBlock(const uint8_t* data, size_t dataSize, size_t blockStart, size_t numFloatTracks, Block& block, std::string& error) const;

	float duration = 0.0f;
	size_t numTracks = 0;
	float blockDuration = 0.0f;
	float blockInverseDuration = 0.0f;
	float frameDuration = 0.0f;
	std::vector<int16_t> trackToBone;
	std::string skeletonName;
	std::vector<Block> blocks;
};

// hkbProjectStringData of a behavior project file (e.g. DefaultFemale.hkx).
struct ProjectStrings {
	std::vector<std::string> characterFilenames;

	bool Load(const Packfile& pf, std::string& error);
};

// hkbCharacterStringData of a character file. Paths are relative to the
// folder of the behavior project file.
struct CharacterStrings {
	std::string name;
	std::string rigName;
	std::string behaviorFilename;
	std::vector<std::string> animationNames;

	bool Load(const Packfile& pf, std::string& error);
};

} // namespace hkx
