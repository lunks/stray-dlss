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

-- THREADING, and why it is shaped like this (user-reported 2026-09-02: "writing sync inside
-- the game is causing frame spikes"). The game thread only READS a few values into the
-- `state` table (collect); the file I/O happens on the UE4SS async thread that LoopAsync
-- ticks on (flush), from whatever collect last stored. No disk write ever runs on the
-- game thread, and the async thread never touches a UObject.
local state = { seq = 0, pawn = 0, pawnname = "", pc = 0, map = "?", paused = 0, ingame = 0 }

local function collect()
    if quiet() then return end
    local pawn = pawnPresent() and 1 or 0
    local pc = controllerPresent() and 1 or 0
    local map = mapName()
    local paused = isPaused() and 1 or 0
    local menu = map:lower():find("menu", 1, true) ~= nil
    state.pawn, state.pawnname, state.pc, state.map, state.paused = pawn, pawnName(), pc, map, paused
    state.ingame = (pawn == 1 and pc == 1 and not menu) and 1 or 0
end

-- ATOMIC writes: write a temp file, then rename over the target. The shell samplers read
-- these files 4x a second; an in-place truncate-and-write let a reader see an empty or
-- half-written number (measured 2026-09-02: a bucket of "-27749.5 fps" in the bench CSV).
-- "wb": the game's C runtime is Windows', and text mode turns "\n" into "\r\n", which the
-- shell readers then mis-compare ("1\r" ~= "1"). Binary mode writes what we say.
local function atomicWrite(path, text)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "wb")
    if not f then return end
    f:write(text); f:close()
    if not os.rename(tmp, path) then
        os.remove(path); os.rename(tmp, path)   -- Windows rename does not overwrite by itself
    end
end

local function flush()
    seq = seq + 1
    if quiet() then
        atomicWrite(STATE, string.format("seq=%d\nt=%d\nquiet=1\n", seq, os.time()))
    else
        atomicWrite(STATE, string.format("seq=%d\nt=%d\npawn=%d\npawnname=%s\npc=%d\nmap=%s\npaused=%d\ningame=%d\n",
            seq, os.time(), state.pawn, state.pawnname, state.pc, state.map, state.paused, state.ingame))
    end
end

LoopAsync(1000, function()
    pcall(flush)                                                    -- async thread: file I/O
    pcall(function() ExecuteInGameThread(function() pcall(collect) end) end)   -- game thread: reads
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
local frameState = { frame = -1, dt = -1 }
local function collectFrame()   -- game thread: two static calls, nothing else
    local ok = pcall(function()
        local ks = UEHelpers.GetKismetSystemLibrary()
        local gs = UEHelpers.GetGameplayStatics()
        local w = world()
        frameState.frame = tonumber(ks and ks:GetFrameCount() or -1) or -1
        frameState.dt = tonumber((gs and w) and gs:GetWorldDeltaSeconds(w) or -1) or -1
    end)
    if not ok then frameState.frame, frameState.dt = -1, -1 end
end
local function flushFrame()     -- async thread: the write, atomic (see atomicWrite)
    atomicWrite(FRAME, string.format("frame=%d\ndt=%.6f\nt=%d\n", frameState.frame, frameState.dt, os.time()))
end
LoopAsync(250, function()
    if benchFlag() then
        pcall(flushFrame)
        pcall(function() ExecuteInGameThread(function() pcall(collectFrame) end) end)
    end
    return false
end)

print("[StrayProbe] loaded; writing " .. STATE .. " every second\n")
