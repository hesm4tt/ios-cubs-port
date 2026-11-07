/* Read-only nl80211 scan-cache probe. Prints counts, never network identifiers. */
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define ALIGN4(n) (((n)+3u)&~3u)
struct request { struct nlmsghdr n; struct genlmsghdr g; unsigned char data[256]; };
static int addattr(struct request *r, unsigned type, const void *p, unsigned len) {
    unsigned off=NLMSG_ALIGN(r->n.nlmsg_len), size=NLA_HDRLEN+len;
    if (off+ALIGN4(size)>sizeof(*r)) return -1;
    struct nlattr *a=(struct nlattr *)((char *)r+off);
    a->nla_type=(uint16_t)type; a->nla_len=(uint16_t)size;
    memcpy((char *)a+NLA_HDRLEN,p,len);
    r->n.nlmsg_len=off+ALIGN4(size);
    return 0;
}
static int sendreq(int fd, struct request *r) { return send(fd,r,r->n.nlmsg_len,0)==(ssize_t)r->n.nlmsg_len ? 0 : -1; }
static int family(int fd) {
    struct request r={0}; char buf[8192]; const char name[]="nl80211";
    r.n.nlmsg_len=NLMSG_LENGTH(GENL_HDRLEN); r.n.nlmsg_type=GENL_ID_CTRL; r.n.nlmsg_flags=NLM_F_REQUEST; r.n.nlmsg_seq=1;
    r.g.cmd=CTRL_CMD_GETFAMILY; r.g.version=1;
    if (addattr(&r,CTRL_ATTR_FAMILY_NAME,name,sizeof(name)) || sendreq(fd,&r)) return -1;
    ssize_t n=recv(fd,buf,sizeof(buf),0); if (n<0) return -1;
    for (struct nlmsghdr *h=(void *)buf; NLMSG_OK(h,n); h=NLMSG_NEXT(h,n)) {
        if (h->nlmsg_type==NLMSG_ERROR) return -1;
        if (h->nlmsg_len<NLMSG_LENGTH(GENL_HDRLEN)) continue;
        int left=(int)h->nlmsg_len-NLMSG_LENGTH(GENL_HDRLEN);
        for (struct nlattr *a=(void *)((char *)NLMSG_DATA(h)+GENL_HDRLEN); left>=(int)NLA_HDRLEN && a->nla_len>=NLA_HDRLEN && a->nla_len<=left; ) {
            if ((a->nla_type & NLA_TYPE_MASK)==CTRL_ATTR_FAMILY_ID && a->nla_len>=NLA_HDRLEN+2) { uint16_t v; memcpy(&v,(char *)a+NLA_HDRLEN,2); return v; }
            int step=(int)ALIGN4(a->nla_len); left-=step; a=(void *)((char *)a+step);
        }
    }
    return -1;
}
static unsigned interval_min=65536, interval_max, nonzero;
static int count_bss(const struct nlattr *outer, unsigned *intervals) {
    int left=outer->nla_len-NLA_HDRLEN, has_interval=0;
    for (struct nlattr *a=(void *)((char *)outer+NLA_HDRLEN); left>=(int)NLA_HDRLEN && a->nla_len>=NLA_HDRLEN && a->nla_len<=left; ) {
        if ((a->nla_type & NLA_TYPE_MASK)==NL80211_BSS_BEACON_INTERVAL && a->nla_len>=NLA_HDRLEN+2) {
            uint16_t value; memcpy(&value,(char *)a+NLA_HDRLEN,2); has_interval=1;
            if (value) { nonzero++; if (value<interval_min) interval_min=value; if (value>interval_max) interval_max=value; }
        }
        int step=(int)ALIGN4(a->nla_len); left-=step; a=(void *)((char *)a+step);
    }
    if (has_interval) (*intervals)++;
    return 1;
}
int main(void) {
    int fd=socket(AF_NETLINK,SOCK_RAW,NETLINK_GENERIC); unsigned index=if_nametoindex("wlan0");
    if (fd<0 || !index) { perror("socket/interface"); return 1; }
    struct timeval timeout={.tv_sec=5}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    int id=family(fd); if (id<0) { perror("nl80211 family"); return 1; }
    struct request r={0}; r.n.nlmsg_len=NLMSG_LENGTH(GENL_HDRLEN); r.n.nlmsg_type=(uint16_t)id;
    r.n.nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP; r.n.nlmsg_seq=2; r.g.cmd=NL80211_CMD_GET_SCAN;
    if (addattr(&r,NL80211_ATTR_IFINDEX,&index,sizeof(index)) || sendreq(fd,&r)) { perror("scan request"); return 1; }
    unsigned bss=0, intervals=0; char buf[65536];
    for (;;) {
        ssize_t n=recv(fd,buf,sizeof(buf),0); if (n<0) { perror("scan response"); return 1; }
        for (struct nlmsghdr *h=(void *)buf; NLMSG_OK(h,n); h=NLMSG_NEXT(h,n)) {
            if (h->nlmsg_type==NLMSG_DONE) { printf("scan entries=%u with-beacon-interval=%u nonzero=%u min=%u max=%u\n",bss,intervals,nonzero,nonzero?interval_min:0,interval_max); return 0; }
            if (h->nlmsg_type==NLMSG_ERROR) { struct nlmsgerr *e=NLMSG_DATA(h); fprintf(stderr,"scan netlink error=%d\n",e->error); return 1; }
            if (h->nlmsg_len<NLMSG_LENGTH(GENL_HDRLEN)) continue;
            int left=(int)h->nlmsg_len-NLMSG_LENGTH(GENL_HDRLEN);
            for (struct nlattr *a=(void *)((char *)NLMSG_DATA(h)+GENL_HDRLEN); left>=(int)NLA_HDRLEN && a->nla_len>=NLA_HDRLEN && a->nla_len<=left; ) {
                if ((a->nla_type & NLA_TYPE_MASK)==NL80211_ATTR_BSS) bss+=(unsigned)count_bss(a,&intervals);
                int step=(int)ALIGN4(a->nla_len); left-=step; a=(void *)((char *)a+step);
            }
        }
    }
}
