-- shimmy default Hyprland config (Lua, Hyprland 0.57)
xwayland.enabled = false
hl.on("hyprland.start", function()
  hl.exec_cmd("foot sh -c 'while true; do date +%T; sleep 0.4; done'")
end)
