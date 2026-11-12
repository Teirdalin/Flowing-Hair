#pragma once
#include <string>
#include <cctype>

namespace flowing_hair
{
    enum HairKind { NotHair, ScalpHair, FacialHair };
    inline std::string normaliseMeshResource(const std::string& input)
    {
        if (input.empty() || input.size() > 512) return std::string();
        std::string path;
        for (size_t i=0;i<input.size();++i)
        {
            const unsigned char c=static_cast<unsigned char>(input[i]);
            if (c < 32) return std::string();
            const char next=c=='\\' ? '/' : static_cast<char>(std::tolower(c));
            if (next=='/' && !path.empty() && path[path.size()-1]=='/') continue;
            path+=next;
        }
        while (path.size()>=2 && path[0]=='.' && path[1]=='/') path.erase(0,2);
        if (path.size()<6 || path.substr(path.size()-5)!=".mesh") return std::string();
        return path;
    }
    inline bool pathSuffixMatches(const std::string& longer,const std::string& shorter)
    {
        return longer.size()>shorter.size() &&
            longer[longer.size()-shorter.size()-1]=='/' &&
            longer.compare(longer.size()-shorter.size(),shorter.size(),shorter)==0;
    }
    inline int hairstyleMeshMatch(const std::string& resource,const std::string& declaredMesh)
    {
        const std::string actual=normaliseMeshResource(resource), declared=normaliseMeshResource(declaredMesh);
        if (actual.empty() || declared.empty()) return 0;
        if (actual==declared) return 3;
        const bool actualBare=actual.find('/')==std::string::npos;
        const bool declaredBare=declared.find('/')==std::string::npos;
        if (pathSuffixMatches(actual,declared) || pathSuffixMatches(declared,actual))
            return (actualBare || declaredBare) ? 2 : 3;
        // Do not collapse two different explicit directories to the same leaf.
        return 0;
    }
    inline bool nameContains(const std::string& value, const char* needle)
    {
        if (value.size() > 512) return false;
        for (size_t i=0;i<value.size();++i)
        {
            size_t n=0;
            while (needle[n] && i+n<value.size() &&
                std::tolower(static_cast<unsigned char>(value[i+n])) == needle[n]) ++n;
            if (!needle[n]) return true;
        }
        return false;
    }
    inline HairKind classifyHairName(const std::string& name)
    {
        if (nameContains(name,"beard") || nameContains(name,"facial") ||
            nameContains(name,"moustache") || nameContains(name,"mustache")) return FacialHair;
        if (nameContains(name,"hair") || nameContains(name,"scalp") ||
            nameContains(name,"ponytail") || nameContains(name,"mohawk") || nameContains(name,"nutfro")) return ScalpHair;
        return NotHair;
    }
    inline HairKind classifyHair(const std::string& meshName, const std::string& entityName)
    {
        const HairKind mesh = classifyHairName(meshName);
        // A scene-instance label must not override a known mesh's identity.
        return mesh != NotHair ? mesh : classifyHairName(entityName);
    }
    inline bool usesLinkedPonyTip(const std::string& meshName)
    {
        // FCS Haircut21-pony: both exports contain an independently meshed tip.
        // Exact resource leaf only; do not change other scalp/beard root policies.
        const size_t slash = meshName.find_last_of("/\\");
        const std::string leaf = meshName.substr(slash == std::string::npos ? 0 : slash + 1);
        return (leaf.size() == 19 && nameContains(leaf, "haircut_male21.mesh")) ||
               (leaf.size() == 21 && nameContains(leaf, "haircut_female21.mesh"));
    }
}
