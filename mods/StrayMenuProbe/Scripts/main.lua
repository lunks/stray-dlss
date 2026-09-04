-- StrayMenuProbe: answer the two questions docs/RESEARCH-STRAY-MENU-OPTIONS.md leaves
-- UNCONFIRMED, with the FEWEST POSSIBLE READS, before anything is injected into the menu.
--
-- READ-ONLY. Constructs nothing, adds nothing, writes nothing into the game.
--
-- IT CRASHED THE GAME ONCE, 2026-09-04 16:54:31 (EXCEPTION_ACCESS_VIOLATION reading 0x10).
-- The first version read the page INSIDE NotifyOnNewObject. That notify fires at object
-- CONSTRUCTION -- before the WidgetTree is built and before any BindWidget property is
-- populated -- so `page.ScrollBox` was an unbound pointer and reading it faulted.
--
-- THREE RULES THAT COST A SESSION, all of which the first version broke:
--
--   1. pcall IS NOT A SAFETY NET. It catches Lua errors. A native access violation inside
--      UE4SS's property binding is not a Lua error, so wrapping every read in pcall protected
--      exactly nothing. obj:IsValid() does not help either -- calling IsValid on a garbage
--      pointer IS the dereference.
--
--   2. A PRINTED WARNING IS NOT A GUARD. Its own output said `WidgetTree: <invalid>` one line
--      before it died. The signal was there; no rule acted on it. Now an invalid tree means
--      refuse and retry, not carry on.
--
--   3. EVERY READ IS A RISK, SO TAKE THE FEWEST. This version asks two questions and stops.
--      The full tree walk is gone -- it was most of the reads and none of the decision.
--
-- Needs NO injected input: the human opening Options -> Graphics is the trigger.
-- Engine touches go through ExecuteInGameThread (mods/StrayFur's own header records an
-- access violation from touching UObjects off the game thread).
local OUT   = "stray-menu-probe.txt"
local lines = {}

local function say(s)
    print("[StrayMenuProbe] " .. s .. "\n")
    lines[#lines + 1] = s
end

local function flush()
    local f = io.open(OUT, "wb")
    if not f then return end
    f:write(table.concat(lines, "\n") .. "\n")
    f:close()
end

local function nameOf(o)
    if o == nil then return "nil" end
    local ok, n = pcall(function() return o:GetFullName() end)
    return ok and n or "<unreadable>"
end

local reported = false
local pending  = nil   -- recorded at notify; NEVER read there

-- THE GATE. An unbound BindWidget property is not nil, it is garbage. The cheapest thing that
-- separates "page constructed" from "page ready" is whether its WidgetTree reads back valid.
-- Nothing below this line runs until it does.
local function page_is_ready(page)
    if page == nil then return false end
    local ok, v = pcall(function() return page:IsValid() end)
    if not ok or v ~= true then return false end
    local ok2, tree = pcall(function() return page.WidgetTree end)
    if not ok2 or tree == nil then return false end
    local ok3, tv = pcall(function() return tree:IsValid() end)
    return ok3 and tv == true
end

local function probe(page)
    lines = {}
    say("=== StrayMenuProbe (minimal) ===")
    say("page: " .. nameOf(page))

    -- Q1: is the ScrollBox's content the VerticalBox the doc guessed? The object dump cannot
    -- say: its [or:] field is UObject Outer, and every widget in a WidgetTree shares the tree
    -- as Outer, so a child and a sibling look identical in static text.
    local ok, scroll = pcall(function() return page.ScrollBox end)
    if not ok or scroll == nil then say("Q1: ScrollBox unreadable"); flush(); return end
    say("ScrollBox: " .. nameOf(scroll))

    local ok2, slots = pcall(function() return scroll.Slots end)
    if not ok2 or slots == nil then say("Q1: ScrollBox.Slots unreadable"); flush(); return end
    local okn, n = pcall(function() return #slots end)
    if not okn or n == nil then say("Q1: Slots count unreadable"); flush(); return end
    say("ScrollBox.Slots count: " .. n)
    for i = 1, n do
        local okc, c = pcall(function() return slots[i].Content end)
        say(string.format("  Slots[%d].Content = %s", i, okc and nameOf(c) or "<unreadable>"))
    end

    -- Q2: is m_screenPercentages really regenerated from supported display modes? If so, our
    -- own values would be overwritten on every page open, which settles relabel-vs-own-row.
    local okp, set = pcall(function() return page.m_screenPercentages end)
    if not okp or set == nil then
        say("Q2: m_screenPercentages unreadable")
    else
        local okc, cnt = pcall(function() return #set end)
        if okc and cnt then
            local v = {}
            for i = 1, cnt do
                local oki, x = pcall(function() return set[i] end)
                v[#v + 1] = oki and tostring(x) or "?"
            end
            say("m_screenPercentages: count=" .. cnt .. " values=" .. table.concat(v, ", "))
        else
            say("m_screenPercentages: present, count unreadable")
        end
    end

    say("=== end ===")
    flush()
    reported = true
end

for _, cls in ipairs({ "/Script/Hk_project.GraphicsSettingsWidget",
                       "/Game/GUI/GameMenus/UMG_GraphicsSettings.UMG_GraphicsSettings_C" }) do
    local ok = pcall(function()
        NotifyOnNewObject(cls, function(obj) pending = obj end)
    end)
    print("[StrayMenuProbe] watching " .. cls .. ": " .. tostring(ok) .. "\n")
end

LoopAsync(1000, function()
    if reported then return false end
    ExecuteInGameThread(function()
        if reported then return end
        if not page_is_ready(pending) then return end
        pcall(probe, pending)
    end)
    return false
end)

print("[StrayMenuProbe] armed (deferred, gated). Open Options -> Graphics.\n")
