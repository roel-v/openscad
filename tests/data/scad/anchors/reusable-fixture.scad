@{type: Part, id: "plate"}
module plate(width = 40, height = 10) {
    @{type: Anchor}
    mount = {id: "mount", position: [width/2, 0, height], normal: [0,0,1], up: [0,1,0]};
    cube([width, 20, height]);
}
@{type: Part, id: "peg"}
module peg(height = 12) {
    @{type: Anchor}
    base = {id: "base", position: [0,0,0], normal: [0,0,-1], up: [0,1,0]};
    cylinder(r = 2, h = height, $fn = 32);
}
@{type: Assembly, id: "fixture"}
module fixture(width = 40) {
    @{type: Component, id: "plate"} plate(width = width);
    @{type: Component, id: "peg"} peg();
    @{type: Attachment}
    peg_on_plate = {id: "peg-on-plate", child: {component: "peg", anchor: "base"},
                   target: {component: "plate", anchor: "mount"}, mode: "mate", offset: [0,0,0], spin: 0};
}
@{type: Component, id: "fixture-a"} fixture(width = 40);
translate([100,0,0]) @{type: Component, id: "fixture-b"} fixture(width = 60);
