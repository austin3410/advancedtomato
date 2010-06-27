/*

	Copyright 2003, CyberTAN  Inc.  All Rights Reserved

	This is UNPUBLISHED PROPRIETARY SOURCE CODE of CyberTAN Inc.
	the contents of this file may not be disclosed to third parties,
	copied or duplicated in any form without the prior written
	permission of CyberTAN Inc.

	This software should be used as a reference only, and it not
	intended for production use!

	THIS SOFTWARE IS OFFERED "AS IS", AND CYBERTAN GRANTS NO WARRANTIES OF ANY
	KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE.  CYBERTAN
	SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
	FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE

*/
/*

	Copyright 2005, Broadcom Corporation
	All Rights Reserved.

	THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
	KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
	SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
	FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.

 */
/*

	Modified for Tomato Firmware
	Portions, Copyright (C) 2006-2009 Jonathan Zarate

*/

#include "rc.h"

#include <sys/sysinfo.h>
#include <sys/ioctl.h>


#define IFUP (IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST)

static void expires(unsigned int seconds)
{
	struct sysinfo info;
	char s[32];

   	sysinfo(&info);
	sprintf(s, "%u", (unsigned int)info.uptime + seconds);
	f_write_string("/var/lib/misc/dhcpc.expires", s, 0, 0);
}

// copy env to nvram
// returns 1 if new/changed, 0 if not changed/no env
static int env2nv(char *env, char *nv)
{
	char *value;
	if ((value = getenv(env)) != NULL) {
		if (!nvram_match(nv, value)) {
			nvram_set(nv, value);
			return 1;
		}
	}
	return 0;
}

static void env2nv_gateway(const char *nv)
{
	char *v;
	char *b;
	if ((v = getenv("router")) != NULL) {
		if ((b = strdup(v)) != NULL) {
			if ((v = strchr(b, ' ')) != NULL) *v = 0;	// truncate multiple entries
			nvram_set(nv, b);
			free(b);
		}
	}
}

static const char renewing[] = "/var/lib/misc/dhcpc.renewing";

static int deconfig(char *ifname)
{
	TRACE_PT("begin\n");

	ifconfig(ifname, IFUP, "0.0.0.0", NULL);

	nvram_set("wan_ipaddr", "0.0.0.0");
	nvram_set("wan_netmask", "0.0.0.0");
	nvram_set("wan_gateway", "0.0.0.0");
	nvram_set("wan_get_dns", "");
	nvram_set("wan_lease", "0");
	nvram_set("wan_routes", "");
	nvram_set("wan_msroutes", "");
	expires(0);

	//	int i = 10;
	//	while ((route_del(ifname, 0, NULL, NULL, NULL) == 0) && (i-- > 0)) { }

	TRACE_PT("end\n");
	return 0;
}

static int renew(char *ifname)
{
	char *a, *b;
	int changed, routes_changed;

	TRACE_PT("begin\n");

	unlink(renewing);

	changed = env2nv("ip", "wan_ipaddr");
	changed |= env2nv("subnet", "wan_netmask");
	if (changed) {
		ifconfig(ifname, IFUP, nvram_safe_get("wan_ipaddr"), nvram_safe_get("wan_netmask"));
	}

	if (get_wan_proto() == WP_L2TP) {
		env2nv_gateway("wan_gateway_buf");
	}
	else {
		a = strdup(nvram_safe_get("wan_gateway"));
		env2nv_gateway("wan_gateway");
		b = nvram_safe_get("wan_gateway");
		if ((a) && (strcmp(a, b) != 0)) {
			route_del(ifname, 0, "0.0.0.0", a, "0.0.0.0");
			route_add(ifname, 0, "0.0.0.0", b, "0.0.0.0");
			changed = 1;
		}
		free(a);
	}

	changed |= env2nv("domain", "wan_get_domain");
	changed |= env2nv("dns", "wan_get_dns");

	nvram_set("wan_routes_save", nvram_safe_get("wan_routes"));
	nvram_set("wan_msroutes_save", nvram_safe_get("wan_msroutes"));

	/* RFC3442: If the DHCP server returns both a Classless Static Routes option
	 * and a Router option, the DHCP client MUST ignore the Router option.
	 * Similarly, if the DHCP server returns both a Classless Static Routes
	 * option and a Static Routes option, the DHCP client MUST ignore the
	 * Static Routes option.
	 * Read more: http://www.faqs.org/rfcs/rfc3442.html
	 */
	routes_changed = env2nv("staticroutes", "wan_routes_save");
	if (routes_changed)
		nvram_set("wan_msroutes_save", "");
	else {
		routes_changed |= env2nv("routes", "wan_routes_save");
		routes_changed |= env2nv("msroutes", "wan_msroutes_save");
	}
	changed |= routes_changed;

	if ((a = getenv("lease")) != NULL) {
		nvram_set("wan_lease", a);
		expires(atoi(a));
	}

	if (changed) {
		set_host_domain_name();
		start_dnsmasq();	// (re)start
	}

	if (routes_changed) {
		do_wan_routes(ifname, 0, 0);
		nvram_set("wan_routes", nvram_safe_get("wan_routes_save"));
		nvram_set("wan_msroutes", nvram_safe_get("wan_msroutes_save"));
		do_wan_routes(ifname, 0, 1);
	}
	nvram_unset("wan_routes_save");
	nvram_unset("wan_msroutes_save");

	TRACE_PT("wan_ipaddr=%s\n", nvram_safe_get("wan_ipaddr"));
	TRACE_PT("wan_netmask=%s\n", nvram_safe_get("wan_netmask"));
	TRACE_PT("wan_gateway=%s\n", nvram_safe_get("wan_gateway"));
	TRACE_PT("wan_get_domain=%s\n", nvram_safe_get("wan_get_domain"));
	TRACE_PT("wan_get_dns=%s\n", nvram_safe_get("wan_get_dns"));
	TRACE_PT("wan_lease=%s\n", nvram_safe_get("wan_lease"));
	TRACE_PT("wan_routes=%s\n", nvram_safe_get("wan_routes"));
	TRACE_PT("wan_msroutes=%s\n", nvram_safe_get("wan_msroutes"));
	TRACE_PT("end\n");
	return 0;
}

static int bound(char *ifname)
{
	TRACE_PT("begin\n");

	unlink(renewing);

	nvram_set("wan_routes", "");
	nvram_set("wan_msroutes", "");
	env2nv("ip", "wan_ipaddr");
	env2nv("subnet", "wan_netmask");
	env2nv_gateway("wan_gateway");
	env2nv("dns", "wan_get_dns");
	env2nv("domain", "wan_get_domain");
	env2nv("lease", "wan_lease");

	/* RFC3442: If the DHCP server returns both a Classless Static Routes option
	 * and a Router option, the DHCP client MUST ignore the Router option.
	 * Similarly, if the DHCP server returns both a Classless Static Routes
	 * option and a Static Routes option, the DHCP client MUST ignore the
	 * Static Routes option.
	 */
	if (!env2nv("staticroutes", "wan_routes")) {
		env2nv("routes", "wan_routes");
		env2nv("msroutes", "wan_msroutes");
	}

	expires(atoi(safe_getenv("lease")));

	TRACE_PT("wan_ipaddr=%s\n", nvram_safe_get("wan_ipaddr"));
	TRACE_PT("wan_netmask=%s\n", nvram_safe_get("wan_netmask"));
	TRACE_PT("wan_gateway=%s\n", nvram_safe_get("wan_gateway"));
	TRACE_PT("wan_get_domain=%s\n", nvram_safe_get("wan_get_domain"));
	TRACE_PT("wan_get_dns=%s\n", nvram_safe_get("wan_get_dns"));
	TRACE_PT("wan_lease=%s\n", nvram_safe_get("wan_lease"));
	TRACE_PT("wan_routes=%s\n", nvram_safe_get("wan_routes"));
	TRACE_PT("wan_msroutes=%s\n", nvram_safe_get("wan_msroutes"));

	ifconfig(ifname, IFUP, nvram_safe_get("wan_ipaddr"), nvram_safe_get("wan_netmask"));

	int wan_proto = get_wan_proto();
	if (wan_proto == WP_L2TP || wan_proto == WP_PPTP) {
		int i = 0;

		/* Delete all default routes */
		while ((route_del(ifname, 0, NULL, NULL, NULL) == 0) || (i++ < 10));

		/* Set default route to gateway if specified */
		route_add(ifname, 0, "0.0.0.0", nvram_safe_get("wan_gateway"), "0.0.0.0");

		/* Backup the default gateway. It should be used if L2TP connection is broken */
		nvram_set("wan_gateway_buf", nvram_get("wan_gateway"));

		dns_to_resolv();
		start_dnsmasq();
		/* clear dns from the resolv.conf */
		nvram_set("wan_get_dns","");

		start_firewall();
		switch (wan_proto) {
		case WP_PPTP:
			start_pptp(BOOT);
			// we don't need dhcp anymore ?
			// xstart("service", "dhcpc", "stop");
			break;
		case WP_L2TP:
			start_l2tp();
			break;
		}
	}
	else {
		start_wan_done(ifname);
	}

	TRACE_PT("end\n");
	return 0;
}

int dhcpc_event_main(int argc, char **argv)
{
	char *ifname;

	if (!wait_action_idle(10)) return 1;

	if ((argc == 2) && (ifname = getenv("interface")) != NULL) {
		TRACE_PT("event=%s\n", argv[1]);

		if (strcmp(argv[1], "deconfig") == 0) return deconfig(ifname);
		if (strcmp(argv[1], "bound") == 0) return bound(ifname);
		if ((strcmp(argv[1], "renew") == 0) || (strcmp(argv[1], "update") == 0)) return renew(ifname);
	}

	return 1;
}


// -----------------------------------------------------------------------------


int dhcpc_release_main(int argc, char **argv)
{
	TRACE_PT("begin\n");

	if (!using_dhcpc()) return 1;

	deconfig(nvram_safe_get("wan_ifname"));
	killall("udhcpc", SIGUSR2);
	unlink(renewing);
	unlink("/var/lib/misc/wan.connecting");

	TRACE_PT("end\n");
	return 0;
}

int dhcpc_renew_main(int argc, char **argv)
{
	int pid;

	TRACE_PT("begin\n");

	if (!using_dhcpc()) return 1;

	if ((pid = pidof("udhcpc")) > 1) {
		kill(pid, SIGUSR1);
		f_write(renewing, NULL, 0, 0, 0);
	}
	else {
		stop_dhcpc();
		start_dhcpc();
	}

	TRACE_PT("end\n");
	return 0;
}


// -----------------------------------------------------------------------------


void start_dhcpc(void)
{
	char *argv[10];
	int argc;
	char *ifname;
	char *p;

	TRACE_PT("begin\n");

	nvram_set("wan_get_dns", "");
	f_write(renewing, NULL, 0, 0, 0);

	ifname = nvram_safe_get("wan_ifname");
	if (get_wan_proto() != WP_L2TP && get_wan_proto() != WP_PPTP) {
		nvram_set("wan_iface", ifname);
	}

	argc = 0;

	p = nvram_safe_get("wan_hostname");
	if (*p) {
		argv[argc++] = "-H";
		argv[argc++] = p;
	}
	p = nvram_safe_get("dhcpc_vendorclass");
	if (*p) {
		argv[argc++] = "-V";
		argv[argc++] = p;
	}
	p = nvram_safe_get("dhcpc_requestip");
	if ((*p) && (strcmp(p, "0.0.0.0") != 0)) {
		argv[argc++] = "-r";
		argv[argc++] = p;
	}

	/* Remove dhcpc_minpkt nvram setting, always minimize packet size.
	 * Busybox already has the same functionality enabled in trunk,
	 * remove the "-m" parameter after merging with upstream.
	 */
	argv[argc++] = "-m";

	if (nvram_contains_word("log_events", "dhcpc")) argv[argc++] = "-S";

	argv[argc] = NULL;

	xstart(
		"udhcpc",
		"-i", ifname,
		"-s", "dhcpc-event",
		argv[0], argv[1],	// -H wan_hostname
		argv[2], argv[3],	// -V vendorclass
		argv[4], argv[5],	// -r requestip
		argv[6],			// -m
		argv[7]				// -S
	);
	TRACE_PT("end\n");
}

void stop_dhcpc(void)
{
	TRACE_PT("begin\n");

	killall("dhcpc-event", SIGTERM);
	if (killall("udhcpc", SIGUSR2) == 0) {	// release
		sleep(2);
	}
	killall_tk("udhcpc");
	unlink(renewing);

	TRACE_PT("end\n");
}

