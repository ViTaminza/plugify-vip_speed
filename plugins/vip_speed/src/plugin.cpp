// vip_speed — Plugify port of the Pisex VIP module "VIP_Speed" / "[VIP] Speed"
// (the original's vip_speed.cpp, 130 lines).
//
// Behaviour kept identical: one frame after a VIP spawns (and for VIPs only), the value of the
// "speed" feature of the player's group is parsed as a float and, when it is inside [0.1, 2.0],
// written to the pawn's m_flVelocityModifier. Non-numeric or out-of-range values are ignored (the
// original's atof + range check).
//
// Port notes:
//   * Pure s2sdk: there is no pisexcompat. The safety rules this module needs live in this file
//     (handoff/weaponskins/PITFALLS.md B1/B2, C2):
//       map gate       OnMapStart/OnMapEnd listeners plus the first OnPluginUpdate latch: not one
//                      client call before the first map or in the window between OnMapEnd and the
//                      next OnMapStart (s2sdk's client list is torn down there);
//       IsAlive(slot)  slot range, connected, in game, not SourceTV, controller present and the
//                      client alive - the original's CCSPlayerController::FromSlot +
//                      m_hPlayerPawn + IsAlive chain;
//       bindings       every __s2sdk_* export pointer is checked before it is called.
//   * The core (IVIPApi) is the Plugify "vip" plugin; the module calls it through the consumer
//     wrapper of plugins/vip/include/vip_api.hpp.
//   * VIP_OnPlayerSpawn is the core's callback; it fires for a living, non-bot player one frame
//     after the player_spawn event, which is the moment the original's forward fired.
//   * m_flVelocityModifier is written through s2sdk's SetClientSpeed (CCSPlayerPawn::SetSpeed,
//     the field plus the network update) instead of the raw schema write of the original; this is
//     the engine's own path for the same field (see the porting notes of the admin modules).
//   * The entity-system globals the original set from IVIPApi::VIP_GetEntitySystem (which no
//     longer exists across the Plugify boundary) are gone: the port keeps no engine pointers.

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The original code's unqualified spellings (VIP_BOOL, TOGGLABLE, ...).
using namespace vip;

// The GetLogTag() of the original ("[VIP-Speed]").
constexpr char kLogTag[] = "VIP-Speed";

// The feature name of the original (the group value is a decimal string, e.g. "1.25").
constexpr char kFeature[] = "speed";

// The original iterated the game's 64 player slots.
constexpr int32_t kMaxSlots = 64;

// The object the ported module uses (the Pisex g_pVIPCore).
IVIPApi* g_pVIPCore = &IVIPApi::Get();

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// The original's ErrorLog("[%s] ...", GetLogTag()).
void LogTaggedError(const std::string& message) {
	LogError(std::string("[") + kLogTag + "] " + message);
}

// --- map gate (PITFALLS B1/B2) ----------------------------------------------------------------

bool g_mapReady = false;      // every client call is safe right now
bool g_mapStartSeen = false;  // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;    // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false;

// s2sdk listeners: plain function pointers called on the game thread. Nothing here may throw and
// no client query is made - only state changes.
void OnMapStartListener() {
	// The new map and its entity system exist: a client call is legal from this point on.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart s2sdk's client list is being torn down and
	// any client query crashes - drop the state and nothing else.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void InitMapGate() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the map-end listener nothing could ever take readiness away again, so the gate
		// stays closed for good (PITFALLS B2: log and stay inactive, never return Failed).
		LogTaggedError("the s2sdk map listeners are unavailable - every client call stays refused");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first update after
	// OnPluginStart is the proof of a live map (the host pumps updates from the server frame).
	// After an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates
	// through a changelevel, so `!g_mapEndSeen` must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

void ShutdownMapGate() {
	if (g_mapListenersRegistered) {
		g_mapListenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister != nullptr) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister != nullptr) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
}

// --- client checks (PITFALLS C2) --------------------------------------------------------------

// The original's CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive chain. Every s2sdk
// export dereferences the engine's client record without validating it, so the checks run in
// layers and each layer only runs once the previous ones passed.
bool IsAlive(int32_t slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && s2sdk::PlayerSlotToEntPointer(slot) != nullptr &&
			   s2sdk::IsClientAlive(slot);
	} catch (...) {
		return false;
	}
}

class VipSpeedPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// VIP_OnVIPLoaded of the original keeps no state of its own; the spawn callback of the core
	// is enough, so this module has no core-ready step beyond the registration in OnPluginStart.

	// VIP_OnPlayerSpawn of the original.
	void OnPlayerSpawn(int32_t iSlot, int32_t iTeam, bool bIsVIP);
};

VipSpeedPlugin g_plugin;

void VipSpeedPlugin::OnPlayerSpawn(int32_t iSlot, int32_t iTeam, bool bIsVIP) {
	(void)iTeam;
	if (!bIsVIP) {
		return;
	}

	// The original's controller -> pawn -> alive chain. IsAlive also refuses every call outside a
	// live map (PITFALLS B1/B2).
	if (!IsAlive(iSlot)) {
		return;
	}

	// The original read the "speed" feature string and skipped an empty value.
	const std::string sSpeed = g_pVIPCore->VIP_GetClientFeatureString(iSlot, kFeature);
	if (sSpeed.empty()) {
		return;
	}
	// The value is parsed as a float (the original's atof).
	const float fSpeed = std::atof(sSpeed.c_str());
	if (!(fSpeed >= 0.1f && fSpeed <= 2.0f)) {
		// Non-numeric (atof -> 0) and out-of-range values are ignored, like the original.
		return;
	}
	// The s2sdk wrapper calls the binding slot unchecked, so a missing binding would be a null
	// call; the guard is the same one vip_endurance uses for the same function.
	if (__s2sdk_SetClientSpeed == nullptr) {
		return;
	}
	try {
		// The original wrote the parsed speed into the pawn's velocity modifier.
		s2sdk::SetClientSpeed(iSlot, fSpeed);
	} catch (...) {
	}
}

// --- lifecycle ---------------------------------------------------------------------------------

plg::PluginResult VipSpeedPlugin::OnPluginStart() {
	try {
		// The map listeners come first: they gate every client call this plugin makes (B1/B2).
		InitMapGate();

		// The original registered the spawn callback in AllPluginsLoaded. The core keys one
		// listener per plugin, so no core-ready gate is needed: a spawn before the core's database
		// is up reports isVIP = false, exactly like the original.
		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int iTeam, bool bIsVIP) {
			OnPlayerSpawn(iSlot, iTeam, bIsVIP);
		});

		// The original registered "speed" as a bool in AllPluginsLoaded, although the group value
		// is a decimal string.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);

		LogInfo(std::string("[") + kLogTag + "] ready");
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
}

plg::PluginResult VipSpeedPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		// A plugin loaded into a running map never saw an OnMapStart: the first update latches
		// readiness (no-op while a map is already up, never after an OnMapEnd).
		PollMapGate();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult VipSpeedPlugin::OnPluginEnd() {
	try {
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
		// The vip API has no feature unregister: re-registering the feature without callbacks
		// replaces the core's entry with null delegates, so the core keeps no pointer into this
		// library after it is unloaded (same value/feature type: the menu entry is unchanged).
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);
	} catch (...) {
		// nothing left to do at unload
	}
	try {
		// The map listeners point into this library: drop them before it is unloaded (A7).
		ShutdownMapGate();
	} catch (...) {
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
