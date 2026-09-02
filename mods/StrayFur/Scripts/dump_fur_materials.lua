-- StrayFur diagnostic: dump every scalar, vector and texture parameter of the two fur
-- material instances the game ships (M_Fur_2sidedshading_backpackON / backpackOff) and of
-- the live dynamic instance on the cat, so the backpack fur mask can be read out of the
-- running game rather than out of the Oodle-compressed pak (2026-09-02: pakextract.py
-- cannot decode method 2, and no ooz build is on the box).
--
-- Loaded by main.lua once the pawn is found; writes stray-fur-materials.txt in the game
-- dir (Binaries/Win64). Read-only: it changes nothing on the materials. Every engine call
-- runs on the game thread (the checkpoint-reload crash lesson) and is wrapped in pcall.

local M = {}

local function objectName(obj)
    local ok, full = pcall(function() return obj:GetFullName() end)
    if not ok then return "?" end
    return full
end

-- Walk a TArray of FScalar/Vector/TextureParameterValue structs via UE4SS's ForEach.
local function dumpParams(out, mat, arrayName)
    local ok, arr = pcall(function() return mat[arrayName] end)
    if not ok or arr == nil then out[#out + 1] = "  " .. arrayName .. ": <unreadable>"; return end
    local n = 0
    pcall(function()
        arr:ForEach(function(_, elem)
            local v = elem:get()
            local name, value = "?", "?"
            -- An FName reaches Lua as userdata; tostring() prints its address (measured: the
            -- first dump listed "FNameUserdata: 0000..."). ToString() gives the text.
            pcall(function()
                local info = v.ParameterInfo
                local fn = info and info.Name or v.ParameterName
                if fn ~= nil and type(fn) == "userdata" and fn.ToString then name = fn:ToString() else name = tostring(fn) end
            end)
            pcall(function()
                if arrayName == "ScalarParameterValues" then
                    value = tostring(v.ParameterValue)
                elseif arrayName == "VectorParameterValues" then
                    local c = v.ParameterValue
                    value = string.format("(%.4f, %.4f, %.4f, %.4f)", c.R, c.G, c.B, c.A)
                else
                    local t = v.ParameterValue
                    value = (t ~= nil and t:IsValid()) and objectName(t) or "None"
                end
            end)
            out[#out + 1] = string.format("  %-40s = %s", name, value)
            n = n + 1
        end)
    end)
    out[#out + 1] = string.format("  (%d %s)", n, arrayName)
end

local function dumpMaterial(out, mat, label)
    out[#out + 1] = "=== " .. label .. ": " .. objectName(mat)
    pcall(function()
        local parent = mat.Parent
        if parent ~= nil and parent:IsValid() then out[#out + 1] = "  Parent = " .. objectName(parent) end
    end)
    dumpParams(out, mat, "ScalarParameterValues")
    dumpParams(out, mat, "VectorParameterValues")
    dumpParams(out, mat, "TextureParameterValues")
end

function M.run(gfur)
    ExecuteInGameThread(function()
        local out = {}
        -- The two shipped instances, by path.
        for _, path in ipairs({
            "/Game/Character/Cat/Fur/M_Fur_2sidedshading_backpackON.M_Fur_2sidedshading_backpackON",
            "/Game/Character/Cat/Fur/M_Fur_2sidedshading_backpackOff.M_Fur_2sidedshading_backpackOff",
            "/Game/Character/Cat/Fur/M_Fur_2sidedshading_backpackON_HDScreenshots.M_Fur_2sidedshading_backpackON_HDScreenshots",
        }) do
            local ok, mat = pcall(function() return StaticFindObject(path) end)
            if ok and mat ~= nil and mat:IsValid() then dumpMaterial(out, mat, "shipped") else out[#out + 1] = "=== not loaded: " .. path end
        end
        -- What the cat is actually wearing right now (after main.lua's edits).
        pcall(function()
            local mats = gfur.FurMaterials
            mats:ForEach(function(i, elem)
                local m = elem:get()
                if m ~= nil and m:IsValid() then dumpMaterial(out, m, "live slot " .. tostring(i)) end
            end)
        end)
        -- The GFur component's geometry knobs, for the record.
        pcall(function()
            out[#out + 1] = string.format("=== GFur: LayerCount=%s FurLength=%s ShellBias=%s", tostring(gfur.LayerCount), tostring(gfur.FurLength), tostring(gfur.ShellBias))
        end)
        local f = io.open("stray-fur-materials.txt", "wb")
        if f then f:write(table.concat(out, "\n") .. "\n"); f:close() end
        print("[StrayFur] material dump written: stray-fur-materials.txt (" .. #out .. " lines)\n")
    end)
end

return M
