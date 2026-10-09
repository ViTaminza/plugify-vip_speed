// vip_speed — Plugify port of the Pisex VIP module "VIP_Speed" / "[VIP] Speed"
// (ref/src/cs2-vip-modules/VIP_Speed/source/vip_speed.cpp, 130 lines).
//
// Behaviour kept identical: one frame after a VIP spawns (and for VIPs only), the value of the
// "speed" feature of the player's group is parsed as a float and, when it is inside [0.1, 2.0],
// written to the pawn's m_flVelocityModifier. Non-numeric or out-of-range values are ignored (the
// original's atof + range check).
//
// Port notes:
//   * The core (IVIPApi) is the Plugify "vip" plugin; the module calls it through the consumer
//     wrapper of plugins/vip/include/vip_api.hpp.
//   * VIP_OnPlayerSpawn is the core's callback; it fires for a living, non-bot player one frame
//     after the player_spawn event, which is the moment the original's forward fired.
//   * m_flVelocityModifier is written through s2sdk's SetClientSpeed (CCSPlayerPawn::SetSpeed,
//     the field plus the network update) instead of the raw schema write of the original; this is
//     the engine's own path for the same field (see the porting notes of the admin modules).
//   * The pawn/alive checks of the original (m_hPlayerPawn + IsAlive) become
//     pisex::g_pPlayers->IsAlive, and the entity-system globals the original set from
//     IVIPApi::VIP_GetEntitySystem (which no longer exists across the Plugify boundary) are gone.

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <vip_api.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>

namespace {

// The GetLogTag() of the original ("[VIP-Speed]").
constexpr char kLogTag[] = "VIP-Speed";

// The object the ported module uses (the Pisex g_pVIPCore).
vip::IVIPApi* g_pVIPCore = &vip::IVIPApi::Get();

class VipSpeedPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// VIP_OnVIPLoaded of the original keeps no state of its own; the spawn callback of the core
	// is enough, so this module has no core-ready step beyond the registration in OnPluginStart.

	// VIP_OnPlayerSpawn of the original.
	void OnPlayerSpawn(int iSlot, int iTeam, bool bIsVIP);
};

VipSpeedPlugin g_plugin;

void VipSpeedPlugin::OnPlayerSpawn(int iSlot, int iTeam, bool bIsVIP) {
	(void)iTeam;
	if (!bIsVIP) {
		return;
	}

	// CCSPlayerController* pPlayerController = CCSPlayerController::FromSlot(iSlot);
	// CCSPlayerPawn* pPlayerPawn = pPlayerController->m_hPlayerPawn();
	// if (!pPlayerPawn || !pPlayerPawn->IsAlive()) return;
	if (!pisex::g_pPlayers->IsAlive(iSlot)) {
		return;
	}

	// const char* sSpeed = VIP_GetClientFeatureString(iSlot, "speed"); if (strlen(sSpeed) > 0)
	const std::string sSpeed = g_pVIPCore->VIP_GetClientFeatureString(iSlot, "speed");
	if (sSpeed.empty()) {
		return;
	}
	// float fSpeed = atof(sSpeed);
	const float fSpeed = std::atof(sSpeed.c_str());
	// The s2sdk wrapper calls the binding slot unchecked, so a missing binding would be a null
	// call; the guard is the same one vip_endurance uses for the same function.
	if (fSpeed >= 0.1f && fSpeed <= 2.0f && __s2sdk_SetClientSpeed) {
		// pPlayerPawn->m_flVelocityModifier() = fSpeed.
		s2sdk::SetClientSpeed(iSlot, fSpeed);
	}
}

// --- lifecycle ---------------------------------------------------------------------------------

plg::PluginResult VipSpeedPlugin::OnPluginStart() {
	try {
		pisex::Init("vip");

		// g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn) of AllPluginsLoaded. The core keys one
		// listener per plugin, so no core-ready gate is needed: a spawn before the core's database
		// is up reports isVIP = false, exactly like the original.
		g_pVIPCore->VIP_OnPlayerSpawn([](int iSlot, int iTeam, bool bIsVIP) {
			g_plugin.OnPlayerSpawn(iSlot, iTeam, bIsVIP);
		});

		// VIP_RegisterFeature("speed", VIP_BOOL, TOGGLABLE) of AllPluginsLoaded (the original
		// registers the feature as a bool although the group value is a decimal string).
		g_pVIPCore->VIP_RegisterFeature("speed", vip::VIP_BOOL, vip::TOGGLABLE);

		return {};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("[%s] OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("[%s] OnPluginStart failed with an unknown exception", kLogTag);
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult VipSpeedPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		pisex::Poll();
	} catch (const std::exception& ex) {
		pisex::ErrorLog("[%s] OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("[%s] OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult VipSpeedPlugin::OnPluginEnd() {
	try {
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
		pisex::Shutdown();
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
