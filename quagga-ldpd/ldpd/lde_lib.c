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
static struct fec_nh	*fec_nh_add(struct fec_node *, int, union ldpd_addr *,
			    uint8_t priority);
static void		 fec_nh_del(struct fec_nh *);
static struct fec_node	*fec_add(struct fec *fec); 

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
mp2mp_uscb_dump(pid_t pid)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_map		*me;
	static struct ctl_rt	 rtctl;

    printf("%s, pid: %d\n", __func__, pid);
	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		if (fn->local_label == NO_LABEL &&
            LIST_EMPTY(&fn->upstream))
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

		rtctl.local_label = fn->local_label; //不予显示
		LIST_FOREACH(me, &fn->upstream, entry) {
            if (me->map.type != MAP_TYPE_MP2MP_UP && me->map.type != MAP_TYPE_MP2MP_DOWN) continue;
			rtctl.in_use = lde_nbr_is_nexthop(fn, me->nexthop);
			rtctl.nexthop = me->nexthop->id;
			rtctl.remote_label = me->map.label;
            if (me->map.type == MAP_TYPE_MP2MP_UP)  rtctl.mp2mp_flags |= U_MAPPING_IN;
            if (me->map.type == MAP_TYPE_MP2MP_DOWN)  rtctl.mp2mp_flags |= D_MAPPING_IN;
            if (fn->data != NULL)  rtctl.mp2mp_flags |= FEC_MP2MP_EXT(fn)->mbb_flag;

            if (fn->data != NULL) printf("%s, mbb_flag: %u\n", __func__, FEC_MP2MP_EXT(fn)->mbb_flag);
			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_MP2MP_USCB, 0, pid,
			    &rtctl, sizeof(rtctl));
			rtctl.first = 0;
            rtctl.mp2mp_flags = 0;
		}
		
        if (LIST_EMPTY(&fn->upstream)) {
			rtctl.in_use = 0;
			rtctl.nexthop.s_addr = INADDR_ANY;
			rtctl.remote_label = NO_LABEL;
            rtctl.flags = 0;
            rtctl.mp2mp_flags = 0;

			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_MP2MP_USCB, 0, pid,
			    &rtctl, sizeof(rtctl));
		}
	}

}

void
mp2mp_dscb_dump(pid_t pid)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_map		*me;
	static struct ctl_rt	 rtctl;

    printf("%s, pid: %d\n", __func__, pid);
	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		if (fn->local_label == NO_LABEL &&
		    LIST_EMPTY(&fn->downstream))
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
		
        rtctl.local_label = fn->local_label; //不予显示
		LIST_FOREACH(me, &fn->downstream, entry) {
            if (me->map.type != MAP_TYPE_MP2MP_UP && me->map.type != MAP_TYPE_MP2MP_DOWN) continue;
			rtctl.in_use = lde_nbr_is_nexthop(fn, me->nexthop);
			rtctl.nexthop = me->nexthop->id;
			rtctl.remote_label = me->map.label;
            if (me->map.type == MAP_TYPE_MP2MP_UP)  rtctl.mp2mp_flags |= U_MAPPING_IN;
            if (me->map.type == MAP_TYPE_MP2MP_DOWN)  rtctl.mp2mp_flags |= D_MAPPING_IN;
            if (fn->data != NULL)  rtctl.mp2mp_flags |= FEC_MP2MP_EXT(fn)->mbb_flag;
            printf("%s, rtctl.prefix.v4: %s, rtctl.prefixlen: %u\n", __func__, inet_ntoa(rtctl.prefix.v4), rtctl.prefixlen);
            if (fn->data != NULL)  printf("%s, mbb_flag: %u\n", __func__, FEC_MP2MP_EXT(fn)->mbb_flag);
			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_MP2MP_DSCB, 0, pid,
			    &rtctl, sizeof(rtctl));
			rtctl.first = 0;
            rtctl.mp2mp_flags = 0;
		}
		if (LIST_EMPTY(&fn->downstream)) {
			rtctl.in_use = 0;
			rtctl.nexthop.s_addr = INADDR_ANY;
			rtctl.remote_label = NO_LABEL;
            rtctl.flags = 0;
            rtctl.mp2mp_flags = 0;

			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_MP2MP_DSCB, 0, pid,
			    &rtctl, sizeof(rtctl));
		}
	}

}

void
mp2mp_lsp_dump(pid_t pid)
{
    struct fec *f;
    struct fec_node *fn;

    RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
        printf("%s, fec: %s, fec.type: %d, fn->data: %p\n", __func__, inet_ntoa(f->u.ipv4.prefix), fn->fec.type, fn->data);
        struct fec_nh *fnh = NULL;
        LIST_FOREACH(fnh, &fn->nexthops, entry) {
            printf("%s, fnh->nexthop: %s, fnh->remote_label: %d, fnh->priority: %u\n",
                    __func__, inet_ntoa(fnh->nexthop.v4), fnh->remote_label, fnh->priority);
        }
    }

}

void
mp2mp_insegment_dump(pid_t pid)
{
    printf("%s\n", __func__);
}

void
mp2mp_outsegment_dump(pid_t pid)
{
    printf("%s\n", __func__);
}

void
rt_dump(pid_t pid)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_map		*me;
	static struct ctl_rt	 rtctl;

    printf("%s, pid: %d\n", __func__, pid);
	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		if (fn->local_label == NO_LABEL &&
		    LIST_EMPTY(&fn->downstream))
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
		LIST_FOREACH(me, &fn->downstream, entry) {
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
    
    //add for mp2mp, free fec_mp2mp_ext
    if (fn->data != NULL) {
        free(fn->data);
        fn->data = NULL;
    }

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

    log_notice("%s, fec->prefix: %s, fec->prefixlen: %u", __func__, inet_ntoa(fec->u.ipv4.prefix), fec->u.ipv4.prefixlen);

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
    printf("%s, fn->fec.u.ipv4.prefix: %s\n",
            __func__, inet_ntoa(fn->fec.u.ipv4.prefix));
	fnh = calloc(1, sizeof(*fnh));
	if (fnh == NULL)
		fatal(__func__);

	fnh->af = af;
	fnh->nexthop = *nexthop;
	fnh->remote_label = NO_LABEL;
	fnh->priority = priority;
	LIST_INSERT_HEAD(&fn->nexthops, fnh, entry);

    struct fec_nh *fnh1 = NULL;
    LIST_FOREACH(fnh1, &fn->nexthops, entry) {
        printf("%s, fnh->nexthop: %s, fnh->remote_label: %d, fnh->priority: %u\n",
                __func__, inet_ntoa(fnh1->nexthop.v4), fnh1->remote_label, fnh1->priority);
    }
 
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

void
lde_kernel_insert(struct fec *fec, int af, union ldpd_addr *nexthop,
    uint8_t priority, int connected, void *data)
{
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_map		*me;
	struct lde_nbr		*ln;

    printf("%s, fec->u.ipv4.prefix: %s, af: %d, nexthop: %s, priority: %u, connected: %d\n",
            __func__, inet_ntoa(fec->u.ipv4.prefix), af, inet_ntoa(nexthop->v4), priority, connected);
	fn = (struct fec_node *)fec_find(&ft, fec);
	if (fn == NULL)
		fn = fec_add(fec);
	if (fec_nh_find(fn, af, nexthop, priority) != NULL)
		return;

	if (fn->fec.type == FEC_TYPE_PWID)
		fn->data = data;

	if (fn->local_label == NO_LABEL) {
		if (connected)
			fn->local_label = egress_label(fn->fec.type);
		else
			fn->local_label = lde_assign_label();

		/* FEC.1: perform lsr label distribution procedure */
		RB_FOREACH(ln, nbr_tree, &lde_nbrs)
			lde_send_labelmapping(ln, fn, 1);
	}

	fnh = fec_nh_add(fn, af, nexthop, priority);
	lde_send_change_klabel(fn, fnh);

	switch (fn->fec.type) {
	case FEC_TYPE_IPV4:
	case FEC_TYPE_IPV6:
		ln = lde_nbr_find_by_addr(af, &fnh->nexthop);
		break;
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
		if (me)
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
	int			 msgsource = 0;

    printf("%s, ln->id: %s, map->type: %d\n", __func__, inet_ntoa(ln->id), map->type);
	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL)
		fn = fec_mp2mp_add(&fec);
    else if (fn->data == NULL && (map->type == MAP_TYPE_MP2MP_UP || map->type == MAP_TYPE_MP2MP_DOWN)) {
        fn->data = calloc(1, sizeof(struct fec_mp2mp_ext));
        if (fn->data == NULL) {
            fatal(__func__);
            return;    
        }
        FEC_MP2MP_EXT(fn)->mbb_flag |= SEND_MAPPING;    
        FEC_MP2MP_EXT(fn)->hold_time = 5;
        FEC_MP2MP_EXT(fn)->switch_delay_time = 5;
        FEC_MP2MP_EXT(fn)->hold_timer = NULL;
        FEC_MP2MP_EXT(fn)->switch_delay_timer = NULL;
    }
	
    /* LMp.1: first check if we have a pending request running */
	lre = (struct lde_req *)fec_find(&ln->sent_req, &fn->fec);
	if (lre)
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
    if (map->type == MAP_TYPE_MP2MP_UP || map->type == MAP_TYPE_MP2MP_DOWN) {
        if (me == NULL)
            me = lde_map_add(ln, fn, 0);
        me->map = *map;
        printf("%s, recive and save mapping\n", __func__);

        if (map->type == MAP_TYPE_MP2MP_DOWN) {
            if (ldeconf->rtr_id.s_addr == ROOT) lde_mp2mp_process_u_mapping(fn);
            struct lde_nbr *lnp = NULL;
            lnp = lde_mp2mp_get_nexthop(fn);
            if (lnp != NULL) {
                lde_send_labelmapping(lnp, fn, 2);
            }
        }
        if (map->type == MAP_TYPE_MP2MP_UP) { 
            if (ldeconf->rtr_id.s_addr == LEAF) return;
            lde_mp2mp_process_u_mapping(fn);
        }
        return;
    }

    if (me) {
		/* LMp.10 */
        printf("%s, me->map.label: %d, map->label: %d\n", __func__, me->map.label, map->label);
		if (me->map.label != map->label && lre == NULL) {
			/* LMp.10a */
			lde_send_labelrelease(ln, fn, me->map.label, me->map.type);

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
		fn = fec_mp2mp_add(&fec);

    struct lde_map *pme = NULL;
    if (map->type == MAP_TYPE_MP2MP_DOWN) {
        LIST_FOREACH(pme, &fn->downstream, entry) {
            if (pme->map.type == map->type && pme->map.label == map->label && pme->nexthop == ln) {
               lde_map_del(ln, pme, 0);
            }
        }
        //TODO:如果没下游了才删上游
        LIST_FOREACH(pme, &fn->upstream, entry) {
            if (pme->map.type == map->type
                || (ldeconf->rtr_id.s_addr == ROOT && pme->map.type == MAP_TYPE_MP2MP_UP && pme->nexthop == ln)) {
               lde_send_labelwithdraw(pme->nexthop, fn, pme->map.label, NULL, pme->map.type);
            }
        }
    }
    else if (map->type == MAP_TYPE_MP2MP_UP) {
        bool is_del_map = true;
        LIST_FOREACH(pme, &fn->upstream, entry) {
            if (pme->map.type == MAP_TYPE_MP2MP_DOWN && pme->nexthop == ln) {
                is_del_map = false;
            }
        }
        if (is_del_map == true) {
            LIST_FOREACH(pme, &fn->downstream, entry) {
                if (pme->map.type == map->type && pme->map.label == map->label) {
                    lde_map_del(ln, pme, 0);
                }
            }
        }

        bool is_send_withdraw = true;
        LIST_FOREACH(pme, &fn->downstream, entry) {
            if (pme->map.type == MAP_TYPE_MP2MP_DOWN) {
                is_send_withdraw = false;
            }
        }
        if (is_send_withdraw == true) { 
            LIST_FOREACH(pme, &fn->upstream, entry) {
                if (pme->map.type == map->type) {
                    lde_send_labelwithdraw(pme->nexthop, fn, pme->map.label, NULL, pme->map.type);
                }
            }
        }
    }

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
	lde_send_labelrelease(ln, fn, map->label, map->type);

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

	/* LWd.2: send label release */
	lde_send_labelrelease(ln, NULL, map->label, map->type);

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;

		/* LWd.1: remove label from forwarding/switching use */
		LIST_FOREACH(fnh, &fn->nexthops, entry) {
			switch (f->type) {
			case FEC_TYPE_IPV4:
			case FEC_TYPE_IPV6:
				if (!lde_address_find(ln, fnh->af,
				    &fnh->nexthop))
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
			fnh->remote_label = NO_LABEL;
		}

		/* LWd.3: check previously received label mapping */
		me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
		if (me && (map->label == NO_LABEL ||
		    map->label == me->map.label))
			/*
			 * LWd.4: remove record of previously received
			 * label mapping
			 */
			lde_map_del(ln, me, 0);
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

struct fec_node *
fec_mp2mp_add(struct fec *fec)
{
    struct fec_node *fn = NULL;
    
    fn = fec_add(fec);
    fn->data = calloc(1, sizeof(struct fec_mp2mp_ext));
    if (fn->data == NULL) {
        fatal(__func__);
        return NULL;    
    }
    FEC_MP2MP_EXT(fn)->mbb_flag |= SEND_MAPPING;    
    FEC_MP2MP_EXT(fn)->hold_time = 5;
    FEC_MP2MP_EXT(fn)->switch_delay_time = 5;
    FEC_MP2MP_EXT(fn)->hold_timer = NULL;
    FEC_MP2MP_EXT(fn)->switch_delay_timer = NULL;

    return fn;
}
