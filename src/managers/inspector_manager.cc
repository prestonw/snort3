/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
// inspector_manager.cc author Russ Combs <rucombs@cisco.com>

#include "inspector_manager.h"

#include <assert.h>
#include <algorithm>
#include <list>
#include <mutex>

#include "module_manager.h"
#include "event_manager.h"
#include "ips_manager.h"
#include "framework/inspector.h"
#include "network_inspectors/network_inspectors.h"
#include "service_inspectors/service_inspectors.h"
#include "detection/detection_util.h"
#include "obfuscation.h"
#include "packet_io/active.h"
#include "ppm.h"
#include "snort.h"
#include "log/messages.h"

using namespace std;

//-------------------------------------------------------------------------
// list stuff
//-------------------------------------------------------------------------
// it will be possible to load new processor so's, even newer versions of
// previously loaded so's, on reload

// class vs instance is currently a little off.  class corresponds to config
// while instance corresponds to policy.  we need to keep "context" in class
// for now since that is needed to config things like post-config-init.
// this distinction should be more precise when policy foo is ripped out of
// the instances.

struct PHGlobal {
    const InspectApi& api;
    bool init;

    PHGlobal(const InspectApi& p) : api(p)
    { init = true; };

    static bool comp (const PHGlobal* a, const PHGlobal* b)
    { return ( a->api.priority < b->api.priority ); };
};

struct PHClass {
    const InspectApi& api;
    void* data;
    int initialized;  // FIXIT should go away now

    PHClass(const InspectApi& p) : api(p)
    {
        initialized = 0;
        // FIXIT this should be Module* and data
        data = nullptr;
    };
    ~PHClass()
    {
    }
    static bool comp (PHClass* a, PHClass* b)
    { return ( a->api.priority < b->api.priority ); };
};

struct PHInstance {
    PHClass& pp_class;
    Inspector* handler;
    bool old; // FIXIT temporary until all inspectors are modularized

    PHInstance(PHClass&);
    ~PHInstance() { };

    static bool comp (PHInstance* a, PHInstance* b)
    { return ( a->pp_class.api.priority < b->pp_class.api.priority ); };
};

PHInstance::PHInstance(PHClass& p) : pp_class(p)
{
    Module* mod = ModuleManager::get_module(p.api.base.name);
    handler = p.api.ctor(mod);
    old = !mod;
}

typedef list<PHGlobal*> PHGlobalList;
typedef list<PHClass*> PHClassList;
typedef list<PHInstance*> PHInstanceList;
typedef list<Inspector*> PHList;

static PHGlobalList s_handlers;
static PHList s_trash;

struct FrameworkConfig
{
    PHClassList ph_list;
};

struct PHVector
{
    PHInstance** vec;
    unsigned num;

    PHVector()
    { vec = nullptr; num = 0; };

    ~PHVector()
    { if ( vec ) delete[] vec; };

    void alloc(unsigned max)
    { vec = new PHInstance*[max]; };

    void add(PHInstance* p)
    { vec[num++] = p; };
};

struct FrameworkPolicy
{
    PHInstanceList ph_list;

    PHVector network;
    PHVector generic;
    PHVector service;

    void Vectorize()
    {
        network.alloc(ph_list.size());
        service.alloc(ph_list.size());
        generic.alloc(ph_list.size());

        for ( auto* p : ph_list )
            if ( p->handler->enabled() )
            {
                if ( p->pp_class.api.priority <= PRIORITY_TRANSPORT )
                    network.add(p);
                else if ( p->pp_class.api.priority < PRIORITY_APPLICATION )
                    generic.add(p);
                else
                    service.add(p);
            }
    }
};

//-------------------------------------------------------------------------
// global stuff
//-------------------------------------------------------------------------

void InspectorManager::add_plugin(const InspectApi* api)
{
    PHGlobal* g = new PHGlobal(*api);
    s_handlers.push_back(g);
}

void InspectorManager::dump_plugins()
{
    Dumper d("Inspectors");

    for ( const auto* p : s_handlers )
        d.dump(p->api.base.name, p->api.base.version);
}

void InspectorManager::release_plugins ()
{
    empty_trash();

    for ( auto* p : s_handlers )
    {
        if ( !p->init && p->api.term )
            p->api.term();
        delete p;
    }
}

void InspectorManager::empty_trash()
{
    while ( !s_trash.empty() )
    {
        auto* p = s_trash.front();

        if ( !p->is_inactive() )
            return;

#if 0
        // FIXIT add name to Inspector to enable proper call to dtor
        InspectApi* api = (InspectApi*)get_api(PT_INSPECTOR, p->get_name());

        if ( api )
            api->dtor(p);
#else
        delete p;
#endif
        s_trash.pop_front();
    }
}

//-------------------------------------------------------------------------
// policy stuff
//-------------------------------------------------------------------------

void InspectorManager::new_policy (InspectionPolicy* pi)
{
    pi->framework_policy = new FrameworkPolicy;
}

void InspectorManager::delete_policy (InspectionPolicy* pi)
{
    for ( auto* p : pi->framework_policy->ph_list )
    {
        s_trash.push_back(p->handler);
        delete p;
    }
    delete pi->framework_policy;
    pi->framework_policy = nullptr;
}

static PHInstance* GetInstance(
    PHClass* ppc, FrameworkPolicy* fp, const char* keyword)
{
    for ( auto* p : fp->ph_list )
        if ( !strncasecmp(p->pp_class.api.base.name, keyword, 
            strlen(p->pp_class.api.base.name)) )
            return p;

    PHInstance* p = new PHInstance(*ppc);

    if ( !p->handler )  // FIXIT is this even possible?
    {
        delete p;
        return NULL;
    }
    fp->ph_list.push_back(p);
    return p;
}

// FIXIT create a separate list for meta handlers?  is there really more than one?
void InspectorManager::dispatch_meta (FrameworkPolicy* fp, int type, const uint8_t* data)
{
    // FIXIT change to select instance by policy and pass that in
    for ( auto* p : fp->ph_list )
        p->handler->meta(type, data);
}

//-------------------------------------------------------------------------
// config stuff
//-------------------------------------------------------------------------

void InspectorManager::new_config (SnortConfig* sc)
{
    sc->framework_config = new FrameworkConfig;
}

void InspectorManager::delete_config (SnortConfig* sc)
{
    for ( auto* p : sc->framework_config->ph_list )
        delete p;

    delete sc->framework_config;
    sc->framework_config = nullptr;
}

// note that we now have multiple preproc configs saved by parser
// (s5-global, s5-tcp, ..., etc.) but just one ppapi.  that means
// we must call the config func multiple times but add only the 1st
// instance to the policy list.
static PHClass* GetClass(const char* keyword, FrameworkConfig* fc)
{
    for ( auto* p : fc->ph_list )
        if ( !strncasecmp(p->api.base.name, keyword, strlen(p->api.base.name)) )
            return p;

    for ( auto* p : s_handlers )
        if ( !strncasecmp(p->api.base.name, keyword, strlen(p->api.base.name)) )
        {
            if ( p->init )
            {
                if ( p->api.init )
                    p->api.init();
                p->init = false;
            }
            PHClass* ppc = new PHClass(p->api);
            fc->ph_list.push_back(ppc);
            return ppc;
        }
    return NULL;
}

void InspectorManager::dump_stats (SnortConfig* sc)
{
    for ( auto* p : sc->framework_config->ph_list )
        if ( p->initialized && p->api.stats )
            p->api.stats(p->data);
}

void InspectorManager::accumulate (SnortConfig* sc)
{
    static mutex stats_mutex;
    stats_mutex.lock();

    for ( auto* p : sc->framework_config->ph_list )
        if ( p->initialized && p->api.sum )
            p->api.sum(p->data);

    pc_sum();
    stats_mutex.unlock();
}

void InspectorManager::reset_stats (SnortConfig* sc)
{
    for ( auto* p : sc->framework_config->ph_list )
        if ( p->initialized && p->api.reset )
            p->api.reset(p->data);
}

int InspectorManager::check_config(SnortConfig* sc)
{
    InspectionPolicy* pi = get_inspection_policy();

    for ( auto* p : pi->framework_policy->ph_list )
    {
        if ( int rval = p->handler->verify(sc) )
            return rval;
    }
    return 0;
}

// this is per thread
void InspectorManager::post_config(SnortConfig* sc)
{
    InspectionPolicy* pi = get_inspection_policy();

    if ( !pi->framework_policy )
        return;

    for ( auto* p : pi->framework_policy->ph_list )
    {
        p->handler->setup(sc);
    }
}

void InspectorManager::thread_init(SnortConfig*, unsigned slot)
{
    EventManager::open_outputs();
    IpsManager::setup_options();

    Inspector::slot = slot;
    InspectionPolicy* pi = get_inspection_policy();

    if ( !pi->framework_policy )
        return;

    for ( auto* p : pi->framework_policy->ph_list )
        p->handler->init();
}

void InspectorManager::thread_term(SnortConfig* sc)
{
    shutdown(sc);

    InspectionPolicy* pi = get_inspection_policy();

    if ( !pi || !pi->framework_policy )
        return;

    for ( auto* p : pi->framework_policy->ph_list )
        p->handler->term();

    accumulate(sc);
    IpsManager::clear_options();
    EventManager::close_outputs();
}

// FIXIT this does 2 things due to the convolution of class and global data
// first it purges all preprocs - this operates on global data like session caches
// then it resets all instances
void InspectorManager::reset (SnortConfig* sc)
{
    for ( auto* p : sc->framework_config->ph_list )
    {
        if ( p->initialized && p->api.purge )
            p->api.purge(p->data);
    }
    InspectionPolicy* pi = get_inspection_policy();

    if ( !pi->framework_policy )
        return;

    for ( auto* p : pi->framework_policy->ph_list )
        p->handler->reset();
}

// this is the last chance to process data - interact with other modules
// after this preprocs are being freed and can't be used to process data
void InspectorManager::shutdown (SnortConfig* sc)
{
    Active_Suspend();

    for ( auto* p : sc->framework_config->ph_list )
    {
        if ( p->initialized && p->api.stop )
            p->api.stop(p->data);
    }
}

//-------------------------------------------------------------------------
// config stuff
//-------------------------------------------------------------------------

// new configuration
void InspectorManager::instantiate(
    const InspectApi* api, Module*, SnortConfig* sc)
{
    // FIXIT only configures Lua inspectors in base policy; must be 
    // revisited when bindings are implemented
    FrameworkConfig* fc = sc->framework_config;
    FrameworkPolicy* fp = sc->policy_map->inspection_policy[0]->framework_policy;

    // FIXIT should not need to lookup inspector etc
    // since given api and mod
    const char* keyword = api->base.name;

    PHClass* ppc = GetClass(keyword, fc);

    if ( !ppc )
        ParseError("Unknown inspector: '%s'.", keyword);

    else
    {
        PHInstance* ppi = GetInstance(ppc, fp, keyword);

        if ( !ppi )
            ParseError("Can't instantiate inspector: '%s'.", keyword);
#if 0
        if ( ppi )
        {
            ppi->handler->configure(sc, keyword, nullptr);
            ppc->initialized = 1;
        }
#endif
    }
}

// iterate over all policies in the snort config
// for each, iterate over all parser-stored preproc text configs
// for each, find the associated preproc config func and configure it
void InspectorManager::configure(SnortConfig *sc)
{
    Inspector::max_slots = sc->max_threads;
    s_handlers.sort(PHGlobal::comp);

    // FIXIT legacy configuration - to be deleted
    {
        FrameworkConfig* fc = sc->framework_config;
        FrameworkPolicy* fp = sc->policy_map->inspection_policy[0]->framework_policy;
        PreprocConfig* config = sc->policy_map->inspection_policy[0]->preproc_configs;

        for (; config != NULL; config = config->next)
        {
            if (config->configured)  // FIXIT should be deleted
                continue;

            push_parse_location(config->file_name, config->file_line);
            PHClass* ppc = GetClass(config->keyword, fc);

            if ( !ppc )
                ParseError("Unknown preprocessor: '%s'.", config->keyword);

            else
            {
                PHInstance* ppi = GetInstance(ppc, fp, config->keyword);

                if ( !ppi )
                    ParseError("Can't instantiate inspector: '%s'.", config->keyword);

                else
                {
                    ppi->handler->configure(sc, config->keyword, config->opts);
                    config->configured = 1;
                }
            }
            pop_parse_location();
        }
    }

    {
        FrameworkConfig* fc = sc->framework_config;
        FrameworkPolicy* fp = sc->policy_map->inspection_policy[0]->framework_policy;
        PreprocConfig* config = sc->policy_map->inspection_policy[0]->preproc_configs;

        for ( auto* p : fp->ph_list )
        {
            if ( !p->old )
                p->handler->configure(sc, nullptr, nullptr);
        }

        fp->ph_list.sort(PHInstance::comp);
        fp->Vectorize();

        /* Set all configured preprocessors to intialized */
        for (; config != NULL; config = config->next)
        {
            if (config->configured)
            {
                // FIXIT should become just iteration over pp class list
                PHClass* ppc = GetClass(config->keyword, fc);

                if ( ppc )
                    ppc->initialized = 1;
            }
        }
    }
}

void InspectorManager::print_config(SnortConfig *sc)
{
    InspectionPolicy* pi = get_inspection_policy();

    if ( !pi->framework_policy )
        return;

    for ( auto* p : pi->framework_policy->ph_list )
        p->handler->show(sc);
}

//-------------------------------------------------------------------------
// packet handling
//-------------------------------------------------------------------------

static inline void execute(
    Packet* p, PHInstance** prep, unsigned num)
{
    for ( unsigned i = 0; i < num; ++i, ++prep )
    {
        if ( p->packet_flags & PKT_PASS_RULE )
            break;

        PHClass& ppc = (*prep)->pp_class;

        // FIXIT these checks can eventually be optimized
	    // but they are required to ensure that session and app
	    // handlers aren't called w/o a session pointer
        if ( !p->flow && (ppc.api.priority >= PRIORITY_SESSION) )
            break;

        if ( (p->proto_bits & ppc.api.proto_bits) )
            (*prep)->handler->eval(p);
    }
}

// FIXIT something here threw total counts off a little :(
void InspectorManager::execute (Packet* p)
{
    FrameworkPolicy* fp = get_inspection_policy()->framework_policy;
    assert(fp);

    ::execute(p, fp->network.vec, fp->network.num);
    ::execute(p, fp->generic.vec, fp->generic.num);

    if ( p->dsize )
        ::execute(p, fp->service.vec, fp->service.num);
    else
        DisableDetect(p);
}
