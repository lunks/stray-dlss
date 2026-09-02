-- StrayProbe: ground truth for "is the game in gameplay?", from inside the engine.
--
-- Every render-side signal the launcher used to gate on is host-dependent and lies somewhere:
-- the ReShade add-on's shader census separates menu from gameplay (~150 vs ~400+), the UE4SS
-- plugin host's census is compute-only (~26 menu / ~34 gameplay, measured 2026-09-02) and
-- cannot, and the TAA pass runs in the menu too (CLAUDE.md 5), so taa_pipelines>0 fires
-- 20 s after launch on the title screen. This mod asks the engine instead and works in every
-- configuration: plugin, ReShade add-on, both, or neither, because UE4SS loads via dwmapi
-- regardless of who owns dxgi.
--
-- Once a second it rewrites stray-game-state.txt in the game's working directory
-- (Binaries/Win64, the same place StrayTriggers writes its sidecars):
--
--   seq=<n>            monotonic, proves the writer is alive (a frozen seq = hung game)
--   t=<unix time>
--   pawn=0|1           the PLAYER cat, BP_CatPawn_C, exists and is valid (the menu's cat and
--                      the companions are other classes, see mods/StrayFur)
--   pc=0|1             a player controller exists
--   map=<world name>   e.g. "BaseMap" in gameplay (measured by StrayFur's log); the launcher
--                      treats a name containing "menu" (any case) as not gameplay
--   ingame=0|1         pawn==1 and pc==1 and map is not a menu map
--
-- Every engine query is wrapped in pcall and degrades to 0/"?" rather than throwing, because
-- this file must never be the thing that takes the game down.

local UEHelpers = require("UEHelpers")

local STATE = "stray-game-state.txt"
local seq = 0

local function valid(obj)
    local ok, v = pcall(function() return obj ~= nil and obj:IsValid() end)
    return ok and v == true
end

local function mapName()
    local ok, name = pcall(function()
        local world = UEHelpers.GetWorld()
        if world == nil or not world:IsValid() then return "?" end
        -- GetFullName() is "World /Game/Maps/Foo.Foo"; keep the object name only.
        local full = world:GetFullName()
        return (full:match("%.([^%.]+)$")) or full
    end)
    return ok and name or "?"
end

-- Returns the pawn's instance name (e.g. BP_CatPawn_C_2147480326) or "" when absent. The
-- name changes if a reload recreates the pawn; measured 2026-09-02 a checkpoint reload
-- keeps it, so this is a diagnostic, not a gate.
local function pawnName()
    local ok, v = pcall(function()
        local pawn = FindFirstOf("BP_CatPawn_C")
        if not valid(pawn) then return "" end
        local full = pawn:GetFullName()
        return (full:match("%.([^%.]+)$")) or full
    end)
    return ok and v or ""
end

local function pawnPresent()
    return pawnName() ~= ""
end

local function controllerPresent()
    local ok, v = pcall(function() return valid(UEHelpers.GetPlayerController()) end)
    return ok and v == true
end

-- paused=1 while the pause menu is up (UGameplayStatics::IsGamePaused). This is what lets
-- stray-reload.sh KNOW the menu opened instead of firing a key sequence into the void:
-- measured 2026-09-02, the sequence sent 2 s after reaching gameplay did nothing (the
-- level's intro still had the menu locked), and nothing distinguished that from a
-- mis-keyed sequence without this field.
local function isPaused()
    local ok, v = pcall(function()
        local gs = UEHelpers.GetGameplayStatics()
        local world = UEHelpers.GetWorld()
        if gs == nil or world == nil or not world:IsValid() then return false end
        return gs:IsGamePaused(world) == true
    end)
    return ok and v == true
end

local function write()
    seq = seq + 1
    local pawn = pawnPresent() and 1 or 0
    local pc = controllerPresent() and 1 or 0
    local map = mapName()
    local paused = isPaused() and 1 or 0
    local menu = map:lower():find("menu", 1, true) ~= nil
    local ingame = (pawn == 1 and pc == 1 and not menu) and 1 or 0
    -- "wb": the game's C runtime is Windows', and text mode turns "\n" into "\r\n", which
    -- the shell readers then mis-compare ("1\r" ~= "1"). Binary mode writes what we say.
    local f = io.open(STATE, "wb")
    if not f then return end
    f:write(string.format("seq=%d\nt=%d\npawn=%d\npawnname=%s\npc=%d\nmap=%s\npaused=%d\ningame=%d\n",
        seq, os.time(), pawn, pawnName(), pc, map, paused, ingame))
    f:close()
end

-- LoopAsync ticks on a UE4SS thread, not the game thread. Touching UObjects from there
-- during a level teardown (a checkpoint reload) is exactly the kind of thing that faults
-- inside ProcessEvent, and the reload crash measured 2026-09-02 sits in the UE4SS mod layer.
-- So the tick only SCHEDULES the work; every engine query runs on the game thread.
LoopAsync(1000, function()
    pcall(function()
        ExecuteInGameThread(function() pcall(write) end)
    end)
    return false   -- keep looping
end)

print("[StrayProbe] loaded; writing " .. STATE .. " every second\n")
