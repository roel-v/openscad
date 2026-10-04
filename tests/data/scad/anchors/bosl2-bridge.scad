include <SCADMaxx/bosl2-anchors.scad>
gp=attach_geom(size=[20,30,10]);
gc=attach_geom(size=[4,6,8]);
np=attach_geom(size=[20,30,10],anchors=[named_anchor("mount",[2,3,4],[1,2,3],35)]);
nc=attach_geom(size=[4,6,8],anchors=[named_anchor("plug",[1,-2,3],[2,-1,1],20)]);
function bridge_frame(a)=let(n=a.normal,u=a.up,x=cross(u,n),p=a.position)
    [[x.x,u.x,n.x,p.x],[x.y,u.y,n.y,p.y],[x.z,u.z,n.z,p.z],[0,0,0,1]];
function expected_frame(g,s)=let(a=_find_anchor(s,g)) move(a[1])*rot(from=UP,to=a[2])*zrot(a[3]);
@{type:Anchor} top=smx_bosl2_anchor("top",gp,TOP);
@{type:Anchor} edge=smx_bosl2_anchor("edge",gp,TOP+RIGHT);
@{type:Anchor} corner=smx_bosl2_anchor("corner",gp,TOP+RIGHT+BACK);
@{type:Anchor} cylinder_end=smx_bosl2_anchor("cylinder-end",attach_geom(r=5,h=20),TOP);
@{type:Anchor} cylinder_radial=smx_bosl2_anchor("cylinder-radial",attach_geom(r=5,h=20),RIGHT);
@{type:Anchor} sphere_direction=smx_bosl2_anchor("sphere-direction",attach_geom(r=10),[1,2,3]);
@{type:Anchor} named=smx_bosl2_anchor("named",np,"mount");
placement=move([5,8,3])*rot([12,34,56])*scale([-2,3,4]);
@{type:Anchor} affine=smx_bosl2_anchor("affine",np,"mount",placement=placement);
@{type:Anchor} reoriented=smx_bosl2_anchor("reoriented",gp,TOP,
    placement=reorient(anchor=RIGHT,spin=25,orient=BACK,geom=gp));
override_geom=attach_geom(size=[20,30,10],override=function(a) a==TOP?[[1,2,3],[1,2,3],45]:[undef,undef,undef]);
@{type:Anchor} overridden=smx_bosl2_anchor("overridden",override_geom,TOP);
for(pair=[[gp,TOP,top],[gp,TOP+RIGHT,edge],[gp,TOP+RIGHT+BACK,corner],[np,"mount",named]])
    assert(approx(bridge_frame(pair[2]),expected_frame(pair[0],pair[1]),eps=1e-10));

// Compare the implemented bridge to the library's actual attachable/attach path.
module check_mate(name,gp,gc,pa,ca,spin=0,gap=0) {
    parent=bridge_frame(smx_bosl2_anchor("parent",gp,pa));
    child=bridge_frame(smx_bosl2_anchor("child",gc,ca));
    expected=parent*move([0,0,gap])*zrot(spin)*yrot(180)*matrix_inverse(child);
    attachable(anchor=CENTER,spin=0,orient=UP,geom=gp) {
        cuboid([1,1,1]);
        attach(pa,ca,spin=spin,overlap=-gap) attachable(anchor=CENTER,spin=0,orient=UP,geom=gc) {
            cuboid([1,1,1]);
            union() {assert(approx($transform,expected,eps=1e-8),str(name," bridge/native BOSL2 mismatch"));echo(bridge_mate_passed=name);}
        }
    }
}
check_mate("top-bottom",gp,gc,TOP,BOT);
check_mate("right-spin-gap",gp,gc,RIGHT,BOT,spin=30,gap=2);
check_mate("bottom-negative-gap",gp,gc,BOT,TOP,spin=-45,gap=-1);
check_mate("oblique",np,nc,"mount","plug",spin=15,gap=2);
@{type:Anchor} parent_named=smx_bosl2_anchor("parent-named",np,"mount");
@{type:Anchor} child_named=smx_bosl2_anchor("child-named",nc,"plug");
expected_named=expected_frame(np,"mount")*move([0,0,2])*zrot(15)*yrot(180)*matrix_inverse(expected_frame(nc,"plug"));
@{type:Anchor} expected_child={id:"expected-child",position:[for(i=[0:2]) expected_named[i][3]],
    normal:[for(i=[0:2]) expected_named[i][2]],up:[for(i=[0:2]) expected_named[i][1]]};
