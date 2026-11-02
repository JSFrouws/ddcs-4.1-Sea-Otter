(arc_rosette - G2/G3 test: 6 petals of two arcs each, a circle round them, a spiral in the middle)
(absolute, work coordinates of the active WCS, fits in 100 x 100 mm, Z -0.5 to 5)
G21 G90 G17
M3 S12000
G0 Z5
(petal 1)
G0 X0 Y0
G1 Z-0.5 F300
G3 X40 Y0 I20 J-20 F1200
G3 X0 Y0 I-20 J20
G0 Z5
(petal 2)
G0 X0 Y0
G1 Z-0.5 F300
G3 X20 Y34.641 I24.641 J-4.641 F1200
G3 X0 Y0 I-4.641 J-24.641
G0 Z5
(petal 3)
G0 X0 Y0
G1 Z-0.5 F300
G3 X-20 Y34.641 I4.641 J24.641 F1200
G3 X0 Y0 I-24.641 J-4.641
G0 Z5
(petal 4)
G0 X0 Y0
G1 Z-0.5 F300
G3 X-40 Y0 I-20 J20 F1200
G3 X0 Y0 I20 J-20
G0 Z5
(petal 5)
G0 X0 Y0
G1 Z-0.5 F300
G3 X-20 Y-34.641 I-24.641 J4.641 F1200
G3 X0 Y0 I4.641 J24.641
G0 Z5
(petal 6)
G0 X0 Y0
G1 Z-0.5 F300
G3 X20 Y-34.641 I-4.641 J-24.641 F1200
G3 X0 Y0 I24.641 J4.641
G0 Z5
(outer circle, clockwise, in two halves)
G0 X45 Y0
G1 Z-0.5 F300
G2 X-45 Y0 I-45 J0 F1500
G2 X45 Y0 I45 J0
G0 Z5
(spiral of quarter arcs growing from the centre)
G0 X2 Y0
G1 Z-0.5 F300
G3 X0 Y3 I-2 J0 F1000
G3 X-4 Y0 I0 J-3
G3 X0 Y-5 I4 J0
G3 X6 Y0 I0 J5
G3 X0 Y7 I-6 J0
G3 X-8 Y0 I0 J-7
G3 X0 Y-9 I8 J0
G3 X10 Y0 I0 J9
G0 Z5
G0 X0 Y0
M5
M30
