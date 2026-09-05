-- StrayHdrProbe: read what the ENGINE thinks about HDR output, instead of inferring it from our
-- hooks, and (behind a flag file) push the engine's own HDR enable path at runtime.
-- Writes <gamedir>/stray-hdr-probe.txt. Diagnostic; enable with enabled.txt for ONE launch.
--
-- 2026-09-05: with r.AllowHDR=1, r.HDR.EnableHDROutput=1, OutputDevice 3, Gamut 2, UI.CompositeMode 1,
-- -hdr, bUseHDRDisplayOutput=True and DXVK_HDR=1/PROTON_ENABLE_HDR=1 the engine stayed SDR while
-- vkd3d built an HDR10 swapchain (dark picture). First probe read: SupportsHDRDisplayOutput()=true
-- (that IS GRHISupportsHDROutput), IsHDREnabled()=true, nits 1000 - so the RHI and the settings
-- object both say yes and the switch still did not happen. This build reads the live cvars (the
-- 4.27 UKismetSystemLibrary::GetConsoleVariableIntValue takes ONE argument) and, if the file
-- stray_hdr_enable.on exists beside the exe, calls UGameUserSettings::EnableHDRDisplayOutput(true,
-- 1000) and the console command "r.HDR.EnableHDROutput 1" once, then keeps reading.
-- Threading (mods/StrayProbe's rule): engine reads inside ExecuteInGameThread, file I/O on the
-- async thread, everything pcall-wrapped.
local UEHelpers = require("UEHelpers")
local OUT = "stray-hdr-probe.txt"
local FLAG = "stray_hdr_enable.on"
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
local function flag_on()
    local f = io.open(FLAG, "rb"); if f then f:close(); return true end
    return false
end
local CVARS = { "r.AllowHDR", "r.HDR.EnableHDROutput", "r.HDR.Display.OutputDevice",
                "r.HDR.Display.ColorGamut", "r.HDR.UI.CompositeMode", "r.HDR.UI.Level" }
local pushed = false
local pushed_device = false
local function probe(do_push)
    local ks = UEHelpers.GetKismetSystemLibrary()
    local world = UEHelpers.GetWorld()
    if not valid(ks) or not valid(world) then say("not yet: kismet/world"); return false end
    local gus = FindFirstOf("GameUserSettings")
    if not valid(gus) then say("not yet: GameUserSettings"); return false end
    local function show(ok, r) return ok and tostring(r) or ("ERR " .. tostring(r)) end
    if do_push and not pushed then
        pushed = true
        local okP, errP = pcall(function() gus:EnableHDRDisplayOutput(true, 1000) end)
        say("PUSH EnableHDRDisplayOutput(true, 1000) -> " .. (okP and "called" or ("ERR " .. tostring(errP))))
        local okC, errC = pcall(function() ks:ExecuteConsoleCommand(world, "r.HDR.EnableHDROutput 1", nil) end)
        say("PUSH console r.HDR.EnableHDROutput 1 -> " .. (okC and "called" or ("ERR " .. tostring(errC))))
    end
    -- Force the OUTPUT DEVICE to ST2084 (3) once the settings apply has run: UGameUserSettings::
    -- EnableHDRDisplayOutput sets 5 (scRGB, linear, FP16) but a Shipping D3D12 build's HDR
    -- swapchain is PF_A2B10G10R10 (WindowsD3D12Device.cpp ~1370), and scRGB values in a 10-bit
    -- UNORM buffer decode as near-black (measured 2026-09-05: the game's own buffer captured black
    -- while the probe read OutputDevice=5). Runs when stray_hdr_device3.on exists, once.
    if not pushed_device and (function() local f = io.open("stray_hdr_device3.on", "rb"); if f then f:close(); return true end; return false end)() then
        pushed_device = true
        local okD, errD = pcall(function() ks:ExecuteConsoleCommand(world, "r.HDR.Display.OutputDevice 3", nil) end)
        say("PUSH console r.HDR.Display.OutputDevice 3 -> " .. (okD and "called" or ("ERR " .. tostring(errD))))
    end
    -- Arbitrary console commands from stray_hdr_cmd.txt (one per line, file deleted once read).
    local cf = io.open("stray_hdr_cmd.txt", "rb")
    if cf then
        local text = cf:read("a"); cf:close(); os.remove("stray_hdr_cmd.txt")
        for line in text:gmatch("[^\r\n]+") do
            local okX, errX = pcall(function() ks:ExecuteConsoleCommand(world, line, nil) end)
            say("CMD " .. line .. " -> " .. (okX and "called" or ("ERR " .. tostring(errX))))
        end
    end
    local okS, supports = pcall(function() return gus:SupportsHDRDisplayOutput() end)
    local okE, enabled  = pcall(function() return gus:IsHDREnabled() end)
    local okN, nits     = pcall(function() return gus:GetCurrentHDRDisplayNits() end)
    say(string.format("GUS SupportsHDRDisplayOutput()=%s IsHDREnabled()=%s nits=%s",
        show(okS, supports), show(okE, enabled), show(okN, nits)))
    local parts = {}
    for _, cv in ipairs(CVARS) do
        local ok, v = pcall(function() return ks:GetConsoleVariableIntValue(cv) end)
        parts[#parts + 1] = cv .. "=" .. (ok and tostring(v) or ("ERR:" .. tostring(v):sub(1, 60)))
    end
    say("  cvars " .. table.concat(parts, " "))
    return true
end
local ticks = 0
LoopAsync(5000, function()
    pcall(flush)
    ticks = ticks + 1
    local push = flag_on()
    if ticks <= 24 or ticks % 12 == 0 or (push and not pushed) then
        pcall(function() ExecuteInGameThread(function() pcall(probe, push) end) end)
    end
    return false
end)
say("StrayHdrProbe loaded (flag file for the push: " .. FLAG .. ")")
