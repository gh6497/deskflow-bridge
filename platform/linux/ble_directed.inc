/* BlueZ local GATT AcquireNotify transport. Included by ble_hid.c to share
 * its private ObjectManager, HCI snapshot and application lifetime. */
struct otg_ble_route {
    char address[18];
    char *path;
    uint16_t handle;
    bool bound, candidate, paired, ready, suspended;
    const char *wait_reason;
    uint64_t generation;
    int fd[2];
    uint8_t value[2][8], leds;
    bool motion_pending;
    uint8_t motion[8];
    uint64_t last_pointer_ns;
};

static void directed_close_fd(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void directed_reset(struct otg_ble_route *r)
{
    if (r->ready) ++r->generation;
    r->ready = false;
    memset(r->value, 0, sizeof(r->value));
    r->motion_pending=false;
    r->last_pointer_ns=0;
}

static void directed_free(struct otg_ble_route *r)
{
    if (!r) return;
    directed_close_fd(&r->fd[0]);
    directed_close_fd(&r->fd[1]);
    g_free(r->path);
    g_free(r);
}

static bool directed_write(struct otg_ble_route *r, unsigned n, const uint8_t data[8])
{
    if (send(r->fd[n], data, 8, MSG_DONTWAIT | MSG_NOSIGNAL) != 8) {
        directed_close_fd(&r->fd[n]);
        unsigned other=1-n;
        const uint8_t neutral[8]={0};
        if (r->fd[other]>=0 && send(r->fd[other],neutral,8,MSG_DONTWAIT|MSG_NOSIGNAL)!=8)
            directed_close_fd(&r->fd[other]);
        directed_reset(r);
        return false;
    }
    memcpy(r->value[n], data, 8);
    return true;
}

static void directed_release(struct otg_ble_route *r)
{
    const uint8_t neutral[8] = {0};
    if (r->ready) {
        for (unsigned n=0;n<2;n++)
            if (r->fd[n]>=0) directed_write(r,n,neutral);
    }
    directed_reset(r);
}

static struct otg_ble_route *directed_route(struct otg_ble_hid *b, const char *address,
                                            const char *discovery_path, uint16_t handle)
{
    if (!b->directed_owner || !address || strlen(address)!=17) return NULL;
    struct otg_ble_route **available=NULL;
    struct otg_ble_route *resolving=NULL;
    for (unsigned i=0;i<G_N_ELEMENTS(b->routes);i++) {
        struct otg_ble_route *r=b->routes[i];
        if (r && !g_ascii_strcasecmp(address,r->address)) return r;
        if (discovery_path && r && r->candidate && !r->paired && r->handle==handle &&
            !g_strcmp0(r->path,discovery_path)) resolving=r;
        if (!r) available=&b->routes[i];
        else if (!available && !r->bound && !r->path) available=&b->routes[i];
    }
    if (resolving) {
        /* BlueZ can publish the identity address after the first CCC write.
         * Keep the FDs only for the same still-live object and HCI handle. */
        g_strlcpy(resolving->address,address,sizeof(resolving->address));
        return resolving;
    }
    if (!available) return NULL;
    directed_free(*available);
    struct otg_ble_route *r=*available=g_new0(struct otg_ble_route,1);
    r->fd[0]=r->fd[1]=-1;
    r->candidate=discovery_path!=NULL;
    g_strlcpy(r->address,address,sizeof(r->address));
    return r;
}

struct otg_ble_route *otg_ble_route_bind(struct otg_ble_hid *b, const char *address)
{
    struct otg_ble_route *r=directed_route(b,address,NULL,0);
    if (!r || r->bound) return NULL;
    r->bound=true;
    r->candidate=false;
    return r;
}

void otg_ble_route_enable_discovery(struct otg_ble_hid *b, bool enabled)
{
    b->discover_routes=enabled;
}

struct otg_ble_route *otg_ble_route_claim_discovered(struct otg_ble_hid *b)
{
    for (unsigned i=0;i<G_N_ELEMENTS(b->routes);i++) {
        struct otg_ble_route *r=b->routes[i];
        if (r && r->candidate && r->paired && r->fd[0]>=0 && r->fd[1]>=0)
            return otg_ble_route_bind(b,r->address);
    }
    return NULL;
}

const char *otg_ble_route_identity(const struct otg_ble_route *r)
{
    return r->address;
}

void otg_ble_route_unbind(struct otg_ble_route *r)
{
    if (!r) return;
    directed_release(r);
    r->bound=false;
}

bool otg_ble_route_ready(const struct otg_ble_route *r, uint64_t *generation)
{
    *generation=r->generation;
    return r->ready;
}

void otg_ble_route_send(struct otg_ble_route *r, uint16_t id, const uint8_t data[8])
{
    if (!r->ready || (id!=AOA_HID_ID_KEYBOARD && id!=AOA_HID_ID_MOUSE)) return;
    bool pointer=id==AOA_HID_ID_MOUSE;
    bool motion=pointer && !memcmp(data,r->value[1],2) && !data[6] && !data[7];
    uint64_t now=monotonic_ns();
    if (motion && now-r->last_pointer_ns<BLE_POINTER_INTERVAL_NS) {
        memcpy(r->motion,data,8);
        r->motion_pending=true;
        return;
    }
    if (r->motion_pending && !motion && !directed_write(r,1,r->motion)) return;
    r->motion_pending=false;
    if (pointer) r->last_pointer_ns=now;
    directed_write(r,pointer,data);
}

/* The online Device1 itself must be paired. Both the object and the HCI
 * peripheral link must resolve uniquely, including after a private-address
 * rotation. An offline bond cannot lend identity to a new connection. */
static void directed_refresh(struct otg_ble_hid *b, struct otg_ble_route *r,
                             GList *objects, const struct link_snapshot *links)
{
    unsigned matches=0, identities=0;
    bool bonded=false;
    const char *path=NULL;
    uint16_t handle=0;
    for (GList *it=objects;it;it=it->next) {
        GDBusObject *obj=it->data;
        const char *p=g_dbus_object_get_object_path(obj);
        if (!g_str_has_prefix(p,b->adapter_path) || p[strlen(b->adapter_path)]!='/') continue;
        GDBusInterface *iface=g_dbus_object_get_interface(obj,"org.bluez.Device1");
        if (!iface) continue;
        GDBusProxy *dev=G_DBUS_PROXY(iface);
        char *addr=device_address(dev);
        GVariant *paired=g_dbus_proxy_get_cached_property(dev,"Paired");
        GVariant *online=g_dbus_proxy_get_cached_property(dev,"Connected");
        if (addr && !g_ascii_strcasecmp(addr,r->address) && online && g_variant_get_boolean(online)) {
            ++identities;
            for (unsigned n=0;n<links->le_count;n++) {
                const struct le_link *le=&links->entries[n];
                if (le->peripheral && matches_address(le->address,addr,p)) {
                    matches++; path=p; handle=le->handle;
                    bonded=paired && g_variant_get_boolean(paired);
                }
            }
        }
        g_clear_pointer(&online,g_variant_unref);
        g_clear_pointer(&paired,g_variant_unref);
        g_free(addr);
        g_object_unref(iface);
    }
    if (identities!=1 || matches!=1) path=NULL;
    if (g_strcmp0(path,r->path) || (path && handle!=r->handle) || (r->paired && !bonded)) {
        directed_close_fd(&r->fd[0]);
        directed_close_fd(&r->fd[1]);
        directed_reset(r);
        r->leds=0; r->suspended=false;
        g_free(r->path); r->path=g_strdup(path); r->handle=handle;
    }
    r->paired=path && bonded;
    for (unsigned n=0;n<2;n++) {
        struct pollfd fd={r->fd[n],0,0};
        if (fd.fd>=0 && poll(&fd,1,0)>0 && fd.revents) {
            directed_close_fd(&r->fd[n]);
            directed_release(r);
        }
    }
    const char *reason=!r->bound || !b->session_enabled ? "idle" :
        !path ? "waiting for a unique LE peripheral link and matching online identity" :
        !r->paired ? "waiting for this LE device to finish pairing" :
        r->fd[0]<0 || r->fd[1]<0 ? "waiting for fresh keyboard and pointer subscriptions" :
        r->suspended ? "HID suspended" : NULL;
    if (g_strcmp0(reason,r->wait_reason)) {
        fprintf(stderr,"INFO: BLE %s: %s\n",r->address,reason?reason:"ready");
        r->wait_reason=reason;
    }
    bool ready=!reason;
    if (ready && !r->ready) {
        const uint8_t neutral[8]={0};
        r->ready=directed_write(r,0,neutral) && directed_write(r,1,neutral);
    } else if (!ready && r->ready) directed_release(r);
    if (r->ready && r->motion_pending && monotonic_ns()-r->last_pointer_ns>=BLE_POINTER_INTERVAL_NS) {
        r->motion_pending=false;
        r->last_pointer_ns=monotonic_ns();
        directed_write(r,1,r->motion);
    }
}

static void directed_observe_advertisement(struct otg_ble_hid *b, const struct link_snapshot *links)
{
    unsigned peripherals = 0;
    uint16_t handles[G_N_ELEMENTS(links->entries)];
    for (unsigned i = 0; i < links->le_count; ++i) {
        if (links->entries[i].peripheral) {
            const uint16_t handle = links->entries[i].handle;
            unsigned j = peripherals++;
            for (; j && handles[j-1] > handle; --j) handles[j] = handles[j-1];
            handles[j] = handle;
        }
    }
    size_t size = peripherals * sizeof(handles[0]);
    if (peripherals == b->adv_peripherals && !memcmp(handles,b->adv_handles,size)) return;
    b->adv_peripherals = peripherals;
    memcpy(b->adv_handles,handles,size);
    if (!b->session_enabled) return;

    /* A peripheral connection can consume the controller's advertisement.
     * BlueZ can still list that instance as registered. Re-arm once per
     * topology change while there is room for another phone. */
    b->adv_rearm = peripherals < G_N_ELEMENTS(b->routes) || b->adv_registered;
    b->adv_attempts = BLE_ADV_ATTEMPTS;
    b->adv_next_ns = monotonic_ns() + BLE_ADV_REARM_DELAY_NS;
}

static void directed_discover(struct otg_ble_hid *b, GList *objects, const struct link_snapshot *links)
{
    for (GList *it=objects;it;it=it->next) {
        GDBusObject *obj=it->data;
        const char *path=g_dbus_object_get_object_path(obj);
        if (!g_str_has_prefix(path,b->adapter_path) || path[strlen(b->adapter_path)]!='/') continue;
        GDBusInterface *iface=g_dbus_object_get_interface(obj,"org.bluez.Device1");
        if (!iface) continue;
        GDBusProxy *dev=G_DBUS_PROXY(iface);
        GVariant *online=g_dbus_proxy_get_cached_property(dev,"Connected");
        char *address=online && g_variant_get_boolean(online) ? device_address(dev) : NULL;
        for (unsigned n=0;address && n<links->le_count;n++) {
            const struct le_link *le=&links->entries[n];
            if (!le->peripheral || !matches_address(le->address,address,path)) continue;
            directed_route(b,address,path,le->handle);
            break;
        }
        g_free(address);
        g_clear_pointer(&online,g_variant_unref);
        g_object_unref(iface);
    }
}

static bool directed_poll(struct otg_ble_hid *b)
{
    char *owner=g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(b->devices));
    bool current=!g_strcmp0(owner,b->directed_owner);
    g_free(owner);
    struct link_snapshot links={0};
    if (current && b->read_links && b->read_links(b,&links))
        directed_observe_advertisement(b,&links);
    else links.le_count=0;
    GList *objects=g_dbus_object_manager_get_objects(b->devices);
    if (b->discover_routes && b->session_enabled && links.le_count)
        directed_discover(b,objects,&links);
    for (unsigned i=0;i<G_N_ELEMENTS(b->routes);i++)
        if (b->routes[i]) directed_refresh(b,b->routes[i],objects,&links);
    g_list_free_full(objects,g_object_unref);
    return current;
}

static bool directed_acquired(const struct otg_ble_hid *b,unsigned report)
{
    for (unsigned i=0;i<G_N_ELEMENTS(b->routes);i++)
        if (b->routes[i] && b->routes[i]->fd[report]>=0) return true;
    return false;
}

static bool directed_method(struct ble_object *o,const char *name,GVariant *args,
                            GDBusMethodInvocation *inv)
{
    if (o->kind!=BLE_REPORT && o->kind!=BLE_LEDS && o->kind!=BLE_CONTROL) return false;
    if (!strcmp(name,"StartNotify") || !strcmp(name,"StopNotify")) {
        g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.NotPermitted","Directed FD required");
        return true;
    }
    bool acquire=!strcmp(name,"AcquireNotify"), write=!strcmp(name,"WriteValue");
    if (!acquire && !write && strcmp(name,"ReadValue")) return false;
    GVariant *options=g_variant_get_child_value(args,write?1:0);
    const char *path=NULL,*link=NULL;
    struct otg_ble_route *r=NULL;
    guint16 mtu=23,offset=0;
    g_variant_lookup(options,"mtu","q",&mtu);
    g_variant_lookup(options,"offset","q",&offset);
    if (g_variant_lookup(options,"device","&o",&path) &&
        g_variant_lookup(options,"link","&s",&link) && !strcmp(link,"LE") && directed_poll(o->b)) {
        for (unsigned i=0;i<G_N_ELEMENTS(o->b->routes);i++) {
            struct otg_ble_route *p=o->b->routes[i];
            if (p && !g_strcmp0(p->path,path)) r=p;
        }
    }
    /* During first pairing the CCC write may precede Device1.Paired. Retain
     * the connection-bound FD, but expose no input, read or write until the
     * same online object and LE handle are verified as paired. */
    if (!r || (!acquire && !r->paired)) {
        g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Failed","No verified LE route");
    } else if (acquire && o->kind==BLE_REPORT && mtu>=23 && r->fd[o->report]<0) {
        int pair[2];
        if (!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair)) {
            GUnixFDList *fds=g_unix_fd_list_new_from_array(&pair[1],1);
            r->fd[o->report]=pair[0];
            g_dbus_method_invocation_return_value_with_unix_fd_list(inv,g_variant_new("(hq)",0,mtu),fds);
            g_object_unref(fds);
        } else g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Failed","socketpair failed");
    } else if (!acquire && !write && o->kind!=BLE_CONTROL) {
        const uint8_t *value=o->kind==BLE_LEDS ? &r->leds : r->value[o->report];
        size_t size=o->kind==BLE_LEDS ? 1 : 8;
        if (offset<=size) g_dbus_method_invocation_return_value(inv,g_variant_new("(@ay)",bytes(value+offset,size-offset)));
        else g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.InvalidOffset","offset exceeds value");
    } else if (write && o->kind!=BLE_REPORT) {
        GVariant *payload=g_variant_get_child_value(args,0);
        gsize len;
        const uint8_t *v=g_variant_get_fixed_array(payload,&len,1);
        if (len==1 && !offset && (o->kind==BLE_LEDS || *v<=1)) {
            if (o->kind==BLE_LEDS) r->leds=*v;
            else { if (!*v) directed_release(r); r->suspended=!*v; }
            g_dbus_method_invocation_return_value(inv,NULL);
        } else g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.InvalidArguments","Invalid output report");
        g_variant_unref(payload);
    } else g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Failed","Unavailable directed report");
    g_variant_unref(options);
    return true;
}
