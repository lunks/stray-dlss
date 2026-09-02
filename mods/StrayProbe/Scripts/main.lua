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

-- COST DISCIPLINE (user-reported 2026-09-02: "your heartbeat is sync so there is a frame
-- spike every second"). FindFirstOf walks the whole object array and this runs on the
-- game thread, so the expensive lookups are CACHED: a tick normally does only IsValid()
-- checks and a couple of cheap getters. A rescan happens only when a cached object has
-- gone invalid, and never more than once per RESCAN_S.
local RESCAN_S = 2
local cachedPawn, cachedWorld = nil, nil
local lastPawnScan, lastWorldScan = 0, 0

local function objectName(obj)
    local full = obj:GetFullName()
    return (full:match("%.([^%.]+)$")) or full
end

local function world()
    if valid(cachedWorld) then return cachedWorld end
    local now = os.time()
    if now - lastWorldScan < RESCAN_S then return nil end
    lastWorldScan = now
    local ok, w = pcall(UEHelpers.GetWorld)
    cachedWorld = (ok and valid(w)) and w or nil
    return cachedWorld
end

local function mapName()
    local w = world()
    if w == nil then return "?" end
    local ok, name = pcall(objectName, w)   -- "World /Game/Maps/Foo.Foo" -> "Foo"
    return ok and name or "?"
end

-- The pawn's instance name (e.g. BP_CatPawn_C_2147480326) or "" when absent. Measured
-- 2026-09-02: a checkpoint reload keeps the same pawn, so this is a diagnostic, not a gate.
local function pawnName()
    if not valid(cachedPawn) then
        cachedPawn = nil
        local now = os.time()
        if now - lastPawnScan < RESCAN_S then return "" end
        lastPawnScan = now
        local ok, p = pcall(FindFirstOf, "BP_CatPawn_C")
        if not (ok and valid(p)) then return "" end
        cachedPawn = p
    end
    local ok, name = pcall(objectName, cachedPawn)
    return ok and name or ""
end

-- Quiet mode: while a measurement window is open, the bench drops the file
-- stray-probe-quiet in the game dir and the probe stops asking the engine anything; it
-- keeps writing seq/t so liveness is still visible. Nothing of ours then runs on the
-- game thread during the window.
local function quiet()
    local f = io.open("stray-probe-quiet", "r")
    if f then f:close(); return true end
    return false
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
        local gs = UEHelpers.GetGameplayStatics()   -- UEHelpers caches this itself
        local w = world()
        if gs == nil or w == nil then return false end
        return gs:IsGamePaused(w) == true
    end)
    return ok and v == true
end

local function write()
    seq = seq + 1
    if quiet() then
        local f = io.open(STATE, "wb")
        if f then f:write(string.format("seq=%d\nt=%d\nquiet=1\n", seq, os.time())); f:close() end
        return
    end
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

-- BENCH COUNTER, host-independent. While stray-probe-bench exists (stray-traverse.sh
-- drops it for its window), write stray-frame.txt four times a second with the engine's
-- own frame counter (UKismetSystemLibrary::GetFrameCount, i.e. GFrameCounter) and the
-- last frame's delta. That is two cheap static calls per tick, the same in every arm,
-- so the baseline with no render host gets real fps and the ReShade/plugin arms are
-- measured by the identical instrument (the user's requirement 2026-09-02: never poison
-- one arm against another).
local FRAME = "stray-frame.txt"
local function benchFlag()
    local f = io.open("stray-probe-bench", "r")
    if f then f:close(); return true end
    return false
end
local function writeFrame()
    local ok, line = pcall(function()
        local ks = UEHelpers.GetKismetSystemLibrary()
        local gs = UEHelpers.GetGameplayStatics()
        local w = world()
        local frame = ks and ks:GetFrameCount() or -1
        local dt = (gs and w) and gs:GetWorldDeltaSeconds(w) or -1
        return string.format("frame=%d\ndt=%.6f\nt=%d\n", tonumber(frame) or -1, tonumber(dt) or -1, os.time())
    end)
    local f = io.open(FRAME, "wb")
    if f then f:write(ok and line or "frame=-1\ndt=-1\n"); f:close() end
end
LoopAsync(250, function()
    if benchFlag() then
        pcall(function() ExecuteInGameThread(function() pcall(writeFrame) end) end)
    end
    return false
end)

print("[StrayProbe] loaded; writing " .. STATE .. " every second\n")
