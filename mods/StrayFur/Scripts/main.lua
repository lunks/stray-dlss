-- StrayFur: better cat fur, at runtime, without touching the pak.
--
-- Everything that matters lives on the live GFurComponent (the GiM GFur shell-fur plugin,
-- compiled into the exe) and on its fur material. BP_CatPawn is a NATIVIZED Blueprint
-- (a DynamicClass, no cooked asset), so its DEFAULTS cannot be edited - but the spawned
-- INSTANCE can be, which is what this does. Measured facts behind every number are in
-- docs/STRAY-DUALSENSE.md's sibling investigation of the cat assets (2026-09-01).
--
-- Two independent tiers, each switchable below:
--
--  TIER 1 - the fur the developers already made better. The pak ships
--    M_Fur_2sidedshading_backpackON_HDScreenshots, a shading variant of the normal fur
--    material. It differs in exactly nine scalars (read from the cooked assets), all
--    cheap: finer tips, less uniform height, slightly glossier, stronger AO. Nothing in it
--    changes geometry, so it costs nothing to run. We apply those nine values to the live
--    material. Zero unknowns.
--
--  TIER 2 - more and longer fur. Density is on the COMPONENT, not the material:
--    LayerCount (shells; the companion cats run 8, the player's value is read here and
--    logged), FurLength, ShellBias. The per-LOD LODs[i].LayerCount and MinScreenSize must
--    be raised too or distance LODs silently undo it. Cost scales linearly with shells.
--    UNCONFIRMED: whether GFur rebuilds its shell geometry when LayerCount changes on a
--    live component. In-editor it does on property change; at runtime it may need the
--    render state recreated, which the visibility toggle below attempts. The log shows
--    before/after values so one launch settles it.
--
-- Nothing here is deployed automatically; see mods/StrayFur/README.md.

------------------------------------------------------------------ configuration
local TIER1 = true          -- apply the HD-screenshot material scalars
local TIER2 = true          -- raise shell count / length
local LAYERS      = 16      -- shells (companion cats ship 8; player value is logged)
local LAYERS_LOD  = 16      -- per-LOD shells; keep >= LAYERS or LODs undo it
local FUR_LENGTH  = nil     -- nil = leave the game's value; e.g. 1.25 = +25%
local SHELL_BIAS  = nil     -- nil = leave; 0..1, higher packs shells toward the root
local MIN_SCREEN  = 0.0     -- never drop the fur for distance

-- The nine scalars where the HD material differs from the shipped one (cooked values).
local HD_SCALARS = {
    ["Fur Tip Thickness"]                 = 0.0,    -- was 0.10
    ["Thickness Root to Tip Distribution"] = 0.75,  -- was 1.00
    ["Fur Pattern Tiling"]                = 6.0,    -- was 7
    ["Fur Pattern Tiling 2"]              = 6.0,    -- was 7
    ["Height Variation"]                  = 0.1,    -- was 0.0
    ["Roughness"]                         = 0.8,    -- was 1.0
    ["AO Power"]                          = 0.2,    -- was 0.1
    ["Spec AO mul"]                       = 0.75,   -- was 1.5
    ["Spec AO power"]                     = 2.0,    -- was 1.0
}

------------------------------------------------------------------ plumbing
local function log(s) print("[StrayFur] " .. tostring(s) .. "\n") end

-- Read a reflected property defensively; nil on any failure rather than a thrown error.
local function get(obj, name)
    local ok, v = pcall(function() return obj[name] end)
    if ok then return v end
    return nil
end

local function num(v)
    if type(v) == "number" then return v end
    local n; pcall(function() n = tonumber(tostring(v)) end)
    return n
end

-- Once per pawn instance: the pawn can be recreated on a level load, so key on the
-- object's full name, not a global flag.
local done = {}

local function applyMaterial(gfur)
    -- GFurComponent keeps its materials in FurMaterials (an array); UMeshComponent's
    -- CreateDynamicMaterialInstance is BlueprintCallable and gives us a writable instance.
    local mats = get(gfur, "FurMaterials")
    if not mats then log("  no FurMaterials on the component"); return end
    local count = 0
    pcall(function() count = #mats end)
    if count == 0 then pcall(function() mats:ForEach(function() count = count + 1 end) end) end
    if count == 0 then log("  FurMaterials is empty"); return end
    for i = 0, count - 1 do
        local ok, mid = pcall(function() return gfur:CreateDynamicMaterialInstance(i, nil) end)
        if not ok or not mid or not mid:IsValid() then
            log(string.format("  material slot %d: could not create a dynamic instance (%s)", i, tostring(mid)))
        else
            local applied = 0
            for pname, pval in pairs(HD_SCALARS) do
                local ok2 = pcall(function() mid:SetScalarParameterValue(FName(pname), pval) end)
                if ok2 then applied = applied + 1 end
            end
            log(string.format("  material slot %d: applied %d/9 HD scalars", i, applied))
        end
    end
end

local function applyDensity(gfur)
    local before = {
        LayerCount    = num(get(gfur, "LayerCount")),
        FurLength     = num(get(gfur, "FurLength")),
        ShellBias     = num(get(gfur, "ShellBias")),
        MinScreenSize = num(get(gfur, "MinScreenSize")),
    }
    -- This is the number nobody could read from the pak: the player cat's shipped value.
    log(string.format("  SHIPPED: LayerCount=%s FurLength=%s ShellBias=%s MinScreenSize=%s",
        tostring(before.LayerCount), tostring(before.FurLength),
        tostring(before.ShellBias), tostring(before.MinScreenSize)))

    pcall(function() gfur.LayerCount = LAYERS end)
    pcall(function() gfur.MinScreenSize = MIN_SCREEN end)
    if FUR_LENGTH then pcall(function() gfur.FurLength = FUR_LENGTH end) end
    if SHELL_BIAS then pcall(function() gfur.ShellBias = SHELL_BIAS end) end

    -- Per-LOD shell counts, or the distance LODs quietly put the old count back.
    local lods = get(gfur, "LODs")
    local nlod = 0
    if lods then
        pcall(function()
            lods:ForEach(function(_, elem)
                local lod = elem:get()
                local was = num(get(lod, "LayerCount"))
                pcall(function() lod.LayerCount = LAYERS_LOD end)
                pcall(function() lod.ScreenSize = 0.0 end)
                nlod = nlod + 1
                log(string.format("  LOD %d: LayerCount %s -> %d", nlod - 1, tostring(was), LAYERS_LOD))
            end)
        end)
    end

    -- UNCONFIRMED: does GFur rebuild its shells for a live component? Toggling visibility
    -- is BlueprintCallable and normally recreates the render state; if the log shows the
    -- new LayerCount but the fur looks unchanged, this is the line that did not work.
    pcall(function() gfur:SetVisibility(false, false) end)
    pcall(function() gfur:SetVisibility(true, false) end)

    log(string.format("  AFTER:   LayerCount=%s FurLength=%s ShellBias=%s MinScreenSize=%s (%d LODs)",
        tostring(num(get(gfur, "LayerCount"))), tostring(num(get(gfur, "FurLength"))),
        tostring(num(get(gfur, "ShellBias"))), tostring(num(get(gfur, "MinScreenSize"))), nlod))
end

local function applyTo(pawn)
    if not pawn or not pawn:IsValid() then return end
    local key = pawn:GetFullName()
    if done[key] then return end
    -- Only the player cat. The dummies and the cinematic cat have their own components.
    if not key:find("BP_CatPawn_C") then return end
    local gfur = get(pawn, "GFur")
    if not gfur or not gfur:IsValid() then
        log("pawn found but its GFur component is not valid yet: " .. key)
        return
    end
    done[key] = true
    log("applying to " .. key)
    if TIER1 then applyMaterial(gfur) end
    if TIER2 then applyDensity(gfur) end
end

------------------------------------------------------------------ triggers
-- Primary: be told when a cat pawn is constructed.
-- UNCONFIRMED: whether NotifyOnNewObject resolves a DynamicClass (nativized BP) by this
-- path. The poll below is the fallback and is what makes this robust either way.
pcall(function()
    NotifyOnNewObject("/Game/Character/Cat/BP_CatPawn.BP_CatPawn_C", function(obj)
        -- Components may not be registered inside the constructor; defer one tick.
        ExecuteWithDelay(500, function() applyTo(obj) end)
    end)
    log("registered NotifyOnNewObject for BP_CatPawn_C")
end)

-- Fallback: poll for a live pawn. Cheap (one FindFirstOf every 2 s), idempotent via `done`.
LoopAsync(2000, function()
    local ok, pawn = pcall(function() return FindFirstOf("BP_CatPawn_C") end)
    if ok and pawn and pawn:IsValid() then applyTo(pawn) end
    return false   -- keep looping; level loads recreate the pawn
end)

log(string.format("loaded: tier1=%s tier2=%s layers=%d", tostring(TIER1), tostring(TIER2), LAYERS))
