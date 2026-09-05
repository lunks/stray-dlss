-- StrayHdr: keep UE 4.27's HDR output on the ST2084 (PQ) output device this stack can present.
-- Writes <gamedir>/stray-hdr.txt. Permanent helper; enable with enabled.txt.
--
-- WHY THIS EXISTS (measured 2026-09-05, docs/STRAY-RENDERING-FACTS.md, HDR section):
--   * Stray ships no HDR: IsHDRAllowed() is false without `-hdr` / r.AllowHDR=1, and
--     UGameUserSettings::ApplyNonResolutionSettings forces r.HDR.EnableHDROutput back to 0 unless
--     GameUserSettings.ini carries bUseHDRDisplayOutput=True. Those two are config, not this mod.
--   * When the cvar flips to 1, the engine's HDR sink (UnrealEngine.cpp HDRSettingChangedSinkCallback)
--     calls FPlatformMisc::ChooseHDRDeviceAndColorGamut(vendor, nits) and on this NVIDIA box sets
--     r.HDR.Display.OutputDevice=5 (scRGB, linear, meant for an FP16 swapchain). A Shipping D3D12
--     build's HDR swapchain is PF_A2B10G10R10 (WindowsD3D12Device.cpp ~1370) and vkd3d-proton
--     presents it as HDR10/ST2084, so scRGB values land in a 10-bit UNORM PQ buffer: the game's own
--     back buffer captured BLACK. Output device 3 (ST2084, 1000 nits) is the consistent choice and
--     the picture is right with it. The sink runs only on the cvar CHANGE, so a value written after
--     it sticks; an Engine.ini value is overwritten by it. Hence this mod.
--
-- COST RULE (measured the same day, the hard way): the first cut re-read the cvars on the game
-- thread every 5 s for the whole session and produced a 40-50 ms stall every 5.00 s - a visible
-- blink under frame generation (CLAUDE.md §2.11: one FindFirstOf per second is a visible spike;
-- UEHelpers.GetWorld() is one). So: poll every 2 s ONLY until the fix has been applied, then STOP
-- the loop for good. Nothing of this mod runs after that.
-- Threading (mods/StrayProbe's rule): engine reads inside ExecuteInGameThread, file I/O on the
-- async thread, everything pcall-wrapped.
local UEHelpers = require("UEHelpers")
local OUT = "stray-hdr.txt"
local WANT_DEVICE = 3   -- ST2084 1000 nits; 4 = 2000 nits
local WANT_GAMUT = 2    -- Rec2020
-- Brightness. This 4.27 build has no HDR scene multiplier (r.HDR.Aces.SceneColorMultiplier is
-- UE5), so the levers are the tonemapper's own, applied before the ACES output transform:
--   r.Color.Mid   midtone level, engine default 0.5; 0.55 lifts mids ~10% (nil = leave alone)
--   r.HDR.UI.Level HUD brightness in the HDR composite, engine default 1.0 (nil = leave alone)
local SCENE_MID = 0.55
local UI_LEVEL = nil
local MAX_POLLS = 150   -- 2 s each: give up after 5 minutes (HDR is off in this config, or engine never switched)
local pending = {}
local function say(l) pending[#pending + 1] = string.format("%s %s", os.date("%H:%M:%S"), tostring(l)) end
local function flush()
    if #pending == 0 then return end
    local f = io.open(OUT, "ab"); if not f then return end
    for _, l in ipairs(pending) do f:write(l, "\n") end
    f:close(); pending = {}
end
local function valid(o)
    local ok, v = pcall(function() return o ~= nil and o:IsValid() end)
    return ok and v == true
end
local done = false
local polls = 0
local function tick()
    local ks = UEHelpers.GetKismetSystemLibrary()
    if not valid(ks) then return end
    local okE, enabled = pcall(function() return ks:GetConsoleVariableIntValue("r.HDR.EnableHDROutput") end)
    if not okE or enabled == 0 then return end  -- engine has not switched (yet); keep polling
    local okD, dev = pcall(function() return ks:GetConsoleVariableIntValue("r.HDR.Display.OutputDevice") end)
    local okG, gam = pcall(function() return ks:GetConsoleVariableIntValue("r.HDR.Display.ColorGamut") end)
    local world = UEHelpers.GetWorld()  -- the one lookup that costs; taken once, here
    if not valid(world) then return end
    if okD and dev ~= WANT_DEVICE then
        pcall(function() ks:ExecuteConsoleCommand(world, "r.HDR.Display.OutputDevice " .. WANT_DEVICE, nil) end)
        say(string.format("r.HDR.Display.OutputDevice %d -> %d (the engine's NVIDIA choice is scRGB, wrong for a 10-bit PQ swapchain)", dev, WANT_DEVICE))
    end
    if okG and gam ~= WANT_GAMUT then
        pcall(function() ks:ExecuteConsoleCommand(world, "r.HDR.Display.ColorGamut " .. WANT_GAMUT, nil) end)
        say(string.format("r.HDR.Display.ColorGamut %d -> %d", gam, WANT_GAMUT))
    end
    if SCENE_MID ~= nil then
        pcall(function() ks:ExecuteConsoleCommand(world, string.format("r.Color.Mid %.3f", SCENE_MID), nil) end)
        say(string.format("r.Color.Mid -> %.3f (midtone lift; engine default 0.5)", SCENE_MID))
    end
    if UI_LEVEL ~= nil then
        pcall(function() ks:ExecuteConsoleCommand(world, string.format("r.HDR.UI.Level %.3f", UI_LEVEL), nil) end)
        say(string.format("r.HDR.UI.Level -> %.3f (HUD brightness in the HDR composite; engine default 1.0)", UI_LEVEL))
    end
    say(string.format("HDR output ON: EnableHDROutput=%s device=%d gamut=%d. Done; this mod does nothing further this session.",
        tostring(enabled), WANT_DEVICE, WANT_GAMUT))
    done = true
end
LoopAsync(2000, function()
    pcall(flush)
    if done then return true end
    polls = polls + 1
    if polls > MAX_POLLS then
        say("HDR output never switched on within 5 minutes (r.HDR.EnableHDROutput stayed 0): needs r.AllowHDR=1 or -hdr, " ..
            "bUseHDRDisplayOutput=True in GameUserSettings.ini, DXVK_HDR=1 / PROTON_ENABLE_HDR=1. Giving up for this session.")
        pcall(flush)
        return true
    end
    pcall(function() ExecuteInGameThread(function() pcall(tick) end) end)
    return false
end)
say("StrayHdr loaded: will set r.HDR.Display.OutputDevice " .. WANT_DEVICE .. " once HDR output is on, then stop")
