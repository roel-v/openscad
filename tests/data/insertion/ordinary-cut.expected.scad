width=20;
// @PROVENANCE@
module smx_insert_item_1() {
cylinder(d = 4, h = 12, center = false);
}
difference() {
    union() {
cube([width,20,10]);
    }
    // Fixed local placement.
    multmatrix([[-1, 0, 0, 10], [0, 1, 0, 10], [0, 0, -1, 10], [0, 0, 0, 1]])
        smx_insert_item_1();
}
