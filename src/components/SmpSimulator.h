/*
 * SmpSimulator — Standalone SMP (Skinned Mesh Physics) simulator for
 * real-time cloth/hair physics preview.
 *
 * Faithfully ports the physics pipeline from hdtSMP64 (HDT-SMP Flex):
 *   - XML parsing of <bone>, <generic-constraint>, <stiffspring-constraint>,
 *     <conetwist-constraint> and their templates/defaults
 *   - Bullet3 rigid body + constraint system built from skeleton NIF transforms
 *   - Per-vertex skinning driven by physics bone transforms
 *
 * Designed to work without SKSE or the game runtime — uses nifly for NIF
 * data and TinyXML-2 for XML parsing.
 */

#pragma once

#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.h>
#include <btBulletDynamicsCommon.h>

#include <NifFile.hpp>
#include <Object3d.hpp>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinyxml2 {
class XMLElement;
}

class SmpSimulator {
public:
	SmpSimulator();
	~SmpSimulator();

	SmpSimulator(const SmpSimulator&) = delete;
	SmpSimulator& operator=(const SmpSimulator&) = delete;

	// -- Configuration --------------------------------------------------------

	bool LoadSkeleton(const std::string& skeletonNifPath);
	void PopulateBoneTransforms(nifly::NifFile& nif);
	bool LoadSmpConfig(const std::string& xmlPath);
	bool LoadShapeSkinning(nifly::NifFile& nif, nifly::NiShape* shape, const std::string& displayName);

	// -- Simulation -----------------------------------------------------------

	bool Initialise();
	void Step(float dt);
	void Reset();
	void SetGravity(const nifly::Vector3& g);
	void SetWind(const nifly::Vector3& wind);

	// -- Output ---------------------------------------------------------------

	bool HasPhysics(const std::string& displayName) const;
	const std::vector<nifly::Vector3>& GetSkinnedVerts(const std::string& displayName) const;
	std::vector<std::string> GetPhysicsShapeNames() const;

private:
	// -- Frame type enum (matches hdtSMP64) -----------------------------------
	enum FrameType { FrameInA, FrameInB, FrameInLerp, AWithXPointToB, AWithYPointToB, AWithZPointToB };

	// -- Internal types -------------------------------------------------------

	struct BoneInfo {
		std::string name;
		btTransform worldTransform = btTransform::getIdentity();
		btTransform origWorldTransform = btTransform::getIdentity();
		btTransform localToRig = btTransform::getIdentity();
		btTransform rigToLocal = btTransform::getIdentity();
		float mass = 0.0f;
		float gravityFactor = 1.0f;
		float marginMultiplier = 1.0f;
		std::shared_ptr<btCollisionShape> collisionShape;
		std::unique_ptr<btRigidBody> rigidBody;
		int parentIdx = -1;
	};

	struct BoneTemplate {
		float mass = 0.0f;
		btVector3 localInertia{0, 0, 0};
		btTransform centerOfMassTransform = btTransform::getIdentity();
		float linearDamping = 0.0f;
		float angularDamping = 0.0f;
		float friction = 0.5f;
		float rollingFriction = 0.0f;
		float restitution = 0.0f;
		float marginMultiplier = 1.0f;
		float gravityFactor = 1.0f;
		std::shared_ptr<btCollisionShape> collisionShape;
		int collisionFilter = 0;
	};

	// Matches hdtSMP64 GenericConstraintTemplate exactly.
	struct GenericConstraintTemplate {
		btVector3 linearLowerLimit{1, 1, 1};
		btVector3 linearUpperLimit{-1, -1, -1};
		btVector3 angularLowerLimit{1, 1, 1};
		btVector3 angularUpperLimit{-1, -1, -1};

		btVector3 linearStiffness{0, 0, 0};
		btVector3 angularStiffness{0, 0, 0};
		btVector3 linearDamping{0, 0, 0};
		btVector3 angularDamping{0, 0, 0};
		btVector3 linearEquilibrium{0, 0, 0};
		btVector3 angularEquilibrium{0, 0, 0};
		btVector3 linearBounce{0, 0, 0};
		btVector3 angularBounce{0, 0, 0};

		btVector3 linearNonHookeanDamping{0, 0, 0};
		btVector3 angularNonHookeanDamping{0, 0, 0};
		btVector3 linearNonHookeanStiffness{0, 0, 0};
		btVector3 angularNonHookeanStiffness{0, 0, 0};

		bool enableLinearSprings = true;
		bool enableAngularSprings = true;
		bool linearStiffnessLimited = false;
		bool angularStiffnessLimited = false;
		bool springDampingLimited = false;

		bool linearMotors = false;
		bool angularMotors = false;
		bool linearServoMotors = false;
		bool angularServoMotors = false;
		btVector3 linearTargetVelocity{0, 0, 0};
		btVector3 angularTargetVelocity{0, 0, 0};
		btVector3 linearMaxMotorForce{0, 0, 0};
		btVector3 angularMaxMotorForce{0, 0, 0};

		float motorERP = 0.9f;
		float motorCFM = 0.0f;
		float stopERP = 0.2f;
		float stopCFM = 0.0f;

		FrameType frameType = FrameInB;
		btTransform frame = btTransform::getIdentity();
		bool useLinearReferenceFrameA = false;
	};

	struct StiffSpringConstraintTemplate {
		float minDistanceFactor = 1.0f;
		float maxDistanceFactor = 1.0f;
		float stiffness = 0.0f;
		float damping = 0.0f;
		float equilibriumFactor = 0.5f;
	};

	struct ConeTwistConstraintTemplate {
		FrameType frameType = FrameInB;
		btTransform frame = btTransform::getIdentity();
		float swingSpan1 = 0.0f;
		float swingSpan2 = 0.0f;
		float twistSpan = 0.0f;
		float limitSoftness = 1.0f;
		float biasFactor = 0.3f;
		float relaxationFactor = 1.0f;
	};

	struct SkinnedShape {
		std::string displayName;

		struct SkinBone {
			int boneIdx;
			nifly::MatTransform skinToBone;
		};
		std::vector<SkinBone> skinBones;

		struct VertexSkin {
			nifly::Vector3 skinPos;
			int boneIdx[4]{-1, -1, -1, -1};
			float weight[4]{0, 0, 0, 0};
		};
		std::vector<VertexSkin> vertices;

		std::vector<nifly::Vector3> currentVerts;
	};

	// -- Internal helpers -----------------------------------------------------

	int FindBoneIdx(const std::string& name) const;
	int GetOrCreateBone(const std::string& name);
	void EnsureBoneHasRigidBody(int idx);
	btTransform GetBoneWorldTransform(int idx) const;

	bool ParseXml(const std::string& xmlPath);
	void ParseBoneTemplate(tinyxml2::XMLElement* elem, BoneTemplate& tmpl);
	void ParseBone(tinyxml2::XMLElement* elem);
	bool ParseFrameType(tinyxml2::XMLElement* elem, FrameType& ft, btTransform& frame);
	void ParseGenericConstraintTemplate(tinyxml2::XMLElement* elem, GenericConstraintTemplate& tmpl);
	void ParseGenericConstraint(tinyxml2::XMLElement* elem);
	void ParseStiffSpringConstraintTemplate(tinyxml2::XMLElement* elem, StiffSpringConstraintTemplate& tmpl);
	void ParseStiffSpringConstraint(tinyxml2::XMLElement* elem);
	void ParseConeTwistConstraintTemplate(tinyxml2::XMLElement* elem, ConeTwistConstraintTemplate& tmpl);
	void ParseConeTwistConstraint(tinyxml2::XMLElement* elem);
	std::shared_ptr<btCollisionShape> ParseShape(tinyxml2::XMLElement* elem);

	void CalcFrame(FrameType type, const btTransform& frame, const btTransform& trA, const btTransform& trB, btTransform& frameA, btTransform& frameB);

	void SkinVertices();

	static btVector3 ToBt(const nifly::Vector3& v);
	static btMatrix3x3 ToBt(const nifly::Matrix3& m);
	static btTransform ToBt(const nifly::MatTransform& t);
	static nifly::Vector3 ToNif(const btVector3& v);

	static float ReadFloat(tinyxml2::XMLElement* elem, float defaultVal = 0.0f);
	static btVector3 ReadVector3(tinyxml2::XMLElement* elem);
	static btTransform ReadTransform(tinyxml2::XMLElement* elem);
	static bool ReadBool(tinyxml2::XMLElement* elem, bool defaultVal = false);

	// -- Data -----------------------------------------------------------------

	std::unique_ptr<nifly::NifFile> skeletonNif_;
	std::unordered_map<std::string, int> boneNameToIdx_;
	std::deque<BoneInfo> bones_;

	std::unordered_map<std::string, BoneTemplate> boneTemplates_;
	std::unordered_map<std::string, GenericConstraintTemplate> genericConstraintTemplates_;
	std::unordered_map<std::string, StiffSpringConstraintTemplate> stiffSpringConstraintTemplates_;
	std::unordered_map<std::string, ConeTwistConstraintTemplate> coneTwistConstraintTemplates_;
	std::unordered_map<std::string, std::shared_ptr<btCollisionShape>> namedShapes_;

	std::vector<std::unique_ptr<btTypedConstraint>> constraints_;
	std::vector<std::shared_ptr<btCollisionShape>> shapeRefs_;

	std::vector<SkinnedShape> skinnedShapes_;

	std::unique_ptr<btDefaultCollisionConfiguration> collisionConfig_;
	std::unique_ptr<btCollisionDispatcher> dispatcher_;
	std::unique_ptr<btDbvtBroadphase> broadphase_;
	std::unique_ptr<btSequentialImpulseConstraintSolver> solver_;
	std::unique_ptr<btDiscreteDynamicsWorld> world_;

	btVector3 wind_{0, 0, 0};
	bool initialised_ = false;

	static const std::vector<nifly::Vector3> emptyVerts_;
};
