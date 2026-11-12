#pragma once
#include "ClothSolver.h"
#include "HairCandidate.h"
#include "CatTail.h"
#include <algorithm>
#include <climits>
#include <cfloat>

namespace flowing_hair
{
    // Exact topology profiles, not a broad 'anything behind the pelvis' mask.
    // Two disjoint index runs are the left/right tail halves in each source.
    struct KhajjitTailProfile
    {
        size_t vertices, triangles, firstFace, secondFace, halfFaces;
        unsigned int indexHash;
        bool cat;
        KhajjitTailProfile() : vertices(0),triangles(0),firstFace(0),secondFace(0),halfFaces(0),indexHash(0),cat(false) {}
        bool tailFace(size_t face) const
        { return cat ? std::binary_search(catTailFaces,catTailFaces+sizeof(catTailFaces)/sizeof(catTailFaces[0]),face) :
            ((face>=firstFace && face-firstFace<halfFaces) || (face>=secondFace && face-secondFace<halfFaces)); }
    };
    inline KhajjitTailProfile khajjitTailProfile(const std::string& name)
    {
        KhajjitTailProfile p;
        const std::string path=normaliseMeshResource(name);
        const std::string leaf=path.substr(path.find_last_of('/')==std::string::npos ? 0 : path.find_last_of('/')+1);
        if (leaf=="khajjit_male.mesh")
        { p.vertices=173470; p.triangles=139062; p.firstFace=68446; p.secondFace=137977; p.halfFaces=1085; p.indexHash=0x829034c1u; }
        if (leaf=="khajjit_female.mesh")
        { p.vertices=67943; p.triangles=33317; p.firstFace=16837; p.secondFace=32333; p.halfFaces=984; p.indexHash=0xbaa60309u; }
        if (leaf=="cat.mesh")
        { p.vertices=5055;p.triangles=8376;p.halfFaces=152;p.indexHash=0xfe964e23u;p.cat=true; }
        return p;
    }
    inline bool splitKhajjitTail(const KhajjitTailProfile& profile,
        const std::vector<Vec3>& positions,const std::vector<unsigned int>& indices,
        std::vector<unsigned int>& body,std::vector<unsigned int>& tail)
    {
        body.clear(); tail.clear();
        if (!profile.vertices || positions.size()!=profile.vertices || indices.size()!=profile.triangles*3) return false;
        unsigned int hash=2166136261u;
        for (size_t i=0;i<indices.size();++i)
        { if (indices[i]>=positions.size()) return false; hash=(hash^indices[i])*16777619u; }
        if (hash!=profile.indexHash) return false;
        std::vector<unsigned char> membership(positions.size(),0);
        for (size_t i=0;i<indices.size();++i)
        {
            const bool isTail=profile.tailFace(i/3);
            membership[indices[i]]|=isTail ? 2 : 1;
            (isTail ? tail : body).push_back(indices[i]);
        }
        if (tail.size()!=profile.halfFaces*6 || body.empty()) { body.clear(); tail.clear(); return false; }
        float bottom=FLT_MAX,top=-FLT_MAX;
        for (size_t i=0;i<membership.size();++i)
        {
            if (membership[i]==3) { body.clear(); tail.clear(); return false; }
            if (membership[i]!=2) continue;
            const Vec3& p=positions[i];
            // Generous enough for normal physique morphs; reject unrelated geometry.
            if (!(profile.cat ? (p.x>-3 && p.x<3 && p.y>0 && p.y<15 && p.z>-22 && p.z<-6) :
                (p.x>-2 && p.x<2 && p.y>-1 && p.y<13 && p.z>-7 && p.z<0)))
            { body.clear(); tail.clear(); return false; }
            bottom=std::min(bottom,p.y); top=std::max(top,p.y);
        }
        if (top-bottom<5 || top<8) { body.clear(); tail.clear(); return false; }
        return true;
    }
    inline std::vector<bool> khajjitTailPins(const std::vector<Vec3>& positions)
    {
        std::vector<bool> pins(positions.size(),false);
        if (positions.empty()) return pins;
        float bottom=positions[0].y,top=bottom;
        for (size_t i=1;i<positions.size();++i)
        { bottom=std::min(bottom,positions[i].y); top=std::max(top,positions[i].y); }
        const float root=top-(top-bottom)*0.06f;
        for (size_t i=0;i<positions.size();++i) pins[i]=positions[i].y>=root;
        return pins;
    }
}
