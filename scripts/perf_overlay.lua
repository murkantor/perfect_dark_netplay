-- Render-time + memory on-screen overlays. Two independent toggles:
--   /fps   render rate + CPU%/GPU% load  (vs the 60Hz budget)
--   /mem   physical memory used/total (the 64MB OG-Xbox watchdog) + vtx scratch pool
--
-- Drawn on the RIGHT, stacked just below the octree overlay (octree_overlay.lua
-- is x=230 y=28-70), so the two read as one group and we stay clear of the AI
-- X-ray (showcase.lua, left column). Kept near the top because the lo-res screen
-- is only ~220 tall -- the bottom few rows are off-screen. Move X/Y_* to taste.
--
-- Reads pd.perf(); show_fps/show_mem come from the /fps and /mem console toggles
-- (g_LuaShowFps / g_LuaShowMem). Loaded by scripts/init.lua. Colours 0xRRGGBBAA.

local C_WHITE = 0xffffffff
local C_GREEN = 0x40ff40ff
local C_YELL  = 0xffe040ff
local C_RED   = 0xff5050ff

local X = 230  -- right column, under the octree overlay
local Y = 80
local LH = 8   -- line height

pd.log("perf_overlay.lua loaded (toggle with /fps and /mem)")

local function mb(bytes) return bytes / 1048576 end
local function kb(bytes) return bytes / 1024 end

-- green < lo, yellow < hi, red otherwise
local function loadColour(pct, lo, hi)
  return (pct < lo) and C_GREEN or (pct < hi) and C_YELL or C_RED
end

pd.on("draw", function()
  local p = pd.perf()
  if not p then return end
  local yy = Y

  if p.show_fps then
    -- frame time: green < 17ms (60fps), yellow < 33ms, red otherwise
    local c = (p.frame_ms < 17) and C_GREEN or (p.frame_ms < 33) and C_YELL or C_RED
    pd.draw_text(X, yy, string.format("%.0ffps %.1fms", p.fps, p.frame_ms), c)
    yy = yy + LH

    -- CPU% / GPU% of the 60Hz budget (>100% = can't hold 60). GPU < 0 = n/a.
    local cpu = p.cpu_pct or 0
    local gpustr = (p.gpu_pct and p.gpu_pct >= 0) and string.format("%.0f%%", p.gpu_pct) or "n/a"
    pd.draw_text(X, yy, string.format("cpu %.0f%% gpu %s", cpu, gpustr), loadColour(cpu, 75, 100))
    yy = yy + LH
  end

  if p.show_mem then
    -- physical memory used/total (the 64MB budget). Hidden when unknown (0).
    if p.mem_total and p.mem_total > 0 then
      local pct = 100 * (p.mem_used or 0) / p.mem_total
      pd.draw_text(X, yy, string.format("mem %.0f/%.0fM", mb(p.mem_used or 0), mb(p.mem_total)),
          loadColour(pct, 75, 90))
    else
      pd.draw_text(X, yy, "mem n/a", C_WHITE)
    end
    yy = yy + LH

    -- per-frame vtx scratch pool (stressed by No-Cull / /octree bigroom)
    local pct = (p.vtx_total > 0) and (100 * p.vtx_used / p.vtx_total) or 0
    pd.draw_text(X, yy, string.format("vtx %.0f/%.0fK", kb(p.vtx_used), kb(p.vtx_total)),
        loadColour(pct, 75, 90))
    yy = yy + LH
  end
end)
