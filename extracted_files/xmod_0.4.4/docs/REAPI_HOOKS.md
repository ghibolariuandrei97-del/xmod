# ReAPI hookchains available to xmod (ReGameDLL required)

Use `xmod.on("reapi:<Name>", fn)` / `xmod.on("reapi:<Name>:post", fn)`.
Args are passed to `fn` in order; `(mod)` args can be changed with `xmod.hook.set_arg(n, value)` (n counts after `this`). Return `xmod.HC_SUPERCEDE, value` to skip the original. Post hooks get the return value as an extra last arg.

| Hook | Args | Returns |
|---|---|---|
| `CBasePlayer_Spawn` | this:ent | - |
| `CBasePlayer_Precache` | this:ent | - |
| `CBasePlayer_ObjectCaps` | this:ent | int |
| `CBasePlayer_Classify` | this:ent | int |
| `CBasePlayer_TraceAttack` | this:ent, ent, float, vec{x,y,z}, nil, int | - |
| `CBasePlayer_TakeDamage` | this:ent, ent, ent, float(mod), int | int |
| `CBasePlayer_TakeHealth` | this:ent, float, int | int |
| `CBasePlayer_Killed` | this:ent, ent, int | - |
| `CBasePlayer_AddPoints` | this:ent, int, int | - |
| `CBasePlayer_AddPointsToTeam` | this:ent, int, int | - |
| `CBasePlayer_AddPlayerItem` | this:ent, ent | int |
| `CBasePlayer_RemovePlayerItem` | this:ent, ent | int |
| `CBasePlayer_GiveAmmo` | this:ent, int, string, int | int |
| `CBasePlayer_ResetMaxSpeed` | this:ent | - |
| `CBasePlayer_Jump` | this:ent | - |
| `CBasePlayer_Duck` | this:ent | - |
| `CBasePlayer_PreThink` | this:ent | - |
| `CBasePlayer_PostThink` | this:ent | - |
| `CBasePlayer_UpdateClientData` | this:ent | - |
| `CBasePlayer_ImpulseCommands` | this:ent | - |
| `CBasePlayer_RoundRespawn` | this:ent | - |
| `CBasePlayer_Blind` | this:ent, float, float, float, int | - |
| `CBasePlayer_Observer_IsValidTarget` | this:ent, int, int | ent |
| `CBasePlayer_SetAnimation` | this:ent, int | - |
| `CBasePlayer_GiveDefaultItems` | this:ent | - |
| `CBasePlayer_GiveNamedItem` | this:ent, string | ent |
| `CBasePlayer_AddAccount` | this:ent, int, int, int | - |
| `CBasePlayer_GiveShield` | this:ent, int | - |
| `CBasePlayer_SetClientUserInfoModel` | this:ent, string, string | - |
| `CBasePlayer_SetClientUserInfoName` | this:ent, string, string | int |
| `CBasePlayer_HasRestrictItem` | this:ent, int, int | int |
| `CBasePlayer_DropPlayerItem` | this:ent, string | ent |
| `CBasePlayer_DropShield` | this:ent, int | ent |
| `CBasePlayer_OnSpawnEquip` | this:ent, int, int | - |
| `CBasePlayer_Radio` | this:ent, string, string, int, int | - |
| `CBasePlayer_Disappear` | this:ent | - |
| `CBasePlayer_MakeVIP` | this:ent | - |
| `CBasePlayer_MakeBomber` | this:ent | int |
| `CBasePlayer_StartObserver` | this:ent, vec{x,y,z}, vec{x,y,z} | - |
| `CBasePlayer_GetIntoGame` | this:ent | int |
| `CBaseAnimating_ResetSequenceInfo` | this:ent | - |
| `GetForceCamera` | ent | int |
| `PlayerBlind` | ent, ent, ent, float, float, int, vec{x,y,z} | - |
| `RadiusFlash_TraceLine` | ent, ent, ent, vec{x,y,z}, vec{x,y,z}, nil | - |
| `RoundEnd` | int, int, float | int |
| `InstallGameRules` | - | nil |
| `PM_Init` | nil | - |
| `PM_Move` | nil, int | - |
| `PM_AirMove` | int | - |
| `HandleMenu_ChooseAppearance` | ent, int | - |
| `HandleMenu_ChooseTeam` | ent, int | int |
| `ShowMenu` | ent, int, int, int, string | - |
| `ShowVGUIMenu` | ent, int, int, string | - |
| `BuyGunAmmo` | ent, ent, int | int |
| `BuyWeaponByWeaponID` | ent, int | ent |
| `InternalCommand` | ent, string, string | - |
| `CSGameRules_FShouldSwitchWeapon` | ent, ent | int |
| `CSGameRules_GetNextBestWeapon` | ent, ent | int |
| `CSGameRules_FlPlayerFallDamage` | ent | float |
| `CSGameRules_FPlayerCanTakeDamage` | ent, ent | int |
| `CSGameRules_PlayerSpawn` | ent | - |
| `CSGameRules_FPlayerCanRespawn` | ent | int |
| `CSGameRules_GetPlayerSpawnSpot` | ent | ent |
| `CSGameRules_ClientUserInfoChanged` | ent, string | - |
| `CSGameRules_PlayerKilled` | ent, ent, ent | - |
| `CSGameRules_DeathNotice` | ent, ent, ent | - |
| `CSGameRules_CanHavePlayerItem` | ent, ent | int |
| `CSGameRules_DeadPlayerWeapons` | ent | int |
| `CSGameRules_ServerDeactivate` | - | - |
| `CSGameRules_CheckMapConditions` | - | - |
| `CSGameRules_CleanUpMap` | - | - |
| `CSGameRules_RestartRound` | - | - |
| `CSGameRules_CheckWinConditions` | - | - |
| `CSGameRules_RemoveGuns` | - | - |
| `CSGameRules_GiveC4` | - | ent |
| `CSGameRules_ChangeLevel` | - | - |
| `CSGameRules_GoToIntermission` | - | - |
| `CSGameRules_BalanceTeams` | - | - |
| `CSGameRules_OnRoundFreezeEnd` | - | - |
| `CSGameRules_CanPlayerHearPlayer` | ent, ent | int |
| `PM_UpdateStepSound` | - | - |
| `CBasePlayer_StartDeathCam` | this:ent | - |
| `CBasePlayer_SwitchTeam` | this:ent | - |
| `CBasePlayer_CanSwitchTeam` | this:ent, int | int |
| `CBasePlayer_ThrowGrenade` | this:ent, ent, vec{x,y,z}, vec{x,y,z}, float, int | ent |
| `CWeaponBox_SetModel` | this:ent, string | - |
| `CGrenade_DefuseBombStart` | this:ent, ent | - |
| `CGrenade_DefuseBombEnd` | this:ent, ent, int | - |
| `CGrenade_ExplodeHeGrenade` | this:ent, nil, int | - |
| `CGrenade_ExplodeFlashbang` | this:ent, nil, int | - |
| `CGrenade_ExplodeSmokeGrenade` | this:ent | - |
| `CGrenade_ExplodeBomb` | this:ent, nil, int | - |
| `ThrowHeGrenade` | ent, vec{x,y,z}, vec{x,y,z}, float, int, int | ent |
| `ThrowFlashbang` | ent, vec{x,y,z}, vec{x,y,z}, float | ent |
| `ThrowSmokeGrenade` | ent, vec{x,y,z}, vec{x,y,z}, float, int | ent |
| `PlantBomb` | ent, vec{x,y,z}, vec{x,y,z} | ent |
| `CBasePlayer_SetSpawnProtection` | this:ent, float | - |
| `CBasePlayer_RemoveSpawnProtection` | this:ent | - |
| `IsPenetrableEntity` | vec{x,y,z}, vec{x,y,z}, ent, ent | int |
| `CBasePlayer_HintMessageEx` | this:ent, string, float, int, int | int |
| `CBasePlayer_UseEmpty` | this:ent | - |
| `CBasePlayerWeapon_CanDeploy` | this:ent | int |
| `CBasePlayerWeapon_DefaultDeploy` | this:ent, string, string, int, string, int | int |
| `CBasePlayerWeapon_DefaultReload` | this:ent, int, int, float | int |
| `CBasePlayerWeapon_DefaultShotgunReload` | this:ent, int, int, float, float, string, string | int |
| `CBasePlayer_DropIdlePlayer` | this:ent, string | - |
| `CreateWeaponBox` | ent, ent, string, vec{x,y,z}, vec{x,y,z}, vec{x,y,z}, float, int | ent |
| `SpawnHeadGib` | ent | ent |
| `SpawnRandomGibs` | ent, int, int | - |
| `CGib_Spawn` | this:ent, string | - |
| `CGib_BounceGibTouch` | this:ent, ent | - |
| `CGib_WaitTillLand` | this:ent | - |
| `CBaseEntity_FireBullets` | this:ent, int, vec{x,y,z}, vec{x,y,z}, vec{x,y,z}, float, int, int, int, ent | - |
| `CBaseEntity_FireBuckshots` | this:ent, int, vec{x,y,z}, vec{x,y,z}, vec{x,y,z}, float, int, int, ent | - |
| `CBaseEntity_FireBullets3` | this:ent, vec{x,y,z}, vec{x,y,z}, float, float, int, int, int, float, ent, int, int | vec{x,y,z} |
| `CBasePlayer_Observer_SetMode` | this:ent, int | - |
| `CBasePlayer_Observer_FindNextPlayer` | this:ent, int, string | - |
| `CBasePlayer_Pain` | this:ent, int, int | - |
| `CBasePlayer_DeathSound` | this:ent | - |
| `CBasePlayer_JoiningThink` | this:ent | - |
| `FreeGameRules` | nil | - |
| `PM_LadderMove` | nil | - |
| `PM_WaterJump` | - | - |
| `PM_CheckWaterJump` | - | - |
| `PM_Jump` | - | - |
| `PM_Duck` | - | - |
| `PM_UnDuck` | - | - |
| `PM_PlayStepSound` | int, float | - |
| `PM_AirAccelerate` | nil, float, float | - |
| `ClearMultiDamage` | - | - |
| `AddMultiDamage` | ent, ent, float, int | - |
| `ApplyMultiDamage` | ent, ent | - |
| `BuyItem` | ent, int | - |
| `CSGameRules_Think` | - | - |
| `CSGameRules_TeamFull` | int | int |
| `CSGameRules_TeamStacked` | int, int | int |
| `CSGameRules_PlayerGotWeapon` | ent, ent | - |
| `CBotManager_OnEvent` | int, ent, ent | - |
| `CBasePlayer_CheckTimeBasedDamage` | this:ent | - |
| `CBasePlayer_EntSelectSpawnPoint` | this:ent | ent |
| `CBasePlayerWeapon_ItemPostFrame` | this:ent | - |
| `CBasePlayerWeapon_KickBack` | this:ent, float, float, float, float, float, float, int | - |
| `CBasePlayerWeapon_SendWeaponAnim` | this:ent, int, int | - |
| `CSGameRules_SendDeathMessage` | ent, ent, ent, ent, string, int, int | - |
| `CBasePlayer_PlayerDeathThink` | this:ent | - |
| `CBasePlayer_Observer_Think` | this:ent | - |
| `CBasePlayer_RemoveAllItems` | this:ent, int | - |
| `CBasePlayer_UpdateStatusBar` | this:ent | - |
| `CBasePlayer_TakeDamageImpulse` | this:ent, ent, float, float | - |
| `SendSayMessage` | ent, string, int, string, string, string, int, string, int | - |
