/*
 * SmpSimulator — Implementation
 *
 * Standalone HDT-SMP physics simulator using Bullet3.
 * Faithfully ports hdtSMP64 physics pipeline for previewing SMP-enabled outfits.
 */

#include "SmpSimulator.h"

#include <tinyxml2.h>
#include <wx/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace nifly;
using namespace tinyxml2;

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

const std::vector<Vector3> SmpSimulator::emptyVerts_;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SmpSimulator::SmpSimulator() = default;

SmpSimulator::~SmpSimulator() {
	if (world_) {
		for (auto& c : constraints_)
			world_->removeConstraint(c.get());
		for (auto& b : bones_) {
			if (b.rigidBody)
				world_->removeRigidBody(b.rigidBody.get());
		}
	}
}

// ---------------------------------------------------------------------------
// Coordinate conversion helpers
// ---------------------------------------------------------------------------

btVector3 SmpSimulator::ToBt(const Vector3& v) {
	return btVector3(v.x, v.y, v.z);
}

btMatrix3x3 SmpSimulator::ToBt(const Matrix3& m) {
	return btMatrix3x3(m[0].x, m[0].y, m[0].z, m[1].x, m[1].y, m[1].z, m[2].x, m[2].y, m[2].z);
}

btTransform SmpSimulator::ToBt(const MatTransform& t) {
	btTransform bt;
	bt.setBasis(ToBt(t.rotation));
	bt.setOrigin(ToBt(t.translation));
	return bt;
}

Vector3 SmpSimulator::ToNif(const btVector3& v) {
	return Vector3(v.x(), v.y(), v.z());
}

// ---------------------------------------------------------------------------
// XML reading helpers
// ---------------------------------------------------------------------------

float SmpSimulator::ReadFloat(XMLElement* elem, float defaultVal) {
	if (!elem)
		return defaultVal;
	const char* text = elem->GetText();
	if (!text)
		return defaultVal;
	char* end = nullptr;
	float val = std::strtof(text, &end);
	return (end != text) ? val : defaultVal;
}

btVector3 SmpSimulator::ReadVector3(XMLElement* elem) {
	if (!elem)
		return btVector3(0, 0, 0);
	float x = 0, y = 0, z = 0;
	elem->QueryFloatAttribute("x", &x);
	elem->QueryFloatAttribute("y", &y);
	elem->QueryFloatAttribute("z", &z);
	return btVector3(x, y, z);
}

btTransform SmpSimulator::ReadTransform(XMLElement* elem) {
	btTransform t;
	t.setIdentity();
	if (!elem)
		return t;

	for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
		std::string name = child->Name();
		if (name == "basis" || name == "basis-axis-angle") {
			if (name == "basis-axis-angle") {
				float ax = 0, ay = 0, az = 1, angle = 0;
				child->QueryFloatAttribute("x", &ax);
				child->QueryFloatAttribute("y", &ay);
				child->QueryFloatAttribute("z", &az);
				child->QueryFloatAttribute("angle", &angle);
				btVector3 axis(ax, ay, az);
				if (axis.fuzzyZero())
					axis.setValue(1, 0, 0);
				else
					axis.normalize();
				t.setRotation(btQuaternion(axis, angle));
			}
			else {
				float qx = 0, qy = 0, qz = 0, qw = 1;
				child->QueryFloatAttribute("x", &qx);
				child->QueryFloatAttribute("y", &qy);
				child->QueryFloatAttribute("z", &qz);
				child->QueryFloatAttribute("w", &qw);
				btQuaternion q(qx, qy, qz, qw);
				if (q.length2() < 1e-6f)
					q = btQuaternion::getIdentity();
				else
					q.normalize();
				t.setRotation(q);
			}
		}
		else if (name == "origin") {
			t.setOrigin(ReadVector3(child));
		}
	}
	return t;
}

bool SmpSimulator::ReadBool(XMLElement* elem, bool defaultVal) {
	if (!elem)
		return defaultVal;
	const char* text = elem->GetText();
	if (!text)
		return defaultVal;
	std::string s = text;
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	if (s == "true" || s == "1")
		return true;
	if (s == "false" || s == "0")
		return false;
	return defaultVal;
}

// ---------------------------------------------------------------------------
// Skeleton loading
// ---------------------------------------------------------------------------

bool SmpSimulator::LoadSkeleton(const std::string& skeletonNifPath) {
	skeletonNif_ = std::make_unique<NifFile>();
	if (skeletonNif_->Load(skeletonNifPath) != 0) {
		wxLogError("SmpSimulator: Failed to load skeleton NIF: %s", skeletonNifPath);
		skeletonNif_.reset();
		return false;
	}

	auto nodes = skeletonNif_->GetNodes();
	boneNameToIdx_.reserve(nodes.size());

	for (auto* node : nodes) {
		if (!node)
			continue;
		std::string nodeName = skeletonNif_->GetNodeName(skeletonNif_->GetBlockID(node));
		if (nodeName.empty())
			continue;
		if (boneNameToIdx_.count(nodeName))
			continue;

		BoneInfo bi;
		bi.name = nodeName;

		MatTransform globalXf;
		if (skeletonNif_->GetNodeTransformToGlobal(nodeName, globalXf)) {
			bi.worldTransform = ToBt(globalXf);
			bi.origWorldTransform = bi.worldTransform;
		}

		auto* parent = skeletonNif_->GetParentNode(node);
		if (parent) {
			std::string parentName = skeletonNif_->GetNodeName(skeletonNif_->GetBlockID(parent));
			auto it = boneNameToIdx_.find(parentName);
			if (it != boneNameToIdx_.end())
				bi.parentIdx = it->second;
		}

		int idx = static_cast<int>(bones_.size());
		boneNameToIdx_[nodeName] = idx;
		bones_.push_back(std::move(bi));
	}

	wxLogMessage("SmpSimulator: Loaded skeleton with %zu bones from %s", bones_.size(), skeletonNifPath);
	return true;
}

void SmpSimulator::PopulateBoneTransforms(NifFile& nif) {
	auto nodes = nif.GetNodes();
	int populated = 0;

	for (auto* node : nodes) {
		if (!node)
			continue;
		std::string nodeName = nif.GetNodeName(nif.GetBlockID(node));
		if (nodeName.empty())
			continue;

		int idx = GetOrCreateBone(nodeName);
		auto& bone = bones_[idx];

		// Only populate if still at identity (not already set from skeleton)
		if (bone.worldTransform.getOrigin().isZero() && bone.worldTransform.getBasis() == btMatrix3x3::getIdentity()) {
			MatTransform globalXf;
			if (nif.GetNodeTransformToGlobal(nodeName, globalXf)) {
				bone.worldTransform = ToBt(globalXf);
				bone.origWorldTransform = bone.worldTransform;
				populated++;
			}
		}
	}

	if (populated > 0)
		wxLogMessage("SmpSimulator: Populated %d bone transforms from outfit NIF", populated);
}

// ---------------------------------------------------------------------------
// Bone lookup / creation
// ---------------------------------------------------------------------------

int SmpSimulator::FindBoneIdx(const std::string& name) const {
	auto it = boneNameToIdx_.find(name);
	return (it != boneNameToIdx_.end()) ? it->second : -1;
}

int SmpSimulator::GetOrCreateBone(const std::string& name) {
	int idx = FindBoneIdx(name);
	if (idx >= 0)
		return idx;

	BoneInfo bi;
	bi.name = name;
	idx = static_cast<int>(bones_.size());
	boneNameToIdx_[name] = idx;
	bones_.push_back(std::move(bi));
	return idx;
}

void SmpSimulator::EnsureBoneHasRigidBody(int idx) {
	auto& bone = bones_[idx];
	if (bone.rigidBody)
		return;

	// Create a kinematic rigid body with empty shape (matches hdtSMP64 default)
	auto shape = std::make_shared<btEmptyShape>();
	shapeRefs_.push_back(shape);
	bone.collisionShape = shape;

	btRigidBody::btRigidBodyConstructionInfo ci(0.0f, nullptr, shape.get());
	bone.rigidBody = std::make_unique<btRigidBody>(ci);
	bone.rigidBody->setCollisionFlags(btCollisionObject::CF_KINEMATIC_OBJECT);
	bone.rigidBody->setActivationState(DISABLE_DEACTIVATION);

	btTransform dest = bone.worldTransform * bone.localToRig;
	bone.rigidBody->setWorldTransform(dest);
	bone.rigidBody->setInterpolationWorldTransform(dest);
}

btTransform SmpSimulator::GetBoneWorldTransform(int idx) const {
	if (idx < 0 || idx >= static_cast<int>(bones_.size()))
		return btTransform::getIdentity();
	return bones_[idx].worldTransform;
}

// ---------------------------------------------------------------------------
// SMP XML parsing
// ---------------------------------------------------------------------------

bool SmpSimulator::LoadSmpConfig(const std::string& xmlPath) {
	return ParseXml(xmlPath);
}

bool SmpSimulator::ParseXml(const std::string& xmlPath) {
	XMLDocument doc;
	if (doc.LoadFile(xmlPath.c_str()) != XML_SUCCESS) {
		wxLogError("SmpSimulator: Failed to parse XML: %s — %s", xmlPath, doc.ErrorStr());
		return false;
	}

	auto* root = doc.FirstChildElement("system");
	if (!root) {
		wxLogError("SmpSimulator: No <system> root element in %s", xmlPath);
		return false;
	}

	for (auto* elem = root->FirstChildElement(); elem; elem = elem->NextSiblingElement()) {
		std::string tagName = elem->Name();

		if (tagName == "bone") {
			ParseBone(elem);
		}
		else if (tagName == "bone-default") {
			const char* clsName = elem->Attribute("name");
			const char* ext = elem->Attribute("extends");
			std::string cls = clsName ? clsName : "";
			BoneTemplate tmpl = boneTemplates_[ext ? ext : ""];
			ParseBoneTemplate(elem, tmpl);
			boneTemplates_[cls] = std::move(tmpl);
		}
		else if (tagName == "generic-constraint") {
			ParseGenericConstraint(elem);
		}
		else if (tagName == "generic-constraint-default") {
			const char* clsName = elem->Attribute("name");
			const char* ext = elem->Attribute("extends");
			std::string cls = clsName ? clsName : "";
			GenericConstraintTemplate tmpl = genericConstraintTemplates_[ext ? ext : ""];
			ParseGenericConstraintTemplate(elem, tmpl);
			genericConstraintTemplates_[cls] = std::move(tmpl);
		}
		else if (tagName == "stiffspring-constraint") {
			ParseStiffSpringConstraint(elem);
		}
		else if (tagName == "stiffspring-constraint-default") {
			const char* clsName = elem->Attribute("name");
			const char* ext = elem->Attribute("extends");
			std::string cls = clsName ? clsName : "";
			StiffSpringConstraintTemplate tmpl = stiffSpringConstraintTemplates_[ext ? ext : ""];
			ParseStiffSpringConstraintTemplate(elem, tmpl);
			stiffSpringConstraintTemplates_[cls] = std::move(tmpl);
		}
		else if (tagName == "conetwist-constraint") {
			ParseConeTwistConstraint(elem);
		}
		else if (tagName == "conetwist-constraint-default") {
			const char* clsName = elem->Attribute("name");
			const char* ext = elem->Attribute("extends");
			std::string cls = clsName ? clsName : "";
			ConeTwistConstraintTemplate tmpl = coneTwistConstraintTemplates_[ext ? ext : ""];
			ParseConeTwistConstraintTemplate(elem, tmpl);
			coneTwistConstraintTemplates_[cls] = std::move(tmpl);
		}
		else if (tagName == "per-vertex-shape" || tagName == "per-triangle-shape") {
			wxLogMessage("SmpSimulator: Note: %s shape '%s' defined in XML", tagName, elem->Attribute("name") ? elem->Attribute("name") : "?");
		}
		else if (tagName == "constraint-group") {
			for (auto* gc = elem->FirstChildElement(); gc; gc = gc->NextSiblingElement()) {
				std::string gcName = gc->Name();
				if (gcName == "generic-constraint")
					ParseGenericConstraint(gc);
				else if (gcName == "stiffspring-constraint")
					ParseStiffSpringConstraint(gc);
				else if (gcName == "conetwist-constraint")
					ParseConeTwistConstraint(gc);
				else if (gcName == "generic-constraint-default") {
					const char* n = gc->Attribute("name");
					const char* e = gc->Attribute("extends");
					GenericConstraintTemplate tmpl = genericConstraintTemplates_[e ? e : ""];
					ParseGenericConstraintTemplate(gc, tmpl);
					genericConstraintTemplates_[n ? n : ""] = std::move(tmpl);
				}
				else if (gcName == "stiffspring-constraint-default") {
					const char* n = gc->Attribute("name");
					const char* e = gc->Attribute("extends");
					StiffSpringConstraintTemplate tmpl = stiffSpringConstraintTemplates_[e ? e : ""];
					ParseStiffSpringConstraintTemplate(gc, tmpl);
					stiffSpringConstraintTemplates_[n ? n : ""] = std::move(tmpl);
				}
				else if (gcName == "conetwist-constraint-default") {
					const char* n = gc->Attribute("name");
					const char* e = gc->Attribute("extends");
					ConeTwistConstraintTemplate tmpl = coneTwistConstraintTemplates_[e ? e : ""];
					ParseConeTwistConstraintTemplate(gc, tmpl);
					coneTwistConstraintTemplates_[n ? n : ""] = std::move(tmpl);
				}
			}
		}
		else if (tagName == "shape") {
			// Named top-level shape definition (hdtSMP64 supports <shape name="...">)
			const char* shapeName = elem->Attribute("name");
			auto shape = ParseShape(elem);
			if (shape && shapeName) {
				shapeRefs_.push_back(shape);
				namedShapes_[shapeName] = shape;
			}
		}
	}

	wxLogMessage("SmpSimulator: Parsed XML %s — %zu bones with physics data, %zu constraints", xmlPath, bones_.size(), constraints_.size());
	return true;
}

// ---------------------------------------------------------------------------
// Collision shape parsing (matches hdtSMP64 readShape)
// ---------------------------------------------------------------------------

std::shared_ptr<btCollisionShape> SmpSimulator::ParseShape(XMLElement* elem) {
	if (!elem)
		return nullptr;

	const char* typeAttr = elem->Attribute("type");
	if (!typeAttr)
		return nullptr;
	std::string type = typeAttr;

	if (type == "ref") {
		// Reference to a named shape
		const char* refName = elem->Attribute("name");
		if (refName) {
			auto it = namedShapes_.find(refName);
			if (it != namedShapes_.end())
				return it->second;
			wxLogWarning("SmpSimulator: Unknown shape ref '%s'", refName);
		}
		return nullptr;
	}
	if (type == "sphere") {
		float radius = 0;
		for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
			std::string n = child->Name();
			if (n == "radius")
				radius = ReadFloat(child);
		}
		return std::make_shared<btSphereShape>(radius);
	}
	if (type == "box") {
		btVector3 halfExtend(0, 0, 0);
		float margin = 0;
		for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
			std::string n = child->Name();
			if (n == "halfExtend")
				halfExtend = ReadVector3(child);
			else if (n == "margin")
				margin = ReadFloat(child);
		}
		auto shape = std::make_shared<btBoxShape>(halfExtend);
		if (margin > 0)
			shape->setMargin(margin);
		return shape;
	}
	if (type == "capsule") {
		float radius = 0, height = 0;
		for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
			std::string n = child->Name();
			if (n == "radius")
				radius = ReadFloat(child);
			else if (n == "height")
				height = ReadFloat(child);
		}
		return std::make_shared<btCapsuleShape>(radius, height);
	}
	if (type == "cylinder") {
		float radius = 0, height = 0;
		float margin = 0;
		for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
			std::string n = child->Name();
			if (n == "radius")
				radius = ReadFloat(child);
			else if (n == "height")
				height = ReadFloat(child);
			else if (n == "margin")
				margin = ReadFloat(child);
		}
		if (radius >= 0 && height >= 0) {
			auto shape = std::make_shared<btCylinderShape>(btVector3(radius, height, radius));
			if (margin > 0)
				shape->setMargin(margin);
			return shape;
		}
		return nullptr;
	}
	if (type == "hull") {
		auto shape = std::make_shared<btConvexHullShape>();
		float margin = 0;
		for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
			std::string n = child->Name();
			if (n == "point")
				shape->addPoint(ReadVector3(child), false);
			else if (n == "margin")
				margin = ReadFloat(child);
		}
		shape->recalcLocalAabb();
		if (shape->getNumPoints() == 0)
			return nullptr;
		if (margin > 0)
			shape->setMargin(margin);
		return shape;
	}
	if (type == "compound") {
		auto compound = std::make_shared<btCompoundShape>();
		for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
			if (std::string(child->Name()) == "child") {
				btTransform tr;
				tr.setIdentity();
				std::shared_ptr<btCollisionShape> childShape;
				for (auto* cc = child->FirstChildElement(); cc; cc = cc->NextSiblingElement()) {
					std::string ccName = cc->Name();
					if (ccName == "transform")
						tr = ReadTransform(cc);
					else if (ccName == "shape")
						childShape = ParseShape(cc);
				}
				if (childShape) {
					compound->addChildShape(tr, childShape.get());
					shapeRefs_.push_back(childShape);
				}
			}
		}
		return compound->getNumChildShapes() ? compound : nullptr;
	}

	wxLogWarning("SmpSimulator: Unknown shape type '%s'", type);
	return nullptr;
}

// ---------------------------------------------------------------------------
// Bone template parsing
// ---------------------------------------------------------------------------

void SmpSimulator::ParseBoneTemplate(XMLElement* elem, BoneTemplate& tmpl) {
	for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
		std::string name = child->Name();
		if (name == "mass")
			tmpl.mass = ReadFloat(child);
		else if (name == "inertia")
			tmpl.localInertia = ReadVector3(child);
		else if (name == "centerOfMassTransform")
			tmpl.centerOfMassTransform = ReadTransform(child);
		else if (name == "linearDamping")
			tmpl.linearDamping = ReadFloat(child);
		else if (name == "angularDamping")
			tmpl.angularDamping = ReadFloat(child);
		else if (name == "friction")
			tmpl.friction = ReadFloat(child);
		else if (name == "rollingFriction")
			tmpl.rollingFriction = ReadFloat(child);
		else if (name == "restitution")
			tmpl.restitution = ReadFloat(child);
		else if (name == "margin-multiplier")
			tmpl.marginMultiplier = ReadFloat(child, 1.0f);
		else if (name == "gravity-factor")
			tmpl.gravityFactor = std::clamp(ReadFloat(child, 1.0f), 0.0f, 1.0f);
		else if (name == "shape") {
			auto shape = ParseShape(child);
			if (shape) {
				shapeRefs_.push_back(shape);
				tmpl.collisionShape = shape;
			}
		}
		else if (name == "collision-filter")
			tmpl.collisionFilter = static_cast<int>(ReadFloat(child));
		// can-collide-with-bone, no-collide-with-bone: skip for preview
	}
}

// ---------------------------------------------------------------------------
// Bone parsing (matches hdtSMP64 createBoneFromNodeName)
// ---------------------------------------------------------------------------

void SmpSimulator::ParseBone(XMLElement* elem) {
	const char* nameAttr = elem->Attribute("name");
	if (!nameAttr) {
		wxLogWarning("SmpSimulator: <bone> without name attribute, skipped");
		return;
	}
	std::string boneName = nameAttr;

	int boneIdx = GetOrCreateBone(boneName);
	BoneInfo& bi = bones_[boneIdx];

	if (bi.rigidBody) {
		wxLogMessage("SmpSimulator: Bone '%s' already has rigid body, skipping duplicate", boneName);
		return;
	}

	// Get and parse template
	const char* tmplAttr = elem->Attribute("template");
	BoneTemplate tmpl = boneTemplates_[tmplAttr ? tmplAttr : ""];
	ParseBoneTemplate(elem, tmpl);

	// Apply to bone info (matches hdtSMP64 createBoneFromNodeName)
	bi.mass = tmpl.mass;
	bi.gravityFactor = tmpl.gravityFactor;
	bi.marginMultiplier = tmpl.marginMultiplier;
	bi.localToRig = tmpl.centerOfMassTransform;
	bi.rigToLocal = tmpl.centerOfMassTransform.inverse();

	// Collision shape (matches hdtSMP64: BoneTemplate emptyShape if no shape set)
	btCollisionShape* colShape = nullptr;
	if (tmpl.collisionShape) {
		bi.collisionShape = tmpl.collisionShape;
		colShape = bi.collisionShape.get();
	}
	else {
		auto empty = std::make_shared<btEmptyShape>();
		shapeRefs_.push_back(empty);
		bi.collisionShape = empty;
		colShape = empty.get();
	}

	// Compute local inertia
	btVector3 localInertia = tmpl.localInertia;
	if (tmpl.mass > 0.0f && localInertia.isZero())
		colShape->calculateLocalInertia(tmpl.mass, localInertia);

	// Create rigid body (no motion state, matches hdtSMP64)
	btRigidBody::btRigidBodyConstructionInfo rbInfo(tmpl.mass, nullptr, colShape, localInertia);
	rbInfo.m_linearDamping = tmpl.linearDamping;
	rbInfo.m_angularDamping = tmpl.angularDamping;
	rbInfo.m_friction = tmpl.friction;
	rbInfo.m_rollingFriction = tmpl.rollingFriction;
	rbInfo.m_restitution = tmpl.restitution;

	bi.rigidBody = std::make_unique<btRigidBody>(rbInfo);

	// Set kinematic/dynamic flags (matches hdtSMP64 SkyrimBone constructor)
	if (tmpl.mass > 0.0f)
		bi.rigidBody->setCollisionFlags(0);
	else
		bi.rigidBody->setCollisionFlags(btCollisionObject::CF_KINEMATIC_OBJECT);

	bi.rigidBody->setActivationState(DISABLE_DEACTIVATION);

	// Set initial transform (matches hdtSMP64 readTransform(RESET_PHYSICS))
	btTransform dest = bi.worldTransform * bi.localToRig;
	bi.rigidBody->setWorldTransform(dest);
	bi.rigidBody->setInterpolationWorldTransform(dest);
	bi.rigidBody->setLinearVelocity(btVector3(0, 0, 0));
	bi.rigidBody->setAngularVelocity(btVector3(0, 0, 0));
	bi.rigidBody->setInterpolationLinearVelocity(btVector3(0, 0, 0));
	bi.rigidBody->setInterpolationAngularVelocity(btVector3(0, 0, 0));
	bi.rigidBody->updateInertiaTensor();

	wxLogMessage("SmpSimulator: Bone '%s' mass=%.3f kinematic=%d", boneName, tmpl.mass, tmpl.mass <= 0.0f ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Frame type parsing (matches hdtSMP64 parseFrameType)
// ---------------------------------------------------------------------------

bool SmpSimulator::ParseFrameType(XMLElement* elem, FrameType& ft, btTransform& frame) {
	std::string name = elem->Name();
	if (name == "frameInA") {
		ft = FrameInA;
		frame = ReadTransform(elem);
		return true;
	}
	if (name == "frameInB") {
		ft = FrameInB;
		frame = ReadTransform(elem);
		return true;
	}
	if (name == "frameInLerp") {
		ft = FrameInLerp;
		frame.setIdentity();
		auto* tlElem = elem->FirstChildElement("translationLerp");
		auto* rlElem = elem->FirstChildElement("rotationLerp");
		if (tlElem)
			frame.getOrigin().setX(ReadFloat(tlElem, 0.5f));
		if (rlElem)
			frame.getOrigin().setY(ReadFloat(rlElem, 0.5f));
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// CalcFrame (matches hdtSMP64::SkyrimSystemCreator::calcFrame)
// ---------------------------------------------------------------------------

static btQuaternion rotFromAtoB(const btVector3& a, const btVector3& b) {
	auto axis = a.cross(b);
	if (axis.fuzzyZero())
		return btQuaternion::getIdentity();
	float sinA = axis.length();
	float cosA = a.dot(b);
	float angle = btAtan2(cosA, sinA);
	return btQuaternion(axis, angle);
}

void SmpSimulator::CalcFrame(FrameType type, const btTransform& frame, const btTransform& trA, const btTransform& trB, btTransform& frameA, btTransform& frameB) {
	switch (type) {
		case FrameInA:
			frameA = frame;
			frameB = trB.inverse() * trA * frame;
			break;
		case FrameInB:
			frameB = frame;
			frameA = trA.inverse() * trB * frame;
			break;
		case FrameInLerp: {
			btVector3 trans = trA.getOrigin().lerp(trB.getOrigin(), frame.getOrigin().x());
			btQuaternion qA, qB;
			trA.getBasis().getRotation(qA);
			trB.getBasis().getRotation(qB);
			btQuaternion rot = qA.slerp(qB, frame.getOrigin().y());
			btTransform frameInWorld;
			frameInWorld.setRotation(rot);
			frameInWorld.setOrigin(trans);
			frameA = trA.inverse() * frameInWorld;
			frameB = trB.inverse() * frameInWorld;
			break;
		}
		case AWithXPointToB: {
			btTransform frameInWorld = trA;
			auto old = trA.getBasis().getColumn(0).normalized();
			auto a2b = (trB.getOrigin() - trA.getOrigin()).normalized();
			auto q = rotFromAtoB(old, a2b);
			frameInWorld.getBasis() *= btMatrix3x3(q);
			frameA = trA.inverse() * frameInWorld;
			frameB = trB.inverse() * frameInWorld;
			break;
		}
		case AWithYPointToB: {
			btTransform frameInWorld = trA;
			auto old = trA.getBasis().getColumn(1).normalized();
			auto a2b = (trB.getOrigin() - trA.getOrigin()).normalized();
			auto q = rotFromAtoB(old, a2b);
			frameInWorld.getBasis() *= btMatrix3x3(q);
			frameA = trA.inverse() * frameInWorld;
			frameB = trB.inverse() * frameInWorld;
			break;
		}
		case AWithZPointToB: {
			btTransform frameInWorld = trA;
			auto old = trA.getBasis().getColumn(2).normalized();
			auto a2b = (trB.getOrigin() - trA.getOrigin()).normalized();
			auto q = rotFromAtoB(old, a2b);
			frameInWorld.getBasis() *= btMatrix3x3(q);
			frameA = trA.inverse() * frameInWorld;
			frameB = trB.inverse() * frameInWorld;
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Generic constraint template parsing (matches hdtSMP64)
// ---------------------------------------------------------------------------

void SmpSimulator::ParseGenericConstraintTemplate(XMLElement* elem, GenericConstraintTemplate& tmpl) {
	for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
		std::string name = child->Name();
		if (ParseFrameType(child, tmpl.frameType, tmpl.frame))
			continue;
		if (name == "enableLinearSprings")
			tmpl.enableLinearSprings = ReadBool(child);
		else if (name == "enableAngularSprings")
			tmpl.enableAngularSprings = ReadBool(child);
		else if (name == "linearStiffnessLimited")
			tmpl.linearStiffnessLimited = ReadBool(child);
		else if (name == "angularStiffnessLimited")
			tmpl.angularStiffnessLimited = ReadBool(child);
		else if (name == "springDampingLimited")
			tmpl.springDampingLimited = ReadBool(child);
		else if (name == "linearNonHookeanDamping")
			tmpl.linearNonHookeanDamping = ReadVector3(child);
		else if (name == "angularNonHookeanDamping")
			tmpl.angularNonHookeanDamping = ReadVector3(child);
		else if (name == "linearNonHookeanStiffness")
			tmpl.linearNonHookeanStiffness = ReadVector3(child);
		else if (name == "angularNonHookeanStiffness")
			tmpl.angularNonHookeanStiffness = ReadVector3(child);
		else if (name == "linearMotors")
			tmpl.linearMotors = ReadBool(child);
		else if (name == "angularMotors")
			tmpl.angularMotors = ReadBool(child);
		else if (name == "linearServoMotors")
			tmpl.linearServoMotors = ReadBool(child);
		else if (name == "angularServoMotors")
			tmpl.angularServoMotors = ReadBool(child);
		else if (name == "linearTargetVelocity")
			tmpl.linearTargetVelocity = ReadVector3(child);
		else if (name == "angularTargetVelocity")
			tmpl.angularTargetVelocity = ReadVector3(child);
		else if (name == "linearMaxMotorForce")
			tmpl.linearMaxMotorForce = ReadVector3(child);
		else if (name == "angularMaxMotorForce")
			tmpl.angularMaxMotorForce = ReadVector3(child);
		else if (name == "stopERP")
			tmpl.stopERP = ReadFloat(child);
		else if (name == "stopCFM")
			tmpl.stopCFM = ReadFloat(child);
		else if (name == "motorERP")
			tmpl.motorERP = ReadFloat(child);
		else if (name == "motorCFM")
			tmpl.motorCFM = ReadFloat(child);
		else if (name == "useLinearReferenceFrameA")
			tmpl.useLinearReferenceFrameA = ReadBool(child);
		else if (name == "linearLowerLimit")
			tmpl.linearLowerLimit = ReadVector3(child);
		else if (name == "linearUpperLimit")
			tmpl.linearUpperLimit = ReadVector3(child);
		else if (name == "angularLowerLimit")
			tmpl.angularLowerLimit = ReadVector3(child);
		else if (name == "angularUpperLimit")
			tmpl.angularUpperLimit = ReadVector3(child);
		else if (name == "linearStiffness")
			tmpl.linearStiffness = ReadVector3(child);
		else if (name == "angularStiffness")
			tmpl.angularStiffness = ReadVector3(child);
		else if (name == "linearDamping")
			tmpl.linearDamping = ReadVector3(child);
		else if (name == "angularDamping")
			tmpl.angularDamping = ReadVector3(child);
		else if (name == "linearEquilibrium")
			tmpl.linearEquilibrium = ReadVector3(child);
		else if (name == "angularEquilibrium")
			tmpl.angularEquilibrium = ReadVector3(child);
		else if (name == "linearBounce")
			tmpl.linearBounce = ReadVector3(child);
		else if (name == "angularBounce")
			tmpl.angularBounce = ReadVector3(child);
	}
}

// ---------------------------------------------------------------------------
// Generic constraint creation (matches hdtSMP64 readGenericConstraint)
// ---------------------------------------------------------------------------

void SmpSimulator::ParseGenericConstraint(XMLElement* elem) {
	const char* bodyAName = elem->Attribute("bodyA");
	const char* bodyBName = elem->Attribute("bodyB");
	if (!bodyAName || !bodyBName) {
		wxLogWarning("SmpSimulator: generic-constraint missing bodyA/bodyB");
		return;
	}

	// Find or create bones (matches hdtSMP64 findBones + createBoneFromNodeName)
	int idxA = GetOrCreateBone(bodyAName);
	int idxB = GetOrCreateBone(bodyBName);

	if (idxA == idxB) {
		wxLogWarning("SmpSimulator: constraint between same bone '%s', skipped", bodyAName);
		return;
	}

	EnsureBoneHasRigidBody(idxA);
	EnsureBoneHasRigidBody(idxB);

	auto& boneA = bones_[idxA];
	auto& boneB = bones_[idxB];

	// Skip constraints between two kinematic objects (matches hdtSMP64)
	if (boneA.rigidBody->isKinematicObject() && boneB.rigidBody->isKinematicObject()) {
		wxLogMessage("SmpSimulator: Skipping constraint between two kinematic bones '%s' <-> '%s'", bodyAName, bodyBName);
		return;
	}

	// Get template
	const char* tmplAttr = elem->Attribute("template");
	GenericConstraintTemplate tmpl = genericConstraintTemplates_[tmplAttr ? tmplAttr : ""];
	ParseGenericConstraintTemplate(elem, tmpl);

	// Calculate constraint frames in bone-local space
	btTransform trA = boneA.worldTransform;
	btTransform trB = boneB.worldTransform;
	btTransform frameA, frameB;
	CalcFrame(tmpl.frameType, tmpl.frame, trA, trB, frameA, frameB);

	// Create btGeneric6DofSpring2Constraint
	// Matches hdtSMP64 Generic6DofConstraint constructor:
	//   - Constructed with identity frames
	//   - Then fa = a->m_rigToLocal * frameInA, fb = b->m_rigToLocal * frameInB
	//   - setFrames(fa, fb)
	std::unique_ptr<btGeneric6DofSpring2Constraint> constraint;
	btTransform fA, fB;

	if (tmpl.useLinearReferenceFrameA) {
		// hdtSMP64 swaps A/B when useLinearReferenceFrameA is true
		fB = boneB.rigToLocal * frameB;
		fA = boneA.rigToLocal * frameA;
		constraint = std::make_unique<btGeneric6DofSpring2Constraint>(*boneB.rigidBody, *boneA.rigidBody, fB, fA, RO_XYZ);
	}
	else {
		fA = boneA.rigToLocal * frameA;
		fB = boneB.rigToLocal * frameB;
		constraint = std::make_unique<btGeneric6DofSpring2Constraint>(*boneA.rigidBody, *boneB.rigidBody, fA, fB, RO_XYZ);
	}

	// Set limits
	constraint->setLinearLowerLimit(tmpl.linearLowerLimit);
	constraint->setLinearUpperLimit(tmpl.linearUpperLimit);
	constraint->setAngularLowerLimit(tmpl.angularLowerLimit);
	constraint->setAngularUpperLimit(tmpl.angularUpperLimit);

	// Set spring parameters (matches hdtSMP64 readGenericConstraint exactly)
	for (int i = 0; i < 3; ++i) {
		constraint->setStiffness(i, tmpl.linearStiffness[i], tmpl.linearStiffnessLimited);
		constraint->setStiffness(i + 3, tmpl.angularStiffness[i], tmpl.angularStiffnessLimited);
		constraint->setDamping(i, tmpl.linearDamping[i], tmpl.springDampingLimited);
		constraint->setDamping(i + 3, tmpl.angularDamping[i], tmpl.springDampingLimited);

		constraint->setEquilibriumPoint(i, tmpl.linearEquilibrium[i]);
		constraint->setEquilibriumPoint(i + 3, tmpl.angularEquilibrium[i]);

		constraint->enableSpring(i, tmpl.enableLinearSprings);
		constraint->enableSpring(i + 3, tmpl.enableAngularSprings);

		constraint->enableMotor(i, tmpl.linearMotors);
		constraint->enableMotor(i + 3, tmpl.angularMotors);
		constraint->setServo(i, tmpl.linearServoMotors);
		constraint->setServo(i + 3, tmpl.angularServoMotors);
		constraint->setServoTarget(i, tmpl.linearEquilibrium[i]);
		constraint->setServoTarget(i + 3, tmpl.angularEquilibrium[i]);
		constraint->setTargetVelocity(i, tmpl.linearTargetVelocity[i]);
		constraint->setTargetVelocity(i + 3, tmpl.angularTargetVelocity[i]);
		constraint->setMaxMotorForce(i, tmpl.linearMaxMotorForce[i]);
		constraint->setMaxMotorForce(i + 3, tmpl.angularMaxMotorForce[i]);

		constraint->setParam(BT_CONSTRAINT_ERP, tmpl.motorERP, i);
		constraint->setParam(BT_CONSTRAINT_CFM, tmpl.motorCFM, i);
		constraint->setParam(BT_CONSTRAINT_STOP_ERP, tmpl.stopERP, i);
		constraint->setParam(BT_CONSTRAINT_STOP_CFM, tmpl.stopCFM, i);

		auto rotMotor = constraint->getRotationalLimitMotor(i);
		if (rotMotor) {
			rotMotor->m_motorERP = tmpl.motorERP;
			rotMotor->m_motorCFM = tmpl.motorCFM;
			rotMotor->m_stopERP = tmpl.stopERP;
			rotMotor->m_stopCFM = tmpl.stopCFM;
		}
	}

	// Bounce
	constraint->getTranslationalLimitMotor()->m_bounce = tmpl.linearBounce;
	constraint->getRotationalLimitMotor(0)->m_bounce = tmpl.angularBounce[0];
	constraint->getRotationalLimitMotor(1)->m_bounce = tmpl.angularBounce[1];
	constraint->getRotationalLimitMotor(2)->m_bounce = tmpl.angularBounce[2];

	constraints_.push_back(std::move(constraint));
	wxLogMessage("SmpSimulator: Generic constraint '%s' <-> '%s'", bodyAName, bodyBName);
}

// ---------------------------------------------------------------------------
// Stiff spring constraint (approximation using btGeneric6DofSpring2Constraint)
// hdtSMP64 uses a custom btTypedConstraint which we cannot port directly.
// We approximate with a 6DOF constraint configured as a distance spring.
// ---------------------------------------------------------------------------

void SmpSimulator::ParseStiffSpringConstraintTemplate(XMLElement* elem, StiffSpringConstraintTemplate& tmpl) {
	for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
		std::string name = child->Name();
		if (name == "minDistanceFactor")
			tmpl.minDistanceFactor = std::max(ReadFloat(child, 1.0f), 0.0f);
		else if (name == "maxDistanceFactor")
			tmpl.maxDistanceFactor = std::max(ReadFloat(child, 1.0f), 0.0f);
		else if (name == "stiffness")
			tmpl.stiffness = std::max(ReadFloat(child), 0.0f);
		else if (name == "damping")
			tmpl.damping = std::max(ReadFloat(child), 0.0f);
		else if (name == "equilibrium")
			tmpl.equilibriumFactor = std::clamp(ReadFloat(child, 0.5f), 0.0f, 1.0f);
	}
}

void SmpSimulator::ParseStiffSpringConstraint(XMLElement* elem) {
	const char* bodyAName = elem->Attribute("bodyA");
	const char* bodyBName = elem->Attribute("bodyB");
	if (!bodyAName || !bodyBName) {
		wxLogWarning("SmpSimulator: stiffspring-constraint missing bodyA/bodyB");
		return;
	}

	int idxA = GetOrCreateBone(bodyAName);
	int idxB = GetOrCreateBone(bodyBName);
	if (idxA == idxB)
		return;

	EnsureBoneHasRigidBody(idxA);
	EnsureBoneHasRigidBody(idxB);

	auto& boneA = bones_[idxA];
	auto& boneB = bones_[idxB];
	if (boneA.rigidBody->isKinematicObject() && boneB.rigidBody->isKinematicObject())
		return;

	const char* tmplAttr = elem->Attribute("template");
	StiffSpringConstraintTemplate tmpl = stiffSpringConstraintTemplates_[tmplAttr ? tmplAttr : ""];
	ParseStiffSpringConstraintTemplate(elem, tmpl);

	// Compute rest distance (in bone-world space) matching hdtSMP64
	btVector3 posA = boneA.worldTransform.getOrigin();
	btVector3 posB = boneB.worldTransform.getOrigin();
	float restDist = (posA - posB).length();
	float minDist = restDist * tmpl.minDistanceFactor;
	float maxDist = restDist * tmpl.maxDistanceFactor;
	float equil = minDist * tmpl.equilibriumFactor + maxDist * (1.0f - tmpl.equilibriumFactor);

	// Approximate as a btGeneric6DofSpring2Constraint with linear spring
	// Frame: identity in each body's COM space, pointing along the line between bones
	btVector3 a2b = posB - posA;
	float dist = a2b.length();
	btVector3 dir = dist > 1e-6f ? a2b / dist : btVector3(0, 0, 1);

	// Build frame where X-axis points from A to B
	btVector3 up(0, 1, 0);
	if (std::abs(dir.dot(up)) > 0.99f)
		up = btVector3(1, 0, 0);
	btVector3 right = dir.cross(up).normalized();
	up = right.cross(dir).normalized();
	btMatrix3x3 basis(dir.x(), up.x(), right.x(), dir.y(), up.y(), right.y(), dir.z(), up.z(), right.z());

	btTransform fA;
	fA.setIdentity();
	fA.setBasis(basis);
	fA = boneA.rigToLocal * fA;

	btTransform fB;
	fB.setIdentity();
	fB.setBasis(basis);
	fB = boneB.rigToLocal * fB;

	auto constraint = std::make_unique<btGeneric6DofSpring2Constraint>(*boneA.rigidBody, *boneB.rigidBody, fA, fB, RO_XYZ);

	// Lock angular DOFs, spring on X (distance axis)
	constraint->setLinearLowerLimit(btVector3(minDist - equil, 0, 0));
	constraint->setLinearUpperLimit(btVector3(maxDist - equil, 0, 0));
	constraint->setAngularLowerLimit(btVector3(1, 1, 1));
	constraint->setAngularUpperLimit(btVector3(-1, -1, -1));

	constraint->enableSpring(0, tmpl.stiffness > 0.0f);
	constraint->setStiffness(0, tmpl.stiffness);
	constraint->setDamping(0, tmpl.damping);
	constraint->setEquilibriumPoint(0, 0.0f);

	// Lock Y/Z translation
	for (int i = 1; i < 3; i++) {
		constraint->enableSpring(i, false);
	}

	constraints_.push_back(std::move(constraint));
	wxLogMessage("SmpSimulator: StiffSpring '%s' <-> '%s' (dist=%.3f)", bodyAName, bodyBName, restDist);
}

// ---------------------------------------------------------------------------
// Cone twist constraint (matches hdtSMP64 readConeTwistConstraint)
// ---------------------------------------------------------------------------

void SmpSimulator::ParseConeTwistConstraintTemplate(XMLElement* elem, ConeTwistConstraintTemplate& tmpl) {
	for (auto* child = elem->FirstChildElement(); child; child = child->NextSiblingElement()) {
		std::string name = child->Name();
		if (ParseFrameType(child, tmpl.frameType, tmpl.frame))
			continue;
		if (name == "swingSpan1" || name == "coneLimit" || name == "limitZ")
			tmpl.swingSpan1 = std::max(ReadFloat(child), 0.0f);
		else if (name == "swingSpan2" || name == "planeLimit" || name == "limitY")
			tmpl.swingSpan2 = std::max(ReadFloat(child), 0.0f);
		else if (name == "twistSpan" || name == "twistLimit" || name == "limitX")
			tmpl.twistSpan = std::max(ReadFloat(child), 0.0f);
		else if (name == "limitSoftness")
			tmpl.limitSoftness = std::clamp(ReadFloat(child, 1.0f), 0.0f, 1.0f);
		else if (name == "biasFactor")
			tmpl.biasFactor = std::clamp(ReadFloat(child, 0.3f), 0.0f, 1.0f);
		else if (name == "relaxationFactor")
			tmpl.relaxationFactor = std::clamp(ReadFloat(child, 1.0f), 0.0f, 1.0f);
	}
}

void SmpSimulator::ParseConeTwistConstraint(XMLElement* elem) {
	const char* bodyAName = elem->Attribute("bodyA");
	const char* bodyBName = elem->Attribute("bodyB");
	if (!bodyAName || !bodyBName) {
		wxLogWarning("SmpSimulator: conetwist-constraint missing bodyA/bodyB");
		return;
	}

	int idxA = GetOrCreateBone(bodyAName);
	int idxB = GetOrCreateBone(bodyBName);
	if (idxA == idxB)
		return;

	EnsureBoneHasRigidBody(idxA);
	EnsureBoneHasRigidBody(idxB);

	auto& boneA = bones_[idxA];
	auto& boneB = bones_[idxB];
	if (boneA.rigidBody->isKinematicObject() && boneB.rigidBody->isKinematicObject())
		return;

	const char* tmplAttr = elem->Attribute("template");
	ConeTwistConstraintTemplate tmpl = coneTwistConstraintTemplates_[tmplAttr ? tmplAttr : ""];
	ParseConeTwistConstraintTemplate(elem, tmpl);

	btTransform trA = boneA.worldTransform;
	btTransform trB = boneB.worldTransform;
	btTransform frameA, frameB;
	CalcFrame(tmpl.frameType, tmpl.frame, trA, trB, frameA, frameB);

	// Convert to rigid body COM space (matches hdtSMP64)
	btTransform ctFrameA = boneA.rigToLocal * frameA;
	btTransform ctFrameB = boneB.rigToLocal * frameB;

	auto constraint = std::make_unique<btConeTwistConstraint>(*boneA.rigidBody, *boneB.rigidBody, ctFrameA, ctFrameB);

	constraint->setLimit(tmpl.swingSpan1, tmpl.swingSpan2, tmpl.twistSpan, tmpl.limitSoftness, tmpl.biasFactor, tmpl.relaxationFactor);

	constraints_.push_back(std::move(constraint));
	wxLogMessage("SmpSimulator: ConeTwist '%s' <-> '%s'", bodyAName, bodyBName);
}

// ---------------------------------------------------------------------------
// Shape skinning loading
// ---------------------------------------------------------------------------

bool SmpSimulator::LoadShapeSkinning(NifFile& nif, NiShape* shape, const std::string& displayName) {
	if (!shape)
		return false;

	SkinnedShape ss;
	ss.displayName = displayName;

	std::vector<std::string> boneNames;
	nif.GetShapeBoneList(shape, boneNames);
	if (boneNames.empty()) {
		wxLogMessage("SmpSimulator: Shape '%s' has no bones, skipping", displayName);
		return false;
	}

	MatTransform globalToSkin;
	nif.GetShapeTransformGlobalToSkin(shape, globalToSkin);

	ss.skinBones.resize(boneNames.size());
	for (size_t bi = 0; bi < boneNames.size(); bi++) {
		int boneIdx = GetOrCreateBone(boneNames[bi]);
		ss.skinBones[bi].boneIdx = boneIdx;

		// Populate world transform from outfit NIF if not already set
		BoneInfo& bone = bones_[boneIdx];
		if (bone.worldTransform.getOrigin().isZero() && bone.worldTransform.getBasis() == btMatrix3x3::getIdentity()) {
			MatTransform globalXf;
			if (nif.GetNodeTransformToGlobal(boneNames[bi], globalXf)) {
				bone.worldTransform = ToBt(globalXf);
				bone.origWorldTransform = bone.worldTransform;
			}
		}

		MatTransform skinToBone;
		nif.GetShapeTransformSkinToBone(shape, boneNames[bi], skinToBone);
		ss.skinBones[bi].skinToBone = skinToBone;
	}

	std::vector<Vector3> verts;
	nif.GetVertsForShape(shape, verts);
	if (verts.empty())
		return false;

	ss.vertices.resize(verts.size());
	ss.currentVerts.resize(verts.size());

	for (size_t bi = 0; bi < boneNames.size(); bi++) {
		std::unordered_map<uint16_t, float> weights;
		nif.GetShapeBoneWeights(shape, static_cast<int>(bi), weights);

		for (auto& [vertIdx, weight] : weights) {
			if (vertIdx >= ss.vertices.size())
				continue;
			auto& v = ss.vertices[vertIdx];
			for (int k = 0; k < 4; k++) {
				if (v.boneIdx[k] < 0 || v.weight[k] <= 0.0f) {
					v.boneIdx[k] = static_cast<int>(bi);
					v.weight[k] = weight;
					break;
				}
			}
		}
	}

	for (size_t i = 0; i < verts.size(); i++) {
		ss.vertices[i].skinPos = verts[i];
		ss.currentVerts[i] = verts[i];
	}

	// Normalise weights
	for (auto& v : ss.vertices) {
		float total = 0.0f;
		for (int k = 0; k < 4; k++)
			total += v.weight[k];
		if (total > 0.0f) {
			for (int k = 0; k < 4; k++)
				v.weight[k] /= total;
		}
	}

	skinnedShapes_.push_back(std::move(ss));
	wxLogMessage("SmpSimulator: Loaded skinning for '%s' (%zu verts, %zu bones)", displayName, verts.size(), boneNames.size());
	return true;
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool SmpSimulator::Initialise() {
	if (initialised_)
		return true;

	collisionConfig_ = std::make_unique<btDefaultCollisionConfiguration>();
	dispatcher_ = std::make_unique<btCollisionDispatcher>(collisionConfig_.get());
	broadphase_ = std::make_unique<btDbvtBroadphase>();
	solver_ = std::make_unique<btSequentialImpulseConstraintSolver>();
	world_ = std::make_unique<btDiscreteDynamicsWorld>(dispatcher_.get(), broadphase_.get(), solver_.get(), collisionConfig_.get());

	// Gravity in NIF units (matches hdtSMP64: scaleSkyrim = 1/0.01425 ≈ 70.175)
	static constexpr float scaleSkyrim = 1.0f / 0.01425f;
	world_->setGravity(btVector3(0, 0, -9.8f * scaleSkyrim));

	// Add rigid bodies (matching hdtSMP64 addSkinnedMeshSystem)
	for (auto& bone : bones_) {
		if (bone.rigidBody) {
			bone.rigidBody->setActivationState(DISABLE_DEACTIVATION);
			// Bones are added with (0,0) collision mask — no bone-bone collision
			world_->addRigidBody(bone.rigidBody.get(), 0, 0);

			// Per-bone gravity (matches hdtSMP64 applyGravity)
			if (!bone.rigidBody->isStaticOrKinematicObject())
				bone.rigidBody->setGravity(world_->getGravity() * bone.gravityFactor);
		}
	}

	// Add constraints (disableCollisionsBetweenLinkedBodies=true matches hdtSMP64)
	for (auto& c : constraints_)
		world_->addConstraint(c.get(), true);

	initialised_ = true;
	wxLogMessage("SmpSimulator: Initialised — %zu bones, %zu constraints, %zu skinned shapes", bones_.size(), constraints_.size(), skinnedShapes_.size());
	return true;
}

// ---------------------------------------------------------------------------
// Simulation stepping (matches hdtSMP64 SkinnedMeshWorld::stepSimulation)
// ---------------------------------------------------------------------------

void SmpSimulator::Step(float dt) {
	if (!initialised_ || !world_)
		return;

	// No readTransform needed for kinematic bones in static preview —
	// they remain at their initial transform. For kinematic bones, Bullet
	// handles the transform through the CF_KINEMATIC_OBJECT flag.

	// Apply gravity per-bone each step (matches hdtSMP64::applyGravity)
	for (auto& bone : bones_) {
		if (bone.rigidBody && !bone.rigidBody->isStaticOrKinematicObject()) {
			bone.rigidBody->setGravity(world_->getGravity() * bone.gravityFactor);
		}
	}

	// Step simulation (single step at dt, matching hdtSMP64 variable time step)
	world_->stepSimulation(dt, 1, dt);

	// Update bone worldTransform from rigid body (matches hdtSMP64 writeTransform)
	for (auto& bone : bones_) {
		if (bone.rigidBody && !bone.rigidBody->isKinematicObject()) {
			btTransform rbTrans = bone.rigidBody->getWorldTransform();
			bone.worldTransform = rbTrans * bone.rigToLocal;
		}
	}

	SkinVertices();
}

void SmpSimulator::Reset() {
	for (auto& bone : bones_) {
		bone.worldTransform = bone.origWorldTransform;
		if (bone.rigidBody) {
			btTransform dest = bone.worldTransform * bone.localToRig;
			bone.rigidBody->setWorldTransform(dest);
			bone.rigidBody->setInterpolationWorldTransform(dest);
			bone.rigidBody->setLinearVelocity(btVector3(0, 0, 0));
			bone.rigidBody->setAngularVelocity(btVector3(0, 0, 0));
			bone.rigidBody->setInterpolationLinearVelocity(btVector3(0, 0, 0));
			bone.rigidBody->setInterpolationAngularVelocity(btVector3(0, 0, 0));
			bone.rigidBody->clearForces();
			bone.rigidBody->updateInertiaTensor();
		}
	}
	SkinVertices();
}

void SmpSimulator::SetGravity(const Vector3& g) {
	if (world_) {
		world_->setGravity(ToBt(g));
		for (auto& bone : bones_) {
			if (bone.rigidBody && !bone.rigidBody->isStaticOrKinematicObject())
				bone.rigidBody->setGravity(world_->getGravity() * bone.gravityFactor);
		}
	}
}

void SmpSimulator::SetWind(const Vector3& wind) {
	wind_ = ToBt(wind);
}

// ---------------------------------------------------------------------------
// Vertex skinning (matches hdtSMP64 SkinnedMeshBody::internalUpdate)
// ---------------------------------------------------------------------------

void SmpSimulator::SkinVertices() {
	for (auto& shape : skinnedShapes_) {
		struct BoneSkinTransform {
			btMatrix3x3 rot;
			btVector3 trans;
		};
		std::vector<BoneSkinTransform> boneTransforms(shape.skinBones.size());

		for (size_t bi = 0; bi < shape.skinBones.size(); bi++) {
			auto& sb = shape.skinBones[bi];
			btTransform boneWorld = GetBoneWorldTransform(sb.boneIdx);

			// worldPos = boneWorld * skinToBone * skinPos
			btTransform skinToBoneBt = ToBt(sb.skinToBone);
			btTransform combined = boneWorld * skinToBoneBt;

			boneTransforms[bi].rot = combined.getBasis();
			boneTransforms[bi].trans = combined.getOrigin();
		}

		for (size_t vi = 0; vi < shape.vertices.size(); vi++) {
			auto& v = shape.vertices[vi];
			btVector3 result(0, 0, 0);

			for (int k = 0; k < 4; k++) {
				if (v.boneIdx[k] < 0 || v.weight[k] <= 0.0f)
					continue;

				auto& bt = boneTransforms[v.boneIdx[k]];
				btVector3 skinPos(v.skinPos.x, v.skinPos.y, v.skinPos.z);
				btVector3 transformed = bt.rot * skinPos + bt.trans;
				result += transformed * v.weight[k];
			}

			shape.currentVerts[vi] = ToNif(result);
		}
	}
}

// ---------------------------------------------------------------------------
// Output queries
// ---------------------------------------------------------------------------

bool SmpSimulator::HasPhysics(const std::string& displayName) const {
	for (auto& shape : skinnedShapes_)
		if (shape.displayName == displayName)
			return true;
	return false;
}

const std::vector<Vector3>& SmpSimulator::GetSkinnedVerts(const std::string& displayName) const {
	for (auto& shape : skinnedShapes_)
		if (shape.displayName == displayName)
			return shape.currentVerts;
	return emptyVerts_;
}

std::vector<std::string> SmpSimulator::GetPhysicsShapeNames() const {
	std::vector<std::string> names;
	names.reserve(skinnedShapes_.size());
	for (auto& shape : skinnedShapes_)
		names.push_back(shape.displayName);
	return names;
}
