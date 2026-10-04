// Bounded adapter for BOSL2 e173fa0ae45f9e2082e0d8c0382dae98621b7c20 (2.0.766).
// The external BOSL2 library is required; it is not distributed with SCADMaxx.
include <BOSL2/std.scad>

function _smx_finite3(v) = is_list(v) && len(v)==3 && [for(x=v) if(!is_finite(x)) 1]==[];
function _smx_finite_matrix(m,n) = is_list(m) && len(m)==n &&
    [for(row=m) if(!is_list(row) || len(row)!=n || [for(x=row) if(!is_finite(x)) 1]!=[]) 1]==[];
function _smx_unit(v) = let(s=max([for(x=v) abs(x)]))
    assert(_smx_finite3(v) && s>0,"SCADMaxx BOSL2 anchor requires a finite nonzero direction")
    let(w=v/s) w/norm(w);
// Largest singular value via the symmetric 3x3 Gram eigenvalue formula.
// Inputs are normalized first; the reciprocal inverse norm avoids subtracting
// nearly equal eigenvalues when checking the smallest singular value.
function _smx_sigma_max(m) =
    let(g=transpose(m)*m,q=(g[0][0]+g[1][1]+g[2][2])/3,
        a=g-q*ident(3),p=sqrt(sum([for(row=a) row*row])/6))
    p==0 ? sqrt(q) :
    let(r=determinant(a/p)/2,phi=acos(max(-1,min(1,r)))/3)
    sqrt(max(0,q+2*p*cos(phi)));
function _smx_inverse3(m) =
    let(s=max([for(row=m) for(x=row) abs(x)]))
    assert(s>0,"SCADMaxx BOSL2 placement is singular")
    let(a=m/s,d=determinant(a))
    assert(d!=0,"SCADMaxx BOSL2 placement is singular")
    let(i=transpose([cross(a[1],a[2]),cross(a[2],a[0]),cross(a[0],a[1])])/d,
        imax=max([for(row=i) for(x=row) abs(x)]),
        ratio=1/(_smx_sigma_max(a)*imax*_smx_sigma_max(i/imax)))
    assert(_smx_finite_matrix(i,3) && ratio>128*2.220446049250313e-16,
        "SCADMaxx BOSL2 placement is outside supported conditioning")
    i/s;

// Return ordinary native Anchor fields. Geometry is an explicit attach_geom()
// descriptor; no bounds inference or arbitrary module introspection is used.
function smx_bosl2_anchor(id,geometry,selector,label=undef,placement=ident(4)) =
    assert(BOSL_VERSION==[2,0,766],"SCADMaxx BOSL2 bridge requires BOSL2 2.0.766")
    assert(is_string(id) && len(id)>0,"SCADMaxx BOSL2 anchor requires a nonempty id")
    assert(is_undef(label) || is_string(label),"SCADMaxx BOSL2 anchor label must be a string")
    assert(is_list(geometry) && len(geometry)>0 &&
        in_list(geometry[0],["prismoid","conoid","spheroid","point"]),
        "SCADMaxx BOSL2 bridge supports only 3D prismoid, conoid, spheroid and point descriptors")
    assert((is_string(selector) && len(selector)>0) || _smx_finite3(selector),
        "SCADMaxx BOSL2 anchor selector must be a name or 3D vector")
    assert(_smx_finite_matrix(placement,4) && norm(placement[3]-[0,0,0,1])<=1e-10,
        "SCADMaxx BOSL2 placement must be a finite affine 4x4 matrix")
    let(anchor=_find_anchor(selector,geometry))
    assert(is_list(anchor) && len(anchor)>=4 && _smx_finite3(anchor[1]) &&
        _smx_finite3(anchor[2]) && is_finite(anchor[3]),"Malformed BOSL2 anchor result")
    let(b=rot(from=UP,to=_smx_unit(anchor[2]))*zrot(anchor[3]),
        l=[for(i=[0:2]) [for(j=[0:2]) placement[i][j]]],inverse=_smx_inverse3(l),
        p=placement*concat(anchor[1],[1]),
        n=_smx_unit(transpose(inverse)*[for(i=[0:2]) b[i][2]]),
        raw=l*[for(i=[0:2]) b[i][1]],u=_smx_unit(raw),t=u-n*(u*n))
    assert(norm(t)>1e-10 && _smx_finite3([p[0],p[1],p[2]]),"Unavailable transformed BOSL2 anchor frame")
    {id:id,label:is_undef(label)?id:label,position:[p[0],p[1],p[2]],normal:n,up:_smx_unit(t),capture:"parametric"};
