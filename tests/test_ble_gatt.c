/* Real D-Bus dispatch on a private bus, not system Bluetooth.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../platform/linux/ble_hid.c"

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
struct mock_bluez {
    const char *address;
    GMainContext *context;
    GMainLoop *loop;
    int ready,registered,unregistered,advertised,stopped,reject_stop;
    char alias[64];
    GDBusConnection *bus;
    int connected_mask, unpaired_mask, private_path;
};
static const char *manager_xml=
"<node><interface name='org.freedesktop.DBus.Properties'>"
"<method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
"<method name='Set'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/></method></interface>"
"<interface name='org.bluez.GattManager1'>"
"<method name='RegisterApplication'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='UnregisterApplication'><arg type='o' direction='in'/></method></interface>"
"<interface name='org.bluez.LEAdvertisingManager1'>"
"<method name='RegisterAdvertisement'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='UnregisterAdvertisement'><arg type='o' direction='in'/></method></interface>"
"<interface name='org.freedesktop.DBus.ObjectManager'><method name='GetManagedObjects'>"
"<arg type='a{oa{sa{sv}}}' direction='out'/></method></interface></node>";
static void manager_method(GDBusConnection *bus,const char *sender,const char *path,
    const char *interface,const char *name,GVariant *args,GDBusMethodInvocation *inv,void *user)
{
    (void)bus;(void)sender;(void)path;(void)interface;
    struct mock_bluez *mock=user;
    if(!strcmp(name,"GetManagedObjects")) {
        GVariantBuilder objects;g_variant_builder_init(&objects,G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        bool privacy=g_atomic_int_get(&mock->private_path)!=0;
        const char *paths[]={privacy ? "/org/bluez/hci0/dev_52_00_00_00_00_01" :
            "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF","/org/bluez/hci0/dev_11_22_33_44_55_66",
            "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", "/org/bluez/hci0/dev_52_00_00_00_00_02"};
        const int unpaired=g_atomic_int_get(&mock->unpaired_mask);
        const char *addresses[]={"AA:BB:CC:DD:EE:FF","11:22:33:44:55:66","AA:BB:CC:DD:EE:FF",
            (unpaired&8)?"52:00:00:00:00:02":"22:33:44:55:66:77"};
        for(unsigned i=0;i<G_N_ELEMENTS(paths);i++) {
            if (i==2 && !privacy) continue;
            GVariantBuilder interfaces,properties;
            g_variant_builder_init(&interfaces,G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_init(&properties,G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&properties,"{sv}","Address",g_variant_new_string(addresses[i]));
            g_variant_builder_add(&properties,"{sv}","Connected",g_variant_new_boolean(!!(g_atomic_int_get(&mock->connected_mask)&(1<<i))));
            g_variant_builder_add(&properties,"{sv}","Paired",g_variant_new_boolean(!(unpaired&(1<<i))));
            g_variant_builder_add(&interfaces,"{s@a{sv}}","org.bluez.Device1",g_variant_builder_end(&properties));
            g_variant_builder_add(&objects,"{o@a{sa{sv}}}",paths[i],g_variant_builder_end(&interfaces));
        }
        g_dbus_method_invocation_return_value(inv,g_variant_new("(@a{oa{sa{sv}}})",g_variant_builder_end(&objects)));return;
    }
    if(!strcmp(name,"Get")) {
        g_dbus_method_invocation_return_value(inv,g_variant_new("(v)",g_variant_new_string(mock->alias)));return;
    }
    if(!strcmp(name,"Set")) {
        const char *iface,*property;GVariant *value;
        g_variant_get(args,"(&s&sv)",&iface,&property,&value);
        CHECK(!strcmp(iface,"org.bluez.Adapter1") && !strcmp(property,"Alias"));
        g_strlcpy(mock->alias,g_variant_get_string(value,NULL),sizeof(mock->alias));g_variant_unref(value);
    } else if(!strcmp(name,"RegisterApplication"))g_atomic_int_inc(&mock->registered);
    else if(!strcmp(name,"UnregisterApplication"))g_atomic_int_inc(&mock->unregistered);
    else if(!strcmp(name,"RegisterAdvertisement"))g_atomic_int_inc(&mock->advertised);
    else if(!strcmp(name,"UnregisterAdvertisement")) {
        if(g_atomic_int_get(&mock->reject_stop)) {
            g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Failed","injected stop failure");return;
        }
        g_atomic_int_inc(&mock->stopped);
    } else CHECK(false);
    g_dbus_method_invocation_return_value(inv,NULL);
}
static gpointer manager_thread(gpointer data)
{
    struct mock_bluez *mock=data;
    mock->context=g_main_context_new();g_main_context_push_thread_default(mock->context);
    mock->loop=g_main_loop_new(mock->context,FALSE);
    GDBusConnection *bus=g_dbus_connection_new_for_address_sync(mock->address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,NULL);
    CHECK(bus);
    mock->bus=bus;
    GVariant *reply=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus",
        "org.freedesktop.DBus","RequestName",g_variant_new("(su)",BLUEZ,0),NULL,0,1000,NULL,NULL);
    CHECK(reply);g_variant_unref(reply);
    GDBusNodeInfo *info=g_dbus_node_info_new_for_xml(manager_xml,NULL);
    const GDBusInterfaceVTable table={.method_call=manager_method};guint ids[4];
    for(int i=0;i<4;i++) {
        ids[i]=g_dbus_connection_register_object(bus,i==3?"/":"/org/bluez/hci0",info->interfaces[i],&table,mock,NULL,NULL);
        CHECK(ids[i]);
    }
    g_atomic_int_set(&mock->ready,1);g_main_loop_run(mock->loop);
    for(int i=0;i<4;i++)g_dbus_connection_unregister_object(bus,ids[i]);
    g_dbus_node_info_unref(info);g_dbus_connection_close_sync(bus,NULL,NULL);g_object_unref(bus);
    g_main_loop_unref(mock->loop);g_main_context_pop_thread_default(mock->context);g_main_context_unref(mock->context);
    return NULL;
}
static gboolean stop_manager(gpointer data){g_main_loop_quit(data);return G_SOURCE_REMOVE;}
static GVariant *invoke_from(struct otg_ble_hid *b,GDBusConnection *caller,const char *path,const char *iface,
                              const char *method_name,GVariant *args,bool success)
{
    struct pending_call call={0};
    g_dbus_connection_call(caller,g_dbus_connection_get_unique_name(b->bus),path,
        iface,method_name,args,NULL,0,1000,NULL,call_finished,&call);
    while(!call.done)g_main_context_iteration(NULL,TRUE);
    CHECK(success ? call.result!=NULL : call.error!=NULL);
    g_clear_error(&call.error);
    return call.result;
}
static GVariant *invoke(struct otg_ble_hid *b,const char *path,const char *iface,
                         const char *method_name,GVariant *args,bool success)
{
    return invoke_from(b,b->bus,path,iface,method_name,args,success);
}
static GVariant *read_at(struct otg_ble_hid *b,const char *path,guint16 offset,bool ok)
{
    GVariantBuilder options;g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","offset",g_variant_new_uint16(offset));
    return invoke(b,path,"org.bluez.GattCharacteristic1","ReadValue",
                  g_variant_new("(a{sv})",&options),ok);
}
static void write_byte(struct otg_ble_hid *b,const char *path,uint8_t value,bool ok)
{
    GVariant *result=invoke(b,path,"org.bluez.GattCharacteristic1","WriteValue",
        g_variant_new("(@ay@a{sv})",bytes(&value,1),g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0)),ok);
    if(result)g_variant_unref(result);
}
static void observe_devices(struct otg_ble_hid *b, struct mock_bluez *mock,
                            int connected, int unpaired, bool privacy)
{
    g_atomic_int_set(&mock->connected_mask,connected);
    g_atomic_int_set(&mock->unpaired_mask,unpaired);
    g_atomic_int_set(&mock->private_path,privacy);
    g_clear_object(&b->devices);
    b->devices=g_dbus_object_manager_client_new_sync(b->bus,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
    CHECK(b->devices);
    refresh_peer(b);
    update_channel_state(b);
}

static void test_automatic_peer(GDBusConnection *bus, struct mock_bluez *mock)
{
    struct otg_ble_hid b={.bus=bus,.adapter_path="/org/bluez/hci0",.latency_fd=-1,
                          .app_registered=true};
    observe_devices(&b,mock,3,0,false);
    start_notify(&b,0);
    start_notify(&b,1);
    observe_devices(&b,mock,1,0,false);
    CHECK(!b.channel_ready && !b.peer_address[0]);
    start_notify(&b,0);
    observe_devices(&b,mock,2,0,false);
    start_notify(&b,1);
    CHECK(!b.channel_ready && !b.peer_address[0]);

    observe_devices(&b,mock,1,1,true);
    start_notify(&b,0);
    start_notify(&b,1);
    CHECK(!b.channel_ready && !b.peer_address[0]);
    observe_devices(&b,mock,1,0,true);
    CHECK(b.channel_ready && !strcmp(b.peer_address,"AA:BB:CC:DD:EE:FF"));
    CHECK(!otg_ble_hid_set_peer(&b,"11:22:33:44:55:66"));
    observe_devices(&b,mock,2,0,false);
    start_notify(&b,0);
    start_notify(&b,1);
    CHECK(!b.channel_ready && !strcmp(b.peer_address,"AA:BB:CC:DD:EE:FF"));
    observe_devices(&b,mock,1,0,false);
    CHECK(b.channel_ready); /* Same identity reconnects on a different object path. */
    g_clear_object(&b.devices);
    g_free(b.peer_path);
    g_atomic_int_set(&mock->connected_mask,0);
}

static bool links_available=true;
static struct link_snapshot fake_links;

static bool read_fake_links(struct otg_ble_hid *b, struct link_snapshot *links)
{
    (void)b;
    *links=fake_links;
    return links_available;
}

static void acquire_and_start(struct otg_ble_hid *b,struct mock_bluez *mock,
                              const char *report,const char *device,const char *link)
{
    GVariantBuilder options;
    g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path(device));
    g_variant_builder_add(&options,"{sv}","link",g_variant_new_string(link));
    CHECK(!invoke_from(b,mock->bus,report,"org.bluez.GattCharacteristic1","AcquireNotify",
                       g_variant_new("(a{sv})",&options),false));
    GVariant *result=invoke_from(b,mock->bus,report,"org.bluez.GattCharacteristic1","StartNotify",NULL,true);
    g_variant_unref(result);
}

static void test_mixed_links(GDBusConnection *bus,struct mock_bluez *mock)
{
    struct otg_ble_hid b={.bus=bus,.adapter="hci0",.adapter_path="/org/bluez/hci0",.latency_fd=-1,
                          .resident=true,.read_links=read_fake_links};
    const char *phone="/org/bluez/hci0/dev_52_00_00_00_00_01";
    CHECK(register_objects(&b));
    CHECK(register_blueZ(&b));
    CHECK(otg_ble_hid_set_session(&b,true));

    fake_links=(struct link_snapshot){.le_count=1,.peripheral=true,.classic=true,.address="52:00:00:00:00:01"};
    observe_devices(&b,mock,3,1,true);
    CHECK(b.peer_path && !b.peer_connected && !b.channel_ready);
    start_notify(&b,0); // An unattributed subscription only is insufficient with classic links.
    start_notify(&b,1);
    CHECK(!b.peer_address[0]);
    acquire_and_start(&b,mock,ROOT "/hid/keys",phone,"LE");
    CHECK(!b.peer_address[0]);
    acquire_and_start(&b,mock,ROOT "/hid/pointer",phone,"LE");
    update_channel_state(&b);
    CHECK(!b.channel_ready && !b.peer_address[0]);
    observe_devices(&b,mock,3,0,true);
    CHECK(b.channel_ready && !strcmp(b.peer_address,"AA:BB:CC:DD:EE:FF"));

    CHECK(otg_ble_hid_set_session(&b,false));
    fake_links.le_count=0;
    observe_devices(&b,mock,3,0,true); // Device1 still shows connected over classic.
    CHECK(otg_ble_hid_set_session(&b,true));
    CHECK(!b.peer_connected && !b.channel_ready);
    fake_links.le_count=1;
    strcpy(fake_links.address,"AA:BB:CC:DD:EE:FF");
    observe_devices(&b,mock,3,0,false);
    CHECK(b.channel_ready); // Same phone, preserved CCCs, no new acquisition.
    fake_links.le_count=2;
    observe_devices(&b,mock,3,0,false);
    CHECK(!b.channel_ready);
    fake_links.le_count=1;
    links_available=false;
    observe_devices(&b,mock,3,0,false);
    CHECK(!b.channel_ready);
    links_available=true;
    observe_devices(&b,mock,3,0,false);
    CHECK(b.channel_ready);

    acquire_and_start(&b,mock,ROOT "/hid/keys","/org/bluez/hci0/dev_11_22_33_44_55_66","BR/EDR");
    update_channel_state(&b);
    CHECK(!b.channel_ready);
    CHECK(otg_ble_hid_set_session(&b,false));
    close_ble(&b);
    g_clear_object(&b.devices);
    g_free(b.peer_path);
}
struct acquired_fd { bool done; GVariant *reply; GUnixFDList *fds; };
static void got_fd(GObject *source,GAsyncResult *result,gpointer user)
{
    struct acquired_fd *a=user;
    a->reply=g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source),&a->fds,result,NULL);
    a->done=true;
}

static GVariant *device_options(const char *path)
{
    GVariantBuilder options;
    g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path(path));
    g_variant_builder_add(&options,"{sv}","link",g_variant_new_string("LE"));
    g_variant_builder_add(&options,"{sv}","mtu",g_variant_new_uint16(23));
    return g_variant_builder_end(&options);
}

static int get_report_fd(struct otg_ble_hid *b,GDBusConnection *caller,const char *phone,unsigned n)
{
    struct acquired_fd a={0};
    g_dbus_connection_call_with_unix_fd_list(caller,g_dbus_connection_get_unique_name(b->bus),
        n?ROOT "/hid/pointer":ROOT "/hid/keys","org.bluez.GattCharacteristic1","AcquireNotify",
        g_variant_new("(@a{sv})",device_options(phone)),G_VARIANT_TYPE("(hq)"),0,1000,NULL,NULL,got_fd,&a);
    while(!a.done)g_main_context_iteration(NULL,TRUE);
    if (!a.reply) return -1;
    gint index;guint16 mtu;g_variant_get(a.reply,"(hq)",&index,&mtu);
    CHECK(mtu==23 && a.fds);
    int fd=g_unix_fd_list_get(a.fds,index,NULL);CHECK(fd>=0);
    g_variant_unref(a.reply);g_object_unref(a.fds);
    return fd;
}

static void expect_report(int fd,const uint8_t expected[8])
{
    uint8_t data[9];
    CHECK(recv(fd,data,sizeof(data),MSG_DONTWAIT)==8 && !memcmp(data,expected,8));
}

static void finish_advertisement_call(struct otg_ble_hid *hid, enum adv_operation operation)
{
    CHECK(hid->adv_operation==operation);
    while (!hid->adv_call.done) g_main_context_iteration(NULL,TRUE);
    poll_directed_advertisement(hid,monotonic_ns());
}

static void test_directed_advertisement(GDBusConnection *bus,struct mock_bluez *mock)
{
    const char *peer="/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
    struct otg_ble_hid hid={.bus=bus,.adapter_path="/org/bluez/hci0",.read_links=read_fake_links,
                            .resident=true,.latency_fd=-1};
    fake_links=(struct link_snapshot){0};
    observe_devices(&hid,mock,0,0,false);
    hid.directed_owner=g_strdup(g_dbus_connection_get_unique_name(mock->bus));
    CHECK(register_objects(&hid) && register_blueZ(&hid));
    struct otg_ble_route *first=otg_ble_route_bind(&hid,"AA:BB:CC:DD:EE:FF");
    CHECK(first && otg_ble_hid_set_session(&hid,true));
    const int started=g_atomic_int_get(&mock->advertised);
    const int stopped=g_atomic_int_get(&mock->stopped);
    const int gatt=g_atomic_int_get(&mock->registered);
    const int gatt_removed=g_atomic_int_get(&mock->unregistered);

    fake_links=(struct link_snapshot){.le_count=1,.entries={{"AA:BB:CC:DD:EE:FF",5,true}}};
    observe_devices(&hid,mock,1,0,false);
    int keys=get_report_fd(&hid,mock->bus,peer,0),pointer=get_report_fd(&hid,mock->bus,peer,1);
    CHECK(keys>=0 && pointer>=0 && directed_poll(&hid) && first->ready && hid.adv_rearm);
    const uint8_t neutral[8]={0},key[8]={0,0,30};
    expect_report(keys,neutral);expect_report(pointer,neutral);
    const uint64_t generation=first->generation;

    g_atomic_int_set(&mock->reject_stop,1);
    poll_directed_advertisement(&hid,hid.adv_next_ns);
    finish_advertisement_call(&hid,ADV_REMOVE);
    CHECK(hid.adv_operation==ADV_NONE && hid.adv_rearm && hid.adv_registered);
    CHECK(g_atomic_int_get(&mock->advertised)==started && g_atomic_int_get(&mock->stopped)==stopped);
    g_atomic_int_set(&mock->reject_stop,0);
    poll_directed_advertisement(&hid,hid.adv_next_ns);
    otg_ble_route_send(first,1,key);expect_report(keys,key);
    finish_advertisement_call(&hid,ADV_REMOVE);
    finish_advertisement_call(&hid,ADV_REGISTER);
    CHECK(hid.adv_registered && !hid.adv_rearm && first->ready && first->generation==generation);
    CHECK(g_atomic_int_get(&mock->advertised)==started+1 && g_atomic_int_get(&mock->stopped)==stopped+1);
    CHECK(g_atomic_int_get(&mock->registered)==gatt && g_atomic_int_get(&mock->unregistered)==gatt_removed);

    struct otg_ble_route *second=otg_ble_route_bind(&hid,"11:22:33:44:55:66");
    CHECK(second && otg_ble_hid_set_session(&hid,true));
    fake_links.le_count=2;
    fake_links.entries[1]=(struct le_link){"11:22:33:44:55:66",9,true};
    observe_devices(&hid,mock,3,0,false);
    const char *other="/org/bluez/hci0/dev_11_22_33_44_55_66";
    int other_keys=get_report_fd(&hid,mock->bus,other,0),other_pointer=get_report_fd(&hid,mock->bus,other,1);
    CHECK(other_keys>=0 && other_pointer>=0 && directed_poll(&hid) && first->ready && second->ready);
    expect_report(other_keys,neutral);expect_report(other_pointer,neutral);
    poll_directed_advertisement(&hid,hid.adv_next_ns);
    CHECK(hid.adv_operation==ADV_REMOVE); /* A third phone can still connect. */
    finish_advertisement_call(&hid,ADV_REMOVE);
    finish_advertisement_call(&hid,ADV_REGISTER);
    CHECK(hid.adv_registered && first->ready && second->ready);

    fake_links.le_count=OTG_BLE_MAX_PHONES;
    for (unsigned i=2;i<fake_links.le_count;i++) {
        fake_links.entries[i].peripheral=true;
        fake_links.entries[i].handle=(uint16_t)(i+10);
    }
    CHECK(directed_poll(&hid));
    poll_directed_advertisement(&hid,hid.adv_next_ns);
    finish_advertisement_call(&hid,ADV_REMOVE);
    CHECK(!hid.adv_registered && hid.adv_operation==ADV_NONE);

    fake_links.le_count=1;
    observe_devices(&hid,mock,1,0,false);
    CHECK(directed_poll(&hid) && first->ready && !second->ready && hid.adv_rearm);
    poll_directed_advertisement(&hid,hid.adv_next_ns);
    CHECK(hid.adv_operation==ADV_REGISTER);
    otg_ble_route_unbind(first);
    otg_ble_route_unbind(second);
    CHECK(otg_ble_hid_set_session(&hid,false));
    CHECK(hid.adv_operation==ADV_NONE && !hid.adv_rearm && !hid.adv_registered && hid.app_registered);
    CHECK(g_atomic_int_get(&mock->advertised)==started+3 && g_atomic_int_get(&mock->stopped)==stopped+4);
    CHECK(g_atomic_int_get(&mock->unregistered)==gatt_removed);
    close(keys);close(pointer);close(other_keys);close(other_pointer);
    close_ble(&hid);directed_free(first);directed_free(second);g_free(hid.directed_owner);
    g_clear_object(&hid.devices);
}

static void test_directed(GDBusConnection *bus,struct mock_bluez *mock)
{
    const char *a="/org/bluez/hci0/dev_52_00_00_00_00_01",*b="/org/bluez/hci0/dev_11_22_33_44_55_66";
    struct otg_ble_hid hid={.bus=bus,.adapter_path="/org/bluez/hci0",.read_links=read_fake_links,
                            .resident=true,.latency_fd=-1,.legacy_handles=true};
    fake_links=(struct link_snapshot){.le_count=2,.entries={{"52:00:00:00:00:01",5,true},{"11:22:33:44:55:66",9,true}}};
    observe_devices(&hid,mock,3,0,true);
    hid.directed_owner=g_strdup(g_dbus_connection_get_unique_name(mock->bus));
    CHECK(register_objects(&hid) && register_blueZ(&hid));
    struct otg_ble_route *one=otg_ble_route_bind(&hid,"AA:BB:CC:DD:EE:FF");
    struct otg_ble_route *two=otg_ble_route_bind(&hid,"11:22:33:44:55:66");
    CHECK(one && two && !otg_ble_route_bind(&hid,"aa:bb:cc:dd:ee:ff"));
    CHECK(otg_ble_hid_set_session(&hid,true));
    CHECK(get_report_fd(&hid,bus,a,0)<0);
    CHECK(!invoke_from(&hid,mock->bus,ROOT "/hid/keys","org.bluez.GattCharacteristic1","StartNotify",NULL,false));
    int ak=get_report_fd(&hid,mock->bus,a,0),bp=get_report_fd(&hid,mock->bus,b,1);
    CHECK(ak>=0 && bp>=0 && directed_poll(&hid) && !one->ready && !two->ready);
    CHECK(otg_ble_route_waiting_for_subscriptions(one) && otg_ble_route_waiting_for_subscriptions(two));
    int ap=get_report_fd(&hid,mock->bus,a,1),bk=get_report_fd(&hid,mock->bus,b,0);
    CHECK(directed_poll(&hid) && one->ready && two->ready);
    CHECK(!otg_ble_route_waiting_for_subscriptions(one) && !otg_ble_route_waiting_for_subscriptions(two));
    uint8_t neutral[8]={0},key[8]={0,0,30},drag[8]={3,1,32,0,42};
    expect_report(ak,neutral);expect_report(ap,neutral);expect_report(bk,neutral);expect_report(bp,neutral);
    otg_ble_route_send(one,1,key);otg_ble_route_send(two,2,drag);
    expect_report(ak,key);expect_report(bp,drag);
    struct pollfd silent[]={{bk,POLLIN,0},{ap,POLLIN,0}};CHECK(!poll(silent,2,0));
    const char *c="/org/bluez/hci0/dev_52_00_00_00_00_02";
    fake_links.le_count=3;
    fake_links.entries[2]=(struct le_link){"52:00:00:00:00:02",11,true};
    observe_devices(&hid,mock,11,8,true);
    CHECK(get_report_fd(&hid,mock->bus,c,0)<0); /* No discovery session yet. */
    otg_ble_route_enable_discovery(&hid,true);
    int ck=get_report_fd(&hid,mock->bus,c,0),cp=get_report_fd(&hid,mock->bus,c,1);
    CHECK(ck>=0 && cp>=0 && !otg_ble_route_claim_discovered(&hid));
    observe_devices(&hid,mock,11,0,true); /* Pairing resolves the same handle's private address. */
    CHECK(directed_poll(&hid));
    struct otg_ble_route *three=otg_ble_route_claim_discovered(&hid);
    CHECK(three && !strcmp(otg_ble_route_identity(three),"22:33:44:55:66:77"));
    CHECK(directed_poll(&hid) && one->ready && two->ready && three->ready);
    expect_report(ck,neutral);expect_report(cp,neutral);
    otg_ble_route_send(three,1,key);expect_report(ck,key);
    CHECK(!poll(silent,2,0));
    otg_ble_route_unbind(three);expect_report(ck,neutral);expect_report(cp,neutral);
    close(ck);close(cp);fake_links.le_count=2;
    observe_devices(&hid,mock,3,0,true);
    otg_ble_route_enable_discovery(&hid,false);
    GVariant *read=invoke_from(&hid,mock->bus,ROOT "/hid/keys","org.bluez.GattCharacteristic1","ReadValue",
        g_variant_new("(@a{sv})",device_options(b)),true);
    GVariant *val=g_variant_get_child_value(read,0);gsize n=0;
    const uint8_t *data=g_variant_get_fixed_array(val,&n,1);CHECK(n==8 && !memcmp(data,neutral,8));
    g_variant_unref(val);g_variant_unref(read);
    uint8_t led=2;
    read=invoke_from(&hid,mock->bus,ROOT "/hid/leds","org.bluez.GattCharacteristic1","WriteValue",
        g_variant_new("(@ay@a{sv})",bytes(&led,1),device_options(a)),true);g_variant_unref(read);
    CHECK(one->leds==2 && !two->leds);
    uint8_t suspend=0;
    read=invoke_from(&hid,mock->bus,ROOT "/hid/control","org.bluez.GattCharacteristic1","WriteValue",
        g_variant_new("(@ay@a{sv})",bytes(&suspend,1),device_options(a)),true);g_variant_unref(read);
    CHECK(directed_poll(&hid) && !one->ready && two->ready);
    expect_report(ak,neutral);expect_report(ap,neutral);
    /* Per-client pacing can merge drag coordinates; a scroll must flush the
     * final drag position and then arrive as a separate packet. */
    uint8_t move[8]={3,1,53,0,42},scroll[8]={3,1,53,0,42,0,1};
    otg_ble_route_send(two,2,move);otg_ble_route_send(two,2,scroll);
    expect_report(bp,move);expect_report(bp,scroll);
    close(ak);close(ap);
    CHECK(directed_poll(&hid) && !one->ready && two->ready && one->generation);
    CHECK(!memcmp(two->value[1],scroll,8));
    otg_ble_route_send(one,1,key);otg_ble_route_send(two,1,key);expect_report(bk,key);
    observe_devices(&hid,mock,3,1,true);
    /* Preserve the identity string while replacing the HCI connection. */
    fake_links.entries[0].handle++;
    ak=get_report_fd(&hid,mock->bus,a,0);CHECK(ak>=0);
    ap=get_report_fd(&hid,mock->bus,a,1);CHECK(ap>=0);
    CHECK(directed_poll(&hid) && !one->ready && two->ready);
    CHECK(!invoke_from(&hid,mock->bus,ROOT "/hid/keys","org.bluez.GattCharacteristic1","ReadValue",
        g_variant_new("(@a{sv})",device_options(a)),false));
    observe_devices(&hid,mock,3,0,true);
    CHECK(directed_poll(&hid) && one->ready);
    expect_report(ak,neutral);expect_report(ap,neutral);
    otg_ble_route_unbind(one);expect_report(ak,neutral);expect_report(ap,neutral);
    CHECK(!one->ready && two->ready && !memcmp(two->value[1],scroll,8));
    links_available=false;CHECK(directed_poll(&hid) && !two->ready && two->fd[0]<0);
    links_available=true;
    close(ak);close(ap);close(bk);close(bp);
    otg_ble_route_unbind(two);CHECK(otg_ble_hid_set_session(&hid,false));
    close_ble(&hid);directed_free(one);directed_free(two);directed_free(three);g_free(hid.directed_owner);
    g_free(hid.peer_path);g_clear_object(&hid.devices);
}

int main(void)
{
    char *daemon=g_find_program_in_path("dbus-daemon");
    if(!daemon)return 77;
    g_free(daemon);
    GTestDBus *test=g_test_dbus_new(G_TEST_DBUS_NONE);g_test_dbus_up(test);
    struct mock_bluez mock={.address=g_test_dbus_get_bus_address(test),.alias="original-host"};
    GThread *thread=g_thread_new("mock-bluez",manager_thread,&mock);
    while(!g_atomic_int_get(&mock.ready))g_usleep(1000);
    struct otg_ble_hid b={.latency_fd=-1};
    b.bus=g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(test),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL,NULL,NULL);
    CHECK(b.bus);
    test_automatic_peer(b.bus,&mock);
    b.original_alias=g_strdup(mock.alias);
    node_info=g_dbus_node_info_new_for_xml(xml,NULL);CHECK(node_info);
    CHECK(register_objects(&b));
    GVariant *advertisement=props(&b.objects[11],"org.bluez.LEAdvertisement1");
    const char *local_name=NULL;
    CHECK(g_variant_lookup(advertisement,"LocalName","&s",&local_name) &&
          !strcmp(local_name,"original-host"));
    g_variant_unref(advertisement);
    uint8_t map[]={1,2,3,4,5,6,7,8};b.report_map=map;b.report_map_size=sizeof(map);
    GVariant *result=invoke(&b,ROOT,"org.freedesktop.DBus.ObjectManager","GetManagedObjects",NULL,true);
    GVariant *objects=g_variant_get_child_value(result,0);
    CHECK(g_variant_n_children(objects)==10);g_variant_unref(objects);g_variant_unref(result);
    result=invoke(&b,ROOT "/hid/keys","org.freedesktop.DBus.Properties","GetAll",
        g_variant_new("(s)","org.bluez.GattCharacteristic1"),true);
    GVariant *props_value=g_variant_get_child_value(result,0);guint16 handle=0;
    CHECK(g_variant_lookup(props_value,"Handle","q",&handle) && handle==0x106);
    g_variant_unref(props_value);g_variant_unref(result);
    b.legacy_handles=true;
    for(unsigned report=0;report<2;report++) {
        result=invoke(&b,report?ROOT "/hid/pointer":ROOT "/hid/keys",
            "org.freedesktop.DBus.Properties","GetAll",
            g_variant_new("(s)","org.bluez.GattCharacteristic1"),true);
        props_value=g_variant_get_child_value(result,0);
        /* Truncating a low value handle on an older host preserves its CCCD lookup. */
        CHECK(g_variant_lookup(props_value,"Handle","q",&handle));
        CHECK(handle==(report?0x004a:0x0046) && (uint8_t)handle==handle);
        g_variant_unref(props_value);g_variant_unref(result);
    }
    b.legacy_handles=false;
    result=read_at(&b,ROOT "/hid/map",3,true);
    GVariant *value=g_variant_get_child_value(result,0);gsize size;
    const uint8_t *data=g_variant_get_fixed_array(value,&size,1);
    CHECK(size==5 && !memcmp(data,map+3,5));g_variant_unref(value);g_variant_unref(result);
    result=read_at(&b,ROOT "/hid/map",8,true);g_variant_unref(result);
    CHECK(!read_at(&b,ROOT "/hid/map",9,false));
    result=read_at(&b,ROOT "/hid/keys",0,true);
    value=g_variant_get_child_value(result,0);data=g_variant_get_fixed_array(value,&size,1);
    CHECK(size==8 && data[2]==0);g_variant_unref(value);g_variant_unref(result);
    write_byte(&b,ROOT "/hid/leds",2,true);CHECK(b.leds==2);
    write_byte(&b,ROOT "/hid/control",0,true);CHECK(b.suspended);
    write_byte(&b,ROOT "/hid/control",1,true);CHECK(!b.suspended);
    write_byte(&b,ROOT "/hid/control",3,false);CHECK(!b.suspended);
    write_byte(&b,ROOT "/hid/keys",4,false);
    /* Real private D-Bus lifecycle: session teardown removes advertising, not
     * the registered GATT service; a second session needs no new StartNotify. */
    b.resident=true;b.adapter="hci0";b.adapter_path="/org/bluez/hci0";
    CHECK(register_blueZ(&b));CHECK(b.app_registered && !b.adv_registered);
    CHECK(otg_ble_hid_set_session(&b,true));
    CHECK(!strcmp(mock.alias,"original-host"));
    b.peer_connected=true;update_channel_state(&b);CHECK(!b.channel_ready);
    result=invoke(&b,ROOT "/hid/keys","org.bluez.GattCharacteristic1","StartNotify",NULL,true);g_variant_unref(result);
    result=invoke(&b,ROOT "/hid/pointer","org.bluez.GattCharacteristic1","StartNotify",NULL,true);g_variant_unref(result);
    update_channel_state(&b);CHECK(b.channel_ready);
    b.values[0][2]=4;b.values[1][1]=1;b.pointer_pending=true;
    CHECK(otg_ble_hid_set_session(&b,false));
    CHECK(b.app_registered && !b.adv_registered && !b.channel_ready);
    CHECK(b.notifying[0] && b.notifying[1]);
    CHECK(!b.values[0][2] && !b.values[1][1] && !b.pointer_pending);
    CHECK(!g_atomic_int_get(&mock.unregistered));
    CHECK(otg_ble_hid_set_session(&b,true));CHECK(b.channel_ready);
    CHECK(g_atomic_int_get(&mock.registered)==1 && g_atomic_int_get(&mock.advertised)==2);
    g_atomic_int_set(&mock.reject_stop,1);
    CHECK(!otg_ble_hid_set_session(&b,false));CHECK(!b.channel_ready && b.adv_registered);
    g_atomic_int_set(&mock.reject_stop,0);
    CHECK(otg_ble_hid_set_session(&b,false));CHECK(!b.adv_registered);
    /* A real ObjectManager snapshot on the private bus exercises identity,
     * pairing and multi-peer fail-closed behavior, without system Bluetooth. */
    CHECK(otg_ble_hid_set_peer(&b,"AA:BB:CC:DD:EE:FF"));
    for(int mask=0;mask<4;mask++) {
        g_atomic_int_set(&mock.connected_mask,mask);
        g_clear_object(&b.devices);
        b.devices=g_dbus_object_manager_client_new_sync(b.bus,
            G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
        CHECK(b.devices);
        refresh_peer(&b);
        CHECK(b.peer_connected==(mask==1));
        b.session_enabled=true;update_channel_state(&b);
        CHECK(b.channel_ready==(mask==1));
    }
    /* Identity Address changes need not rename an object's original private
     * path. An offline bonded alias must never authenticate the live link. */
    g_atomic_int_set(&mock.private_path,1);g_atomic_int_set(&mock.connected_mask,1);
    g_clear_object(&b.devices);
    b.devices=g_dbus_object_manager_client_new_sync(b.bus,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
    CHECK(b.devices);refresh_peer(&b);update_channel_state(&b);
    CHECK(b.channel_ready && !strcmp(b.peer_path,"/org/bluez/hci0/dev_52_00_00_00_00_01"));
    GVariantBuilder options;g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path(b.peer_path));
    GVariant *target=g_variant_ref_sink(g_variant_builder_end(&options));
    CHECK(target_options(&b,target));g_variant_unref(target);
    g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"));
    target=g_variant_ref_sink(g_variant_builder_end(&options));
    CHECK(!target_options(&b,target));g_variant_unref(target);
    g_atomic_int_set(&mock.connected_mask,1);g_atomic_int_set(&mock.unpaired_mask,1);
    g_clear_object(&b.devices);
    b.devices=g_dbus_object_manager_client_new_sync(b.bus,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
    CHECK(b.devices);refresh_peer(&b);update_channel_state(&b);
    CHECK(!b.peer_connected && !b.channel_ready);
    CHECK(strstr(b.wait_reason,"not paired"));
    CHECK(!read_at(&b,ROOT "/hid/keys",0,false)); // Non-BlueZ D-Bus caller rejected.
    result=invoke(&b,ROOT "/hid/keys","org.bluez.GattCharacteristic1","StartNotify",NULL,false);
    CHECK(!result);
    close_ble(&b);
    CHECK(g_atomic_int_get(&mock.unregistered)==1);
    g_free(b.original_alias);
    g_clear_object(&b.devices);g_free(b.peer_path);
    test_mixed_links(b.bus,&mock);
    test_directed(b.bus,&mock);
    test_directed_advertisement(b.bus,&mock);
    g_dbus_node_info_unref(node_info);node_info=NULL;
    g_dbus_connection_close_sync(b.bus,NULL,NULL);g_object_unref(b.bus);
    g_main_context_invoke(mock.context,stop_manager,mock.loop);g_thread_join(thread);
    g_test_dbus_down(test);g_object_unref(test);
    puts("PASS: GATT export, stable handles, long-read offsets, neutral reports and control point");
    return 0;
}
