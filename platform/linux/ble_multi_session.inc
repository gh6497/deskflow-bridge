/* Directed extension of the resident seqpacket protocol. The attach packet
 * reserves both identities before any descriptor or input can be accepted. */
struct directed_client {
    int fd;
    gint64 last_request;
    char screen[256];
    struct otg_ble_route *route;
    uint64_t generation;
    struct message descriptors[2];
    bool active;
};

struct directed_service {
    struct directed_client client[OTG_BLE_MAX_PHONES];
    bool ever_attached;
};

static unsigned multi_count(const struct directed_service *multi)
{
    unsigned count=0;
    for (size_t i=0;i<G_N_ELEMENTS(multi->client);i++)
        if (multi->client[i].fd>=0) ++count;
    return count;
}

static bool multi_reply(int fd,bool ok,uint8_t flags,bool idle)
{
    uint8_t status[]={flags,idle};
    return send_message(fd,REPLY,ok?0:1,status,sizeof(status));
}

static void multi_drop(struct service *s,struct directed_client *c,bool ack)
{
    int fd=c->fd;
    otg_ble_route_unbind(c->route);
    *c=(struct directed_client){.fd=-1};
    bool last=multi_count(s->directed)==0;
    if (last) {
        s->clean=otg_ble_hid_set_session(s->hid,false);
        s->shutdown=true;
    }
    if (ack) multi_reply(fd,s->clean,0,last && s->clean);
    close(fd);
}

static bool multi_register(struct service *s,struct directed_client *c,const struct message *m)
{
    if (c->active || m->size<6 || (m->id!=AOA_HID_ID_KEYBOARD && m->id!=AOA_HID_ID_MOUSE)) {
        fprintf(stderr,"WARN: directed BLE rejected report descriptor id=%u size=%u active=%d\n",
                m->id,m->size,c->active);
        return false;
    }
    c->descriptors[m->id==AOA_HID_ID_MOUSE]=*m;
    if (!c->descriptors[0].size || !c->descriptors[1].size) return true;
    /* A paired phone may have reconnected before RegisterApplication returns. */
    if (!c->route) otg_ble_route_enable_discovery(s->hid,true);
    for (unsigned i=0;i<2;i++) {
        const struct message *desc=&c->descriptors[i];
        if (!otg_ble_hid_ops()->register_report(s->hid,desc->id,desc->data,desc->size)) {
            fprintf(stderr,"ERROR: directed BLE GATT report registration failed id=%u\n",desc->id);
            return false;
        }
    }
    c->active=otg_ble_hid_set_session(s->hid,true);
    fprintf(stderr,"INFO: directed BLE client %s registered keyboard and pointer, session=%s\n",
            c->screen,c->active?"active":"failed");
    return c->active;
}

static void multi_tick(struct service *s)
{
    bool discovering=false;
    for (unsigned i=0;i<G_N_ELEMENTS(s->directed->client);i++) {
        struct directed_client *c=&s->directed->client[i];
        if (c->fd<0) continue;
        if (c->active && !c->route) c->route=otg_ble_route_claim_discovered(s->hid);
        struct message m;
        int n=receive_message(c->fd,&m);
        gint64 now=g_get_monotonic_time();
        if (n<0 || (!n && now-c->last_request>=5000000)) {multi_drop(s,c,false);continue;}
        if (!n) {
            discovering|=c->active && !c->route;
            continue;
        }
        c->last_request=now;
        if (m.operation==DETACH && !m.size && !m.id) {multi_drop(s,c,true);continue;}
        uint64_t generation=0;
        bool ready=c->route && otg_ble_route_ready(c->route,&generation) && c->active;
        /* Even a complete reconnect between two RPCs must be observed as a
         * disconnect. The false reply drops the bridge's old bounded FIFO. */
        if (generation!=c->generation) {ready=false;c->generation=generation;}
        if ((m.operation == GET_IDENTITY || m.operation == GET_IDENTITY_WITH_NAME) &&
            !m.size && !m.id && c->route) {
            /* The route was claimed only after verifying the paired LE link.
             * Its identity stays pinned if the phone drops during this RPC. */
            char *name = m.operation == GET_IDENTITY_WITH_NAME ? otg_ble_route_dup_peer_name(s->hid, c->route) : NULL;
            if (!reply_identity(c->fd, otg_ble_route_identity(c->route), name)) multi_drop(s,c,false);
            g_free(name);
            continue;
        }
        bool ok=false;
        switch (m.operation) {
        case REGISTER: ok=multi_register(s,c,&m);break;
        case POLL_SESSION:
        case POLL_DIRECTED_SESSION: ok=!m.size && !m.id;break;
        case REPORT:
            ok=c->active && m.size==8 && (m.id==AOA_HID_ID_KEYBOARD || m.id==AOA_HID_ID_MOUSE);
            if (ok && ready) {
                otg_ble_route_send(c->route,m.id,m.data);
                ready=otg_ble_route_ready(c->route,&c->generation);
            }
            break;
        }
        uint8_t flags = ready ? STATUS_READY : 0;
        if (m.operation==POLL_DIRECTED_SESSION && !ready && c->active &&
            otg_ble_route_waiting_for_subscriptions(c->route))
            flags |= STATUS_WAITING_FOR_SUBSCRIPTIONS;
        if (!multi_reply(c->fd,ok,flags,false) || !ok) multi_drop(s,c,false);
        discovering|=c->active && !c->route;
    }
    otg_ble_route_enable_discovery(s->hid,discovering);
}

static bool multi_request(struct service *s,int fd,bool owner,const struct message *m)
{
    struct directed_service *multi=s->directed;
    bool idle=!multi_count(multi) && s->clean;
    if (!m->size && !m->id && (m->operation==STATUS || m->operation==STATUS_WITH_LAYOUT)) {
        uint8_t flags=m->operation==STATUS_WITH_LAYOUT ? STATUS_DIRECTED | STATUS_BLE_ONLY | STATUS_DISCOVERY |
            (s->legacy_handles?STATUS_LEGACY_HANDLES:0) : 0;
        for (unsigned i=0;i<G_N_ELEMENTS(multi->client);i++) {
            uint64_t gen;
            if (multi->client[i].active && multi->client[i].route &&
                otg_ble_route_ready(multi->client[i].route,&gen)) flags|=STATUS_READY;
        }
        multi_reply(fd,true,flags,idle);
        return false;
    }
    if (owner && !m->size && !m->id && m->operation==STOP_SERVICE) {
        if (multi_reply(fd,idle,0,idle) && idle) s->shutdown=true;
        return false;
    }
    bool discovery=m->operation==ATTACH_DIRECTED_DISCOVERY;
    unsigned address_size=discovery?0:17;
    if (!owner || s->shutdown || !s->clean ||
        (!discovery && m->operation!=ATTACH_DIRECTED && m->operation!=ATTACH_DIRECTED_BLE_ONLY) ||
        m->id!=(unsigned)s->legacy_handles || m->size<=address_size || m->size>address_size+255) goto reject;
    char addr[18]={0},screen[256]={0};
    for (unsigned i=0;i<address_size;i++) {
        if (i%3==2 ? m->data[i]!=':' : !g_ascii_isxdigit(m->data[i])) goto reject;
        addr[i]=g_ascii_toupper(m->data[i]);
    }
    memcpy(screen,m->data+address_size,m->size-address_size);
    for (unsigned i=address_size;i<m->size;i++) if (m->data[i]<=0x20 || m->data[i]==127) goto reject;
    struct directed_client *slot=NULL;
    for (unsigned i=0;i<G_N_ELEMENTS(multi->client);i++) {
        struct directed_client *c=&multi->client[i];
        if (c->fd<0) slot=c;
        else if (!g_ascii_strcasecmp(c->screen,screen)) goto reject;
    }
    if (!slot) goto reject;
    struct otg_ble_route *route=discovery?NULL:otg_ble_route_bind(s->hid,addr);
    if (!discovery && !route) goto reject;
    *slot=(struct directed_client){.fd=fd,.route=route,.last_request=g_get_monotonic_time()};
    multi->ever_attached=true;
    g_strlcpy(slot->screen,screen,sizeof(slot->screen));
    if (!multi_reply(fd,true,STATUS_DISCOVERY | STATUS_IDENTITY_WITH_NAME,false)) multi_drop(s,slot,false);
    return true;
reject:
    multi_reply(fd,false,0,idle);
    return false;
}
