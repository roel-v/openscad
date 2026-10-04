use <bracket-pair.scad>

@{type: Assembly, id: "rack", label: "Rack"}
module rack(width = 20) {
    for (side = ["left", "right"]) {
        translate([side == "left" ? 0 : 150, 0, 0]) {
            @{type: Component, id: side}
            bracket_pair(gap = 60, width = width);
        }
    }
}

@{type: Component, id: "rack-a"}
rack(width = 35);
translate([0, 100, 0]) {
    @{type: Component, id: "rack-b"}
    rack(width = 45);
}
