/* Native counterpart of the experimental ble_latency.py. HCI events only;
 * never receives/logs ACL input or keys. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ble_link.h"
#ifdef __linux__
#include <glib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BLE_INTERVAL_MIN 6
#define BLE_INTERVAL_MAX 12
#define BLE_INTERVAL_MAX_ATTEMPTS 3
#define BLE_INTERVAL_RETRY_DELAY_US G_USEC_PER_SEC
#define BLE_INTERVAL_RETRY_STATUS 0x23

struct link_state {
    bool used, encrypted;
    uint8_t role;
    uint16_t interval, latency;
    unsigned attempts;
    gint64 retry_at;
};
struct otg_ble_link {
    int hci,channel;uint16_t index,pending;
    bool ready,waiting,multi;gint64 deadline;
    struct link_state links[4096];
};
static uint16_t get16(const uint8_t *p){return (uint16_t)(p[0]|p[1]<<8);}
static void set16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static unsigned single_link(struct otg_ble_link *b,uint16_t *handle)
{
    unsigned count=0;
    for(unsigned i=0;i<G_N_ELEMENTS(b->links);i++)if(b->links[i].used){count++;*handle=(uint16_t)i;}
    return count;
}
static bool interval_is_target(const struct link_state *link)
{
    return link->interval>=BLE_INTERVAL_MIN && link->interval<=BLE_INTERVAL_MAX && !link->latency;
}
static void schedule_retry(struct link_state *link,const char *reason)
{
    if(link->attempts>=BLE_INTERVAL_MAX_ATTEMPTS) {
        fprintf(stderr,"WARN: BLE interval tuning exhausted after %u attempts (%s)\n",
                link->attempts,reason);
        return;
    }
    link->retry_at=g_get_monotonic_time()+BLE_INTERVAL_RETRY_DELAY_US;
    fprintf(stderr,"INFO: BLE interval retry scheduled in 1s after %s (%u/%u)\n",
            reason,link->attempts,BLE_INTERVAL_MAX_ATTEMPTS);
}
static bool retryable_status(uint8_t status)
{
    return status==BLE_INTERVAL_RETRY_STATUS;
}
static void publish(struct otg_ble_link *b,uint16_t handle)
{
    struct link_state *link=&b->links[handle];char text[64];
    int size=snprintf(text,sizeof(text),b->multi ? "interval_us=%u;handle=%u\n" : "interval_us=%u\n",
                      link->interval*1250u,handle);
    (void)send(b->channel,text,(size_t)size,MSG_DONTWAIT|MSG_NOSIGNAL);
    fprintf(stderr,"INFO: BLE actual interval=%.2fms latency=%u (controller event)\n",link->interval*1.25,link->latency);
}
static void link_event(struct otg_ble_link *b,const uint8_t *packet,size_t size)
{
    if(size<3 || packet[0]!=4 || packet[2]!=size-3)return;
    uint8_t event=packet[1];const uint8_t *p=packet+3;size-=3;
    if(event==5 && size==4 && !p[0]) {
        uint16_t handle=get16(p+1);
        if(handle>=4096 || !b->links[handle].used)return;
        memset(&b->links[handle],0,sizeof(b->links[handle]));b->ready=false;
        if(b->waiting && b->pending==handle)b->waiting=false;
    } else if((event==8 || event==0x59) && size>=4) {
        uint16_t handle=get16(p+1);
        if(handle<4096)b->links[handle].encrypted=!p[0] && p[3];
    } else if(event==15 && size==4 && get16(p+2)==0x2013 && p[0] && b->waiting) {
        uint16_t handle=b->pending;
        fprintf(stderr,"WARN: BLE interval request rejected: status=0x%02x\n",p[0]);
        b->waiting=false;
        if(handle<4096 && b->links[handle].used && retryable_status(p[0]))
            schedule_retry(&b->links[handle],"HCI status 0x23");
    } else if(event==0x3e && size) {
        if((p[0]==1 && size==19) || (p[0]==10 && size==31)) {
            uint16_t handle=get16(p+2);if(p[1] || handle>=4096)return;
            unsigned offset=p[0]==1 ? 12 : 24;
            b->links[handle]=(struct link_state){.used=true,.role=p[4],.interval=get16(p+offset),.latency=get16(p+offset+2)};
            b->ready=false;
        } else if(p[0]==3 && size==10) {
            uint16_t handle=get16(p+2);if(handle>=4096 || !b->links[handle].used)return;
            bool pending=b->waiting && b->pending==handle;
            if(p[1]) {
                fprintf(stderr,"WARN: BLE interval update failed: status=0x%02x\n",p[1]);
                if(pending && retryable_status(p[1]))
                    schedule_retry(&b->links[handle],"LE update status 0x23");
            } else {
                struct link_state *link=&b->links[handle];
                link->interval=get16(p+4);link->latency=get16(p+6);
                uint16_t only=0;if((b->multi || b->ready) &&
                   (b->multi || (single_link(b,&only)==1 && only==handle)))publish(b,handle);
                if(interval_is_target(link)) {
                    link->attempts=0;link->retry_at=0;
                } else {
                    schedule_retry(link,"observed non-target interval");
                }
            }
            if(pending)b->waiting=false;
        }
    }
}
static void negotiate(struct otg_ble_link *b,gint64 now)
{
    if(b->waiting && now>=b->deadline) {
        uint16_t handle=b->pending;
        fprintf(stderr,"WARN: BLE interval update not confirmed within 5s\n");
        b->waiting=false;
        if(handle<4096 && b->links[handle].used)
            schedule_retry(&b->links[handle],"update confirmation timeout");
    }
    uint16_t handle=0;
    if (b->waiting) return;
    if (!b->multi) {
        if(!b->ready || single_link(b,&handle)!=1)return;
        struct link_state *link=&b->links[handle];
        if(!link->encrypted || link->role!=1)return;
        if(interval_is_target(link)) {link->attempts=0;link->retry_at=0;return;}
        if(link->attempts>=BLE_INTERVAL_MAX_ATTEMPTS ||
           (link->retry_at && now<link->retry_at))return;
    } else {
        bool found=false;
        for (unsigned i=0;i<G_N_ELEMENTS(b->links);i++) {
            struct link_state *candidate=&b->links[i];
            if (candidate->used && candidate->encrypted && candidate->role==1 &&
                !interval_is_target(candidate) && candidate->attempts<BLE_INTERVAL_MAX_ATTEMPTS &&
                (!candidate->retry_at || now>=candidate->retry_at)) {
                handle=(uint16_t)i;found=true;break;
            }
        }
        if (!found) return;
    }
    struct link_state *link=&b->links[handle];
    publish(b,handle);
    link->attempts++;
    uint8_t cmd[18]={1,0x13,0x20,14};set16(cmd+4,handle);set16(cmd+6,BLE_INTERVAL_MIN);set16(cmd+8,BLE_INTERVAL_MAX);set16(cmd+12,500);
    if(send(b->hci,cmd,sizeof(cmd),MSG_DONTWAIT)!=(ssize_t)sizeof(cmd)) {
        fprintf(stderr,"WARN: cannot send BLE interval request\n");
        schedule_retry(link,"HCI command send failure");
        return;
    }
    b->waiting=true;b->pending=handle;b->deadline=now+5000000;
    fprintf(stderr,"INFO: BLE requesting interval 7.5–15ms; waiting for controller confirmation\n");
}
struct otg_ble_link *otg_ble_link_open(uint16_t index,int channel,bool multi)
{
    struct otg_ble_link *b=g_new0(struct otg_ble_link,1);b->channel=channel;b->index=index;b->multi=multi;
    b->hci=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,1);
    struct {sa_family_t family;uint16_t index,channel;} address={AF_BLUETOOTH,index,0};
    /* Match the native HCI_FILTER ABI, including its trailing padding. New
     * kernels reject the former hard-coded 14-byte (unpadded) length. */
    struct {uint32_t type_mask,event_mask[2];uint16_t opcode;} filter={
        .type_mask=1u<<4,
        .event_mask={(1u<<5)|(1u<<8)|(1u<<15)|(1u<<25),1u<<30},
        .opcode=0
    };
    const char *operation="socket";
    if(b->hci<0)goto fail;
    operation="bind";
    if(bind(b->hci,(struct sockaddr *)&address,sizeof(address)))goto fail;
    operation="HCI_FILTER";
    if(setsockopt(b->hci,0,2,&filter,sizeof(filter)))goto fail;
    return b;
fail:
    {
        int saved_errno=errno;
        fprintf(stderr,"WARN: BLE timing monitor %s failed on hci%u: %s (errno=%d)\n",
                operation,index,strerror(saved_errno),saved_errno);
        if(b->hci>=0)close(b->hci);
        g_free(b);return NULL;
    }
}
void otg_ble_link_poll(struct otg_ble_link *b)
{
    if(!b)return;
    uint8_t packet[260];ssize_t size;
    while((size=recv(b->hci,packet,sizeof(packet),MSG_DONTWAIT))>0)link_event(b,packet,(size_t)size);
    char ready[48],not_ready[48];
    snprintf(ready,sizeof(ready),"ready:hci%u\n",b->index);
    snprintf(not_ready,sizeof(not_ready),"not-ready:hci%u\n",b->index);
    while((size=recv(b->channel,packet,sizeof(packet)-1,MSG_DONTWAIT))>0) {
        packet[size]=0;
        if(!strcmp((char *)packet,ready))b->ready=true;
        else if(!strcmp((char *)packet,not_ready))b->ready=false;
    }
    negotiate(b,g_get_monotonic_time());
}
void otg_ble_link_close(struct otg_ble_link *b)
{
    if(!b)return;
    close(b->hci);g_free(b);
}
#endif
