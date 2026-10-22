(name: Spindle warm-up)
(info: spindle at the warm-up speed for the warm-up time, then off - Settings > Macros)
M3 S{macro.warm_rpm}
G4 P{macro.warm_s}
M5
M30
