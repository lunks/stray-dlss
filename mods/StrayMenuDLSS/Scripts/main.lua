-- StrayMenuDLSS: make Stray's own Screen Percentage row offer the DLSS presets, and nothing else.
--
-- STAGE A. It writes ONE property: `m_screenPercentages` on the live GraphicsSettingsWidget,
-- replacing the shipped 50..200 step-10 set with the five DLSS ratios. It creates no widget,
-- swaps no pointer, and rewrites no label.
--
-- WHY THIS AND NOT A NEW ROW -- all measured on the box 2026-09-04, do not re-derive:
--
--   The added row RENDERS but the gamepad cannot reach it, and a Collapsed stock row is STILL
--   a focus stop. Slate skips Collapsed widgets automatically, so navigation is not Slate focus.
--   `bIsFocusable` reads FALSE on the stock row that IS reachable; `SetIsFocusable` fails
--   outright; `SetNavigationRuleExplicit` succeeds and does nothing. Three independent lines,
--   one conclusion: the page walks its own native list of the eleven BindWidget slots, and
--   nothing we add can join it (docs/RESEARCH-STRAY-MENU-OPTIONS.md §3, now resolved NEGATIVE).
--
--   Using the stock row keeps navigation, keeps the native handler that actually writes
--   HKGameUserSettings.ScreenPercentage, and keeps the native label. We change only WHICH
--   values it steps through. There is nothing left to fight.
--
--   The percentages ARE the presets, so the labels stay honest: 33/50/58/67/100 read as
--   Ultra Performance / Performance / Balanced / Quality / DLAA.
--
--   And it removes 110..200 from reach, which is not cosmetic: 200% renders 7680x4320 and
--   downsamples, and DLSS cannot accept an input larger than its output -- our own matcher
--   already refuses it ("downsampling, not TAA upscaling"), so that stop silently turned DLSS
--   off (CLAUDE.md §5, DLAA section).
--
-- CRASH RULES, paid for twice today (16:54:31 reading 0x10, 17:13:33 reading -1):
--   1. NotifyOnNewObject fires at CONSTRUCTION, before WidgetTree and BindWidget properties
--      are populated. Record the object there; read nothing.
--   2. pcall catches Lua errors, NOT native access violations. It is not a safety net.
--   3. Announce each new engine call BEFORE making it, so a fault names itself.
--
-- RE-APPLIES ON EVERY PAGE OPEN. The page is reconstructed each time the menu is opened -- the
-- first build latched after one success and the change rolled back on reopen.
local UEHelpers = require("UEHelpers")

local OUT = "stray-menu-dlss.txt"

-- DLAA 100, Quality 67, Balanced 58, Performance 50, Ultra Performance 33.
-- UNCONFIRMED: 33 is below UE 4.27's own kMinTAAUpsampleResolutionFraction (0.5), which our
-- view search also gates on. It is offered so the box can answer whether it works rather than
-- us guessing; if it misbehaves, drop it from this table and nothing else changes.
local PRESET_PCT = { 33, 50, 58, 67, 100 }

local pending, applied_to = nil, {}   -- applied_to: page fullname -> true, so we do it once PER PAGE

local function log(s)
    print("[StrayMenuDLSS] " .. s .. "\n")
    local f = io.open(OUT, "ab"); if f then f:write(s .. "\n"); f:close() end
end

local function nameOf(o)
    if o == nil then return "nil" end
    local ok, n = pcall(function() return o:GetFullName() end)
    return ok and n or "<unreadable>"
end

-- Rule 1+3: nothing is read until the tree reads back valid.
local function page_is_ready(p)
    if p == nil then return false end
    local ok, v = pcall(function() return p:IsValid() end)
    if not ok or v ~= true then return false end
    local ok2, t = pcall(function() return p.WidgetTree end)
    if not ok2 or t == nil then return false end
    local ok3, tv = pcall(function() return t:IsValid() end)
    return ok3 and tv == true
end

local function dump_set(set, label)
    -- MEASURED: a TSet is NOT indexable -- `set[i]` returns nil for every i, which is why the
    -- first probe read 16 nils. ForEach is the accessor (UE4SS Lua TSet API).
    local vals = {}
    local ok = pcall(function()
        set:ForEach(function(e)
            local okg, v = pcall(function() return e:get() end)
            vals[#vals + 1] = tostring(okg and v or e)
        end)
    end)
    log(label .. " (ForEach ok=" .. tostring(ok) .. "): " .. table.concat(vals, ", "))
    return vals
end

local function apply(page)
    local key = nameOf(page)
    if applied_to[key] then return end

    log("=== StrayMenuDLSS stage A ===")
    log("page: " .. key)

    log("about to read m_screenPercentages")
    local okp, set = pcall(function() return page.m_screenPercentages end)
    if not okp or set == nil then log("ABORT: m_screenPercentages unreadable"); return end

    local okc, before = pcall(function() return #set end)
    log("count before: " .. tostring(okc and before))
    dump_set(set, "values before")

    log("about to Empty() the set -- if this is the last line, Empty is the fault")
    local oke = pcall(function() set:Empty() end)
    log("Empty returned " .. tostring(oke))
    if not oke then log("ABORT: Empty failed, set left alone"); return end

    for _, pct in ipairs(PRESET_PCT) do
        local oka = pcall(function() set:Add(pct) end)
        log("Add(" .. pct .. ") -> " .. tostring(oka))
    end

    local okc2, after = pcall(function() return #set end)
    log("count after: " .. tostring(okc2 and after))
    dump_set(set, "values after")

    applied_to[key] = true
    log("=== done for this page ===")
end

for _, cls in ipairs({ "/Script/Hk_project.GraphicsSettingsWidget",
                       "/Game/GUI/GameMenus/UMG_GraphicsSettings.UMG_GraphicsSettings_C" }) do
    pcall(function() NotifyOnNewObject(cls, function(o) pending = o end) end)
end

-- No `done` latch: the page is rebuilt on every menu open, so this must re-apply. applied_to
-- keys on the page's own name so one page is only written once, however long the menu is open.
LoopAsync(1000, function()
    ExecuteInGameThread(function()
        if page_is_ready(pending) then pcall(apply, pending) end
    end)
    return false
end)

log("[armed] stage A: rewrite m_screenPercentages to " .. table.concat(PRESET_PCT, "/") ..
    ". Open Options -> Graphics.")
