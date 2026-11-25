/* Real Unix seqpacket IPC, fake HID backend; no root or system Bluetooth.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../platform/linux/ble_session.c"
#include <sys/wait.h>
static volatile sig_atomic_t stop_service;
static void stop_signal(int signal_number){(void)signal_number;stop_service=1;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)

struct otg_ble_hid {
    bool application,advertising,active,connected,subscribed[2];
    uint8_t report[2][8];
    uint64_t interval;
};
struct otg_ble_route { bool bound; char address[18]; uint64_t gen; unsigned sent; };
bool otg_ble_verified_instance(char *token,size_t capacity) {(void)token;(void)capacity;return false;}
static struct otg_ble_route fake_routes[2];
struct otg_ble_hid *otg_ble_hid_create_directed(const char *adapter,bool legacy,const char *instance)
{
    CHECK(!strcmp(instance,"fixture"));
    return otg_ble_hid_create_resident(adapter,legacy);
}
struct otg_ble_route *otg_ble_route_bind(struct otg_ble_hid *b,const char *addr)
{
    (void)b;
    for (unsigned i=0;i<2;i++) if (fake_routes[i].bound && !g_ascii_strcasecmp(fake_routes[i].address,addr)) return NULL;
    for (unsigned i=0;i<2;i++) if (!fake_routes[i].bound) {
        fake_routes[i].bound=true;
        g_strlcpy(fake_routes[i].address,addr,18);
        return &fake_routes[i];
    }
    return NULL;
}
void otg_ble_route_unbind(struct otg_ble_route *r) {r->bound=false;}
bool otg_ble_route_ready(const struct otg_ble_route *r,uint64_t *gen) {*gen=r->gen;return r->bound;}
void otg_ble_route_send(struct otg_ble_route *r,uint16_t id,const uint8_t data[8])
{
    CHECK(r->bound && id==1 && data[2]==(uint8_t)(r-fake_routes+4));
    r->sent++;
}
static int created,destroyed,activated,deactivated;
static bool fail_idle;
static bool fake_register(void *ctx,uint16_t id,const uint8_t *data,uint16_t size)
{
    struct otg_ble_hid *b=ctx;
    CHECK((id==1 || id==2) && size==8 && data[0]==5);
    if(id==2)b->application=true;
    return true;
}
static bool fake_ready(void *ctx)
{
    struct otg_ble_hid *b=ctx;
    return b->active && b->connected && b->subscribed[0] && b->subscribed[1];
}
static bool fake_send(void *ctx,uint16_t id,const uint8_t *data,uint16_t size)
{
    CHECK(size==8 && (id==1 || id==2));
    if(!fake_ready(ctx))return false;
    memcpy(((struct otg_ble_hid *)ctx)->report[id-1],data,size);return true;
}
static bool fake_poll(void *ctx){(void)ctx;return true;}
static const struct otg_hid_transport_ops FAKE_OPS={fake_register,fake_send,fake_poll,fake_ready,NULL};
const struct otg_hid_transport_ops *otg_ble_hid_ops(void){return &FAKE_OPS;}
static bool last_created_legacy_handles;
struct otg_ble_hid *otg_ble_hid_create_resident(const char *adapter,bool legacy_handles)
{
    CHECK(!strcmp(adapter,"hci0"));created++;
    last_created_legacy_handles=legacy_handles;
    return g_new0(struct otg_ble_hid,1);
}
void otg_ble_hid_destroy(struct otg_ble_hid *b){destroyed++;g_free(b);}
bool otg_ble_hid_set_session(struct otg_ble_hid *b,bool enabled)
{
    CHECK(b->application);
    if(enabled)activated++;
    else {deactivated++;memset(b->report,0,sizeof(b->report));}
    b->active=b->advertising=enabled;
    return enabled || !fail_idle;
}
void otg_ble_hid_set_interval(struct otg_ble_hid *b,uint64_t us){b->interval=us;}
bool otg_ble_hid_set_peer(struct otg_ble_hid *b,const char *address)
{(void)b;return strlen(address)==17;}

static void pair_socket(int *fds)
{CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,fds));}
static struct message descriptor(unsigned id)
{
    struct message m={.operation=REGISTER,.id=id,.size=8,.data={5,1,9,6,0xa1,1,0xc0,0}};
    return m;
}
static void test_packets(void)
{
    int fds[2];pair_socket(fds);struct message m;
    CHECK(!receive_message(fds[0],&m));
    CHECK(send_message(fds[1],REGISTER,1,"12345678",8));
    CHECK(receive_message(fds[0],&m)==1 && m.id==1 && m.size==8);
    CHECK(!memcmp(m.data,"12345678",8));
    CHECK(peer_uid(fds[0],getuid()));CHECK(!peer_uid(fds[0],getuid()+1));
    m.version++;
    CHECK(send(fds[1],&m,HEADER_SIZE+8,0)==HEADER_SIZE+8);
    CHECK(receive_message(fds[0],&m)==-1);
    uint8_t oversized[sizeof(m)+1]={0};
    CHECK(send(fds[1],oversized,sizeof(oversized),0)==sizeof(oversized));
    CHECK(receive_message(fds[0],&m)==-1);
    CHECK(send_message(fds[1],STATUS,0,NULL,0));
    CHECK(receive_message(fds[0],&m)==1);
    m.size=1;CHECK(send(fds[1],&m,HEADER_SIZE,0)==HEADER_SIZE);
    CHECK(receive_message(fds[0],&m)==-1);
    close(fds[1]);CHECK(receive_message(fds[0],&m)==-1);close(fds[0]);
    char dir[128];struct sockaddr_un address;
    CHECK(socket_path(getuid(),"hci0",dir,sizeof(dir),&address));
    CHECK(!socket_path(getuid(),"hci0/../../x",dir,sizeof(dir),&address));
    CHECK(!valid_adapter("hci65535") && !valid_adapter("hci-1"));
}

static void test_sessions(void)
{
    int fds[2];pair_socket(fds);
    struct service s={.client=fds[0],.clean=true,.adapter="hci0"};
    struct message keys=descriptor(1),pointer=descriptor(2),reply;
    struct message peer={.operation=SELECT_PEER,.size=17,.data="aa:bb:cc:dd:ee:ff"};
    CHECK(handle_session(&s,&peer));
    CHECK(!strcmp(s.peer,"AA:BB:CC:DD:EE:FF"));
    const struct message automatic={.operation=SELECT_PEER};
    CHECK(!handle_session(&s,&automatic));
    peer.data[0]='Z';CHECK(!handle_session(&s,&peer));peer.data[0]='a';
    CHECK(handle_session(&s,&keys));CHECK(!s.hid && !s.active);
    CHECK(handle_session(&s,&pointer));
    CHECK(created==1 && activated==1 && s.hid->application && s.hid->advertising);
    s.hid->connected=true;
    CHECK(reply_status(&s,fds[0],true));
    CHECK(receive_message(fds[1],&reply)==1 && !reply.data[0]);
    s.hid->subscribed[0]=true;
    CHECK(!fake_ready(s.hid)); /* One CCC is not enough. */
    s.hid->subscribed[1]=true;CHECK(fake_ready(s.hid));
    struct message report={.operation=REPORT,.id=1,.size=8,.data={2,0,4}};
    CHECK(handle_session(&s,&report) && s.hid->report[0][2]==4);
    report.id=2;report.data[0]=3;report.data[1]=1;
    CHECK(handle_session(&s,&report) && s.hid->report[1][1]==1);
    struct message interval={.operation=POLL_SESSION,.size=8};uint64_t us=7500;
    memcpy(interval.data,&us,8);CHECK(handle_session(&s,&interval));CHECK(s.hid->interval==7500);

    /* SIGKILL/EOF equivalent: the socket vanishes with held input. The service
     * must release it and stop advertising without destroying GATT or CCCs. */
    close(fds[1]);CHECK(receive_message(fds[0],&reply)==-1);drop_client(&s);
    CHECK(!s.active && s.clean && !s.hid->advertising);
    CHECK(!s.hid->report[0][2] && !s.hid->report[1][1]);
    CHECK(s.hid->application && s.hid->subscribed[0] && s.hid->subscribed[1]);
    CHECK(!fake_ready(s.hid) && destroyed==0);

    pair_socket(fds);
    struct pending_peer pending={fds[0],g_get_monotonic_time()+1000000,true};
    CHECK(send_message(fds[1],ATTACH,0,NULL,0));handle_pending(&s,&pending);
    CHECK(pending.fd==-1 && s.client==fds[0]);
    CHECK(receive_message(fds[1],&reply)==1 && !reply.id);
    CHECK(handle_session(&s,&peer));
    peer.data[0]='1';CHECK(!handle_session(&s,&peer));peer.data[0]='a';
    CHECK(handle_session(&s,&keys) && handle_session(&s,&pointer));
    CHECK(created==1 && activated==2 && fake_ready(s.hid));
    CHECK(!s.hid->report[0][2] && !s.hid->report[1][1]);
    s.hid->connected=false;CHECK(!fake_ready(s.hid));
    report.id=1;report.data[2]=7;
    CHECK(handle_session(&s,&report) && !s.hid->report[0][2]);
    s.hid->connected=true;CHECK(fake_ready(s.hid));
    s.hid->subscribed[1]=false;CHECK(!fake_ready(s.hid));

    int other[2];pair_socket(other);
    pending=(struct pending_peer){other[0],g_get_monotonic_time()+1000000,true};
    CHECK(send_message(other[1],ATTACH,0,NULL,0));handle_pending(&s,&pending);
    CHECK(receive_message(other[1],&reply)==1 && reply.id==1);close(other[1]);
    pair_socket(other);
    pending=(struct pending_peer){other[0],g_get_monotonic_time()+1000000,true};
    CHECK(send_message(other[1],STOP_SERVICE,0,NULL,0));handle_pending(&s,&pending);
    CHECK(receive_message(other[1],&reply)==1 && reply.id==1 && !s.shutdown);close(other[1]);
    pair_socket(other);
    pending=(struct pending_peer){other[0],g_get_monotonic_time()+1000000,false};
    CHECK(send_message(other[1],ATTACH,0,NULL,0));handle_pending(&s,&pending);
    CHECK(receive_message(other[1],&reply)==-1);close(other[1]);

    CHECK(idle_service(&s));close(s.client);s.client=-1;close(fds[1]);
    keys.data[0]=6;CHECK(!handle_session(&s,&keys));keys.data[0]=5;
    CHECK(created==1 && destroyed==0); /* Schema changes never replace GATT. */
    pair_socket(fds);s.client=fds[0];s.last_request=100;
    s.active=s.hid->active=s.hid->advertising=true;s.hid->report[0][2]=4;
    expire_client(&s,5000099);CHECK(s.client==fds[0]);
    expire_client(&s,5000100);CHECK(s.client==-1 && s.clean && !s.active);
    CHECK(!s.hid->report[0][2] && s.hid->application && !s.hid->advertising);
    close(fds[1]);
    fail_idle=true;CHECK(!idle_service(&s) && !s.clean);
    fail_idle=false;CHECK(idle_service(&s) && s.clean);
    CHECK(deactivated>=3);
    otg_ble_hid_destroy(s.hid);
}

static void test_client_rpc(bool abrupt)
{
    int fds[2],ipc[2];pair_socket(fds);
    CHECK(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,ipc));
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid) {
        close(fds[1]);close(ipc[0]);close(ipc[1]);
        struct service s={.client=fds[0],.clean=true,.adapter="hci0"};
        bool received_report=false;
        for(;;) {
            struct message m;int n=receive_message(s.client,&m);
            if(!n){g_usleep(1000);continue;}
            if(n<0){drop_client(&s);break;}
            if(m.operation==DETACH) {
                CHECK(idle_service(&s));CHECK(reply_status(&s,s.client,true));
                close(s.client);s.client=-1;break;
            }
            CHECK(handle_session(&s,&m));
            if(s.active) {
                /* Simulate genuine BlueZ callbacks, not client assertions. */
                s.hid->connected=s.hid->subscribed[0]=s.hid->subscribed[1]=true;
            }
            if(m.operation==REPORT)received_report=true;
            CHECK(reply_status(&s,s.client,true));
        }
        CHECK(received_report && s.clean && !s.active && s.hid->application);
        CHECK(!s.hid->advertising && !s.hid->report[0][2]);
        CHECK(s.hid->subscribed[0] && s.hid->subscribed[1]);
        otg_ble_hid_destroy(s.hid);_exit(0);
    }
    close(fds[0]);
    struct session_owner *owner=g_new0(struct session_owner,1);
    struct otg_ble_session *s=&owner->session;
    s->fd=fds[1];s->alive=true;s->latency_fd=ipc[0];g_strlcpy(owner->adapter,"hci0",sizeof(owner->adapter));
    struct message keys=descriptor(1),pointer=descriptor(2);
    CHECK(session_register(s,1,keys.data,keys.size));
    CHECK(session_register(s,2,pointer.data,pointer.size));
    CHECK(session_poll(s) && session_ready(s));
    char text[64]={0};CHECK(recv(ipc[1],text,sizeof(text),0)==11);
    CHECK(!strcmp(text,"ready:hci0\n"));
    CHECK(send(ipc[1],"interval_us=7500\n",17,0)==17);
    CHECK(session_poll(s));
    uint8_t report[8]={0,0,4};CHECK(session_send(s,1,report,8));
    gint64 deadline=g_get_monotonic_time()+1000000;
    while(s->in_flight || s->count) {
        CHECK(g_get_monotonic_time()<deadline && session_pump(s,false));
        g_usleep(100);
    }
    if(abrupt) {
        close(s->fd);s->fd=-1; /* No DETACH; service must react to EOF. */
    }
    otg_ble_session_destroy(s);close(ipc[1]);
    int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

static void test_async_reports(void)
{
    int fds[2];pair_socket(fds);
    struct session_owner owner={.session={.fd=fds[0],.latency_fd=-1,.alive=true,.ready=true}};
    struct otg_ble_session *s=&owner.session;
    struct message m;
    uint8_t report[8]={2,0,0,0,0,0,0,0};
    CHECK(session_send(s,2,report,8));
    CHECK(receive_message(fds[1],&m)==1 && m.operation==REPORT);
    /* Deliberately withhold the service reply. Input processing must continue
     * and keep only the latest hover/drag, without losing any state edges. */
    for(unsigned i=1;i<=1000;i++) {
        report[2]=(uint8_t)i;report[3]=(uint8_t)(i>>8);
        CHECK(session_send(s,2,report,8));
    }
    CHECK(s->in_flight && s->count==1);
    CHECK(!receive_message(fds[1],&m)); /* No unbounded kernel-side backlog. */
    uint8_t expected[10][8];uint16_t ids[10];unsigned total=0;
#define EXPECT(id) do {ids[total]=id;memcpy(expected[total++],report,8);}while(0)
    EXPECT(2); /* Latest hover, before the press. */
    report[0]=3;report[1]=1;CHECK(session_send(s,2,report,8));EXPECT(2);
    for(unsigned i=1001;i<=2000;i++) {
        report[2]=(uint8_t)i;report[3]=(uint8_t)(i>>8);
        CHECK(session_send(s,2,report,8));
    }
    EXPECT(2); /* Press location must not be overwritten by this drag. */
    report[0]=2;report[1]=0;CHECK(session_send(s,2,report,8));EXPECT(2);
    report[6]=1;CHECK(session_send(s,2,report,8));EXPECT(2);
    CHECK(session_send(s,2,report,8));EXPECT(2); /* Two scroll steps, not one. */
    memset(report,0,8);report[2]=4;CHECK(session_send(s,1,report,8));EXPECT(1);
    report[2]=0;CHECK(session_send(s,1,report,8));EXPECT(1);
    CHECK(session_send(s,2,report,8));EXPECT(2); /* Range exit. */
#undef EXPECT
    CHECK(s->count==total);
    const uint8_t ready[2]={1,0};
    for(unsigned i=0;i<total;i++) {
        CHECK(send_message(fds[1],REPLY,0,ready,2));CHECK(session_pump(s,false));
        CHECK(receive_message(fds[1],&m)==1 && m.operation==REPORT && m.id==ids[i]);
        CHECK(!memcmp(m.data,expected[i],8));
    }
    CHECK(send_message(fds[1],REPLY,0,ready,2));CHECK(session_pump(s,false));
    CHECK(!s->in_flight && !s->count);
    /* Repeated cancellation/tick checks within one status period are local. */
    s->last_request=g_get_monotonic_time();
    CHECK(session_poll(s));CHECK(session_poll(s));
    CHECK(!receive_message(fds[1],&m));
    s->last_request=0;CHECK(session_poll(s));
    CHECK(receive_message(fds[1],&m)==1 && m.operation==POLL_SESSION);
    for(unsigned i=0;i<100;i++)CHECK(session_poll(s));
    CHECK(!receive_message(fds[1],&m));
    report[0]=2;CHECK(session_send(s,2,report,8));
    report[2]=9;CHECK(session_send(s,2,report,8));CHECK(s->count);
    const uint8_t offline[2]={0,0};
    CHECK(send_message(fds[1],REPLY,0,offline,2));CHECK(session_pump(s,false));
    CHECK(!s->ready && !s->count && !s->have_pointer_state);
    s->last_request=0;CHECK(session_pump(s,true));
    CHECK(receive_message(fds[1],&m)==1 && m.operation==POLL_SESSION);
    CHECK(send_message(fds[1],REPLY,0,ready,2));CHECK(session_pump(s,false));
    CHECK(s->ready && !receive_message(fds[1],&m)); /* No stale replay. */
    s->last_request=0;CHECK(session_pump(s,true));s->request_deadline=0;
    CHECK(!session_pump(s,false) && !s->alive && !s->ready);
    close(fds[0]);close(fds[1]);

    pair_socket(fds);
    owner.session=(struct otg_ble_session){.fd=fds[0],.latency_fd=-1,.alive=true,.ready=true};
    CHECK(session_send(s,1,report,8));
    for(unsigned i=0;i<MAX_REPORTS;i++)CHECK(session_send(s,1,report,8));
    CHECK(!session_send(s,1,report,8)); /* Fail closed instead of losing release. */
    CHECK(!s->alive && !s->ready);
    CHECK(receive_message(fds[1],&m)==1);CHECK(receive_message(fds[1],&m)==-1);
    close(fds[0]);close(fds[1]);
}

static void test_service_process(bool legacy_handles)
{
    /* Exercise the actual accept loop/lock/client API in an isolated runtime
     * tree. The backend is still fake; never touch /run/user or system BlueZ. */
    if(!geteuid())return; /* Production explicitly refuses a root daemon. */
    char *tmp=g_dir_make_tmp("otg-ble-session-XXXXXX",NULL);CHECK(tmp);
    runtime_base=tmp;
    char *user_dir=g_strdup_printf("%s/%lu",tmp,(unsigned long)getuid());
    CHECK(!mkdir(user_dir,0700));
    int initial_created=created,initial_activated=activated;
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid) {
        signal(SIGTERM,stop_signal);alarm(10);
        CHECK(otg_ble_service_run("hci0",legacy_handles,&stop_service)==0);
        CHECK(created==initial_created+1 && activated==initial_activated+2);
        CHECK(last_created_legacy_handles==legacy_handles);
        _exit(0);
    }
    bool listening=false;
    for(int i=0;i<200;i++) {
        if(otg_ble_service_wait_idle(getuid(),"hci0")){listening=true;break;}
        g_usleep(10000);
    }
    CHECK(listening);
    CHECK(otg_ble_service_single_idle(getuid(),"hci0"));
    int fd=connect_service(getuid(),"hci0");CHECK(fd>=0);
    struct message status_reply;
    CHECK(request(fd,STATUS_WITH_LAYOUT,0,NULL,0,&status_reply));
    CHECK(status_reply.data[0]==(legacy_handles?STATUS_LEGACY_HANDLES:0) && status_reply.data[1]);
    close(fd);
    CHECK(!otg_ble_session_open("hci0",!legacy_handles));
    for(int session=0;session<2;session++) {
        struct otg_ble_session *client=otg_ble_session_open("hci0",legacy_handles);CHECK(client);
        CHECK(otg_ble_session_set_peer(client,NULL));
        struct message keys=descriptor(1),pointer=descriptor(2);
        CHECK(session_register(client,1,keys.data,keys.size));
        CHECK(session_register(client,2,pointer.data,pointer.size));
        CHECK(session_poll(client) && !session_ready(client));
        CHECK(!otg_ble_session_open("hci0",legacy_handles)); /* Exclusive owner. */
        otg_ble_session_destroy(client);
        CHECK(otg_ble_service_wait_idle(getuid(),"hci0"));
    }
    CHECK(otg_ble_service_status("hci0")==0);
    CHECK(otg_ble_service_stop("hci0")==0);
    int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    char dir[128];struct sockaddr_un address;
    CHECK(socket_path(getuid(),"hci0",dir,sizeof(dir),&address));
    CHECK(access(address.sun_path,F_OK)<0);
    char *lock=g_strdup_printf("%s/ble-hci0.lock",dir);
    CHECK(!unlink(lock));CHECK(!rmdir(dir));CHECK(!rmdir(user_dir));CHECK(!rmdir(tmp));
    g_free(lock);g_free(user_dir);runtime_base="/run/user";g_free(tmp);
}

static void test_multi_process(bool cancel_startup)
{
    if (!geteuid()) return;
    char *dir=g_dir_make_tmp("otg-multi-session-XXXXXX",NULL);CHECK(dir);
    runtime_base=dir;
    char *user=g_strdup_printf("%s/%u",dir,(unsigned)getuid());CHECK(!mkdir(user,0700));
    int gui[2];CHECK(!pipe(gui));
    pid_t pid=fork();CHECK(pid>=0);
    if (!pid) {
        alarm(10);
        close(gui[1]);
        CHECK(dup2(gui[0],STDIN_FILENO)>=0);close(gui[0]);
        CHECK(!otg_ble_multi_service_run("hci0",true,"fixture",-1,true,&stop_service));
        if (!cancel_startup) CHECK(fake_routes[0].sent==1 && fake_routes[1].sent==2);
        _exit(0);
    }
    close(gui[0]);
    for (int i=0;i<200 && !otg_ble_service_wait_idle(getuid(),"hci0");i++) g_usleep(5000);
    struct message status_reply;
    int query=connect_service(getuid(),"hci0");CHECK(query>=0);
    CHECK(request(query,STATUS_WITH_LAYOUT,0,NULL,0,&status_reply));close(query);
    CHECK(status_reply.data[0]==(STATUS_DIRECTED|STATUS_BLE_ONLY|STATUS_LEGACY_HANDLES));
    if (cancel_startup) {
        /* Closing the last waiting GUI session cancels an unused service. */
        close(gui[1]);
    } else {
        const char *a="AA:BB:CC:DD:EE:01", *b="AA:BB:CC:DD:EE:02";
        CHECK(!otg_ble_service_single_idle(getuid(),"hci0"));
        CHECK(!otg_ble_session_open("hci0",true));
        CHECK(!otg_ble_directed_session_open("hci0",false,a,"left"));
        struct otg_ble_session *first=otg_ble_directed_session_open("hci0",true,a,"left");CHECK(first);
        CHECK(!otg_ble_directed_session_open("hci0",true,"aa:bb:cc:dd:ee:01","right"));
        CHECK(!otg_ble_directed_session_open("hci0",true,b,"LEFT"));
        struct otg_ble_session *second=otg_ble_directed_session_open("hci0",true,b,"right");CHECK(second);
        /* Once attached, the IPC sessions own the lifetime even after GUI EOF. */
        close(gui[1]);
        for (unsigned i=1;i<=2;i++) {
            struct message m=descriptor(i);
            CHECK(session_register(first,i,m.data,m.size));
            CHECK(session_register(second,i,m.data,m.size));
        }
        uint8_t keys[8]={0,0,4};
        CHECK(session_rpc(first,POLL_SESSION,0,NULL,0) && first->ready);
        CHECK(session_rpc(first,REPORT,1,keys,8));
        keys[2]=5;CHECK(session_rpc(second,REPORT,1,keys,8));
        close(first->fd);first->fd=-1;otg_ble_session_destroy(first);
        CHECK(otg_ble_service_stop("hci0")!=0);
        CHECK(session_rpc(second,REPORT,1,keys,8) && second->ready);
        otg_ble_session_destroy(second);
    }
    int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    char *service=g_build_filename(user,"deskflow-bridge",NULL);
    char *lock=g_build_filename(service,"ble-hci0.lock",NULL);
    CHECK(!unlink(lock) && !rmdir(service) && !rmdir(user) && !rmdir(dir));
    g_free(lock);g_free(service);g_free(user);g_free(dir);runtime_base="/run/user";
}

static void test_multi_stale_reports(void)
{
    int a[2],b[2];pair_socket(a);pair_socket(b);
    struct otg_ble_route first={.bound=true,.gen=42},second={.bound=true,.gen=9};
    struct directed_service multi={.client={{.fd=a[0],.active=true,.route=&first,.generation=41},
                                           {.fd=b[0],.active=true,.route=&second,.generation=9}}};
    struct service s={.directed=&multi,.clean=true};
    const uint8_t old_key[8]={0,0,30};
    CHECK(send_message(a[1],REPORT,1,old_key,8));
    CHECK(send_message(b[1],POLL_SESSION,0,NULL,0));
    multi_tick(&s);
    struct message reply;
    CHECK(receive_message(a[1],&reply)==1 && valid_reply(&reply) && !reply.data[0]);
    CHECK(receive_message(b[1],&reply)==1 && valid_reply(&reply) && reply.data[0]==STATUS_READY);
    CHECK(!first.sent && multi.client[0].generation==42);
    CHECK(send_message(a[1],POLL_SESSION,0,NULL,0));multi_tick(&s);
    CHECK(receive_message(a[1],&reply)==1 && valid_reply(&reply) && reply.data[0]==STATUS_READY);
    close(a[0]);close(a[1]);close(b[0]);close(b[1]);
}

int main(void)
{
    test_packets();test_sessions();test_client_rpc(false);test_client_rpc(true);
    test_async_reports();test_service_process(false);test_service_process(true);
    test_multi_process(false);test_multi_process(true);
    test_multi_stale_reports();
    puts("PASS: resident GATT sessions, EOF release, retained CCCs, schema/busy/UID guards and real seqpacket framing");
    return 0;
}
