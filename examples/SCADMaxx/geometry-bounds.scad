// Enable object-function. Capture computes module parameters without displaying the shape.
module plate() {
    difference() {
        cube([40, 30, 5]);
        translate([25, -1, -1]) cube([20, 32, 7]);
    }
}

shape = {{ plate(); }};
bounds = geometry_bounds(shape);
echo(bounds); // min=[0,0,0], max=[25,30,5], size=[25,30,5], center=[12.5,15,2.5]
plate();
translate([bounds.max.x + 5, 0, 0]) cube([5, bounds.size.y, 5]);
