include <SCADMaxx/bosl2-anchors.scad>
@{type:Part,id:"plate"} module plate(size=[40,30,10]) {
    @{type:Anchor} top=smx_bosl2_anchor("top",attach_geom(size=size),TOP);
    cuboid(size);
}
@{type:Part,id:"peg"} module peg() {
    @{type:Anchor} bottom=smx_bosl2_anchor("bottom",attach_geom(r=2,h=12),BOT);
    cyl(r=2,h=12,$fn=32);
}
@{type:Attachment} connection={id:"connection",child:{component:"/peg",anchor:"bottom"},
    target:{component:"/plate",anchor:"top"},mode:"mate",spin:30,offset:[0,0,2]};
@{type:Component,id:"plate"} plate();
@{type:Component,id:"peg"} peg();
