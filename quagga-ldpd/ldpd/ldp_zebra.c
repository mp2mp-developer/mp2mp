/*
 * Copyright (C) 2016 by Open Source Routing.
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <zebra.h>

#include "prefix.h"
#include "stream.h"
#include "memory.h"
#include "zclient.h"
#include "command.h"
#include "network.h"
#include "linklist.h"
#include "mpls.h"

#include "ldpd.h"
#include "ldpe.h"
#include "lde.h"
#include "log.h"
#include "ldp_debug.h"

static void	 ifp2kif(struct interface *, struct kif *);
static void	 ifc2kaddr(struct interface *, struct connected *,
		    struct kaddr *);
static int	 ldp_router_id_update(int, struct zclient *, zebra_size_t,
		    vrf_id_t);
static int	 ldp_interface_add(int, struct zclient *, zebra_size_t,
		    vrf_id_t);
static int	 ldp_interface_delete(int, struct zclient *, zebra_size_t,
		    vrf_id_t);
static int	 ldp_interface_status_change(int command, struct zclient *,
		    zebra_size_t, vrf_id_t);
static int	 ldp_interface_address_add(int, struct zclient *, zebra_size_t,
		    vrf_id_t);
static int	 ldp_interface_address_delete(int, struct zclient *,
		    zebra_size_t, vrf_id_t);
static int	 ldp_zebra_read_route(int, struct zclient *, zebra_size_t,
		    vrf_id_t);
static void	 ldp_zebra_connected(struct zclient *);

struct zclient		*zclient = NULL;

static void
ifp2kif(struct interface *ifp, struct kif *kif)
{
	memset(kif, 0, sizeof(*kif));
	strlcpy(kif->ifname, ifp->name, sizeof(kif->ifname));
	kif->ifindex = ifp->ifindex;
	kif->flags = ifp->flags;
}

static void
ifc2kaddr(struct interface *ifp, struct connected *ifc, struct kaddr *ka)
{
	memset(ka, 0, sizeof(*ka));
	ka->ifindex = ifp->ifindex;
	ka->af = ifc->address->family;
	ka->prefixlen = ifc->address->prefixlen;

	switch (ka->af) {
	case AF_INET:
		ka->addr.v4 = ifc->address->u.prefix4;
		if (ifc->destination)
			ka->dstbrd.v4 = ifc->destination->u.prefix4;
		break;
	case AF_INET6:
		ka->addr.v6 = ifc->address->u.prefix6;
		if (ifc->destination)
			ka->dstbrd.v6 = ifc->destination->u.prefix6;
		break;
	default:
		break;
	}
}

static int
zebra_send_mpls_lsp(u_char cmd, struct zclient *zclient, int af,
    union ldpd_addr *nexthop, mpls_label_t in_label, mpls_label_t out_label)
{
	struct stream		*s;

	debug_zebra_out("ILM %s label %s -> nexthop %s label %s",
	    (cmd == ZEBRA_MPLS_LSP_ADD) ? "add" : "delete", log_label(in_label),
	    log_addr(af, nexthop), log_label(out_label));

	/* Reset stream. */
	s = zclient->obuf;
	stream_reset(s);

	zclient_create_header(s, cmd, VRF_DEFAULT);
	stream_putc(s, ZEBRA_LSP_LDP);
	stream_putl(s, af);
	switch (af) {
	case AF_INET:
		stream_put_in_addr(s, &nexthop->v4);
		break;
	case AF_INET6:
		stream_write (s, (u_char *)&nexthop->v6, 16);
		break;
	default:
		fatalx("zebra_send_mpls_lsp: unknown af");
	}
	stream_putl(s, in_label);
	stream_putl(s, out_label);

	/* Put length at the first point of the stream. */
	stream_putw_at(s, 0, stream_get_endp(s));

	return (zclient_send_message(zclient));
}

static int
zebra_send_mpls_ftn(int delete, struct zclient *zclient, int af,
    union ldpd_addr *prefix, uint8_t prefixlen, union ldpd_addr *nexthop,
    mpls_label_t label)
{
	unsigned char		 cmd;
	struct prefix_ipv4	 p4;
	struct prefix_ipv6	 p6;
	struct zapi_ipv4	 api4;
	struct zapi_ipv6	 api6;
	struct in_addr		*nexthop4;
	struct in6_addr		*nexthop6;

	debug_zebra_out("FTN %s %s/%d nexthop %s label %s",
	    (delete) ? "delete" : "add", log_addr(af, prefix), prefixlen,
	    log_addr(af, nexthop), log_label(label));

	switch (af) {
	case AF_INET:
		p4.family = AF_INET;
		p4.prefixlen = prefixlen;
		p4.prefix = prefix->v4;
		nexthop4 = &nexthop->v4;

		api4.vrf_id = VRF_DEFAULT;
		api4.type = ZEBRA_ROUTE_LDP;
		api4.flags = 0;
		api4.message = 0;
		api4.safi = SAFI_UNICAST;
		SET_FLAG(api4.message, ZAPI_MESSAGE_NEXTHOP);
		api4.nexthop_num = 1;
		api4.nexthop = &nexthop4;
		api4.ifindex_num = 0;
		SET_FLAG(api4.message, ZAPI_MESSAGE_LABEL);
		api4.label = label;

		if (delete)
			cmd = ZEBRA_IPV4_ROUTE_DELETE;
		else
			cmd = ZEBRA_IPV4_ROUTE_ADD;
		return (zapi_ipv4_route(cmd, zclient, &p4, &api4));
	case AF_INET6:
		p6.family = AF_INET6;
		p6.prefixlen = prefixlen;
		p6.prefix = prefix->v6;
		nexthop6 = &nexthop->v6;

		api6.vrf_id = VRF_DEFAULT;
		api6.type = ZEBRA_ROUTE_LDP;
		api6.flags = 0;
		api6.message = 0;
		api6.safi = SAFI_UNICAST;
		SET_FLAG(api6.message, ZAPI_MESSAGE_NEXTHOP);
		api6.nexthop_num = 1;
		api6.nexthop = &nexthop6;
		api6.ifindex_num = 0;
		SET_FLAG(api6.message, ZAPI_MESSAGE_LABEL);
		api6.label = label;

		if (delete)
			cmd = ZEBRA_IPV6_ROUTE_DELETE;
		else
			cmd = ZEBRA_IPV6_ROUTE_ADD;
		return (zapi_ipv6_route(cmd, zclient, &p6, &api6));
	default:
		fatalx("zebra_send_mpls_ftn: unknown af");
	}
}

int
kr_change(struct kroute *kr)
{
	if (kr->local_label < MPLS_LABEL_RESERVED_MAX ||
	    kr->remote_label == NO_LABEL)
		return (0);

	/* FEC -> NHLFE */
	if (kr->remote_label != MPLS_LABEL_IMPLNULL)
		zebra_send_mpls_ftn(0, zclient, kr->af, &kr->prefix,
		    kr->prefixlen, &kr->nexthop, kr->remote_label);

	/* ILM -> NHLFE */
	zebra_send_mpls_lsp(ZEBRA_MPLS_LSP_ADD, zclient, kr->af, &kr->nexthop,
	    kr->local_label, kr->remote_label);

	return (0);
}

int
kr_delete(struct kroute *kr)
{
	if (kr->local_label < MPLS_LABEL_RESERVED_MAX ||
	    kr->remote_label == NO_LABEL)
		return (0);

	/* FEC -> NHLFE */
	if (kr->remote_label != MPLS_LABEL_IMPLNULL)
		zebra_send_mpls_ftn(1, zclient, kr->af, &kr->prefix,
		    kr->prefixlen, &kr->nexthop, kr->remote_label);

	/* ILM -> NHLFE */
	zebra_send_mpls_lsp(ZEBRA_MPLS_LSP_DELETE, zclient, kr->af,
	    &kr->nexthop, kr->local_label, kr->remote_label);

	return (0);
}

int
kmpw_set(struct kpw *kpw)
{
	/* TODO */
	return (0);
}

int
kmpw_unset(struct kpw *kpw)
{
	/* TODO */
	return (0);
}

void
kif_redistribute(const char *ifname)
{
	struct listnode		*node, *cnode;
	struct interface	*ifp;
	struct connected	*ifc;
	struct kif		 kif;
	struct kaddr		 ka;

	for (ALL_LIST_ELEMENTS_RO(vrf_iflist(VRF_DEFAULT), node, ifp)) {
		if (ifname && strcmp(ifname, ifp->name) != 0)
			continue;

		ifp2kif(ifp, &kif);
		main_imsg_compose_ldpe(IMSG_IFSTATUS, 0, &kif, sizeof(kif));

		for (ALL_LIST_ELEMENTS_RO(ifp->connected, cnode, ifc)) {
			ifc2kaddr(ifp, ifc, &ka);
			main_imsg_compose_ldpe(IMSG_NEWADDR, 0, &ka,
			    sizeof(ka));
		}
	}
}

static int
ldp_router_id_update(int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
	struct prefix	 router_id;

	zebra_router_id_update_read(zclient->ibuf, &router_id);

	if (bad_addr_v4(router_id.u.prefix4))
		return (0);

	debug_zebra_in("router-id update %s", inet_ntoa(router_id.u.prefix4));

	global.rtr_id.s_addr = router_id.u.prefix4.s_addr;
	main_imsg_compose_ldpe(IMSG_RTRID_UPDATE, 0, &global.rtr_id,
	    sizeof(global.rtr_id));

	return (0);
}

static int
ldp_interface_add(int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
	struct interface	*ifp;
	struct kif		 kif;

	ifp = zebra_interface_add_read(zclient->ibuf, vrf_id);
	debug_zebra_in("interface add %s index %d mtu %d", ifp->name,
	    ifp->ifindex, ifp->mtu);

	ifp2kif(ifp, &kif);
	main_imsg_compose_ldpe(IMSG_IFSTATUS, 0, &kif, sizeof(kif));

	return (0);
}

static int
ldp_interface_delete(int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
	struct interface	*ifp;

	/* zebra_interface_state_read() updates interface structure in iflist */
	ifp = zebra_interface_state_read(zclient->ibuf, vrf_id);
	if (ifp == NULL)
		return (0);

	debug_zebra_in("interface delete %s index %d mtu %d", ifp->name,
	    ifp->ifindex, ifp->mtu);

	/* To support pseudo interface do not free interface structure.  */
	/* if_delete(ifp); */
	ifp->ifindex = IFINDEX_INTERNAL;

	return (0);
}

static int
ldp_interface_status_change(int command, struct zclient *zclient,
    zebra_size_t length, vrf_id_t vrf_id)
{
	struct interface	*ifp;
	struct listnode		*node;
	struct connected	*ifc;
	struct kif		 kif;
	struct kaddr		 ka;
	int			 link_new;

	/*
	 * zebra_interface_state_read() updates interface structure in
	 * iflist.
	 */
	ifp = zebra_interface_state_read(zclient->ibuf, vrf_id);
	if (ifp == NULL)
		return (0);

	debug_zebra_in("interface %s state update", ifp->name);

	ifp2kif(ifp, &kif);
	main_imsg_compose_ldpe(IMSG_IFSTATUS, 0, &kif, sizeof(kif));

	link_new = (ifp->flags & IFF_UP) && (ifp->flags & IFF_RUNNING);
	if (link_new) {
		for (ALL_LIST_ELEMENTS_RO(ifp->connected, node, ifc)) {
			ifc2kaddr(ifp, ifc, &ka);
			main_imsg_compose_ldpe(IMSG_NEWADDR, 0, &ka,
			    sizeof(ka));
		}
	} else {
		for (ALL_LIST_ELEMENTS_RO(ifp->connected, node, ifc)) {
			ifc2kaddr(ifp, ifc, &ka);
			main_imsg_compose_ldpe(IMSG_DELADDR, 0, &ka,
			    sizeof(ka));
		}
	}

	return (0);
}

static int
ldp_interface_address_add(int command, struct zclient *zclient,
    zebra_size_t length, vrf_id_t vrf_id)
{
	struct connected	*ifc;
	struct interface	*ifp;
	struct kaddr		 ka;

	ifc = zebra_interface_address_read(command, zclient->ibuf, vrf_id);
	if (ifc == NULL)
		return (0);

	ifp = ifc->ifp;
	ifc2kaddr(ifp, ifc, &ka);

	/* Filter invalid addresses.  */
	if (bad_addr(ka.af, &ka.addr))
		return (0);

	debug_zebra_in("address add %s/%u", log_addr(ka.af, &ka.addr),
	    ka.prefixlen);

	/* notify ldpe about new address */
	main_imsg_compose_ldpe(IMSG_NEWADDR, 0, &ka, sizeof(ka));

	return (0);
}

static int
ldp_interface_address_delete(int command, struct zclient *zclient,
    zebra_size_t length, vrf_id_t vrf_id)
{
	struct connected	*ifc;
	struct interface	*ifp;
	struct kaddr		 ka;

	ifc = zebra_interface_address_read(command, zclient->ibuf, vrf_id);
	if (ifc == NULL)
		return (0);

	ifp = ifc->ifp;
	ifc2kaddr(ifp, ifc, &ka);
	connected_free(ifc);

	/* Filter invalid addresses.  */
	if (bad_addr(ka.af, &ka.addr))
		return (0);

	debug_zebra_in("address delete %s/%u", log_addr(ka.af, &ka.addr),
	    ka.prefixlen);

	/* notify ldpe about removed address */
	main_imsg_compose_ldpe(IMSG_DELADDR, 0, &ka, sizeof(ka));

	return (0);
}

static int
ldp_zebra_read_route(int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
	struct stream		*s;
	u_char			 type;
	u_char			 flags, message_flags;
	struct kroute		 kr;

	memset(&kr, 0, sizeof(kr));
	s = zclient->ibuf;

	type = stream_getc(s);
	flags = stream_getc(s);
	message_flags = stream_getc(s);

	switch (command) {
	case ZEBRA_IPV4_ROUTE_ADD:
	case ZEBRA_IPV4_ROUTE_DELETE:
		kr.af = AF_INET;
		break;
	case ZEBRA_IPV6_ROUTE_ADD:
	case ZEBRA_IPV6_ROUTE_DELETE:
		kr.af = AF_INET6;
		break;
	default:
		fatalx("ldp_zebra_read_route: unknown command");
	}
	kr.prefixlen = stream_getc(s);
	stream_get(&kr.prefix, s, PSIZE(kr.prefixlen));

	if (bad_addr(kr.af, &kr.prefix) ||
	    (kr.af == AF_INET6 && IN6_IS_SCOPE_EMBED(&kr.prefix.v6)))
		return (0);

	/*
	 * Consider networks with nexthop loopback as not redistributable
	 * unless it is a reject or blackhole route.
	 */
	if (CHECK_FLAG(message_flags, ZAPI_MESSAGE_NEXTHOP)) {
		stream_getc(s);	/* nexthop_num, unused. */
		switch (kr.af) {
		case AF_INET:
			kr.nexthop.v4.s_addr = stream_get_ipv4(s);

			if (kr.nexthop.v4.s_addr == htonl(INADDR_LOOPBACK) &&
			    (CHECK_FLAG(flags, ZEBRA_FLAG_BLACKHOLE)
			    || CHECK_FLAG(flags, ZEBRA_FLAG_REJECT)))
				return (0);
			break;
		case AF_INET6:
			stream_get(&kr.nexthop.v6, s, sizeof(kr.nexthop.v6));

			if (IN6_IS_ADDR_LOOPBACK(&kr.nexthop.v6) &&
			    (CHECK_FLAG(flags, ZEBRA_FLAG_BLACKHOLE)
			    || CHECK_FLAG(flags, ZEBRA_FLAG_REJECT)))
				return (0);
			break;
		default:
			break;
		}
	}
	if (CHECK_FLAG(message_flags, ZAPI_MESSAGE_IFINDEX)) {
		stream_getc(s);	/* ifindex_num, unused. */
		kr.ifindex = stream_getl(s);
	}

	if (CHECK_FLAG(message_flags, ZAPI_MESSAGE_DISTANCE))
		stream_getc(s); /* distance, not used */
	if (CHECK_FLAG(message_flags, ZAPI_MESSAGE_METRIC))
		stream_getl(s);	/* metric, not used */

	if (type == ZEBRA_ROUTE_CONNECT)
		kr.flags |= F_CONNECTED;

	switch (command) {
	case ZEBRA_IPV4_ROUTE_ADD:
	case ZEBRA_IPV6_ROUTE_ADD:
		debug_zebra_in("route add %s/%d nexthop %s (%s)",
		    log_addr(kr.af, &kr.prefix), kr.prefixlen,
		    log_addr(kr.af, &kr.nexthop), zebra_route_string(type));
		main_imsg_compose_lde(IMSG_NETWORK_ADD, 0, &kr, sizeof(kr));
		break;
	case ZEBRA_IPV4_ROUTE_DELETE:
	case ZEBRA_IPV6_ROUTE_DELETE:
		debug_zebra_in("route delete %s/%d nexthop %s (%s)",
		    log_addr(kr.af, &kr.prefix), kr.prefixlen,
		    log_addr(kr.af, &kr.nexthop), zebra_route_string(type));
		main_imsg_compose_lde(IMSG_NETWORK_DEL, 0, &kr, sizeof(kr));
		break;
	default:
		fatalx("ldp_zebra_read_route: unknown command");
	}

	return (0);
}

static void
ldp_zebra_connected(struct zclient *zclient)
{
	int	i;

	zclient_send_requests(zclient, VRF_DEFAULT);

	for (i = 0; i < ZEBRA_ROUTE_MAX; i++) {
		switch (i) {
		case ZEBRA_ROUTE_KERNEL:
		case ZEBRA_ROUTE_CONNECT:
		case ZEBRA_ROUTE_STATIC:
		case ZEBRA_ROUTE_RIP:
		case ZEBRA_ROUTE_RIPNG:
		case ZEBRA_ROUTE_OSPF:
		case ZEBRA_ROUTE_OSPF6:
		case ZEBRA_ROUTE_ISIS:
			zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, zclient,
			    i, VRF_DEFAULT);
			break;
		case ZEBRA_ROUTE_BGP:
		default:
			/* LDP should follow the IGP and ignore BGP routes */
			break;
		}
	}
}

void
ldp_zebra_init(struct thread_master *master)
{
	/* Set default values. */
	zclient = zclient_new(master);
	zclient_init(zclient, ZEBRA_ROUTE_LDP);

	/* set callbacks */
	zclient->zebra_connected = ldp_zebra_connected;
	zclient->router_id_update = ldp_router_id_update;
	zclient->interface_add = ldp_interface_add;
	zclient->interface_delete = ldp_interface_delete;
	zclient->interface_up = ldp_interface_status_change;
	zclient->interface_down = ldp_interface_status_change;
	zclient->interface_address_add = ldp_interface_address_add;
	zclient->interface_address_delete = ldp_interface_address_delete;
	zclient->ipv4_route_add = ldp_zebra_read_route;
	zclient->ipv4_route_delete = ldp_zebra_read_route;
	zclient->ipv6_route_add = ldp_zebra_read_route;
	zclient->ipv6_route_delete = ldp_zebra_read_route;
}
