-- Local override archive for the macOS / OpenGL 4.1 port.
--
-- Depends on the stock BAR archive and shadows individual files in it. Widgets
-- here load as ARCHIVE widgets, so they are not subject to the user-widget
-- sandbox that blocks a write-dir copy from registering custom callins - which
-- is the whole reason this archive has to exist rather than dropping files in
-- <write-dir>/LuaUI/Widgets.
--
-- Keep the override list in devtools/macos-port/bar-overrides/ in the engine
-- repo; this directory is the deployment, not the source of truth.
return {
	name = "BAR macOS overrides",
	shortname = "BARMACOS",
	game = "Beyond All Reason",
	shortGame = "BAR",
	mutator = "official",
	version = "1",
	description = "BAR with the GL 4.1 fixes the macOS port needs",
	modtype = 1,
	depend = {
		"Beyond All Reason test-31765-420384b",
	},
}
