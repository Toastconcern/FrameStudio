// ── Shared node-rotation fitter — the GLOBAL V79->V203 converter core, used by BOTH the glTF loader and the OPA
//    loader so there is ONE implementation of "sampled world motion -> spin/sway about an axis". Given a mesh's
//    WORLD positions sampled across an animation clip (frames[0..NS]) plus the rotation node's world PIVOT, it
//    classifies the motion and returns the axis + (continuous omega | oscillation amp+period). Method (see the
//    long-form notes in gltf_loader.h): centroid travels -> ORBIT (axis = centroid-plane normal, noise-immune for
//    slow spins); centroid stationary -> SPIN IN PLACE (axis = angular momentum L). Spin vs sway = accumulated
//    signed sweep. Axis canonicalized (dominant component +) so direction lives in the sign of omega/amp. ──
#pragma once
#include <vector>
#include <cstddef>
#include <cmath>

namespace noderot {

struct Result {
    bool  rotAnim = false;          // a usable rotation/sway was found
    bool  isOsc   = false;          // true = oscillation (sway), false = continuous spin
    float omega   = 0.f;            // signed rad/s (spin)
    float amp     = 0.f;            // signed peak angle rad (sway)
    float period  = 0.f;            // seconds (sway period / spin clip)
    float axis[3] = { 0, 1, 0 };    // unit world axis (canonical: dominant component +)
    float pivot[3]= { 0, 0, 0 };    // rotation node world origin
};

// frames[f] -> pointer to nv*3 world positions at time f*clipDur/NS, f = 0..NS (so frames.size() == NS+1).
inline Result fit(const std::vector<const float*>& frames, size_t nv, const float pivot[3], float clipDur) {
    Result out; for (int k = 0; k < 3; k++) out.pivot[k] = pivot[k];
    int NS = (int)frames.size() - 1;
    if (NS < 4 || nv < 1 || clipDur <= 0.f) return out;
    auto cross = [](const float* a, const float* b, float* o){ o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; };
    auto dot   = [](const float* a, const float* b){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    auto nrm   = [](float* a){ float l=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); if(l>1e-9f){a[0]/=l;a[1]/=l;a[2]/=l;} return l; };
    const float* P0f = frames[0];
    // probe A = farthest from pivot at t=0 (cleanest arc for the signed-angle classification)
    int ai = -1; float aR = 0.f;
    for (size_t v = 0; v < nv; v++){ float d[3]={P0f[v*3]-pivot[0],P0f[v*3+1]-pivot[1],P0f[v*3+2]-pivot[2]}; float r=dot(d,d); if(r>aR){aR=r;ai=(int)v;} }
    if (ai < 0 || aR < 1e-2f) return out;
    std::vector<float> R((size_t)(NS+1)*3);   // probe radius vector r(t)=pos(t)-pivot
    for (int i=0;i<=NS;i++){ const float* P=frames[i]; R[(size_t)i*3]=P[ai*3]-pivot[0]; R[(size_t)i*3+1]=P[ai*3+1]-pivot[1]; R[(size_t)i*3+2]=P[ai*3+2]-pivot[2]; }
    float* r0=&R[0]; float r0l=std::sqrt(dot(r0,r0)); if (r0l<1e-3f) return out;
    int ti=0; float bestV=0.f;                 // step of max probe speed = max angular velocity (sway has 0 vel at t=0)
    for (int i=0;i<NS;i++){ float dx=R[(size_t)(i+1)*3]-R[(size_t)i*3], dy=R[(size_t)(i+1)*3+1]-R[(size_t)i*3+1], dz=R[(size_t)(i+1)*3+2]-R[(size_t)i*3+2]; float s=dx*dx+dy*dy+dz*dz; if(s>bestV){bestV=s;ti=i;} }
    if (bestV < 1e-12f) return out;
    // centroid trajectory over the clip -> ORBIT (centroid travels) vs SPIN IN PLACE (centroid stationary)
    std::vector<float> Cc((size_t)(NS+1)*3);
    for (int i=0;i<=NS;i++){ const float* P=frames[i]; double sx=0,sy=0,sz=0; for(size_t v=0;v<nv;v++){sx+=P[v*3];sy+=P[v*3+1];sz+=P[v*3+2];} Cc[(size_t)i*3]=(float)(sx/(double)nv); Cc[(size_t)i*3+1]=(float)(sy/(double)nv); Cc[(size_t)i*3+2]=(float)(sz/(double)nv); }
    float Cm[3]={0,0,0}; for(int i=0;i<=NS;i++){Cm[0]+=Cc[(size_t)i*3];Cm[1]+=Cc[(size_t)i*3+1];Cm[2]+=Cc[(size_t)i*3+2];} Cm[0]/=(NS+1);Cm[1]/=(NS+1);Cm[2]/=(NS+1);
    float orbitSpan=0.f; for(int i=0;i<=NS;i++){float d[3]={Cc[(size_t)i*3]-Cm[0],Cc[(size_t)i*3+1]-Cm[1],Cc[(size_t)i*3+2]-Cm[2]}; float s=dot(d,d); if(s>orbitSpan)orbitSpan=s;} orbitSpan=std::sqrt(orbitSpan);
    float bodySpan=0.f; for(size_t v=0;v<nv;v++){float d[3]={P0f[v*3]-Cc[0],P0f[v*3+1]-Cc[1],P0f[v*3+2]-Cc[2]}; float s=dot(d,d); if(s>bodySpan)bodySpan=s;} bodySpan=std::sqrt(bodySpan);
    float ax[3]={0,0,0};
    // ORBIT only when the centroid's travel is MUCH larger than the body: a real orbit (King Kai planet circling
    // a distant center) has orbitSpan >> bodySpan. A PENDULUM hinged just outside the mesh (for example a torch flame
    // rocking about its wick node: centroid arc ≈ body size) tripped the old `>` test -> pivot=centroid -> the
    // cooked sway wobbled the flame about its own MIDDLE instead of the hinge ("flame anim not correct").
    bool orbit = orbitSpan > 2.f*bodySpan && orbitSpan > 1e-2f;
    if (orbit) {
        for (int i=0;i<NS;i++){ float a[3]={Cc[(size_t)i*3]-Cm[0],Cc[(size_t)i*3+1]-Cm[1],Cc[(size_t)i*3+2]-Cm[2]}, b[3]={Cc[(size_t)(i+1)*3]-Cm[0],Cc[(size_t)(i+1)*3+1]-Cm[1],Cc[(size_t)(i+1)*3+2]-Cm[2]}, c[3]; cross(a,b,c); ax[0]+=c[0];ax[1]+=c[1];ax[2]+=c[2]; }
        for (int k=0;k<3;k++) out.pivot[k]=Cm[k];   // ORBIT: the rotation center is the centroid's orbit center, not the mesh node origin
    } else {
        const float* A=frames[ti]; const float* B=frames[ti+1];
        double C[3]={0,0,0}; for(size_t v=0;v<nv;v++){C[0]+=A[v*3];C[1]+=A[v*3+1];C[2]+=A[v*3+2];} C[0]/=nv;C[1]/=nv;C[2]/=nv;
        for (size_t v=0;v<nv;v++){ float ri[3]={(float)(A[v*3]-C[0]),(float)(A[v*3+1]-C[1]),(float)(A[v*3+2]-C[2])}, vi[3]={B[v*3]-A[v*3],B[v*3+1]-A[v*3+1],B[v*3+2]-A[v*3+2]}, c[3]; cross(ri,vi,c); ax[0]+=c[0];ax[1]+=c[1];ax[2]+=c[2]; }
    }
    if (nrm(ax) < 1e-6f) return out;
    auto signedAng=[&](const float* a,const float* b)->float{ float da=dot(a,ax),db=dot(b,ax);
        float pA[3]={a[0]-da*ax[0],a[1]-da*ax[1],a[2]-da*ax[2]}, pB[3]={b[0]-db*ax[0],b[1]-db*ax[1],b[2]-db*ax[2]};
        float c[3]; cross(pA,pB,c); return std::atan2(dot(c,ax), dot(pA,pB)); };
    float total=0.f, peak=0.f, maxR=0.f, minR=1e30f;
    for (int i=1;i<=NS;i++){ total += signedAng(&R[(size_t)(i-1)*3], &R[(size_t)i*3]);
        float th = signedAng(r0, &R[(size_t)i*3]); if (std::fabs(th)>std::fabs(peak)) peak=th;
        float rl=std::sqrt(dot(&R[(size_t)i*3],&R[(size_t)i*3])); if(rl>maxR)maxR=rl; if(rl<minR)minR=rl; }
    if (maxR-minR > 0.15f*r0l + 1e-3f) return out;     // radius must stay ~constant -> a pivot rotation, not a slide
    int dom=0; if (std::fabs(ax[1])>std::fabs(ax[dom])) dom=1; if (std::fabs(ax[2])>std::fabs(ax[dom])) dom=2;
    if (ax[dom] < 0.f) { ax[0]=-ax[0]; ax[1]=-ax[1]; ax[2]=-ax[2]; total=-total; peak=-peak; }
    out.axis[0]=ax[0]; out.axis[1]=ax[1]; out.axis[2]=ax[2];
    if (std::fabs(total) > 4.712f) { out.omega = total/clipDur; out.isOsc=false; out.period=clipDur; out.rotAnim = std::fabs(out.omega) > 1e-4f; }
    else { out.isOsc=true; out.amp=peak; out.period=clipDur; out.omega=0.f; out.rotAnim = std::fabs(out.amp) > 0.02f; }
    return out;
}

} // namespace noderot
