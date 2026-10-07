// Exact Boolean bounds, not preview operand bounds.
cut = {{
    difference() {
        cube([10, 8, 6]);
        translate([4, -1, -1]) cube([7, 10, 8]);
    }
}};
b = geometry_bounds(cut);
assert(b.min == [0, 0, 0]);
assert(b.max == [4, 8, 6]);
assert(b.size == [4, 8, 6]);
assert(b.center == [2, 4, 3]);
assert(geometry_bounds(shape=cut) == b);

function box_at(x, width) = {{ translate([x, -2, 3]) cube([width, 4, 5]); }};
function bounds_of(shape) = geometry_bounds(shape);
saved = box_at(7, 2);
assert(bounds_of(saved).min == [7, -2, 3]);
assert(bounds_of(saved).max == [9, 2, 8]);
assert(geometry_bounds(box_at(-10, 3)).max == [-7, 2, 8]);
assert(bounds_of(saved).max == [9, 2, 8]); // another call does not mutate the capture

flat = {{ translate([2, 3]) square([4, 5]); }};
assert(geometry_bounds(flat).min == [2, 3, 0]);
assert(geometry_bounds(flat).max == [6, 8, 0]);

many = {{ cube(1); translate([5, 0, 0]) cube(2); }};
assert(geometry_bounds(many).max == [7, 2, 2]);
local = {{ side = 3; module box() { cube(side); } box(); }};
assert(geometry_bounds(local).size == [3, 3, 3]);
assert(geometry_bounds([saved][0]).max == [9, 2, 8]);
assert(geometry_bounds({shape: saved}.shape).max == [9, 2, 8]);
assert(str(saved) == "<geometry>");
echo("geometry bounds passed");
