(name: Park)
(info: Z up, X Y to the park position, Z to its height - Settings > Macros, machine coordinates)
(needs: homed)
G90
G53 G0 Z{max:machine.z,macro.park_z}
G53 G0 X{macro.park_x} Y{macro.park_y}
G53 G0 Z{macro.park_z}
