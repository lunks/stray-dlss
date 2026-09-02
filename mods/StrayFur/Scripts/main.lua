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
-- MEASURED on the first launch (2026-09-02): the PLAYER cat ships LayerCount=16,
-- FurLength=1.15, ShellBias=1.0, MinScreenSize=0 - twice the companion cats' 8. So 16 was
-- a no-op; anything that should be felt as "more fur" has to go above it.
local LAYERS      = 48      -- shells (player ships 16, companions 8); 48 = smoother volume
local LAYERS_LOD  = 48      -- per-LOD shells; keep >= LAYERS or LODs undo it
local FUR_LENGTH  = 2.0     -- shipped 1.15; the single most VISIBLE knob. nil = leave alone
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

-- User-requested on top of the HD tier (2026-09-02). These are OUR values, not the game's;
-- the shipped numbers are noted so they can be dialled back.
--   Plush: thicker strands and a scruffier coat, length untouched.
--   Distance: the shipped fade thins the shells out well inside third-person gameplay
--   range, which is why the change was obvious up close and hard to see from behind.
--   The fade scalars' units are UNCONFIRMED (SOFT: they read as metres-ish); pushed ~2.5x.
local PLUSH_SCALARS = {
    ["Fur Root Thickness"]  = 1.5,    -- shipped 1.0
    ["Height Variation"]    = 0.25,   -- HD 0.1, shipped 0.0
}
local DISTANCE_SCALARS = {
    ["Fade min"]              = 90.0,   -- shipped 35
    ["Fade max"]              = 110.0,  -- shipped 40
    ["Camera Distance Blend"] = 150.0,  -- shipped 75
}

-- THE BACKPACK CLIP (user screenshot 2026-09-02: shells poking through the harness at the
-- shoulders/neck). How the shipped fur stays short there: the material samples a painted
-- per-vertex length map, Cat_furmesh_FurGrowth (read out of the decoded pak assets; both
-- backpackON and backpackOff share it), and the component's FurLength multiplies the WHOLE
-- map. Doubling FurLength therefore doubles the short harness fur too, and it grows through
-- the mesh. The material also ships "Fur Length Power", a power curve on that map: raising
-- it pushes the map's low (short) values toward zero while values near 1 (the long body
-- fur) barely move. So a higher power keeps the body length and shortens the harness
-- region, with no new textures. 1.0 is neutral; the shipped value is read by the material
-- dump (stray-fur-materials.txt) on the next launch. UNCONFIRMED until seen on screen:
-- start at 2.0 and dial by eye; "Avoid Short - Offset/Power" are the other two shipped
-- knobs that act on short fur only, if the power alone is not enough.
-- MEASURED 2026-09-02 (stray-fur-materials.txt): the SHIPPED "Fur Length Power" is 4.0, not
-- 1.0, so the first try at 2.0 flattened the curve and made the harness fur LONGER; the user
-- saw no change. Above 4 shortens the short regions harder. "Avoid Short" ships neutral
-- (Offset 0, Power 1); a small positive offset subtracts a constant from the map, which
-- kills the very short harness fur outright while long fur loses almost nothing.
local BACKPACK_SCALARS = {
    ["Fur Length Power"]   = 7.0,   -- shipped 4.0
    ["Avoid Short - Offset"] = 0.05, -- shipped 0.0
}
local DUMP_MATERIALS = true     -- write stray-fur-materials.txt once per pawn (read-only)

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
        -- MEASURED: the BlueprintCallable takes THREE params (ElementIndex, SourceMaterial,
        -- OptionalName); two threw "UFunction expected 3 parameters, received 2".
        local ok, mid = pcall(function() return gfur:CreateDynamicMaterialInstance(i, nil, FName("None")) end)
        if not ok or not mid or not mid:IsValid() then
            log(string.format("  material slot %d: could not create a dynamic instance (%s)", i, tostring(mid)))
        else
            -- Three tables, three counts, so a name that stops resolving is visible per group.
            local function applyTable(tbl, label, want)
                local applied = 0
                for pname, pval in pairs(tbl) do
                    local ok2 = pcall(function() mid:SetScalarParameterValue(FName(pname), pval) end)
                    if ok2 then applied = applied + 1 end
                end
                log(string.format("  material slot %d: applied %d/%d %s scalars", i, applied, want, label))
            end
            applyTable(HD_SCALARS,       "HD",       9)
            applyTable(PLUSH_SCALARS,    "plush",    2)
            applyTable(DISTANCE_SCALARS, "distance", 3)
            applyTable(BACKPACK_SCALARS, "backpack", 2)
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

    -- MEASURED (second launch): setting LayerCount 16->32 plus a visibility toggle produced
    -- no visible change - the property changed, the geometry did not. GFur exposes the
    -- real trigger as a BlueprintCallable: GFurComponent:RegenerateFur() (no params),
    -- found in the object dump. Call it after every geometry-affecting write.
    local ok, err = pcall(function() gfur:RegenerateFur() end)
    log("  RegenerateFur() -> " .. (ok and "called" or ("FAILED: " .. tostring(err))))

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
    if DUMP_MATERIALS then
        local ok, err = pcall(function() require("dump_fur_materials").run(gfur) end)
        if not ok then log("material dump failed: " .. tostring(err)) end
    end
end

------------------------------------------------------------------ triggers
-- Primary: be told when a cat pawn is constructed.
-- UNCONFIRMED: whether NotifyOnNewObject resolves a DynamicClass (nativized BP) by this
-- path. The poll below is the fallback and is what makes this robust either way.
-- MEASURED 2026-09-02, and it was THIS mod: "reload last checkpoint" crashed the game every
-- time (EXCEPTION_ACCESS_VIOLATION reading 0x270, through UE4SS's ProcessEvent hook) with
-- StrayFur enabled and never with it disabled, DLSS and ReShade and the DualSense mod
-- being present or absent making no difference (tools/stray-bench.sh bisection, 2 cycles
-- per arm). The mechanism: ExecuteWithDelay and LoopAsync callbacks run on a UE4SS
-- thread, so FindFirstOf / applyTo touched UObjects while the game thread was tearing
-- the checkpoint state down. Every engine call now goes through ExecuteInGameThread,
-- which is what let mods/StrayProbe survive the same reloads.
local function applyOnGameThread(pawn)
    pcall(function() ExecuteInGameThread(function() pcall(applyTo, pawn) end) end)
end

pcall(function()
    NotifyOnNewObject("/Game/Character/Cat/BP_CatPawn.BP_CatPawn_C", function(obj)
        -- Components may not be registered inside the constructor; defer, then hop to
        -- the game thread.
        ExecuteWithDelay(500, function() applyOnGameThread(obj) end)
    end)
    log("registered NotifyOnNewObject for BP_CatPawn_C")
end)

-- Fallback: poll for a live pawn. One FindFirstOf every 2 s ON THE GAME THREAD, idempotent
-- via `done`.
LoopAsync(2000, function()
    pcall(function()
        ExecuteInGameThread(function()
            local ok, pawn = pcall(function() return FindFirstOf("BP_CatPawn_C") end)
            if ok and pawn and pawn:IsValid() then pcall(applyTo, pawn) end
        end)
    end)
    return false   -- keep looping; level loads recreate the pawn
end)

log(string.format("loaded: tier1=%s tier2=%s layers=%d", tostring(TIER1), tostring(TIER2), LAYERS))
