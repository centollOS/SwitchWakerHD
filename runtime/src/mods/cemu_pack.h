#pragma once
#include "mod_json.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
namespace mods::cemu {
struct Preset {std::string name,category;std::map<std::string,std::string> variables;};
struct TextureRule {std::map<std::string,std::string> fields;};
struct ShaderSource {uint64_t base=0,aux=0;bool vertex=false;std::string source;};
struct Pack {
    std::string name,description,aspect_expression;
    std::map<std::string,std::string> defaults;
    std::vector<Preset> presets;
    std::vector<std::string> categories;
    std::vector<TextureRule> textures;
    std::vector<ShaderSource> shaders;
};
Pack parse(const std::filesystem::path& folder);
json::Value options(const Pack& pack);
void import_legacy(const std::filesystem::path& stage,const std::string& source_name);
struct Selection {std::string id;Pack pack;json::Value config;};
void validate(const std::vector<Selection>& selections);
void activate(const std::vector<Selection>& selections); // before guest execution only
void set_vulkan(bool available); // requested at startup, then actual backend after fallback
bool vulkan();
bool has_shaders();
bool legacy_pixel_uniforms(uint64_t base);
float aspect_ratio(); // known WWHD resolution data patches use the native projection adapter
// Absolute physical render-target sizes; guest sizes/formats are preserved.
bool texture_extent(uint32_t width,uint32_t height,uint32_t format,uint32_t depth,uint32_t tile,uint32_t& out_width,uint32_t& out_height);
std::string shader_source(uint64_t base,uint64_t aux,bool vertex);
void report_shader(uint64_t base,uint64_t aux,bool vertex,bool accepted,const std::string& reason);
std::string runtime_status(const std::string& id);
double expression(const std::string& text,const std::map<std::string,double>& variables);
}
