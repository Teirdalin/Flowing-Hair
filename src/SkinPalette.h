#pragma once

#include <cfloat>
#include <cstddef>

namespace flowing_hair
{
    // Tail-only: all shader slots use ONE attachment transform. Therefore the
    // artist's normalized per-vertex bone weights cannot bend the simulated tail.
    // Root weights locate the attachment; they do not deform individual vertices.
    inline bool packTailAttachmentPalette(const float* matrices4x4,size_t count,
        const float* rootWeights,float* rows3x4,size_t capacity)
    {
        if(!matrices4x4 || !rootWeights || !rows3x4 || !count || count>capacity || capacity>60) return false;
        float anchor[12]={0},total=0;
        for(size_t m=0;m<60;++m)
        {
            const float w=rootWeights[m];
            if(!(w>=0 && w<=1)) return false;
            if(w==0) continue;
            if(m>=count) return false;
            for(size_t n=0;n<12;++n)
            {
                const float v=matrices4x4[m*16+n];
                if(!(v>=-FLT_MAX && v<=FLT_MAX)) return false;
                anchor[n]+=v*w;
            }
            total+=w;
        }
        if(!(total>=0.99f && total<=1.01f)) return false;
        for(size_t n=0;n<12;++n)
        {
            anchor[n]/=total;
            if(!(anchor[n]>=-FLT_MAX && anchor[n]<=FLT_MAX)) return false;
        }
        for(size_t m=0;m<capacity;++m)
            for(size_t n=0;n<12;++n) rows3x4[m*12+n]=anchor[n];
        return true;
    }

    // Kenshi skin.hlsl: row-major float3x4 worldMatrix3x4Array[60].
    // The input is the SOURCE renderable's world-space palette, not local bone
    // poses. Applying the character transform again would double-transform it.
    inline bool packSkinPalette(const float* matrices4x4, size_t count,
                                float* rows3x4, size_t capacity)
    {
        if (!matrices4x4 || !rows3x4 || !count || count > capacity || capacity > 60)
            return false;
        for (size_t m = 0; m < count; ++m)
            for (size_t n = 0; n < 16; ++n)
            {
                const float f = matrices4x4[m * 16 + n];
                if (!(f >= -FLT_MAX && f <= FLT_MAX)) return false;
            }
        for (size_t m = 0; m < capacity; ++m)
            for (size_t n = 0; n < 12; ++n)
                rows3x4[m * 12 + n] = matrices4x4[(m < count ? m : 0) * 16 + n];
        return true;
    }
}
