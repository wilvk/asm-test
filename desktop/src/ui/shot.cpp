#include "ui/shot.h"

#include <nlohmann/json.hpp>

#include "scene3d/layers.h" // scene_layers_all()

namespace asmdesk {

bool shot_view_from_name(const std::string &n, ViewId &out) {
    if (n == "summary")  { out = ViewId::Summary;  return true; }
    if (n == "canvas")   { out = ViewId::Canvas;   return true; }
    if (n == "timeline") { out = ViewId::Timeline; return true; }
    if (n == "slice")    { out = ViewId::Slice;    return true; }
    if (n == "diff")     { out = ViewId::Diff;     return true; }
    if (n == "observer") { out = ViewId::Observer; return true; }
    if (n == "loom")     { out = ViewId::Loom;     return true; }
    if (n == "scrubber") { out = ViewId::Scrubber; return true; }
    if (n == "abixray")  { out = ViewId::AbiXray;  return true; }
    if (n == "scene3d")  { out = ViewId::Scene3D;  return true; }
    return false;
}

bool shot_scene_from_name(const std::string &n, scene3d::SceneKind &out) {
    using K = scene3d::SceneKind;
    if (n == "Plane")        { out = K::Plane;        return true; }
    if (n == "Divergence")   { out = K::Divergence;   return true; }
    if (n == "Invocation")   { out = K::Invocation;   return true; }
    if (n == "ModuleRibbon") { out = K::ModuleRibbon; return true; }
    if (n == "LanePrism")    { out = K::LanePrism;    return true; }
    if (n == "SessionFlow")  { out = K::SessionFlow;  return true; }
    return false;
}

void shot_clear_layers(scene3d::SceneLayers &out) {
    for (const scene3d::LayerDesc &d : scene3d::scene_layers_all())
        out.*(d.flag) = false;
}

bool shot_apply_layers(const std::vector<std::string> &ids,
                       scene3d::SceneLayers &out, std::string &err) {
    err.clear();
    for (const std::string &id : ids) {
        bool found = false;
        for (const scene3d::LayerDesc &d : scene3d::scene_layers_all())
            if (id == d.id) {
                out.*(d.flag) = true;
                found = true;
                break;
            }
        if (!found) {
            err = "unknown layer id \"" + id + "\"";
            return false;
        }
    }
    return true;
}

bool shot_manifest_parse(const std::string &text, std::vector<ShotSpec> &out,
                         std::string &err) {
    out.clear();
    err.clear();

    // Non-throwing parse: a corrupt manifest must produce a MESSAGE, not an
    // exception through a caller that has a GL context open.
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        err = "shot manifest: not a JSON array";
        return false;
    }

    for (const nlohmann::json &e : j) {
        if (!e.is_object()) {
            err = "shot manifest: an entry is not an object";
            return false;
        }
        ShotSpec s;
        s.name = e.value("name", std::string());
        if (s.name.empty()) {
            err = "shot manifest: an entry has no \"name\"";
            return false;
        }
        if (e.contains("open") && e["open"].is_array())
            for (const nlohmann::json &p : e["open"])
                if (p.is_string())
                    s.open.push_back(p.get<std::string>());

        if (e.contains("view")) {
            const std::string v = e.value("view", std::string());
            if (!shot_view_from_name(v, s.view)) {
                err = "shot manifest: unknown view \"" + v + "\" in \"" + s.name +
                      "\"";
                return false;
            }
        }
        if (e.contains("scene")) {
            const std::string k = e.value("scene", std::string());
            if (!shot_scene_from_name(k, s.scene)) {
                err = "shot manifest: unknown scene \"" + k + "\" in \"" + s.name +
                      "\"";
                return false;
            }
        }
        if (e.contains("layers") && e["layers"].is_array())
            for (const nlohmann::json &l : e["layers"])
                if (l.is_string())
                    s.layers.push_back(l.get<std::string>());

        if (e.contains("size") && e["size"].is_array() && e["size"].size() == 2 &&
            e["size"][0].is_number_integer() && e["size"][1].is_number_integer()) {
            s.width = e["size"][0].get<int>();
            s.height = e["size"][1].get<int>();
        }
        if (e.contains("warmup") && e["warmup"].is_number_integer())
            s.warmup = e["warmup"].get<int>();
        if (e.contains("hud") && e["hud"].is_boolean())
            s.hud = e["hud"].get<bool>();

        if (s.width <= 0 || s.height <= 0) {
            err = "shot manifest: non-positive size in \"" + s.name + "\"";
            return false;
        }
        if (s.warmup < 0) {
            err = "shot manifest: negative warmup in \"" + s.name + "\"";
            return false;
        }
        // Validate the layer ids NOW, against a throwaway SceneLayers, so a typo
        // fails the manifest rather than silently producing an image missing the
        // layer its author asked for.
        scene3d::SceneLayers probe;
        std::string lerr;
        if (!shot_apply_layers(s.layers, probe, lerr)) {
            err = "shot manifest: " + lerr + " in \"" + s.name + "\"";
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

} // namespace asmdesk
