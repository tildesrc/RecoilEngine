-- Regression baseline scenario for the macOS core-profile port.
--
-- Stages a fixed, deliberately STATIC scene: a fixed camera over a fixed set of
-- units, with no combat. Combat is avoided on purpose - weapon spread and
-- particle effects draw on the unsynced RNG, so a battle does not reproduce
-- frame-for-frame and cannot be diffed.
--
-- What this still covers: terrain, unit models and team colour, features and
-- trees, shadows, fog of war, the build menu with its icons, and UI text.

function widget:GetInfo()
  return { name = "Port Baseline Scenario", desc = "static scene for regression capture",
           layer = 10000, enabled = true }
end

local staged = false

function widget:Update()
  if staged then return end
  if Spring.GetGameFrame() < 20 then return end

  Spring.SendCommands("cheat")
  -- one of each kind we care about rendering, all on our own team so nothing fights
  Spring.SendCommands("give 3 armpw @760,100,560")
  Spring.SendCommands("give 2 armllt @700,100,640")
  Spring.SendCommands("give 1 armsolar @860,100,640")
  Spring.SendCommands("give 1 armmex @860,100,540")
  Spring.SendCommands("nocheat")

  Spring.SetCameraState({ name = "ta", px = 790, py = 100, pz = 600, height = 330, mode = 1,
                          dx = 0, dy = -0.82, dz = 0.57, flipped = -1,
                          zscale = 0.5, oldHeight = 330 }, 0)

  staged = true
  Spring.Echo("BASELINE: staged")
end
