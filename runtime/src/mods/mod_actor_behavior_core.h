// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace mod_actor::behavior {
inline constexpr uint32_t Version=1,MaxUserdata=256;
inline constexpr size_t MaxRegistrations=128,MaxBindings=128,MaxTickets=128;
enum class Event:uint32_t {Create,Execute,Draw,Talk,Hit};
enum class Creation:uint32_t {New,Restored};
struct Table {
    uint32_t size=32,version=Version,userdata_size=0;
    uint32_t create=0,execute=0,draw=0,talk=0,on_hit=0;
    uint32_t function(Event event)const {
        switch(event){case Event::Create:return create;case Event::Execute:return execute;
            case Event::Draw:return draw;case Event::Talk:return talk;case Event::Hit:return on_hit;}
        return 0;
    }
};
static_assert(sizeof(Table)==32);
struct Identity {uint32_t process_id=0,declaration_token=0;uint64_t epoch=0;};
struct Binding {
    uint32_t handle=0;std::string owner;Identity identity;Creation creation=Creation::New;uint32_t userdata_size=0;
    bool initialize_userdata()const{return creation==Creation::New;}
};
struct Ticket {uint32_t handle=0;uint64_t epoch=0,serial=0;};
struct Dispatch {Ticket ticket;Binding binding;Event event=Event::Execute;uint32_t function=0;};

// Single-threaded host adapter core. It never calls guest code, owns no guest
// memory, and exposes copies only. Keep this object alive across module/state
// reloads: its handle and ticket counters intentionally never rewind.
class Registry {
    using Key=std::pair<std::string,uint32_t>;
    struct Registration {Table table;bool quarantined=false;};
    struct Entry {Binding binding;bool busy=false;};
    struct Pending {Ticket ticket;Key key;};
    std::map<Key,Registration> registrations_;
    std::map<uint32_t,Entry> bindings_;
    std::map<uint64_t,Pending> pending_;
    uint64_t next_handle_=1,epoch_=1,next_serial_=1;
    bool epoch_exhausted_=false,serial_exhausted_=false,registering_=false;
    const Registration* registration(const Binding& binding)const {
        const auto it=registrations_.find({binding.owner,binding.identity.declaration_token});
        return it==registrations_.end()||it->second.quarantined?nullptr:&it->second;
    }
    static bool same(const Ticket& a,const Ticket& b){return a.handle==b.handle&&a.epoch==b.epoch&&a.serial==b.serial;}
public:
    // Non-default seeds exercise exhaustion without billions of allocations.
    explicit Registry(uint64_t next_handle=1,uint64_t epoch=1,uint64_t next_serial=1):next_handle_(next_handle),epoch_(epoch),next_serial_(next_serial){
        if(!next_handle_)next_handle_=uint64_t(std::numeric_limits<uint32_t>::max())+1;
        if(!epoch_)epoch_exhausted_=true;
        if(!next_serial_)serial_exhausted_=true;
    }
    Registry(const Registry&)=delete;Registry& operator=(const Registry&)=delete;
    Registry(Registry&&)=delete;Registry& operator=(Registry&&)=delete;
    uint64_t epoch()const{return epoch_;}
    size_t registrations()const{return registrations_.size();}
    size_t bindings()const{return bindings_.size();}
    size_t in_flight()const{return pending_.size();}

    template<class OwnedFunction>
    bool register_behavior(const std::string& owner,uint32_t token,const Table& table,OwnedFunction&& owned){
        if(registering_||owner.empty()||owner.size()>64||!token||!pending_.empty()||registrations_.size()>=MaxRegistrations||
            table.size!=sizeof(Table)||table.version!=Version||table.userdata_size>MaxUserdata||!table.create||!table.execute||
            registrations_.contains({owner,token}))return false;
        registering_=true;
        struct Guard{bool& active;~Guard(){active=false;}} guard{registering_};
        for(uint32_t fn:std::array<uint32_t,5>{table.create,table.execute,table.draw,table.talk,table.on_hit})
            if(fn&&!owned(owner,fn))return false;
        return registrations_.emplace(Key{owner,token},Registration{table,false}).second;
    }
    std::optional<Binding> issue(const std::string& owner,uint32_t token,uint32_t process_id,Creation reason){
        const auto it=registrations_.find({owner,token});
        if((reason!=Creation::New&&reason!=Creation::Restored)||!process_id||epoch_exhausted_||next_handle_>std::numeric_limits<uint32_t>::max()||bindings_.size()>=MaxBindings||
            it==registrations_.end()||it->second.quarantined)return {};
        for(const auto& [handle,entry]:bindings_)
            if(entry.binding.identity.process_id==process_id)return {}; // native process IDs are globally unique
        Binding binding{uint32_t(next_handle_++),owner,{process_id,token,epoch_},reason,it->second.table.userdata_size};
        bindings_.emplace(binding.handle,Entry{binding,false});return binding;
    }
    template<class NativeIdentity>
    std::optional<Binding> resolve(uint32_t handle,const std::string& owner,NativeIdentity&& native)const {
        const auto it=bindings_.find(handle);
        if(!handle||epoch_exhausted_||it==bindings_.end())return {};
        const auto binding=it->second.binding;
        if(binding.owner!=owner||binding.identity.epoch!=epoch_||!registration(binding)||!native(binding.identity))return {};
        const auto current=bindings_.find(handle);
        if(epoch_exhausted_||binding.identity.epoch!=epoch_||current==bindings_.end()||!registration(binding))return {};
        return binding;
    }
    template<class NativeIdentity,class OwnedFunction>
    std::optional<Dispatch> begin_callback(uint32_t handle,const std::string& owner,Event event,NativeIdentity&& native,OwnedFunction&& owned){
        if(pending_.size()>=MaxTickets||serial_exhausted_)return {};
        const auto binding=resolve(handle,owner,std::forward<NativeIdentity>(native));
        if(!binding)return {};
        const auto* reg=registration(*binding);const uint32_t fn=reg->table.function(event);
        if(bindings_.at(handle).busy||!fn||!owned(owner,fn))return {};
        const auto current=bindings_.find(handle);
        if(current==bindings_.end()||binding->identity.epoch!=epoch_||current->second.busy||!registration(*binding))return {};
        auto& entry=current->second;
        Ticket ticket{handle,epoch_,next_serial_};
        if(next_serial_==std::numeric_limits<uint64_t>::max())serial_exhausted_=true;else ++next_serial_;
        entry.busy=true;pending_.emplace(ticket.serial,Pending{ticket,{owner,binding->identity.declaration_token}});
        return Dispatch{ticket,*binding,event,fn};
    }
    bool finish_callback(const Ticket& ticket,bool success){
        const auto p=pending_.find(ticket.serial);
        if(p==pending_.end()||!same(p->second.ticket,ticket))return false;
        const Key key=p->second.key;pending_.erase(p);
        const auto it=bindings_.find(ticket.handle);
        const bool live=it!=bindings_.end()&&it->second.binding.identity.epoch==ticket.epoch&&ticket.epoch==epoch_;
        if(live)it->second.busy=false;
        if(!success){const auto reg=registrations_.find(key);if(reg!=registrations_.end())reg->second.quarantined=true;}
        return live;
    }
    bool invalidate(uint32_t handle){return bindings_.erase(handle)!=0;}
    // Pending tickets deliberately outlive bindings. Their eventual completion
    // releases the callback guard without reviving a deleted/restored actor.
    bool invalidate_all(){
        bindings_.clear();
        if(epoch_exhausted_)return false;
        if(epoch_==std::numeric_limits<uint64_t>::max()){epoch_exhausted_=true;return false;}
        ++epoch_;return true;
    }
    void invalidate_owner(const std::string& owner){
        for(auto it=bindings_.begin();it!=bindings_.end();)it=it->second.binding.owner==owner?bindings_.erase(it):std::next(it);
    }
    bool unregister_owner(const std::string& owner){
        for(const auto& [serial,p]:pending_)if(p.key.first==owner)return false;
        invalidate_owner(owner);
        for(auto it=registrations_.begin();it!=registrations_.end();)it=it->first.first==owner?registrations_.erase(it):std::next(it);
        return true;
    }
    bool quarantine_registration(const std::string& owner,uint32_t token){
        const auto it=registrations_.find({owner,token});if(it==registrations_.end())return false;
        it->second.quarantined=true;return true;
    }
    bool quarantined(const std::string& owner,uint32_t token)const {
        const auto it=registrations_.find({owner,token});return it!=registrations_.end()&&it->second.quarantined;
    }
};
}
