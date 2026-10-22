(name: Tool change position)
(info: Z up, X Y to the tool-change position, Z to its height - Settings > Macros)
(needs: homed)
G90
G53 G0 Z{max:machine.z,macro.tc_z}
G53 G0 X{macro.tc_x} Y{macro.tc_y}
G53 G0 Z{macro.tc_z}
