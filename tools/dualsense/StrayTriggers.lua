-- StrayTriggers: scratch triggers (working) + PS5 vibration diagnostics + live platform override.
--
-- Purpose: order the easiest->hardest ladder in ONE launch.
--   1. Are the PS5 vibration Blueprints even entered?  (hooks below)
--   2. If entered but nothing reaches libScePad, the GetPlatform() gate is the blocker ->
--      flip it live by creating <gamedir>/stray_platform.on (no restart needed).
--
-- EHKPlatform declaration order: 0 Windows, 1 PS4, 2 PS5, 3 XboxOne, 4 XboxSeriesX,
--                                5 WindowsGDK, 6 Unknown
local function log(m) print("[StrayTriggers] " .. tostring(m) .. "\n") end

local STATE = "stray_trigger.state"
local COMP  = "/Game/Technical/Components/COMP_CatScratchableComponent.COMP_CatScratchableComponent_C:"
local PC    = "/Game/Technical/BP_HKPlayerController.BP_HKPlayerController_C:"
local PS5   = 2

local trigFx = nil        -- {mode, v1, v2, v3} read from the game, nil until seen
local function readTriggerEffect()
    local ok, r = pcall(function()
        local pc = FindFirstOf("HKPlayerController")
        if not pc or not pc:IsValid() then return nil end
        local e = pc.m_scratchablePS5TriggerEffect
        if not e then return nil end
        local function num(v)
            if type(v) == "number" then return v end
            local n; pcall(function() n = tonumber(tostring(v)) end)
            return n
        end
        return { mode = num(e.Mode) or 1, v1 = num(e.Value1) or 0,
                 v2   = num(e.Value2) or 0, v3 = num(e.Value3) or 0 }
    end)
    if ok and r then return r end
    return nil
end

local function publish(on, side)
    if not trigFx then pcall(function() trigFx = readTriggerEffect() end) end
    local f = io.open(STATE, "w")
    if f then
        local x = trigFx
        if x then
            f:write(string.format("%s %s %d %d %d %d\n", tostring(on), tostring(side),
                    x.mode, x.v1, x.v2, x.v3))
        else
            f:write(tostring(on) .. " " .. tostring(side) .. "\n")
        end
        f:close()
    end
end

local function overrideOn()
    local f = io.open("stray_platform.on", "r")
    if f then f:close() return true end
    return false
end

publish(0, 0)
log("loaded - ladder build (override=" .. tostring(overrideOn()) .. ")")

local pending = {}
local function want(path, short, fn, post)
    pending[#pending+1] = { path = path, short = short, fn = fn, post = post }
end

-- (working) scratch-driven triggers
local loggedFx = false
want(COMP .. "SetPS5TriggerActivated", "SetPS5TriggerActivated", function(...)
    if not loggedFx then
        loggedFx = true
        local x = readTriggerEffect()
        if x then
            log(string.format("TRIGGER EFFECT (authored): mode=%d v1=%d v2=%d v3=%d",
                x.mode, x.v1, x.v2, x.v3))
        else
            log("TRIGGER EFFECT: could not read m_scratchablePS5TriggerEffect; using defaults")
        end
    end
    local A = table.pack(...)
    pcall(function()
        local st, sd
        pcall(function() st = A[2]:get() end)
        pcall(function() sd = A[3]:get() end)
        if type(sd) ~= "number" then sd = 0 end
        local on = (st == true or st == 1) and 1 or 0
        if trigAccumulate then
            sideOn[sd] = on
        else
            -- authoritative: this call defines the whole state
            sideOn[0] = (on == 1 and sd == 0) and 1 or 0
            sideOn[1] = (on == 1 and sd == 1) and 1 or 0
        end
        publish(sideOn[0], sideOn[1])
        log(string.format("SetPS5TriggerActivated state=%s side=%d -> L=%d R=%d",
            tostring(st), sd, sideOn[0], sideOn[1]))
    end)
end)
want(COMP .. "_OnUseStarted",   "_OnUseStarted",   function() pcall(function() log("_OnUseStarted") end) end)
want(COMP .. "_OnAfterUseDone", "_OnAfterUseDone", function()
    pcall(function() sideOn[0] = 0; sideOn[1] = 0; publish(0, 0); log("_OnAfterUseDone -> both off") end)
end)

-- STEP 1: are the PS5 vibration blueprints entered at all?
-- StartPS5Vibration(SoundVibration, FadeInTime, Level): arg1 names the waveform.
local function describe(a)
    if type(a) ~= "userdata" then return type(a) end
    local out = "?"
    pcall(function() if a.get then local o = a:get(); if o and o.GetFullName then out = o:GetFullName() end end end)
    if out == "?" then pcall(function() if a.GetFullName then out = a:GetFullName() end end) end
    return out
end
-- Argument shapes differ between the plain and OnAudioComponent variants:
--   StartPS5Vibration(SoundVibration, FadeInTime, Level)
--   StartPS5VibrationOnAudioComponent(AudioComponent, SoundVibration, FadeInTime, Level, VibrationComponent)
-- So locate the sound by what it resolves to, and the level as the last numeric argument.
local function findSound(A)
    for i = 2, A.n do
        local d = describe(A[i])
        if type(d) == "string" and d:find("SoundWave") then return d end
    end
    return nil
end
local function findLevel(A)
    for i = A.n, 2, -1 do
        local v; pcall(function() v = A[i]:get() end)
        if type(v) == "number" then return v, true end
    end
    return 1.0, false
end
local GAIN = 1.0    -- master volume lives in the shim ("gain" cmd), tunable live
local function padVibrationEnabled()
    local ok, v = pcall(function()
        local s = FindFirstOf("HKGameUserSettings")
        if not s or not s:IsValid() then return true end   -- unknown: do not silence
        return s.PadVibrationEnabled
    end)
    if not ok or v == nil then return true end
    return v and true or false
end
local function vibecmd(line)
    local f = io.open("stray_vibe.cmd", "w")
    if f then f:write(line .. "\n") f:close() end
end
-- Separate file: StartPS5ControllerSound and StartPS5Vibration fire in the same frame for
-- the purr, and a shared one-shot file lost whichever wrote first.
local function spkcmd(line)
    local f = io.open("stray_spk.cmd", "w")
    if f then f:write(line .. "\n") f:close() end
end
local function shortName(full)
    -- "SoundWave /Game/.../CatPurr2_VIBE.CatPurr2_VIBE" -> "CatPurr2_VIBE"
    local n = tostring(full):match("([%w_]+)%.[%w_]+$") or tostring(full):match("([%w_]+)$")
    return n
end
playingComponent = nil   -- which AudioComponent owns the current haptic, if any
sideOn = {[0] = 0, [1] = 0}
trigAccumulate = true    -- the game drives BOTH triggers, one call per side (confirmed by feel)
local vibN = 0
local function startVibration(...)
    local A = table.pack(...)
    local viaComponent = false
    pcall(function()
        for i = 2, A.n do
            local d = describe(A[i])
            if type(d) == "string" and d:find("AudioComponent") then viaComponent = true break end
        end
    end)
    pcall(function()
        vibN = vibN + 1
        local parts = {}
        for i = 2, A.n do
            local v; pcall(function() v = A[i]:get() end)
            local d = v
            if type(v) == "userdata" or v == nil then d = describe(A[i]) end
            parts[#parts+1] = string.format("arg%d=%s(%s)", i, tostring(d), type(v))
        end
        local comp = nil
        for i = 2, A.n do
            local d = describe(A[i])
            if type(d) == "string" and d:find("AudioComponent") then comp = d break end
        end
        playingComponent = comp          -- nil for the non-component path
        local full = findSound(A)
        local name = full and shortName(full)
        local fadeIn = 0
        for i = 2, A.n do
            local v; pcall(function() v = A[i]:get() end)
            if type(v) == "number" then fadeIn = v break end   -- FadeInTime precedes Level
        end
        local lv, seen = findLevel(A)
        -- Component-attached vibrations carry their level in the submix send (constant 1.0
        -- per PS5VibrationAttenuation), not in this argument, which measures 0.0 for rain.
        if viaComponent and lv <= 0.0 then lv = 1.0 end   -- component level lives in the submix send
        local amp = math.floor(math.max(0, math.min(1, lv)) * 255 * GAIN)
        log(string.format("VIB START #%d nargs=%d component=%s %s",
            vibN, A.n, tostring(viaComponent), table.concat(parts, " ")))
        log(string.format("   -> %s level=%.3f amp=%d seen=%s",
            tostring(name), lv, amp, tostring(seen)))
        if not padVibrationEnabled() then
            log("VIB suppressed: PadVibrationEnabled is off")
            vibecmd("hapstop")
        elseif name then
            vibecmd(string.format("hap %s %d 1 %d", name, amp,
                math.floor(math.max(0, math.min(10, tonumber(fadeIn) or 0)) * 1000)))
        end
    end)
end
want(PC .. "StartPS5Vibration", "StartPS5Vibration", startVibration)
want(PC .. "StartPS5VibrationOnAudioComponent", "StartPS5VibrationOnAudioComponent", startVibration)
want(PC .. "StopPS5VibrationOnAudioComponent", "StopPS5VibrationOnAudioComponent", function(...)
    local A = table.pack(...)
    pcall(function()
        local comp = nil
        for i = 2, A.n do
            local d = describe(A[i])
            if type(d) == "string" and d:find("AudioComponent") then comp = d break end
        end
        -- Only the component that is actually playing may stop it.
        if playingComponent and comp == playingComponent then
            log("VIB STOP (component) " .. tostring(comp))
            playingComponent = nil
            vibecmd("hapstop")
        end
    end)
end)
want(PC .. "SetPS5VibrationLevelOnAudioComponent", "SetPS5VibrationLevelOnAudioComponent", function(...)
    local A = table.pack(...)
    pcall(function()
        local lv = findLevel(A)
        vibecmd(string.format("haplevel %d", math.floor(math.max(0, math.min(1, lv)) * 255 * GAIN)))
    end)
end)
want(PC .. "StopPS5Vibration", "StopPS5Vibration", function()
    pcall(function() log("VIB STOP (global)"); playingComponent = nil; vibecmd("hapstop") end)
end)
want(PC .. "SetPS5VibrationLevel", "SetPS5VibrationLevel", function(...)
    local B = table.pack(...)          -- varargs must be captured OUTSIDE the pcall
    pcall(function() local v; pcall(function() v = B[2]:get() end); log("VIB LEVEL " .. tostring(v)) end)
    local A = table.pack(...)
    pcall(function()
        if A.n >= 2 then
            local lv = tonumber(tostring(A[2]:get())) or 1.0
            vibecmd(string.format("haplevel %d", math.floor(math.max(0, math.min(1, lv)) * 255 * GAIN)))
        end
    end)
end)
-- Controller-SPEAKER audio. Stray implements this fully and gates it on platform, same
-- as the haptics. The pad is a real Windows audio endpoint here
-- (PROTON_SONY_WINDOWS_DEVICE_NAMES + PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE), so the
-- shim plays the asset straight onto it. Nothing touches ALSA.
local function startControllerSound(...)
    local A = table.pack(...)          -- capture OUTSIDE the pcall (varargs)
    pcall(function()
        local full = findSound(A)
        local name = full and shortName(full)
        local lv, seen = findLevel(A)
        local amp = math.floor(math.max(0, math.min(1, lv)) * 255)
        log(string.format("SPK START sound=%s -> spk %s level=%.3f amp=%d",
            full, tostring(name), lv, amp))
        if name then spkcmd(string.format("spk %s %d 1", name, amp)) end
    end)
end
want(PC .. "StartPS5ControllerSound", "StartPS5ControllerSound", startControllerSound)
want(PC .. "StartPS5ControllerSoundOnAudioComponent", "StartPS5ControllerSoundOnAudioComponent", startControllerSound)
want(PC .. "StopPS5ControllerSound", "StopPS5ControllerSound",
    function() pcall(function() spkcmd("spkstop"); log("SPK STOP") end) end)
want(PC .. "SetPS5ControllerSoundLevel", "SetPS5ControllerSoundLevel", function(...)
    local C = table.pack(...)
    pcall(function()
        local v; pcall(function() v = C[2]:get() end)
        if type(v) == "number" then
            spkcmd(string.format("spklevel %d", math.floor(math.max(0, math.min(1, v)) * 255)))
        end
    end)
end)


-- STEP 2: live platform override
local plat = 0
want("/Script/Hk_project.HKUtilities:GetPlatform", "GetPlatform",
    function() end,
    function(self, ReturnValue)
        pcall(function()
            if not overrideOn() then return end
            ReturnValue:set(PS5)
            plat = plat + 1
            if plat <= 3 or plat % 200 == 0 then log("GetPlatform -> PS5 (" .. plat .. ")") end
        end)
    end)

local GLYPH_PS5 = 3
local glyphOn = function()
    local f = io.open("stray_glyphs.on", "r")
    if f then f:close() return true end
    return false
end
local glyphCalls = 0

-- Diagnostic: does our set() actually take? Log the value BEFORE and AFTER, and describe
-- every argument, since the post-hook arity was guessed. If the value changes and glyphs
-- still do not, the decision is cached somewhere else and hooking earlier will not help.
want("/Script/Hk_project.InputSubsystem:GetGameControllerType", "GetGameControllerType",
    function() end,
    function(...)
        local A = table.pack(...)
        pcall(function()
            local n = A.n
            glyphCalls = glyphCalls + 1
            if glyphCalls <= 6 then
                local parts = {}
                for i = 1, n do
                    local a = A[i]
                    local t = type(a)
                    local v = "?"
                    if t == "userdata" and a.get then
                        local ok, got = pcall(function() return a:get() end)
                        v = ok and tostring(got) or "get-failed"
                    end
                    parts[#parts+1] = string.format("arg%d=%s(%s)", i, t, v)
                end
                log("GGCT call #" .. glyphCalls .. " nargs=" .. n .. "  " .. table.concat(parts, " "))
            end
            -- the ReturnValue is the LAST argument in a UE4SS post-callback
            local rv = nil
            for i = 2, n do
                local a = A[i]
                if type(a) == "userdata" and a.get then
                    local ok, got = pcall(function() return a:get() end)
                    if ok and type(got) == "number" then rv = a break end
                end
            end
            if glyphOn() and rv and rv.set then
                local before = "?"
                pcall(function() before = tostring(rv:get()) end)
                rv:set(GLYPH_PS5)
                local after = "?"
                pcall(function() after = tostring(rv:get()) end)
                if glyphCalls <= 6 then
                    log("   set -> before=" .. before .. " after=" .. after ..
                        (before ~= after and "   <== SET TOOK" or "   <== SET DID NOT TAKE"))
                end
            end
        end)
    end)

local tries = 0
LoopAsync(3000, function()
    tries = tries + 1
    for i = #pending, 1, -1 do
        local h = pending[i]
        local ok = pcall(function()
            if h.post then RegisterHook(h.path, h.fn, h.post) else RegisterHook(h.path, h.fn) end
        end)
        if ok then log("hooked " .. h.short); table.remove(pending, i) end
    end
    if #pending == 0 then log("ALL HOOKS REGISTERED"); return true end
    if tries > 40 then
        for _, h in ipairs(pending) do log("NEVER HOOKED: " .. h.short) end
        return true
    end
    return false
end)
