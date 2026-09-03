-- StrayAudioProbe: what does the PC build do with its PS5 haptic audio? Read from the engine.
--
-- Written 2026-09-03 after the submix tap bound and reported ZERO callbacks on
-- Submix_vibrationMaster for a whole session (docs/STRAY-DUALSENSE.md §14). The UE 4.27.2
-- source says an EndpointSubmix whose endpoint type has no factory on this platform gets the
-- DUMMY factory and FMixerSubmix::ProcessAudioAndSendToEndpoint returns before processing
-- its children -- so the whole vibration subtree is never rendered. This probe measures the
-- parts of that story the source cannot settle for a licensee build:
--
--   1. the submix graph: class of each node, ParentSubmix, ChildSubmixes, EndpointType,
--      OutputVolume, effect chains                          -> stray-audio-probe.txt
--   2. whether the vibration Blueprints ever reach the audio engine on PC: hooks on
--      AudioComponent Play/FadeIn/SetSound/SetSubmixSend and the GameplayStatics spawners,
--      logging only controller-class sounds (_VIBE / _CONTROL / ControllerVibration)
--   3. BP_HKPlayerController_C.DebugPS5Haptic, a bool that sits next to the platform gate:
--      while <gamedir>/stray_debughaptic.on exists it is set TRUE in the StartPS5Vibration
--      PRE-hook, i.e. before the Blueprint body evaluates its gate. If the audio hooks then
--      fire for _VIBE sounds, the gate is (platform == PS5) OR DebugPS5Haptic.
--   4. HKUtilities:GetPlatform call count and value, NOT overridden.
--
-- Threading (mods/StrayProbe's rule, user-measured 2026-09-02): engine reads on the game
-- thread inside hooks or ExecuteInGameThread; file I/O on the async thread; expensive lookups
-- once. Everything is pcall-wrapped: this file must never be what takes the game down.

local OUT = "stray-audio-probe.txt"
local DEBUG_FLAG = "stray_debughaptic.on"

local pending = {}      -- lines waiting for the async flusher
local function say(line)
    pending[#pending + 1] = string.format("%s %s", os.date("%H:%M:%S"), tostring(line))
end

local function flush()
    if #pending == 0 then return end
    local f = io.open(OUT, "ab")
    if not f then return end
    for _, l in ipairs(pending) do f:write(l, "\n") end
    f:close()
    pending = {}
end

local function valid(o)
    local ok, v = pcall(function() return o ~= nil and o:IsValid() end)
    return ok and v == true
end

local function fullName(o)
    if not valid(o) then return "null" end
    local ok, n = pcall(function() return o:GetFullName() end)
    return ok and n or "?"
end

local function className(o)
    if not valid(o) then return "?" end
    local ok, n = pcall(function() return o:GetClass():GetFullName() end)
    return ok and n or "?"
end

local function prop(o, name)
    local ok, v = pcall(function() return o[name] end)
    if not ok then return "<err:" .. tostring(v) .. ">" end
    return v
end

local function show(v)
    local t = type(v)
    if t == "userdata" then
        -- FName has ToString; UObjects have GetFullName; TArrays have GetArrayNum.
        local ok, s = pcall(function() return v:ToString() end)
        if ok and type(s) == "string" then return s end
        ok, s = pcall(function() return v:GetFullName() end)
        if ok and type(s) == "string" then return s end
        ok, s = pcall(function() return "array[" .. tostring(v:GetArrayNum()) .. "]" end)
        if ok then return s end
        return tostring(v)
    end
    return tostring(v)
end

local function arrayNames(arr)
    local out = {}
    local ok = pcall(function()
        local n = arr:GetArrayNum()
        for i = 1, n do
            local e = arr[i]
            local ok2, got = pcall(function() return e:get() end)
            out[#out + 1] = show(ok2 and got or e)
        end
    end)
    if not ok then return "<not an array>" end
    return "[" .. table.concat(out, ", ") .. "]"
end

local function flagOn(path)
    local f = io.open(path, "r")
    if f then f:close(); return true end
    return false
end

local function isControllerSound(name)
    if type(name) ~= "string" then return false end
    return name:find("_VIBE", 1, true) or name:find("_CONTROL", 1, true)
        or name:find("ControllerVibration", 1, true) or name:find("controller", 1, true)
end

-- ---------------------------------------------------------------------------------------
-- 1. The graph dump. Once, on the game thread, when the player controller exists.
-- ---------------------------------------------------------------------------------------
local SUBMIXES = {
    "/Game/Sound/tools/settings/VibrationEndpointSubmix.VibrationEndpointSubmix",
    "/Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster",
    "/Game/Sound/tools/settings/Submix_vibration.Submix_vibration",
    "/Game/Sound/tools/settings/ControllerEndpointSubmix.ControllerEndpointSubmix",
    "/Game/Sound/tools/settings/Submix_controllerMaster.Submix_controllerMaster",
    "/Game/Sound/tools/settings/Submix_controllerPre.Submix_controllerPre",
    "/Game/Sound/tools/settings/Submix_controller.Submix_controller",
    "/Game/Sound/tools/settings/Submix_unused.Submix_unused",
    "/Game/Sound/tools/settings/Submix_Master.Submix_Master",
}
local SOUNDS = {
    "/Game/Sound/SFX/controllers/Vibrations/CatPurr2_VIBE.CatPurr2_VIBE",
    "/Game/Sound/SFX/controllers/Vibrations/Scratch_VIBE.Scratch_VIBE",
    "/Game/Sound/SFX/controllers/sounds/cat_purr_loop_01_CONTROL.cat_purr_loop_01_CONTROL",
}

local function dumpSubmix(path)
    local o = StaticFindObject(path)
    if not valid(o) then say("SUBMIX " .. path .. " -> NOT LOADED"); return end
    say(string.format("SUBMIX %s class=%s", fullName(o), className(o)))
    for _, p in ipairs({ "ParentSubmix", "EndpointType", "EndpointSettings", "EndpointSettingsClass",
                         "OutputVolume", "WetLevel", "DryLevel", "bMuteWhenBackgrounded",
                         "bAutoDisable", "AutoDisableTime", "AmbisonicsPluginSettings" }) do
        local v = prop(o, p)
        if v ~= nil then say("   ." .. p .. " = " .. show(v)) end
    end
    say("   .ChildSubmixes = " .. arrayNames(prop(o, "ChildSubmixes")))
    say("   .SubmixEffectChain = " .. arrayNames(prop(o, "SubmixEffectChain")))
end

local function dumpSound(path)
    local o = StaticFindObject(path)
    if not valid(o) then say("SOUND " .. path .. " -> NOT LOADED"); return end
    say(string.format("SOUND %s class=%s", fullName(o), className(o)))
    for _, p in ipairs({ "SoundSubmixObject", "SoundClassObject", "AttenuationSettings", "bLooping",
                         "Volume", "Pitch", "Duration", "NumChannels", "SampleRate",
                         "bEnableBusSends", "bEnableBaseSubmix", "bEnableSubmixSends",
                         "VirtualizationMode", "bIsUISound" }) do
        local v = prop(o, p)
        if v ~= nil then say("   ." .. p .. " = " .. show(v)) end
    end
    say("   .SoundSubmixSends = " .. arrayNames(prop(o, "SoundSubmixSends")))
    say("   .BusSends = " .. arrayNames(prop(o, "BusSends")))
end

local function dumpAttenuation()
    local o = StaticFindObject("/Game/Sound/tools/settings/attenuation/PS5VibrationAttenuation.PS5VibrationAttenuation")
    if not valid(o) then say("ATTENUATION not loaded"); return end
    say("ATTENUATION " .. fullName(o))
    local att = prop(o, "Attenuation")
    if type(att) == "userdata" then
        for _, p in ipairs({ "bAttenuate", "bSpatialize", "bEnableSubmixSends", "bEnableSourceDataOverride" }) do
            local ok, v = pcall(function() return att[p] end)
            if ok and v ~= nil then say("   .Attenuation." .. p .. " = " .. show(v)) end
        end
        local ok, sends = pcall(function() return att.SubmixSendSettings end)
        if ok and sends ~= nil then
            local ok2 = pcall(function()
                local n = sends:GetArrayNum()
                say("   .Attenuation.SubmixSendSettings = " .. n .. " entr(y/ies)")
                for i = 1, n do
                    local e = sends[i]
                    local sub, lvl, method = "?", "?", "?"
                    pcall(function() sub = show(e.Submix) end)
                    pcall(function() lvl = show(e.ManualSendLevel) end)
                    pcall(function() method = show(e.SendLevelControlMethod) end)
                    say(string.format("      [%d] Submix=%s ManualSendLevel=%s method=%s", i, sub, lvl, method))
                end
            end)
            if not ok2 then say("   .Attenuation.SubmixSendSettings = <unreadable>") end
        end
    end
end

local function dumpPlayerController()
    local pc = FindFirstOf("HKPlayerController")
    if not valid(pc) then say("PLAYERCONTROLLER not found"); return false end
    say("PLAYERCONTROLLER " .. fullName(pc))
    for _, p in ipairs({ "m_PS5VibrationSubmix", "DebugPS5Haptic", "ControllerVibration" }) do
        say("   ." .. p .. " = " .. show(prop(pc, p)))
    end
    local comp = prop(pc, "ControllerVibration")
    if valid(comp) then
        say("   ControllerVibration component " .. fullName(comp))
        for _, p in ipairs({ "Sound", "bAutoActivate", "bIsUISound", "bOverrideAttenuation",
                             "AttenuationSettings", "SoundClassOverride", "VolumeMultiplier",
                             "bAllowSpatialization", "bIsPaused", "bIsPreviewSound" }) do
            local v = prop(comp, p)
            if v ~= nil then say("      ." .. p .. " = " .. show(v)) end
        end
        local ok, playing = pcall(function() return comp:IsPlaying() end)
        say("      IsPlaying() = " .. (ok and tostring(playing) or "<err>"))
    end
    return true
end

local dumped = false
local function dumpAll()
    if dumped then return end
    if not dumpPlayerController() then return end
    dumped = true
    for _, p in ipairs(SUBMIXES) do dumpSubmix(p) end
    for _, p in ipairs(SOUNDS) do dumpSound(p) end
    dumpAttenuation()
    say("DUMP COMPLETE")
end

-- ---------------------------------------------------------------------------------------
-- 2. Hooks. Native functions, so pre and post both exist; we use PRE.
-- ---------------------------------------------------------------------------------------
local counts = {}
local function bump(k) counts[k] = (counts[k] or 0) + 1; return counts[k] end

local function componentLine(tag, ctx, extra)
    local comp = nil
    pcall(function() comp = ctx:get() end)
    local compName = fullName(comp)
    local sound = valid(comp) and prop(comp, "Sound") or nil
    local soundName = show(sound)
    if isControllerSound(soundName) or isControllerSound(compName) then
        say(string.format("%s #%d comp=%s sound=%s%s", tag, bump(tag), compName, soundName, extra or ""))
    end
end

local function describeArgs(A)
    local parts = {}
    for i = 2, A.n do
        local v
        pcall(function() v = A[i]:get() end)
        parts[#parts + 1] = show(v)
    end
    return table.concat(parts, " | ")
end

local hooks = {}
local function want(path, fn, post)
    hooks[#hooks + 1] = { path = path, fn = fn, post = post }
end

want("/Script/Engine.AudioComponent:Play", function(ctx) pcall(componentLine, "AC.Play", ctx) end)
want("/Script/Engine.AudioComponent:FadeIn", function(ctx, ...)
    local A = table.pack(...)
    pcall(componentLine, "AC.FadeIn", ctx, " args=" .. describeArgs(table.pack(nil, table.unpack(A, 1, A.n))))
end)
want("/Script/Engine.AudioComponent:SetSound", function(ctx, newSound)
    pcall(function()
        local s; pcall(function() s = newSound:get() end)
        local name = show(s)
        local comp; pcall(function() comp = ctx:get() end)
        if isControllerSound(name) or isControllerSound(fullName(comp)) then
            say(string.format("AC.SetSound #%d comp=%s new=%s", bump("AC.SetSound"), fullName(comp), name))
        end
    end)
end)
want("/Script/Engine.AudioComponent:SetSubmixSend", function(ctx, submix, level)
    pcall(function()
        local s, l; pcall(function() s = submix:get() end); pcall(function() l = level:get() end)
        local comp; pcall(function() comp = ctx:get() end)
        say(string.format("AC.SetSubmixSend #%d comp=%s submix=%s level=%s sound=%s",
            bump("AC.SetSubmixSend"), fullName(comp), show(s), tostring(l), show(prop(comp, "Sound"))))
    end)
end)
want("/Script/Engine.AudioComponent:Stop", function(ctx) pcall(componentLine, "AC.Stop", ctx) end)
want("/Script/Engine.AudioComponent:FadeOut", function(ctx) pcall(componentLine, "AC.FadeOut", ctx) end)

local function spawner(tag)
    return function(ctx, ...)
        local A = table.pack(ctx, ...)
        pcall(function()
            local desc = describeArgs(A)
            if isControllerSound(desc) then
                say(string.format("%s #%d %s", tag, bump(tag), desc))
            end
        end)
    end
end
want("/Script/Engine.GameplayStatics:SpawnSound2D", spawner("GS.SpawnSound2D"))
want("/Script/Engine.GameplayStatics:CreateSound2D", spawner("GS.CreateSound2D"))
want("/Script/Engine.GameplayStatics:SpawnSoundAttached", spawner("GS.SpawnSoundAttached"))
want("/Script/Engine.GameplayStatics:PlaySound2D", spawner("GS.PlaySound2D"))

-- GetPlatform: observed, never overridden here. The plugin's ladder is the place for that.
want("/Script/Hk_project.HKUtilities:GetPlatform", function() end, function(ctx, rv)
    pcall(function()
        local n = bump("GetPlatform")
        local v = "?"; pcall(function() v = tostring(rv:get()) end)
        if n <= 3 or n % 500 == 0 then say("GetPlatform #" .. n .. " -> " .. v) end
    end)
end)

-- The Blueprint entry points. PRE-hook: set DebugPS5Haptic before the body runs its gate.
local PC = "/Game/Technical/BP_HKPlayerController.BP_HKPlayerController_C:"
local function bpEntry(short)
    return function(ctx, ...)
        local A = table.pack(...)
        pcall(function()
            local pc; pcall(function() pc = ctx:get() end)
            local before = show(prop(pc, "DebugPS5Haptic"))
            local set = "no"
            if flagOn(DEBUG_FLAG) and valid(pc) then
                local ok = pcall(function() pc.DebugPS5Haptic = true end)
                set = ok and ("yes -> " .. show(prop(pc, "DebugPS5Haptic"))) or "FAILED"
            end
            say(string.format("BP.%s #%d DebugPS5Haptic(before)=%s set=%s args=%s",
                short, bump("BP." .. short), before, set, describeArgs(table.pack(nil, table.unpack(A, 1, A.n)))))
        end)
        pcall(dumpAll)
    end
end
want(PC .. "StartPS5Vibration", bpEntry("StartPS5Vibration"))
want(PC .. "StartPS5VibrationOnAudioComponent", bpEntry("StartPS5VibrationOnAudioComponent"))
want(PC .. "StartPS5ControllerSound", bpEntry("StartPS5ControllerSound"))
want(PC .. "StopPS5Vibration", bpEntry("StopPS5Vibration"))
want(PC .. "SetPS5VibrationLevel", function(ctx, lvl)
    pcall(function()
        local n = bump("BP.SetPS5VibrationLevel")
        if n <= 3 or n % 600 == 0 then
            local v; pcall(function() v = lvl:get() end)
            say("BP.SetPS5VibrationLevel #" .. n .. " level=" .. tostring(v))
        end
    end)
end)

-- ---------------------------------------------------------------------------------------
-- 5. A COMMAND FILE, so the probe can be driven from a shell while the game runs. Polled on
-- the async thread, executed on the game thread, deleted once read. One command per line:
--
--   debughaptic 0|1            set BP_HKPlayerController_C.DebugPS5Haptic
--   vib <Name> <fadeIn> <lvl>  call the Blueprint StartPS5Vibration with a _VIBE SoundWave
--   stop <fadeOut>             call the Blueprint StopPS5Vibration
--   play <Name>                ControllerVibration:SetSound + Play (bypasses the Blueprint)
--   send <SubmixPath> <lvl>    ControllerVibration:SetSubmixSend(submix, level)
--   class                      dump SCLASS_controllerVibration's Properties
--   state                      ControllerVibration.IsPlaying() and Sound
-- ---------------------------------------------------------------------------------------
local CMD = "stray-audio-probe.cmd"
local VIBE_DIR = "/Game/Sound/SFX/controllers/Vibrations/"

local function findSound(name)
    if name:sub(1, 1) == "/" then return StaticFindObject(name) end
    return StaticFindObject(VIBE_DIR .. name .. "." .. name)
end

local function dumpSoundClass()
    local o = StaticFindObject("/Game/Sound/tools/settings/SCLASS_controllerVibration.SCLASS_controllerVibration")
    if not valid(o) then say("CLASS not loaded"); return end
    say("CLASS " .. fullName(o))
    local props = prop(o, "Properties")
    if type(props) == "userdata" then
        for _, p in ipairs({ "Volume", "Pitch", "LowPassFilterFrequency", "AttenuationDistanceScale",
                             "bIsUISound", "bIsMusic", "bCenterChannelOnly", "bApplyAmbientVolumes",
                             "bReverb", "Default2DReverbSendAmount", "OutputTarget", "LoadingBehavior",
                             "DefaultSubmix", "bApplyEffects", "bAlwaysPlay" }) do
            local ok, v = pcall(function() return props[p] end)
            if ok and v ~= nil then say("   .Properties." .. p .. " = " .. show(v)) end
        end
    else
        say("   .Properties = " .. show(props))
    end
    say("   .ParentClass = " .. show(prop(o, "ParentClass")))
    say("   .ChildClasses = " .. arrayNames(prop(o, "ChildClasses")))
end

local function runCommand(line)
    local args = {}
    for w in line:gmatch("%S+") do args[#args + 1] = w end
    local c = args[1]
    if not c then return end
    local pc = FindFirstOf("HKPlayerController")
    local comp = valid(pc) and prop(pc, "ControllerVibration") or nil
    say("CMD " .. line)
    if c == "debughaptic" then
        if valid(pc) then
            local ok, err = pcall(function() pc.DebugPS5Haptic = (args[2] == "1") end)
            say("   DebugPS5Haptic -> " .. show(prop(pc, "DebugPS5Haptic")) .. (ok and "" or (" ERR " .. tostring(err))))
        end
    elseif c == "vib" then
        local s = findSound(args[2] or "")
        if not valid(s) then say("   sound not found: " .. tostring(args[2])); return end
        local ok, err = pcall(function() pc:StartPS5Vibration(s, tonumber(args[3]) or 0.0, tonumber(args[4]) or 1.0) end)
        say("   StartPS5Vibration(" .. fullName(s) .. ") -> " .. (ok and "called" or ("ERR " .. tostring(err))))
    elseif c == "stop" then
        local ok, err = pcall(function() pc:StopPS5Vibration(tonumber(args[2]) or 0.0) end)
        say("   StopPS5Vibration -> " .. (ok and "called" or ("ERR " .. tostring(err))))
    elseif c == "play" then
        local s = findSound(args[2] or "")
        if not valid(s) or not valid(comp) then say("   sound/component not found"); return end
        local ok, err = pcall(function() comp:SetSound(s); comp:Play(0.0) end)
        say("   ControllerVibration SetSound+Play -> " .. (ok and "called" or ("ERR " .. tostring(err))))
    elseif c == "send" then
        local sub = StaticFindObject(args[2] or "")
        if not valid(sub) or not valid(comp) then say("   submix/component not found"); return end
        local ok, err = pcall(function() comp:SetSubmixSend(sub, tonumber(args[3]) or 1.0) end)
        say("   SetSubmixSend(" .. fullName(sub) .. ") -> " .. (ok and "called" or ("ERR " .. tostring(err))))
    elseif c == "class" then
        dumpSoundClass()
    elseif c == "state" then
        if valid(comp) then
            local ok, playing = pcall(function() return comp:IsPlaying() end)
            say("   ControllerVibration playing=" .. (ok and tostring(playing) or "?") .. " sound=" .. show(prop(comp, "Sound")))
        end
    else
        say("   unknown command")
    end
end

LoopAsync(1000, function()
    local f = io.open(CMD, "r")
    if not f then return false end
    local text = f:read("a"); f:close(); os.remove(CMD)
    for line in text:gmatch("[^\r\n]+") do
        pcall(function() ExecuteInGameThread(function() pcall(runCommand, line) end) end)
    end
    return false
end)

-- ---------------------------------------------------------------------------------------
-- Registration, retried until the Blueprints have loaded; the async flusher; a periodic
-- ControllerVibration.IsPlaying() readout so "the game asked" and "the component is
-- playing" are separate lines.
-- ---------------------------------------------------------------------------------------
local tries = 0
LoopAsync(3000, function()
    tries = tries + 1
    for i = #hooks, 1, -1 do
        local h = hooks[i]
        local ok, err = pcall(function()
            if h.post then RegisterHook(h.path, h.fn, h.post) else RegisterHook(h.path, h.fn) end
        end)
        if ok then say("hooked " .. h.path); table.remove(hooks, i)
        elseif tries == 1 then say("not yet: " .. h.path .. " (" .. tostring(err) .. ")") end
    end
    if #hooks == 0 then say("ALL HOOKS REGISTERED"); return true end
    if tries > 40 then
        for _, h in ipairs(hooks) do say("NEVER HOOKED: " .. h.path) end
        return true
    end
    return false
end)

local ticks = 0
LoopAsync(1000, function()
    pcall(flush)
    ticks = ticks + 1
    if not dumped or ticks % 5 == 0 then
        pcall(function() ExecuteInGameThread(function()
            pcall(dumpAll)
            if dumped then
                pcall(function()
                    local pc = FindFirstOf("HKPlayerController")
                    local comp = valid(pc) and prop(pc, "ControllerVibration") or nil
                    if valid(comp) then
                        local ok, playing = pcall(function() return comp:IsPlaying() end)
                        say(string.format("TICK ControllerVibration playing=%s sound=%s DebugPS5Haptic=%s flag=%s counts: AC.Play=%d AC.FadeIn=%d AC.SetSound=%d SetSubmixSend=%d GetPlatform=%d",
                            ok and tostring(playing) or "?", show(prop(comp, "Sound")), show(prop(pc, "DebugPS5Haptic")),
                            tostring(flagOn(DEBUG_FLAG)), counts["AC.Play"] or 0, counts["AC.FadeIn"] or 0,
                            counts["AC.SetSound"] or 0, counts["AC.SetSubmixSend"] or 0, counts["GetPlatform"] or 0))
                    end
                end)
            end
        end) end)
    end
    return false
end)

say("StrayAudioProbe loaded; debug-haptic flag file = " .. DEBUG_FLAG)
print("[StrayAudioProbe] loaded; writing " .. OUT .. "\n")
