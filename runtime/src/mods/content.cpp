#include "content.h"
#include "mod_archive.h"
#include "mod_json.h"
#include <fstream>
#include <vector>
#include <sstream>
#include <atomic>
#include <stdexcept>
namespace mods::content {
namespace fs=std::filesystem;
namespace {
Files active;
std::atomic<bool> present{false};
std::string lower(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c+= 'a'-'A';return s;}
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
}
bool known_pack(const std::string& filename){
    auto name=lower(filename);
    for(auto allowed:{"permanent_3d.pack","permanent_2d_usenglish.pack","permanent_2d_usfrench.pack","permanent_2d_usspanish.pack","first_szs_permanent.pack","szs_permanent0.pack","szs_permanent1.pack","szs_permanent2.pack"})if(name==allowed)return true;
    return false;
}
void import_legacy(const fs::path& stage,const std::string& source_name){
    std::vector<fs::path> candidates,loose_packs;
    for(const auto& e:fs::recursive_directory_iterator(stage)){
        auto name=lower(e.path().filename().string());
        if(e.is_directory()){
            if(name=="content")candidates.push_back(e.path());
            require(name!="code"&&name!="meta"&&name!="aoc", "Only content replacements are supported; code, meta and DLC folders cannot be imported");
            if(name.size()==16&&name.starts_with("00050000"))require(name=="0005000010143500","This SDCafiine pack targets another game or region (requires WWHD USA)");
        }else{
            auto ext=lower(e.path().extension().string());
            if(ext==".pack")loose_packs.push_back(e.path());
            require(name!="patches.txt"&&!name.ends_with("_vs.txt")&&!name.ends_with("_ps.txt")&&ext!=".glsl"&&ext!=".rpx"&&ext!=".rpl", "This mod includes unsupported code patches or shaders; import the complete mod through a future adapter");
            if(name=="rules.txt"){
                require(e.file_size()<=1024*1024,"Oversized Cemu rules file");std::ifstream f(e.path());std::string text{std::istreambuf_iterator<char>(f),{}};auto rules=lower(text);
                // A file-only Cemu pack may have Definition metadata, but no patch/shader/texture rules.
                require(rules.find("[texture")==std::string::npos&&rules.find("[control")==std::string::npos&&rules.find("[preset")==std::string::npos,"Cemu texture/control/preset rules require an adapter; this import supports file-only packs");
                std::istringstream lines(rules);std::string line;
                while(std::getline(lines,line)){auto start=line.find_first_not_of(" \t\r");if(start!=std::string::npos&&line[start]=='['){auto end=line.find(']',start);require(end!=std::string::npos&&line.substr(start,end-start+1)=="[definition]","Only Definition metadata is supported in file-only Cemu packs");}}
                auto title=rules.find("titleids");if(title!=std::string::npos){auto end=rules.find('\n',title);auto line=rules.substr(title,end-title);require(line.find("0005000010143500")!=std::string::npos,"Cemu pack does not target WWHD USA");}
            }
        }
    }
    if(candidates.empty()&&!loose_packs.empty()){
        for(const auto& file:loose_packs)require(known_pack(file.filename().string()),"Unknown loose pack filename; use a content/ tree with its game-relative path");
        auto target=stage/"content"/"Common"/"Pack";fs::create_directories(target);
        for(const auto& file:loose_packs){auto dest=target/file.filename();require(!fs::exists(dest),"Duplicate loose pack filename");fs::rename(file,dest);}
        candidates.push_back(stage/"content");
    }
    require(candidates.size()==1,"Select a single mod pack folder containing exactly one content/ directory");
    // Stable deterministic ID by source name. An explicit manifest can supply a different ID/version.
    auto name=fs::path(source_name).stem().string();require(!name.empty(),"Missing mod name");if(name.size()>128)name.resize(128);
    auto id=lower(name);for(char& c:id)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))c='-';if(id.size()>55)id.resize(55);id="content."+id;
    json::Value m;m["format_version"]=1;m["id"]=id;m["name"]=name;m["version"]="1.0.0";m["game_id"]="wwhd-usa";m["kind"]="content";m["minimum_manager_version"]="1.1.0";
    m["content_dir"]=candidates.front().lexically_relative(stage).generic_string();
    m["description"]="Imported local content replacement. Requires restart. Model and archive compatibility must be checked in game.";
    std::ofstream out(stage/"manifest.json");out<<json::dump(m)<<'\n';out.close();require(bool(out),"Cannot write imported manifest");
}
Files index(const fs::path& directory){
    require(fs::is_directory(directory),"Content folder is missing");
    require(!fs::is_symlink(directory),"Content paths may not use symlinks");
    Files out;uint64_t total=0;size_t count=0;
    for(const auto& e:fs::recursive_directory_iterator(directory)){
        require(++count<=4096,"Too many content entries");require(!e.is_symlink(),"Content files may not use symlinks");
        if(e.is_directory())continue;
        require(e.is_regular_file(),"Unsupported content file type");require(!lower(e.path().filename().string()).starts_with(".deleted_"),"SDCafiine file hiding is not supported yet");auto n=e.file_size();total+=n;
        require(n<=128ull*1024*1024&&total<=512ull*1024*1024,"Content size limit exceeded");
        auto relative=e.path().lexically_relative(directory).generic_string();
        require(archive::relative_path(relative),"Invalid content path");
        require(out.emplace(lower(relative),e.path().string()).second,"Duplicate content path (case insensitive)");
    }
    require(!out.empty(),"Content mod contains no files");return out;
}
void activate(Files files){require(!present.load(),"Content overrides already activated");active=std::move(files);present.store(!active.empty(),std::memory_order_release);}
std::string replacement(const std::string& guest,const std::string& mode){
    if(!present.load(std::memory_order_acquire))return {};
    if(mode!="r"&&mode!="rb")return {}; // never redirect writers or update handles
    auto relative=guest;
    if(relative.starts_with("/vol/content/"))relative.erase(0,13);
    else if(relative.empty()||relative.front()=='/')return {};
    if(!archive::relative_path(relative))return {};
    auto it=active.find(lower(relative));return it==active.end()?std::string{}:it->second;
}
}
