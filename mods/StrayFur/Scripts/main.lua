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
    -- MEASURED 2026-09-02 (stray-fur-materials.txt): the fur's live dynamic instance sits in
    -- FurMaterials SLOT 1 (slot 0 is the base material), and the MID our old code made with
    -- CreateDynamicMaterialInstance(0, ...) was a fresh instance the shells never sampled.
    -- Every "applied N/N" line before this was a pcall succeeding on a setter that reached
    -- nothing: no HD, plush, distance or backpack scalar ever touched the fur, which is why
    -- only LayerCount/FurLength (component properties) ever changed the look. So: walk
    -- FurMaterials, and for each slot write onto the MaterialInstanceDynamic the component
    -- already holds; only if a slot is not dynamic yet, create one for THAT slot.
    local mats = get(gfur, "FurMaterials")
    if not mats then log("  no FurMaterials on the component"); return end
    local slots = {}
    pcall(function() mats:ForEach(function(i, elem) slots[#slots + 1] = { index = i, mat = elem:get() } end) end)
    if #slots == 0 then log("  FurMaterials is empty"); return end
    for _, s in ipairs(slots) do
        local mid = s.mat
        local isDyn = false
        pcall(function() isDyn = mid ~= nil and mid:IsValid() and mid:GetFullName():find("MaterialInstanceDynamic", 1, true) ~= nil end)
        if not isDyn then
            local ok, made = pcall(function() return gfur:CreateDynamicMaterialInstance(s.index, nil, FName("None")) end)
            if ok and made and made:IsValid() then mid = made else mid = nil end
        end
        if mid == nil then
            log(string.format("  material slot %d: no dynamic instance to write to", s.index))
        else
            -- Read back one value after writing, so "applied" means the instance really took it.
            local function applyTable(tbl, label, want)
                local applied, verified = 0, 0
                for pname, pval in pairs(tbl) do
                    local ok2 = pcall(function() mid:SetScalarParameterValue(FName(pname), pval) end)
                    if ok2 then applied = applied + 1 end
                    pcall(function()
                        local got = mid:K2_GetScalarParameterValue(FName(pname))
                        if math.abs(tonumber(got) - pval) < 1e-3 then verified = verified + 1 end
                    end)
                end
                log(string.format("  material slot %d: applied %d/%d %s scalars, read back %d", s.index, applied, want, label, verified))
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

-- `route` names the trigger that got here ("notify" or "poll"), so one session's log settles
-- whether NotifyOnNewObject fires for this nativized class and the poll can be retired
-- (staged 2026-09-05; the poll is the only timer in the enabled mods that does game-thread
-- work on a schedule).
local function applyTo(pawn, route)
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
    log("applying to " .. key .. " (route: " .. tostring(route) .. ")")
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
local function applyOnGameThread(pawn, route)
    pcall(function() ExecuteInGameThread(function() pcall(applyTo, pawn, route) end) end)
end

pcall(function()
    NotifyOnNewObject("/Game/Character/Cat/BP_CatPawn.BP_CatPawn_C", function(obj)
        -- This line alone answers the UNCONFIRMED above: the notification resolved the class.
        log("notify fired for a new BP_CatPawn_C")
        -- Components may not be registered inside the constructor; defer, then hop to
        -- the game thread.
        ExecuteWithDelay(500, function() applyOnGameThread(obj, "notify") end)
    end)
    log("registered NotifyOnNewObject for BP_CatPawn_C")
end)

-- Fallback: poll for a live pawn. One FindFirstOf every POLL_INTERVAL_MS ON THE GAME THREAD,
-- idempotent via `done`.
--
-- POLL_ENABLED is a DIAGNOSTIC knob, not a safe default to ship off: NotifyOnNewObject alone
-- may not catch every spawn (see the UNCONFIRMED note above), so turning this off relies on a
-- mechanism we have not proven. It exists so a periodic engine query can be ruled in or out
-- when chasing a hitch, without disabling the fur itself.
local POLL_ENABLED     = true
local POLL_INTERVAL_MS = 2000

if POLL_ENABLED then
    LoopAsync(POLL_INTERVAL_MS, function()
        pcall(function()
            ExecuteInGameThread(function()
                local ok, pawn = pcall(function() return FindFirstOf("BP_CatPawn_C") end)
                if ok and pawn and pawn:IsValid() then pcall(applyTo, pawn, "poll") end
            end)
        end)
        return false   -- keep looping; level loads recreate the pawn
    end)
else
    log("POLL_ENABLED=false: relying on NotifyOnNewObject only")
end

log(string.format("loaded: tier1=%s tier2=%s layers=%d", tostring(TIER1), tostring(TIER2), LAYERS))
