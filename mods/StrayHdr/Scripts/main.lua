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
--     it sticks; an Engine.ini value is overwritten by it.
--
-- NOT HERE, BY DECISION (2026-09-05): an exposure lever. The image is darker under the 1000-nit
-- ACES transform than under the SDR tonemap, and three levers were tried: the cvars (inert, the
-- game's volumes own the bias), the PostProcessVolumes' AutoExposureBias by reflection (inert, the
-- view sits outside them in the Slums), and the cat camera component's PostProcessSettings (reaches
-- the view, but the game rewrites that struct ~8 s after each camera respawn and the observed
-- brightness did not track the number). The user chose to keep only the output-device fix.
--
-- COST RULE (measured the same day): every FindFirstOf/FindAllOf is a full UObject-array scan on
-- the game thread, 25-50 ms here, and polling one every 5 s produced a blink every 5 s under frame
-- generation. This helper polls only cvars (cheap) and stops entirely once the fix is applied.
-- Threading (mods/StrayProbe's rule): engine reads inside ExecuteInGameThread, file I/O on the
-- async thread, everything pcall-wrapped.
local UEHelpers = require("UEHelpers")
local OUT = "stray-hdr.txt"
local WANT_DEVICE = 3            -- ST2084 1000 nits; 4 = 2000 nits
local WANT_GAMUT = 2             -- Rec2020
local UI_LEVEL = nil             -- r.HDR.UI.Level (HUD brightness in the HDR composite), nil = default 1.0
local TICK_MS = 5000
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
local function fix_cvars()
    local ks = UEHelpers.GetKismetSystemLibrary()
    local world = UEHelpers.GetWorld()
    if not valid(ks) or not valid(world) then return false end
    local okE, enabled = pcall(function() return ks:GetConsoleVariableIntValue("r.HDR.EnableHDROutput") end)
    if not okE or enabled == 0 then return false end    -- HDR not on yet (or not configured); keep polling
    local okD, dev = pcall(function() return ks:GetConsoleVariableIntValue("r.HDR.Display.OutputDevice") end)
    local okG, gam = pcall(function() return ks:GetConsoleVariableIntValue("r.HDR.Display.ColorGamut") end)
    if okD and dev ~= WANT_DEVICE then
        pcall(function() ks:ExecuteConsoleCommand(world, "r.HDR.Display.OutputDevice " .. WANT_DEVICE, nil) end)
        say(string.format("r.HDR.Display.OutputDevice %d -> %d (the engine's NVIDIA choice is scRGB, wrong for a 10-bit PQ swapchain)", dev, WANT_DEVICE))
    end
    if okG and gam ~= WANT_GAMUT then
        pcall(function() ks:ExecuteConsoleCommand(world, "r.HDR.Display.ColorGamut " .. WANT_GAMUT, nil) end)
        say(string.format("r.HDR.Display.ColorGamut %d -> %d", gam, WANT_GAMUT))
    end
    if UI_LEVEL ~= nil then
        pcall(function() ks:ExecuteConsoleCommand(world, string.format("r.HDR.UI.Level %.3f", UI_LEVEL), nil) end)
        say(string.format("r.HDR.UI.Level -> %.3f", UI_LEVEL))
    end
    say(string.format("HDR output ON: EnableHDROutput=%s device=%d gamut=%d. Done; no further polling.", tostring(enabled), WANT_DEVICE, WANT_GAMUT))
    return true
end

LoopAsync(TICK_MS, function()
    pcall(flush)
    if done then pcall(flush); return true end     -- returning true ends the loop
    pcall(function() ExecuteInGameThread(function() local ok, r = pcall(fix_cvars); done = ok and r == true end) end)
    return false
end)
say(string.format("StrayHdr loaded: output device %d gamut %d once HDR is on; polling every %ds until then", WANT_DEVICE, WANT_GAMUT, TICK_MS / 1000))
