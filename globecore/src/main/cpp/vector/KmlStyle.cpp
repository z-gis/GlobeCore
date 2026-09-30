#include "vector/KmlStyle.h"

#include <cctype>
#include <map>
#include <string>

#include "gdal/cpl_string.h"
#include "gdal/cpl_vsi.h"

#include "util/Log.h"

namespace globecore {
namespace {

std::string toLower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// 去首尾空白
std::string trim(const std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// 解常见 XML 实体（名称匹配用）
std::string unescapeXml(const std::string &in) {
    if (in.find('&') == std::string::npos) return in;
    std::string r;
    r.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { r.push_back(in[i]); continue; }
        if (in.compare(i, 5, "&amp;") == 0) { r.push_back('&'); i += 4; }
        else if (in.compare(i, 4, "&lt;") == 0) { r.push_back('<'); i += 3; }
        else if (in.compare(i, 4, "&gt;") == 0) { r.push_back('>'); i += 3; }
        else if (in.compare(i, 6, "&quot;") == 0) { r.push_back('"'); i += 5; }
        else if (in.compare(i, 6, "&apos;") == 0) { r.push_back('\''); i += 5; }
        else r.push_back('&');
    }
    return r;
}

/// 从 from 起查找下一处带标签边界校验的 <tag>…</tag>：命中则输出内部文本 inner（及起始标签原文
/// openTag，可选），并把 from 推进到闭合标签之后，返回 true；无更多命中返回 false。
/// 边界校验避免前缀误配（如查 "color" 命中 "colorMode"、查 "Style" 命中 "StyleMap"）。
bool extractInner(const std::string &s, const std::string &tag, size_t &from,
                  std::string &inner, std::string *openTag = nullptr) {
    const std::string open = "<" + tag;
    size_t os = from;
    for (;;) {
        os = s.find(open, os);
        if (os == std::string::npos) return false;
        const size_t nameEnd = os + open.size();
        const char nx = nameEnd < s.size() ? s[nameEnd] : '\0';
        if (nx != '>' && nx != ' ' && nx != '\t' && nx != '\n' && nx != '\r' && nx != '/') {
            os = nameEnd; // 前缀误配，继续找下一处
            continue;
        }
        const size_t gt = s.find('>', nameEnd);
        if (gt == std::string::npos) return false;
        if (openTag != nullptr) *openTag = s.substr(os, gt - os + 1);
        if (s[gt - 1] == '/') { // 自闭合 <tag/>
            inner.clear();
            from = gt + 1;
            return true;
        }
        const std::string close = "</" + tag + ">";
        const size_t cs = s.find(close, gt + 1);
        if (cs == std::string::npos) { os = gt + 1; continue; }
        inner = s.substr(gt + 1, cs - (gt + 1));
        from = cs + close.size();
        return true;
    }
}

/// 从起始标签原文取属性值（如 id="x"）
std::string getAttr(const std::string &tag, const std::string &name) {
    size_t p = 0;
    for (;;) {
        p = tag.find(name, p);
        if (p == std::string::npos) return std::string();
        const bool preOk = (p == 0) || tag[p - 1] == ' ' || tag[p - 1] == '\t'
                           || tag[p - 1] == '\n' || tag[p - 1] == '\r';
        size_t q = p + name.size();
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) q++;
        if (preOk && q < tag.size() && tag[q] == '=') {
            q++;
            while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) q++;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                const char quote = tag[q];
                const size_t e = tag.find(quote, q + 1);
                if (e != std::string::npos) return tag.substr(q + 1, e - q - 1);
            }
        }
        p = q > p ? q : p + name.size();
    }
}

/// 解析 KML 颜色 aabbggrr（8 位十六进制，容错 # 前缀/空白）→ RGBA[0,1]。失败 false。
bool parseKmlColor(const std::string &in, float out[4]) {
    std::string h;
    for (char c : in)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (h.size() < 6) return false;
    auto hv = [](char c) -> int { return c <= '9' ? c - '0' : c - 'a' + 10; };
    int aa = 255, bb = 0, gg = 0, rr = 0;
    if (h.size() >= 8) {
        aa = hv(h[0]) * 16 + hv(h[1]);
        bb = hv(h[2]) * 16 + hv(h[3]);
        gg = hv(h[4]) * 16 + hv(h[5]);
        rr = hv(h[6]) * 16 + hv(h[7]);
    } else { // 6 位：bbggrr，alpha 视为不透明
        bb = hv(h[0]) * 16 + hv(h[1]);
        gg = hv(h[2]) * 16 + hv(h[3]);
        rr = hv(h[4]) * 16 + hv(h[5]);
    }
    out[0] = rr / 255.0f;
    out[1] = gg / 255.0f;
    out[2] = bb / 255.0f;
    out[3] = aa / 255.0f;
    return true;
}

/// 取某 style 块内子标签（LineStyle/PolyStyle）首个 <color> 的 RGBA。
bool firstColorOfSubTag(const std::string &block, const std::string &subTag, float out[4]) {
    size_t pos = 0;
    std::string sinner;
    while (extractInner(block, subTag, pos, sinner)) {
        std::string cinner;
        size_t cpos = 0;
        while (extractInner(sinner, "color", cpos, cinner))
            if (parseKmlColor(cinner, out)) return true;
    }
    return false;
}

/// 读 KML/KMZ 全文（.kml 直读；.kmz 经 /vsizip 取包内首个 .kml）。
bool readKmlText(const std::string &path, std::string &out) {
    std::string target = path;
    if (path.size() >= 4 && toLower(path.substr(path.size() - 4)) == ".kmz") {
        const std::string zip = "/vsizip/" + path;
        char **entries = VSIReadDir(zip.c_str());
        std::string kmlName;
        for (char **p = entries; p != nullptr && *p != nullptr; ++p) {
            const std::string e = *p;
            if (e.size() >= 4 && toLower(e.substr(e.size() - 4)) == ".kml") { kmlName = e; break; }
        }
        CSLDestroy(entries);
        if (kmlName.empty()) return false;
        target = zip + "/" + kmlName;
    }
    VSILFILE *f = VSIFOpenL(target.c_str(), "rb");
    if (f == nullptr) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = VSIFReadL(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    VSIFCloseL(f);
    return !out.empty();
}

/// styleUrl 文本 → 本地样式 id（去 # 前缀 / 空白）；外部引用（http/非 #）返回空串表示不处理。
std::string localStyleId(const std::string &styleUrlRaw) {
    const std::string u = trim(styleUrlRaw);
    if (u.empty() || u[0] != '#') return std::string(); // 仅处理本地 #id 引用
    return u.substr(1);
}

} // namespace

std::map<std::string, KmlStyleColors> parseKmlStyleMap(const std::string &path) {
    std::map<std::string, KmlStyleColors> result;
    std::string doc;
    if (!readKmlText(path, doc)) {
        LOGW("[KmlStyle] 读取 KML 文本失败，逐要素配色跳过: %s", path.c_str());
        return result;
    }

    // 1) 样式定义：Style id → 颜色；StyleMap id → normal 指向的 Style id
    std::map<std::string, KmlStyleColors> styles;
    std::map<std::string, std::string> styleMapNormal;
    {
        size_t from = 0;
        std::string block, openTag;
        while (extractInner(doc, "Style", from, block, &openTag)) {
            const std::string id = getAttr(openTag, "id");
            if (id.empty()) continue;
            KmlStyleColors c;
            c.hasFill = firstColorOfSubTag(block, "PolyStyle", c.fill);
            c.hasLine = firstColorOfSubTag(block, "LineStyle", c.line);
            if (c.hasFill || c.hasLine) styles[id] = c;
        }
    }
    {
        size_t from = 0;
        std::string block, openTag;
        while (extractInner(doc, "StyleMap", from, block, &openTag)) {
            const std::string id = getAttr(openTag, "id");
            if (id.empty()) continue;
            // 找 <key>normal</key> 之后首个 <styleUrl>
            const size_t kp = block.find("normal");
            if (kp == std::string::npos) continue;
            size_t sp = kp;
            std::string su;
            if (extractInner(block, "styleUrl", sp, su)) {
                const std::string tgt = localStyleId(su);
                if (!tgt.empty()) styleMapNormal[id] = tgt;
            }
        }
    }

    auto resolve = [&](const std::string &id) -> KmlStyleColors {
        std::string real = id;
        const auto sm = styleMapNormal.find(id);
        if (sm != styleMapNormal.end()) real = sm->second;
        const auto it = styles.find(real);
        return it != styles.end() ? it->second : KmlStyleColors();
    };

    // 2) Placemark name → 解析后样式颜色（同名保留首个）
    {
        size_t from = 0;
        std::string pm;
        while (extractInner(doc, "Placemark", from, pm)) {
            std::string nameRaw;
            size_t np = 0;
            if (!extractInner(pm, "name", np, nameRaw)) continue;
            const std::string name = trim(unescapeXml(nameRaw));
            if (name.empty()) continue;
            std::string suRaw;
            size_t sp = 0;
            if (!extractInner(pm, "styleUrl", sp, suRaw)) continue;
            const std::string id = localStyleId(suRaw);
            if (id.empty()) continue;
            if (result.find(name) != result.end()) continue; // 首个生效
            result[name] = resolve(id);
        }
    }

    LOGI("[KmlStyle] 解析完成 styles=%zu placemarkName→style=%zu path=%s",
         styles.size(), result.size(), path.c_str());
    return result;
}

} // namespace globecore
