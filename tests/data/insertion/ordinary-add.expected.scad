width=20;
// @PROVENANCE@
module smx_insert_item_1() {
cube(size = [10, 10, 30], center = false);
}
union() {
cube([width,20,10]);
    // Fixed local placement.
    translate([0, 0, 10])
        smx_insert_item_1();
}
