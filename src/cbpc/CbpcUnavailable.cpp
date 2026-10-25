/*
 * CbpcScene for builds without CBPC's source in lib/CBPC (see cbpc.cmake).
 */

#include "CbpcScene.h"

namespace cbpc {

bool Available() {
	return false;
}

std::string Version() {
	return {};
}

void Initialise(GameData, LogSink sink, const std::string&) {
	static bool logged = false;
	if (sink && !logged) {
		sink(LogLevel::Info, "the previewer is built without CBPC (no CBPC source in lib/CBPC)");
		logged = true;
	}
}

struct Actor::Impl {};

Actor::Actor(const ActorInfo&) {}
Actor::~Actor() = default;
void Actor::SetScene(const std::vector<SceneNode>&) {}
void Actor::SetWornItems(const std::vector<uint32_t>&) {}
void Actor::Step(float, std::vector<NodeTransforms>&) {}

} // namespace cbpc
