-- StrayDebugMenu: open the debug UI Stray itself ships.
--
-- The game carries its own debug widgets (found in the pak index, and DefaultGame.ini's
-- [/Script/Hk_project.HKGameSettings] names one of them outright):
--     HUDDebugWidgetClass=/Game/GUI/HUD/UMG_HUD_Debug.UMG_HUD_Debug_C
--     /Game/GUI/HUD/UMG_DebugMenu.UMG_DebugMenu_C
--     /Game/GUI/HUD/UMG_DebugButton.UMG_DebugButton_C
--     /Game/GUI/GameMenus/UMG_DebugInput.UMG_DebugInput_C
-- Nothing in the shipped build appears to route input to them, so this constructs one and
-- adds it to the viewport directly.
--
-- Drop a widget name into stray-debug.cmd in the game dir (Binaries/Win64) — one line, e.g.
--     UMG_DebugMenu
--     UMG_HUD_Debug
--     close
-- Polls every 500 ms on the async thread; every engine call runs on the GAME thread through
-- ExecuteInGameThread and is wrapped in pcall (the StrayFur checkpoint-reload crash lesson:
-- touching UObjects off the game thread is what killed the game during teardown).

local UEHelpers = require("UEHelpers")
local CMD = "stray-debug.cmd"
local LOG = "stray-debug.txt"

local KNOWN = {
    UMG_DebugMenu  = "/Game/GUI/HUD/UMG_DebugMenu.UMG_DebugMenu_C",
    UMG_HUD_Debug  = "/Game/GUI/HUD/UMG_HUD_Debug.UMG_HUD_Debug_C",
    UMG_DebugInput = "/Game/GUI/GameMenus/UMG_DebugInput.UMG_DebugInput_C",
    UMG_DebugButton= "/Game/GUI/HUD/UMG_DebugButton.UMG_DebugButton_C",
}

local open = {}   -- name -> widget

local function note(line)
    print("[StrayDebugMenu] " .. line .. "\n")
    local f = io.open(LOG, "ab")
    if f then f:write(line .. "\n"); f:close() end
end

local function readCommand()
    local f = io.open(CMD, "rb")
    if not f then return nil end
    local text = f:read("*a"); f:close()
    local w = io.open(CMD, "wb"); if w then w:close() end
    if not text or text == "" then return nil end
    return (text:gsub("[\r\n]", ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function closeAll()
    for name, w in pairs(open) do
        pcall(function() if w:IsValid() then w:RemoveFromViewport() end end)
        note("closed " .. name)
    end
    open = {}
end

local function show(name)
    local path = KNOWN[name] or name
    local cls = StaticFindObject(path)
    if cls == nil or not cls:IsValid() then
        -- Not loaded yet: LoadAsset pulls it in without needing the class resident first.
        pcall(function() cls = StaticFindObject(path) end)
    end
    if cls == nil or not cls:IsValid() then
        note("NOT FOUND (not loaded?): " .. path)
        return
    end
    local world = UEHelpers.GetWorld()
    local pc = UEHelpers.GetPlayerController()
    local lib = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    if lib == nil or not lib:IsValid() then note("UMG.WidgetBlueprintLibrary unavailable"); return end
    local widget = lib:Create(world, cls, pc)
    if widget == nil or not widget:IsValid() then note("Create returned nothing for " .. path); return end
    widget:AddToViewport(1000)
    -- A debug widget built for a dev build commonly ships with its root Collapsed or Hidden,
    -- so AddToViewport succeeds and draws nothing. Force it visible and REPORT what we found,
    -- because "created but invisible" and "created and empty" need different fixes.
    local vis_before = "?"
    pcall(function() vis_before = tostring(widget.Visibility) end)
    pcall(function() widget:SetVisibility(0) end)   -- ESlateVisibility::Visible
    local inview, nchild = "?", "?"
    pcall(function() inview = tostring(widget:IsInViewport()) end)
    pcall(function()
        local root = widget.WidgetTree
        if root ~= nil and root:IsValid() then
            local rw = root.RootWidget
            nchild = (rw ~= nil and rw:IsValid()) and rw:GetFullName() or "no RootWidget"
        else nchild = "no WidgetTree" end
    end)
    open[name] = widget
    note(string.format("OPENED %s  visibilityWas=%s inViewport=%s root=%s",
        path, vis_before, inview, nchild))
end

-- KEYBINDS. UE4SS delivers these on its own thread, so each one only queues the work; the
-- engine calls still go through ExecuteInGameThread inside run().
--   F9            toggle UMG_DebugMenu
--   F10           toggle UMG_HUD_Debug
--   Ctrl+F9       UMG_DebugInput
--   Ctrl+F10      list what is loaded
--   Shift+F9      close everything
-- NOT F6/F7: the game binds those itself (DefaultInput.ini "DumpCat" / "LoadCat"), and the
-- game's own V is ToggleDebugMenu — press that first, it is the real menu.
-- The file channel (stray-debug.cmd) still works and takes any widget path, so a widget these
-- binds do not cover is one echo away.
local function bind(key, mods, cmd, label)
    local ok, err = pcall(function()
        if mods then RegisterKeyBind(key, mods, function() run(cmd) end)
        else RegisterKeyBind(key, function() run(cmd) end) end
    end)
    note(ok and ("keybind " .. label .. " -> " .. cmd)
            or ("keybind " .. label .. " FAILED: " .. tostring(err)))
end

bind(Key.F9,  nil,                   "UMG_DebugMenu",  "F9")
bind(Key.F10, nil,                   "UMG_HUD_Debug",  "F10")
bind(Key.F9,  {ModifierKey.CONTROL}, "UMG_DebugInput", "Ctrl+F9")
bind(Key.F10, {ModifierKey.CONTROL}, "list",           "Ctrl+F10")
bind(Key.F9,  {ModifierKey.SHIFT},   "close",          "Shift+F9")

-- One entry point for both channels. Everything engine-facing happens on the game thread.
function run(cmd)
    ExecuteInGameThread(function()
        local ok, err = pcall(function()
            if cmd == "close" then closeAll()
            elseif cmd == "dump" then
                -- The widget opens, is visible and has a root CanvasPanel, yet draws nothing —
                -- so the GAME fills it. Dump BP_HKHUD_C completely (every function, every
                -- property) rather than filtering on "debug", because the mechanism may not
                -- carry that word. UE4SS's ForEachProperty is deprecated at this SHA; use
                -- TFieldRange the way mods/StrayDualSense had to.
                local objs = FindAllOf("BP_HKHUD_C") or {}
                for _, o in ipairs(objs) do
                    note("== " .. o:GetFullName())
                    local cls = o:GetClass()
                    pcall(function()
                        cls:ForEachFunction(function(fn) note("  fn   " .. fn:GetName()) end)
                    end)
                    pcall(function()
                        cls:ForEachProperty(function(pr)
                            note(string.format("  prop %s = %s", pr:GetName(),
                                tostring(pcall(function() return o[pr:GetName()] end))))
                        end)
                    end)
                    break
                end
            elseif cmd == "hud" then
                -- DefaultGame.ini's [/Script/Hk_project.HKGameSettings] names
                -- HUDDebugWidgetClass, so some HUD/controller owns these widgets and probably
                -- has its own way to raise them. List what the live objects actually offer
                -- instead of inventing a route.
                for _, cls in ipairs({ "HUD", "PlayerController", "GameInstance", "HKHUD" }) do
                    local objs = FindAllOf(cls)
                    if objs ~= nil then
                        for _, o in ipairs(objs) do
                            local full = o:GetFullName()
                            if full:find("Hk") or full:find("HUD") then
                                note("OBJECT " .. full)
                                pcall(function()
                                    local c = o:GetClass()
                                    for _, f in ipairs(c:GetAllFunctions() or {}) do
                                        local n = f:GetName()
                                        if n:lower():find("debug") then note("   fn  " .. n) end
                                    end
                                end)
                                pcall(function()
                                    o:ForEachProperty(function(prop)
                                        local n = prop:GetName()
                                        if n:lower():find("debug") then note("   prop " .. n) end
                                    end)
                                end)
                            end
                        end
                    end
                end
            elseif cmd == "list" then
                for k, v in pairs(KNOWN) do
                    local o = StaticFindObject(v)
                    note(string.format("  %-16s %s  loaded=%s", k, v,
                        (o ~= nil and o:IsValid()) and "yes" or "no"))
                end
            else
                -- A second press closes it rather than stacking another copy on the viewport.
                local w = open[cmd]
                if w ~= nil then
                    pcall(function() if w:IsValid() then w:RemoveFromViewport() end end)
                    open[cmd] = nil
                    note("closed " .. cmd)
                else
                    show(cmd)
                end
            end
        end)
        if not ok then note("FAILED on '" .. tostring(cmd) .. "': " .. tostring(err)) end
    end)
end

LoopAsync(500, function()
    local cmd = readCommand()
    if cmd == nil or cmd == "" then return false end
    run(cmd)
    return false
end)

note("loaded. Write a widget name into " .. CMD .. " (UMG_DebugMenu | UMG_HUD_Debug | "
     .. "UMG_DebugInput | list | close)")
