# a 0.4 x 0.1 x 0.1 m box, a solid a face, named as people name them (t_stl_name_kinds): Inlet, Outlet_2, outlet1, pinlet
# (a name that only contains "inlet"), walls, walls2
import sys
f = open(sys.argv[1], "w")
def quad(n, a, b, c, d):
    f.write("solid %s\n" % n)
    for t in ((a, b, c), (a, c, d)):
        f.write(" facet normal 0 0 0\n  outer loop\n" + "".join("   vertex %g %g %g\n" % v for v in t) + "  endloop\n endfacet\n")
    f.write("endsolid %s\n" % n)
quad("Inlet", (0, 0, 0), (0, 0, .1), (0, .1, .1), (0, .1, 0))
quad("Outlet_2", (.4, 0, 0), (.4, .1, 0), (.4, .1, .1), (.4, 0, .1))
quad("outlet1", (0, 0, 0), (.4, 0, 0), (.4, 0, .1), (0, 0, .1))
quad("pinlet", (0, .1, 0), (0, .1, .1), (.4, .1, .1), (.4, .1, 0))
quad("walls", (0, 0, 0), (0, .1, 0), (.4, .1, 0), (.4, 0, 0))
quad("walls2", (0, 0, .1), (.4, 0, .1), (.4, .1, .1), (0, .1, .1))
