// Authored provider example; no third-party library required.
module bracket(width=20, depth=12, height=5, diameter=4, make_hole=true,
               segments=32, label="Bracket", hole_shape="round") {
    echo(label=label);
    difference() {
        cube([width, depth, height]);
        if (make_hole) translate([width/2, depth/2, -0.5]) {
            if (hole_shape == "round") cylinder(d=diameter, h=height+1, $fn=segments);
            else translate([-diameter/2,-diameter/2,0]) cube([diameter,diameter,height+1]);
        }
    }
}
