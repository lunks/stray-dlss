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

------------------------------------------------------------------ configuration
-- Two independent periodic writers below, each with its own switch. Both default ON
-- (identical behaviour to before these switches existed). Turn one off when chasing a
-- periodic hitch, to rule its write in or out without losing the other, or the mod's
-- existing stray-probe-quiet flag file (see flagPresent(), below), which suppresses the
-- engine queries but keeps the heartbeat's own liveness write going.
-- ARMED ONLY WHEN THE LAUNCHER ASKED FOR IT.
--
-- The probe exists so tools/launch-stray-safe.sh can tell gameplay from the title screen, and
-- that is the only time anyone needs it. Left running it costs a file write and one engine read
-- on the GAME THREAD every second, forever — which is exactly the shape of the ~1 Hz frame-time
-- blip the user reports, and a diagnostic that perturbs what it measures is worse than none.
--
-- So: the launcher writes `stray-probe-armed` before asking Steam to start the game, and this
-- consumes it (deletes it) at load. A launch the user starts from Steam therefore finds no flag
-- and the probe does nothing at all — no loops scheduled, not merely no writes. The flag is
-- one-shot by deletion rather than by the launcher cleaning up, because the launcher does not
-- outlive every session and a stale flag would silently re-arm the next Steam launch.
local ARM_FLAG = "stray-probe-armed"
local function consume_arm_flag()
    local f = io.open(ARM_FLAG, "rb")
    if not f then return false end
    f:close()
    os.remove(ARM_FLAG)          -- one-shot: the next Steam launch must not inherit it
    return true
end
local ARMED = consume_arm_flag()

local HEARTBEAT_ENABLED     = true   -- STATE (stray-game-state.txt), once a second
local HEARTBEAT_INTERVAL_MS = 1000
local BENCH_ENABLED         = true   -- FRAME (stray-frame.txt), 4x/s while stray-probe-bench
                                      -- exists (flagPresent(), below) - this switch skips
                                      -- even scheduling the poll, not just the write in it
local BENCH_INTERVAL_MS     = 250

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
--
-- ROUND 3 (user-reported 2026-09-03: the spike is still there). Round 1 moved the write
-- off the game thread, round 2 cached FindFirstOf, and this is what was still crossing
-- onto the game thread every second:
--
--   * an io.open of stray-probe-quiet, through Wine's path translation, for a file that
--     normally does not exist. The flag now travels the other way: the async loop reads
--     it, where every other file touch already lives, and a quiet tick queues NOTHING on
--     the game thread - which is what the quiet() comment always claimed it did.
--   * three GetFullName() calls, an FString build plus a Lua pattern match each. The
--     NAMES are now cached beside the objects they came from and recomputed only when a
--     cached object is replaced, exactly as the objects themselves already were.
--   * one of those three was pawnName() evaluated TWICE per tick, once through
--     pawnPresent() and once for state.pawnname. pawnPresent is gone; collect asks once.
--
-- What is deliberately NOT done: skipping ExecuteInGameThread on a non-quiet tick because
-- "nothing can have changed". Nothing here knows that, and the launcher polls ingame=1 to
-- decide the game reached gameplay - staleness there costs a launch, not a frame.
local RESCAN_S = 2
local cachedPawn, cachedWorld = nil, nil
local lastPawnScan, lastWorldScan = 0, 0
-- The name that goes with each cached object. nil means "not read yet", so a GetFullName
-- that throws is retried next tick rather than sticking for the object's whole life, and a
-- successful read is never repeated while the object is the same one.
local cachedPawnName, cachedWorldName = nil, nil

local function objectName(obj)
    local full = obj:GetFullName()
    return (full:match("%.([^%.]+)$")) or full
end

local function world()
    if valid(cachedWorld) then return cachedWorld end
    cachedWorld, cachedWorldName = nil, nil
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
    if cachedWorldName == nil then
        local ok, name = pcall(objectName, w)   -- "World /Game/Maps/Foo.Foo" -> "Foo"
        cachedWorldName = ok and name or nil
    end
    return cachedWorldName or "?"
end

-- The pawn's instance name (e.g. BP_CatPawn_C_2147480326) or "" when absent. Measured
-- 2026-09-02: a checkpoint reload keeps the same pawn, so this is a diagnostic, not a gate.
local function pawnName()
    if not valid(cachedPawn) then
        cachedPawn, cachedPawnName = nil, nil
        local now = os.time()
        if now - lastPawnScan < RESCAN_S then return "" end
        lastPawnScan = now
        local ok, p = pcall(FindFirstOf, "BP_CatPawn_C")
        if not (ok and valid(p)) then return "" end
        cachedPawn = p
    end
    if cachedPawnName == nil then
        local ok, name = pcall(objectName, cachedPawn)
        cachedPawnName = ok and name or nil
    end
    return cachedPawnName or ""
end

-- Quiet mode: while a measurement window is open, the bench drops the file
-- stray-probe-quiet in the game dir and the probe stops asking the engine anything; it
-- keeps writing seq/t so liveness is still visible. Nothing of ours then runs on the
-- game thread during the window - INCLUDING this test, which is an io.open and so belongs
-- with the rest of the file work on the async thread. The loop bodies below read the flags
-- once per tick and hand the answer to the game-thread closure; nothing here is ever called
-- from inside ExecuteInGameThread.
local QUIET_FLAG = "stray-probe-quiet"
local BENCH_FLAG = "stray-probe-bench"
local function flagPresent(path)   -- ASYNC THREAD ONLY
    local f = io.open(path, "rb")
    if f then f:close(); return true end
    return false
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

-- Quiet is decided by the caller (the async loop), which simply does not queue this on a
-- quiet tick - so there is no file test here and no engine query on a silenced frame.
local function collect()
    local name = pawnName()                     -- asked ONCE: it is the GetFullName path
    local pawn = (name ~= "") and 1 or 0
    local pc = controllerPresent() and 1 or 0
    local map = mapName()
    local paused = isPaused() and 1 or 0
    local menu = map:lower():find("menu", 1, true) ~= nil
    state.pawn, state.pawnname, state.pc, state.map, state.paused = pawn, name, pc, map, paused
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

local function flush(isQuiet)
    seq = seq + 1
    if isQuiet then
        atomicWrite(STATE, string.format("seq=%d\nt=%d\nquiet=1\n", seq, os.time()))
    else
        atomicWrite(STATE, string.format("seq=%d\nt=%d\npawn=%d\npawnname=%s\npc=%d\nmap=%s\npaused=%d\ningame=%d\n",
            seq, os.time(), state.pawn, state.pawnname, state.pc, state.map, state.paused, state.ingame))
    end
end

if ARMED and HEARTBEAT_ENABLED then
    LoopAsync(HEARTBEAT_INTERVAL_MS, function()
        -- ONE flag read per tick, on this thread, governing both halves: the write says
        -- quiet=1 and the game thread is not touched at all.
        local isQuiet = flagPresent(QUIET_FLAG)
        pcall(flush, isQuiet)                                           -- async thread: file I/O
        if not isQuiet then
            pcall(function() ExecuteInGameThread(function() pcall(collect) end) end)   -- game thread: reads
        end
        return false   -- keep looping
    end)
else
    print("[StrayProbe] idle: " .. (ARMED and ("HEARTBEAT_ENABLED=false, not writing " .. STATE)
        or "not armed (no " .. ARM_FLAG .. "), so this session costs nothing") .. "\n")
end

-- BENCH COUNTER, host-independent. While stray-probe-bench exists (stray-traverse.sh
-- drops it for its window), write stray-frame.txt four times a second with the engine's
-- own frame counter (UKismetSystemLibrary::GetFrameCount, i.e. GFrameCounter) and the
-- last frame's delta. That is two cheap static calls per tick, the same in every arm,
-- so the baseline with no render host gets real fps and the ReShade/plugin arms are
-- measured by the identical instrument (the user's requirement 2026-09-02: never poison
-- one arm against another).
local FRAME = "stray-frame.txt"
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
if ARMED and BENCH_ENABLED then
    LoopAsync(BENCH_INTERVAL_MS, function()
        if flagPresent(BENCH_FLAG) then   -- async thread, like every other file test
            pcall(flushFrame)
            pcall(function() ExecuteInGameThread(function() pcall(collectFrame) end) end)
        end
        return false
    end)
else
    print("[StrayProbe] BENCH_ENABLED=false: not writing " .. FRAME .. "\n")
end

print("[StrayProbe] loaded; writing " .. STATE .. " every second\n")
