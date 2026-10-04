height = 40;
@{type: Section}
middle_section = {
  id: "section-middle", label: "Middle section",
  origin: [0,0,height/2], normal: [0,0,1], up: [0,1,0],
  adjust: {translation: [0,0,0], rotation: [0,0,0,1], offset: 0},
};
@{type: View}
front_view = {
  id: "view-front", label: "Front", projection: "orthographic",
  direction: [0,1,0], up: [0,0,1], margin: 0.05,
};
cube([20,20,height]);
