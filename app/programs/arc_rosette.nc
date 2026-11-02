(arc_rosette - G2/G3 test: 6 petals of two arcs each, a circle round them, a spiral in the middle)
(absolute, work coordinates of the active WCS, fits in 100 x 100 mm, Z -0.5 to 5)
G21 G90 G17
M3 S12000
G0 Z5
(petal 1, 0 deg)
G0 X0 Y0
G1 Z-0.5 F300
G3 X40 Y0 I20 J-20 F1200
G3 X0 Y0 I-20 J20
G0 Z5
(petal 2, 60 deg)
G0 X0 Y0
G1 Z-0.5 F300
G3 X20 Y34.641 I27.321 J7.321 F1200
G3 X0 Y0 I-27.321 J-7.321
G0 Z5
(petal 3, 120 deg)
G0 X0 Y0
G1 Z-0.5 F300
G3 X-20 Y34.641 I7.321 J27.321 F1200
G3 X0 Y0 I-7.321 J-27.321
G0 Z5
(petal 4, 180 deg)
G0 X0 Y0
G1 Z-0.5 F300
G3 X-40 Y0 I-20 J20 F1200
G3 X0 Y0 I20 J-20
G0 Z5
(petal 5, 240 deg)
G0 X0 Y0
G1 Z-0.5 F300
G3 X-20 Y-34.641 I-27.321 J-7.321 F1200
G3 X0 Y0 I27.321 J7.321
G0 Z5
(petal 6, 300 deg)
G0 X0 Y0
G1 Z-0.5 F300
G3 X20 Y-34.641 I-7.321 J-27.321 F1200
G3 X0 Y0 I7.321 J27.321
G0 Z5
(outer circle, clockwise, in two halves)
G0 X45 Y0
G1 Z-0.5 F300
G2 X-45 Y0 I-45 J0 F1500
G2 X45 Y0 I45 J0
G0 Z5
(spiral of half circles about two centres, 0,0 and 1,0)
G0 X1 Y0
G1 Z-0.5 F300
G3 X-1 Y0 I-1 J0 F1000
G3 X3 Y0 I2 J0
G3 X-3 Y0 I-3 J0
G3 X5 Y0 I4 J0
G3 X-5 Y0 I-5 J0
G3 X7 Y0 I6 J0
G0 Z5
G0 X0 Y0
M5
M30
