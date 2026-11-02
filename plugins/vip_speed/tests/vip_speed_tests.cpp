// vip_speed tests — no engine: the plugin TU is compiled in (its map gate and client checks live
// in an anonymous namespace) and every __s2sdk_* / __vip_* binding slot is a recording fake.
//
// Covered:
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client call before the first map, in the
//     window between OnMapEnd and the next OnMapStart, or after OnPluginEnd; the first update
//     latches readiness only when no OnMapEnd was seen since OnPluginStart (a plugin loaded into
//     the map-change window); OnMapStart re-opens the gate; without the map listeners the gate
//     never opens at all;
//   * PITFALLS C2 — IsAlive: the slot range, connected / in-game / SourceTV / controller / alive
//     layers, each checked before the next export is called, and a missing binding refuses
//     instead of crashing;
//   * the spawn write of the original: only a VIP spawn with an alive client reads the "speed"
//     feature, and only a value inside [0.1, 2.0] is written to the velocity modifier; an empty,
//     non-numeric or out-of-range value writes nothing;
//   * a clean unload (PITFALLS A7): the map and VIP listeners are unregistered and the feature's
//     entry is replaced with null callbacks, then a reload registers everything again exactly
//     once;
//   * a plugin started without the vip bindings stays harmless (never Failed).
//
// Run by ./test.sh (build/tests/vip_speed_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

// --- fake s2sdk client exports ---------------------------------------------------------------

struct FakeClient {
	bool connected = true;
	bool inGame = true;
	bool sourceTV = false;
	bool alive = true;
	void* controller = reinterpret_cast<void*>(0x1000);
};

FakeClient g_clients[kMaxSlots];

int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTVCalls = 0;
int g_controllerCalls = 0;
int g_aliveCalls = 0;
int g_badSlotCalls = 0;

int g_speedSets = 0;
int32_t g_lastSpeedSlot = -1;
float g_lastSpeedValue = 0.0f;

bool InRange(int32_t slot) {
	if (slot >= 0 && slot < kMaxSlots) {
		return true;
	}
	++g_badSlotCalls;
	return false;
}

void ResetClientCounters() {
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTVCalls = 0;
	g_controllerCalls = 0;
	g_aliveCalls = 0;
	g_badSlotCalls = 0;
}

int ClientCalls() {
	return g_connectedCalls + g_inGameCalls + g_sourceTVCalls + g_controllerCalls + g_aliveCalls;
}

void ResetSpeedCounters() {
	g_speedSets = 0;
	g_lastSpeedSlot = -1;
	g_lastSpeedValue = 0.0f;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_connectedCalls;
	return InRange(slot) && g_clients[slot].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_inGameCalls;
	return InRange(slot) && g_clients[slot].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_sourceTVCalls;
	return InRange(slot) && g_clients[slot].sourceTV;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_controllerCalls;
	return InRange(slot) ? g_clients[slot].controller : nullptr;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_aliveCalls;
	return InRange(slot) && g_clients[slot].alive;
}

void FakeSetClientSpeed(int32_t slot, float speed) {
	++g_speedSets;
	if (!InRange(slot)) {
		return;
	}
	g_lastSpeedSlot = slot;
	g_lastSpeedValue = speed;
}

// --- fake s2sdk map listeners ----------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	++g_mapStartRegisters;
	g_onMapStart = callback;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	++g_mapStartUnregisters;
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	++g_mapEndRegisters;
	g_onMapEnd = callback;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	++g_mapEndUnregisters;
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
}

void MapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- fake vip core exports -------------------------------------------------------------------

bool g_vipLoaded = true;

std::string g_featureValues[kMaxSlots];
int g_featureCalls = 0;
std::string g_lastFeatureName;

int g_vipRegisterFeatureCalls = 0;
std::string g_registeredFeature;
vip::ValueType g_registeredValueType = vip::ValueType::VIP_NULL;
vip::FeatureType g_registeredFeatureType = vip::FeatureType::HIDE;
vip::ItemSelectableDelegate g_registeredSelect = nullptr;
vip::ItemTogglableDelegate g_registeredToggle = nullptr;
vip::ItemDisplayDelegate g_registeredDisplay = nullptr;

vip::SpawnDelegate g_onPlayerSpawn = nullptr;

void ResetFeatureCounters() {
	g_featureCalls = 0;
	g_lastFeatureName.clear();
}

void ResetSpawnCounters() {
	ResetClientCounters();
	ResetFeatureCounters();
	ResetSpeedCounters();
}

bool FakeVipIsLoaded() {
	return g_vipLoaded;
}

plg::string FakeVipGetClientFeatureString(int32_t slot, const plg::string& feature) {
	++g_featureCalls;
	g_lastFeatureName = ToStd(feature);
	if (!InRange(slot)) {
		return plg::string();
	}
	return plg::string(g_featureValues[slot].c_str());
}

void FakeVipOnPlayerSpawnRegister(vip::SpawnDelegate callback) {
	g_onPlayerSpawn = callback;
}

void FakeVipOnPlayerSpawnUnregister(vip::SpawnDelegate callback) {
	if (g_onPlayerSpawn == callback) {
		g_onPlayerSpawn = nullptr;
	}
}

void FakeVipRegisterFeature(const plg::string& feature, vip::ValueType valueType,
							vip::FeatureType featureType, vip::ItemSelectableDelegate select,
							vip::ItemTogglableDelegate toggle, vip::ItemDisplayDelegate display) {
	++g_vipRegisterFeatureCalls;
	g_registeredFeature = ToStd(feature);
	g_registeredValueType = valueType;
	g_registeredFeatureType = featureType;
	g_registeredSelect = select;
	g_registeredToggle = toggle;
	g_registeredDisplay = display;
}

// The core fires the VIP_OnPlayerSpawn listener one frame after player_spawn.
void Spawn(int32_t slot, int32_t team, bool isVip) {
	if (g_onPlayerSpawn != nullptr) {
		g_onPlayerSpawn(slot, team, isVip);
	}
}

// --- the fake world --------------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_SetClientSpeed = &FakeSetClientSpeed;

	__vip_VIP_IsVIPLoaded = &FakeVipIsLoaded;
	__vip_VIP_GetClientFeatureString = &FakeVipGetClientFeatureString;
	__vip_VIP_OnPlayerSpawn_Register = &FakeVipOnPlayerSpawnRegister;
	__vip_VIP_OnPlayerSpawn_Unregister = &FakeVipOnPlayerSpawnUnregister;
	__vip_VIP_RegisterFeature = &FakeVipRegisterFeature;
}

// Forgets every fake registration and all recorded state. The plugin must have been stopped first
// (PrepareTest/FinishTest do that), otherwise it still believes its listeners are registered.
void ResetWorld() {
	for (FakeClient& client : g_clients) {
		client = FakeClient{};
	}
	ResetSpawnCounters();

	for (std::string& value : g_featureValues) {
		value.clear();
	}

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_vipLoaded = true;
	g_vipRegisterFeatureCalls = 0;
	g_registeredFeature.clear();
	g_registeredValueType = vip::ValueType::VIP_NULL;
	g_registeredFeatureType = vip::FeatureType::HIDE;
	g_registeredSelect = nullptr;
	g_registeredToggle = nullptr;
	g_registeredDisplay = nullptr;
	g_onPlayerSpawn = nullptr;
}

// Stops whatever a previous test left behind (OnPluginEnd is a no-op when nothing is registered)
// and starts the next test from an empty fake world.
void PrepareTest() {
	g_plugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_plugin.OnPluginEnd();
	ResetWorld();
}

// --- A. the map gate (PITFALLS B1/B2) ----------------------------------------------------------

void TestMapGate() {
	PrepareTest();
	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "OnPluginStart registers both map listeners");
	Check(!g_mapReady, "the gate is shut on plugin start");
	Check(g_onPlayerSpawn != nullptr, "OnPluginStart registers the VIP_OnPlayerSpawn listener");

	// --- before the first map: not one client call may reach s2sdk ----------------------------
	Check(!IsAlive(3), "IsAlive is false before the first map");
	Check(ClientCalls() == 0, "not one s2sdk client call is made before the first map");

	// A spawn before the first map must not read the feature or write the speed either.
	g_featureValues[3] = "1.25";
	Spawn(3, 2, true);
	Check(g_featureCalls == 0 && g_speedSets == 0,
		  "a spawn before the first map reads no feature and writes no speed");

	// A plugin loaded into a running map: its first update latches readiness.
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(g_mapReady, "the first OnPluginUpdate latches the gate");
	Check(IsAlive(3), "a live client is alive after the latch");
	Check(ClientCalls() > 0, "the client calls reach s2sdk after the latch");

	Spawn(3, 2, true);
	Check(g_featureCalls == 1 && g_speedSets == 1 && g_lastSpeedValue == 1.25f,
		  "the spawn write works after the latch");

	// --- after OnMapEnd: not one client call may reach s2sdk ---------------------------------
	MapEnd();
	Check(!g_mapReady, "OnMapEnd drops readiness");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "an update after OnMapEnd does not re-open the gate");

	ResetSpawnCounters();
	Check(!IsAlive(3), "IsAlive is false after OnMapEnd");
	Check(ClientCalls() == 0, "not one s2sdk client call is made after OnMapEnd");

	Spawn(3, 2, true);
	Check(g_featureCalls == 0 && g_speedSets == 0, "a spawn between maps writes nothing");

	// --- the next OnMapStart opens the gate again without waiting for an update --------------
	MapStart();
	Check(g_mapReady, "OnMapStart latches the gate");
	Check(IsAlive(3), "the client is alive again");

	FinishTest();
}

// A plugin loaded during the map change (OnMapEnd before its first update) must not latch on the
// update: the first update only counts when no OnMapEnd was seen since OnPluginStart.
void TestMapChangeWindowStart() {
	PrepareTest();
	g_plugin.OnPluginStart();
	MapEnd();
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "the first update after an OnMapEnd does not latch the gate");
	Check(!IsAlive(2), "no client is alive in the map-change window");
	Check(ClientCalls() == 0, "not one s2sdk client call is made in the map-change window");

	MapStart();
	Check(g_mapReady, "the next OnMapStart opens the gate");
	Check(IsAlive(2), "the client is alive after the map start");

	FinishTest();
}

// Without the map listeners nothing could ever close the gate again, so it must never open.
void TestNoMapListeners() {
	PrepareTest();
	const s2sdk::_OnMapStart_Register savedStart = __s2sdk_OnMapStart_Register;
	const s2sdk::_OnMapEnd_Register savedEnd = __s2sdk_OnMapEnd_Register;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;

	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart is Ok without the map listeners");
	Check(!g_mapListenersRegistered, "without the map listeners the gate is never armed");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "an update cannot open a gate that has no map-end listener");
	Check(!IsAlive(0), "no client is alive without the map listeners");
	Check(ClientCalls() == 0, "not one s2sdk client call is made without the map listeners");

	g_plugin.OnPluginEnd();
	Check(!g_mapListenersRegistered, "OnPluginEnd leaves the unarmed gate alone");

	__s2sdk_OnMapStart_Register = savedStart;
	__s2sdk_OnMapEnd_Register = savedEnd;
	FinishTest();
}

// --- B. the client checks (PITFALLS C2) -------------------------------------------------------

void TestClientGuards() {
	PrepareTest();
	g_plugin.OnPluginStart();
	MapStart();
	Check(g_mapReady, "the map gate is open");

	// --- the slot range is checked before any export ------------------------------------------
	ResetClientCounters();
	Check(!IsAlive(-1), "slot -1 is refused");
	Check(!IsAlive(kMaxSlots), "slot 64 is refused");
	Check(!IsAlive(1000), "a far out-of-range slot is refused");
	Check(ClientCalls() == 0, "no s2sdk call is made for an out-of-range slot");
	Check(g_badSlotCalls == 0, "no export ever receives an out-of-range slot");

	// --- every layer in order ------------------------------------------------------------------
	Check(IsAlive(7), "a connected, in-game, alive client with a controller is alive");

	g_clients[7].connected = false;
	ResetClientCounters();
	Check(!IsAlive(7), "a disconnected slot is not alive");
	Check(g_connectedCalls == 1 && g_inGameCalls == 0 && g_sourceTVCalls == 0 &&
			  g_controllerCalls == 0 && g_aliveCalls == 0,
		  "IsClientInGame and the layers below it are not reached for a disconnected slot");

	g_clients[7].connected = true;
	g_clients[7].inGame = false;
	ResetClientCounters();
	Check(!IsAlive(7), "a client outside the game is not alive");
	Check(g_connectedCalls == 1 && g_inGameCalls == 1 && g_sourceTVCalls == 0 &&
			  g_controllerCalls == 0 && g_aliveCalls == 0,
		  "SourceTV and the layers below it are not reached for a client outside the game");

	g_clients[7].inGame = true;
	g_clients[7].sourceTV = true;
	ResetClientCounters();
	Check(!IsAlive(7), "the SourceTV bot is not a player");
	Check(g_connectedCalls == 1 && g_inGameCalls == 1 && g_sourceTVCalls == 1 &&
			  g_controllerCalls == 0 && g_aliveCalls == 0,
		  "the controller is not queried for SourceTV");

	g_clients[7].sourceTV = false;
	g_clients[7].controller = nullptr;
	ResetClientCounters();
	Check(!IsAlive(7), "a client without a controller is not alive");
	Check(g_controllerCalls == 1 && g_aliveCalls == 0,
		  "IsClientAlive is not reached without a controller");

	g_clients[7].controller = reinterpret_cast<void*>(0x1000);
	g_clients[7].alive = false;
	ResetClientCounters();
	Check(!IsAlive(7), "a dead client is not alive");
	Check(g_aliveCalls == 1, "the alive check is reached for a valid client");

	// --- a missing binding refuses instead of crashing ----------------------------------------
	g_clients[7].alive = true;
	const s2sdk::_IsClientConnected savedConnected = __s2sdk_IsClientConnected;
	__s2sdk_IsClientConnected = nullptr;
	ResetClientCounters();
	Check(!IsAlive(7), "IsAlive refuses when a client binding is missing");
	Check(ClientCalls() == 0, "no export below a missing binding is called");
	__s2sdk_IsClientConnected = savedConnected;

	FinishTest();
}

// --- C. the spawn write (the original's VIP_OnPlayerSpawn) -------------------------------------

bool FeatureIsRegisteredAsSpeed() {
	return g_registeredFeature == "speed" && g_registeredValueType == vip::ValueType::VIP_BOOL &&
		   g_registeredFeatureType == vip::FeatureType::TOGGLABLE;
}

void TestSpawnSpeedWrite() {
	PrepareTest();
	g_plugin.OnPluginStart();
	Check(g_onPlayerSpawn != nullptr, "OnPluginStart registers the VIP_OnPlayerSpawn listener");
	Check(g_vipRegisterFeatureCalls == 1, "OnPluginStart registers the feature once");
	Check(FeatureIsRegisteredAsSpeed(), "the feature is registered as the bool togglable 'speed'");
	Check(g_registeredSelect == nullptr && g_registeredToggle == nullptr &&
			  g_registeredDisplay == nullptr,
		  "the feature is registered without callbacks, like the original");

	MapStart();
	Check(g_mapReady, "the map gate is open");

	// A VIP with a value in range gets it written to the velocity modifier.
	g_featureValues[3] = "1.25";
	ResetSpawnCounters();
	Spawn(3, 2, true);
	Check(g_featureCalls == 1, "the spawn reads the feature of the alive VIP");
	Check(g_lastFeatureName == "speed", "the feature read is the 'speed' one");
	Check(g_speedSets == 1 && g_lastSpeedSlot == 3 && g_lastSpeedValue == 1.25f,
		  "the in-range value is written to the velocity modifier of that slot");

	// A non-VIP never reads the feature and never writes the speed.
	g_featureValues[4] = "1.25";
	ResetSpawnCounters();
	Spawn(4, 3, false);
	Check(g_featureCalls == 0 && g_speedSets == 0,
		  "a non-VIP spawn reads no feature and writes no speed");

	// An empty value is skipped before the parse (the original's std::string::empty check).
	g_featureValues[5] = "";
	ResetSpawnCounters();
	Spawn(5, 2, true);
	Check(g_featureCalls == 1 && g_speedSets == 0, "an empty feature value writes nothing");

	// A non-numeric value parses as 0 (atof) and is out of range.
	g_featureValues[6] = "fast";
	ResetSpawnCounters();
	Spawn(6, 2, true);
	Check(g_featureCalls == 1 && g_speedSets == 0, "a non-numeric value writes nothing");

	// Out-of-range values are ignored.
	g_featureValues[7] = "0.05";
	ResetSpawnCounters();
	Spawn(7, 2, true);
	Check(g_speedSets == 0, "a value below 0.1 writes nothing");

	g_featureValues[8] = "2.5";
	ResetSpawnCounters();
	Spawn(8, 2, true);
	Check(g_speedSets == 0, "a value above 2.0 writes nothing");

	// The bounds of the original are inclusive.
	g_featureValues[9] = "0.1";
	ResetSpawnCounters();
	Spawn(9, 2, true);
	Check(g_speedSets == 1 && g_lastSpeedValue == 0.1f, "the lower bound 0.1 is written");

	g_featureValues[10] = "2.0";
	ResetSpawnCounters();
	Spawn(10, 2, true);
	Check(g_speedSets == 1 && g_lastSpeedValue == 2.0f, "the upper bound 2.0 is written");

	// The client checks run before the feature read: a dead / absent / SourceTV client is skipped
	// without one s2sdk or vip call.
	g_featureValues[11] = "1.25";
	g_clients[11].alive = false;
	ResetSpawnCounters();
	Spawn(11, 2, true);
	Check(g_featureCalls == 0 && g_speedSets == 0, "a dead VIP spawn writes nothing");

	g_clients[11].alive = true;
	g_clients[11].sourceTV = true;
	ResetSpawnCounters();
	Spawn(11, 2, true);
	Check(g_featureCalls == 0 && g_speedSets == 0, "the SourceTV slot writes nothing");

	g_clients[11].sourceTV = false;
	g_clients[11].controller = nullptr;
	ResetSpawnCounters();
	Spawn(11, 2, true);
	Check(g_featureCalls == 0 && g_speedSets == 0, "a VIP without a controller writes nothing");

	// An out-of-range slot from the core (never happens) is refused before any export.
	g_clients[11].controller = reinterpret_cast<void*>(0x1000);
	ResetSpawnCounters();
	Spawn(kMaxSlots, 2, true);
	Spawn(-1, 2, true);
	Check(ClientCalls() == 0 && g_featureCalls == 0 && g_speedSets == 0,
		  "out-of-range slots make no call");
	Check(g_badSlotCalls == 0, "no export receives an out-of-range slot");

	// A missing SetClientSpeed binding is refused instead of called.
	const s2sdk::_SetClientSpeed savedSetSpeed = __s2sdk_SetClientSpeed;
	__s2sdk_SetClientSpeed = nullptr;
	ResetSpawnCounters();
	Spawn(3, 2, true);
	Check(g_featureCalls == 1 && g_speedSets == 0, "a missing SetClientSpeed binding is refused");
	__s2sdk_SetClientSpeed = savedSetSpeed;

	// Between maps the spawn handler must not reach s2sdk or the feature at all (PITFALLS B2).
	MapEnd();
	ResetSpawnCounters();
	Spawn(3, 2, true);
	Check(ClientCalls() == 0 && g_featureCalls == 0 && g_speedSets == 0,
		  "a spawn between maps makes no call");

	FinishTest();
}

// --- D. a clean unload and a reload (PITFALLS A7) ---------------------------------------------

void TestUnload() {
	PrepareTest();
	g_plugin.OnPluginStart();
	MapStart();
	g_featureValues[3] = "1.25";
	Spawn(3, 2, true);
	Check(g_speedSets == 1, "the spawn write works before the unload");
	Check(IsAlive(3), "the client is alive before the unload");

	g_plugin.OnPluginEnd();
	Check(!g_mapReady, "OnPluginEnd drops readiness");
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1,
		  "OnPluginEnd unregisters both map listeners");
	Check(g_onMapStart == nullptr && g_onMapEnd == nullptr,
		  "the map callbacks are gone from s2sdk");
	Check(g_onPlayerSpawn == nullptr, "OnPluginEnd unregisters the VIP spawn listener");
	Check(g_vipRegisterFeatureCalls == 2, "OnPluginEnd re-registers the feature entry");
	Check(g_registeredSelect == nullptr && g_registeredToggle == nullptr &&
			  g_registeredDisplay == nullptr,
		  "OnPluginEnd replaces the feature entry with null callbacks");

	ResetSpawnCounters();
	Check(!IsAlive(3), "no client is alive after the unload");
	Check(ClientCalls() == 0, "a check after OnPluginEnd makes no client call");
	Spawn(3, 2, true); // the core no longer holds a listener: nothing happens
	Check(g_featureCalls == 0 && g_speedSets == 0, "a spawn after OnPluginEnd does nothing");

	// plg reload: a fresh start after the end re-registers everything exactly once.
	g_plugin.OnPluginStart();
	Check(g_mapStartRegisters == 2 && g_mapEndRegisters == 2,
		  "a reload registers the map listeners again");
	Check(g_onPlayerSpawn != nullptr, "a reload registers the VIP spawn listener again");
	Check(g_vipRegisterFeatureCalls == 3 && FeatureIsRegisteredAsSpeed(),
		  "a reload registers the feature again");
	Check(!g_mapReady, "a reload starts with the gate shut");
	MapStart();
	Check(g_mapReady, "the reloaded plugin opens the gate on OnMapStart");
	Check(IsAlive(3), "the client is alive after the reload");

	ResetSpawnCounters();
	Spawn(3, 2, true);
	Check(g_speedSets == 1 && g_lastSpeedValue == 1.25f, "the spawn write works after the reload");

	FinishTest();
}

// --- E. a plugin started without the vip bindings ----------------------------------------------

void TestNoVIPBindings() {
	PrepareTest();
	const vip::_VIP_IsVIPLoaded savedLoaded = __vip_VIP_IsVIPLoaded;
	__vip_VIP_IsVIPLoaded = nullptr;

	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart is Ok without the vip bindings");
	Check(g_vipRegisterFeatureCalls == 0, "no feature is registered while the vip core is absent");
	Check(g_onPlayerSpawn == nullptr, "no spawn listener is registered while the vip core is absent");

	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(g_mapReady, "the map gate still opens without the vip bindings");
	Spawn(3, 2, true);
	Check(g_featureCalls == 0 && g_speedSets == 0, "no call is made without the vip bindings");

	__vip_VIP_IsVIPLoaded = savedLoaded;
	FinishTest();
}

} // namespace

int main() {
	std::printf("vip_speed tests (fake s2sdk/vip bindings, no engine)\n");

	InstallFakes();

	TestMapGate();
	TestMapChangeWindowStart();
	TestNoMapListeners();
	TestClientGuards();
	TestSpawnSpeedWrite();
	TestUnload();
	TestNoVIPBindings();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
