// SPDX-License-Identifier: MPL-2.0
#include "mods/mod_actor_behavior_core.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace mod_actor::behavior;
namespace {
Table table(){Table t;t.create=0x100;t.execute=0x104;t.draw=0x108;t.talk=0x10c;t.on_hit=0x110;t.userdata_size=16;return t;}
bool owned(const std::string& owner,uint32_t fn){return owner=="owner"&&fn>=0x100&&fn<=0x110&&fn%4==0;}
bool native(const Identity& id){return id.process_id>=100&&id.process_id<1000&&id.declaration_token==7;}
void registration_contract(){
 Registry r;auto t=table();
 for(unsigned mode=0;mode<7;++mode){auto bad=t;if(mode==0)bad.size=31;if(mode==1)bad.version=2;if(mode==2)bad.userdata_size=257;if(mode==3)bad.create=0;if(mode==4)bad.execute=0;if(mode==5)bad.draw=0x101;if(mode==6)bad.on_hit=0x200;assert(!r.register_behavior("owner",7,bad,owned));}
 assert(!r.register_behavior("foreign",7,t,owned));assert(!r.register_behavior("",7,t,owned));assert(!r.register_behavior("owner",0,t,owned));
 bool attempted=false;assert(r.register_behavior("owner",7,t,[&](const std::string& owner,uint32_t fn){attempted=true;assert(!r.register_behavior("owner",8,t,owned));return owned(owner,fn);}));assert(attempted);assert(r.registrations()==1);
 assert(!r.register_behavior("owner",7,t,owned));
 Table optional=t;optional.draw=optional.talk=optional.on_hit=0;assert(r.register_behavior("owner",8,optional,owned));
 Registry throwing;try{throwing.register_behavior("owner",7,t,[](const auto&,auto)->bool{throw std::runtime_error("adapter");});assert(false);}catch(const std::runtime_error&){}assert(throwing.register_behavior("owner",7,t,owned));
}
void identity_and_restore(){
 Registry r;assert(r.register_behavior("owner",7,table(),owned));auto a=r.issue("owner",7,100,Creation::New);assert(a&&a->handle==1&&a->initialize_userdata());assert(!r.issue("owner",7,100,Creation::New));
 assert(!r.resolve(a->handle,"foreign",native));assert(r.resolve(a->handle,"owner",native));assert(!r.resolve(a->handle,"owner",[](const Identity&){return false;}));
 std::array<unsigned char,256> guest;guest.fill(0xaa);if(a->initialize_userdata())std::fill_n(guest.begin(),a->userdata_size,0);assert(guest[0]==0&&guest[15]==0&&guest[16]==0xaa);guest[0]=0x42;
 const auto epoch=r.epoch();assert(r.invalidate_all());assert(r.epoch()==epoch+1);assert(!r.resolve(a->handle,"owner",native));
 auto restored=r.issue("owner",7,100,Creation::Restored);assert(restored&&restored->handle!=a->handle&&!restored->initialize_userdata());if(restored->initialize_userdata())std::fill_n(guest.begin(),restored->userdata_size,0);assert(guest[0]==0x42);
 assert(r.invalidate(restored->handle));auto recreated=r.issue("owner",7,100,Creation::New);assert(recreated&&recreated->handle>restored->handle&&recreated->initialize_userdata());
 assert(!r.resolve(recreated->handle,"owner",[](const Identity& i){return i.declaration_token==8;}));assert(r.unregister_owner("owner"));assert(!r.resolve(recreated->handle,"owner",native));assert(r.register_behavior("owner",7,table(),owned));auto reloaded=r.issue("owner",7,100,Creation::New);assert(reloaded&&reloaded->handle>recreated->handle);
}
void dispatch_guards(){
 Registry r;assert(r.register_behavior("owner",7,table(),owned));auto a=r.issue("owner",7,100,Creation::New);auto b=r.issue("owner",7,101,Creation::New);assert(a&&b);
 auto first=r.begin_callback(a->handle,"owner",Event::Create,native,owned);assert(first&&first->function==0x100&&first->binding.initialize_userdata());
 assert(!r.begin_callback(a->handle,"owner",Event::Execute,native,owned));assert(!r.register_behavior("owner",8,table(),owned));assert(!r.unregister_owner("owner"));
 auto second=r.begin_callback(b->handle,"owner",Event::Execute,native,owned);assert(second&&r.in_flight()==2);assert(r.finish_callback(first->ticket,true));assert(!r.finish_callback(first->ticket,true));
 auto third=r.begin_callback(a->handle,"owner",Event::Draw,native,owned);assert(third&&third->ticket.serial!=first->ticket.serial);assert(!r.finish_callback(first->ticket,false));assert(!r.quarantined("owner",7));assert(r.finish_callback(third->ticket,true));assert(r.finish_callback(second->ticket,true));
 assert(!r.begin_callback(a->handle,"owner",Event::Hit,native,[](const auto&,auto){return false;}));
 auto cancelled=r.begin_callback(a->handle,"owner",Event::Talk,native,owned);assert(cancelled);r.invalidate_all();assert(r.in_flight()==1);assert(!r.register_behavior("owner",8,table(),owned));assert(!r.finish_callback(cancelled->ticket,true));assert(r.in_flight()==0);assert(r.register_behavior("owner",8,table(),owned));
}
void quarantine_and_delete(){
 Registry r;assert(r.register_behavior("owner",7,table(),owned));assert(r.register_behavior("owner",8,table(),owned));auto a=r.issue("owner",7,100,Creation::New);auto b=r.issue("owner",7,101,Creation::New);auto separate=r.issue("owner",8,102,Creation::New);assert(a&&b&&separate);
 auto callback=r.begin_callback(a->handle,"owner",Event::Hit,native,owned);assert(callback);assert(r.finish_callback(callback->ticket,false));assert(r.quarantined("owner",7));assert(!r.resolve(a->handle,"owner",native)&&!r.resolve(b->handle,"owner",native));assert(!r.issue("owner",7,103,Creation::New));assert(r.resolve(separate->handle,"owner",[](const Identity& i){return i.process_id==102&&i.declaration_token==8;}));
 assert(r.unregister_owner("owner"));assert(r.register_behavior("owner",7,table(),owned));auto next=r.issue("owner",7,100,Creation::New);auto ticket=r.begin_callback(next->handle,"owner",Event::Execute,native,owned);assert(ticket);assert(r.invalidate(next->handle));assert(!r.finish_callback(ticket->ticket,true));assert(r.in_flight()==0);
}
void invalidation_during_adapter_checks(){
 Registry r;assert(r.register_behavior("owner",7,table(),owned));auto a=r.issue("owner",7,100,Creation::New);assert(a);
 assert(!r.resolve(a->handle,"owner",[&](const Identity&){r.invalidate(a->handle);return true;}));
 auto b=r.issue("owner",7,101,Creation::New);assert(b);assert(!r.begin_callback(b->handle,"owner",Event::Draw,native,[&](const auto&,auto){r.invalidate(b->handle);return true;}));
 auto c=r.issue("owner",7,102,Creation::New);assert(c);auto ticket=r.begin_callback(c->handle,"owner",Event::Execute,native,owned);assert(ticket);r.invalidate_owner("owner");assert(r.in_flight()==1);assert(!r.unregister_owner("owner"));assert(!r.finish_callback(ticket->ticket,true));assert(r.unregister_owner("owner"));
}
void counter_exhaustion(){
 Registry handles(std::numeric_limits<uint32_t>::max());assert(handles.register_behavior("owner",7,table(),owned));auto last=handles.issue("owner",7,100,Creation::New);assert(last&&last->handle==std::numeric_limits<uint32_t>::max());handles.invalidate(last->handle);assert(!handles.issue("owner",7,101,Creation::New));handles.invalidate_all();assert(!handles.issue("owner",7,101,Creation::New));
 Registry epochs(1,std::numeric_limits<uint64_t>::max());assert(epochs.register_behavior("owner",7,table(),owned));auto bound=epochs.issue("owner",7,100,Creation::New);assert(bound);assert(!epochs.invalidate_all());assert(!epochs.resolve(bound->handle,"owner",native)&&!epochs.issue("owner",7,101,Creation::New));
 Registry serials(1,1,std::numeric_limits<uint64_t>::max());assert(serials.register_behavior("owner",7,table(),owned));auto actor=serials.issue("owner",7,100,Creation::New);auto ticket=serials.begin_callback(actor->handle,"owner",Event::Execute,native,owned);assert(ticket&&ticket->ticket.serial==std::numeric_limits<uint64_t>::max());assert(serials.finish_callback(ticket->ticket,true));assert(!serials.begin_callback(actor->handle,"owner",Event::Execute,native,owned));
}
void bounded_capacity(){
 Registry r;for(uint32_t i=1;i<=128;++i)assert(r.register_behavior("owner",i,table(),owned));assert(!r.register_behavior("owner",129,table(),owned));
 for(uint32_t i=1;i<=128;++i)assert(r.issue("owner",i,100+i,Creation::New));assert(!r.issue("owner",1,500,Creation::New));
 std::vector<Ticket> tickets;for(uint32_t i=1;i<=128;++i){auto t=r.begin_callback(i,"owner",Event::Execute,[](const Identity&){return true;},owned);assert(t);tickets.push_back(t->ticket);}assert(r.in_flight()==128);
 r.invalidate_all();assert(r.in_flight()==128);for(const auto& ticket:tickets)assert(!r.finish_callback(ticket,true));assert(r.in_flight()==0);
}
}
int main(){registration_contract();identity_and_restore();dispatch_guards();quarantine_and_delete();invalidation_during_adapter_checks();counter_exhaustion();bounded_capacity();std::cout<<"mod actor behavior core: PASS\n";}
