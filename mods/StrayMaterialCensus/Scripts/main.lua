-- StrayMaterialCensus: how many of Stray's TRANSLUCENT materials write velocity?
--
-- WHY THIS EXISTS. Our motion-vector reconstruction carries 96.09% of the frame (facts §37).
-- The pixels it CANNOT be right about are translucent ones that write neither depth nor
-- velocity: the reconstruction reads depth, so it hands such a pixel the motion of whatever
-- opaque surface is behind it — smoke, steam and neon glow moving with the wall.
--
-- Whether that happens here is a CONTENT question, not an engine one (facts §42). UE 4.27 fully
-- supports translucent velocity; the gate is one per-material flag with no cvar above it:
--
--     UMaterial::IsTranslucencyWritingVelocity()          Material.cpp:5723-5726
--         return bOutputTranslucentVelocity && IsTranslucentBlendMode(GetBlendMode());
--
-- The pak route to that answer is blocked — the entries are Oodle-compressed and no ooz build
-- exists on this machine — but the flag is a UPROPERTY, so reflection reads it straight off the
-- LOADED materials. That is a capability the UE4SS migration bought and the ReShade era did not
-- have, and it is worth noticing that the blocked route was the ReShade-era one.
--
-- COST, AND WHY IT IS SAFE. FindAllOf walks the whole object array, which the probe's own
-- history records as a visible spike when done every second. So this is ONE-SHOT and TRIGGERED:
-- it does nothing at all until someone creates the flag file, runs once, writes the report, and
-- removes the flag. An idle session pays a single io.open per tick and nothing else.
--
-- USE:  touch stray-material-census        (in the game directory)
--       ... play for a moment so the area's materials are loaded ...
--       read stray-material-census.txt
--
-- READ IT IN GAMEPLAY, NOT THE MENU. Only loaded materials are visible, so a menu census
-- describes the menu. The interesting content is The Slums.

local UEHelpers = require("UEHelpers")

local FLAG   = "stray-material-census"
local REPORT = "stray-material-census.txt"
local TICK_MS = 2000

-- IsTranslucentBlendMode, MaterialShared.h: anything that is not Opaque(0) or Masked(1).
local BLEND_OPAQUE, BLEND_MASKED = 0, 1
local BLEND_NAME = { [0]="Opaque", [1]="Masked", [2]="Translucent", [3]="Additive",
                     [4]="Modulate", [5]="AlphaComposite", [6]="AlphaHoldout" }

local function atomicWrite(path, text)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "wb")
    if not f then return end
    f:write(text); f:close()
    if not os.rename(tmp, path) then
        os.remove(path); os.rename(tmp, path)   -- Windows rename does not overwrite by itself
    end
end

local function flagPresent()                    -- ASYNC THREAD ONLY, like StrayProbe's
    local f = io.open(FLAG, "rb")
    if f then f:close(); return true end
    return false
end

-- Everything below runs on the GAME THREAD, once, inside one ExecuteInGameThread.
local function census()
    local total, translucent, withVelocity, unreadable = 0, 0, 0, 0
    local byBlend, offenders = {}, {}

    local ok, materials = pcall(FindAllOf, "Material")
    if not ok or materials == nil then
        return "error=FindAllOf(\"Material\") returned nothing; UE4SS could not enumerate\n"
    end

    for _, m in pairs(materials) do
        total = total + 1
        -- Every read is guarded: a UPROPERTY that is absent on this build must degrade to a
        -- counted "unreadable", never to a wrong number that gets quoted later.
        local okB, blend = pcall(function() return m.BlendMode end)
        if not okB or blend == nil then
            unreadable = unreadable + 1
        else
            blend = tonumber(blend) or -1
            byBlend[blend] = (byBlend[blend] or 0) + 1
            if blend ~= BLEND_OPAQUE and blend ~= BLEND_MASKED then
                translucent = translucent + 1
                local okV, v = pcall(function() return m.bOutputTranslucentVelocity end)
                if not okV or v == nil then
                    unreadable = unreadable + 1
                elseif v == true then
                    withVelocity = withVelocity + 1
                elseif #offenders < 25 then
                    local okN, n = pcall(function() return m:GetFullName() end)
                    offenders[#offenders + 1] = (okN and n or "<unnamed>")
                end
            end
        end
    end

    local out = {}
    out[#out+1] = string.format("t=%d", os.time())
    out[#out+1] = string.format("materials=%d translucent=%d writingVelocity=%d unreadable=%d",
        total, translucent, withVelocity, unreadable)
    if translucent > 0 then
        out[#out+1] = string.format("translucentWritingVelocityPct=%.2f",
            100.0 * withVelocity / translucent)
    end
    for b, n in pairs(byBlend) do
        out[#out+1] = string.format("blend[%s]=%d", BLEND_NAME[b] or tostring(b), n)
    end
    out[#out+1] = "-- translucent materials NOT writing velocity (first 25): every pixel one of"
    out[#out+1] = "-- these covers is handed the motion of the opaque surface behind it"
    for _, n in ipairs(offenders) do out[#out+1] = "  " .. n end
    return table.concat(out, "\n") .. "\n"
end

print("[StrayMaterialCensus] armed; create " .. FLAG .. " in the game dir to run it once\n")

-- EXECUTEINGAMETHREAD IS ASYNCHRONOUS. It QUEUES the closure and returns immediately, so
-- writing the report in the same tick that queues it writes whatever the variable held before
-- the census ran — measured 2026-09-04, the first run produced exactly "error=census did not
-- run" with the flag consumed, which looks identical to a census that failed.
--
-- StrayProbe already solved this and this mod should have copied it: the game thread only
-- STORES into a table, and a LATER async tick does the file I/O. Two ticks, never one.
local queued, ready, report = false, false, nil

LoopAsync(TICK_MS, function()
    if ready then                                 -- a previous tick's census has landed
        atomicWrite(REPORT, report or "error=census produced nothing\n")
        os.remove(FLAG)                           -- one-shot: consumed only once it has RUN
        print("[StrayMaterialCensus] wrote " .. REPORT .. "\n")
        return true                               -- stop the loop; re-arm by touching the flag
    end
    if queued then return false end               -- still waiting on the game thread
    if not flagPresent() then return false end

    queued = true
    pcall(function() ExecuteInGameThread(function()
        local ok, r = pcall(census)
        report = ok and r or ("error=" .. tostring(r) .. "\n")
        ready = true                              -- read by the async thread on a later tick
    end) end)
    return false
end)
