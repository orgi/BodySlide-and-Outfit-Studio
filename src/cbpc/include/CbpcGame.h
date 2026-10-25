/*
 * Stand-in for the SKSE64 headers CBPC includes (skse64/*.h, the Papyrus
 * sources config.cpp includes, skse64_common/*.h, common/*.h): the engine
 * classes, members and functions CBPC uses, with SKSE's names. cbpc.cmake
 * makes every one of those headers include this one. NiTypes.h is SKSE's own
 * (copied from CBPC's source by cbpc.cmake), as it doesn't depend on the game.
 *
 * The objects are filled by CbpcScene.cpp; the functions are in StandIns.cpp.
 */

#pragma once

#include "common/ITypes.h"
#include "common/IErrors.h"
#include "common/IFormHeap.h"
#include "skse64/NiTypes.h"

#include <cstdarg>
#include <fstream>
#include <set>
#include <string>
#include <vector>

class NiNode;
class BSTriShape;
class TESForm;
class TESObjectREFR;
class Actor;
class TESFaction;

namespace cbpc {
// The file a path that CBPC built from GetRuntimeDirectory() denotes: Windows
// separators, names matched without regard to case like on Windows.
std::string ResolveGamePath(const std::string& path);

// std::ifstream for game paths; cbpc.cmake puts it in place of std::ifstream.
class GameFileStream : public std::ifstream {
public:
	GameFileStream() = default;
	explicit GameFileStream(const std::string& path, std::ios_base::openmode mode = std::ios_base::in) { open(path, mode); }
	explicit GameFileStream(const char* path, std::ios_base::openmode mode = std::ios_base::in) { open(std::string(path), mode); }
	void open(const std::string& path, std::ios_base::openmode mode = std::ios_base::in) { std::ifstream::open(ResolveGamePath(path), mode); }
	void open(const char* path, std::ios_base::openmode mode = std::ios_base::in) { open(std::string(path), mode); }
};

// The memory behind a game address CBPC reads through RelocPtr (an offset
// from the executable's base for the game version CBPC is built for).
void* GameAddress(uintptr_t offset);
} // namespace cbpc

// ---------------------------------------------------------------------------
// Calls and casts
// ---------------------------------------------------------------------------

// SKSE calls engine member functions through pointers; the stand-ins are members.
#define CALL_MEMBER_FN(obj, fn) ((obj)->fn)
#define DYNAMIC_CAST(obj, from, to) (dynamic_cast<to*>(static_cast<from*>(obj)))
#define ni_cast(obj, type) (dynamic_cast<type*>(static_cast<NiObject*>(obj)))

template <class T>
class RelocPtr {
public:
	RelocPtr(uintptr_t offset) : m_offset(offset) {}

	T* GetPtr() const { return static_cast<T*>(cbpc::GameAddress(m_offset)); }
	T& operator*() const { return *GetPtr(); }
	T* operator->() const { return GetPtr(); }
	operator T*() const { return GetPtr(); }

private:
	uintptr_t m_offset;
};

template <class T>
class RelocAddr {
public:
	RelocAddr(uintptr_t offset) : m_offset(offset) {}
	operator T() const { return reinterpret_cast<T>(cbpc::GameAddress(m_offset)); }

private:
	uintptr_t m_offset;
};

// ---------------------------------------------------------------------------
// Logging (IDebugLog.h, GameAPI.h)
// ---------------------------------------------------------------------------

void _MESSAGE(const char* fmt, ...);
void _ERROR(const char* fmt, ...);
void Console_Print(const char* fmt, ...);

// The game folder, with a trailing separator.
std::string GetRuntimeDirectory();

// ---------------------------------------------------------------------------
// Strings and arrays (GameTypes.h)
// ---------------------------------------------------------------------------

// A string of the engine's string cache: equal strings, without regard to
// case, share one pointer with the first spelling the cache has seen.
class BSFixedString {
public:
	BSFixedString();
	BSFixedString(const char* buf);
	BSFixedString(const BSFixedString& other) = default;
	BSFixedString& operator=(const BSFixedString& other) = default;

	bool operator==(const BSFixedString& lhs) const { return data == lhs.data; }
	bool operator<(const BSFixedString& lhs) const { return data < lhs.data; }

	BSFixedString* Set(const char* buf);
	const char* Get() const { return c_str(); }
	const char* c_str() const { return data ? data : ""; }

	const char* data;
};

// A string with its own buffer.
class BSString {
public:
	BSString() = default;
	BSString(const char* buf) : value(buf ? buf : "") {}

	const char* Get() const { return value.c_str(); }
	UInt16 GetLength() const { return static_cast<UInt16>(value.size()); }

private:
	std::string value;
};

// Points to elements kept elsewhere (by the scene).
template <class T>
class tArray {
public:
	T* entries = nullptr;
	UInt32 capacity = 0;
	UInt32 pad0C = 0;
	UInt32 count = 0;
	UInt32 pad14 = 0;

	T& operator[](UInt64 index) { return entries[index]; }
	const T& operator[](UInt64 index) const { return entries[index]; }

	bool GetNthItem(UInt64 index, T& pT) const {
		if (index >= count)
			return false;
		pT = entries[index];
		return true;
	}

	void Assign(std::vector<T>& items) {
		entries = items.data();
		capacity = count = static_cast<UInt32>(items.size());
	}
};
STATIC_ASSERT(sizeof(tArray<void*>) == 0x18);

// ---------------------------------------------------------------------------
// Scene graph (NiRTTI.h, NiObjects.h, NiNodes.h, NiGeometry.h)
// ---------------------------------------------------------------------------

class NiRTTI {
public:
	const char* name;
	NiRTTI* parent;
};

class NiRefObject {
public:
	NiRefObject() = default;
	virtual ~NiRefObject() = default;

	void IncRef() { m_uiRefCount++; }
	void DecRef() { m_uiRefCount--; }

	volatile SInt32 m_uiRefCount = 0;
};

class NiObject : public NiRefObject {
public:
	virtual NiRTTI* GetRTTI();
	virtual NiNode* GetAsNiNode() { return nullptr; }
	virtual BSTriShape* GetAsBSTriShape() { return nullptr; }
};

class NiObjectNET : public NiObject {
public:
	const char* m_name = nullptr;
};

class NiAVObject : public NiObjectNET {
public:
	struct ControllerUpdateContext {
		enum {
			kDirty = 1 << 0,
		};

		float delta;
		UInt32 flags;
	};

	NiRTTI* GetRTTI() override;

	// By the name's pointer, like the engine compares strings of its cache.
	virtual NiAVObject* GetObjectByName(const char** name);
	// The world transform from the parent's and the local one, of this object only.
	virtual void UpdateWorldData(ControllerUpdateContext* ctx);

	NiNode* m_parent = nullptr;
	NiTransform m_localTransform;
	NiTransform m_worldTransform;
	UInt32 m_flags = 0;
};

class NiNode : public NiAVObject {
public:
	NiRTTI* GetRTTI() override;
	NiNode* GetAsNiNode() override { return this; }
	NiAVObject* GetObjectByName(const char** name) override;

	NiTArray<NiAVObject*> m_children;
};

class BSGeometry : public NiAVObject {
public:
	NiRTTI* GetRTTI() override;
};

class BSTriShape : public BSGeometry {
public:
	NiRTTI* GetRTTI() override;
	BSTriShape* GetAsBSTriShape() override { return this; }
};

// ---------------------------------------------------------------------------
// Forms (GameForms.h, GameFormComponents.h, GameObjects.h)
// ---------------------------------------------------------------------------

enum FormType {
	kFormType_Keyword = 4,
	kFormType_Class = 10,
	kFormType_Faction = 11,
	kFormType_Race = 14,
	kFormType_Armor = 26,
	kFormType_NPC = 43,
	kFormType_Cell = 60,
	kFormType_Reference = 61,
	kFormType_Character = 62,
	kFormType_WorldSpace = 71,
	kFormType_CombatStyle = 80,
	kFormType_VoiceType = 98,
};

class BaseFormComponent {
public:
	virtual ~BaseFormComponent() = default;
};

class TESForm : public BaseFormComponent {
public:
	enum {
		kFlagIsDeleted = 0x20,
	};

	UInt8 formType = 0;
	UInt32 flags = 0;
	UInt32 formID = 0;
};

class TESFullName : public BaseFormComponent {
public:
	const char* GetName() const { return name.c_str(); }

	BSFixedString name;
};

class BGSKeyword : public TESForm {
public:
	BSString keyword; // editor ID
};

class BGSKeywordForm : public BaseFormComponent {
public:
	bool HasKeyword(BGSKeyword* keyword) const;
	// Points the form at `list` (kept by the scene).
	void Assign(std::vector<BGSKeyword*>& list);

	BGSKeyword** keywords = nullptr;
	UInt32 numKeywords = 0;
	UInt32 pad14 = 0;
};

class BGSBipedObjectForm : public BaseFormComponent {
public:
	struct Data {
		UInt32 parts = 0; // bit n: biped slot 30 + n
		UInt32 weightClass = 0;
	};

	UInt32 GetSlotMask() const { return data.parts; }

	Data data;
};

class TESObjectARMO : public TESForm {
public:
	TESFullName fullName;
	BGSBipedObjectForm bipedObject;
	BGSKeywordForm keyword;
};

class TESFaction : public TESForm {};
class TESClass : public TESForm {};
class BGSArtObject : public TESForm {};
class TESCombatStyle : public TESForm {};
class BGSVoiceType : public TESForm {};

class TESRace : public TESForm {
public:
	enum {
		kRace_Playable = 0x00000001,
		kRace_FaceGenHead = 0x00000002,
		kRace_Child = 0x00000004,
	};

	struct Data {
		UInt32 raceFlags = 0; // RACE DATA flags
	};

	TESFullName fullName;
	BGSKeywordForm keyword;
	Data data;
};

class TESActorBaseData : public BaseFormComponent {
public:
	struct FactionInfo {
		TESFaction* faction;
		SInt8 rank;
	};

	UInt32 flags = 0; // ACBS flags
	BGSVoiceType* voiceType = nullptr;
	tArray<FactionInfo> factions;
};

class TESNPC : public TESForm {
public:
	char GetSex() const { return formType == kFormType_NPC ? static_cast<char>(actorData.flags & 1) : -1; }

	TESActorBaseData actorData;
	TESFullName fullName;
	BGSKeywordForm keyword;
	TESRace* race = nullptr;
	TESClass* npcClass = nullptr;
	TESCombatStyle* combatStyle = nullptr;
	float weight = 0.0f;
};

class TESWorldSpace : public TESForm {
public:
	void* unk158 = nullptr; // parent world space
};

class TESObjectCELL : public TESForm {
public:
	TESWorldSpace* worldSpace = nullptr;
};

// ---------------------------------------------------------------------------
// References (GameReferences.h)
// ---------------------------------------------------------------------------

class TESObjectREFR : public TESForm {
public:
	struct LoadedState {
		NiNode* node = nullptr;
	};

	void IncRef() {}
	void DecRef() {}

	float GetWeight() const { return weight; }
	float GetBaseScale() const { return baseScale; }
	const char* GetReferenceName() const;

	TESForm* baseForm = nullptr;
	TESObjectCELL* parentCell = nullptr;
	LoadedState* loadedState = nullptr;

	float weight = 0.0f;
	float baseScale = 1.0f;
};

class Actor : public TESObjectREFR {
public:
	enum {
		kFlags_IsPlayerTeammate = 0x4000000,
	};

	class FactionVisitor {
	public:
		virtual ~FactionVisitor() = default;
		virtual bool Accept(TESFaction* faction, SInt8 rank) = 0;
	};

	virtual bool IsInCombat() { return false; }
	bool VisitFactions(FactionVisitor& visitor);

	TESRace* race = nullptr;
	UInt32 flags1 = 0;
	UInt32 flags2 = 0;

	// The inventory's worn items, in the order they were added.
	std::vector<TESForm*> wornItems;
};

class PlayerCharacter : public Actor {};

extern PlayerCharacter** g_thePlayer;

bool LookupREFRByHandle(UInt32& refHandle, NiPointer<TESObjectREFR>& refrOut);
TESForm* LookupFormByID(UInt32 formID);

class PlayerCamera {
public:
	static PlayerCamera* GetSingleton();

	NiNode* cameraNode = nullptr;
};

// ---------------------------------------------------------------------------
// Load order (GameData.h)
// ---------------------------------------------------------------------------

struct ModInfo {
	enum {
		kFileFlags_Light = 0x200,
	};

	bool IsActive() const { return modIndex != 0xFF; }
	bool IsLight() const { return (fileFlags & kFileFlags_Light) == kFileFlags_Light; }

	char name[0x104] = {};
	UInt32 fileFlags = 0;
	UInt8 modIndex = 0xFF;
	UInt16 lightIndex = 0;
};

class DataHandler {
public:
	static DataHandler* GetSingleton();

	// Without regard to case, like the engine.
	const ModInfo* LookupModByName(const char* modName);

	std::vector<ModInfo> modList;
};

class Setting {
public:
	bool GetDouble(double* out) const;

	double value = 0.0;
};

Setting* GetINISetting(const char* name);

// ---------------------------------------------------------------------------
// Events (GameEvents.h, GameMenus.h, PapyrusEvents.h)
// ---------------------------------------------------------------------------

enum EventResult {
	kEvent_Continue = 0,
	kEvent_Abort,
};

template <class T>
class EventDispatcher;

template <class T>
class BSTEventSink {
public:
	virtual ~BSTEventSink() = default;
	virtual EventResult ReceiveEvent(T* evn, EventDispatcher<T>* dispatcher) = 0;
};

template <class T>
class EventDispatcher {
public:
	void AddEventSink(BSTEventSink<T>* sink) {
		for (auto* known : sinks) {
			if (known == sink)
				return;
		}
		sinks.push_back(sink);
	}

	void RemoveEventSink(BSTEventSink<T>* sink) {
		for (auto it = sinks.begin(); it != sinks.end(); ++it) {
			if (*it == sink) {
				sinks.erase(it);
				return;
			}
		}
	}

	void SendEvent(T* evn) {
		const std::vector<BSTEventSink<T>*> current = sinks;
		for (auto* sink : current) {
			if (sink->ReceiveEvent(evn, this) == kEvent_Abort)
				break;
		}
	}

private:
	std::vector<BSTEventSink<T>*> sinks;
};

struct TESEquipEvent {
	TESObjectREFR* actor;
	UInt32 baseObject;
	UInt32 originalRefr;
	UInt16 uniqueID;
	bool equipped;
};

struct MenuOpenCloseEvent {
	BSFixedString menuName;
	bool opening;
};

class MenuManager {
public:
	static MenuManager* GetSingleton();

	bool IsGamePaused() const { return numPauseGame != 0; }
	EventDispatcher<MenuOpenCloseEvent>* MenuOpenCloseEventDispatcher() { return &menuOpenCloseEventDispatcher; }

	UInt32 numPauseGame = 0;
	EventDispatcher<MenuOpenCloseEvent> menuOpenCloseEventDispatcher;
};

struct SKSEModCallbackEvent {
	SKSEModCallbackEvent(BSFixedString a_eventName, BSFixedString a_strArg, float a_numArg, TESForm* a_sender) :
		eventName(a_eventName), strArg(a_strArg), numArg(a_numArg), sender(a_sender) {}

	BSFixedString eventName;
	BSFixedString strArg;
	float numArg;
	TESForm* sender;
};

// ---------------------------------------------------------------------------
// Plugin interface (PluginAPI.h)
// ---------------------------------------------------------------------------

typedef UInt32 PluginHandle;
enum {
	kPluginHandle_Invalid = 0xFFFFFFFF,
};

// The engine has no SKSE in the previewer; CBPC's plugin load is emulated by CbpcScene.cpp.
struct SKSEMessagingInterface {
	struct Message {
		const char* sender;
		UInt32 type;
		UInt32 dataLen;
		void* data;
	};

	bool Dispatch(PluginHandle sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};

struct SKSETaskInterface;

// ---------------------------------------------------------------------------
// Papyrus (PapyrusVM.h, PapyrusNativeFunctions.h, PapyrusActor.cpp,
// PapyrusGame.cpp, PapyrusKeyword.h)
// ---------------------------------------------------------------------------

class StaticFunctionTag;

template <class T>
class VMArray {
public:
	UInt32 Length() const { return static_cast<UInt32>(values.size()); }
	void Get(T* dst, UInt32 idx) const { *dst = values[idx]; }

	std::vector<T> values;
};

class IFunction {
public:
	virtual ~IFunction() = default;
};

class VMClassRegistry {
public:
	// There are no scripts in the previewer.
	void RegisterFunction(IFunction* fn);
};

template <class Base, class Result, class... Args>
class NativeFunctionStandIn : public IFunction {
public:
	NativeFunctionStandIn(const char* fnName, const char* className, Result (*callback)(Base*, Args...), VMClassRegistry* registry) {}
};

template <class B, class R>
using NativeFunction0 = NativeFunctionStandIn<B, R>;
template <class B, class R, class T1>
using NativeFunction1 = NativeFunctionStandIn<B, R, T1>;
template <class B, class R, class T1, class T2>
using NativeFunction2 = NativeFunctionStandIn<B, R, T1, T2>;
template <class B, class R, class T1, class T2, class T3>
using NativeFunction3 = NativeFunctionStandIn<B, R, T1, T2, T3>;
template <class B, class R, class T1, class T2, class T3, class T4>
using NativeFunction4 = NativeFunctionStandIn<B, R, T1, T2, T3, T4>;
template <class B, class R, class T1, class T2, class T3, class T4, class T5>
using NativeFunction5 = NativeFunctionStandIn<B, R, T1, T2, T3, T4, T5>;
template <class B, class R, class T1, class T2, class T3, class T4, class T5, class T6>
using NativeFunction6 = NativeFunctionStandIn<B, R, T1, T2, T3, T4, T5, T6>;
template <class B, class R, class T1, class T2, class T3, class T4, class T5, class T6, class T7>
using NativeFunction7 = NativeFunctionStandIn<B, R, T1, T2, T3, T4, T5, T6, T7>;
template <class B, class R, class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8>
using NativeFunction8 = NativeFunctionStandIn<B, R, T1, T2, T3, T4, T5, T6, T7, T8>;
template <class B, class R, class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9>
using NativeFunction9 = NativeFunctionStandIn<B, R, T1, T2, T3, T4, T5, T6, T7, T8, T9>;

typedef std::set<TESFaction*> FactionRankSet;

// SKSE's visitor (PapyrusActor.cpp): the factions with a rank from min to max.
class CollectUniqueFactions : public Actor::FactionVisitor {
public:
	CollectUniqueFactions(FactionRankSet* rankSet, SInt8 min, SInt8 max) : m_min(min), m_max(max), m_rankSet(rankSet) {}

	bool Accept(TESFaction* faction, SInt8 rank) override {
		if (rank >= m_min && rank <= m_max)
			m_rankSet->insert(faction);
		return false;
	}

private:
	SInt8 m_min;
	SInt8 m_max;
	FactionRankSet* m_rankSet;
};

namespace papyrusActor {
// The first worn item, in inventory order, on a biped slot of `mask`.
TESForm* GetWornForm(Actor* thisActor, UInt32 mask);
} // namespace papyrusActor

namespace papyrusGame {
static const int LIGHT_MOD_OFFSET = 0x100;
} // namespace papyrusGame

namespace papyrusKeyword {
// The keyword with this editor ID, without regard to case.
BGSKeyword* GetKeyword(StaticFunctionTag*, BSFixedString keyword);
} // namespace papyrusKeyword
