-- StrayHdr: keep UE 4.27's HDR output on the ST2084 (PQ) output device this stack can present,
-- and lift the HDR image's mid-tones to the SDR reference through the one gain that reaches the
-- HDR branch: APlayerCameraManager::ColorScale.
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
-- BRIGHTNESS (docs/RESEARCH-UE4-HDR-BRIGHTNESS.md, measured in nits from PQ captures): HDR on
-- device 3 is ~17x (4 EV) darker than the SDR path in the MID-TONES and only 1.2-1.7x at the
-- peak. Everything the console reaches acts on the SDR branch only (r.Color.Mid, r.TonemapperGamma,
-- the film curve, the game's gamma), r.DefaultFeature.AutoExposure.Bias is a construction-time
-- default, and the camera component's AutoExposureBias is rewritten by the game ~8 s after every
-- camera respawn. The one uniform gain on the GRADED colour right before the ACES ODT is
-- APlayerCameraManager::ColorScale with bEnableColorScaling (LocalPlayer.cpp:763-769 ->
-- View.ColorScale -> PostProcessCombineLUTs.usf:344 -> ACESOutputTransforms1000). It does not
-- feed back into eye adaptation. k = 6.7 matches the mid-tones (peak ~600 nits); k = 1.5 matches
-- only the peak. UNCONFIRMED: whether the game ever rewrites ColorScale - the read-back below logs
-- it once if it does.
--
-- LIVE TUNING: write a number into <gamedir>/stray-hdr-scale.txt (e.g. 4.0); re-read every tick.
-- Absent file = COLOR_SCALE below. 1.0 = the engine's own image.
--
-- COST RULE (measured the same day): every FindFirstOf/FindAllOf is a full UObject-array scan on
-- the game thread, 25-50 ms here, and polling one every 5 s produced a blink every 5 s under frame
-- generation. So: the cvar fix polls only cvars and stops once applied; the camera manager is
-- looked up ONCE and again only when the cached object has gone invalid (a level change), and the
-- per-tick work on it is three property reads.
-- Threading (mods/StrayProbe's rule): engine reads inside ExecuteInGameThread, file I/O on the
-- async thread, everything pcall-wrapped.
local UEHelpers = require("UEHelpers")
local OUT = "stray-hdr.txt"
local SCALE_FILE = "stray-hdr-scale.txt"
local WANT_DEVICE = 3            -- ST2084 1000 nits; 4 = 2000 nits
local WANT_GAMUT = 2             -- Rec2020
local UI_LEVEL = nil             -- r.HDR.UI.Level (HUD brightness in the HDR composite), nil = default 1.0
local COLOR_SCALE = 6.7          -- ColorScale gain when no file overrides it; nil = leave the engine alone
local TICK_MS = 1000
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
local function desired_scale()
    if COLOR_SCALE == nil then return nil end
    local f = io.open(SCALE_FILE, "rb")
    if not f then return COLOR_SCALE end
    local t = f:read("a"); f:close()
    local n = tonumber((t or ""):match("[-+]?%d+%.?%d*"))
    if n == nil or n <= 0 then return COLOR_SCALE end
    return n
end

-- ---- the cvar fix, once ----
local cvars_done = false
local function fix_cvars(ks, world)
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
    say(string.format("HDR output ON: EnableHDROutput=%s device=%d gamut=%d.", tostring(enabled), WANT_DEVICE, WANT_GAMUT))
    return true
end

-- ---- the colour scale, on the camera manager ----
local pcm = nil                 -- cached APlayerCameraManager
local pcm_name = nil
local applied_k = nil           -- what we last wrote
local fight_logged = false
local rewrites = 0
local function find_pcm()
    local ok, found = pcall(function() return FindFirstOf("PlayerCameraManager") end)
    if ok and valid(found) then
        pcm = found
        local okN, n = pcall(function() return found:GetFullName() end)
        pcm_name = okN and n or "?"
        applied_k = nil
        say("colorScale: camera manager " .. pcm_name)
        return true
    end
    return false
end
local function write_scale(k)
    local ok, err = pcall(function()
        pcm.bEnableColorScaling = true
        pcm.ColorScale.X = k
        pcm.ColorScale.Y = k
        pcm.ColorScale.Z = k
    end)
    if not ok then say("colorScale: write failed: " .. tostring(err)); return false end
    return true
end
local function apply_scale(k)
    if not valid(pcm) then
        if not find_pcm() then return end
    end
    -- Read back first: if the value we wrote is gone, the game rewrote it (log once, count).
    local okR, en, x = pcall(function() return pcm.bEnableColorScaling, pcm.ColorScale.X end)
    if applied_k ~= nil and okR and (en ~= true or math.abs((x or 0) - applied_k) > 0.001) then
        rewrites = rewrites + 1
        if not fight_logged then
            fight_logged = true
            say(string.format("colorScale: the game REWROTE it (enabled=%s X=%.3f after we wrote %.3f); re-asserting every tick and counting", tostring(en), x or 0, applied_k))
        end
        applied_k = nil
    end
    if applied_k == k then return end
    if write_scale(k) then
        say(string.format("colorScale: %.3f applied to %s (rewrites so far %d)", k, pcm_name or "?", rewrites))
        applied_k = k
    end
end

local function tick()
    local ks = UEHelpers.GetKismetSystemLibrary()
    if not valid(ks) then return end
    if not cvars_done then
        local world = UEHelpers.GetWorld()
        if not valid(world) then return end
        cvars_done = fix_cvars(ks, world)
        if not cvars_done then return end   -- HDR not on: the scale would be wrong for SDR, so wait
    end
    local k = desired_scale()
    if k ~= nil then apply_scale(k) end
end
LoopAsync(TICK_MS, function()
    pcall(flush)
    pcall(function() ExecuteInGameThread(function() pcall(tick) end) end)
    return false
end)
say(string.format("StrayHdr loaded: output device %d gamut %d once HDR is on; ColorScale %.2f (tune via %s); tick %ds",
    WANT_DEVICE, WANT_GAMUT, COLOR_SCALE or 1.0, SCALE_FILE, TICK_MS / 1000))
