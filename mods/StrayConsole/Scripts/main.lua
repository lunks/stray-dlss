-- StrayConsole: run console commands in the live game from a file, so a cvar can be tested
-- without touching Engine.ini (which the game rewrites on exit) and without a relaunch.
--
-- Drop lines into stray-console.cmd in the game dir (Binaries/Win64); the mod executes each
-- line once on the game thread through UKismetSystemLibrary::ExecuteConsoleCommand, echoes
-- it to UE4SS.log, then truncates the file. One line per command, e.g.
--     r.SSR.Temporal 1
--     r.Reflections.Denoiser.TemporalAccumulation 0
-- Polls every 500 ms from the async thread; only the execute runs on the game thread, and
-- it is wrapped in pcall. Nothing here writes anything but the echo and the truncate.

local UEHelpers = require("UEHelpers")
local CMD = "stray-console.cmd"

local function readCommands()
    local f = io.open(CMD, "rb")
    if not f then return nil end
    local text = f:read("*a"); f:close()
    if text == nil or text == "" then return nil end
    local lines = {}
    for raw in text:gmatch("[^\r\n]+") do
        local line = raw:gsub("^%s+", ""):gsub("%s+$", "")
        if line ~= "" and line:sub(1, 1) ~= "#" then lines[#lines + 1] = line end
    end
    -- Truncate so the same commands are not replayed next tick.
    local w = io.open(CMD, "wb"); if w then w:close() end
    return lines
end

local function execute(lines)
    ExecuteInGameThread(function()
        local ok, err = pcall(function()
            local ks = UEHelpers.GetKismetSystemLibrary()
            local world = UEHelpers.GetWorld()
            for _, line in ipairs(lines) do
                ks:ExecuteConsoleCommand(world, line, nil)
                print("[StrayConsole] ran: " .. line .. "\n")
            end
        end)
        if not ok then print("[StrayConsole] FAILED: " .. tostring(err) .. "\n") end
    end)
end

LoopAsync(500, function()
    local lines = readCommands()
    if lines and #lines > 0 then pcall(execute, lines) end
    return false
end)

print("[StrayConsole] loaded; append console commands to " .. CMD .. "\n")
