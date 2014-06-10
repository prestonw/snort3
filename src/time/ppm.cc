/****************************************************************************
 *
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2006-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 ****************************************************************************/

/*
**
**  ppm.c
**
**  Packet Performance Monitor
**
**  Author: Marc Norton <mnorton@sourcefire.com>
**  Date: 10/2006
**
**  Usage:
*
*   config ppm: max-pkt-time usecs
*   config ppm: max-rule-time usecs
*   config ppm: max-suspend-time secs
*   config ppm: threshold count
*   config ppm: suspend-expensive-rules
*   config ppm: fastpath-expensive-packets
*   config ppm: pkt-events  syslog|console
*   config ppm: rule-events alert|syslog|console
*   config ppm: debug-rules
*   config ppm: debug-pkts
*
*/

#include "ppm.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <time.h>

#include "ppm_module.h"
#include "snort.h"
#include "rules.h"
#include "treenodes.h"
#include "treenodes.h"
#include "parser.h"
#include "util.h"
#include "rules.h"
#include "treenodes.h"
#include "treenodes.h"
#include "fpcreate.h"
#include "event_wrapper.h"
#include "snort_types.h"
#include "detection/detect.h"
#include "detection/fpdetect.h"
#include "actions/actions.h"
#include "protocols/packet.h"

#ifdef PPM_MGR

#define PPM_BASE_SUSPEND_RULE_GID 1000
#define PPM_BASE_CLEAR_RULE_GID   2000

#define PPM_DEFAULT_MAX_PKT_TICKS    0
#define PPM_DEFAULT_MAX_RULE_TICKS   0
#define PPM_DEFAULT_MAX_SUSP_SECS   60
#define PPM_DEFAULT_RULE_THRESHOLD   5

PPM_TICKS ppm_tpu = 0; /* ticks per usec */

THREAD_LOCAL ppm_stats_t       ppm_stats;
THREAD_LOCAL ppm_pkt_timer_t   ppm_pkt_times[PPM_MAX_TIMERS];
THREAD_LOCAL ppm_pkt_timer_t  *ppm_pt = NULL;
THREAD_LOCAL unsigned int      ppm_pkt_index = 0;
THREAD_LOCAL ppm_rule_timer_t  ppm_rule_times[PPM_MAX_TIMERS];
THREAD_LOCAL ppm_rule_timer_t *ppm_rt = NULL;
THREAD_LOCAL unsigned int      ppm_rule_times_index = 0;
THREAD_LOCAL uint64_t          ppm_cur_time = 0;

/* temporary flags */
THREAD_LOCAL int ppm_abort_this_pkt = 0;
THREAD_LOCAL int ppm_suspend_this_rule = 0;

/* debug-pkts  data */
#define MAX_DP_NRULES 1000
typedef struct
{
    uint64_t   pkt;
    detection_option_tree_root_t * tree;
    PPM_TICKS ticks;

} ppm_rules_t;

/* suspended rules */
// FIXIT storing rule tree pointers is a bad idea
// - no accommodation for reload
// - really not used except to log
// - should be deleted
static THREAD_LOCAL ppm_rules_t ppm_rules[MAX_DP_NRULES];
static THREAD_LOCAL int ppm_n_rules;

/* cleared rules - re-enabled */
// FIXIT see above re storing rule tree pointers
static THREAD_LOCAL detection_option_tree_root_t * ppm_crules[MAX_DP_NRULES];
static THREAD_LOCAL int ppm_n_crules;

void ppm_init_rules(void)
{
    ppm_n_rules = 0;
    ppm_n_crules = 0;
}

void ppm_set_rule(detection_option_tree_root_t * root,PPM_TICKS ticks)
{
    if( ppm_n_rules < MAX_DP_NRULES )
    {
        ppm_rules[ppm_n_rules].tree=root;
        ppm_rules[ppm_n_rules].ticks=ticks;
        ppm_n_rules++;
    }
}

void ppm_clear_rule(detection_option_tree_root_t *root)
{
    if( ppm_n_crules < MAX_DP_NRULES )
    {
        ppm_crules[ppm_n_crules++]=root;
    }
}

/*
 * calc ticks per micro-secs in integer units
 */
static int ppm_calc_ticks(void)
{
    ppm_tpu = (PPM_TICKS)get_ticks_per_usec();

    if( ppm_tpu == 0 )
    {
        return -1;
    }

    return 0;
}
void ppm_print_cfg(ppm_cfg_t *ppm_cfg)
{
    if (ppm_cfg == NULL)
        return;

    if (!ppm_cfg->enabled)
        return ;

    if( ppm_cfg->max_pkt_ticks )
    {
        LogMessage("\n");
        LogMessage("Packet Performance Monitor Config:\n");
        LogMessage("  ticks per usec  : %lu ticks\n",(unsigned long)ppm_tpu);

        LogMessage("  max packet time : %lu usecs\n",(unsigned long)(ppm_cfg->max_pkt_ticks/ppm_tpu));
        LogMessage("  packet action   : ");
        if( ppm_cfg->pkt_action )
            LogMessage("fastpath-expensive-packets\n");
        else
            LogMessage("none\n");
        LogMessage("  packet logging  : ");
        if(ppm_cfg->pkt_log&PPM_LOG_ALERT) LogMessage("alert " );
        if(ppm_cfg->pkt_log&PPM_LOG_MESSAGE) LogMessage("log ");
        if(!ppm_cfg->pkt_log) LogMessage("none ");
        LogMessage("\n");
#ifdef DEBUG
        LogMessage("  debug-pkts      : %s\n",(ppm_cfg->debug_pkts)? "enabled":"disabled");
#endif
    }

    if( ppm_cfg->max_rule_ticks)
    {
        LogMessage("\n");
        LogMessage("Rule Performance Monitor Config:\n");
        LogMessage("  ticks per usec  : %lu ticks\n",(unsigned long)ppm_tpu);

        LogMessage("  max rule time   : %lu usecs\n",(unsigned long)(ppm_cfg->max_rule_ticks/ppm_tpu));
        LogMessage("  rule action     : ");
        if( ppm_cfg->rule_action )
        {
            LogMessage("suspend-expensive-rules\n");
            LogMessage("  rule threshold  : %u \n",(unsigned int)ppm_cfg->rule_threshold);
        }
        else
            LogMessage("none\n");

#ifdef PPM_TEST
        /* use usecs instead of ticks for rule suspension during pcap playback */
        LogMessage("  suspend timeout : %lu secs\n", (unsigned long)(ppm_cfg->max_suspend_ticks/((uint64_t)1000000)) );
#else
        LogMessage("  suspend timeout : %lu secs\n", (unsigned long)(ppm_cfg->max_suspend_ticks/((uint64_t)ppm_tpu*1000000)) );
#endif
        LogMessage("  rule logging    : ");
        if(ppm_cfg->rule_log&PPM_LOG_ALERT) LogMessage("alert " );
        if(ppm_cfg->rule_log&PPM_LOG_MESSAGE) LogMessage("log ");
        if(!ppm_cfg->rule_log) LogMessage("none ");
        LogMessage("\n");
#ifdef DEBUG
        /*LogMessage("  debug-rules     : %s\n",(ppm_cfg->debug_rules)?"enabled":"disabled"); unsupported */
#endif
    }
}

static
int print_rule( int, RuleTreeNode*, OptTreeNode * o )
{
    if( !o->enabled )
    {
        //if( o->sigInfo.generator==1 || o->sigInfo.generator==3 )
        LogMessage("   disabled gid=%u, sid=%u\n",o->sigInfo.generator,o->sigInfo.id);
    }

    return 0;
}

void ppm_print_summary(ppm_cfg_t *ppm_cfg)
{
    if (ppm_cfg == NULL)
        return;

    if (!ppm_cfg->enabled)
        return;

    LogMessage("===============================================================================\n");
    if(ppm_cfg->max_pkt_ticks)
    {
        LogMessage("Packet Performance Summary:\n");

        LogMessage("   max packet time       : %g usecs\n",
            ppm_ticks_to_usecs(ppm_cfg->max_pkt_ticks));

        LogMessage("   packet events         : %u\n",
            (unsigned int)ppm_stats.pkt_event_cnt);

        if( ppm_stats.tot_pkts )
            LogMessage("   avg pkt time          : %g usecs\n",
                ppm_ticks_to_usecs((PPM_TICKS)(ppm_stats.tot_pkt_time/
                    ppm_stats.tot_pkts)));
    }

    if(ppm_cfg->max_rule_ticks)
    {
        LogMessage("Rule Performance Summary:\n");

        LogMessage("   max rule time         : %lu usecs\n",
            (unsigned long)(ppm_cfg->max_rule_ticks/ppm_tpu));

        LogMessage("   rule events           : %u\n",
            (unsigned int)ppm_stats.rule_event_cnt);

        if( ppm_stats.tot_rules )
            LogMessage("   avg rule time         : %g usecs\n",
                ppm_ticks_to_usecs((PPM_TICKS)(ppm_stats.tot_rule_time/
                    ppm_stats.tot_rules)));

        if( ppm_stats.tot_nc_rules )
            LogMessage("   avg nc-rule time      : %g usecs\n",
                ppm_ticks_to_usecs((PPM_TICKS)(ppm_stats.tot_nc_rule_time/
                    ppm_stats.tot_nc_rules)));

        if( ppm_stats.tot_pcre_rules )
            LogMessage("   avg nc-pcre-rule time : %g usecs\n",
                ppm_ticks_to_usecs((PPM_TICKS)(ppm_stats.tot_pcre_rule_time/
                    ppm_stats.tot_pcre_rules)));

        fpWalkOtns( 0, print_rule );
    }
}

double ppm_ticks_to_usecs(PPM_TICKS ticks)
{
    if (ppm_tpu > 0)
        return (double)ticks / ppm_tpu;

    return 0.0;
}

/*
 *  Initialization
 */
void ppm_init(ppm_cfg_t *ppm_cfg)
{
    /* calc ticks per usec */
    if (ppm_calc_ticks() == -1)
        return;

    ppm_cfg->enabled = 1;

    ppm_cfg->max_pkt_ticks = PPM_DEFAULT_MAX_PKT_TICKS;
    ppm_cfg->max_rule_ticks = PPM_DEFAULT_MAX_RULE_TICKS;

    /* use usecs instead of ticks for rule suspension during pcap playback */
    ppm_cfg->max_suspend_ticks = (uint64_t)PPM_DEFAULT_MAX_SUSP_SECS * 1000000;
#ifndef PPM_TEST
    ppm_cfg->max_suspend_ticks *= ppm_tpu;
#endif
    ppm_cfg->rule_threshold = PPM_DEFAULT_RULE_THRESHOLD;
}

/*
 *  Logging functions - syslog and/or events
 */
#define PPM_FMT_FASTPATH "PPM: Pkt-Event Pkt[" STDi64 "] used=%g usecs, %u rules, %u nc-rules tested, packet fastpathed (%s:%d -> %s:%d).\n"
#define PPM_FMT_PACKET   "PPM: Pkt-Event Pkt[" STDi64 "] used=%g usecs, %u rules, %u nc-rules tested (%s:%d -> %s:%d).\n"

void ppm_pkt_log(ppm_cfg_t *ppm_cfg, Packet* p)
{
    int filterEvent = 0;
    if (!ppm_cfg->max_pkt_ticks)
        return;

    ppm_stats.pkt_event_cnt++;

    if (ppm_cfg->pkt_log & PPM_LOG_ALERT)
    {
        OptTreeNode* potn;

        /* make sure we have an otn already in our table for this event */
        // FIXIT should be able to remove this custom event processing
        potn = OtnLookup(snort_conf->otn_map, GID_PPM, PPM_EVENT_PACKET_ABORTED);

        if (potn == NULL)
            return;

        if ( IPH_IS_VALID(p) )
        {
            filterEvent = sfthreshold_test(
                        potn->sigInfo.generator,
                        potn->sigInfo.id,
                        GET_SRC_IP(p), GET_DST_IP(p),
                        p->pkth->ts.tv_sec);
        }
        else
        {
            snort_ip cleared;
            IP_CLEAR(cleared);

            filterEvent = sfthreshold_test(
                        potn->sigInfo.generator,
                        potn->sigInfo.id,
                        IP_ARG(cleared), IP_ARG(cleared),
                        p->pkth->ts.tv_sec);
        }

        if(filterEvent < 0)
            filterEvent = 0;
        else
            AlertAction(p, potn);
    }

    if (ppm_cfg->pkt_log & PPM_LOG_MESSAGE)
    {
        char src[INET6_ADDRSTRLEN];
        char dst[INET6_ADDRSTRLEN];

        sfip_t* addr = GET_SRC_IP(p);
        sfip_ntop(addr, src, sizeof(src));

        addr = GET_DST_IP(p);
        sfip_ntop(addr, dst, sizeof(dst));

        if (ppm_abort_this_pkt)
        {
            LogMessage(PPM_FMT_FASTPATH,
                       ppm_pt->pktcnt,
                       ppm_ticks_to_usecs((PPM_TICKS)ppm_pt->tot),
                       ppm_pt->rule_tests, ppm_pt->nc_rule_tests,
                       src, p->sp, dst, p->dp);
        }
        else
        {
            LogMessage(PPM_FMT_PACKET,
                       ppm_pt->pktcnt,
                       ppm_ticks_to_usecs((PPM_TICKS)ppm_pt->tot),
                       ppm_pt->rule_tests, ppm_pt->nc_rule_tests,
                       src, p->sp, dst, p->dp);
        }
    }
}

#define PPM_FMT_SUS_PKT   "PPM: Rule-Event Pkt[" STDi64 "] suspended (%s:%d -> %s:%d).\n"
#define PPM_FMT_SUSPENDED "PPM: Rule-Event Pkt[" STDi64 "] address=0x%p used=%g usecs suspended %s\n"
#define PPM_FMT_REENABLED "PPM: Rule-Event Pkt[" STDi64 "] address=0x%p re-enabled %s\n"

void ppm_rule_log(ppm_cfg_t *ppm_cfg, uint64_t pktcnt, Packet *p)
{
    detection_option_tree_root_t *proot;
    OptTreeNode *otn;
    char timestamp[TIMEBUF_SIZE];
    int filterEvent = 0;
    *timestamp = '\0';

    if (!ppm_cfg->max_rule_ticks)
        return ;

    if (ppm_n_crules)
    {
        if (ppm_cfg->rule_log & PPM_LOG_ALERT)
        {
            // FIXIT should be able to remove this custom event processing
            otn = GetOTN(GID_PPM, PPM_EVENT_RULE_TREE_ENABLED);

            if (otn != NULL)
            {
                if ( IPH_IS_VALID(p) )
                {
                    filterEvent = sfthreshold_test(
                                otn->sigInfo.generator,
                                otn->sigInfo.id,
                                GET_SRC_IP(p), GET_DST_IP(p),
                                p->pkth->ts.tv_sec);
                }
                else
                {
                    snort_ip cleared;
                    IP_CLEAR(cleared);

                    filterEvent = sfthreshold_test(
                                otn->sigInfo.generator,
                                otn->sigInfo.id,
                                IP_ARG(cleared), IP_ARG(cleared),
                                p->pkth->ts.tv_sec);
                }

                if(filterEvent < 0)
                    filterEvent = 0;
                else
                    AlertAction(p, otn);
            }
        }

        if (ppm_cfg->rule_log & PPM_LOG_MESSAGE)
        {
            int i;

            if(!*timestamp)
                ts_print((struct timeval*)&p->pkth->ts, timestamp);

            for (i=0; i< ppm_n_crules; i++)
            {
                proot = ppm_crules[i];

                LogMessage(PPM_FMT_REENABLED,
                           pktcnt, (void*)proot, timestamp);
            }
        }

        ppm_n_crules = 0;
    }

    if (ppm_n_rules)
    {
        if (ppm_cfg->rule_log & PPM_LOG_ALERT)
        {
            // FIXIT should be able to remove this custom event processing
            otn = GetOTN(GID_PPM, PPM_EVENT_RULE_TREE_DISABLED);

            if (otn != NULL)
            {
                // FIXIT why was this done specially?
                if ( IPH_IS_VALID(p) )
                {
                    filterEvent = sfthreshold_test(
                                otn->sigInfo.generator,
                                otn->sigInfo.id,
                                GET_SRC_IP(p), GET_DST_IP(p),
                                p->pkth->ts.tv_sec);
                }
                else
                {
                    snort_ip cleared;
                    IP_CLEAR(cleared);

                    filterEvent = sfthreshold_test(
                                otn->sigInfo.generator,
                                otn->sigInfo.id,
                                IP_ARG(cleared), IP_ARG(cleared),
                                p->pkth->ts.tv_sec);
                }

                if(filterEvent < 0)
                    filterEvent = 0;
                else
                    AlertAction(p, otn);
            }
        }

        if (ppm_cfg->rule_log & PPM_LOG_MESSAGE)
        {
            int i;
            char src[INET6_ADDRSTRLEN];
            char dst[INET6_ADDRSTRLEN];

            sfip_t* addr = GET_SRC_IP(p);
            sfip_ntop(addr, src, sizeof(src));

            addr = GET_DST_IP(p);
            sfip_ntop(addr, dst, sizeof(dst));

            LogMessage(PPM_FMT_SUS_PKT, pktcnt, src, p->sp, dst, p->dp);

            if(!*timestamp)
                ts_print((struct timeval*)&p->pkth->ts, timestamp);

            for (i=0; i< ppm_n_rules; i++)
            {
                proot = ppm_rules[i].tree;

                LogMessage(PPM_FMT_SUSPENDED,
                           pktcnt, (void*)proot,
                           ppm_ticks_to_usecs((PPM_TICKS)ppm_rules[i].ticks),
                           timestamp);
            }
        }

        ppm_n_rules = 0;
    }
}

void ppm_set_rule_event(ppm_cfg_t *ppm_cfg, detection_option_tree_root_t *root)
{
    if (!ppm_cfg->max_rule_ticks)
        return;

    ppm_stats.rule_event_cnt++;

    if (ppm_cfg->rule_log && ppm_rt)
        ppm_set_rule(root, ppm_rt->tot);
}

void ppm_clear_rule_event(ppm_cfg_t *ppm_cfg, detection_option_tree_root_t *root)
{
    if (!ppm_cfg->max_rule_ticks)
        return;

    ppm_stats.rule_event_cnt++;

    if (ppm_cfg->rule_log)
        ppm_clear_rule(root);
}


/*
 * Config functions
 */

void ppm_set_pkt_action(ppm_cfg_t *ppm_cfg, int flag)
{
    ppm_cfg->pkt_action = flag;
}

void ppm_set_pkt_log(ppm_cfg_t *ppm_cfg, int flag)
{
    ppm_cfg->pkt_log |= flag;
}

void ppm_set_rule_action(ppm_cfg_t *ppm_cfg, int flag)
{
    ppm_cfg->rule_action = flag;
}

void ppm_set_rule_log(ppm_cfg_t *ppm_cfg, int flag)
{
    ppm_cfg->rule_log |= flag;
}

void ppm_set_max_pkt_time(ppm_cfg_t *ppm_cfg, PPM_USECS usecs)
{
    ppm_cfg->max_pkt_ticks = usecs * ppm_tpu;
}

void ppm_set_max_rule_time(ppm_cfg_t *ppm_cfg, PPM_USECS usecs)
{
    ppm_cfg->max_rule_ticks = usecs * ppm_tpu;
}

void ppm_set_max_suspend_time(ppm_cfg_t *ppm_cfg, PPM_SECS secs)
{
    /* use usecs instead of ticks for rule suspension during pcap playback */
    ppm_cfg->max_suspend_ticks = (uint64_t)secs * 1000000;
#ifndef PPM_TEST
    ppm_cfg->max_suspend_ticks *= ppm_tpu;
#endif
}

void ppm_set_rule_threshold(ppm_cfg_t *ppm_cfg, unsigned int cnt)
{
    ppm_cfg->rule_threshold = cnt;
}

#ifdef DEBUG
void ppm_set_debug_rules(ppm_cfg_t *ppm_cfg, int flag)
{
    ppm_cfg->debug_rules = flag;
}

void ppm_set_debug_pkts(ppm_cfg_t *ppm_cfg, int flag)
{
    ppm_cfg->debug_pkts = flag;
}
#endif

#endif

