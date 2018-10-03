/*	$OpenBSD$ */

/*
 * Copyright (c) 2013, 2016 Renato Westphal <renato@openbsd.org>
 * Copyright (c) 2009 Michele Marchetto <michele@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <zebra.h>

#include "ldpd.h"
#include "lde.h"
#include "log.h"


#include "mpls.h"

static __inline int	 fec_compare(struct fec *, struct fec *);
static int		 lde_nbr_is_nexthop(struct fec_node *,
			    struct lde_nbr *);
static void		 fec_free(void *);
static struct fec_node	*fec_add(struct fec *fec);
static struct fec_nh	*fec_nh_add(struct fec_node *, int, union ldpd_addr *,
			    uint8_t priority);
static void		 fec_nh_del(struct fec_nh *);

RB_GENERATE(fec_tree, fec, entry, fec_compare)

struct fec_tree		 ft = RB_INITIALIZER(&ft);
struct thread		*gc_timer;

/* FEC tree functions */
void
fec_init(struct fec_tree *fh)
{
	RB_INIT(fh);
}

static __inline int
fec_compare(struct fec *a, struct fec *b)
{
	if (a->type < b->type)
		return (-1);
	if (a->type > b->type)
		return (1);

	switch (a->type) {
	case FEC_TYPE_IPV4:
		if (ntohl(a->u.ipv4.prefix.s_addr) <
		    ntohl(b->u.ipv4.prefix.s_addr))
			return (-1);
		if (ntohl(a->u.ipv4.prefix.s_addr) >
		    ntohl(b->u.ipv4.prefix.s_addr))
			return (1);
		if (a->u.ipv4.prefixlen < b->u.ipv4.prefixlen)
			return (-1);
		if (a->u.ipv4.prefixlen > b->u.ipv4.prefixlen)
			return (1);
		return (0);
	case FEC_TYPE_IPV6:
		if (memcmp(&a->u.ipv6.prefix, &b->u.ipv6.prefix,
		    sizeof(struct in6_addr)) < 0)
			return (-1);
		if (memcmp(&a->u.ipv6.prefix, &b->u.ipv6.prefix,
		    sizeof(struct in6_addr)) > 0)
			return (1);
		if (a->u.ipv6.prefixlen < b->u.ipv6.prefixlen)
			return (-1);
		if (a->u.ipv6.prefixlen > b->u.ipv6.prefixlen)
			return (1);
		return (0);
	case FEC_TYPE_PWID:
		if (a->u.pwid.type < b->u.pwid.type)
			return (-1);
		if (a->u.pwid.type > b->u.pwid.type)
			return (1);
		if (a->u.pwid.pwid < b->u.pwid.pwid)
			return (-1);
		if (a->u.pwid.pwid > b->u.pwid.pwid)
			return (1);
		if (ntohl(a->u.pwid.lsr_id.s_addr) <
		    ntohl(b->u.pwid.lsr_id.s_addr))
			return (-1);
		if (ntohl(a->u.pwid.lsr_id.s_addr) >
		    ntohl(b->u.pwid.lsr_id.s_addr))
			return (1);
		return (0);
	}

	return (-1);
}

struct fec *
fec_find(struct fec_tree *fh, struct fec *f)
{
	return (RB_FIND(fec_tree, fh, f));
}

int
fec_insert(struct fec_tree *fh, struct fec *f)
{
	if (RB_INSERT(fec_tree, fh, f) != NULL)
		return (-1);
	return (0);
}

int
fec_remove(struct fec_tree *fh, struct fec *f)
{
	if (RB_REMOVE(fec_tree, fh, f) == NULL) {
		log_warnx("%s failed for %s", __func__, log_fec(f));
		return (-1);
	}
	return (0);
}

void
fec_clear(struct fec_tree *fh, void (*free_cb)(void *))
{
	struct fec	*f;

	while ((f = RB_ROOT(fh)) != NULL) {
		fec_remove(fh, f);
		free_cb(f);
	}
}

/* routing table functions */
static int
lde_nbr_is_nexthop(struct fec_node *fn, struct lde_nbr *ln)
{
	struct fec_nh		*fnh;

	LIST_FOREACH(fnh, &fn->nexthops, entry)
		if (lde_address_find(ln, fnh->af, &fnh->nexthop))
			return (1);

	return (0);
}

void
rt_dump(pid_t pid)
{
/*
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_map		*me;
	static struct ctl_rt	 rtctl;

	RB_FOREACH(f, fec_tree, &ft) {//遍历路由信息
		fn = (struct fec_node *)f;
		if (fn->local_label == NO_LABEL &&
		    LIST_EMPTY(&fn->downstream))//没有交互的lsr
			continue;

		rtctl.first = 1;
		switch (fn->fec.type) {
		case FEC_TYPE_IPV4:
			rtctl.af = AF_INET;
			rtctl.prefix.v4 = fn->fec.u.ipv4.prefix;
			rtctl.prefixlen = fn->fec.u.ipv4.prefixlen;
			break;
		case FEC_TYPE_IPV6:
			rtctl.af = AF_INET6;
			rtctl.prefix.v6 = fn->fec.u.ipv6.prefix;
			rtctl.prefixlen = fn->fec.u.ipv6.prefixlen;
			break;
		default:
			continue;
		}

		rtctl.local_label = fn->local_label;
		LIST_FOREACH(me, &fn->downstream, entry) {//遍历收到的map
			rtctl.in_use = lde_nbr_is_nexthop(fn, me->nexthop);
			rtctl.nexthop = me->nexthop->id;
			rtctl.remote_label = me->map.label;

			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_LIB, 0, pid,
			    &rtctl, sizeof(rtctl));
			rtctl.first = 0;
		}
		if (LIST_EMPTY(&fn->downstream)) {
			rtctl.in_use = 0;
			rtctl.nexthop.s_addr = INADDR_ANY;
			rtctl.remote_label = NO_LABEL;

			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_LIB, 0, pid,
			    &rtctl, sizeof(rtctl));
		}
	}

*/

	struct label_nbr *lnb;
	//struct fec_node 	*fn;
	struct lde_nbr	*ln;
	static struct ctl_rt rtctl;
	RB_FOREACH(lnb, label_nbr_tree, &label_nbrs){
		rtctl.first = 1;
		switch (lnb->fec.type){
			case FEC_TYPE_IPV4:
			rtctl.af = AF_INET;
			rtctl.prefix.v4 = lnb->fec.u.ipv4.prefix;
			rtctl.prefixlen = lnb->fec.u.ipv4.prefixlen;
			break;
		case FEC_TYPE_IPV6:
			rtctl.af = AF_INET6;
			rtctl.prefix.v6 = lnb->fec.u.ipv6.prefix;
			rtctl.prefixlen = lnb->fec.u.ipv6.prefixlen;
			break;
		default:
			continue;
		}
		rtctl.local_label=lnb->local_label;
		rtctl.remote_label=lnb->label;
		ln=lde_nbr_find(lnb->peerid);
		rtctl.nexthop=ln->id;
		if(lnb->type==STREAM_TYPE_UP)rtctl.in_use=1;
		else rtctl.in_use=0;		
		lde_imsg_compose_ldpe(IMSG_CTL_SHOW_LIB, 0, pid,
			    &rtctl, sizeof(rtctl));
		rtctl.first = 0;
	}
	
}

void
fec_snap(struct lde_nbr *ln)
{
	struct fec	*f;
	struct fec_node	*fn;

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		if (fn->local_label == NO_LABEL)
			continue;

		lde_send_labelmapping(ln, fn, 0);
	}

	lde_imsg_compose_ldpe(IMSG_MAPPING_ADD_END, ln->peerid, 0, NULL, 0);
}

static void
fec_free(void *arg)
{
	struct fec_node	*fn = arg;
	struct fec_nh	*fnh;

	while ((fnh = LIST_FIRST(&fn->nexthops)))
		fec_nh_del(fnh);
	if (!LIST_EMPTY(&fn->downstream))
		log_warnx("%s: fec %s downstream list not empty", __func__,
		    log_fec(&fn->fec));
	if (!LIST_EMPTY(&fn->upstream))
		log_warnx("%s: fec %s upstream list not empty", __func__,
		    log_fec(&fn->fec));

	free(fn);
}

void
fec_tree_clear(void)
{
	fec_clear(&ft, fec_free);
}

static struct fec_node *
fec_add(struct fec *fec)
{
	struct fec_node	*fn;

	fn = calloc(1, sizeof(*fn));
	if (fn == NULL)
		fatal(__func__);

	fn->fec = *fec;
	fn->local_label = NO_LABEL;
	LIST_INIT(&fn->upstream);
	LIST_INIT(&fn->downstream);
	LIST_INIT(&fn->nexthops);

	if (fec_insert(&ft, &fn->fec))
		log_warnx("failed to add %s to ft tree",
		    log_fec(&fn->fec));

	return (fn);
}

struct fec_nh *
fec_nh_find(struct fec_node *fn, int af, union ldpd_addr *nexthop,
    uint8_t priority)
{
	struct fec_nh	*fnh;

	LIST_FOREACH(fnh, &fn->nexthops, entry)
		if (fnh->af == af &&
		    ldp_addrcmp(af, &fnh->nexthop, nexthop) == 0 &&
		    fnh->priority == priority)
			return (fnh);

	return (NULL);
}

static struct fec_nh *
fec_nh_add(struct fec_node *fn, int af, union ldpd_addr *nexthop,
    uint8_t priority)
{
	struct fec_nh	*fnh;

	fnh = calloc(1, sizeof(*fnh));
	if (fnh == NULL)
		fatal(__func__);

	fnh->af = af;
	fnh->nexthop = *nexthop;
	fnh->remote_label = NO_LABEL;
	fnh->priority = priority;
	LIST_INSERT_HEAD(&fn->nexthops, fnh, entry);

	return (fnh);
}

static void
fec_nh_del(struct fec_nh *fnh)
{
	LIST_REMOVE(fnh, entry);
	free(fnh);
}

uint32_t
egress_label(enum fec_type fec_type)
{
	switch (fec_type) {
	case FEC_TYPE_IPV4:
		if (ldeconf->ipv4.flags & F_LDPD_AF_EXPNULL)
			return (MPLS_LABEL_IPV4NULL);
		break;
	case FEC_TYPE_IPV6:
		if (ldeconf->ipv6.flags & F_LDPD_AF_EXPNULL)
			return (MPLS_LABEL_IPV6NULL);
		break;
	default:
		fatalx("egress_label: unexpected fec type");
	}

	return (MPLS_LABEL_IMPLNULL);
}

//建立fec_node信息
void
lde_kernel_insert(struct fec *fec, int af, union ldpd_addr *nexthop,
    uint8_t priority, int connected, void *data)
{
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_map		*me;
	struct lde_nbr		*ln;
	////////////////////////////////////////////
	struct fec_node		*fn1;
	struct fec          *fec1;
	struct lde_nbr		*ln1;
	struct Information	info;
	///////////////////////////////////////////////
    printf("kernel_insert start\n");
//	leaf = 0;
	fn = (struct fec_node *)fec_find(&ft, fec);//查找本地fec_tree（fec_node_tree），fn=NULL说明是新加入的路由信息，新增一个fec_node
	if (fn == NULL)
		fn = fec_add(fec);//初始建立fec_node，local_label=NO_LABEL
	if (fec_nh_find(fn, af, nexthop, priority) != NULL)
		return;

   //fec_node中下一跳信息不匹配
	if (fn->fec.type == FEC_TYPE_PWID)
		fn->data = data;

/*	if (fn->local_label == NO_LABEL) {//分配local_label
		if (connected)
			fn->local_label = egress_label(fn->fec.type);
		else
			fn->local_label = lde_assign_label();
*/
		/* FEC.1: perform lsr label distribution procedure */
		//向邻居通报新增的fec_node信息
	/*	if(fn->fec.u.ipv4.type){//当其为叶子节点时，向上发送labelmapping
		RB_FOREACH(ln, nbr_tree, &lde_nbrs) 
			
			lde_send_labelmapping(ln, fn, 1);
			}
		}*/
	//nexthop信息加入，remote_label初始为NO_LABEL,通报变化信息
	fnh = fec_nh_add(fn, af, nexthop, priority);//该fec与谁（nexthop）直连
	printf("%s\n",inet_ntoa(fec->u.ipv4.prefix));
	printf("nexthop:%s\n",inet_ntoa(fnh->nexthop.v4));
	lde_send_change_klabel(fn, fnh);
	
///////////////////////////////////////////////////////////////////////////////////
	/*info=init_info();
	printf("%s\n",inet_ntoa(fec->u.ipv4.prefix));
	if(info.leaf&&info.root_addr.s_addr==fec->u.ipv4.prefix.s_addr){//root 路由加入，本节点为叶子节点，判断与下一跳的会话是否建立
		switch (fn->fec.type) {
			case FEC_TYPE_IPV4:
			case FEC_TYPE_IPV6:
				ln1 = lde_nbr_find_by_addr(af, &fnh->nexthop);//寻址
			case FEC_TYPE_PWID:
				ln1 = lde_nbr_find_by_lsrid(fn->fec.u.pwid.lsr_id);
				break;
			default:
				ln1 = NULL;
				break;
		}	
		if(ln1!=NULL){//与下一跳会话建立，构造fec_node，发送labelmapping
			fec1 = calloc(1, sizeof(*fec1));
			if (fec1 == NULL)
				fatal(__func__);
			fec1->type=fn->fec.type;
			if(fec1->type==FEC_TYPE_IPV4){
				fec1->u.ipv4.prefix=info.root_addr;
				fec1->u.ipv4.prefixlen=info.ov;
			}
			else{
				free(fec1);
				return;
				} 
			if(fec_find(&ln1->sent_map, fec1)==NULL){
				fn1 = calloc(1, sizeof(*fn1));
				if (fn1 == NULL)
					fatal(__func__);
				fn1->fec = *fec1;
				fn1->local_label =lde_assign_label();
				LIST_INIT(&fn1->upstream);
				LIST_INIT(&fn1->downstream);
				LIST_INIT(&fn1->nexthops);
				label_nbr_add(NO_LABEL,fn1->local_label, ln1->peerid, fec1, STREAM_TYPE_UP);
				lde_send_labelmapping(ln1, fn1, 1);	
				free(fn1);
			}
			free(fec1);
		}
	}*/
///////////////////////////////////////////////////////////////////////////////////
	switch (fn->fec.type) {
	case FEC_TYPE_IPV4:
	case FEC_TYPE_IPV6:
		ln = lde_nbr_find_by_addr(af, &fnh->nexthop);//寻址
	case FEC_TYPE_PWID:
		ln = lde_nbr_find_by_lsrid(fn->fec.u.pwid.lsr_id);
		break;
	default:
		ln = NULL;
		break;
	}

	if (ln) {
		/* FEC.2  */
	
		me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
		if (me)//收到过，处理信息
			/* FEC.5 */
			lde_check_mapping(&me->map, ln);
	}
}

void 
lde_kernel_remove(struct fec *fec, int af, union ldpd_addr *nexthop,
    uint8_t priority)
{
	struct fec_node		*fn;
	struct fec_nh		*fnh;

	fn = (struct fec_node *)fec_find(&ft, fec);
	if (fn == NULL)
		/* route lost */
		return;
	fnh = fec_nh_find(fn, af, nexthop, priority);
	if (fnh == NULL)
		/* route lost */
		return;

	lde_send_delete_klabel(fn, fnh);
	fec_nh_del(fnh);
	if (LIST_EMPTY(&fn->nexthops)) {
		lde_send_labelwithdraw_all(fn, NO_LABEL);
		fn->local_label = NO_LABEL;
		if (fn->fec.type == FEC_TYPE_PWID)
			fn->data = NULL;
	}
}

void
lde_check_mapping(struct map *map, struct lde_nbr *ln)
{
	struct fec		 fec;
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_req		*lre;
	struct lde_map		*me;
	struct l2vpn_pw		*pw;
	struct ldpd_global   lg;//读取本机ip
	int			 msgsource = 0;

	//提取收到的map信息中的fec信息，找到相应的fec_node。如果fec_node不存在则新建一个fec_node

	lde_map2fec(map, ln->id, &fec);
//	if(!lde_nbr_find_by_addr(fec.type,fec.u.ipv4.prefix))//新添的路由信息不是本机的邻居节点，只为邻居建立fec_node_tree
	//	return;
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL)
		fn = fec_add(&fec);

	/* LMp.1: first check if we have a pending request running */
	lre = (struct lde_req *)fec_find(&ln->sent_req, &fn->fec);
	if (lre)//lre！=NULL说明发送的请求得到了回应，把相应请求记录删除
		/* LMp.2: delete record of outstanding label request */
		lde_req_del(ln, lre, 1);

	/* RFC 4447 control word and status tlv negotiation */
	if (map->type == MAP_TYPE_PWID && l2vpn_pw_negotiate(ln, fn, map))
		return;

	/*
	 * LMp.3 - LMp.8: loop detection - unnecessary for frame-mode
	 * mpls networks.
	 */

	/* LMp.9 */
	me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
	if (me) {
		/* LMp.10 */
		if (me->map.label != map->label && lre == NULL) {
			/* LMp.10a */
			lde_send_labelrelease(ln, fn, me->map.label);//新收到的label与以前的label对比，如果不一样更新label

			/*
			 * Can not use lde_nbr_find_by_addr() because there's
			 * the possibility of multipath.
			 */
			LIST_FOREACH(fnh, &fn->nexthops, entry) {
				if (lde_address_find(ln, fnh->af,
				    &fnh->nexthop) == NULL)
					continue;

				lde_send_delete_klabel(fn, fnh);
				fnh->remote_label = NO_LABEL;
			}
		}
	}

	/*
	 * LMp.11 - 12: consider multiple nexthops in order to
	 * support multipath
	 */
	 //分发remote_label
	LIST_FOREACH(fnh, &fn->nexthops, entry) {
		/* LMp.15: install FEC in FIB */
		switch (fec.type) {
		case FEC_TYPE_IPV4:
		case FEC_TYPE_IPV6:
			if (!lde_address_find(ln, fnh->af, &fnh->nexthop))
				continue;

			fnh->remote_label = map->label;
			lde_send_change_klabel(fn, fnh);
			break;
		case FEC_TYPE_PWID:
			pw = (struct l2vpn_pw *) fn->data;
			if (pw == NULL)
				continue;

			pw->remote_group = map->fec.pwid.group_id;
			if (map->flags & F_MAP_PW_IFMTU)
				pw->remote_mtu = map->fec.pwid.ifmtu;
			if (map->flags & F_MAP_PW_STATUS)
				pw->remote_status = map->pw_status;
			fnh->remote_label = map->label;
			if (l2vpn_pw_ok(pw, fnh))
				lde_send_change_klabel(fn, fnh);
			break;
		default:
			break;
		}

		msgsource = 1;
	}
	/* LMp.13 & LMp.16: Record the mapping from this peer */
	if (me == NULL)
		me = lde_map_add(ln, fn, 0);
	me->map = *map;

	if (msgsource == 0)
		/* LMp.13: just return since we use liberal lbl retention */
		return;
	/*
	 * LMp.17 - LMp.27 are unnecessary since we don't need to implement
	 * loop detection. LMp.28 - LMp.30 are unnecessary because we are
	 * merging capable.
	 */
}

void
lde_check_request(struct map *map, struct lde_nbr *ln)
{
	struct fec	 fec;
	struct lde_req	*lre;
	struct fec_node	*fn;
	struct fec_nh	*fnh;

	/* LRq.1: skip loop detection (not necessary) */

	/* LRq.2: is there a next hop for fec? */
	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL || LIST_EMPTY(&fn->nexthops)) {
		/* LRq.5: send No Route notification */
		lde_send_notification(ln->peerid, S_NO_ROUTE, map->msg_id,
		    htons(MSG_TYPE_LABELREQUEST));
		return;
	}

	/* LRq.3: is MsgSource the next hop? */
	LIST_FOREACH(fnh, &fn->nexthops, entry) {
		switch (fec.type) {
		case FEC_TYPE_IPV4:
		case FEC_TYPE_IPV6:
			if (!lde_address_find(ln, fnh->af, &fnh->nexthop))
				continue;

			/* LRq.4: send Loop Detected notification */
			lde_send_notification(ln->peerid, S_LOOP_DETECTED,
			    map->msg_id, htons(MSG_TYPE_LABELREQUEST));
			return;
		default:
			break;
		}
	}

	/* LRq.6: first check if we have a pending request running */
	lre = (struct lde_req *)fec_find(&ln->recv_req, &fn->fec);
	if (lre != NULL)
		/* LRq.7: duplicate request */
		return;

	/* LRq.8: record label request */
	lre = lde_req_add(ln, &fn->fec, 0);
	if (lre != NULL)
		lre->msg_id = ntohl(map->msg_id);

	/* LRq.9: perform LSR label distribution */
	lde_send_labelmapping(ln, fn, 1);

	/*
	 * LRq.10: do nothing (Request Never) since we use liberal
	 * label retention.
	 * LRq.11 - 12 are unnecessary since we are merging capable.
	 */
}

void
lde_check_release(struct map *map, struct lde_nbr *ln)
{
	struct fec		 fec;
	struct fec_node		*fn;
	struct lde_wdraw	*lw;
	struct lde_map		*me;

	/* TODO group wildcard */
	if (map->type == MAP_TYPE_PWID && !(map->flags & F_MAP_PW_ID))
		return;

	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	/* LRl.1: does FEC match a known FEC? */
	if (fn == NULL)
		return;

	/* LRl.3: first check if we have a pending withdraw running */
	lw = (struct lde_wdraw *)fec_find(&ln->sent_wdraw, &fn->fec);
	if (lw && (map->label == NO_LABEL ||
	    (lw->label != NO_LABEL && map->label == lw->label))) {
		/* LRl.4: delete record of outstanding label withdraw */
		lde_wdraw_del(ln, lw);
	}

	/* LRl.6: check sent map list and remove it if available */
	me = (struct lde_map *)fec_find(&ln->sent_map, &fn->fec);
	if (me && (map->label == NO_LABEL || map->label == me->map.label))
		lde_map_del(ln, me, 1);

	/*
	 * LRl.11 - 13 are unnecessary since we remove the label from
	 * forwarding/switching as soon as the FEC is unreachable.
	 */
}

void
lde_check_release_wcard(struct map *map, struct lde_nbr *ln)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_wdraw	*lw;
	struct lde_map		*me;

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;

		/* LRl.3: first check if we have a pending withdraw running */
		lw = (struct lde_wdraw *)fec_find(&ln->sent_wdraw, &fn->fec);
		if (lw && (map->label == NO_LABEL ||
		    (lw->label != NO_LABEL && map->label == lw->label))) {
			/* LRl.4: delete record of outstanding lbl withdraw */
			lde_wdraw_del(ln, lw);
		}

		/* LRl.6: check sent map list and remove it if available */
		me = (struct lde_map *)fec_find(&ln->sent_map, &fn->fec);
		if (me &&
		    (map->label == NO_LABEL || map->label == me->map.label))
			lde_map_del(ln, me, 1);

		/*
		 * LRl.11 - 13 are unnecessary since we remove the label from
		 * forwarding/switching as soon as the FEC is unreachable.
		 */
	}
}

void
lde_check_withdraw(struct map *map, struct lde_nbr *ln)
{
	struct fec		 fec;
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_map		*me;
	struct l2vpn_pw		*pw;

	/* TODO group wildcard */
	if (map->type == MAP_TYPE_PWID && !(map->flags & F_MAP_PW_ID))
		return;

	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL)
		fn = fec_add(&fec);

	/* LWd.1: remove label from forwarding/switching use */
	LIST_FOREACH(fnh, &fn->nexthops, entry) {
		switch (fec.type) {
		case FEC_TYPE_IPV4:
		case FEC_TYPE_IPV6:
			if (!lde_address_find(ln, fnh->af, &fnh->nexthop))
				continue;
			break;
		case FEC_TYPE_PWID:
			pw = (struct l2vpn_pw *) fn->data;
			if (pw == NULL)
				continue;
			break;
		default:
			break;
		}
		lde_send_delete_klabel(fn, fnh);
		fnh->remote_label = NO_LABEL;
	}

	/* LWd.2: send label release */
	lde_send_labelrelease(ln, fn, map->label);

	/* LWd.3: check previously received label mapping */
	me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
	if (me && (map->label == NO_LABEL || map->label == me->map.label))
		/* LWd.4: remove record of previously received lbl mapping */
		lde_map_del(ln, me, 0);
}

void
lde_check_withdraw_wcard(struct map *map, struct lde_nbr *ln)
{
	struct fec	*f;
	struct fec_node	*fn;
	struct fec_nh	*fnh;
	struct lde_map	*me;
	struct in_addr local;
	local.s_addr=inet_addr("0.0.0.0");
	/* LWd.2: send label release */
	printf("a\n");
	lde_send_labelrelease(ln, NULL, map->label);
	printf("b\n");

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		printf("c addr:%s\n ",inet_ntoa(f->u.ipv4.prefix));
	//	LIST_FOREACH(fnh, &fn->nexthops, entry)
		//	if(fnh->nexthop.v4==local)
			//	break;
		//if(f->u.ipv4.prefix.s_addr==local.s_addr)
			//continue;
		/* LWd.1: remove label from forwarding/switching use */
	/*	LIST_FOREACH(fnh, &fn->nexthops, entry) {
			switch (f->type) {
			case FEC_TYPE_IPV4:
			case FEC_TYPE_IPV6:
				if (!lde_address_find(ln, fnh->af, &fnh->nexthop))
					continue;
				break;
			case FEC_TYPE_PWID:
				if (f->u.pwid.lsr_id.s_addr != ln->id.s_addr)
					continue;
				break;
			default:
				break;
			}
			lde_send_delete_klabel(fn, fnh);
			printf("d\n");
			fnh->remote_label = NO_LABEL;
		}*/

		/* LWd.3: check previously received label mapping */
		me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
		if (me && (map->label == NO_LABEL || map->label == me->map.label))
			/*
			 * LWd.4: remove record of previously received
			 * label mapping
			 */
			lde_map_del(ln, me, 0);
		printf("e\n");
	}
}

/* gabage collector timer: timer to remove dead entries from the LIB */

/* ARGSUSED */
int
lde_gc_timer(struct thread *thread)
{
	struct fec	*fec, *safe;
	struct fec_node	*fn;
	int		 count = 0;

	RB_FOREACH_SAFE(fec, fec_tree, &ft, safe) {
		fn = (struct fec_node *) fec;

		if (!LIST_EMPTY(&fn->nexthops) ||
		    !LIST_EMPTY(&fn->downstream) ||
		    !LIST_EMPTY(&fn->upstream))
			continue;

		fec_remove(&ft, &fn->fec);
		free(fn);
		count++;
	}

	if (count > 0)
		log_debug("%s: %u entries removed", __func__, count);

	lde_gc_start_timer();

	return (0);
}

void
lde_gc_start_timer(void)
{
	THREAD_TIMER_OFF(gc_timer);
	gc_timer = thread_add_timer(master, lde_gc_timer, NULL,
	    LDE_GC_INTERVAL);
}

void
lde_gc_stop_timer(void)
{
	THREAD_TIMER_OFF(gc_timer);
}
