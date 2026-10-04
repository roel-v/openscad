@{type: Part, id: "bracket", label: "Bracket"}
module bracket(width = 30) {
    difference() {
        cube([width, 10, 3]);
        translate([width / 2, 5, -1]) cylinder(d = 3, h = 5, $fn = 32);
    }
}

@{type: Assembly, id: "bracket-pair", label: "Bracket pair"}
module bracket_pair(gap = 50, width = 30) {
    @{type: Component, id: "left", label: "Left bracket"}
    bracket(width = width);
    translate([gap, 0, 0]) {
        @{type: Component, id: "right", label: "Right bracket"}
        bracket(width = width + 10);
    }
}

@{type: Component, id: "pair", label: "Bracket pair"}
bracket_pair(gap = 60, width = 35);
