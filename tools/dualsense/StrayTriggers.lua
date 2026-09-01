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

local function publish(on, side)
    local f = io.open(STATE, "w")
    if f then f:write(tostring(on) .. " " .. tostring(side) .. "\n") f:close() end
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
want(COMP .. "SetPS5TriggerActivated", "SetPS5TriggerActivated", function(...)
    local A = table.pack(...)
    pcall(function()
        local st, sd
        pcall(function() st = A[2]:get() end)
        pcall(function() sd = A[3]:get() end)
        if type(sd) ~= "number" then sd = 0 end
        sideOn[sd] = (st == true or st == 1) and 1 or 0
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
sideOn = {[0] = 0, [1] = 0}   -- per-side trigger state; the game sets each separately
local vibN = 0
want(PC .. "StartPS5Vibration", "StartPS5Vibration", function(...)
    local A = table.pack(...)
    pcall(function()
        vibN = vibN + 1
        -- Log EVERY argument raw. "level=255" was ambiguous: it could be a real 1.0 or the
        -- fallback used when the argument is absent, and those need different fixes.
        local parts = {}
        for i = 2, A.n do
            local v; pcall(function() v = A[i]:get() end)
            local d = v
            if type(v) == "userdata" then d = describe(A[i]) end
            parts[#parts+1] = string.format("arg%d=%s(%s)", i, tostring(d), type(v))
        end
        log(string.format("VIB START #%d nargs=%d %s", vibN, A.n, table.concat(parts, " ")))

        local full = describe(A[2])
        local name = shortName(full)
        -- Take the LAST numeric argument as Level; nil (absent) is distinct from 0.
        local lv, lvSeen = nil, false
        for i = A.n, 3, -1 do
            local v; pcall(function() v = A[i]:get() end)
            if type(v) == "number" then lv = v lvSeen = true break end
        end
        if not lvSeen then lv = 1.0 end
        local amp = math.floor(math.max(0, math.min(1, lv)) * 255 * GAIN)
        log(string.format("   -> play %s level=%.3f amp=%d seen=%s", tostring(name), lv, amp, tostring(lvSeen)))
        if not padVibrationEnabled() then
            log("VIB suppressed: PadVibrationEnabled is off")
            vibecmd("stop")
        elseif name then
            vibecmd(string.format("play %s %d 1", name, amp))
        end
    end)
end)
want(PC .. "StopPS5Vibration", "StopPS5Vibration", function()
    pcall(function() log("VIB STOP"); vibecmd("stop") end)
end)
want(PC .. "SetPS5VibrationLevel", "SetPS5VibrationLevel", function(...)
    local B = table.pack(...)          -- varargs must be captured OUTSIDE the pcall
    pcall(function() local v; pcall(function() v = B[2]:get() end); log("VIB LEVEL " .. tostring(v)) end)
    local A = table.pack(...)
    pcall(function()
        if A.n >= 2 then
            local lv = tonumber(tostring(A[2]:get())) or 1.0
            vibecmd(string.format("level %d", math.floor(math.max(0, math.min(1, lv)) * 255 * GAIN)))
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
        local full = describe(A[2])
        local name = shortName(full)
        local lv, seen = nil, false
        for i = A.n, 3, -1 do
            local v; pcall(function() v = A[i]:get() end)
            if type(v) == "number" then lv = v seen = true break end
        end
        if not seen then lv = 1.0 end
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
want(PC .. "StartPS5VibrationOnAudioComponent", "StartPS5VibrationOnAudioComponent",
    function() pcall(function() log("VIB-BP >>> StartPS5VibrationOnAudioComponent") end) end)

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
