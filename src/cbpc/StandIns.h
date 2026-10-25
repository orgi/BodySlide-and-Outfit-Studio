/*
 * The previewer's side of CBPC's stand-ins (include/CbpcGame.h): the engine
 * state CbpcScene.cpp sets up and StandIns.cpp serves.
 */

#pragma once

#include "CbpcScene.h"
#include "CbpcGame.h"

#include <cstdint>
#include <string>

namespace cbpc {

void SetLogSink(LogSink sink);
void Report(LogLevel level, const std::string& message);

// The folder holding Data (GetRuntimeDirectory).
void SetGameFolder(const std::string& folder);
// The load order (DataHandler) and its records (LookupFormByID, which builds a
// form from its record the first time it is asked for it).
void SetGameData(GameData data);

// The memory behind a game address CBPC reads through RelocPtr.
void RegisterGameAddress(uintptr_t offset, void* address);

// The handle the engine identifies a reference with (LookupREFRByHandle).
UInt32 RegisterReference(TESObjectREFR* reference);
void UnregisterReference(UInt32 handle);

// The engine's NiNode::AttachChild and DetachChild: detaching leaves an empty slot.
void AttachChild(NiNode* parent, NiAVObject* child);
void DetachChild(NiNode* parent, NiAVObject* child);

} // namespace cbpc
